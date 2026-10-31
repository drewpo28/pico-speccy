# RAR archives, like ZIP — plan (deferred, nothing implemented)

Status 2026-11-01: analysis only, no code in the tree. The reason to want it:
https://www.ep128.hu/ (Enterprise software) publishes everything as `.rar`.
All sizes below were measured on a PC / by cross-compiling, nothing has run on hardware.

## What the site carries

546 RAR archives: games 155 (`Ep_Games/Prg/`), demos 180, utilities 197, music 14.
Sample: 60 random archives, 11 MB.

| Property | Result |
|---|---|
| Format | RAR4 59, RAR5 1 |
| Solid, encrypted | none |
| Method | store and `-m3` only; RAR3 streams are LZ, no PPMd seen |
| RarVM filters | needed by 8 of 60 archives (13%) — cannot be dropped |
| Unpacked file | median 5 KB, max 696 KB |
| Files per archive | median 4, max 609 |
| Dictionary flag | 128 KB nearly always, 1 MB on 63 of 1674 files |

Zero PPMd in 60 means at most ~5% of the site; such an archive is refused with a message.

## Decoder

`dmc_unrar` (github.com/DrMcCoy/dmc_unrar): one C file, GPLv2+, compatible with our
GPLv3. RARLAB's unrar is not usable, its licence is not GPL-compatible. Extracted all 60
archives (1676 files, RAR5 included), CRC checked. Allocator callbacks and an extract
callback exist, so it can stream to FatFs and take memory from `Buffer::palloc`.

Code size (`-Os`, Cortex-M33, `-DDMC_UNRAR_DISABLE_STDIO=1 -DDMC_UNRAR_32BIT=1`):

| Variant | .text |
|---|---|
| Full | 44.3 KB |
| `DMC_UNRAR_DISABLE_PPMD=1` (chosen) | 39.0 KB |
| + `DMC_UNRAR_DISABLE_FILTERS=1` | 32.4 KB, loses 13% of the archives |

Peak heap, stock: 7.3 MB. Two local patches, all CRCs still good:

1. `dmc_unrar_rar4_get_dict_size()`, versions 29/36: instead of a fixed 4 MB window, the
   smallest power of two >= the file's unpacked size, from 64 KB, capped at 4 MB. A match
   cannot reach further back than the file start, so this is safe for NON-SOLID files
   only; a solid archive must keep the full window (or be refused).
2. `dmc_unrar_huff_create_tree*()`: `tree_size` = `code_count * 40 + 64` instead of
   `(1 << (max_length + 1)) * 2`, which was five blocks of 512 KB.

Result: peak **median 425 KB, p90 1.14 MB, max 1.42 MB** (262 KB of it is the filter VM
memory). `DMC_UNRAR_HUFF_MAX_TABLE_DEPTH` changes almost nothing, leave it at 10.

Consequences: PSRAM boards only, the window and tables are read per byte, so expect the
decode to be slower than inflate. Static SRAM ~64 B.

## Flash

**16 MB images only**, by the project rule (what does not fit 4 MB goes behind the 16 MB
guard, as `YRW801_BUILTIN` does): `RAR_SUPPORT` = on with `FLASH_16MB`. 39 KB is not
known to fit the 4 MB images: the only current figures are DVp2 at 53 KB of headroom to
the hard GM.DLS floor (build of 10-30) and 4 KB (11-01, with `NEXTREG_TRACE`). Other
4 MB ELFs predate Enterprise. A plain HEAD build answers it, if 4 MB is ever wanted.
z0p2 boards are 16 MB, so z0p2 does not constrain this.

## Firmware work (1-2 days)

- `src/fs/RarExtract.{h,cpp}` beside `ZipExtract`, sharing its temp files, owner release
  and the 8 KB `zipCallOnStack` stack. Vendor the decoder in `external/dmc_unrar/` with
  the two patches marked `PICO-SPEC PATCH`. Headers are sequential, there is no central
  directory, so the file list is one walk through the archive.
- FatFs reader callback; extract callback writing to SD, so the output is never in RAM.
- `.rar` in: F5 Enter + member picker, F1 Info, F4 extract, Web catalog launch
  (around 50 zip references in the tree, only part of them apply). Detect by the
  signature `Rar!\x1a\x07`, as `isZipByMagic` does.
- Refuse with a message: solid, encrypted, PPMd, no PSRAM.
- Check the decoder's stack use (name buffers of 512 entries, 4 KB bitstream cache
  inside the context) against the 8 KB stack.

Enterprise specifically: quick launch extracts one member, while a typical archive holds
several files. Programs with data files need the whole archive extracted into the
`FILE:` folder. The same applies to ZIP, it is a separate job from RAR.

## Alternative for the Web catalog

`pico-spec-catalog` has no Enterprise adapter yet; one is needed either way. Its Action
could repack RAR to ZIP and publish the result on Pages: no firmware change, works on
every board, roughly 100 MB (rough scale-up of the sample). It needs the site owner's
permission to mirror, and does nothing for a `.rar` on the SD card.

## Order, when this is picked up

1. Catalog adapter + RAR->ZIP repack (covers 4 MB boards).
2. Multi-file launch for Enterprise.
3. `RAR_SUPPORT` on 16 MB for `.rar` on the card; measure speed on DVp2w first.
