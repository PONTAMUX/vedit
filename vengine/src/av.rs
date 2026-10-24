//! ffmpeg(libav*)を Rust から使うための小さな道具。
use ffmpeg_sys_next as ff;
use std::ffi::{CStr, CString};
use std::ptr;

pub type R<T> = Result<T, String>;

pub const EAGAIN: i32 = -(ff::EAGAIN as i32);
pub const EOF: i32 = ff::AVERROR_EOF;

/// ffmpeg のエラー番号を文字にする
pub fn errstr(code: i32) -> String {
    let mut buf = [0i8; 256];
    unsafe {
        ff::av_strerror(code, buf.as_mut_ptr(), buf.len());
        CStr::from_ptr(buf.as_ptr()).to_string_lossy().into_owned()
    }
}

/// 負ならエラーにする(what = 何をしていたか)
pub fn check(ret: i32, what: &str) -> R<i32> {
    if ret < 0 { Err(format!("{what}: {}", errstr(ret))) } else { Ok(ret) }
}

pub fn cs(s: &str) -> CString {
    CString::new(s).unwrap_or_else(|_| CString::new(s.replace('\0', "")).unwrap())
}

/// AVFrame(参照カウントつき)。スレッドをまたいで渡す
pub struct Frame(pub *mut ff::AVFrame);
unsafe impl Send for Frame {}

impl Frame {
    pub fn new() -> Frame {
        Frame(unsafe { ff::av_frame_alloc() })
    }
    /// 同じ絵を指す別の Frame(中身はコピーしない)
    pub fn share(&self) -> Frame {
        Frame(unsafe { ff::av_frame_clone(self.0) })
    }
    pub fn f(&self) -> &ff::AVFrame {
        unsafe { &*self.0 }
    }
    pub fn fm(&mut self) -> &mut ff::AVFrame {
        unsafe { &mut *self.0 }
    }
    /// 平面 i の (先頭, 1行のバイト数)
    pub fn plane(&self, i: usize) -> (*mut u8, isize) {
        let f = self.f();
        (f.data[i], f.linesize[i] as isize)
    }
}

impl Drop for Frame {
    fn drop(&mut self) {
        unsafe { ff::av_frame_free(&mut self.0) }
    }
}

/// AVDictionary
pub struct Dict(pub *mut ff::AVDictionary);
impl Dict {
    pub fn new() -> Dict {
        Dict(ptr::null_mut())
    }
    pub fn set(&mut self, k: &str, v: &str) {
        let (k, v) = (cs(k), cs(v));
        unsafe { ff::av_dict_set(&mut self.0, k.as_ptr(), v.as_ptr(), 0) };
    }
    /// 使われずに残ったキー(ffmpeg が知らないオプション)
    pub fn keys(&self) -> Vec<String> {
        let mut out = vec![];
        let mut e: *mut ff::AVDictionaryEntry = ptr::null_mut();
        let empty = cs("");
        loop {
            e = unsafe { ff::av_dict_get(self.0, empty.as_ptr(), e, ff::AV_DICT_IGNORE_SUFFIX as i32) };
            if e.is_null() {
                break;
            }
            out.push(unsafe { CStr::from_ptr((*e).key) }.to_string_lossy().into_owned());
        }
        out
    }
}
impl Drop for Dict {
    fn drop(&mut self) {
        unsafe { ff::av_dict_free(&mut self.0) }
    }
}

pub fn q(num: i32, den: i32) -> ff::AVRational {
    ff::AVRational { num, den }
}

/// "30" / "30000/1001" / "29.97" → 分数
pub fn parse_rate(s: &str) -> R<ff::AVRational> {
    let mut r = q(0, 1);
    let c = cs(s);
    check(unsafe { ff::av_parse_video_rate(&mut r, c.as_ptr()) }, &format!("フレームレート {s}"))?;
    Ok(r)
}

/// 生のポインタをスレッドへ渡すための入れ物(使う側が安全を保証する)
pub struct SendPtr<T>(pub *mut T);
unsafe impl<T> Send for SendPtr<T> {}
