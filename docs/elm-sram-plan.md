# eLeMeNt ZX: SRAM for HGFX (analysis 2026-11-18, nothing implemented)

Goal (owner 2026-11-18): free SRAM for HGFX by moving to PSRAM (or out of the way) whatever
does not cost the Z80 loop or the render.

Source: hw boot ledger `logs/devttyACM0_2026_11_18.16.10.09.403.txt` (DVp2w, eLeMeNt, NeoGS,
HDMI audio, FB 360x288) + `nm -S` / gdb `sizeof` on `build-all/DVp2w` (non-PERF, HEAD fe939abb).

## Where the eLeMeNt boot heap goes

| Step | Heap after | Taken | What |
|---|---|---|---|
| FB claimed (3 donors) | 128 488 | - | FB 103 680 B all in the `.ram_128k` strip |
| HGFX arena (early) | 49 072 | 48 896 | region 1 (the Next window), 5 of 6 row blocks |
| ZX pages 0-7 | 44 928 | 4 144 | page descriptors |
| GS::init (NeoGS) | 42 696 | 2 232 | |
| AUDIO begin -> ZiFi line | 35 312 | 7 280 | `Z80DMA::reset` -> `DmaAttrBuf` 7 272 B (dma=1 on eLeMeNt) |
| audio init | 33 976 | 1 336 | |
| `Subsystems::applyPending` | 20 720 | 13 256 | TSFM (2 x OpnFm 936 + tables 2 560 + buf 1 280), SAA 1 508, Covox 1 280, AY chip1 1 616, ~3 KB not attributed |
| `CPU::reset` -> `Elm::reset` | **6 808** | 13 912 | `Hgfx::Hot` 8 268, `ElmSdma::State` 2 928, `ElmSid::State` 1 472, `Elm::Regs` 1 216 |

6.8 KB free while a program runs: the lazy HGFX tables do not fit - HRXC `s_cnt2` (8 KB,
`pairLutUpdate`, tryCalloc fails -> `lutOn = false`), the sixth row block (8 KB), and
`Ham` (4.8 KB) only just.

## Candidates, ranked

1. **The idle tail of `pages57` - 14 824 B of SRAM nobody uses (0 perf cost).** `.elmovl`
   takes 12 544 of the 32 KB, the FB's last donor takes 15 rows (5 400 B at 360 wide, 0 at
   320x240), the rest is not in any heap region. Put the four `Elm::reset` structures
   there (`Hot` + `Sdma` + `Sid` + `Regs` = 13 884 B, all SRAM-speed as today):
   heap +13.9 KB. Needs `VIDEO::fbDonorSpare(0xA0)` (pointer + bytes left after the FB
   took its rows) and a bump allocator with tryMalloc fallback; the tail exists for the
   whole session (`s_elmPages57` is decided once per boot).
2. **`DmaAttrBuf` split (+6.9 KB, render untouched).** `valid[192]` + flags (~300 B) are read
   per scanline and stay in SRAM; `shadow[6144]` is read only on lines marked valid (DMA
   multicolour, rare) and `prev_attrs[768]` only on a DMA into #5800-#5AFF ->
   `Buffer::palloc(PREFER_PSRAM)`. Costs only a DMA-multicolour title, ~0.15 ms worst case;
   can be eLeMeNt-only if the owner prefers.
3. **Lazy sound chips (+5..10 KB when the program does not use them).** TSFM 5.7 KB, SID
   1.5 KB, SAA 1.5 KB, Covox 1.3 KB are allocated at boot because the eLeMeNt has them;
   allocate on the first write to the chip's port. Per-sample data, so they stay SRAM when
   used. PF2026 needs SoundDMA (keep `Sdma` eager or in the tail).
4. **Cold statics, generic for every machine (~20 KB, needs pointer-isation).** BTstack pools
   (~9 KB: hci_connection_storage 4.7 KB, hci_stack 1.3 KB, a2dp/avdtp/l2cap/hids/sdp),
   BtHost s_dev + desc stores 2.1 KB, nm:: UI statics ~3.6 KB, fopen2_pool 1.2 KB,
   DirBuf/LfnBuf 1.1 KB, EpFdc s_drv/s_none 1.7 KB (Enterprise only), FileUtils::fileTypes
   0.5 KB. Main-loop / UI only. No PSRAM .bss section exists, and boards without PSRAM
   need SRAM, so each becomes a `Buffer::palloc(PREFER_PSRAM)` pointer. Keep
   hci_packet_with_pre_buffer (CYW43 DMA) and cyw43_state in SRAM.

Not candidates: FB (HDMI ISR), `.hdmi_lut`, `.dmaovl`/`.dmarovl`/`.gsovl`/`.ngsovl`/`.elmovl`
(hot code), page descriptors (remap path), `Ham` / `s_cnt2` (per pixel), RAM code of
other machines (code, not data: an overlay question, not PSRAM).

## What it buys

