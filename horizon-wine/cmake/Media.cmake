include(ExternalProject)
find_program(MEDIA_MAKE make REQUIRED)
set(WINE_NX_FFMPEG_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../vendor/ffmpeg" CACHE PATH "FFmpeg 7.1.5 source checkout")
set(media_install "${CMAKE_CURRENT_BINARY_DIR}/media/install")
if(NOT EXISTS "${WINE_NX_FFMPEG_SOURCE}/configure")
    message(FATAL_ERROR "Run horizon-wine/tools/bootstrap-wma.sh before configuring the runtime")
endif()
set(media_jobs "$ENV{WINE_NX_JOBS}")
if(NOT media_jobs)
    set(media_jobs 8)
endif()
ExternalProject_Add(wine-media
    SOURCE_DIR "${WINE_NX_FFMPEG_SOURCE}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/media/build"
    INSTALL_DIR "${media_install}"
    CONFIGURE_COMMAND sh <SOURCE_DIR>/configure
        --prefix=<INSTALL_DIR> --arch=aarch64 --target-os=none
        --enable-cross-compile --cross-prefix=${DEVKITA64}/bin/aarch64-none-elf-
        --disable-everything --disable-autodetect --disable-programs --disable-doc
        --disable-network --disable-avdevice --disable-avfilter --disable-postproc
        --enable-pthreads --disable-w32threads --disable-os2threads
        --disable-shared --enable-static --enable-avformat --enable-avcodec
        --enable-avutil --enable-swresample --enable-swscale
        --enable-decoder=h264,aac,wmav1,wmav2,wmapro,wmalossless
        --enable-demuxer=mov --enable-parser=h264,aac --enable-bsf=h264_mp4toannexb
        "--extra-cflags=-O2 -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ffixed-x18 -ffunction-sections -fdata-sections -D__SWITCH__ -I${LIBNX}/include"
        "--extra-ldflags=-march=armv8-a+crc+crypto -mtp=soft -fPIE -specs=${LIBNX}/switch.specs -L${LIBNX}/lib"
        "--extra-libs=-lnx -lm"
    BUILD_COMMAND ${MEDIA_MAKE} -j${media_jobs}
    INSTALL_COMMAND ${MEDIA_MAKE} install
        COMMAND ${CMAKE_COMMAND} -E copy <SOURCE_DIR>/COPYING.LGPLv2.1
            "${CMAKE_CURRENT_BINARY_DIR}/licenses/FFmpeg-LGPL-2.1.txt"
    BUILD_BYPRODUCTS "${media_install}/lib/libavformat.a" "${media_install}/lib/libavcodec.a"
        "${media_install}/lib/libswscale.a" "${media_install}/lib/libswresample.a"
        "${media_install}/lib/libavutil.a")
add_library(wine-media-unix OBJECT
    ../dlls/winedmo/unixlib.c ../dlls/winedmo/unix_demuxer.c ../dlls/winedmo/unix_media_type.c
    source/media_unix.c)
add_dependencies(wine-media-unix wine-media)
target_compile_definitions(wine-media-unix PRIVATE ${WINE_SWITCH_NTDLL_DEFS}
    HAVE_FFMPEG HAVE_LIBAVCODEC_BSF_H __WINE_CONFIG_H
    __wine_unix_call_funcs=wine_nx_winedmo_unix_funcs
    __wine_unix_call_wow64_funcs=wine_nx_winedmo_wow64_unix_funcs)
target_include_directories(wine-media-unix PRIVATE ${WINE_SWITCH_WIN32U_INCLUDES}
    "${media_install}/include")
target_compile_options(wine-media-unix PRIVATE -std=gnu11 -Werror=implicit-function-declaration)
target_sources(wine-nx-runtime PRIVATE $<TARGET_OBJECTS:wine-media-unix>)
add_dependencies(wine-nx-runtime wine-media)
target_include_directories(wine-nx-runtime PRIVATE "${media_install}/include")
target_link_libraries(wine-nx-runtime PRIVATE "${media_install}/lib/libavformat.a"
    "${media_install}/lib/libavcodec.a" "${media_install}/lib/libswscale.a"
    "${media_install}/lib/libswresample.a" "${media_install}/lib/libavutil.a")
