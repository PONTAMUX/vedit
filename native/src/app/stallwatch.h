#pragma once
// メインスレッドが長く止まったら、その時の呼び出しの段を %APPDATA%/vedit/stall.log に残す
void startStallWatch();
