cmake_minimum_required(VERSION 3.18)

project(wine_nx_probe LANGUAGES C CXX ASM)

set(WINE_NX_PE_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build-wine-arm64-pe-clean"
    CACHE PATH "Wine PE build supplying generated COM headers")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(wine-horizon OBJECT
    ../dlls/ntdll/unix/horizon.c
)

target_compile_definitions(wine-horizon PRIVATE
    __SWITCH__
    HORIZON_STANDALONE_SYNTAX
    HORIZON_NO_LIBNX_EXCEPTION_HANDLER
)

target_compile_options(wine-horizon PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
)

target_include_directories(wine-horizon PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/syntax"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dlls/ntdll/unix"
)

set(WINE_SWITCH_NTDLL_DEFS
    __SWITCH__
    __WINESRC__
    WINE_UNIX_LIB
    _WIN64
    _NTSYSTEM_
    _ACRTIMP=
    WINBASEAPI=
    WINE_NO_TRACE_MSGS
    WINE_NO_DEBUG_MSGS
)

set(WINE_SWITCH_NTDLL_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/syntax"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dlls/ntdll"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dlls/ntdll/unix"
)

add_library(wine-horizon-real OBJECT
    ../dlls/ntdll/unix/horizon.c
)

target_compile_definitions(wine-horizon-real PRIVATE ${WINE_SWITCH_NTDLL_DEFS})

target_compile_options(wine-horizon-real PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -ffunction-sections
    -fdata-sections
)

target_include_directories(wine-horizon-real PRIVATE ${WINE_SWITCH_NTDLL_INCLUDES})

add_library(wine-server-real OBJECT
    ../dlls/ntdll/unix/server.c
)

target_compile_definitions(wine-server-real PRIVATE ${WINE_SWITCH_NTDLL_DEFS})

target_compile_options(wine-server-real PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -ffunction-sections
    -fdata-sections
)

target_include_directories(wine-server-real PRIVATE ${WINE_SWITCH_NTDLL_INCLUDES})

add_library(wine-sync-real OBJECT
    ../dlls/ntdll/unix/sync.c
)

target_compile_definitions(wine-sync-real PRIVATE ${WINE_SWITCH_NTDLL_DEFS})

target_compile_options(wine-sync-real PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -ffunction-sections
    -fdata-sections
)

target_include_directories(wine-sync-real PRIVATE ${WINE_SWITCH_NTDLL_INCLUDES})

add_library(wine-file-real OBJECT
    ../dlls/ntdll/unix/file.c
)

target_compile_definitions(wine-file-real PRIVATE ${WINE_SWITCH_NTDLL_DEFS})

target_compile_options(wine-file-real PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -ffunction-sections
    -fdata-sections
)

target_include_directories(wine-file-real PRIVATE ${WINE_SWITCH_NTDLL_INCLUDES})

add_library(wine-thread-real OBJECT
    ../dlls/ntdll/unix/thread.c
)

target_compile_definitions(wine-thread-real PRIVATE ${WINE_SWITCH_NTDLL_DEFS})

target_compile_options(wine-thread-real PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -ffunction-sections
    -fdata-sections
)

target_include_directories(wine-thread-real PRIVATE ${WINE_SWITCH_NTDLL_INCLUDES})

add_library(wine-virtual-real OBJECT
    ../dlls/ntdll/unix/virtual.c
)

target_compile_definitions(wine-virtual-real PRIVATE ${WINE_SWITCH_NTDLL_DEFS})

target_compile_options(wine-virtual-real PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -ffunction-sections
    -fdata-sections
)

target_include_directories(wine-virtual-real PRIVATE ${WINE_SWITCH_NTDLL_INCLUDES})

add_library(wine-signal-arm64-real OBJECT
    ../dlls/ntdll/unix/alpc.c
    ../dlls/ntdll/unix/signal_arm64.c
    ../dlls/ntdll/unix/syscall.c
)

target_compile_definitions(wine-signal-arm64-real PRIVATE ${WINE_SWITCH_NTDLL_DEFS})

target_compile_options(wine-signal-arm64-real PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -ffunction-sections
    -fdata-sections
)

target_include_directories(wine-signal-arm64-real PRIVATE ${WINE_SWITCH_NTDLL_INCLUDES})

add_library(wine-env-real OBJECT
    ../dlls/ntdll/unix/env.c
)

target_compile_definitions(wine-env-real PRIVATE ${WINE_SWITCH_NTDLL_DEFS})

target_compile_options(wine-env-real PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -ffunction-sections
    -fdata-sections
)

target_include_directories(wine-env-real PRIVATE ${WINE_SWITCH_NTDLL_INCLUDES})

add_library(wine-debug-real OBJECT
    ../dlls/ntdll/unix/debug.c
)

target_compile_definitions(wine-debug-real PRIVATE ${WINE_SWITCH_NTDLL_DEFS})

target_compile_options(wine-debug-real PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -ffunction-sections
    -fdata-sections
)

target_include_directories(wine-debug-real PRIVATE ${WINE_SWITCH_NTDLL_INCLUDES})

add_library(wine-system-real OBJECT
    ../dlls/ntdll/unix/system.c
)

target_compile_definitions(wine-system-real PRIVATE ${WINE_SWITCH_NTDLL_DEFS})

target_compile_options(wine-system-real PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -ffunction-sections
    -fdata-sections
)

target_include_directories(wine-system-real PRIVATE ${WINE_SWITCH_NTDLL_INCLUDES})

add_library(wine-loadorder-real OBJECT
    ../dlls/ntdll/unix/loadorder.c
)

target_compile_definitions(wine-loadorder-real PRIVATE ${WINE_SWITCH_NTDLL_DEFS})

target_compile_options(wine-loadorder-real PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -ffunction-sections
    -fdata-sections
)

target_include_directories(wine-loadorder-real PRIVATE ${WINE_SWITCH_NTDLL_INCLUDES})

add_library(wine-loader-unix-real OBJECT
    ../dlls/ntdll/unix/loader.c
)

target_compile_definitions(wine-loader-unix-real PRIVATE ${WINE_SWITCH_NTDLL_DEFS})

target_compile_options(wine-loader-unix-real PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -ffunction-sections
    -fdata-sections
)

target_include_directories(wine-loader-unix-real PRIVATE ${WINE_SWITCH_NTDLL_INCLUDES})

add_library(wine-ntdll-loader-real OBJECT
    ../dlls/ntdll/loader.c
)

target_compile_definitions(wine-ntdll-loader-real PRIVATE
    __SWITCH__
    __WINESRC__
    WINE_NX_PE_LOADER
    _WIN64
    _NTSYSTEM_
    _ACRTIMP=
    WINBASEAPI=
    WINE_NO_TRACE_MSGS
    WINE_NO_DEBUG_MSGS
    HORIZON_NO_LIBNX_EXCEPTION_HANDLER
)

