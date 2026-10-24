#!/bin/sh
# 全文書を描く。第1引数: 実行ファイル(vimg.exe なら --render、cli_only なら位置引数)
export PATH="/c/Libs/Qt/6.10.3/mingw_64/bin:/c/Libs/Qt/Tools/mingw1310_64/bin:$PATH"
EXE="${1:-/c/Temp/vimgn-build/cli_only.exe}"
for d in /c/Temp/vimgn/*/; do
  [ -f "$d/doc.vimg.json" ] || continue
  case "$EXE" in
    *vimg.exe) timeout 60 "$EXE" --render "$d/doc.vimg.json" -o "$d/new.png" || echo "FAIL $d" ;;
    *) "$EXE" "$d/doc.vimg.json" "$d/new.png" || echo "FAIL $d" ;;
  esac
done
