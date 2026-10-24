#!/bin/sh
# vengine のビルド(Git Bash で実行)。ライブラリは C:/Libs、作業用の target は C:/Temp
SC="$HOME/.rustup/toolchains/stable-x86_64-pc-windows-gnu/lib/rustlib/x86_64-pc-windows-gnu/bin/self-contained"
export PATH="/c/Libs/mtools:$HOME/.cargo/bin:$PATH:$SC"
export FFMPEG_DIR="C:/Libs/ffmpeg9/ffmpeg-n9.0-latest-win64-gpl-shared-9.0"
export LIBCLANG_PATH="C:/Libs/libclang/libclang-18.1.1.data/platlib/clang/native"
export CARGO_TARGET_DIR="C:/Temp/vengine-target"
export CC=C:/Libs/mingw64/bin/gcc.exe  # ffmpeg-sys-next の確認用の小さな C のビルドにだけ使う
export BINDGEN_EXTRA_CLANG_ARGS="--target=x86_64-w64-mingw32 -IC:/Libs/mingw64/x86_64-w64-mingw32/include -IC:/Libs/clangstub"
cd "$(dirname "$0")" && cargo build --release "$@" || exit 1
# 実行に要るもの(exe と ffmpeg の DLL)を dist にそろえる
mkdir -p dist && cp "$CARGO_TARGET_DIR/release/vengine.exe" dist/ && cp -u "$FFMPEG_DIR"/bin/*.dll dist/