target_compile_options(wine-ntdll-loader-real PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -ffunction-sections
    -fdata-sections
    -fshort-wchar
    -include
    "${CMAKE_CURRENT_SOURCE_DIR}/source/ntdll_loader_compat.h"
)

target_include_directories(wine-ntdll-loader-real PRIVATE ${WINE_SWITCH_NTDLL_INCLUDES})

foreach(WINE_SWITCH_EXTRA_UNIX_MODULE IN ITEMS registry security process socket serial cdrom tape)
    add_library(wine-${WINE_SWITCH_EXTRA_UNIX_MODULE}-real OBJECT
        ../dlls/ntdll/unix/${WINE_SWITCH_EXTRA_UNIX_MODULE}.c
    )

    target_compile_definitions(wine-${WINE_SWITCH_EXTRA_UNIX_MODULE}-real PRIVATE ${WINE_SWITCH_NTDLL_DEFS})

    target_compile_options(wine-${WINE_SWITCH_EXTRA_UNIX_MODULE}-real PRIVATE
        -std=gnu99
        -Wall
        -Wextra
        -Werror=implicit-function-declaration
        -ffunction-sections
        -fdata-sections
    )

    target_include_directories(wine-${WINE_SWITCH_EXTRA_UNIX_MODULE}-real PRIVATE ${WINE_SWITCH_NTDLL_INCLUDES})
endforeach()

# win32u unix side: GDI/USER kernel implementation + syscall table (KeServiceDescriptorTable[1]).
# This is what NtUser*/NtGdi* syscalls dispatch into; required for any GUI app.
set(WINE_SWITCH_WIN32U_INCLUDES
    ${WINE_SWITCH_NTDLL_INCLUDES}
    # <dlfcn.h> shim: Horizon has no dynamic linker; dlopen() returns NULL so
    # opengl/vulkan/freetype fall back to "no host driver". Must precede any
    # toolchain dlfcn.h (there is none on devkitA64, but keep it first).
    "${CMAKE_CURRENT_SOURCE_DIR}/switch-shims"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dlls/win32u"
    # widl-generated COM headers (objidlbase.h etc.) produced by the PE build;
    # last so checked-in include/ and syntax/config.h still win.
    "${WINE_NX_PE_BUILD_DIR}/include"
    "$ENV{DEVKITPRO}/portlibs/switch/include/freetype2"
)

add_library(wine-win32u-real OBJECT
    ../dlls/win32u/bitblt.c
    ../dlls/win32u/bitmap.c
    ../dlls/win32u/brush.c
    ../dlls/win32u/class.c
    ../dlls/win32u/clipboard.c
    ../dlls/win32u/clipping.c
    ../dlls/win32u/cursoricon.c
    ../dlls/win32u/d3dkmt_switch.c
    ../dlls/win32u/dc.c
    ../dlls/win32u/dce.c
    ../dlls/win32u/defwnd.c
    ../dlls/win32u/dib.c
    ../dlls/win32u/driver.c
    ../dlls/win32u/emfdrv.c
    ../dlls/win32u/font.c
    ../dlls/win32u/freetype.c
    ../dlls/win32u/gdiobj.c
    ../dlls/win32u/hook.c
    ../dlls/win32u/imm.c
    ../dlls/win32u/input.c
    ../dlls/win32u/mapping.c
    ../dlls/win32u/menu.c
    ../dlls/win32u/message.c
    ../dlls/win32u/opengl.c
    ../dlls/win32u/opentype.c
    ../dlls/win32u/painting.c
    ../dlls/win32u/palette.c
    ../dlls/win32u/path.c
    ../dlls/win32u/pen.c
    ../dlls/win32u/printdrv.c
    ../dlls/win32u/rawinput.c
    ../dlls/win32u/region.c
    ../dlls/win32u/scroll.c
    ../dlls/win32u/spy.c
    ../dlls/win32u/syscall.c
    ../dlls/win32u/sysparams.c
    ../dlls/win32u/systray.c
    ../dlls/win32u/vertical.c
    ../dlls/win32u/vulkan.c
    ../dlls/win32u/window.c
    ../dlls/win32u/winstation.c
    ../dlls/win32u/winnx_drv.c
    ../dlls/win32u/winnx_opengl.c
    # dibdrv: the software DIB rasterizer — the actual pixel engine that draws
    # into memory bitmaps / window surfaces. Core of the software window path.
    ../dlls/win32u/dibdrv/bitblt.c
    ../dlls/win32u/dibdrv/dc.c
    ../dlls/win32u/dibdrv/graphics.c
    ../dlls/win32u/dibdrv/objects.c
    ../dlls/win32u/dibdrv/primitives.c
)

target_compile_definitions(wine-win32u-real PRIVATE
    ${WINE_SWITCH_NTDLL_DEFS}
    HAVE_FT2BUILD_H=1
    SONAME_LIBFREETYPE="libfreetype.a"
    SONAME_LIBEGL="libEGL.a"
)

target_compile_options(wine-win32u-real PRIVATE
    -std=gnu11
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -ffunction-sections
    -fdata-sections
)

target_include_directories(wine-win32u-real PRIVATE ${WINE_SWITCH_WIN32U_INCLUDES})

# opengl32's unix side: wgl and the GL thunks over win32u's EGL functions, with
# its unixlib tables renamed for virtual.c's static table lookup.
add_library(wine-opengl32-unix-real OBJECT
    ../dlls/opengl32/unix_thunks.c
    ../dlls/opengl32/unix_wgl.c
    source/opengl32_unix.c
)

target_compile_definitions(wine-opengl32-unix-real PRIVATE
    ${WINE_SWITCH_NTDLL_DEFS}
    _OPENGL32_
    __wine_unix_call_funcs=wine_nx_opengl32_unix_funcs
    __wine_unix_call_wow64_funcs=wine_nx_opengl32_wow64_unix_funcs
)

target_compile_options(wine-opengl32-unix-real PRIVATE
    -std=gnu11
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -ffunction-sections
    -fdata-sections
)

target_include_directories(wine-opengl32-unix-real PRIVATE
    ${WINE_SWITCH_WIN32U_INCLUDES}
    "${CMAKE_CURRENT_SOURCE_DIR}/../dlls/opengl32"
)

add_executable(wine-nx-probe
    source/main.cpp
    $<TARGET_OBJECTS:wine-horizon>
)

set_target_properties(wine-nx-probe PROPERTIES SUFFIX ".elf")

target_compile_options(wine-nx-probe PRIVATE
    -Wall
    -Wextra
    -fno-exceptions
    -fno-rtti
)

target_include_directories(wine-nx-probe PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../dlls/ntdll/unix"
)

target_link_libraries(wine-nx-probe PRIVATE nx)

