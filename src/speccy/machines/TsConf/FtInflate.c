// A private copy of miniz's inflater for the VDAC2 coprocessor's CMD_INFLATE.
//
// miniz.c's own tinfl_decompress lives in flash and is shared with the zip loader
// and the music player on every machine, so it cannot move into a window that is
// heap on most boots. This file compiles the INFLATE half of the same miniz.c a
// second time with every public name prefixed ft_, and rp2350-memmap.ld collects
// this object (code and its tables) BY OBJECT FILE into the .ftovl window — loaded
// only on a TS-Conf boot with VDAC2 on, i.e. exactly when Ft812 can call it.
// From flash it ran through the XIP cache that core1's texel stream thrashes
// (R-Type, hw 2026-10-01: 0.2-1.6 ms per KB of output, the bulk of a heavy frame).
//
// The object is also excluded from every general .text/.rodata rule of the
// linker script, so with the overlay compiled out it must not exist at all.
#if VDAC2_CODE_OVERLAY

#define MINIZ_NO_ZLIB_APIS
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME

// everything miniz.c still defines with those five off
#define miniz_def_alloc_func             ft_miniz_def_alloc_func
#define miniz_def_free_func              ft_miniz_def_free_func
#define miniz_def_realloc_func           ft_miniz_def_realloc_func
#define mz_adler32                       ft_mz_adler32
#define mz_crc32                         ft_mz_crc32
#define mz_free                          ft_mz_free
#define mz_version                       ft_mz_version
#define tinfl_decompress                 ft_tinfl_decompress
#define tinfl_decompress_mem_to_callback ft_tinfl_decompress_mem_to_callback
#define tinfl_decompress_mem_to_heap     ft_tinfl_decompress_mem_to_heap
#define tinfl_decompress_mem_to_mem      ft_tinfl_decompress_mem_to_mem
#define tinfl_decompressor_alloc         ft_tinfl_decompressor_alloc
#define tinfl_decompressor_free          ft_tinfl_decompressor_free

#include "miniz/miniz.c"

#endif
