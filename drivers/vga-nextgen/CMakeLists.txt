add_library(vga-nextgen INTERFACE)

target_sources(vga-nextgen INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/vga.c
        ${CMAKE_CURRENT_LIST_DIR}/vga_hstx.c
)

target_link_libraries(vga-nextgen INTERFACE hardware_pio hardware_dma hardware_clocks)

target_include_directories(vga-nextgen INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
)