add_executable(wine-nx-pe-smoke
    source/pe_smoke.c
    $<TARGET_OBJECTS:wine-horizon>
)

set_target_properties(wine-nx-pe-smoke PROPERTIES SUFFIX ".elf")

target_compile_options(wine-nx-pe-smoke PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
)

target_include_directories(wine-nx-pe-smoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../dlls/ntdll/unix"
)

target_link_libraries(wine-nx-pe-smoke PRIVATE nx)

add_executable(wine-nx-pe-real-report
    source/pe_real_report.c
    $<TARGET_OBJECTS:wine-horizon>
)

set_target_properties(wine-nx-pe-real-report PROPERTIES SUFFIX ".elf")

target_compile_options(wine-nx-pe-real-report PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
)

target_include_directories(wine-nx-pe-real-report PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../dlls/ntdll/unix"
)

target_link_libraries(wine-nx-pe-real-report PRIVATE nx)

add_executable(wine-nx-pe-real-run
    source/pe_real_report.c
    $<TARGET_OBJECTS:wine-horizon>
)

set_target_properties(wine-nx-pe-real-run PROPERTIES SUFFIX ".elf")

target_compile_definitions(wine-nx-pe-real-run PRIVATE PE_REAL_EXECUTE=1)

target_compile_options(wine-nx-pe-real-run PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
)

target_include_directories(wine-nx-pe-real-run PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../dlls/ntdll/unix"
)

target_link_libraries(wine-nx-pe-real-run PRIVATE nx)

add_executable(wine-nx-pe-ntdll-smoke
    source/pe_ntdll_smoke.c
    source/ntdll_server_smoke_stubs.c
    $<TARGET_OBJECTS:wine-horizon-real>
    $<TARGET_OBJECTS:wine-server-real>
    $<TARGET_OBJECTS:wine-sync-real>
)

set_target_properties(wine-nx-pe-ntdll-smoke PROPERTIES SUFFIX ".elf")

target_compile_definitions(wine-nx-pe-ntdll-smoke PRIVATE ${WINE_SWITCH_NTDLL_DEFS})

target_compile_options(wine-nx-pe-ntdll-smoke PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -ffunction-sections
    -fdata-sections
)

target_include_directories(wine-nx-pe-ntdll-smoke PRIVATE ${WINE_SWITCH_NTDLL_INCLUDES})

target_link_options(wine-nx-pe-ntdll-smoke PRIVATE -Wl,--gc-sections)

target_link_libraries(wine-nx-pe-ntdll-smoke PRIVATE nx)

add_executable(wine-nx-ntdll-smoke
    source/ntdll_server_smoke.c
    source/ntdll_server_smoke_stubs.c
    $<TARGET_OBJECTS:wine-horizon-real>
    $<TARGET_OBJECTS:wine-server-real>
)

set_target_properties(wine-nx-ntdll-smoke PROPERTIES SUFFIX ".elf")

target_compile_definitions(wine-nx-ntdll-smoke PRIVATE ${WINE_SWITCH_NTDLL_DEFS})

target_compile_options(wine-nx-ntdll-smoke PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -ffunction-sections
    -fdata-sections
)

target_include_directories(wine-nx-ntdll-smoke PRIVATE ${WINE_SWITCH_NTDLL_INCLUDES})

target_link_options(wine-nx-ntdll-smoke PRIVATE -Wl,--gc-sections)

target_link_libraries(wine-nx-ntdll-smoke PRIVATE nx)

add_executable(wine-nx-ntdll-file-smoke
    source/ntdll_file_smoke.c
    $<TARGET_OBJECTS:wine-horizon-real>
    $<TARGET_OBJECTS:wine-server-real>
    $<TARGET_OBJECTS:wine-sync-real>
    $<TARGET_OBJECTS:wine-file-real>
    $<TARGET_OBJECTS:wine-thread-real>
    $<TARGET_OBJECTS:wine-virtual-real>
    $<TARGET_OBJECTS:wine-signal-arm64-real>
    $<TARGET_OBJECTS:wine-env-real>
    $<TARGET_OBJECTS:wine-debug-real>
    $<TARGET_OBJECTS:wine-system-real>
)

set_target_properties(wine-nx-ntdll-file-smoke PROPERTIES SUFFIX ".elf")

target_compile_definitions(wine-nx-ntdll-file-smoke PRIVATE ${WINE_SWITCH_NTDLL_DEFS})

target_compile_options(wine-nx-ntdll-file-smoke PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -ffunction-sections
    -fdata-sections
)

target_include_directories(wine-nx-ntdll-file-smoke PRIVATE ${WINE_SWITCH_NTDLL_INCLUDES})

target_link_options(wine-nx-ntdll-file-smoke PRIVATE -Wl,--gc-sections)

target_link_libraries(wine-nx-ntdll-file-smoke PRIVATE nx)

# The homebrew loader a forwarder is built around: the same nx-hbloader sphaira
# uses, built here as the exefs of the program NCA the launcher installs. Its
# own address space is patched into the NPDM when a forwarder is made.
add_executable(wine-nx-hbl
    hbl/source/main.c
    hbl/source/trampoline.s
)
set_target_properties(wine-nx-hbl PROPERTIES SUFFIX ".elf" C_STANDARD 11 C_EXTENSIONS ON)
target_compile_definitions(wine-nx-hbl PRIVATE VERSION="3.0.0")
target_link_options(wine-nx-hbl PRIVATE -Wl,-wrap,exit)
target_link_libraries(wine-nx-hbl PRIVATE nx)

add_executable(autorun-game-forwarder source/game_forwarder.c)
set_target_properties(autorun-game-forwarder PROPERTIES SUFFIX ".elf" C_STANDARD 11)
target_link_options(autorun-game-forwarder PRIVATE -Wl,--gc-sections)
target_link_libraries(autorun-game-forwarder PRIVATE nx)

