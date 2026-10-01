# Display driver selection (TFT / TV / SOFTTV / VGA_HDMI) and the HDMI HSTX back-end.

# DISPLAY_TAG is the same choice in one word, for the compact name in cmake/build-name.cmake.
IF(TFT)
    target_link_libraries(${PROJECT_NAME} PRIVATE st7789)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TFT)
    IF(ILI9341)
        SET(BUILD_NAME "${BUILD_NAME}-ILI9341")
        SET(DISPLAY_TAG "ILI9341")
        target_compile_definitions(${PROJECT_NAME} PRIVATE ILI9341)
    ELSE()
        IF(TFT_INV)
            SET(BUILD_NAME "${BUILD_NAME}-ST7789V")
            SET(DISPLAY_TAG "ST7789V")
        ELSE()
            SET(BUILD_NAME "${BUILD_NAME}-ST7789")
            SET(DISPLAY_TAG "ST7789")
        ENDIF()
    ENDIF()
ELSEIF(TV)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TV)
    target_link_libraries(${PROJECT_NAME} PRIVATE tv)
    SET(BUILD_NAME "${BUILD_NAME}-TV")
    SET(DISPLAY_TAG "TV")
ELSEIF(SOFTTV)
	target_compile_definitions(${PROJECT_NAME} PRIVATE SOFTTV)
	target_link_libraries(${PROJECT_NAME} PRIVATE tv-software)
	SET(BUILD_NAME "${BUILD_NAME}-TV-SOFT")
	SET(DISPLAY_TAG "TVSOFT")
ELSE()
    target_link_libraries(${PROJECT_NAME} PRIVATE hdmi)
    target_link_libraries(${PROJECT_NAME} PRIVATE vga-nextgen)
    target_compile_definitions(${PROJECT_NAME} PRIVATE VGA_HDMI)
    SET(BUILD_NAME "${BUILD_NAME}-VGA-HDMI")
    # ONE image drives both outputs and the pick is made at boot (SELECT_VGA, from
    # linkVGA01 or Config::video_driver), so this tag is only the fallback: the OSD
    # substitutes the output that actually came up. Every other DISPLAY_TAG above is
    # a real build-time choice and needs no such treatment.
    SET(DISPLAY_TAG "VGAHDMI")

    # HSTX drives the same eight pins as the PIO TMDS program and only exists in
    # this branch — the TFT/TV/SOFTTV displays have no HDMI at all.
    # HDMI_HSTX_ON: 0 PIO, 1 raw serializer, 2 command expander + TMDS encoder.
    IF(HDMI_HSTX STREQUAL "AUTO")
        IF(PICO_PC OR MURM2)
            # TMDS is the intended back-end; RAW remains an explicit fallback.
            SET(HDMI_HSTX_ON 2)
        ELSE()
            SET(HDMI_HSTX_ON 0)
        ENDIF()
    ELSEIF(HDMI_HSTX STREQUAL "TMDS")
        SET(HDMI_HSTX_ON 2)
    ELSEIF(HDMI_HSTX STREQUAL "RAW" OR HDMI_HSTX)
        SET(HDMI_HSTX_ON 1)
    ELSE()
        SET(HDMI_HSTX_ON 0)
    ENDIF()
    IF(HDMI_HSTX_ON)
        IF(NOT (PICO_PC OR MURM2))
            message(FATAL_ERROR
                "HDMI_HSTX needs the display on GPIO 12-19 (HDMI_BASE_PIN=12): "
                "that is PICO_PC, MURM2 and MURM2_W only")
        ENDIF()
        target_compile_definitions(${PROJECT_NAME} PRIVATE HDMI_HSTX=${HDMI_HSTX_ON})
        # The VGA half of the same board moves with it.  It is not a second
        # decision: HSTX drives GPIO 12-19 and both outputs live there, so a
        # board that can serialize HDMI can serialize VGA, and the VGA pixel
        # clocks in video_mode_table.h are chosen to be 126 MHz / k for exactly
        # this.  What it buys is the COLOUR — four PWM sub-samples per pixel
        # instead of one 2-bit-per-channel value plus a Bayer block.
        target_compile_definitions(${PROJECT_NAME} PRIVATE VGA_HSTX=1)
        IF(HDMI_HSTX_TRACE)
            target_compile_definitions(${PROJECT_NAME} PRIVATE HDMI_HSTX_TRACE=1)
        ENDIF()
        # The name is tagged in cmake/build-name.cmake, beside PIOUSB's: a VGA_HDMI build's Hardware Info
        # line is built from CONFIG_BOARD_TAG + the LIVE output + VARIANT_TAG, not
        # from DISPLAY_TAG, so a variant that only touches DISPLAY_TAG is invisible
        # on screen — and telling the two images apart there is the whole point.
    ENDIF()
ENDIF()
