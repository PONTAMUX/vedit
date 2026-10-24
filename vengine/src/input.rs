//! 入力(ffmpeg の `-ss X -i file` と同じ読み方)。デコードは裏のスレッドで先読みする。
//! H.264 / HEVC の 8bit 4:2:0 は GPU のデコーダー(NVDEC)で読み、CPU にはコピーするだけにする(CPU のデコードは 1コマ 10ms ほどかかる)。
use crate::av::*;
use ffmpeg_sys_next as ff;
use std::ptr;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::mpsc::{Receiver, SyncSender, sync_channel};
use std::sync::OnceLock;
use std::thread::JoinHandle;

/// 先読みしておくコマ数
pub const POOL: usize = 4;
/// GPU のデコーダーを同時にいくつまで使うか(1つ 100MB ほど VRAM を使う。超えたら CPU で読む)
const NVDEC_MAX: usize = 12;
static NVDEC_USED: AtomicUsize = AtomicUsize::new(0);

#[derive(Clone, Debug, Default)]
pub struct InSpec {
    pub url: String,
    pub ss: Option<f64>,
    pub t: Option<f64>,
    pub fmt: Option<String>,
    pub threads: Option<i32>,
    pub opts: Vec<(String, String)>,
}

/// ffmpeg の引数から入力を取り出す → (入力, それ以外の (オプション, 値))。知らない形はエラー(ffmpeg でやり直させる)
pub fn parse_args(args: &[String]) -> R<(Vec<InSpec>, Vec<(String, String)>)> {
    let mut ins = vec![];
    let mut cur = InSpec::default();
    let mut rest = vec![];
    let mut i = 0;
    let mut pending = false; // cur に入力用のオプションが溜まっている
    while i < args.len() {
        let k = args[i].as_str();
        if k == "-an" {
            i += 1; // 値を取らない
            continue;
        }
        let v = args.get(i + 1).cloned().ok_or_else(|| format!("引数 {k} の値がありません"))?;
        i += 2;
        match k {
            "-i" => {
                cur.url = v;
                ins.push(std::mem::take(&mut cur));
                pending = false;
            }
            "-filter_complex_threads" | "-filter_complex_script" | "-filter_complex" | "-map" | "-frames:v" | "-vf" => {
                rest.push((k.to_string(), v))
            }
            "-ss" => {
                cur.ss = Some(v.parse().map_err(|_| format!("-ss {v}"))?);
                pending = true
            }
            "-t" => {
                cur.t = Some(v.parse().map_err(|_| format!("-t {v}"))?);
                pending = true
            }
            "-f" => {
                cur.fmt = Some(v);
                pending = true
            }
            "-threads" => {
                cur.threads = v.parse().ok();
                pending = true
            }
            // GPU で読むかどうかはこちらで決める
            "-hwaccel" | "-hwaccel_device" | "-hwaccel_output_format" => pending = true,
            _ if k.starts_with('-') => {
                cur.opts.push((k[1..].to_string(), v));
                pending = true
            }
            _ => return Err(format!("知らない引数 {k}")),
        }
    }
    if pending {
        return Err("入力の後ろに知らないオプションがあります".into());
    }
    Ok((ins, rest))
}

/// プロセスで1つの CUDA の器(デコード・エンコードで共有する。区間ごとに作ると VRAM を食う)
pub fn hw_device() -> Option<*mut ff::AVBufferRef> {
    static HW: OnceLock<Option<SendPtr<ff::AVBufferRef>>> = OnceLock::new();
    HW.get_or_init(|| {
        let mut dev = ptr::null_mut();
        let r = unsafe { ff::av_hwdevice_ctx_create(&mut dev, ff::AVHWDeviceType::AV_HWDEVICE_TYPE_CUDA, ptr::null(), ptr::null_mut(), 0) };
        if r < 0 { None } else { Some(SendPtr(dev)) }
    })
    .as_ref()
    .map(|p| p.0)
}
unsafe impl Sync for SendPtr<ff::AVBufferRef> {}

unsafe extern "C" fn pick_cuda(_c: *mut ff::AVCodecContext, fmts: *const ff::AVPixelFormat) -> ff::AVPixelFormat {
    let mut p = fmts;
    unsafe {
        while *p != ff::AVPixelFormat::AV_PIX_FMT_NONE {
            if *p == ff::AVPixelFormat::AV_PIX_FMT_CUDA {
                return *p;
            }
            p = p.add(1);
        }
    }
    ff::AVPixelFormat::AV_PIX_FMT_NONE // GPU で読めない形(途中で変わった等)はエラーにして ffmpeg でやり直させる
}

/// フィルタの入口(buffer)に渡す、絵の形
#[derive(Clone, Copy)]
pub struct Info {
    pub w: i32,
    pub h: i32,
    pub fmt: i32,
    pub tb: ff::AVRational,
    pub fr: ff::AVRational,
    pub sar: ff::AVRational,
    pub range: i32,
    pub space: i32,
}

struct Opened {
    ic: *mut ff::AVFormatContext,
    si: i32,
    spec: InSpec,
    hw: bool,
    off: i64, // 時刻のずらし(AV_TIME_BASE)
}
unsafe impl Send for Opened {}