add_executable(wine-nx-runtime
    source/runtime.c
    "${CMAKE_CURRENT_BINARY_DIR}/forwarder_embed.c"
    source/launcher.c
    source/launcher_graphics.c
    source/launcher_catalog.c
    source/launcher_console.c
    source/launcher_svg.c
    source/launcher_ui.c
    source/launcher_audio.c
    source/launcher_forwarder.c
    source/launcher_image.c
    source/steamgriddb.c
    source/dxvk_releases.c
    source/autorun_update.c
    source/autorun_install.c
    source/launcher_update.c
    source/launcher_setup.c
    source/setup_boot.c
    source/runtime_platform.c
    source/sd_cache.c
    source/wow64_box64_bridge.c
    source/ntdll_pe_compat.c
    source/ws2_32_unix_stub.c
    source/audio_unix.c
    source/xinput_unix.c
    source/thread_profile.c
    source/compositor.c
    source/osk.c
    source/compositor_gl.c
    source/compositor_egl.c
    source/forwarder.c
    $<TARGET_OBJECTS:wine-horizon-real>
    $<TARGET_OBJECTS:wine-server-real>
    $<TARGET_OBJECTS:wine-sync-real>
    $<TARGET_OBJECTS:wine-file-real>
    $<TARGET_OBJECTS:wine-thread-real>
    $<TARGET_OBJECTS:wine-virtual-real>
    $<TARGET_OBJECTS:wine-signal-arm64-real>
    $<TARGET_OBJECTS:wine-env-real>
    $<TARGET_OBJECTS:wine-debug-real>
    $<TARGET_OBJECTS:wine-system-real>
    $<TARGET_OBJECTS:wine-loadorder-real>
    $<TARGET_OBJECTS:wine-loader-unix-real>
    $<TARGET_OBJECTS:wine-ntdll-loader-real>
    $<TARGET_OBJECTS:wine-registry-real>
    $<TARGET_OBJECTS:wine-security-real>
    $<TARGET_OBJECTS:wine-process-real>
    $<TARGET_OBJECTS:wine-socket-real>
    $<TARGET_OBJECTS:wine-serial-real>
    $<TARGET_OBJECTS:wine-cdrom-real>
    $<TARGET_OBJECTS:wine-tape-real>
    $<TARGET_OBJECTS:wine-win32u-real>
    $<TARGET_OBJECTS:wine-opengl32-unix-real>
)

set_target_properties(wine-nx-runtime PROPERTIES SUFFIX ".elf")

# What the launcher carries to build a forwarder with: the loader's exefs and
# the icons the console shows for each of the two it can make.
find_program(ELF2NSO elf2nso HINTS "$ENV{DEVKITPRO}/tools/bin" "/opt/devkitpro/tools/bin")
find_program(NPDMTOOL npdmtool HINTS "$ENV{DEVKITPRO}/tools/bin" "/opt/devkitpro/tools/bin")
if(ELF2NSO AND NPDMTOOL)
    set(HBL_NSO "${CMAKE_CURRENT_BINARY_DIR}/hbl-main")
    set(HBL_NPDM "${CMAKE_CURRENT_BINARY_DIR}/hbl-main.npdm")
    set(GAME_FORWARDER_NSO "${CMAKE_CURRENT_BINARY_DIR}/game-forwarder-main")
    add_custom_command(
        OUTPUT "${GAME_FORWARDER_NSO}"
        COMMAND "${ELF2NSO}" "$<TARGET_FILE:autorun-game-forwarder>" "${GAME_FORWARDER_NSO}"
        DEPENDS autorun-game-forwarder
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${HBL_NSO}"
        COMMAND "${ELF2NSO}" "$<TARGET_FILE:wine-nx-hbl>" "${HBL_NSO}"
        DEPENDS wine-nx-hbl
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${HBL_NPDM}"
        COMMAND "${NPDMTOOL}" "${CMAKE_CURRENT_SOURCE_DIR}/hbl/hbl.json" "${HBL_NPDM}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/hbl/hbl.json"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/forwarder_embed.c"
        COMMAND "${CMAKE_COMMAND}" -E env python3
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/make-embed.py"
                "${CMAKE_CURRENT_BINARY_DIR}/forwarder_embed.c"
                wine_nx_hbl_main "${HBL_NSO}"
                wine_nx_game_forwarder_main "${GAME_FORWARDER_NSO}"
                wine_nx_hbl_npdm "${HBL_NPDM}"
                wine_nx_icon_any "${CMAKE_CURRENT_SOURCE_DIR}/assets/autorun.jpg"
                wine_nx_logo "${CMAKE_CURRENT_SOURCE_DIR}/assets/logo.png"
        DEPENDS "${HBL_NSO}" "${HBL_NPDM}" "${GAME_FORWARDER_NSO}"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/autorun.jpg"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/logo.png"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/make-embed.py"
        VERBATIM
    )
else()
    message(FATAL_ERROR "elf2nso and npdmtool are needed to build the forwarder loader")