1+2: heap at COMPLETE 6.8 -> ~27.6 KB: sixth row block (HGFXAGON buffer fully SRAM),
HRXC `s_cnt2`, HAM, with ~10 KB margin. +3 when unused: ~35 KB. A second 48 KB buffer
in SRAM is still out of reach without 4.

## Status 2026-11-18: items 1 + 2 done, host-tested, NOT hw-tested, uncommitted

- Item 1: `VIDEO::fbDonorSpare(tag, &p)` (what the live map left of a donor) + `Elm::tailAlloc`
  (ElmTail.h, bump allocation, tryMalloc fallback, never freed) for `Hot`, `Regs`, `ElmSdma::State`,
  `ElmSid::State`. Log line `[ELM] pages57 tail: N B of SRAM at ...` once at Elm::reset.
  Bench knob `PICOSPECCY_ELM_TAIL=12544` (HOST_BUILD only) lays out the hw strip on the host:
  tail 14 824 B, all four objects fit. HGFXAGON draws, GALWAY1 SID plays (WAV level as without
  the tail), FDMATEST as without it.
- Item 2: `DmaAttrBuf` = flags (SRAM heap) + `DmaAttrBytes` (shadow + prev_attrs, butter PSRAM on
  `A_ELM` via `Buffer::palloc`, SRAM elsewhere). `captureAttrAfterTransfer` still calls nothing
  (no `bl`), RAM +4 B. NaPICu (DMA port 11) on the eLeMeNt draws its 8x1 letters on the bench.
- Static RAM +18 B. Expected hw: heap at COMPLETE 6.8 -> ~27.6 KB. Test ELF
  `debug/DVp2w-elm-tail-1.1.3` (build 19:53:59; superseded by `debug/DVp2w-elm-clk-1.1.3`, build 20:22:28, which also has the e_zxi_030 clocks). Read from the owner's log: the tail line,
  `setup: COMPLETE, freeHeap=`, HGFXAGON `HGFX buffer 0 in SRAM (the arena): 5 of 6 blocks + 1 from the heap`
  (sramPromote now takes the blocks past the arena from the heap when free >= 8 KB + SRAM_KEEP;
  `s_sramOwn` marks them for sramOff; bench-run with PICOSPECCY_ELM_ARENA=48896).

## Heap walk on hw 2026-11-20 (DVp2w, `debug/DVp2w-elm-perf2-1.1.3`, PLANETOS running)

SRAM dumped over OpenOCD (`dump_image`, 2.5 s, target not halted), dlmalloc chunks walked and
each named by the word that points at it (script: chunk header walk from `__malloc_sbrk_base`,
referrers resolved with `nm -S`). Boot said `heap free 18784 largest 8324`.

**23 144 B of region 0 are lost, and `getFreeHeap` does not count them.** Region 0 =
`20054870..20065A60`; its chunks end at a dlmalloc fencepost at `2005FFF0`, and
`2005FFF8..20065A60` is never handed out (a second dump a minute later: 0 bytes changed). The
cursor jumped to region 1 (the released Next window) when the arena was claimed early, and a
jump strands whatever the current region has above the cursor. The jump itself comes from
`getLargestAllocatable()` (OSDMain.cpp): it binary-searches with REAL `malloc`/`free`, starting
at `getFreeHeap()`, which includes the stranded 57 KB of region 1; the first probe that region 0
cannot take makes dlmalloc extend top, `hr_sbrk` jumps, and the probe's `free` cannot undo it.
dlmalloc 2.6.4 also asks `nb + MINSIZE` (page-rounded) for an extension, not `nb - top`, so a
region is entered by any request bigger than its own gap. Same mechanism on every machine whose
heap has two regions. NOT fixed yet.

| Chunk(s) | Bytes | Where it could go |
|---|---|---|
| `Hgfx::s_arena` (region 1) | 46 088 | stays (HGFX rows) |
| `Hgfx::s_hot` | 8 280 | the pages57 tail: it is allocated AFTER Regs/Sdma/Sid, which took 5.6 KB of the 10 216 B tail, so Hot (8 268) no longer fits there. Hot + Regs first = 9 484 B in the tail, Sdma + Sid (4.4 KB) to the heap: +3.8 KB |
| TSFM: 2 x OpnFm 944 + `s_sin_tab` 2 056 + `s_tl_base` 520 + `audioBufferFM` 1 288 | 5 752 | lazy, first #FFFD FM write |
| AY chip1 1 624, `saaChip` 1 512, Covox buffer 1 288 | 4 424 | lazy, first port write |
| `s_dbg_ring` | 4 104 | console ring; PSRAM if both cores can write it there |
| page descriptors (`MemESP::ram` 1 040 + 3 104, `acc_bank` 800, `MemESP::rom` 800) | 5 744 | stays (remap) |
| `aq_blob` (HDMI audio queue) | 3 080 | stays (ISR) |
| `FileUtils::fileTypes` std::strings (7 chunks) | 1 488 | static const tables |
| ZiFi AT emulator state `(anon)::s` | 1 336 | lazy (first ZiFi port access) or PSRAM |
| DivMMC sector/ide/identity | 1 256 | stays (esxDOS on) |
| `VIDEO::vga` / sharedFB | 1 160 | stays |
| libstdc++ `emergency_pool` | 1 096 | 0 with the pool tunable / no exceptions |
| Buffer pools `g_butter` 776, `g_swapAlloc` 200, `g_flash` 104 | 1 080 | stays |
| `g_dma_buf` (DMA attr flags) | 368 | stays |
| strings (`snap_export_dir`, `MountPoint`), `SaveRect` | 80 | - |

