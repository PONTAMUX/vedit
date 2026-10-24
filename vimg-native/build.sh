#!/bin/sh
# vimg-native のビルド(Git Bash で実行)。ビルド出力は C:/Temp/vimgn-cmake、実行一式は dist/
QT="C:/Libs/Qt/6.10.3/mingw_64"
GCC="C:/Libs/Qt/Tools/mingw1310_64/bin"
CM="C:/Libs/cpptools/Scripts"
B="C:/Temp/vimgn-cmake"
export PATH="$GCC:$CM:$QT/bin:$PATH"
cd "$(dirname "$0")" || exit 1
SRC="$(pwd -W 2>/dev/null || pwd)"
"$CM/cmake.exe" -S "$SRC" -B "$B" -G Ninja -DCMAKE_MAKE_PROGRAM="$CM/ninja.exe" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT" \
  -DCMAKE_C_COMPILER="$GCC/gcc.exe" -DCMAKE_CXX_COMPILER="$GCC/g++.exe" \
  -DCMAKE_CXX_FLAGS_RELEASE="-O2" || exit 1
"$CM/cmake.exe" --build "$B" || exit 1
mkdir -p dist && cp "$B/vimg.exe" "$B/vimg_test.exe" dist/ || exit 1
# Qt の DLL・プラグイン(webp など imageformats も)を dist にそろえる
"$QT/bin/windeployqt.exe" --release --no-translations --no-opengl-sw --no-system-d3d-compiler --no-compiler-runtime dist/vimg.exe >/dev/null || exit 1
mkdir -p dist/imageformats && cp -u "$QT/plugins/imageformats/qwebp.dll" dist/imageformats/ 2>/dev/null
cp -u "$GCC"/libgcc_s_seh-1.dll "$GCC"/libstdc++-6.dll "$GCC"/libwinpthread-1.dll dist/ 2>/dev/null
cp -u "$QT/bin/Qt6Test.dll" dist/
mkdir -p dist/platforms && cp -u "$QT/plugins/platforms/qoffscreen.dll" dist/platforms/
echo "OK: $SRC/dist/vimg.exe"
