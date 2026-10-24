cmake_minimum_required(VERSION 3.21)
project(vedit-native LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT DEFINED FFMPEG_DIR)
  set(FFMPEG_DIR "C:/Libs/ffmpeg9/ffmpeg-n9.0-latest-win64-gpl-shared-9.0")
endif()
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Multimedia)

# core: 画面と分離(プロジェクト読み込み・時刻の対応・層の一覧)。Qt Core のみ
add_library(vcore STATIC src/core/timeutil.cpp src/core/project.cpp src/core/timeline.cpp src/core/ass.cpp src/core/expr.cpp src/core/fxcore.cpp src/core/presets.cpp)
target_include_directories(vcore PUBLIC src)
target_link_libraries(vcore PUBLIC Qt6::Core)

# media: libav でデコード(NVDEC/CPU)・先読み
add_library(vmedia STATIC src/media/videostream.cpp src/media/imageload.cpp src/media/cudainterop.cpp src/media/subrender.cpp src/media/audiomixer.cpp src/media/audioplayer.cpp src/media/qtthread.cpp)
target_include_directories(vmedia PUBLIC src ${FFMPEG_DIR}/include)
target_link_libraries(vmedia PUBLIC vcore Qt6::Gui Qt6::Multimedia
  ${FFMPEG_DIR}/lib/libavformat.dll.a ${FFMPEG_DIR}/lib/libavcodec.dll.a
  ${FFMPEG_DIR}/lib/libavfilter.dll.a ${FFMPEG_DIR}/lib/libswscale.dll.a ${FFMPEG_DIR}/lib/libswresample.dll.a ${FFMPEG_DIR}/lib/libavutil.dll.a)

# render: GL の描画器(窓あり/なしの両方で使う。テストからも使う)
add_library(vrender STATIC src/app/renderer.cpp src/app/renderer_post.cpp src/app/renderer_telop.cpp src/app/renderer_reload.cpp src/app/fxgen.cpp)
target_include_directories(vrender PUBLIC src src/app)
target_link_libraries(vrender PUBLIC vmedia Qt6::OpenGL)

add_executable(vedit-native src/app/main.cpp src/app/mainwindow.cpp src/app/stallwatch.cpp)
target_link_libraries(vedit-native PRIVATE vrender vmedia Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)

# ui: 編集画面(第5段階 第1部)。画面を持たない EditModel(app.js の状態・編集・保存の移植)と、QPainter のタイムライン・編集ウィンドウ
add_library(vui STATIC src/ui/ojson.cpp src/ui/editmodel.cpp src/ui/editops.cpp src/ui/timelineview.cpp src/ui/editorwindow.cpp src/ui/pybridge.cpp src/ui/widgets.cpp src/ui/editorpanels.cpp src/ui/editordialogs.cpp src/ui/reviewplayer.cpp)
target_include_directories(vui PUBLIC src src/ui)
target_link_libraries(vui PUBLIC vmedia Qt6::Widgets psapi)
target_link_libraries(vedit-native PRIVATE vui psapi)
add_executable(vedit-ui-test tests/ui_test.cpp)
target_link_libraries(vedit-ui-test PRIVATE vui)
add_executable(vedit-tl-bench tests/tl_bench.cpp)
target_link_libraries(vedit-tl-bench PRIVATE vui)
add_executable(vedit-gl-test tests/gl_test.cpp)
target_link_libraries(vedit-gl-test PRIVATE vui vrender)
