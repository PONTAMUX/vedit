cmake_minimum_required(VERSION 3.21)
project(vaudio CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
# 浮動小数の挙動を JUCE/pedalboard(MSVC /fp:precise 相当)に寄せる: FMA 融合を禁止
add_compile_options(-ffp-contract=off -fno-fast-math)

set(VST3 C:/Libs/vst3sdk)
set(FFMPEG_ROOT "C:/Libs/ffmpeg9/ffmpeg-n9.0-latest-win64-gpl-shared-9.0")

add_library(vst3host STATIC
  ${VST3}/base/source/baseiids.cpp
  ${VST3}/base/source/fobject.cpp
  ${VST3}/base/source/fdebug.cpp
  ${VST3}/base/source/fstring.cpp
  ${VST3}/base/source/updatehandler.cpp
  ${VST3}/base/thread/source/flock.cpp
  ${VST3}/pluginterfaces/base/coreiids.cpp
  ${VST3}/pluginterfaces/base/funknown.cpp
  ${VST3}/pluginterfaces/base/ustring.cpp
  ${VST3}/pluginterfaces/base/conststringtable.cpp
  ${VST3}/public.sdk/source/vst/vstinitiids.cpp
  ${VST3}/public.sdk/source/common/memorystream.cpp
  ${VST3}/public.sdk/source/common/commoniids.cpp
  ${VST3}/public.sdk/source/vst/utility/stringconvert.cpp
  ${VST3}/public.sdk/source/common/commonstringconvert.cpp
  ${VST3}/public.sdk/source/vst/hosting/module.cpp
  ${VST3}/public.sdk/source/vst/hosting/module_win32.cpp
  ${VST3}/public.sdk/source/vst/hosting/hostclasses.cpp
  ${VST3}/public.sdk/source/vst/hosting/eventlist.cpp
  ${VST3}/public.sdk/source/vst/hosting/parameterchanges.cpp
  ${VST3}/public.sdk/source/vst/hosting/processdata.cpp
  ${VST3}/public.sdk/source/vst/hosting/plugprovider.cpp
  ${VST3}/public.sdk/source/vst/hosting/connectionproxy.cpp
  ${VST3}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
)
target_include_directories(vst3host PUBLIC ${VST3})
target_compile_definitions(vst3host PUBLIC RELEASE=1 UNICODE _UNICODE NOMINMAX)
target_compile_options(vst3host PRIVATE -w)
target_link_libraries(vst3host PUBLIC ole32 shell32 uuid)

add_executable(vst3probe tools/vst3probe.cpp)
target_link_libraries(vst3probe vst3host)

# ---- エンジン中核(DSP / JSON / メディア)
add_library(vcore STATIC src/json.cpp src/dsp.cpp src/media.cpp)
target_include_directories(vcore PUBLIC src ${FFMPEG_ROOT}/include)
target_link_directories(vcore PUBLIC ${FFMPEG_ROOT}/lib)
target_link_libraries(vcore PUBLIC avfilter avformat avcodec swresample avutil)

add_library(vcore_vst STATIC src/vst3host.cpp src/declick.cpp)
target_link_libraries(vcore_vst PUBLIC vcore vst3host)

add_library(vengine STATIC src/engine.cpp)
target_link_libraries(vengine PUBLIC vcore_vst)

add_executable(vaudio-cli src/main.cpp)
target_link_libraries(vaudio-cli vengine)

# ---- Qt GUI ----
set(CMAKE_PREFIX_PATH C:/Libs/Qt/6.10.3/mingw_64)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
set(CMAKE_AUTOMOC ON)
add_library(vgui STATIC src/gui.cpp)
# moc が日本語パスに書けないので autogen の出力先だけ ASCII のパスにする
set_target_properties(vgui PROPERTIES AUTOGEN_BUILD_DIR C:/Temp/vaudio_autogen/vgui)
target_include_directories(vgui PUBLIC src)
target_link_libraries(vgui PUBLIC vengine Qt6::Widgets)

# GUI 本体。--process / --worker も受ける(コンソールが親にあれば出力する)
add_executable(vaudio WIN32 src/main.cpp)
target_compile_definitions(vaudio PRIVATE VAUDIO_GUI)
target_link_libraries(vaudio vgui)

add_executable(gui_test tests/gui_test.cpp)
target_link_libraries(gui_test vgui)
