//! V-edit の自前の書き出しエンジン(engine.py の Rust 版)。ffmpeg のライブラリ(libav*)を1つのプロセスの中で使う。
//! 背景のフィルタグラフ・差し込みのデコード(H.264/HEVC は NVDEC)・重ね合わせ・テロップ・エンコード(NVENC)を
//! 別々のスレッドで同時に進め、生の絵のパイプや ffmpeg の立ち上げをなくす。
//!
//! 使い方:
//!   vengine --serve          書き出し1回ぶんの常駐(render.py が使う)
//!       標準入力に1行1件 {"k": 区間番号, "job": 指示.json, "out": 出力.mp4}。区間ごとに並列で作る。
//!       標準出力に "k frame=N" / "k ok" / "k err 理由"
//!   vengine 指示.json 出力.mp4   1区間だけ(試験用)
mod av;
mod comp;
mod enc;
mod graph;
mod input;

use std::io::{BufRead, Write};
use std::sync::{Arc, Mutex};

fn load(path: &str) -> av::R<comp::Job> {
    let s = std::fs::read_to_string(path).map_err(|e| format!("{path} を読めません: {e}"))?;
    serde_json::from_str(&s).map_err(|e| format!("{path} の形が違います: {e}"))
}

fn one(job: &str, out: &str, progress: &dyn Fn(i64)) -> av::R<()> {
    let j = load(job)?;
    // 落ちてもプロセスごと止めない(その区間だけ失敗にして ffmpeg でやり直させる)
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| comp::run(&j, out, progress))) {
        Ok(r) => r,
        Err(e) => Err(format!("panic: {}", e.downcast_ref::<String>().cloned().or_else(|| e.downcast_ref::<&str>().map(|s| s.to_string())).unwrap_or_default())),
    }
}

fn serve() {
    let out = Arc::new(Mutex::new(std::io::stdout()));
    let say = {
        let out = out.clone();
        move |s: String| {
            let mut o = out.lock().unwrap_or_else(|e| e.into_inner());
            let _ = writeln!(o, "{s}");
            let _ = o.flush();
        }
    };
    say("ready".into());
    let mut ths = vec![];
    for line in std::io::stdin().lock().lines() {
        let Ok(line) = line else { break };
        let line = line.trim().to_string();
        if line.is_empty() {
            continue;
        }
        let say = say.clone();
        ths.push(std::thread::spawn(move || {
            let req: serde_json::Value = match serde_json::from_str(&line) {
                Ok(v) => v,
                Err(e) => return say(format!("-1 err 指示の形が違います: {e}")),
            };
            let k = req["k"].as_i64().unwrap_or(-1);
            let (job, o) = (req["job"].as_str().unwrap_or(""), req["out"].as_str().unwrap_or(""));
            match one(job, o, &|n| say(format!("{k} frame={n}"))) {
                Ok(()) => say(format!("{k} ok")),
                Err(e) => {
                    let _ = std::fs::remove_file(o);
                    let e: String = e.replace(['\n', '\r'], " ").chars().take(1500).collect();
                    say(format!("{k} err {e}"))
                }
            }
        }));
    }
    for th in ths {
        let _ = th.join();
    }
}

fn main() {
    unsafe { ffmpeg_sys_next::av_log_set_level(ffmpeg_sys_next::AV_LOG_ERROR as i32) };
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.first().map(String::as_str) == Some("--serve") {
        return serve();
    }
    if args.len() != 2 {
        eprintln!("使い方: vengine --serve | vengine 指示.json 出力.mp4");
        std::process::exit(2);
    }
    let t = std::time::Instant::now();
    match one(&args[0], &args[1], &|n| println!("frame={n}")) {
        Ok(()) => println!("ok {:.2}s", t.elapsed().as_secs_f64()),
        Err(e) => {
            eprintln!("自前エンジンで失敗: {e}");
            std::process::exit(1);
        }
    }
}
