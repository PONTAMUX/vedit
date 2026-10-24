#!/bin/sh
QT="/c/Libs/Qt/6.10.3/mingw_64"; GCC="/c/Libs/Qt/Tools/mingw1310_64/bin"
export PATH="$GCC:$QT/bin:$PATH"
cd "$(dirname "$0")"; mkdir -p /c/Temp/vimgn-build
g++ -std=c++17 -O2 -Isrc -I$QT/include -I$QT/include/QtCore -I$QT/include/QtGui tools/cli_only.cpp src/model.cpp src/render.cpp -L$QT/lib -lQt6Gui -lQt6Core -o /c/Temp/vimgn-build/cli_only.exe
