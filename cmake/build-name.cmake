# Firmware file name (BUILD_NAME), the short OSD name and the git stamp.

# The PIO-USB host is a build-time variant of the same board, not a display mode, so
# it tags the name after the display part: z0p2-speccy-VGA-HDMI-PIOUSB-<version>.
SET(VARIANT_TAG "")
IF(ZERO2 AND ZERO2_PIO_USB)
    SET(BUILD_NAME "${BUILD_NAME}-PIOUSB")
    SET(VARIANT_TAG "+PIOUSB")
    SET(DISPLAY_TAG "${DISPLAY_TAG}${VARIANT_TAG}")
ENDIF()
# Same shape for the HSTX back-end: PCp2-speccy-VGA-HDMI-HSTX-<version> for the
# expander form ("PCp2 / HDMI+HSTX [v...]" in Hardware Info), -HSTX-RAW / +HSTXRAW
# for the raw serializer, so the two can be told apart on screen and on disk.
IF(HDMI_HSTX_ON EQUAL 2)
    SET(BUILD_NAME "${BUILD_NAME}-HSTX")
    SET(VARIANT_TAG "+HSTX")
    SET(DISPLAY_TAG "${DISPLAY_TAG}${VARIANT_TAG}")
ELSEIF(HDMI_HSTX_ON)
    SET(BUILD_NAME "${BUILD_NAME}-HSTX-RAW")
    SET(VARIANT_TAG "+HSTXRAW")
    SET(DISPLAY_TAG "${DISPLAY_TAG}${VARIANT_TAG}")
ENDIF()

SET(BUILD_NAME "${BUILD_NAME}-${PORT_VERSION}")

# Compact form for the OSD's Hardware Info row — z0p2+HDMI+1.0.2 — where the full
# file name beside an aligned label overruns the narrowest page nm:: supports (40
# cols). Built from the same pieces rather than parsed back out of BUILD_NAME, so the
# two cannot drift apart. The file name itself is unchanged — release assets use it.
SET(BUILD_NAME_SHORT "${BOARD_TAG}+${DISPLAY_TAG}+${PORT_VERSION}")

# ALF is a standard runtime machine (not a separate build), so no "-ALF" suffix.

message(STATUS "BUILD_NAME: ${BUILD_NAME}")

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${BUILD_NAME}")

execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    PICO_BUILD_NAME="${BUILD_NAME}"
    PICO_BUILD_NAME_SHORT="${BUILD_NAME_SHORT}"
    PICO_BUILD_VARIANT_TAG="${VARIANT_TAG}"
    PICO_GIT_BRANCH="${GIT_BRANCH}"
    PICO_GIT_COMMIT="${GIT_COMMIT_HASH}"
)