endif()
# libnx's service macros use C11's static_assert, which the rest of the tree,
# built as gnu99, does not have.
set_source_files_properties(source/forwarder.c PROPERTIES COMPILE_OPTIONS "-std=gnu11")
set_source_files_properties(source/audio_unix.c source/xinput_unix.c PROPERTIES COMPILE_DEFINITIONS WINE_UNIX_LIB)
target_include_directories(wine-nx-runtime PRIVATE "${WINE_NX_PE_BUILD_DIR}/include")
find_package(Git QUIET)
set(AUTORUN_BUILD_TAG "")
set(AUTORUN_BUILD_EPOCH 0)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.." OUTPUT_VARIABLE AUTORUN_BUILD_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${GIT_EXECUTABLE}" log -1 --format=%ct
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.." OUTPUT_VARIABLE AUTORUN_BUILD_EPOCH
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --git-path HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.." OUTPUT_VARIABLE AUTORUN_GIT_HEAD
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${GIT_EXECUTABLE}" symbolic-ref -q HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.." OUTPUT_VARIABLE AUTORUN_GIT_REF
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(AUTORUN_GIT_HEAD)
        get_filename_component(AUTORUN_GIT_HEAD "${AUTORUN_GIT_HEAD}" ABSOLUTE
            BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${AUTORUN_GIT_HEAD}")
    endif()
    if(AUTORUN_GIT_REF)
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --git-path "${AUTORUN_GIT_REF}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.." OUTPUT_VARIABLE AUTORUN_GIT_REF_PATH
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        get_filename_component(AUTORUN_GIT_REF_PATH "${AUTORUN_GIT_REF_PATH}" ABSOLUTE
            BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${AUTORUN_GIT_REF_PATH}")
    endif()
endif()
if(NOT AUTORUN_BUILD_EPOCH MATCHES "^[0-9]+$")
    set(AUTORUN_BUILD_EPOCH 0)
endif()
configure_file(cmake/autorun_version.h.in autorun_version.h @ONLY)
target_include_directories(wine-nx-runtime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

option(WINE_NX_USB_STORAGE "Mount USB drives as D: to H: through libusbhsfs" OFF)
if(WINE_NX_USB_STORAGE)
    set(WINE_NX_LIBUSBHSFS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libusbhsfs" CACHE PATH
        "libusbhsfs checkout made by tools/bootstrap-libusbhsfs.sh")
    if(NOT EXISTS "${WINE_NX_LIBUSBHSFS_DIR}/include/usbhsfs.h")
        message(FATAL_ERROR "Missing libusbhsfs; run wine-nx-probe/tools/bootstrap-libusbhsfs.sh")
    endif()
    # FAT and exFAT through FatFs; NTFS and ext4 would link GPL libraries.
    set(USBHSFS_GPL OFF CACHE BOOL "" FORCE)
    set(USBHSFS_NTFS OFF CACHE BOOL "" FORCE)
    set(USBHSFS_EXT4 OFF CACHE BOOL "" FORCE)
    set(USBHSFS_SXOS_DISABLE ON CACHE BOOL "" FORCE)
    set(USBHSFS_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory("${WINE_NX_LIBUSBHSFS_DIR}" libusbhsfs EXCLUDE_FROM_ALL)
    set(WINE_NX_USBHSFS_UASP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/usbhsfs-uasp")
    target_sources(libusbhsfs PRIVATE "${WINE_NX_USBHSFS_UASP_DIR}/usbhsfs_uasp.c")
    target_include_directories(libusbhsfs PRIVATE "${WINE_NX_USBHSFS_UASP_DIR}" "${WINE_NX_LIBUSBHSFS_DIR}/source")
    target_compile_definitions(libusbhsfs PRIVATE get_fattime=usbhsfs_get_fattime)
    target_sources(wine-nx-runtime PRIVATE source/usb_storage.c)
    target_compile_definitions(wine-nx-runtime PRIVATE WINE_NX_USB_STORAGE)
    target_link_libraries(wine-nx-runtime PRIVATE libusbhsfs)
endif()

option(WINE_NX_BOX64_INTERPRETER "Build the Box64 interpreter adapter" OFF)
option(WINE_NX_BOX64_DYNAREC "Use the ARM64 dynarec in the Box64 adapter" OFF)
option(WINE_NX_AMD64 "Enable AMD64 applications through Wine ARM64EC and Box64" OFF)
option(WINE_NX_FEX "Enable the Horizon FEX WoW64 and ARM64EC backends" OFF)
if(WINE_NX_FEX AND NOT WINE_NX_AMD64)
    message(FATAL_ERROR "WINE_NX_FEX requires WINE_NX_AMD64")
endif()
if(WINE_NX_AMD64 AND NOT (WINE_NX_BOX64_INTERPRETER OR WINE_NX_BOX64_DYNAREC))
    message(FATAL_ERROR "WINE_NX_AMD64 requires WINE_NX_BOX64_INTERPRETER or WINE_NX_BOX64_DYNAREC")
endif()
if(WINE_NX_BOX64_INTERPRETER OR WINE_NX_BOX64_DYNAREC)
    include(cmake/Box64Core.cmake)
    if(WINE_NX_BOX64_DYNAREC)
        wine_nx_add_box64_core(wine-nx-box64-core DYNAREC)
        target_sources(wine-nx-runtime PRIVATE source/wow64_box64_dynarec.c)
    else()
        wine_nx_add_box64_core(wine-nx-box64-core)
    endif()
    target_sources(wine-nx-runtime PRIVATE source/wow64_box64_engine.c source/wow64_box64_unix.c)
    set_source_files_properties(source/wow64_box64_unix.c PROPERTIES COMPILE_DEFINITIONS WINE_UNIX_LIB)
    target_compile_definitions(wine-virtual-real PRIVATE WINE_NX_BOX64_INTERPRETER)
    target_compile_definitions(wine-nx-runtime PRIVATE WINE_NX_BOX64_INTERPRETER)
    target_link_libraries(wine-nx-runtime PRIVATE wine-nx-box64-core)
endif()
if(WINE_NX_AMD64)
    target_sources(wine-nx-runtime PRIVATE source/arm64ec_box64_unix.c)
    set_source_files_properties(source/arm64ec_box64_unix.c PROPERTIES COMPILE_DEFINITIONS WINE_UNIX_LIB)
    target_compile_definitions(wine-virtual-real PRIVATE WINE_NX_AMD64)
    target_compile_definitions(wine-nx-runtime PRIVATE WINE_NX_AMD64)
endif()
if(WINE_NX_FEX)
    target_sources(wine-nx-runtime PRIVATE source/fex_jit.c source/fex_unix.c source/fex_context.S source/fex_exceptions.c)
    set_source_files_properties(source/fex_unix.c PROPERTIES COMPILE_DEFINITIONS WINE_UNIX_LIB)
    target_compile_definitions(wine-virtual-real PRIVATE WINE_NX_FEX)
    target_compile_definitions(wine-nx-runtime PRIVATE WINE_NX_FEX)
    target_link_options(wine-nx-runtime PRIVATE "-Wl,--wrap=__libnx_exception_entry")
endif()

target_compile_definitions(wine-nx-runtime PRIVATE ${WINE_SWITCH_NTDLL_DEFS})

target_compile_options(wine-nx-runtime PRIVATE
    -std=gnu99
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -ffunction-sections
    -fdata-sections
)

target_include_directories(wine-nx-runtime PRIVATE ${WINE_SWITCH_NTDLL_INCLUDES})

target_sources(wine-nx-runtime PRIVATE source/horizon_virtmem.c source/low_window.c source/native_heap.c)
foreach(symbol _sbrk_r _free_r _malloc_usable_size_r _mallinfo_r _memalign_r _realloc_r)
    target_link_options(wine-nx-runtime PRIVATE "-Wl,--wrap=${symbol}")
endforeach()

option(WINE_NX_SWAP_POC "Build experimental SD-backed guest memory paging" OFF)
if(WINE_NX_SWAP_POC)
    if(NOT WINE_NX_FEX)
        message(FATAL_ERROR "WINE_NX_SWAP_POC requires the per-thread FEX exception entry")
    endif()
    target_sources(wine-nx-runtime PRIVATE source/swap_store.c source/swap_file.c source/swap_native.c
        source/swap_pager.c source/swap_poc.c source/launcher_swap.c)
    target_compile_definitions(wine-nx-runtime PRIVATE WINE_NX_SWAP_POC)
    foreach(target wine-horizon-real wine-virtual-real wine-system-real wine-win32u-real)
        target_compile_definitions(${target} PRIVATE WINE_NX_SWAP_POC)
    endforeach()
    foreach(symbol threadCreate threadClose svcSendSyncRequest svcSendSyncRequestWithUserBuffer
            svcReplyAndReceive svcReplyAndReceiveWithUserBuffer svcSendAsyncRequestWithUserBuffer
            svcCreateTransferMemory nvioctlNvmap_Alloc nvMapCreate svcSetMemoryAttribute svcMapMemory
            svcMapDeviceAddressSpace svcMapDeviceAddressSpaceByForce svcMapDeviceAddressSpaceAligned
            svcWaitForAddress svcSignalToAddress svcArbitrateLock svcArbitrateUnlock
            svcWaitProcessWideKeyAtomic svcSignalProcessWideKey _malloc_r _calloc_r)
        target_link_options(wine-nx-runtime PRIVATE "-Wl,--wrap=${symbol}")
    endforeach()
endif()
target_link_options(wine-nx-runtime PRIVATE -Wl,--gc-sections -Wl,--wrap=svcCreateThread
    -Wl,--wrap=svcSetThreadCoreMask
    -Wl,--wrap=virtmemFindAslr -Wl,--wrap=virtmemFindCodeMemory
    -Wl,--wrap=virtmemFindStack -Wl,--wrap=virtmemAddReservation -Wl,--wrap=virtmemRemoveReservation)

# freetype (+ its deps) for win32u font rendering; nx last for the libnx syscalls
find_library(SWITCH_FREETYPE freetype HINTS "$ENV{DEVKITPRO}/portlibs/switch/lib")
find_library(SWITCH_HARFBUZZ harfbuzz HINTS "$ENV{DEVKITPRO}/portlibs/switch/lib")
find_library(SWITCH_PNG png HINTS "$ENV{DEVKITPRO}/portlibs/switch/lib")
find_library(SWITCH_TURBOJPEG turbojpeg HINTS "$ENV{DEVKITPRO}/portlibs/switch/lib" REQUIRED)
find_library(SWITCH_BZ2 bz2 HINTS "$ENV{DEVKITPRO}/portlibs/switch/lib")
find_library(SWITCH_Z z HINTS "$ENV{DEVKITPRO}/portlibs/switch/lib")
find_library(SWITCH_ZSTD zstd HINTS "$ENV{DEVKITPRO}/portlibs/switch/lib" REQUIRED)
find_library(SWITCH_CURL curl HINTS "$ENV{DEVKITPRO}/portlibs/switch/lib")
find_library(SWITCH_MINIZIP minizip HINTS "$ENV{DEVKITPRO}/portlibs/switch/lib" REQUIRED)
find_library(SWITCH_MBEDTLS mbedtls HINTS "$ENV{DEVKITPRO}/portlibs/switch/lib")
find_library(SWITCH_MBEDX509 mbedx509 HINTS "$ENV{DEVKITPRO}/portlibs/switch/lib")
find_library(SWITCH_MBEDCRYPTO mbedcrypto HINTS "$ENV{DEVKITPRO}/portlibs/switch/lib")
# devkitPro's Mesa for OpenGL through EGL on the GPU (nouveau over nvservices);
# its nouveau compiler is C++.
find_library(SWITCH_EGL EGL HINTS "$ENV{DEVKITPRO}/portlibs/switch/lib")
find_library(SWITCH_GLAPI glapi HINTS "$ENV{DEVKITPRO}/portlibs/switch/lib")
find_library(SWITCH_DRM_NOUVEAU drm_nouveau HINTS "$ENV{DEVKITPRO}/portlibs/switch/lib")
# build-switch-mesa.sh builds switch-mesa with Wine-NX's nouveau changes; this
# links it instead of devkitPro's package.
set(WINE_NX_MESA_LIB_DIR "" CACHE PATH "Folder with libEGL.a, libglapi.a and libdrm_nouveau.a from build-switch-mesa.sh")
option(WINE_NX_STOCK_MESA "Link devkitPro's Mesa even when build-switch-mesa.sh's is installed" OFF)
# A build tree configured without the option silently lost every nouveau fix
# (NFSU2 builds 56-66), so use build-switch-mesa.sh's install when it is there.
set(WINE_NX_MESA_DEFAULT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build-switch-mesa/install/opt/devkitpro/portlibs/switch/lib")
if(NOT WINE_NX_MESA_LIB_DIR AND NOT WINE_NX_STOCK_MESA AND EXISTS "${WINE_NX_MESA_DEFAULT_DIR}/libdrm_nouveau.a")
    set(WINE_NX_MESA_LIB_DIR "${WINE_NX_MESA_DEFAULT_DIR}")
endif()
message(STATUS "Mesa: ${WINE_NX_MESA_LIB_DIR}")
if(WINE_NX_MESA_LIB_DIR)
    set(SWITCH_EGL "${WINE_NX_MESA_LIB_DIR}/libEGL.a")
    set(SWITCH_GLAPI "${WINE_NX_MESA_LIB_DIR}/libglapi.a")
    # Its libdrm_nouveau maps application memory for GL_AMD_pinned_memory.
    set(SWITCH_DRM_NOUVEAU "${WINE_NX_MESA_LIB_DIR}/libdrm_nouveau.a")
endif()
# build-mesa-switch.sh builds danfromtico/mesa-switch: Mesa 26 with OpenGL through
# nvc0 and loaderless NVK Vulkan, both over its own Horizon backend, so without
# libdrm_nouveau. This links it in place of the Mesa above, in one archive group
# because EGL, GL, glapi and Vulkan share Mesa's utility libraries.
set(WINE_NX_MESA_SWITCH_DIR "" CACHE PATH "Folder with libEGL.a, libGL.a, libglapi.a and libvulkan.a from build-mesa-switch.sh")
if(WINE_NX_MESA_SWITCH_DIR)
    message(STATUS "Mesa: mesa-switch, ${WINE_NX_MESA_SWITCH_DIR}")
    foreach(lib EGL GL glapi vulkan mesa_util_c11 blake3 mesa_util mesa_util_simd xmlconfig)
        if(NOT EXISTS "${WINE_NX_MESA_SWITCH_DIR}/lib${lib}.a")
            message(FATAL_ERROR "mesa-switch SDK is missing lib${lib}.a")
        endif()
    endforeach()
    set(SWITCH_EGL -Wl,--start-group)
    foreach(lib EGL GL glapi vulkan mesa_util_c11 blake3 mesa_util mesa_util_simd xmlconfig)
        list(APPEND SWITCH_EGL "${WINE_NX_MESA_SWITCH_DIR}/lib${lib}.a")
    endforeach()
    find_library(SWITCH_ELF elf HINTS "$ENV{DEVKITPRO}/portlibs/switch/lib" REQUIRED)
    find_library(SWITCH_EXPAT expat HINTS "$ENV{DEVKITPRO}/portlibs/switch/lib" REQUIRED)
    list(APPEND SWITCH_EGL ${SWITCH_ELF} ${SWITCH_EXPAT} ${SWITCH_ZSTD} ${SWITCH_Z} -Wl,--end-group)
    set(SWITCH_GLAPI "")
    set(SWITCH_DRM_NOUVEAU "")
    target_include_directories(wine-nx-runtime BEFORE PRIVATE "${WINE_NX_MESA_SWITCH_DIR}/../include")
    target_link_options(wine-nx-runtime PRIVATE
        -Wl,-u,vk_icdGetInstanceProcAddr -Wl,-u,vk_icdNegotiateLoaderICDInterfaceVersion -pthread)
    target_compile_definitions(wine-nx-runtime PRIVATE WINE_NX_MESA_SWITCH)
    target_sources(wine-nx-runtime PRIVATE source/vulkan_probe.c)
    set_source_files_properties(source/vulkan_probe.c PROPERTIES
        INCLUDE_DIRECTORIES "${WINE_NX_MESA_SWITCH_DIR}/../include")

    # Vulkan for Windows programs: win32u's Vulkan over NVK, the display driver's
    # VK_NN_vi_surface (dlls/win32u/winnx_vulkan.c), and winevulkan's unix side
    # with its tables renamed like opengl32's (source/winevulkan_unix.c).
    target_sources(wine-win32u-real PRIVATE ../dlls/win32u/winnx_vulkan.c)
    target_compile_definitions(wine-win32u-real PRIVATE WINE_NX_MESA_SWITCH)
    target_compile_definitions(wine-virtual-real PRIVATE WINE_NX_MESA_SWITCH)
    add_library(wine-winevulkan-unix-real OBJECT
        ../dlls/winevulkan/vulkan.c
        ../dlls/winevulkan/vulkan_thunks.c
        source/winevulkan_unix.c
    )
    target_compile_definitions(wine-winevulkan-unix-real PRIVATE
        ${WINE_SWITCH_NTDLL_DEFS}
        __wine_unix_call_funcs=wine_nx_winevulkan_unix_funcs
        __wine_unix_call_wow64_funcs=wine_nx_winevulkan_wow64_unix_funcs
    )
    target_compile_options(wine-winevulkan-unix-real PRIVATE
        -std=gnu11
        -Werror=implicit-function-declaration
        -ffunction-sections
        -fdata-sections
    )
    target_include_directories(wine-winevulkan-unix-real PRIVATE
        ${WINE_SWITCH_WIN32U_INCLUDES}
        "${CMAKE_CURRENT_SOURCE_DIR}/../dlls/winevulkan"
    )
    target_sources(wine-nx-runtime PRIVATE $<TARGET_OBJECTS:wine-winevulkan-unix-real>)
    option(WINE_NX_LSFG "Enable the GPL LSFG-VK frame-generation backend" ON)
    if(WINE_NX_LSFG)
        include(cmake/LsfgVk.cmake)
    endif()
endif()
# The launcher (source/launcher.c) draws with SDL2 and SDL2_ttf; SDL's GL
# functions come through eglGetProcAddress, so it works over either Mesa.
find_library(SWITCH_SDL2 SDL2 HINTS "$ENV{DEVKITPRO}/portlibs/switch/lib")
find_library(SWITCH_SDL2_TTF SDL2_ttf HINTS "$ENV{DEVKITPRO}/portlibs/switch/lib")
set_source_files_properties(source/launcher.c source/launcher_catalog.c source/launcher_graphics.c source/launcher_ui.c source/launcher_audio.c source/launcher_forwarder.c source/launcher_image.c source/launcher_update.c source/launcher_setup.c source/launcher_swap.c source/steamgriddb.c source/dxvk_releases.c source/osk.c PROPERTIES
    INCLUDE_DIRECTORIES "$ENV{DEVKITPRO}/portlibs/switch/include/SDL2"
    COMPILE_DEFINITIONS _REENTRANT)
set_property(SOURCE source/launcher_update.c APPEND PROPERTY COMPILE_DEFINITIONS WINE_NX_BUILD_VERSION)
# The mark in the corner is only there in the build that generates it.
set_property(SOURCE source/launcher.c APPEND PROPERTY COMPILE_DEFINITIONS WINE_NX_HAS_EMBEDDED_LOGO)
target_link_libraries(wine-nx-runtime PRIVATE
    ${SWITCH_SDL2_TTF} ${SWITCH_SDL2}
    ${SWITCH_CURL} ${SWITCH_MBEDTLS} ${SWITCH_MBEDX509} ${SWITCH_MBEDCRYPTO} ${SWITCH_MINIZIP}
    ${SWITCH_FREETYPE} ${SWITCH_PNG} ${SWITCH_TURBOJPEG} ${SWITCH_BZ2} ${SWITCH_Z} ${SWITCH_ZSTD} ${SWITCH_HARFBUZZ}
    ${SWITCH_EGL} ${SWITCH_GLAPI} ${SWITCH_DRM_NOUVEAU} stdc++ nx m)

find_program(NACPTOOL nacptool HINTS "$ENV{DEVKITPRO}/tools/bin" "/opt/devkitpro/tools/bin")
find_program(ELF2NRO elf2nro HINTS "$ENV{DEVKITPRO}/tools/bin" "/opt/devkitpro/tools/bin")

if(NACPTOOL AND ELF2NRO)
    set(NACP_FILE "${CMAKE_CURRENT_BINARY_DIR}/wine-nx-probe.nacp")
    set(NRO_FILE "${CMAKE_CURRENT_BINARY_DIR}/wine-nx-probe.nro")
    set(PE_SMOKE_NACP_FILE "${CMAKE_CURRENT_BINARY_DIR}/wine-nx-pe-smoke.nacp")
    set(PE_SMOKE_NRO_FILE "${CMAKE_CURRENT_BINARY_DIR}/wine-nx-pe-smoke.nro")
    set(PE_REAL_REPORT_NACP_FILE "${CMAKE_CURRENT_BINARY_DIR}/wine-nx-pe-real-report.nacp")
    set(PE_REAL_REPORT_NRO_FILE "${CMAKE_CURRENT_BINARY_DIR}/wine-nx-pe-real-report.nro")
    set(PE_REAL_RUN_NACP_FILE "${CMAKE_CURRENT_BINARY_DIR}/wine-nx-pe-real-run.nacp")
    set(PE_REAL_RUN_NRO_FILE "${CMAKE_CURRENT_BINARY_DIR}/wine-nx-pe-real-run.nro")
    set(PE_NTDLL_SMOKE_NACP_FILE "${CMAKE_CURRENT_BINARY_DIR}/wine-nx-pe-ntdll-smoke.nacp")
    set(PE_NTDLL_SMOKE_NRO_FILE "${CMAKE_CURRENT_BINARY_DIR}/wine-nx-pe-ntdll-smoke.nro")
    set(NTDLL_SMOKE_NACP_FILE "${CMAKE_CURRENT_BINARY_DIR}/wine-nx-ntdll-smoke.nacp")
    set(NTDLL_SMOKE_NRO_FILE "${CMAKE_CURRENT_BINARY_DIR}/wine-nx-ntdll-smoke.nro")
    set(NTDLL_FILE_SMOKE_NACP_FILE "${CMAKE_CURRENT_BINARY_DIR}/wine-nx-ntdll-file-smoke.nacp")
    set(NTDLL_FILE_SMOKE_NRO_FILE "${CMAKE_CURRENT_BINARY_DIR}/wine-nx-ntdll-file-smoke.nro")
    set(RUNTIME_NACP_FILE "${CMAKE_CURRENT_BINARY_DIR}/wine-nx-runtime.nacp")
    set(RUNTIME_NRO_FILE "${CMAKE_CURRENT_BINARY_DIR}/wine-nx-runtime.nro")

    add_custom_command(
        OUTPUT "${NACP_FILE}"
        COMMAND "${NACPTOOL}" --create "Wine-NX Probe" "ticoverse.com" "0.1.0" "${NACP_FILE}"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${NRO_FILE}"
        COMMAND "${ELF2NRO}" "$<TARGET_FILE:wine-nx-probe>" "${NRO_FILE}" "--nacp=${NACP_FILE}"
        DEPENDS wine-nx-probe "${NACP_FILE}"
        VERBATIM
    )

    add_custom_target(wine-nx-probe-nro ALL DEPENDS "${NRO_FILE}")

    add_custom_command(
        OUTPUT "${PE_SMOKE_NACP_FILE}"
        COMMAND "${NACPTOOL}" --create "Wine-NX PE Smoke" "ticoverse.com" "0.1.0" "${PE_SMOKE_NACP_FILE}"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${PE_SMOKE_NRO_FILE}"
        COMMAND "${ELF2NRO}" "$<TARGET_FILE:wine-nx-pe-smoke>" "${PE_SMOKE_NRO_FILE}" "--nacp=${PE_SMOKE_NACP_FILE}"
        DEPENDS wine-nx-pe-smoke "${PE_SMOKE_NACP_FILE}"
        VERBATIM
    )

    add_custom_target(wine-nx-pe-smoke-nro ALL DEPENDS "${PE_SMOKE_NRO_FILE}")

    add_custom_command(
        OUTPUT "${PE_REAL_REPORT_NACP_FILE}"
        COMMAND "${NACPTOOL}" --create "Wine-NX PE Real Report" "ticoverse.com" "0.1.0" "${PE_REAL_REPORT_NACP_FILE}"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${PE_REAL_REPORT_NRO_FILE}"
        COMMAND "${ELF2NRO}" "$<TARGET_FILE:wine-nx-pe-real-report>" "${PE_REAL_REPORT_NRO_FILE}" "--nacp=${PE_REAL_REPORT_NACP_FILE}"
        DEPENDS wine-nx-pe-real-report "${PE_REAL_REPORT_NACP_FILE}"
        VERBATIM
    )

    add_custom_target(wine-nx-pe-real-report-nro ALL DEPENDS "${PE_REAL_REPORT_NRO_FILE}")

    add_custom_command(
        OUTPUT "${PE_REAL_RUN_NACP_FILE}"
        COMMAND "${NACPTOOL}" --create "Wine-NX PE Real Run" "ticoverse.com" "0.1.0" "${PE_REAL_RUN_NACP_FILE}"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${PE_REAL_RUN_NRO_FILE}"
        COMMAND "${ELF2NRO}" "$<TARGET_FILE:wine-nx-pe-real-run>" "${PE_REAL_RUN_NRO_FILE}" "--nacp=${PE_REAL_RUN_NACP_FILE}"
        DEPENDS wine-nx-pe-real-run "${PE_REAL_RUN_NACP_FILE}"
        VERBATIM
    )

    add_custom_target(wine-nx-pe-real-run-nro ALL DEPENDS "${PE_REAL_RUN_NRO_FILE}")

    add_custom_command(
        OUTPUT "${PE_NTDLL_SMOKE_NACP_FILE}"
        COMMAND "${NACPTOOL}" --create "Wine-NX PE NTDLL Smoke" "ticoverse.com" "0.1.0" "${PE_NTDLL_SMOKE_NACP_FILE}"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${PE_NTDLL_SMOKE_NRO_FILE}"
        COMMAND "${ELF2NRO}" "$<TARGET_FILE:wine-nx-pe-ntdll-smoke>" "${PE_NTDLL_SMOKE_NRO_FILE}" "--nacp=${PE_NTDLL_SMOKE_NACP_FILE}"
        DEPENDS wine-nx-pe-ntdll-smoke "${PE_NTDLL_SMOKE_NACP_FILE}"
        VERBATIM
    )

    add_custom_target(wine-nx-pe-ntdll-smoke-nro ALL DEPENDS "${PE_NTDLL_SMOKE_NRO_FILE}")

    add_custom_command(
        OUTPUT "${NTDLL_SMOKE_NACP_FILE}"
        COMMAND "${NACPTOOL}" --create "Wine-NX NTDLL Smoke" "ticoverse.com" "0.1.0" "${NTDLL_SMOKE_NACP_FILE}"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${NTDLL_SMOKE_NRO_FILE}"
        COMMAND "${ELF2NRO}" "$<TARGET_FILE:wine-nx-ntdll-smoke>" "${NTDLL_SMOKE_NRO_FILE}" "--nacp=${NTDLL_SMOKE_NACP_FILE}"
        DEPENDS wine-nx-ntdll-smoke "${NTDLL_SMOKE_NACP_FILE}"
        VERBATIM
    )

    add_custom_target(wine-nx-ntdll-smoke-nro ALL DEPENDS "${NTDLL_SMOKE_NRO_FILE}")

    add_custom_command(
        OUTPUT "${NTDLL_FILE_SMOKE_NACP_FILE}"
        COMMAND "${NACPTOOL}" --create "Wine-NX NTDLL File Smoke" "ticoverse.com" "0.1.0" "${NTDLL_FILE_SMOKE_NACP_FILE}"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${NTDLL_FILE_SMOKE_NRO_FILE}"
        COMMAND "${ELF2NRO}" "$<TARGET_FILE:wine-nx-ntdll-file-smoke>" "${NTDLL_FILE_SMOKE_NRO_FILE}" "--nacp=${NTDLL_FILE_SMOKE_NACP_FILE}"
        DEPENDS wine-nx-ntdll-file-smoke "${NTDLL_FILE_SMOKE_NACP_FILE}"
        VERBATIM
    )

    add_custom_target(wine-nx-ntdll-file-smoke-nro ALL DEPENDS "${NTDLL_FILE_SMOKE_NRO_FILE}")

    add_custom_command(
        OUTPUT "${RUNTIME_NACP_FILE}"
        COMMAND "${NACPTOOL}" --create "Autorun" "ticoverse.com" "0.0.1" "${RUNTIME_NACP_FILE}"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${RUNTIME_NRO_FILE}"
        COMMAND "${ELF2NRO}" "$<TARGET_FILE:wine-nx-runtime>" "${RUNTIME_NRO_FILE}" "--nacp=${RUNTIME_NACP_FILE}"
                "--icon=${CMAKE_CURRENT_SOURCE_DIR}/assets/autorun.jpg"
        DEPENDS wine-nx-runtime "${RUNTIME_NACP_FILE}" "${CMAKE_CURRENT_SOURCE_DIR}/assets/autorun.jpg"
        VERBATIM
    )

    add_custom_target(wine-nx-runtime-nro ALL DEPENDS "${RUNTIME_NRO_FILE}")
endif()