pub struct Decoder {
    pub info: Info,
    opened: Option<Opened>,
    rx: Option<Receiver<R<Option<Frame>>>>,
    th: Option<JoinHandle<()>>,
}

impl Decoder {
    /// 開いて形を調べる(デコードはまだしない)
    pub fn open(spec: InSpec, allow_hw: bool) -> R<Decoder> {
        unsafe {
            let mut ic: *mut ff::AVFormatContext = ptr::null_mut();
            let mut d = Dict::new();
            for (k, v) in &spec.opts {
                d.set(k, v);
            }
            let ifmt = match &spec.fmt {
                Some(f) => {
                    let c = cs(f);
                    let p = ff::av_find_input_format(c.as_ptr());
                    if p.is_null() {
                        return Err(format!("知らない形式 {f}"));
                    }
                    p
                }
                None => ptr::null(),
            };
            let url = cs(&spec.url);
            check(ff::avformat_open_input(&mut ic, url.as_ptr(), ifmt, &mut d.0), &format!("開けません {}", spec.url))?;
            let left = d.keys();
            let guard = IcGuard(ic);
            if !left.is_empty() {
                return Err(format!("知らない入力オプション {left:?}"));
            }
            check(ff::avformat_find_stream_info(ic, ptr::null_mut()), &format!("形式を調べられません {}", spec.url))?;
            let mut si = -1;
            for i in 0..(*ic).nb_streams as usize {
                let st = *(*ic).streams.add(i);
                if (*(*st).codecpar).codec_type == ff::AVMediaType::AVMEDIA_TYPE_VIDEO && si < 0 {
                    si = i as i32;
                } else {
                    (*st).discard = ff::AVDiscard::AVDISCARD_ALL;
                }
            }
            if si < 0 {
                return Err(format!("映像がありません {}", spec.url));
            }
            // ffmpeg と同じシーク: 頭の時刻 + ss へ(B フレームのある素材は 3/23 秒手前から)
            let start = if (*ic).start_time != ff::AV_NOPTS_VALUE { (*ic).start_time } else { 0 };
            let ts = (spec.ss.unwrap_or(0.0) * ff::AV_TIME_BASE as f64).round() as i64 + start;
            if spec.ss.is_some() {
                let mut seek = ts;
                if (*(*ic).iformat).flags & ff::AVFMT_SEEK_TO_PTS as i32 == 0 {
                    let delay = (0..(*ic).nb_streams as usize).any(|i| (*(*(*(*ic).streams.add(i))).codecpar).video_delay != 0);
                    if delay {
                        seek -= 3 * ff::AV_TIME_BASE as i64 / 23;
                    }
                }
                ff::avformat_seek_file(ic, -1, i64::MIN, seek, seek, 0);
            }
            let st = *(*ic).streams.add(si as usize);
            let par = &*(*st).codecpar;
            let cid = par.codec_id;
            let hw = allow_hw
                && std::env::var_os("VENGINE_NO_NVDEC").is_none()
                && matches!(cid, ff::AVCodecID::AV_CODEC_ID_H264 | ff::AVCodecID::AV_CODEC_ID_HEVC)
                && par.format == ff::AVPixelFormat::AV_PIX_FMT_YUV420P as i32
                && par.width <= 4096
                && par.height <= 4096
                && hw_device().is_some()
                && NVDEC_USED.try_update(Ordering::SeqCst, Ordering::SeqCst, |n| (n < NVDEC_MAX).then_some(n + 1)).is_ok();
            let info = Info {
                w: par.width,
                h: par.height,
                fmt: if hw { ff::AVPixelFormat::AV_PIX_FMT_NV12 as i32 } else { par.format },
                tb: (*st).time_base,
                fr: ff::av_guess_frame_rate(ic, st, ptr::null_mut()),
                sar: par.sample_aspect_ratio,
                range: par.color_range as i32,
                space: par.color_space as i32,
            };
            std::mem::forget(guard);
            Ok(Decoder { info, opened: Some(Opened { ic, si, spec, hw, off: -ts }), rx: None, th: None })
        }
    }

    /// デコードを始める(裏のスレッドで先読み)
    pub fn start(&mut self) {
        if let Some(o) = self.opened.take() {
            let (tx, rx) = sync_channel(POOL);
            self.rx = Some(rx);
            self.th = Some(std::thread::spawn(move || {
                let hw = o.hw;
                let ic = IcGuard(o.ic);
                let r = decode_loop(&o, &tx);
                drop(ic);
                if hw {
                    NVDEC_USED.fetch_sub(1, Ordering::SeqCst);
                }
                match r {
                    Ok(()) => {
                        let _ = tx.send(Ok(None));
                    }
                    Err(e) => {
                        let _ = tx.send(Err(e));
                    }
                }
            }));
        }
    }

    /// 次のコマ(None = 終わり)
    pub fn recv(&mut self) -> R<Option<Frame>> {
        self.start();
        match &self.rx {
            Some(rx) => rx.recv().unwrap_or(Ok(None)),
            None => Ok(None),
        }
    }
}

