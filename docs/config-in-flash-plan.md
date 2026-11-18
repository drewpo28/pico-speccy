# Config in flash (plan, deferred 2026-11-18)

Status: **plan only, nothing written.** Owner decision 2026-11-18: record the plan, not now.

## Why

A board with no SD card (and no USB root) keeps its config only in `nvs_ram_buf`
(`src/app/Config.cpp`, .bss): every reboot, and every machine switch that hard-resets,
boots the defaults again (see `memory-sram-flash.md`, "No PSRAM + no SD"). A small
flash area would make settings survive on card-less boards.

## Scope

- **Fallback only:** flash is used only when no SD/USB config storage is mounted.
  Boards with a card keep `storage.nvs` on SD exactly as today, so no flash write ever
  stalls emulation on them.
- Content = the same text `storage.nvs` holds (Config::save's buffer); profiles,
  `palette.nvs`, wifi/remotes stay SD-only (can be added later if wanted).
- Open question for later: also use the flash copy as a fallback on a card whose
  `storage.nvs` is missing or corrupt? Not in the first round.

## Layout

- Today `storage.nvs` is about 4.8 KB (host copy), so one copy = 2 sectors (8 KB).
- Two slots A/B (8 KB each, 16 KB in all) for power-fail safety. Slot header:
  magic, sequence number, length, CRC32. Boot reads both and takes the valid one with
  the higher sequence; a save writes the OTHER slot.
- Place: fixed at the top of flash, just below the BTstack pairing sectors
  (`@BT_FLASH_RESERVE@`, `rp2350-memmap.ld` ~line 489), so it does not move with
  firmware size and survives reflashing a new version (keep the slot format versioned).
  `__gm_bank_end` moves down by 16 KB: add a `CONFIG_FLASH_RESERVE` beside
  `BT_FLASH_RESERVE` in `cmake/memory-layout.cmake`.
- **Check the 4 MB images first:** they sit near the GM.DLS floor
  (`GM_BANK_MIN_KB`, 1632 KB); 16 KB less bank may trip the ASSERT on the tightest
  board. If it does, 4 MB gets one slot (8 KB, no A/B) or the feature goes 16 MB-only.

## Write path

Reuse what custom ROM flashing already does (`memory-sram-flash.md`, "Custom ROM
flashing runs at 252 MHz"):
- `Buffer::flashErase` / `Buffer::flashProgram` (RAM-resident, `src/app/Buffer.cpp`).
- `Buffer::flashClockEnter/Exit` around the whole write, core1 parked, and
  `flash_timings(mhz)` + `xip_cache_invalidate_all()` re-applied after the bootrom
  calls (boot2 timing otherwise).
- Write only when the content differs from the current slot (wear: about 100k cycles
  per sector; also skip the stall).
- Only from the UI paths (menu close / explicit save), never from the frame loop.

## Cost

Datasheet numbers for a typical W25Q-class QSPI flash, **NOT measured on our boards**:

| Step | Typical | Worst |
|---|---|---|
| Erase 2 × 4 KB sectors | ~90 ms | ~0.8 s |
| Program ~19 × 256 B pages | ~10-15 ms | ~60 ms |
| **Total per save** | **~0.1 s** | **~1 s** |

XIP is off for the whole time: emulation and sound pause, core1 is parked. HDMI line
ISR is RAM-resident, so the picture should hold (check `HDMIAU dur/gap`).
Flash/SRAM cost: the slot read/write code (~1-2 KB flash, write helpers already in RAM).

Estimate: about half a day of work + a hw round on DVp2w.

## Steps

1. `CONFIG_FLASH_RESERVE` in the linker script and memory-layout; build all boards and
   check the GM.DLS floor on the 4 MB images.
2. `ConfigFlash` (in `src/app/`): `readSlot(std::string&)`, `writeSlot(const std::string&)`
   with header + CRC + A/B.
3. Config::load: no SD -> parse the flash slot into `nvs_ram_buf`.
   Config::save: no SD -> after building `nvs_ram_buf`, `writeSlot` if changed.
4. Host bench: emulate the flash area as a file, `PICOSPECCY_NOSD=1`, change settings,
   restart, check they survive; corrupt one slot, check fallback to the other.
5. hw (DVp2w, card removed): settings survive reboot and machine switch; measure the
   save stall; HDMI audio clean during the save.
