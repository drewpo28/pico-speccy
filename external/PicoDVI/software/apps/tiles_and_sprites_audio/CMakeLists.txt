add_executable(tiles_and_sprites_audio
	main.c
)

target_compile_options(tiles_and_sprites_audio PRIVATE -Wall)

target_compile_definitions(tiles_and_sprites_audio PRIVATE
	DVI_DEFAULT_SERIAL_CONFIG=${DVI_DEFAULT_SERIAL_CONFIG}
	DVI_N_TMDS_BUFFERS=5
	)

target_link_libraries(tiles_and_sprites_audio
	pico_stdlib
	pico_multicore
	pico_util
	libdvi
	libsprite
)

# create map/bin/hex file etc.
pico_add_extra_outputs(tiles_and_sprites_audio)
