add_library(ps2kbd INTERFACE)

target_sources(ps2kbd INTERFACE
	${CMAKE_CURRENT_LIST_DIR}/ps2kbd_mrmltr.cpp
	${CMAKE_CURRENT_LIST_DIR}/ps2kbd_mrmltr.h
)

target_link_libraries(ps2kbd INTERFACE hardware_pio hardware_clocks)

target_include_directories(ps2kbd INTERFACE
  ${CMAKE_CURRENT_LIST_DIR}
)


# One program for every board pin: the driver patches the `wait gpio` index at
# load time (see ps2kbd_program_for), so no per-pin .pio copies are needed.
pico_generate_pio_header(ps2kbd
${CMAKE_CURRENT_LIST_DIR}/ps2kbd_mrmltr.pio
)
