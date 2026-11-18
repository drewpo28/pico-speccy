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
  `debug/DVp2w-elm-tail-1.1.3` (build 19:53:59). Read from the owner's log: the tail line,
  `setup: COMPLETE, freeHeap=`, HGFXAGON `HGFX buffer 0 in SRAM (the arena): 5 of 6 blocks + 1 from the heap`
  (sramPromote now takes the blocks past the arena from the heap when free >= 8 KB + SRAM_KEEP;
  `s_sramOwn` marks them for sramOff; bench-run with PICOSPECCY_ELM_ARENA=48896).
