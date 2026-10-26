# pico-speccy host build (Linux / Windows / macOS) on SDL2 — stage 1 of
# docs: "pico-speccy на ПК и Android: план". Builds the emulator sources from
# ../src against host stand-ins for the Pico SDK (host/include) and host
# implementations of the driver APIs (host/*.cpp). The firmware build is the
# root CMakeLists.txt and is untouched by this file.
cmake_minimum_required(VERSION 3.16)
project(pico-speccy-host C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(WIN32)
    # The SDL2 mingw development package (SDL2_DIR = its lib/cmake/SDL2).
    find_package(SDL2 REQUIRED CONFIG)
    set(SDL2_LIBRARIES SDL2::SDL2main SDL2::SDL2)
    set(SDL2_INCLUDE_DIRS "")
    enable_language(RC)
else()
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 REQUIRED sdl2)
    else()
        find_package(SDL2 REQUIRED)
    endif()
endif()

file(GLOB_RECURSE EMU_SRC CONFIGURE_DEPENDS
    ${ROOT}/src/*.c ${ROOT}/src/*.cpp)
# Firmware entry point and hardware drivers are replaced by host/*.cpp.
list(FILTER EMU_SRC EXCLUDE REGEX "/src/main\\.cpp$")
list(FILTER EMU_SRC EXCLUDE REGEX "/src/drivers/")
# Pure pieces of src/drivers that run unchanged on the host.
list(APPEND EMU_SRC
    ${ROOT}/src/drivers/input/keyboard.cpp
    ${ROOT}/src/drivers/input/codepages.cpp
    ${ROOT}/src/drivers/framebuffer/Font.cpp
    ${ROOT}/src/drivers/board/BoardPins.cpp
    ${ROOT}/src/drivers/framebuffer/VGA.cpp
    ${ROOT}/src/drivers/framebuffer/I2S.cpp
    ${ROOT}/src/drivers/graphics/graphics.c)

# The on-chip radio's lwIP/CYW43 halves; host/host_net.cpp puts the PC's network
# behind the same interfaces (WifiNet, WifiSock, AtEmu).
list(FILTER EMU_SRC EXCLUDE REGEX "/src/net/(WifiNet|WifiSock|AtEmu)\\.cpp$")
list(FILTER EMU_SRC EXCLUDE REGEX "/src/net/cyw43_[^/]*\\.c$")

# Firmware-only: libc/libstdc++ link-time shims (malloc wrap, terminate handler).
list(FILTER EMU_SRC EXCLUDE REGEX "/src/app/cxx_shims\\.cpp$")

# Vendored code, the same set the firmware compiles (root CMakeLists.txt).
file(GLOB_RECURSE EXT_SRC CONFIGURE_DEPENDS
    ${ROOT}/external/miniz/*.c
    ${ROOT}/external/redcode/*.c
    ${ROOT}/external/tjpgd/*.c
    ${ROOT}/external/picomp3lib/*.c)
file(GLOB XMP_SRC CONFIGURE_DEPENDS ${ROOT}/external/libxmp/*.c ${ROOT}/external/libxmp/loaders/*.c)
set_source_files_properties(${XMP_SRC} PROPERTIES COMPILE_FLAGS
    "-DLIBXMP_CORE_PLAYER -DLIBXMP_STATIC -I${ROOT}/external/libxmp -include ${ROOT}/src/player/pp_xmp_alloc.h")
# mbedTLS for HTTPS (TlsSock) and SSH: the Pico SDK's vendored copy with the
# firmware's own config, so the host network client is the board's. Without it the
# client (catalog over HTTPS, FTP, SSH) is left out and only the guest NIC works.
set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "Pico SDK (for its mbedTLS)")
if(NOT PICO_SDK_PATH)
    file(GLOB _sdks "$ENV{HOME}/.pico-sdk/sdk/*")
    if(_sdks)
        list(SORT _sdks)
        list(REVERSE _sdks)
        list(GET _sdks 0 _sdk0)
        set(PICO_SDK_PATH "${_sdk0}")
    endif()
endif()
set(MBEDTLS_DIR "${PICO_SDK_PATH}/lib/mbedtls")
if(PICO_SDK_PATH AND NOT EXISTS "${MBEDTLS_DIR}/library/ssl_tls.c")
    message(STATUS "host: ${PICO_SDK_PATH} has no lib/mbedtls")
endif()
if(EXISTS "${MBEDTLS_DIR}/library/ssl_tls.c")
    file(GLOB MBEDTLS_SRC "${MBEDTLS_DIR}/library/*.c")
    set(HOST_NET_CLIENT 1)
    message(STATUS "host: network client on (mbedTLS from ${MBEDTLS_DIR})")
else()
    set(MBEDTLS_SRC "")
    set(HOST_NET_CLIENT 0)
    message(STATUS "host: no Pico SDK mbedTLS found - HTTPS/FTP/SSH left out (set PICO_SDK_PATH)")
endif()
list(APPEND EXT_SRC ${MBEDTLS_SRC})

# No external/fatfs/ff.c: host/host_fatfs.cpp implements the FatFs API over a host
# directory. ff.h is still the interface (types, constants).
list(APPEND EXT_SRC ${XMP_SRC})

# Per-file flags the firmware sets in cmake/source-flags.cmake that change meaning.
set_source_files_properties(${ROOT}/external/redcode/Z80_redcode.c PROPERTIES
    COMPILE_FLAGS "-DZ80_STATIC -DZ80_EXTERNAL_HEADER=\\\"Z80_compat.h\\\"")
set_source_files_properties(${ROOT}/src/speccy/devices/gs/GS.cpp PROPERTIES
    COMPILE_FLAGS "-DZ80_STATIC -DZ80_EXTERNAL_HEADER=\\\"Z80_compat.h\\\"")

file(GLOB HOST_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(WIN32)
    list(APPEND HOST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/win/pico-speccy.rc)
endif()

# The USB-keyboard mapping (map_key / kbdExtraMapping / process_kbd_report) is taken
# from src/main.cpp as it stands, so the host and the firmware share one key map.
file(STRINGS ${ROOT}/external/tinyusb/src/class/hid/hid.h HID_KEY_LINES REGEX "^#define HID_KEY_")
string(REPLACE ";" "\n" HID_KEY_TEXT "${HID_KEY_LINES}")
file(WRITE ${CMAKE_BINARY_DIR}/gen/host_hid_keys.h "#pragma once\n${HID_KEY_TEXT}\n")
file(READ ${ROOT}/src/main.cpp MAIN_CPP)
string(FIND "${MAIN_CPP}" "inline static bool isInReport" KBD_BEGIN)
string(FIND "${MAIN_CPP}" "Ps2Kbd_Mrmltr ps2kbd(" KBD_END)
if(KBD_BEGIN LESS 0 OR KBD_END LESS KBD_BEGIN)
    message(FATAL_ERROR "host: cannot find the keyboard mapping in src/main.cpp")
endif()
math(EXPR KBD_LEN "${KBD_END} - ${KBD_BEGIN}")
string(SUBSTRING "${MAIN_CPP}" ${KBD_BEGIN} ${KBD_LEN} KBD_TEXT)
file(WRITE ${CMAKE_BINARY_DIR}/gen/host_kbd_gen.cpp.in
    "// Generated from src/main.cpp by host/CMakeLists.txt - do not edit.\n#include \"host_kbd_prelude.h\"\n${KBD_TEXT}")
configure_file(${CMAKE_BINARY_DIR}/gen/host_kbd_gen.cpp.in ${CMAKE_BINARY_DIR}/gen/host_kbd_gen.cpp COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ROOT}/src/main.cpp)
list(APPEND HOST_SRC ${CMAKE_BINARY_DIR}/gen/host_kbd_gen.cpp)

add_executable(pico-speccy ${EMU_SRC} ${EXT_SRC} ${HOST_SRC})

target_include_directories(pico-speccy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/gen
    ${ROOT}/src
    ${ROOT}/external
    ${ROOT}/src/drivers/board
    ${ROOT}/external/embeded-midi-synth
    ${ROOT}/external/fatfs
    ${ROOT}/external/picomp3lib
    ${ROOT}/external/redcode
    ${ROOT}/src/drivers/graphics
    ${ROOT}/src/drivers/sdcard
    ${ROOT}/src/drivers/audio
    ${ROOT}/src/drivers/psram
    ${ROOT}/src/drivers/hdmi
    ${ROOT}/src/drivers/vga-nextgen
    ${ROOT}/src/drivers/ps2kbd
    ${ROOT}/src/drivers/ps2
    ${ROOT}/src/drivers/nespad
    ${ROOT}/src/drivers/st7789
    ${ROOT}/src/drivers/framebuffer
    ${ROOT}/src/drivers/input
    ${SDL2_INCLUDE_DIRS})

if(WIN32)
    # 64-bit off_t / stat sizes (disk images past 2 GB); the heap allocator's source.
    target_compile_definitions(pico-speccy PRIVATE _FILE_OFFSET_BITS=64 LIBXMP_STATIC)
    target_include_directories(pico-speccy PRIVATE ${ROOT}/external)
endif()
target_compile_definitions(pico-speccy PRIVATE ZIFI_NET_CLIENT=${HOST_NET_CLIENT}
    "MBEDTLS_CONFIG_FILE=\"${ROOT}/src/drivers/board/mbedtls_config_picospeccy.h\"")
# GS per-second perf counters ([GS PERF] lines), as on the board: -DGS_PERF_TRACE=ON.
option(GS_PERF_TRACE "General Sound per-second perf counters" OFF)
if(GS_PERF_TRACE)
    target_compile_definitions(pico-speccy PRIVATE GS_PERF_TRACE=1)
endif()
if(HOST_NET_CLIENT)
    target_include_directories(pico-speccy PRIVATE ${MBEDTLS_DIR}/include)
endif()

target_compile_options(pico-speccy PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:
    -include ${CMAKE_CURRENT_SOURCE_DIR}/host_config.h
    -w -fno-strict-aliasing -fwrapv>)
# The emulator tells ROM from RAM by address: below 0x11000000 is flash (a write
# there is dropped). Put code + .rodata (the ROM images) at 0x10000000, where the
# RP2350 maps its flash, and .data/.bss/heap at 0x20000000 like its SRAM.
set_target_properties(pico-speccy PROPERTIES POSITION_INDEPENDENT_CODE OFF)
if(WIN32)
    # PE: image base 0x10000000 without ASLR, sections laid out by win/pico-speccy.ld
    # (code + .rdata, then a gap holding the PSRAM window, then .data/.bss at
    # 0x20000000); the heap is host_winheap.c's region at 0x30000000. The mingw
    # runtime is linked in, so the .exe needs only SDL2.dll beside it.
    target_link_libraries(pico-speccy PRIVATE ${SDL2_LIBRARIES} ws2_32 iphlpapi winmm ole32 uuid)
    target_link_options(pico-speccy PRIVATE
        -Wl,--image-base=0x10000000 -Wl,--disable-dynamicbase -Wl,--disable-high-entropy-va
        -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/win/pico-speccy.ld
        -static-libgcc -static-libstdc++ -Wl,-Bstatic,--whole-archive -lwinpthread
        -Wl,--no-whole-archive,-Bdynamic
        $<$<CONFIG:Release,MinSizeRel>:-s> -mconsole)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/win/pico-speccy.rc PROPERTIES
        OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/win/pico-speccy.manifest)
    # SDL2.dll next to the .exe, so the build directory runs as it is.
    add_custom_command(TARGET pico-speccy POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:pico-speccy>)
else()
    target_link_libraries(pico-speccy PRIVATE ${SDL2_LIBRARIES} m pthread)
    target_compile_options(pico-speccy PRIVATE -fno-pie)
    target_link_options(pico-speccy PRIVATE -no-pie
        -Wl,-Ttext-segment=0x10000000 -Wl,-Tdata=0x20000000)
endif()
target_link_directories(pico-speccy PRIVATE ${SDL2_LIBRARY_DIRS})
