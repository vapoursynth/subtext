cmake_minimum_required(VERSION 3.12)
project(subtext)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_C_VISIBILITY_PRESET hidden)

include(FetchContent)
FetchContent_Declare(VapourSynth
    URL          https://github.com/vapoursynth/vapoursynth/archive/refs/tags/R55-API4-RC3.zip
    URL_HASH MD5=2352CD4FD2DEEA88A9495279820AC2A0)
FetchContent_MakeAvailable(VapourSynth)

find_package(Iconv REQUIRED)
find_package(FFmpeg COMPONENTS AVCODEC AVFORMAT AVUTIL REQUIRED)
find_package(PkgConfig REQUIRED)
PKG_CHECK_MODULES(LIBASS REQUIRED libass>=0.12.0)

add_library(subtext SHARED
    "src/common.c"
    "src/image.cpp"
    "src/text.c"
    "src/toass.cpp"
    "src/toutf8.c"
)
target_include_directories(subtext PRIVATE "${vapoursynth_SOURCE_DIR}/include" ${FFMPEG_INCLUDE_DIRS} ${LIBASS_INCLUDE_DIRS})
target_link_libraries(subtext PRIVATE Iconv::Iconv ${FFMPEG_LIBRARIES} ${LIBASS_LINK_LIBRARIES})