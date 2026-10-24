#!/bin/sh
# vedit-native のビルド(Git Bash で実行)。ビルド出力は C:/Temp/vedit-native-build、実行一式は native/dist/
QT="C:/Libs/Qt/6.10.3/mingw_64"
GCC="C:/Libs/Qt/Tools/mingw1310_64/bin"
CM="C:/Libs/cpptools/Scripts"
FF="C:/Libs/ffmpeg9/ffmpeg-n9.0-latest-win64-gpl-shared-9.0"
B="C:/Temp/vedit-native-build"
export PATH="$GCC:$CM:$QT/bin:$PATH"
cd "$(dirname "$0")" || exit 1
SRC="$(pwd -W 2>/dev/null || pwd)"
"$CM/cmake.exe" -S "$SRC" -B "$B" -G Ninja -DCMAKE_MAKE_PROGRAM="$CM/ninja.exe" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT" -DFFMPEG_DIR="$FF" \
  -DCMAKE_C_COMPILER="$GCC/gcc.exe" -DCMAKE_CXX_COMPILER="$GCC/g++.exe" \
  -DCMAKE_CXX_FLAGS_RELEASE="-O2" || exit 1
"$CM/cmake.exe" --build "$B" || exit 1
mkdir -p dist && cp "$B/vedit-native.exe" "$B/vedit-ui-test.exe" "$B/vedit-gl-test.exe" dist/ || exit 1
cp -u "$FF"/bin/*.dll dist/
# Qt の DLL・プラグインを dist にそろえる(OpenGL ソフト実装は入れない)
"$QT/bin/windeployqt.exe" --release --no-translations --no-opengl-sw --no-system-d3d-compiler --no-compiler-runtime dist/vedit-native.exe >/dev/null || exit 1
mkdir -p dist/platforms && cp -u "$QT/plugins/platforms/qoffscreen.dll" dist/platforms/   # vedit-ui-test(offscreen)用
cp -u "$GCC"/libgcc_s_seh-1.dll "$GCC"/libstdc++-6.dll "$GCC"/libwinpthread-1.dll dist/ 2>/dev/null
echo "OK: $SRC/dist/vedit-native.exe"
