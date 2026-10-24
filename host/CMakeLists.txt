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

find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 REQUIRED sdl2)
else()
    find_package(SDL2 REQUIRED)
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
list(APPEND EXT_SRC ${XMP_SRC}
    ${ROOT}/external/fatfs/ff.c
    ${ROOT}/external/fatfs/ffsystem.c
    ${ROOT}/external/fatfs/ffunicode.c)

# Per-file flags the firmware sets in cmake/source-flags.cmake that change meaning.
set_source_files_properties(${ROOT}/external/redcode/Z80_redcode.c PROPERTIES
    COMPILE_FLAGS "-DZ80_STATIC -DZ80_EXTERNAL_HEADER=\\\"Z80_compat.h\\\"")
set_source_files_properties(${ROOT}/src/speccy/devices/gs/GS.cpp PROPERTIES
    COMPILE_FLAGS "-DZ80_STATIC -DZ80_EXTERNAL_HEADER=\\\"Z80_compat.h\\\"")

file(GLOB HOST_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

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

target_compile_options(pico-speccy PRIVATE
    -include ${CMAKE_CURRENT_SOURCE_DIR}/host_config.h
    -w -fno-strict-aliasing -fwrapv)
target_link_libraries(pico-speccy PRIVATE ${SDL2_LIBRARIES} m pthread)

# The emulator tells ROM from RAM by address: below 0x11000000 is flash (a write
# there is dropped). Put code + .rodata (the ROM images) at 0x10000000, where the
# RP2350 maps its flash, and .data/.bss/heap at 0x20000000 like its SRAM.
set_target_properties(pico-speccy PROPERTIES POSITION_INDEPENDENT_CODE OFF)
target_compile_options(pico-speccy PRIVATE -fno-pie)
target_link_options(pico-speccy PRIVATE -no-pie
    -Wl,-Ttext-segment=0x10000000 -Wl,-Tdata=0x20000000)
target_link_directories(pico-speccy PRIVATE ${SDL2_LIBRARY_DIRS})
