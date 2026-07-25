# Demonstrate pixel doubling in both dimensions - with audio

add_executable(moon_double_audio
	main.c
	tmds_double.S
)

target_compile_options(moon_double_audio PRIVATE -Wall)

target_compile_definitions(moon_double_audio PRIVATE
	DVI_DEFAULT_SERIAL_CONFIG=${DVI_DEFAULT_SERIAL_CONFIG}
	DVI_VERTICAL_REPEAT=2
	DVI_N_TMDS_BUFFERS=3
	DVI_MONOCHROME_TMDS
)

target_link_libraries(moon_double_audio
	pico_stdlib
	pico_multicore
	libdvi
)

pico_add_extra_outputs(moon_double_audio)