impl Drop for Decoder {
    fn drop(&mut self) {
        if let Some(o) = self.opened.take() {
            drop(IcGuard(o.ic));
            if o.hw {
                NVDEC_USED.fetch_sub(1, Ordering::SeqCst);
            }
        }
        self.rx = None; // 先読みのスレッドは送れなくなって終わる
        if let Some(th) = self.th.take() {
            let _ = th.join();
        }
    }
}

struct IcGuard(*mut ff::AVFormatContext);
impl Drop for IcGuard {
    fn drop(&mut self) {
        unsafe { ff::avformat_close_input(&mut self.0) }
    }
}

struct CtxGuard(*mut ff::AVCodecContext);
impl Drop for CtxGuard {
    fn drop(&mut self) {
        unsafe { ff::avcodec_free_context(&mut self.0) }
    }
}

struct PktGuard(*mut ff::AVPacket);
impl Drop for PktGuard {
    fn drop(&mut self) {
        unsafe { ff::av_packet_free(&mut self.0) }
    }
}

fn decode_loop(o: &Opened, tx: &SyncSender<R<Option<Frame>>>) -> R<()> {
    unsafe {
        let st = *(*o.ic).streams.add(o.si as usize);
        let tb = (*st).time_base;
        let par = (*st).codecpar;
        let codec = ff::avcodec_find_decoder((*par).codec_id);
        if codec.is_null() {
            return Err(format!("デコーダーがありません {}", o.spec.url));
        }
        let dc = CtxGuard(ff::avcodec_alloc_context3(codec));
        let c = dc.0;
        check(ff::avcodec_parameters_to_context(c, par), "デコーダーの設定")?;
        (*c).pkt_timebase = tb;
        (*c).thread_count = o.spec.threads.unwrap_or(0);
        if o.hw {
            (*c).hw_device_ctx = ff::av_buffer_ref(hw_device().unwrap());
            (*c).get_format = Some(pick_cuda);
        }
        check(ff::avcodec_open2(c, codec, ptr::null_mut()), &format!("デコーダーを開けません {}", o.spec.url))?;
        let off = ff::av_rescale_q(o.off, ff::AV_TIME_BASE_Q, tb);
        let tlim = o.spec.t.map(|t| ff::av_rescale_q((t * ff::AV_TIME_BASE as f64).round() as i64, ff::AV_TIME_BASE_Q, tb));
        let mut first: Option<i64> = None;
        let pkt = PktGuard(ff::av_packet_alloc());
        let mut f = Frame::new();
        let mut draining = false;
        loop {
            if !draining {
                let r = ff::av_read_frame(o.ic, pkt.0);
                if r < 0 {
                    if r != EOF {
                        return Err(format!("読めません {}: {}", o.spec.url, errstr(r)));
                    }
                    draining = true;
                    check(ff::avcodec_send_packet(c, ptr::null()), "デコーダー")?;
                } else {
                    if (*pkt.0).stream_index != o.si {
                        ff::av_packet_unref(pkt.0);
                        continue;
                    }
                    if (*pkt.0).pts != ff::AV_NOPTS_VALUE {
                        (*pkt.0).pts += off;
                    }
                    if (*pkt.0).dts != ff::AV_NOPTS_VALUE {
                        (*pkt.0).dts += off;
                    }
                    let r = ff::avcodec_send_packet(c, pkt.0);
                    ff::av_packet_unref(pkt.0);
                    if r < 0 && r != EAGAIN {
                        return Err(format!("デコードできません {}: {}", o.spec.url, errstr(r)));
                    }
                }
            }
            loop {
                let r = ff::avcodec_receive_frame(c, f.0);
                if r == EAGAIN {
                    break;
                }
                if r == EOF {
                    return Ok(());
                }
                check(r, &format!("デコードできません {}", o.spec.url))?;
                let pts = f.f().best_effort_timestamp;
                let mut out = if f.f().format == ff::AVPixelFormat::AV_PIX_FMT_CUDA as i32 {
                    let sw = Frame::new();
                    (*sw.0).format = ff::AVPixelFormat::AV_PIX_FMT_NV12 as i32;
                    check(ff::av_hwframe_transfer_data(sw.0, f.0, 0), "GPU から絵を受け取れません")?;
                    check(ff::av_frame_copy_props(sw.0, f.0), "絵の情報")?;
                    ff::av_frame_unref(f.0);
                    sw
                } else {
                    std::mem::replace(&mut f, Frame::new())
                };
                out.fm().pts = pts;
                // -ss のときは、シーク位置より前のコマを捨てる(ffmpeg の accurate_seek)
                if o.spec.ss.is_some() && pts != ff::AV_NOPTS_VALUE && pts < 0 {
                    continue;
                }
                if let Some(t) = tlim {
                    let p0 = *first.get_or_insert(pts);
                    if pts != ff::AV_NOPTS_VALUE && pts - p0 >= t {
                        return Ok(());
                    }
                }
                if tx.send(Ok(Some(out))).is_err() {
                    return Ok(()); // 受け取る側が閉じた
                }
            }
            if draining {
                // receive_frame が EOF を返すまで回る(上で return する)
            }
        }
    }
}