Free: 7 528 (region 0, the old top in the bins) + 8 344 (region 1 top) + 2 912 (region 1 above
the cursor) = 18 784 = the boot figure; the largest single piece 8 324 is what tryMalloc can get.

### Fixes 2026-11-20 (`debug/DVp2w-elm-heap1-1.1.3`, PERF, build 17:43:06)

**hw-confirmed 2026-11-20 on DVp2w (eLeMeNt, log `...17.43.14.432`):** `[OVL] heap: 23136 B left
below a jump given back to malloc` (the host test's figure), `CPU reset` takes 4 416 B (SoundDMA +
SID; Hot is in the tail), boot `entering ESPectrum::loop(), freeHeap=45624` (was 18 784); PLANETOS
`[ELM] mem: ... heap free 44176 largest 32420` (was 18 784 / 8 324), realFPS 47.5-48.0, no OOM.
Other machines with two heap regions (TS-Conf, Pentagon + DMA) NOT hw-tested yet.

1. **Jump tail back to malloc (generic, every machine with two heap regions).** `hr_sbrk`
   records `cursor..region end` when it jumps (`HeapRegions::left[]`, the old region's hwm =
   hi); `heap_reclaim_left()` (CodeOverlay.cpp) lays it out with `hr_left_chunk` the way
   malloc_extend_top lays out an old top (PREV_INUSE, two SIZE_SZ fenceposts) and `free()`s
   it, OUTSIDE malloc. Called from `getFreeHeap`, `getContiguousHeap`, after the probes of
   `getLargestAllocatable` (re-measures when a probe jumped) and after `tryMalloc`'s malloc.
   Boot log shows `[OVL] heap: N B left below a jump given back to malloc` with the region
   lines. Host: `tools/heap_regions_test.c` scenario 6 (the hw layout, newlib 4.1.0
   `mallocr.c` — the recipe's `_mallocr.c` name was wrong) gives back a 23 136 B tail,
   a 22 880 B malloc lands in it, churn + resident windows clean; scenarios 1-5 unchanged.
   Expected hw: eLeMeNt boot heap 18.8 -> ~42 KB, largest ~23 KB. Cost: .data +32 B, code in flash.
2. **Hot first in the pages57 tail.** `Hgfx::hotEnsure()` (the s_hot half of `ensure()`) runs
   in `Elm::reset` right after Regs: Hot + Regs = 9 488 B in the 10 216 B tail, SoundDMA + SID
   (4.4 KB) to the heap. Expected +3.8 KB. `.elmovl` unchanged (17 128 / 17 152 PERF).

## Item 4, first round 2026-11-21: the Bluetooth statics (heap +11 560 B and BT use hw-confirmed DVp2w, committed)

BTstack pools (HAVE_MALLOC + `malloc=tryMalloc` in btstack_memory.c, AVDTP pools kept),
BtHost's discovery/SDP/HID-descriptor arrays and the UI's BT lists moved to the heap, taken
on first use: .bss -11 556 B, expected boot heap +~10.2 KB on every machine of a W board
until Bluetooth is used. Details and the rest of the cold .bss / one-machine RAM code
ledger: `docs/notes/memory-sram-flash.md`, "Bluetooth state on the heap". Test ELF
`debug/DVp2w-btheap-1.1.3`.

## Item 4, second round 2026-11-21: FatFs scratch + nm:: UI tables (hw-confirmed DVp2w, uncommitted)

`Buffer::pcold` (PSRAM first) for FatFs LFN scratch + the fopen2 pool, UiStage's staged
values, the ZiFi transport table; rawSourcePick rows per call. .bss -4 332 B. Details:
`docs/notes/memory-sram-flash.md`, "FatFs scratch and nm:: UI tables". Test ELF
`debug/DVp2w-coldbss3-1.1.3` (the first cut tested PSRAM with `>=` and left the LFN block on the heap).

## Item 4, third round 2026-11-21: EpFdc buffers + BTstack file statics (hw-confirmed DVp2w, committed)

.bss -3 977 B; `docs/notes/memory-sram-flash.md`, "EpFdc sector buffers + three BTstack file
statics". Test ELF `debug/DVp2w-coldbt-1.1.3`.
