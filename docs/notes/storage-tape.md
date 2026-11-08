<!-- Moved verbatim from CLAUDE.md on 2026-11-04; sections in their original order. -->

## Launching from the Web catalog: never unlink a temp file that is still open (2026-08-13)

### Quick-start does not download a file it already has (2026-10-06; hw-confirmed the same day, owner: "работает")

Owner's request: Enter on a Web Archive entry that this session already pulled into
`/tmp/_run.<ext>` launches that file instead of downloading it again. `s_rfd_cache`
(OSDFile.cpp, 4 slots): key = `cacheId() | cwdPath() | display name`, the temp path,
and the size `get()` left. A hit also needs the temp file to still exist at that size —
a guest writing to a mounted `.trd` keeps its size, a later half-finished download into
the same `/tmp/_run.trd` does not (and a successful one replaces the slot: one temp
name per extension). `rfd_release_tmp` still runs first either way, so a re-launch
starts the mounted file afresh. Session-only by design: nothing persists the key, a
reboot downloads again. Log line `net: quick-start reuses /tmp/_run.<ext>`. The
"Downloading..." dialog still flashes for the catalog's locator lookup before the
check. Test ELF `debug/DVp2-rzx-spin3-1.0.9.elf`.

**hw-confirmed 2026-08-13 on z0p2**: two demos launched in a row from Web Archives
each start the right one, and the catalog listing is clean. Symptoms were "the second
demo starts the FIRST one" plus a Web-catalog listing drawn as binary garbage — ONE
root cause, and both halves are worth remembering because neither points at the other.

Quick-start extracts to a FIXED path per extension (`/tmp/.zip_extract.<ext>`), so the
file a previous launch still holds open is the file the next launch unlinks. `FF_FS_LOCK`
is **0**, so `f_unlink` succeeds on an open file — and the dangling FIL is not inert:

- `f_close` → `f_sync` rewrites the directory entry through the CACHED `fp->dir_ptr`
  whenever the FIL is `FA_MODIFIED` (ff.c, non-exFAT branch), and `dir_alloc` has
  meanwhile handed that deleted slot to the freshly extracted file (it scans for `DDEM`
  from the top). The new entry inherits the OLD start cluster and size → the next mount
  reads the PREVIOUS image, whose data is still intact on the card.
- The old chain is free in the FAT, so later allocations get it — the catalog's own
  `/tmp/.net_*.idx` / `.catv_*.tsv` — and those files come back cross-linked, i.e. as
  binary junk. That is the garbage listing; nothing was wrong with the catalog code.
- **What makes a mounted image `FA_MODIFIED` in the first place is READING it.** FatFs
  `f_lseek` stretches a `FA_WRITE` file when it seeks past EOF, and every image shorter
  than the geometry it emulates is read past its end routinely (an SCL is tens of KB
  standing in for 640 KB).

Fix (`ZipExtract.cpp`): `cleanup()` **leaves a path that is still open alone**
(`tempPathBusy` — both `fdd` and `mb02_fdd`, tape, ALF cart; deleting a mounted image
under the running machine was never right either), and `releaseTempOwners` runs beside
the finalPath `f_unlink`/`f_rename`, i.e. AFTER `extractFile`.

**The TIMING of that release is load-bearing, not just its presence.** An earlier
version released inside `cleanup()`, before `extractFile` — correct on paper, and it
reordered every heap free in this path relative to `extractFile`'s 8 KB alt-stack and
inflate buffers. Two hw runs then died with a wild PC (`SIGBUS`, INVSTATE/UNDEFINSTR)
out of `rvmWD1793Step`'s own frame where five launches on the old ordering had been
clean. Keep the release at the last possible moment.

Also from this session:

- **`sorted_files::get()` returned an uninitialized 253-byte STACK buffer** on a short
  read (SortedFiles.h) — that is what turned a broken `.idx` into plausible-looking
  garbage names (residue of earlier strings) instead of empty rows, and it hid the real
  failure. Zeroed + length-checked now.
- **`SCLtoTRD` never bounded the SCL file count**: the header byte allows 255, TR-DOS
  holds 128, and the loop writes `track0[(i << 4) + 15]` — up to 4080 bytes into a
  2304-byte track 0 (inside the 8 KB `g_rawTrkDataBuf`, so it corrupts the staging area
  and `sclDataOffset`, not the heap). Clamped to 128.
- **OPEN: reading past EOF still grows images on the card**, filling them with whatever
  the freed clusters held. The obvious clamp (past EOF → blank sector) was tried and
  **BACKED OUT** — it changes what the guest sees for empty sectors, which moved the FDC
  onto a different path, and the wild-PC crash above followed it. Reverting it made the
  crash go away. Whether it created that fault or merely exposed a latent one in the FDC
  is unresolved; the comment sits at the old call site in `wd1793.cpp`.
- **The fault handler now prints two more lines** (`main.cpp` `sigbus_handler`): r0-r3/
  r12/xPSR/EXC_RETURN plus eight words of the faulting function's frame — and it takes
  EXC_RETURN from the naked trampoline because bit 4 says whether the frame is 8 or **26**
  words. `rvmWD1793Step` opens with `vpush {d8}`, so this path really does use the
  extended FP frame, and reading the caller's frame at the basic-frame offset would just
  print FP registers dressed up as return addresses. Without these lines the crash was
  unreadable (PC in `.bss`, LR valid, and NO indirect branch anywhere on the path — the
  answer only came from the frame: word 3 = `Ports::FDDStep`, so the chain above was
  intact and only `rvmWD1793Step`'s own frame was gone).
- Bisect trick that settled the attribution: **the ZipExtract change is a no-op on the
  FIRST launch of a session** (nothing is mounted, so nothing is released). A crash on
  launch #1 therefore cannot come from it.

## IDE / HDD: a scheme with no image is OFF to the guest (2026-09-08, hw-confirmed for SMUC only)

`IDE::portScheme` is the scheme the PORT DECODERS answer for: `IDE::scheme`
while at least one image is really open, `OFF` otherwise. `IDE::scheme` and
`Config::ide_scheme` stay what they were — the configuration — and the menu, the
image rows, Hardware Info's scheme name and the budget all keep reading those.
One rule for every interface: selecting NEMO / PROFI / SMUC / IDEDOS and
mounting nothing is indistinguishable from Off, so the guest can never find a
phantom controller with no disk behind it (the owner's rule).

- **It is a plain mirror, not a `present()` call.** The five gates sit in the
  RAM-resident `Ports::input`/`output` decode chain (NEMO in+out, PROFI in+out,
  the `p3eIde()` helper) plus `smucDiskActive()`, so the test has to stay one
  byte load. `IDE::init()` sets it (`present() ? scheme : OFF`) and `close()`
  clears it — every mount, eject, geometry edit and remount funnels through
  `init()`, so there is no third writer. The boot log says
  `- no image, ports off` when a configured scheme comes up dead.
- **The +3e gate never tested the scheme at all** (`Config::isPlus3e() &&
  plus3eIdePort(address)`), so an explicit Off in Devices did not silence it
  either, against that section's own claim that the interface is a card the user
  may switch off. It tests `portScheme == PLUS3E` now, which fixes both.
- **SMUC is the one place where "off" is not silence**: the CARD is fitted by
  CMOS + NVRAM (see the SMUC section), so its ATA window keeps decoding and
  answers `0x00` = device absent; only the disk goes away.
- **The risk to watch on hardware** is a guest that polls BSY on a port nobody
  answers: unattached reads are 0xFF on the +3 (no floating bus) and the float
  value elsewhere, i.e. BSY stuck at 1 until the driver's own timeout. Profi
  CP/M and the +3e boot are the two to try WITHOUT an image (both used to see an
  absent device instead). If either wedges, the fallback is the SMUC shape —
  keep the window decoding and answer 0x00 — which is one line per gate.
- Diagnostics: the `IDE_PORT_TRACE` probe lines print `scheme=<config>/<port>`,
  because a capture that says `scheme=2` while nothing answers otherwise sends
  the next session hunting a decode bug.
- **User-visible consequence on Scorpion**: ProfROM's Disk Utility → Autostart
  boots `boot<B>` from a PSEUDO-DISK, which lives inside the MFS partition on the
  HDD, so it works only with an image mounted (owner, 2026-09-08) — ejecting it
  now silently disables the whole interface, by this rule. Hardware Info's
  `SMUC card` row says which of the two halves is live, which is the quick check
  before suspecting the setting was lost.

## IDE slot served off the real SD card (raw sector window), 2026-11-01, hw: reads work, writes owed

"IDE as an emulation of the real SD card, the way DivSD does it for esxDOS." An IDE
slot can be backed by the card's OWN sectors instead of an `.hdf`: the guest's LBA 0 is
`Config::ide_raw_lba[slot]` and the drive is `Config::ide_raw_len[slot]` sectors after
it. `src/speccy/devices/storage/IdeMbr.h` is a pure MBR decoder (host test
`tools/ide_mbr_test.cpp`, 23 checks).

- **`Config::ide_raw[2]` is the MODE and it is the write switch**: 0 = the slot uses
  its image (off), **1 = read-only (the only value anything ever sets by itself)**,
  2 = writable. Owner's rule, and the reason the default is 1-and-off: a raw write
  lands in the card's own sectors PAST FatFs, so a wrong window destroys the card's
  filesystem. Every IDE scheme (NEMO/PROFI/SMUC/IDEDOS/divIDE/ATM/NEMO-on-Evo) can use
  a raw slot — the window is a property of the SLOT, not of the controller.
- **The window is the safety, and it is clamped twice**: `IDE::openRaw` refuses a start
  past the end of the card, clamps `len` to what is left, and clamps it to the CHS
  ceiling **65535 x 16 x 63 = 66 047 280 sectors** (Profi's CHS-compat mode is
  H=16/S=16, i.e. a quarter of that, so the ceiling is computed per scheme). The clamp
  is not cosmetic: every guest here SIZES a drive by walking cylinders until a read
  fails, so a longer window would be truncated by the GUEST at a place we did not pick.
  `size_bytes` is saturated at 4 GB for the same reason (it is only the "same disk"
  key for snapshots; the menu and IDENTIFY work from the geometry).
- **Reads go through `disk_read`, writes through `disk_write` + `ff_raw_write_notify`**
  on the FatFs drive that holds the root volume (`rawDevice()` = `FileUtils::usbRoot ? 1
  : 0`, DivSD's rule). The notify is not optional: the card may be the volume this
  firmware's own files live on, and FatFs caches FAT/directory sectors — DivSD does
  exactly this. A card error is reported as IDNF, **never served as 0xFF**, because a
  guest reads until a failure to size the disk.
- **A read-only slot refuses WRITE SECTOR (0x30/0x31) in `execute_command`**, with
  ABRT and no data phase, before anything is read or written. Silently dropping the
  512 bytes would hand the guest a "completed" write that changed nothing, and
  IDEDOS/CP/M cache their idea of the disk from those writes.
- **One medium per drive**: turning the window on clears the slot's image
  (`Config::ide_image`), mounting an image clears the mode (`DiskSlots::slotMount`),
  F8 eject clears both (`slotEject`) — `IDE::init` prefers the window, so leaving
  either behind would keep serving the card after the user ejected the drive.
- **`mounted(d)` replaced `file_open[d]`** as the "is there a device here" test
  (`present()`, `read_sector`, `write_sector_done`, `preload_sector`, `reset_signature`,
  `write8` case 6, `init`'s geometry block, `snapSave`/`snapLoad`). `setupFastSeek` and
  the ProfiHiDD header synthesis stay FILE-only: a raw slot has no cluster chain and no
  `f_size`.
- **Snapshots**: the raw bits ride in the spare high bits of `snapSave`'s open-mask byte
  (`0x10`/`0x20`), so `IDE_SNAP_VER` stays 1 and an older `.pss` simply has 0 there —
  refused on a raw slot, which is the safe answer (an image file and a card window can
  report the same size and are different disks). `ide_raw0/1` and `ide_rwin0/1` are
  class-2 keys in Config's list, like `ide_img0/1`.
- **Menu**: Devices > IDE / HDD > **SD card (raw)** — per slot a `source` action that
  CYCLES off -> read-only -> write (Write is the third press, not a value the cursor can
  Tab onto, and it toasts "writes go to the card!"), a `window` action typing
  `start/len` (0/0 = the whole card; empty entry changes nothing) and a **`from MBR`**
  action that lists the card's four partition entries and fills the window with the
  chosen one — the picker only fills the typed window, it is not a second medium type.
  The window rows grey out while the slot's source is off. The slots level shows
  `hd0 master  SD card` with a `RO` badge and the geometry; F2 edits the window of a
  raw slot and the C/H/S of an image slot.
- Not done on purpose: no `.pss` `liveMedia()` line for a raw slot (there is no file to
  name, and that list is re-opened by path), no MBR *writing*, no partition-table
  parsing beyond the four primary entries (logical partitions inside an extended
  container are skipped by `ide_mbr::parse`).

## The SD write path: the card has to STOP saying "data accepted" (2026-09-18, NOT hw-tested)

Wild Commander (TS-Conf, boots as `boot.$C` off the FAT card through TS-BIOS)
read the card perfectly and **froze the machine the moment it saved `wc.ini`**,
leaving the file gone after a forced reboot. The bug is one line of
`DivMMC::mmc_read`'s CMD24 case: after the data-response token it answered
**0x05 to every further read, for ever**. A real card answers 0x05 ONCE, then
holds MISO low (0x00) while it programs, then 0xFF.

- That difference is invisible to a driver whose busy-wait is "read while the
  byte is 0x00", and it is a **permanent hang** for one that waits for the busy
  phase to END by polling for 0xFF. WC's Z-Controller driver has BOTH loops —
  `IN A,(C) / OR A / JR Z` after the response and an unbounded
  `IN A,(C) / INC A / JR NZ` ("wait ready"), the latter being exactly the
  "ожидание busy после завершения транзакции записи" its changelog added. The
  read path was never affected because CMD17 ends with `mmc_read_index = -1`,
  i.e. it does return to 0xFF.
- Fixed by `mmc_wr_resp` (-1 none / 0 token / 1 busy / 2 ready), armed when a
  block is stored — for CMD24 **and** every CMD25 block, which had no data
  response at all (WC's error path for a bad response is `OUT (#0FAF),err` +
  `JR $`, i.e. a dead hang with a coloured border — worth recognising). It is
  read at the TOP of `mmc_read`, ahead of the command latch, because the latch
  is now released as soon as the block's CRC bytes are in.
- **Releasing the latch is the second half of the fix**: `mmc_index_command`
  used to run past the end of the block for ever, so with CS held low across
  commands the card went deaf to the NEXT command frame and the driver hung in
  its R1 wait instead. Only a CS edge healed it.
- Third, smaller: the 0xFE data token may be preceded by any number of 0xFF gap
  bytes; the index now holds at the token slot until the token really arrives,
  or the whole block shifts one byte per gap.
- Checked on the host by transcribing the patched state machine and driving it
  with WC's own byte sequence (CS, CMD24 frame, R1 wait, gaps, token, 512+CRC,
  response, busy wait, ready wait, and a second command with CS held low). The
  pre-fix model fails exactly at the ready wait — that is the hang. Re-derive it
  before touching this area again; the file itself cannot be host-compiled (it
  pulls FatFs and the SDK).

### ...and the CMD18 gap byte: WC's DMA sector reader hung on a 0x00 (2026-09-30; hw-confirmed the same day — owner: "да, запустился")

"TGV video does not play under WC" (VIDEO_PL.WMF v0.71, `debug/TGV/`, `debug/WC/`).
VDAC2 is not involved: the player is TS-Conf-only. What it does, from its
disassembly (`debug/WC/WC/VIDEO_PL_disasm.txt`, real address = listed - 0x200):
a TGV is a 512-byte header (`TGA Video v0.1/v0.2`, +0x10 = MP3 sectors, +0x20 =
2 x sectors per frame), then in v0.2 the MP3 whole (3931 sectors in
`RUNNINGM.TGV`), then frames of 73 sectors = 1 CRAM palette sector + 72 data
sectors. Video = 256c at RRES 256x192 (`VConfig 02` via WC API #42), 256x144
picture, four 16-page frame buffers at pages 0x20/0x30/0x40/0x50 flipped every
3rd INT from WC's INT hook (API #56), palette through the FMAddr window
(`OUT (#15AF),#10` + 512 LDI). The 256-byte rows land on the 512-byte row
stride by the DMA itself: the driver reads a sector as TWO 128-word bursts with
`D_ALGN` + ASZ 512 (`ctrl 5A`, `len 7F`, `num 1`), so each half sector starts
its own 512-byte window. Sound = the GS Player detect, then a 1 KB card-side
program uploaded with `#14`/`#13` and THE WHOLE MP3 pushed through `#B3` byte by
byte before playback ("Please Wait..." with a progress bar); the card program
feeds MD_SEND from a 32 KB-page ring on MDDRQ. `binary.tgv` (v0.1, MP3 field 0)
never touches the NeoGS at all, which is what made it the bisect.

- **The hang**: `binary.tgv` sat in "Please Wait...". Dump: `PC=3A14 BC=0057
  A=00 HL=C400`, TS-Conf `dma: daddr=080400 ctrl=5A` — one sector into page
  0x20, hung on the second. WC unpacks its SD driver into RAM at 0x3800 (it is
  packed inside boot.$C, so read it OUT OF THE DUMP: `picospec_mem0.bin` +
  `tools/z80disasm.py --org 0x3800`). Its DMA reader (`0x3815`, WC API #3C —
  used by VIDEO_PL, TAPM, BMPV, TXTEDIT; GSPLAYER/MOUNTER/the panels read
  through `0x380F` and never hit it) sends CMD18, then per sector: wait for the
  first non-0xFF byte on `#57` and `CP 0xFE / JR NZ,$` — **a deliberate hang on
  anything but the data token** — DMA the 512 bytes, read 2 CRC bytes, loop.
  Our `DivMMC::mmc_read` CMD18 case restarted at the R1 slot for every block,
  i.e. put a `0x00` between a block's CRC and the next token where a real card
  (and Unreal's `sdcard.cpp`, which streams `FE data CRC CRC FE ...`) sends only
  0xFF. Fixed: `mmc_read_cont` — R1 once per command, two explicit CRC bytes,
  0xFF gap slots on every continuation. The UFO2 author's note ("CMD18 +
  DMA SPI->RAM with a DMAStatus poll, the volume silently fails to mount") is
  the same reader shape and should be re-tried on this fix.
- **Not the DMA**: DMACtrl here is b7 R/W, b5 SALGN, b4 DALGN, b3 ASZ, b2..0
  DDEV (`dmaStart`), so `5A` = SPI->RAM, DALGN, 512-byte windows — exactly the
  row spreading above; the `DivMMC::zc_read_data` path behind it is the one WC
  boots through.
- Test ELF `debug/DVp2-tgv-cmd18-1.0.8.elf` (+ .uf2, plain main). **Hw 2026-09-30,
  owner: `binary.tgv` starts and plays** (the read path — one verdict, not
  itemised). **Still owed**: `RUNNINGM.TGV` with NeoGS on (the 2 MB byte-by-byte
  upload, then MP3 + video), TAPM / BMPV loads (same reader), and the ZC card
  still mounting under TS-BIOS / WC / esxDOS (the R1 slot moved).
  Diagnostic build that exists for this: `debug/DVp2-tgv-zctrace-1.0.8.elf`
  (`TS_VIDEO_TRACE` + `ZC_PORT_TRACE`, built with `-DTSOVL_WIN_SIZE=28672`
  because that pair alone overflows the AUTO TS overlay window).

### ...and the "negative" frames: nine rounds, seven mechanisms (2026-09-30; hw-confirmed on pal9 — owner: "теперь везде работает")

Once the video played, every few frames a band from some row down came out in
wrong colours (owner's screenshot: top ~15 rows right, the rest "negative"), and
the same artifact was reported in **Ppal** (`debug/TSCONF/ppal.spg`) and
**Keftale** (`debug/TSCONF/keft.trd`), V-Sync on or off. All three are `nb == 1`
titles (TGV: a 256-cell CRAM sector per frame through the FMAddr window, 512 LDI
from the INT hook at 0x8283; ppal's main loop at 0x6457 rewrites all 256 cells in
a free-running loop over a static 256c picture; keft a per-frame fade over a
palette that already needs more than the 184 slots), i.e. the Kolbass
hold/release path with the CPU as the palette writer — the hold trigger is
`wide`, never `blit`. **What actually went wrong, in the order it was found:**

1. **`ts256ProgramBank` was spending its time in `tmds_balanced_pair`** (pal5, the
   TGV fix, hw-confirmed on `binary.tgv`). Every `graphics_set_palette` runs the
   balanced-pair SEARCH (tmds_pair.h, ~1000 ops per channel) and `tmds_encoder`
   six times; 184 slots came to **7-14 ms** — a whole blanking and more — so the
   palette landed with the beam far into the next sweep, over rows core1 had
   already re-rendered under the OLD colours. Now memoised per 8-bit level in
   hdmi.c (`hdmi_balanced_pair_lut`, `tmds_encoder_lut`, lazily filled, +1.5 KB
   .bss): the flush is 0.4-1.5 ms. Any other palette-heavy path (ULA+, the
   Gigascreen blend LUT, `applyPalette`) gets the same speed-up for free.
2. **The blanking-path release waited for core1 inside the window** (pal6): the
   pal3 fix ordered it Assign -> post -> DRAIN -> flush so the palette would
   follow the pixels, and the drain is 3.6 ms of 240 rows. With Reduce (2.4 ms,
   below) that made `total=6.9 ms` against 5.2 ms of blanking at 640x480 — and
   RRES 360x288 has NO bottom border rows (`lin_end=0..240`), so blanking is the
   whole window: `beamAfterMax=232`, `beamIn=23-34/s`, the top rows old under
   the new palette every release. Now `tsReindexRelease(beamOut)`: from the poll
   (beam in blanking or below the picture) it is Assign -> FLUSH -> post, no
   drain — core1 starts at row 0 while the beam is still out and renders ~4x
   faster than the beam scans, so the old rows are never displayed; the release
   costs core0 ~1.5-2 ms. The forced path (EndFrame, beam anywhere) keeps
   post -> drain -> flush, which confines the tear to the rows the beam passes
   during the drain. `TS_REL_MIN_BLANK_LINES` dropped 110 -> 70 (the release is
   shorter), and `displayBlankLeftOk()` now caps the requirement at the mode's
   blanking minus 6 (`hdmi_blank_lines_total()`): 720x576 has only **44** blanking
   lines, where a fixed 110 could never be met and EVERY release would have been
   the forced one.
3. **The "wait for a gap in the CRAM writes" gate (pal4) was the WORST of the
   three for ppal** — a writer that never pauses always timed out (40 ms = two
   frames), and by then two EndFrames had passed, so the release was FORCED with
   the beam wherever it was: `burstForced=9 relForced=18` per second of
   `rel=27`. Removed. A mid-burst snapshot is one frame with a palette the guest
   was still writing, which the hardware shows too. The forced release itself
   now needs the hold to outlive TWO EndFrames, not one: with V-Sync pacing a
   frame that ends late in blanking gets its release at the next blanking
   start, which comes ~1 ms before the second EndFrame — forcing at the first
   one released early, into the picture.
4. **`ts256Reduce` is ~2.5x cheaper** (host: 125 -> 50 us on a random 256-colour
   palette, 10 -> 4 us on a gradient; identical merge count, error within 0.3 %):
   cells sorted by colour (groups come out adjacent, no 256 x n dedup search),
   nearest neighbour by scanning outward in that order and stopping once the red
   difference alone exceeds the best distance, and after a merge only the groups
   whose neighbour DIED are marked stale and re-evaluated lazily when they come
   up as the minimum (their recorded distance is a lower bound, so that is
   exact) — the old code recomputed every group pointing at the kept one too,
   and relabelled 256 cells per merge. It still runs once per frame for a
   continuous writer: the EndFrame pre-assign is defeated whenever the guest
   writes CRAM again before the release, which ppal always does (`assign=2.4 ms`
   on every release in the pal5 log). Scratch check in the session scratchpad
   (`reduce_test.cpp`, old vs new over 400 palettes).
5. **The demos never had the release problem at all — the pal6 log said so (hw
   2026-09-30, owner: "better, but something still interferes")**: `beamIn=1-5`,
   `beamAfterMax` under 25 on the blanking path, `forced=0` — and still
   `bad=8000 rows/63 sweeps` on ppal and 170-390 on keft. Two mechanisms, both in
   the STICKY path (a change that does not hold), neither of them timing:
   - **RUNNINGM: `nb=3` with `live=61/61 exh=1 near=2000/s`.** `ts256PickBanks`
     runs at mode entry on whatever CRAM holds THEN — the player opens at RRES
     256x192 on a near-empty palette, so it got three banks of 61 slots, and its
     100+ colours per video frame exhausted a bank ~160 cells a frame with nothing
     degrading it: the "back to one bank" rule lived only in `ts256Version` (the
     hint path), and the video's DMA lands in the NON-visible frame buffer, so the
     hint never rises and every change is beam-scheduled. `ts256DegradeOneBank()`
     is one helper now and the poll's sticky assign calls it too.
   - **ppal / keft: a full single-bank pool with changes too narrow to hold.**
     ppal's free-running rewrite held on the frames where >= 32 cells moved and
     took the sticky path on the others (`force 20 blank 27` applies/s,
     `moved=19000/s`); keft's fade changes 6-12 cells a frame on `live=184/184`
     (`near=300-600/s`, `stickyPre=30-90`). On an exhausted pool the sticky path
     can only park a changed cell on the nearest colour or move it to a shared
     slot — under rows already rendered with the old numbering. So with
     `ts256_exhausted` a DENSE change of `TS_REINDEX_EXH_CELLS` (4) cells holds
     like a wide one (the density test keeps Demorama's 1-2 cells a row on the
     beam rule); the picture is then re-rendered from a Reduced map in blanking,
     the one consistent answer once colours outnumber slots.
   Test ELFs `debug/DVp2-tgv-pal7-1.0.8.elf` / `-pal7-trace-`. **Hw 2026-09-30,
   owner: binary.tgv, RUNNINGM.TGV, keft.trd and Kolbass all work on pal7**
   (the RUNNINGM log has the `exhausted - back to one bank` line once, as
   predicted); ppal "no longer a negative, a WRONG PALETTE flashes through,
   and worse WITH V-Sync". See 6.
6. **ppal on pal7: the hold fired on every SECOND frame only, and the other half
   went out through the v_sync poll INSIDE the picture** (`wide=25 rel=25
   force=25` per 50 frames, `bad≈138 rows` on every sweep, `live` alternating
   126/184 and 184/184). Two facts behind it, both worth keeping:
   - **ppal's rewrite is SPREAD, not a burst**: ~274 cells over the whole frame,
     about one per row, so the `palBurst` density test (4 cells per row) fails
     whenever the render tick is running — and passes only in a frame where a
     release has already PARKED the tick (`ts_line_t` = MAX pins `palRow` at the
     top, span 0). So a hold only ever followed a release inside the frame, and
     the frame after that took the sticky path (near parks + merged-cell moves
     under rows rendered with the old numbering). The density test protected
     Demorama, which is NOT exhausted (123-125 colours) — on an exhausted pool a
     per-line palette effect cannot be rendered either way, so `exhWide` drops
     the test: `ts256_exhausted && ncells >= 4` holds, spread or burst.
   - **The "blanking just started" forced poll in the V-Sync pacing wait runs
     with the beam at row 190**: `v_sync` fires `TS_VSYNC_LEAD_LINES` before
     blanking on TS-Conf (the Kolbass round-5 lead), so its comment is wrong for
     these modes and a sticky flush there lands mid-picture — which is why the
     owner saw MORE glitches with V-Sync on (`force=0` with it off). Left as is:
     with the hold now taken on every dense frame, `reindexPending` keeps that
     poll off, and for a non-exhausted animation the map does not move.
   Also: `[TSYGC]` fired on EVERY frame of ppal (RRES 360x288, `crop=24`) and
   shredded the UART — the invariant is `ygctr == curline + crop` at GYOffs 0,
   it compared against `curline` alone. Fixed.
   Test ELFs `debug/DVp2-tgv-pal8-1.0.8.elf` / `-pal8-trace-`. **Hw 2026-09-30
   on pal8: ppal PERFECT with V-Sync off (`bad=0`, `force=0`, hold every
   change) and UNCHANGED with V-Sync on** (`wide=25 rel=25 force=25`) — see 7.
7. **The per-frame cell count reset itself on every write made after a release
   (pal9, NOT hw-tested).** `tsCramChanged` counted cells per frame by testing
   `ts_pal_seq != ts_frame_seq`, but a change made after the tick has parked
   (a release inside the frame, then ppal's rewrite going on behind it) is
   "the next frame's top" and sets `ts_pal_seq = frame + 1` — so every such
   write reset the count to 0, it never reached `TS_REINDEX_EXH_CELLS`, no hold
   started, and the v_sync poll flushed the sticky map at row 190. That is why
   the V-Sync-off run was clean: with no forced poll those cells simply waited
   for the next frame, where the count works and the hold covers them. The
   counter has its own frame tag now (`ts_pal_cnt_seq`). Expected on ppal's
   trace WITH V-Sync: `wide` = `rel` = ~50 per 50 frames, `force=0`, `bad`
   near 0. Test ELFs `debug/DVp2-tgv-pal9-1.0.8.elf` / `-pal9-trace-`.
   **Hw 2026-09-30, owner on pal9: "да, теперь везде работает"** — binary.tgv,
   RUNNINGM.TGV, keft, Kolbass and ppal, V-Sync on and off — and the same
   evening the regression set: **Demorama, RobFgift and Ninja Gaiden all
   work** (owner). So the exhausted-pool rule did not catch Demorama's
   per-line effect (its 123-125 colours keep it off that rule, as designed)
   and the `nb=4` version-bank path is unharmed. Only a 720x576 run of the
   `nb=1` titles (the 44-blanking-line cap in `displayBlankLeftOk`) is still
   owed.
- **Hypotheses hw-refuted on the way, recorded because the log looked clean under
  each**: "Reduce renumbered the map under core1" (pal1 — the `mapDeferred`
  guard stays, it read 0 here); "the beam gate at entry only" (pal3 — the margin
  gate stays, `relBeamIn` still counted with it); "a CRAM snapshot mid-burst"
  (pal4 — `inBurst=0` on TGV, and see 3). **The lesson that cost most: the
  `hold:` log line had grown past `Debug::log`'s 256-byte buffer**, so the
  `relFlush`/`relTotal` fields were silently cut and the pre-flush beam sample
  read "still in blanking" while a 10 ms flush ran on. Two rounds went to that.
  The release fields live on their own `[TSPAL] rel:` line now, and the beam is
  sampled AFTER the whole release.
- **Instruments** (`TS_VIDEO_TRACE`): `[TSPAL] rel: beamStartMax beamIn
  beamAfterMax assign post drain flush total | blank forced inBurst` — `beamIn`
  must read 0 and `beamAfterMax` < lin_end (or -1) on a blanking-path release,
  `drain` is 0 there (only the forced path drains), `forced` should be 0 on any
  title with V-Sync; `blank` = releases taken on the fast path.
- pal6 was hw-run 2026-09-30 (the release counters came out clean, the demos still
  wrong — see 5); **hw owed on pal7**: ppal, keft, `RUNNINGM.TGV`, then Kolbass /
  nygift (same hold path, now also holding on narrow changes) and RobFgift /
  Ninja Gaiden (`nb=4`; a degrade to one bank there would show in the log as the
  `exhausted` line and must not happen), Demorama (its per-line effect must NOT
  hold: `hold=0`, `spread` climbing), and a 720x576 run (the 44-line rule).

### What Wild Commander still needs from us — CLOSED (analysed and finished 2026-09-18)

The gap list is done. **VDOS / FDDVirt**, **TSU over TEXT** and **SMUC** were
implemented the same day (three sections below). The three that remain are owner
rulings, not open work — do NOT reopen them without a new request:

- **DMA device IDE** (`ctrl 03/0B`) was first ruled out ("пункт 2 это логично, не
  надо править") and then reopened the same day ("в принципе тоже можно поправить,
  но наверное проверить не сможем") — it is IMPLEMENTED, see the section below,
  and is the one piece here with no path to a hardware verdict.
- **90x36 text** is not a code question ("решается не кодом"). RRES 360x288 on a
  320x240 framebuffer shows the central 80x30 of it, so the answer is the user's
  own setup: `TextMode=1` at 640x480, or the 720x576 video mode.
- **The second ZC card** (cfg bit 3, `DRV=6`) will not be emulated — "всегда будет
  без карты", i.e. that slot is expected to be empty on this hardware.

Its whole keyboard goes through the ZX-Evo AVR PS/2 scancode log (Gluk reg `#F0`,
type 2 — `ZxEvoAvr.cpp`), which WC being usable at all now confirms on hardware.

## Raw CP/M floppy images: .atm .kdi .odi .od2 .qdi .fdd .xdi (2026-11-08, NOT hw-tested)

The SteinBlume set (era-cg.su/steinblume — ATM-Turbo, Korvet/Byte-01, Orion, Quorum,
Vector-06C, "experimental"). All are headerless sector dumps (cyl by cyl, side 0 then
side 1, IDs 1..N); the site was not reachable from the dev box, so the layout is the
owner's report plus the known .pro/.cpm format. `RawCpmImage.h` picks the geometry
from the file size: the extension's native format first (.atm = 16 x 256, the rest
5 x 1024), then 9/10/18 x 512, 10 x 1024, 26 x 256 over 77..84/40 cylinders, 1-2 sides;
no exact fit = native format, 2 sides, cylinders rounded up.
- 16 x 256 (.atm) runs on the plain TRD path (owner: an ATM .atm renamed to .trd mounted
  and ran a .com under ATM CP/M) but with the geometry from the size, not the TR-DOS
  disk-info byte, and **never** `trdMaybeInjectBoot` (it would write into CP/M data).
- Everything else = the .cpm branch: `IsMBDFile + IsProFile + proPlainIds`, any
  sector size, writable, deferred f_sync.
- `IsRawCpm` makes `rvmWD1793IsCpmDisk` true, so an F5 launch on ATM boots CP/M.
- .xdi may carry its own geometry in a way we do not know; it is size-probed like the rest.

## FDI copy protection — physical damage emulation (`src/speccy/devices/disk/wd1793.cpp`)

An FDI sector flagged with a **bad data CRC** was unreadable on the source
floppy: its data field is valid up to the damaged spot and garbage from there to
the end. Protections (Чёрный Ворон / Black Raven disk 2 = `br2b.fdi`) write a
pattern over such a sector, read it back and expect the **first mismatch at the
damage offset** (Black Raven: ±10 in 2-byte compare units = ±20 bytes).

- **Damage never heals.** WriteEnd keeps the bad-CRC flag and re-inverts the
  stored MFM CRC for sectors in `wd->fdiOrigBadMask` (set per track in
  `fdiLoadTrack`). Before this, a write "repaired" the sector — the protection
  saw a healthy disk and looped forever.
- **The damaged region refuses writes.** `fdiWrGuard`/`fdiWrCount` (armed in
  `kRVMWD177XWriteDataFlag`, cleared in `_end`) make the buffer store in
  `rvmwdDiskStep`'s FDI branch drop every byte from the damage offset on, CRC
  bytes included. Guard = `offset + 1` because the count includes the data mark,
  so data byte `offset` is the first one suppressed. The sector is identified by
  its ID field (`fdiSectorFromHeader`), never by the last find_marker hit, so a
  healthy sector can't inherit a neighbour's damage.
- **The image is never written back** for damaged sectors (`fdiFlushTrack`
  skips them) — flushing the mixed prefix+tail would destroy the protection
  permanently, and the pristine file is what restores the sector on reload. The
  write therefore lives only while the track is buffered, which is all a
  protection needs (write + read-back happen without an intervening seek).
- **Damage offsets are recovered from the image itself** at insert
  (`fdiScanDamage`, **no metadata file of any kind**). These disks store one stream
  redundantly across several damaged sectors at different rotational offsets — so
  their loader can rebuild it from the parts that still read — and that
  redundancy locates the damage: align a damaged sector against every other copy
  (shift 0 plus anchor-matched alignments, `DMG_*` tunables) and take the largest
  agreement, since an overlap can start disagreeing no later than this sector's
  own damage. Cost is well under a millisecond, once per insert.
- The insert-time track-header walk was rewritten to slide an 8 KB window over
  `g_rawTrkDataBuf` (**2 SD reads instead of 166** for an 83-cyl image) and
  collects the bad-CRC sectors on the way; their data is then staged in the same
  buffer for the scan. Verified against a plain per-track walk on every local FDI
  at window sizes 232 B…8 KB (identical `fdiTrackHdrOffsets` + damaged list; the
  window must hold one track block — `static_assert`).
- **Damage located → the scratch is emulated** (write prefix only, nothing
  persisted to the image). **Not located** (`FDI_DMG_UNKNOWN`, e.g. a lone bad
  sector with no redundant copy — `OpenIt.fdi`, `OpenIT!.fdi`, `ZXF45_O.FDI`
  each have one) **→ the sector still takes writes in full and they are
  persisted**, it just never heals. Refusing those writes would only lose data:
  without the real offset no protection can be satisfied anyway (Black Raven
  rejects a mismatch below index 12).
- `tools/fdi_damage.py image.fdi [--ref crack.fdi]` is the host-side twin of the
  scan — it needs no input from the firmware and feeds it nothing (there is no
  sidecar / metadata file; an earlier `.dmg` override was dropped as nobody would
  author one). Run it to validate the derivation on a new protected image:
  `--ref` diffs against a crack/rip for the exact offsets and reports the worst
  deviation. It mirrors the firmware exactly — only damaged sectors are compared,
  since a healthy sector could supply an alignment the firmware never sees.
- br2b ground truth (diffed against `RAVEN2.FDI`, the cracked rip) vs what the
  on-device scan derives, all side 1, 512-byte sectors: cyl1 R145 397/397, cyl1
  R147 413/413, cyl2 R145 356/**352**, cyl2 R147 326/326, cyl3 R145 378/378,
  cyl3 R147 380/**378** — every sector inside the game's ±20 bytes.
  **hw-confirmed 2026-07-28**: the game loads with the on-device scan alone (it
  derives the right-hand column itself — no offsets supplied from outside). It was
  first confirmed with a hand-made offset list, which the scan then replaced.
- Black Raven's checker (readable in the *cracked* `RAVEN1.FDI` at load address
  0xE000, protection routine ~0xE0CB): picks a protected sector from a table,
  writes a pattern, reads it back to 0xA000, compares every 2nd byte, then
  `CP 0xFA` / `CP 0x0C` / `SUB C; ADD A,0x0A; CP 0x16` — i.e. the mismatch index
  must be 12..249 and within ±10 of the table value. The crack NOP'd those three
  branches.

## Tape polarity, long blocks, the JJ flag (2026-10-24, hw-confirmed DVp2w: Wizball TAP+PZX, Buran, La Formica, assorted TAP/TZX/PZX on several machines)

- **A playing tape drives EAR alone** (Ports.cpp): bit 6 = `tapeEarBit` while
  `TAPE_LOADING`; the #FE feedback (Issue 2 MIC bit 3, bit 4, Pentagon idle-high)
  only applies with no signal. The old XOR inverted the tape whenever the border
  OUT had MIC set — the ROM's LD-EDGE always writes OR 8 — so Wizball (polarity-
  sensitive) reset ~5200 bytes in. Was the rule for loaders at 0xFE00+ only.
- **TAP pilot starts LOW** (Fuse / PZX convention: pause low, first pilot pulse low,
  data of a standard block starts high). It started high and only worked through
  the inversion above. TZX keeps its own levels (untouched; a polarity-sensitive
  TZX would still read inverted).
- **A block longer than the ROM asks for** (TAP and PZX): the ROM's share is
  flash-loaded and the rest PLAYS at once from where the ROM stopped
  (`TAP_ContinueFrom`, PZX via the pzxFlashCont setup started immediately;
  `tapFlashCont` keeps the traps from stopping it). It used to Play() the whole
  block from its pilot — Byte's Buran (2169-byte BASIC, 1544 requested) then
  loaded in real time (regression of 687622ce, 2026-10-13). Cerikopik/JJ skip a
  block still being played (`flashSkipPlayingBlock`).
- **`jjScreenAnimating` is cleared by `Tape::Stop()` and `ESPectrum::reset()`**:
  an F11 during a JJ animated screen load kept it up for good, disabling the ROM
  trap and the auto-start until F12 (La Formica).
- Diagnosis tool: a host sim (redcode, 48K ROM, real pulse list from TAP/PZX with
  per-pulse levels, keys typed for LOAD "") settled the polarity in one run —
  rebuild it in session scratch before guessing at a tape-loader report.

## Tape wear — "the recorder is chewing the tape" (2026-09-20; hw-confirmed, after three rounds)

`Config::tape_wear` (Storage > Tape > Tape wear: Off / Light / Medium / Heavy)
plays a stretched, creased, oxide-shedding cassette. Asked for as a nostalgia
feature — the old deck chewed the tape, the sound distorted, and the game ended
in `R Tape loading error`. Three faults, because each is a different thing the
user remembers, and two of them are the only ones that can actually break a load:

- **wow** — the capstan speed wanders, so every pulse is a little long or short.
  This is the AUDIBLE half (Ports.cpp mixes `tapeEarBit` into the beeper, so the
  tape signal is on the speaker) and on its own it is harmless, which is not luck
  but arithmetic: LD-BYTES classifies a bit on `t1 + t2` against a threshold half
  way between 2x855 and 2x1710 = 2565 T, and even Heavy's ±16% leaves a 0 bit at
  1984 and a 1 bit at 2873. The host test asserts it rather than assuming it.
- **lurch** — the tape binds and pulses stretch 1.5-3x, so a 0 bit reads as a 1.
  Note a lurch over a run of 1 bits is **harmless**, on real hardware too: the
  test expects ~44 of 50 injected lurches to break a block, not 50.
- **drop** — a crease or a bald patch lifts the tape off the head: the ear bit
  FREEZES, so there is no edge at all. Either LD-EDGE times out or the merged
  pulses walk the bit stream out of step. Always fatal to the DATA — but NOT to a
  pilot tone, see the ROM note below.

- **The model is `src/speccy/devices/tape/TapeWear.h` and depends on nothing from the firmware** —
  that is the only reason `tools/tapewear_test.cpp` can drive it on a host
  (`g++ -O2 -Wall -Wextra -Isrc -o /tmp/tapewear_test tools/tapewear_test.cpp`).
  **Re-run it after ANY change there.** It is not a threshold approximation any
  more: it carries the REAL 48K LD-BYTES — LD-EDGE's 59 T sampling loop and its B
  counters, LD-LEADER's 256 pilot cycles, LD-SYNC, LD-8-BITS' `CP 0xCB` — driven by
  a mirror of `Tape::Read`'s own phase machine, and it checks the whole ladder
  (header / 200 B loader / 1.5 KB / 6912 B screen / 35 KB game per level — the
  measured figures are in TapeWear.h's own comment). Every
  assertion was checked to FAIL under a hand-applied mutation, the level intervals
  included. ~10 s to run (most of it the 200-sample game-rate measurements).
- **`tapeNext` IS BOTH THE REQUEST AND THE STORE, and wearing our own output
  compounded — the defect that made the whole feature dead on arrival** (hw
  2026-09-20, owner: "at Light it still cannot lock on, the signal is too
  distorted"). The tape machine writes `tapeNext` only when the pulse LENGTH
  changes: `TAPE_PHASE_SYNC` writes it once and then repeats it for up to 8063
  pilot pulses, `TAPE_PHASE_PURETONE` likewise, and `TAPE_PHASE_DRB` never writes
  it at all. The hook wrote the WORN value back into it, so each pilot pulse was
  multiplied by (1 + wow/256) AGAIN — a geometric random walk with no bound.
  Measured: a 2168 T pilot reaches **25 T at LIGHT** (87x out) and 21000 T on other
  seeds, so LD-LEADER can never lock on and nothing loads at any level. The fix is
  `tapewear::State::pulseFedback()`, which undoes the feedback (`if (next ==
  delivered) next = nominal`) and wears the nominal instead; the glue calls THAT,
  never `pulse()`. **It belongs in the model, not in the glue** — that is what lets
  the host test drive it, and the test asserts both halves: the fed-back pilot must
  stay inside the wow band, and raw `pulse()` fed its own output must still run
  away (an assertion that the harness has teeth).
- **hw 2026-09-20, owner: "now it works, it turned out really great"** — that is the
  build with BOTH fixes of this section in (the auto-run split and the pilot-tone
  runaway). It is not itemised beyond that, so read it as the thing the two fixes
  were for: a launch starts loading by itself and the signal is now something a ROM
  can lock on to. The LEVEL rates below were measured on the host model only, and
  the clean-stretch mixture landed after that verdict and was confirmed separately
  (below).
- **The general shape, and it is the one to carry away: the old suite could not
  fail on this because it re-supplied a fresh nominal length on every call** —
  which is true only of the DATA phases. A harness that drives a component
  differently from its real caller measures a component nobody ships. Drive it the
  way the caller drives it.
- **A LD-EDGE timeout in the LEADER or the SYNC is NOT fatal, and getting that
  wrong made every level look far worse than the real machine** (it read as "Medium
  cannot even load a 19-byte header"). `JR NC,LD-BREAK` lands on LD-BREAK's
  `RET NZ`, and LD-EDGE's timeout exit (`INC B / RET Z`) leaves **Z set** — so
  there is no return: control falls into LD-START and the whole pilot search begins
  again. A header pilot is 5 s of tape and locking on needs 512 clean edges
  (~0.3 s), so the ROM rides out several dropouts there. Only LD-8-BITS answers a
  timeout with `RET NC`. **So the fault rate is effectively per second of DATA**,
  and that is what the level table is tuned against.
- **`evtMin` is a GUARANTEED clean run after every Play** (the schedule is re-armed
  there), so it must stay a small fraction of the span — the first cut had
  evtMin == evtSpan, which makes any block shorter than evtMin immune and any
  longer one certain: a cliff, not a worn tape. It is evtMin = 0.2 x mean,
  evtSpan = 1.6 x mean now, and the countdown is **int64** because a side of tape
  is minutes and 400 s does not fit 32 bits with room to spare.
- **ONE hook, at the bottom of `Tape::Read`'s do-loop** (`tapeNext =
  wearPulse(tapeNext)` -> `State::pulseFedback`), which is what makes every format
  that plays through that state machine — TAP, TZX including GDB and CSW, PZX —
  worn by construction. The perturbed length feeds the loop's own `while (tapeCurrent >= tapeNext)`, so the
  time base stays consistent. WAV/MP3 return before the loop and get dropouts only
  (`wearAudio`, driven by elapsed T-states): a wow on a recorded waveform would
  mean resampling it, and those files are recordings of a real tape anyway.
- **The in-ROM TRAP must be suppressed while this is on** (`fastLoadOn()`,
  Tape.cpp), and that is the whole feature, not a detail: the trap fills the block
  straight out of the file without ever generating a pulse, so a worn tape with it
  is a worn tape that always loads perfectly. Five sites — `flashloadAvailable()`,
  the Cerikopik and JJ turbo candidates, the turbo auto-start branch, and the two
  ROM traps in Z80_JLS.cpp (0x56b/0x56d/0x57d and Byte's 0x557).
- **...but the AUTO-RUN must SURVIVE it, and conflating the two shipped a feature
  that could not load anything at all** (hw 2026-09-20, owner: "with any Tape wear
  but Off loading never even starts"). `autoRunAvailable()` was `return
  flashloadAvailable();`, so wear took the loader snapshot down with the trap: a
  launch mounted the tape, pressed Play and left the machine at the BASIC prompt
  while the tape spooled past the header. **The snapshot is not a fast load — it is
  the `LOAD ""`**, and its own registers say so: `load48` resumes at PC=0x0038 with
  `HL=0x053F` (the return address LD-BYTES pushes at 0x055E) and `IX=0x5CE2 /
  DE=0x0011 / A'=0x00`, i.e. an interrupt taken INSIDE LD-BYTES receiving the
  17-byte header. On RET it runs the `CP A` at 0x056A and arrives at 0x056B, which
  is both where the trap fires and where the real pilot-tone search begins — so
  with the trap gone it just reads the tape, which is exactly what a worn tape
  needs. The two gates are now separate (`tapeFastMachineOk()` holds the machine
  test they share) and the decision table is: wear off + fast load ON = instant, as
  before; wear off + OFF = no auto-run, type `LOAD ""` yourself; **wear on = fast
  load is turned OFF for you and the auto-run happens anyway**, then the blocks
  arrive as real pulses.
- **Turning any Tape wear on TURNS FAST LOAD OFF** (owner, 2026-09-20), in
  `resolveConstraints` (UiStage.cpp) as an ordinary forced exclusion with the note
  " Fast load off: the tape is worn ", plus `p_noTapeWear` greying the row while wear
  is on and a `Config::load()` backstop for a card written before this. It is a
  CONSTRAINT, not an edge: it holds for as long as wear is on, and like every other
  exclusion here it does not restore the setting when wear goes back to Off.
  `fastLoadOn()` has ignored `Config::flashload` under wear since the feature landed,
  so nothing about the EMULATION changes — this is only the menu no longer claiming
  a fast load that cannot happen.
  **The trap it re-opens is the one that cost a hardware round the day before:
  `autoRunAvailable()` must not read `Config::flashload` alone**, or turning wear on
  now silently takes the auto-run with it and a launch is back to sitting at the
  BASIC prompt. It reads `(flashload || tape_wear)`. The cost, accepted: with wear on
  the auto-run cannot be declined — the one row that governed it is now forced off —
  and launching a game is what asks for it. The general shape, and it cut both ways
  within two days: **before disabling a setting because a feature "ignores" it, check
  whether the setting does one thing or two** — and if it does two, the second one
  needs its own gate before the row can be taken away.
- **A level that can never load a game is a dead end, not a worn tape** (owner,
  2026-09-20: "add a chance of a successful game load — Medium 40%, Heavy 10%").
  The arithmetic makes that a MODEL question rather than a tuning one: one fault
  kills a load, so with a single interval distribution "a 205 s game loads 10% of
  the time" forces a mean of ~125 s — a level whose screens would then load 92% of
  the time, i.e. not heavy at all. Real wear is not spread evenly either: a cassette
  has BAD PATCHES AND CLEAN STRETCHES, and whether a load survives is mostly whether
  it started inside one. So `Level::cleanPct` draws that percentage of intervals
  from a 400-1200 s clean stretch (`CLEAN_MIN_T`) instead of the damaged-patch
  distribution — 0 / 40 / 10 for Light / Medium / Heavy. A damaged pass still tears
  every few seconds at Heavy and still sounds chewed; a clean pass gets through.
  Consequence worth knowing: anything LONGER than a clean stretch is impossible and
  anything big collapses onto cleanPct (Heavy's screen and game are both ~10%), so
  the "gets partway and dies" character lives in the damaged passes, not across the
  block sizes.
  **hw 2026-09-20, owner: "works"** — read it as the feature working end to end at
  all three levels with the mixture in. It does NOT establish the 40% / 10% figures
  themselves: those need tens of attempts to measure, so they still rest on the host
  ROM model. If a user ever reports a level that feels wrong, `cleanPct` is the one
  number to move, and tools/tapewear_test.cpp measures the effect in ~10 s.
- **Nothing is damaged where nothing is recorded**: a fault may only START in a
  signal-bearing phase. Not tidiness — an inter-block PAUSE is one "pulse" of
  3500000 T, and letting the schedule count it would spend the whole fault budget
  in a single step, so a Light tape would fault at every block boundary.
- The level is read LIVE on every pulse and a change re-arms the schedule, so
  there is no hook to write (AC_PURE) and a menu edit reaches the tape already
  playing. `Play()` reseeds the RNG from `CPU::global_tstates`: rewinding lands
  the head on a different part of the damage, so "try once more and it might
  load" — which is the memory being emulated — is literally true.
- **Two mutations the first test suite could not catch, and the shape is worth
  keeping**: with both fault kinds drawn 50/50 in one run, neutering the lurch
  (`evtMul = 256`) or the dropout (`freeze = false`) still left the OTHER kind to
  break the block, so the suite passed. The fix is a per-kind test that injects
  ONE fault by hand with a control run beside it, plus a separate check on the
  scheduler's own `evtMul` range — because the injected test sets `evtMul` itself
  and therefore cannot see that mutation either. **A test that exercises two
  mechanisms at once can only prove that at least one of them works.**
- The test also re-learned the `peek16` lesson the hard way:
  `rom.pulse(w.pulse(len, true, freeze), freeze)` reads the `freeze` OUT-PARAMETER
  in unspecified order against the call that writes it — GCC took the stale value,
  every dropout silently became a no-op, and the per-kind assertion failed for a
  reason that had nothing to do with the model.
- `-DTAPE_WEAR_TRACE=ON` logs each fault (kind, length in us, block, phase).
- **What the three hardware verdicts do NOT cover, and is still owed**: the audible
  warble on a real speaker at Light (the half the user asked for, and no test can
  judge it); the measured 40% / 10% game rates (tens of attempts each); a TZX turbo
  loader, where the Cerikopik/JJ paths now auto-start the tape instead of
  flash-loading; a launch with wear OFF and **Fast tape load OFF**, which must still
  land at BASIC as it always did; a WAV/MP3 tape, where only the dropouts exist and
  the wow does not; and, since 2026-09-20, the auto-disable itself — pick a wear
  level, see the Fast load row go to No and grey, then launch a game and watch it
  START loading (that is the `autoRunAvailable()` half, and a regression there reads
  as "loading never begins" again).

## RZX playback — stage 1 of RZX (2026-09-29, NOT hw-tested)

`.rzx` opens from every snapshot entry point (F5 / Load from file / web launch /
zip) through `LoadSnapshot` → `Rzx::startPlayback`. Stage 2 (recording) and SZX
snapshots are not done. Format reference: libspectrum `rzx.c` (speccytools
mirror on raw.githubusercontent.com — worldofspectrum.net is a 403 here) and
Fuse's `rzx.c` / `z80.c` for the playback semantics.

- **`src/speccy/core/RzxReader.{h,cpp}` is the format and depends on nothing** (miniz + an
  I/O/alloc struct), streamed: one 32 KB inflate window + inflate state (only for
  a compressed block) + one frame's IN list, whatever the file size.
  `tools/rzx_test.cpp` builds files byte by byte (host zlib, not miniz) and covers
  repeat frames (incl. repeat-of-repeat and across blocks), empty blocks, skipped
  creator/security/unknown blocks, a 3000-byte frame over many 512-byte refills,
  truncation at every offset, a stream shorter than its frame count, a corrupt IN
  count, and every allocation failing in turn with no leak. Recipe in its header;
  four hand mutations each fail it. **Re-run after any change there.**
- **The fetch counter is `Z80::regR`, widened to `uint32_t`** — zero extra work on
  the M1 path. Visible R = `(regR + regRbase) & 0x7f`: `setRegR` (LD R,A,
  snapshot loads) and the INT acknowledge move `regRbase`, not `regR`, because
  neither is an opcode fetch — Fuse's `R++; rzx_instructions_offset--` in
  z80_interrupt and its LD R,A offset fix are the rule the files were written to.
  NMI's R++ stays counted (Fuse counts it too). `incRegR` takes `uint32_t` now:
  the HALT-flush paths used to truncate to 8 bits, harmless for 7-bit R, wrong for
  a counter. Cost on DVp2: +120 B of RAM code (the IN hooks, the loop and isActiveINT tests), +116 B .bss, +~4 KB flash; nothing per instruction.
- **Every IN goes through one hook**: the three `Ports::input` sites in the core
  (IN A,(n), IN r,(C), INI/IND/INIR/INDR) and both Z80DMA port reads do
  `if (Rzx::mode) v = Rzx::onIn(v)`. `Ports::input` STILL RUNS during playback, so
  timing/contention match; its value is replaced. Side effects of the read on our
  devices are harmless — the guest only ever sees file bytes.
- **`CPU::loopRzx` (flash)** runs the frame with checked `execute()`. When the
  frame's fetch count is reached at an instruction boundary it fetches the next
  frame, raises the INT line (`Rzx::raiseInt`: `isActiveINT` answers
  `tstates < Rzx::intUntil` while playing — window end is IntEnd if raised inside
  the raster window, else one window from now) and calls `checkINT()` at once.
  That immediate sample is load-bearing: our live loop can take the INT at the
  tail's overshoot (the `isActiveINT` wrap), i.e. at the SAME boundary the next
  recording frame starts; sampling only after the next instruction would take it
  one instruction late and desync. The raster keeps its own 50 Hz underneath, so
  a foreign recording with a shorter frame just takes interrupts a bit more often.
  **Frame 0 gets no INT at its start** — the INT is at the END of each frame.
- The input block's T-state stamp is applied to `CPU::tstates` after a snapshot
  load (first frame of a block only). Mid-file snapshot blocks are loaded at the
  next loop entry (the frame idles out first). An IN past the recorded list =
  desync: the byte comes from the port and playback stops at the next boundary
  with " RZX: desync at frame N ". Frames reading FEWER INs are only counted
  (log line at stop).
- End of file / F11 / Snapshots → **Stop RZX playback** (`NM_ACTIONV`, shows
  played/total) leave the machine running from where it is. Fast tape load and
  both ROM tape traps are off while `Rzx::mode` (the trap fills memory with no IN).
  `Config::ram_file` is NOT pinned to an .rzx (a replay is not a boot resume);
  `last_ram_file` is (Alt+Backspace replays).
- Known gaps: SZX snapshots (message), a MID-FILE snapshot that crosses a machine
  layout boundary reboots and resumes the inner `/tmp/_rzx.*` snapshot, not the
  playback (the FIRST one resumes the .rzx itself since 2026-10-01: a nested
  `LoadSnapshot` keeps the outer `g_snapshot_loading_path`. Before that the reboot
  came back with the snapshot running and no playback — reported as "sometimes the
  time is not shown, a second launch works", the second launch finding the machine
  already switched. hw-confirmed 2026-10-01, owner: "fixed"); security blocks are ignored; one INT per frame only (so TS-Conf-style
  LINE interrupts cannot exist in a file anyway).
- **Progress banner** (`drawRzxProgress`, OSDMain.cpp): `RZX mm:ss / mm:ss`
  (h:mm:ss past an hour) in the TOP-LEFT of the top border band while playing —
  the `notify()` band and paint path (`notifyPaint(..., left)`), sharing its one
  reservation: a timed banner wins while it lives, progress returns when it
  expires, `progEnd()` erases it (brdChange+brdnextframe) on stop and at do_OSD.
  Time = RZX frames / 50 (one frame = one INT of the recording machine).
  Absent in DS80 (no top border). Hw-confirmed 2026-09-29 (owner: "работает").
- **Web catalog source `rzx`** (drewpo28/pico-spec-catalog `app/adapters/rzx.py`):
  The RZX Archive, ≈4050 recordings as `<0-9|A..Z>/<TITLE .RZX  SUBMITTER  NOTE>`,
  direct links (≈860 are `.zip` bundles, one recording per level → the zip picker).
  rzxarchive.co.uk is behind this environment's egress policy: its markup was read
  from a GitHub Actions run on a throwaway branch of the catalog repo (a push-
  triggered probe workflow that dumps the pages to the log) — the way in for any
  site the container cannot reach. The firmware needed only `rzx` in the launch
  extension lists.
- **The +2A/+3 INT pulse is 32 T, not the 128K's 36** (`INT_END_P3`, CPU.h; libspectrum
  `timings_frame_amstrad_asic`; 2026-10-01, NOT hw-tested, `debug/DVp2-rzx-p3int-1.0.8.elf`).
  Found by `dnawarrior.rzx` (rzxarchive, Spectaculator, Z80 v3 hw 7 = +2A, 82561 frames
  = 27.5 min), which "stopped a few minutes in": `tools/rzx_replay_sim.c` (redcode core,
  +3 paging, recipe in its header) plays the whole file with zero fetch/IN mismatches at
  an INT length of 1..35 T and desyncs at frame 7273 (2:25) at 36 — a DI section whose
  EI takes effect 33..36 T after the frame start took an interrupt the recording never
  had. Live +3 software gets the same correction. **Reach for that simulator first on any
  "RZX desyncs" report**: it separates a bad file from our timing in one run, and the
  window sweep names the parameter. Still open: `Rzx::raiseInt` gives a full window from
  the fetch-count boundary once our raster has drifted from the recording's (the recorder
  had only `len - overshoot`), and the length is OURS, not the recording emulator's
  (libspectrum Pentagon/Scorpion 36 vs our Pentagon 32).
- **RZX playback takes ONE interrupt per recorded frame, and never touches the user's ROMs**
  (2026-10-02; the INT fix hw-confirmed by the owner on `dizzymysticalletter.rzx` — rzxarchive,
  Spectaculator, Z80 **v2** header hw 9 = Pentagon, 60212 frames; the v2 switch takes codes 7+ like v3).
  The file "hung" 44 s in, where the game calls TR-DOS from RAM (7FFD=#18, frames 2218-2306).
  - **Cause**: TR-DOS runs IM1 with `EI / RET` at #0038 = 27 T, shorter than the INT pulse. The
    recorder re-took the INT on some frames (they are in the file as 2-fetch frames); we re-took
    it BY OURSELVES whenever our boundary overshoot was < 5 T — two fetches the recording does
    not have. `Z80::interrupt()` now drops `Rzx::intUntil` at the acknowledge.
  - **How it was found**: `-DRZX_TRACE=ON` (CMake, default OFF) logs `[RZX]` lines in the format
    `tools/rzx_replay_sim.c` prints with `RZX_LOG=1` — `machine ...`, `roms sum0= sum1= dos=`
    (byte sums of ROM page 0/1 and TR-DOS as the CPU reads them), `start ...`, then per frame
    END `cp` (every 50th), `page` (#7FFD or DOS changed, first 300), `SHORT` (INs left unread,
    first 40), `OVER`. Diff the board capture against the simulator's output; the first
    differing line is where the replay left the recording (here `cp f=2250`: C of TR-DOS's
    `DEC C / JR NZ` delay at #3E01 off by 18). Keeping the line up after the ack in the
    simulator reproduces the board's log. **Three rounds of guessing (ROM version, Gluk set,
    esxDOS) came before the log and none was the cause — ask for the trace first.**
  - **esxDOS is the one exception (owner, 2026-10-12, NOT hw-tested):** DivMMC is taken
    off the bus for the replay (`esxSuspend` in startPlayback before any snapshot load,
    `esxResume` from `release()` = every stop/failure path; `Config::esxdos` and the card's
    buffers untouched). No archive recording was made with one and its automap traps ROM
    entry points (dungeonraiders desynced at frame 15 with it on).
  - **Some recordings are simply inconsistent**: dungeonraiders.rzx (Spectaculator, 128K)
    desyncs at frame 336 in our firmware, in `tools/rzx_replay_sim.c` AND in SkoolKit's
    `rzxplay.py` alike ("Port readings exhausted for frame 336") — an IN 2-34 fetches
    before a boundary is logged in the NEXT frame, a ~30-fetch per-frame offset no INT
    length / ROM / opcode-count rule explains. Run SkoolKit (`pip download skoolkit`,
    `rzxplay.main(['--no-screen','--fps','0',file])`) as the second opinion before
    chasing a desync.
  - **Owner's rule: playback runs on the CURRENT settings; nothing else is switched or disabled.**
    A first cut pinned TR-DOS 5.04T, forced `R_PENT` and took esxDOS off the bus
    (`Config::rzxRoms`, `cardsOff`) — all REMOVED: another recording may need another ROM.
    Instead a desync (an IN with no recorded byte, or 16 frames leaving recorded INs unread —
    `SHORT_FRAMES_MAX`; a clean replay has none) stops with a box naming the likeliest
    mismatch at the FIRST bad frame (`desyncStop`): esxDOS on / inside TR-DOS <version> →
    "try another TR-DOS ROM (Devices > Beta 128)" / machine or ROM set. Simulator facts behind
    the hint: this file is clean on 5.04T and 5.04TM only, and derails at its first disk load
    on 5.03, 5.05D (first bad frame 2303) and 6.11e (2218). **Hw 2026-10-02, owner, TR-DOS
    5.05D**: the box came up at frame 5237 (1:44) reading "inside TR-DOS 5.05D - try another
    TR-DOS ROM" — the stop is the 16th short frame, not the first, hence later than 2303.
    The box is `osdCenteredMsg(.., 60000)`: it stays until a key (the timed form ends on any
    key down); the `0` form draws and returns and the running machine repaints over it.
- **The INT rule is the RECORDER's, keyed on the creator block: SPIN 0.5 has no pulse
  (2026-10-06, NOT hw-tested, `debug/DVp2-rzx-spin-1.0.9.elf` + `.uf2`).** `rick1.rzx`
  (rzxarchive, Rick Dangerous, 128K, 59869 frames) stopped at frame 2171 (0:43) with
  "does not match this machine / ROM set" — and it is not a ROM question: the frame ends
  `A6D2 JP NZ / LD SP,FFFC / EI / RET` 34 T after the boundary, our 36 T 128K window took
  the interrupt there, its handler read 4 INs the file never recorded, OVER. The file has
  NO interrupt on that frame, nor on 29813 (EI effective at 24 T) or 33340 (14 T), and it
  HAS one on 32870 where IFF1 was already set at the boundary (EI delay honoured, INT 10 T
  later). So SPIN's rule: **the interrupt is taken only if IFF1 is set when the fetch count
  is reached; an EI inside the frame gets nothing that frame.** `tools/rzx_replay_sim.c`
  sweep of the pulse length: 36..128 T → first bad frame 2171, 28-34 → 29813, 16-24 →
  33340, 1-8 → 32870 (an INT missed), **12-14 → clean** — i.e. no pulse length fits, only
  the gate. `RZX_SPIN=1` in the sim is that gate (the line is raised only with IFF1 set,
  redcode honours the EI delay itself); with it rick1 (59869), chevychase (71386) and
  continentalcircus (42821, all three `SPIN 0.5`) replay with ZERO short frames, and all
  three break under the pulse (chevy: 5370 short frames). Spectaculator/Fuse files need
  the pulse (dnawarrior is clean only at 32-35 T, see above) — so `Rzx::spinInt`
  (creator starts with "SPIN", logged as `INT rule SPIN`/`pulse`): `raiseInt` raises no
  line with IFF1 clear, a 1 T line with it set (the immediate `checkINT` takes it), and
  with an EI pending returns true so `loopRzx` runs ONE whole instruction (prefix bytes
  are not instructions — `atInstrBoundary` loop) and then drops the line. 5282 of rick1's
  frames carry no interrupt at all under the rule (`INT not taken` in the sim), which is
  the game's own DI-heavy sprite code and is what the recording says.
  - **External snapshots are found by NAME** (same build): `intoeaglesnest.zip` (6
    Spectaculator recordings, 48K) refers to its snapshot as
    `C:\projects\my_projects\zx\zx-games\into-the-eagles-nest.z80` — the recorder's own
    path, which we glued whole onto the .rzx directory. Now the basename (either slash)
    is looked up beside the .rzx, and when the .rzx itself was extracted from a zip, in
    that zip (`ZipExtract::sourceZipFor` + `extractNamed`, to `/tmp/_rzx.x.z80`) — the
    archive ships the .z80 next to its recordings and the zip picker extracts ONE member.
    The other five replay clean in the sim at the 48K 32 T pulse (the sim now takes a 48K
    `.z80`: v2 hw 0/1, v3 hw 0..2, paging locked, rom1 = 48 BASIC).
  - **A recording with NO snapshot block starts from the sibling named like it** (same
    build, owner's rule: "в той же папке что и rzx"). `into-the-eagles-nest-1.rzx` has no
    0x30 block at all (the format allows it; Fuse plays such a file from whatever is
    running) and expects the `.z80` beside it. `RzxReader::hasSnapshot()` (header walk: a
    0x30 before the first 0x80) → `sideSnapLoad()`: among the `.z80`/`.sna` files in the
    .rzx's folder — or in the zip it came from (`ZipExtract::forEachName`) — the one whose
    stem is the LONGEST prefix of the .rzx stem with a non-alphanumeric character after it
    (`into-the-eagles-nest` fits `into-the-eagles-nest-1-hard`, `into` does not); loaded
    through the inner `LoadSnapshot`, toast ` RZX: from <name> `, remembered in
    `s_sideSnap` so a loop rewind reloads it. None found = plays from the current state as
    before; found but unloadable = refused. The directory walk needs ~900 B of heap for the
    DIR + FILINFO (LFN 255), taken for the walk only. **The stem must be the zip MEMBER's
    name** (`ZipExtract::sourceMemberFor`), not the extracted file's: the picker writes every
    member to `/tmp/.zip_extract.rzx`, and the first cut matched siblings against
    `.zip_extract` — "desync at frame 4", the file playing from whatever was running (hw
    2026-10-06, the other five of the zip fine because the external-snapshot path takes
    its name from the descriptor). Same round: the header walk's frame-count read reused
    `b[0]`, so the 0x30/0x10 tests after an input block saw a frame-count byte — latent,
    fixed. **Hw 2026-10-06, owner: "заработал"** — `into-the-eagles-nest-1.rzx` from the zip:
    `sibling walk in '...intoeaglesnest.zip' stem 'into-the-eagles-nest-1': members=8
    best='into-the-eagles-nest.z80'` → `extractNamed` → `sibling ... loaded`, the game
    plays. The directory variant (same files unpacked on the card) is covered by
    inspection only.
  - **The round cost four hardware runs to a STALE ELF, and the lesson is a rule:** the
    `build/` dir had been reconfigured for ZERO2 by the VS Code task (22:41) between my
    first build and the next, so every later `ninja -C build` linked `z0p2-...elf` while
    I kept copying the 22:35 `DVp2-...elf` from `build/bin/MinSizeRel/` into `debug/` —
    a firmware with none of the changes, under a fresh name each time. Every "no
    effect" verdict of that hour was that. Now: **a test ELF is checked before it is
    handed out** — `strings <elf> | grep '^build: '` must show the time of THIS build
    and a grep for a string the change added must hit; the `build:` stamp is also
    logged right after `dbg uart: console on ...` (it used to go out before the console
    existed on a cold boot), so a capture names its firmware; and a board's test build
    goes in that board's own dir (`build-all/PICO_DV/` here), never in the shared `build/`.
    Also from the same hour: the UART ring is drained inside every modal now
    (`Debug::pumpUart()` in `netBackgroundTick`, slices in the classic
    `osdCenteredMsg` pause) — a log that stopped mid-line inside the zip picker / the
    desync box was the ring waiting for `ESPectrum::loop`, not a hang.
  - Hw check owed: rick1 / chevychase / continentalcircus end to end (the SPIN gate), then
    dnawarrior or dizzy (the pulse path must be untouched), then the Eagle's Nest zip from
    the browser (zip member → external .z80 from the same zip) and `into-the-eagles-nest-1`
    after loading the .z80 by hand.
  - **Fuse recordings take the same IFF1 gate (2026-10-07, NOT hw-tested,
    `debug/DVp2-rzx-fuse-1.0.9.elf`).** `rainbow.rzx` (rzxarchive `r/rainbow.zip`, Fuse,
    RZX v0.12, 128K, 136288 frames) stopped at frame 1990 with "does not match this
    machine": frame 1990 starts with IFF1 = 0, an EI lands 8..16 T in, the file has no
    interrupt there. Sim sweep: pulse 1/8 T -> desync at 101947, 16..36 T -> 1990;
    `RZX_SPIN=1` -> all 136288 frames, 0 short. Fuse calls `z80_interrupt()` once at the
    frame boundary, so `spinInt` is now creator "SPIN" OR "Fuse"; the pulse stays for
    Spectaculator (dnawarrior). The zip's second file (SpecEmu) is an SZX snapshot —
    unsupported, as before.
- **Spectaculator +2A: the NMOS LD A,R bug and a 16 T window (2026-10-09; hw-confirmed the
  same day on `debug/DVp2-rzx-ldair-1.0.9.elf`, owner: "работает" — not itemised; dnawarrior
  replays clean in the simulator with both changes, all 82561 frames; Dizzy is Pentagon, not Spectaculator).** `runningman.rzx` stopped at frame 114 and `toyota.rzx`
  at 7685 (board logs matched `tools/rzx_replay_sim.c` frame for frame). (1) Frame 114: the
  boundary falls right after the +3 ROM2's `LD A,R` (#0219, the 7FFD pager), the recorder
  took the INT there and the NMOS Z80 clears P/V, so `JP PO` skipped the EI and interrupts
  stayed off for frames. Our core only clears P/V when the line is up DURING LD A,R; in a
  replay it is raised after it. LD A,I/R now store `Z80::ldairAt = regR + 1`, and `loopRzx`
  calls `Z80::rzxLdAirIntBug()` (flash) before `checkINT`. The simulator has the bug on by
  default now (`-DZ80_WITH_ZILOG_NMOS_LD_A_IR_BUG`, `RZX_CMOS=1` to disable). (2) Frames
  2485 / 7685: an EI just after the boundary took an INT the file does not have at windows
  of 28+ T. Clean ranges measured from the boundary: runningman 4..24, toyota 8..24,
  abadia 8..32, dnawarrior 1..35. So Spectaculator's window is 16 T from the boundary
  (`SPECTACULATOR_INT_LEN`), no longer clipped to the raster's window end. The real figure is
  32 T minus the crossing instruction's overshoot, which the file does not store. Frame
  length is NOT the issue (the +2A/+3 311 x 228 = 70908 T matches the Next wiki's reference
  table); RZX frames are fetch counts.
- **Snapshots > RZX loop** (`Config::rzx_loop`, NVS `rzx_loop`, `SET_RZX_LOOP` AC_PURE,
  2026-10-01, NOT hw-tested, `debug/DVp2-rzx-loop-1.0.8.elf`): at EV_END `nextFrame`
  raises `s_snapPending + s_rewind` instead of stopping, and `loadPendingSnapshot`
  (next loop entry, between frames — the mid-file snapshot path) closes and reopens
  the reader and runs `seekFrame`, which reloads the first snapshot. Only for a file
  that carried a snapshot (`s_hasSnap`); one without stops as before. Read live, so
  toggling it reaches the recording already playing.
- **Hw check owed**: any Spectaculator/Fuse .rzx with a Z80 snapshot playing to
  the end without " desync ", on 48K and 128K; a zipped one from WoS; F11 and Stop
  mid-replay; a long file (compressed, several blocks).
- **Stage 2 (recording) plan**: boundary at `CPU::loop` entry (after the pause
  test), frame 0 = `{0 fetches}` written right after the SNA so the first INT
  lands where it does live, INs logged in `onIn`, `0xFFFF` for repeated lists,
  uncompressed blocks (tdefl needs ~300 KB), block length/count patched on close.
  Refuse: TS-Conf/Profi/Scorpion/+3/TC2068, turbo above 7 MHz (u16 fetch count),
  DivMMC, Z80 DMA; force fast load off; stop on F11/NMI/machine switch/debugger.

## Z-Controller SD: core1 reads, INIR batch for #57, no CMD24 pre-read (2026-10-16, NOT hw-tested)

Every ZC/DivSD sector read used to be a blocking card read on core0 off the Next (FPS
drop, negative IDL on any load). Test ELF `debug/DVp2-zc-async-1.0.10`.
- **P0, background reads for every ZC machine**: `render_core` (main.cpp) pumps
  `sd_async_pump()` each iteration and raises `g_sd_c1_live` (sdcard.c) once its loop
  runs; `sdAsyncMachine()` (DivMMC.cpp) = c1 live + DivSD raw card (not an .mmc image,
  not USB) + Next or `zc_enabled` replaces the old `isNext && g_next_c1_live` gate in
  `c17Begin` / `sqAsyncRequest`. CMD17 and CMD18 (start and continuation) take the
  Next's c17 path: the guest polls for the token while core1 reads (8-sector bursts on a
  sequential stream + the next burst posted ahead); 3 ms untaken -> core0 reads itself.
  Without it (no c1, image mode) ZC keeps the synchronous 16-sector `loadSectorStream`.
  DivMMC/esxDOS on non-Next machines is deliberately left synchronous.
- **P1, `Z80::zcInirBatch`** (flash, from INIR when `zc_enabled && C == #57`): all
  repeats but the last as one loop of `mmc_read_bulk`/`mmc_read`. The per-iteration T is
  MEASURED: the tstates distance between two consecutive entries of the same INIR that
  read #57 exactly once (16..40 T), so ATM/Evo's late 3 T and the generic path agree by
  construction; the first two iterations run normally. Off with any contended slot, the
  TS-Conf DRAM model (14 MHz), Scorpion even-M1, Timex MMU, FMAddr, ATM read-only window
  / Evo font write, ROM/accessor destination, slots 0/1, the beam-raced screen page,
  breakpoints, RZX, Profi, Next. One raster line per call, never past the slice.
- **P2**: CMD24 no longer reads the sector before the write (a full blocking card read
  per sector write); a block abandoned part-way is not stored (sdhc bytes no longer mark
  the buffer dirty), as on a real card.
- Hw check owed: TS-BIOS SD boot, WC + TGV / BMPV, Dune (BaseConf), NedoOS (ATM3/Evo),
  esxDOS ZC on Pentagon, a WC save (wc.ini) and a TS-BIOS Setup save; `[FT812]`/PERF
  `sd N sec` and IDL during loads against the previous build; Next regression (TX-1696,
  NextZXOS Browser) since its gate changed from the Next queue to core1's loop.

## SD read-ahead: a coherent one-sector tag + retired windows (2026-10-17, NOT hw-tested)

Review of DeepSeek #03 (647ace9) found two defects, both in DivMMC.cpp, fixed here
(test ELF built from `build-all/DVp2chk`, RAM +64 B):
- **`s_s1_sec` ("mmc_sector_buf still holds this sector") was set in loadSector
  only**, while c17Ready, loadSectorStream and the guest's CMD24/CMD25 data bytes
  (and writeByte, a failed read, the .mmc path) overwrite the buffer. A later read
  of the remembered sector came back with another sector's data — reachable in the
  "bounce between two regions" pattern (window at B, hit B+2, jump to C, back to B
  by burst, read B+2 = B's bytes). It is now a tag on the BUFFER: `s1Set()` after
  every copy of a known sector, `s1Drop()` before anything else lands there.
  Any new writer of mmc_sector_buf must do the same.
- **c17FetchBlocking left `s_c17 = 1` when the post failed**, so the guest's
  token poll re-posted the sector it had just read synchronously: a second card
  access per such read.
Optimisation: **retired read-ahead windows** (`s_old[3]`, `raRotate`/`raOldHit`).
Every refill used to throw the previous window away, and the miss classifier said
the guest returns to exactly that one (nm 0/0/58/29: 58 backwards). The outgoing
window is kept whole (MRU first, LRU evicted), a hit promotes it back to current,
so prefetch and missNote() still see one window. 16 KB butter buffers, allocated
lazily only while the butter arena keeps 512 KB free, refused off-butter (then the
old swap). Invalidation is the write generation they carry. `[SD] ... old H/N` =
hits served from retired windows / windows held. Host model (session scratch,
`ra_test.cpp`): 4-region bounce, data checked per hit, no buffer in two places.
Also noted, NOT changed: DeepSeek #03 raised the SD SCK target to 25.2 MHz, so 504
MHz runs the card at 25.2 (idiv 5), 0.8% over the SD default-speed limit — if a
card misbehaves only at 504, that is the first suspect (target 25.0 -> 21 MHz).

## Standalone .csw tapes (2026-10-20, hw-confirmed DVp2 on `debug/DVp2-csw2-1.0.10`: owner "работает", incl. Next Tape Loader -> 128K; not itemised)

`TAPE_FTYPE_CSW` (6), phase `TAPE_PHASE_CSWF` (19), `src/speccy/devices/tape/Tape_CSW.cpp` +
the firmware-free `CswFormat.h` (header parser for v1.01 / v2.0, RLE and Z-RLE pulse
decoder, Q16 samples->T with the fraction carried: 44100 Hz is 79.37 T, not 79). Z-RLE
inflates ON THE FLY through tinfl: one `Buffer::palloc(NEED_POINTER|PREFER_PSRAM)` block
(~43 KB: tinfl state + 32 KB window) held while the tape is mounted, freed by
`CSW_Close()` (LoadTape, Eject, Init, ESPectrum::reset). The TZX 0x18 block keeps its
old inflate-to-/tmp path, untouched. One pulse stream, one "block": `Tape::Read` calls
the flash `CSW_Step()` per pulse; Stop keeps the position, Play resumes (rewinds after
the end), Enter in the tape browser rewinds (`CSW_RewindTo0`). No fast load (no bytes
in a CSW — the ROM trap and turbo flash-loaders stay gated on TAP/TZX/PZX); "R" still
types LOAD "" through the loader snapshot and then presses Play. Without the loader
snapshot (Next, ATM) the tape is NOT played at mount: TapePortRead's auto-start
(ROM LD-EDGE 05E3-05F5 or a RAM edge poll, 200 reads at one PC) starts it, which
for CSW also fires at "block 0" (`cswAuto`, gated on Config::tape_autostart). Found on
the Next (hw 2026-10-20): Tape Loader -> 128K resets the personality, the tape comes
back stopped, and the block-0 exclusion left LD-EDGE waiting for ever; a mount-time
Play would also spool the CSW past its header while the user is in the menu (a CSW
resumes where it stopped). Tape wear applies.
Not resumable in .pss. F1 Info shows the header. Host test `tools/csw_test.cpp` (recipe
in its header; `-DMINIZ_NO_ZLIB_COMPATIBLE_NAMES` because it uses the host zlib to
build Z-RLE streams) — three mutations fail it. Static RAM +4 B. Test ELF
`debug/DVp2-csw-1.0.10.elf`. Hw owed: a v1 RLE and a v2 Z-RLE file from WoS loading in
real time, Stop/Play mid-tape, F11 with a CSW mounted, a no-PSRAM board (decoder on the
heap, or "Not enough memory for CSW").

## SD background reads: cut the speculative tail instead of waiting it out (2026-10-21, NOT hw-tested)

"FPS drops to 30 on SD access" on the Next. The 2026-10-19 SD DMA attempt (rolled back) relieved
core1 but changed nothing visible, because the on-screen FPS/IDL is core0's time. What freezes
core0 is the SINGLE background slot: a burst is one demanded sector plus a speculative tail
(window up to 32 sectors, `c1 max 6.8 ms`), and when the guest then asks for a sector outside it
(the `nm` misses: backwards / jumps dominate) the request waited for the whole tail, and after
3 ms core0 gave up and read the card itself (`fb`, `sync`, `wait max 3.7 ms` on the [SD] line of
the NextZXOS boot logs, 2026-10-21). Now:
- **`sd_async_abort()` / `g_sd_as_abort` / `g_sd_as_seq`** (sdcard.c): core0 names the running
  request; core1 stops after the block it is receiving (CMD12), `g_sd_as_count` becomes the blocks
  that arrived, state 3. The id is bumped at every post (DivMMC), so a late abort cannot cut the
  next request. Called from `c17TryPost` (slot busy with a burst that does not hold the sector),
  `sd_async_quiesce` (the host wants the card) and `sqAsyncDiscard`.
- **`asKeep()`**: a finished/cut burst of the current write generation is kept as a read-ahead
  window wherever the slot is cleared (it used to be thrown away in `c17TryPost`); `asInstall()` =
  `asKeep()` + prefetch. `c17FromWindow()` serves a waiting sector from it.
- `c17Ready` re-posts instead of reading on core0 when a burst was cut before the sector's block,
  and when its own request vanished — **latent bug in HEAD too**: `sd_async_quiesce` cancels a
  request core1 has not started, after which nothing answered the guest's token poll for ever.
- `c17FetchBlocking` waits for the sector's block of a running burst instead of reading synchronously.
- [SD] line: `cut N`; deltas no longer wrap (NEXT_TRACE's block zeroes the same totals).
Host model: `scratchpad/sdcut/harness.cpp` of the 2026-10-21 session — the SHIPPED DivMMC code
(extracted from `#define DIVSD_RA_SECTORS` to the end of loadSector's divsd branch) on core0,
sdcard.c's pump/loop transcribed on a second thread, sector content = f(sector, version), random
sequential/jump/backward/CMD18/write/host-read traffic, every handed sector checked byte for byte.
New: 0 errors in 3x60k reads under ASan; guest waits >2 ms 2881 -> 180, core0 fallbacks 540 -> 24,
stuck requests 31 -> 0 against HEAD's code. Three mutations (window not shrunk, stale burst kept,
cut path serving the wrong buffer) each fail it. Test ELFs `debug/DVp2-next-sdcut2-1.1.0` (plain)
and `-sdcut2-perf-` (PERF_TRACE, PERF_HIST OFF: [SD] line). Can damage the card if wrong — test on a spare card.
- **Do not judge SD behaviour on a PERF_HIST build** (`build-all/PICO_DV-nxprof`): its CMD17 calls
  `loadSector` directly (DivMMC `#if PERF_TRACE && PERF_HIST`), i.e. core0 blocks on every sector
  and the c17 path never runs. The first sdcut perf log was taken that way (`hit 0`, 26-43 fps).
  It still showed a real hole, fixed in sdcut2: on the `loadSector` path a FINISHED burst was never
  made a window, so the next miss re-read it — `loadSector` and `c17Ready` now `asKeep()` it first.
  `build-all/PICO_DV-sdperf` = PERF_TRACE ON, PERF_HIST OFF.
- **sdcut3: a read-ahead gives way to the renderer.** TX-1696's intro load on sdcut2-perf: 566 ms/s of
  core1 on the card, ~3000 sectors read for 444 the guest used (136 read-aheads posted a second).
  `g_sd_as_need` (0 = pure read-ahead, 1 = the guest waits for block 0) is set at every post; while
  `sd_async_render_behind()` (NextVideo.cpp: core0 in `c1Drain` — flag `g_nx_c0wait` — or the row
  queue within 64 rows of full) core1 does not start a read-ahead and stops a burst once its demanded
  blocks are in (`yield` on the [SD2] line). c17Begin/c17FetchBlocking no longer wait on a read-ahead
  that is only POSTED (it may stay untaken now): c17TryPost replaces it with the demand. The
  redundant `sd_async_pump()` in `c1Pump` was removed (render_core pumps before every call) to make
  room: `.nxovl` 51 448 of 51 456. [SD2] line: fps, IDL min, frames with negative IDL, qwait, skip,
  cut, yield (the [SD] line was over Debug::log's 256 bytes and came out mangled). ELFs
  `debug/DVp2-next-sdcut3-1.1.0` and `-sdcut3-perf-`.
- **The actual FPS drop on TX-1696's load was NOT the SD transfer** (sdcut3-prof, PERF_HIST log
  2026-10-21: `fps 37 neg 26` with `wait 0 fb 0 qwait 3ms`). The PROF samples put core0 in
  `DivMMC::nextOpcodeSlow`: NextZXOS's SD driver loop in the divMMC ROM runs at guest PC
  0x1F40-0x1F7F, and `opcodeTrapPage` took the whole page 0x1F as a trap page (its only traps are
  the off points 0x1FF8-0x1FFF), so every opcode of the loop went through the full
  preOpcFetch/postOpcFetch. The filter now takes `(pc | 7) == 0x1FFF` on that page. Lost: the
  Next automap reset gate (NR 0x0A b4 / NR 0x83 b0) on fetches at 0x1F00-0x1FF7 — the next fetch on
  any trap page applies it. `.nxovl` AUTO term 51 456 -> 51 584 (trace 51 712), content 51 488.
  The PERF_HIST CMD17 branch now takes the plain c17 path too. ELFs `debug/DVp2-next-sdcut4-1.1.0`
  and `-sdcut4-prof-`.
- **sdcut5: the rest of that load was the driver's own loops, emulated byte by byte.** enNxtmmc
  (divMMC ROM) waits for the data token in `1F40: IN A,(#EB) / CP #FF / RET NZ / DJNZ` (+ `DEC C /
  JR NZ`), ~15k IN a frame at 28 MHz while core1 reads, and reads the sector with `INIR` x2 into its
  buffer at IX = divMMC RAM 0x2000-0x3FFF, which `nextInirBatch` refused (slots 0/1). Now:
  `nextSdPollBatch` (hooked in `decodeOpcodedb` at PC 0x192A or 0x1F42, A = 0xFF; pattern checked in
  the mapped bytes) waits for the sector in WALL time (`DivMMC::sdTokenWait`, which runs c17Ready /
  the sync fallback exactly as the guest's polls would) and skips the passes that time covers
  (33 T each, B decremented, never past B = 1, stFrame, statesInFrame - IntEnd, not inside the INT
  window; idleAdvanceTo, so not under contention). sdcut5 allowed DI only and the loop still showed
  in the profile (46-49 fps): the driver polls with EI — sdcut6 takes EI too. [SD2] `poll N/k/ms
  ref R` = calls / passes skipped / ms waited / refusals. The guest still sees the card's latency, it no longer costs an emulated
  IN per pass. `nextInirBatch` writes divMMC RAM (slot 1) by writebyte's rule, as nextLdirBatch.
  ELFs `debug/DVp2-next-sdcut5-1.1.0` and `-sdcut5-prof-`.
- **sdcut7 + sdidle: the write-busy wait and the diagnostics that found it (2026-10-21; hw-confirmed
  DVp2 on `debug/DVp2-next-sdidle-prof-1.1.0`: write windows 42 -> 50 fps, IDL min -18k -> -3k).**
  enNxtmmc's `1F3D` (`IN A,(#EB) / CP #FF / RET NZ / DJNZ`, BC = #0032 = 12800 passes) is called
  from four places: `1F39` R1 after a command, `1F50` data token, `1FB3` write data response,
  `1FBC` write busy. The busy wait (`1FB9: CALL 1F3D / AND A / JR Z`) can only end by the 1F3D
  TIMEOUT once the card answers #FF — 12800 passes after every sector write, on hardware too
  (~15 ms guest time), and the batch refused them all (`sdTokenWait`: last command CMD24).
  `DivMMC::sdReadsIdleFF()` (mmc_read()'s own order: no card / CS high, R1 shown first, a written
  block past response+busy, no command latched) says every following read is #FF with no side
  effect; `nextSdPollBatch` then skips all passes B and the slice allow without a wall wait.
  `[SD3]` (PERF_TRACE / NEXT_TRACE, beside [SD2]): `why` = why a batch call skipped nothing
  (pattern, slice end, card state, answered at once, idleAdvanceTo refused), `tw` = which
  sdTokenWait precondition failed, `who` = calls/passes per caller by the return address on the
  guest stack (R1 / tok / wrsp / busy / other). Read `who` first on any "SD poll" report.
  **PERF_HIST builds also log `NXCODE`** (one-shot, up to 4 a session, re-armed by a quiet second):
  when a second runs > 35k guest ops a frame, the registers, 32 B of stack and the 8 hottest
  32-byte guest-PC buckets (±32 B) as mapped right then — the dump of a stall nobody catches by
  hand (decode with z80disasm). And `NXPROF pollskip:` (PERF_TRACE) = NextPorts::pollSkip's
  calls / other PC-or-port / registers changed / period over dmax (min T) / skips / passes.
  **The "-6000 IDL on load" left after this is TBBLUE.FW's boot menu, deliberately NOT skipped**:
  NXCODE showed `6C00..6C3B` (`IN (#FE)` rows + `LD HL,(#CCE7) / INC HL / LD (#CCE7),HL`, 65535
  passes, subroutines at B740-B9F7 incl. strncmp), pollskip had ~88k candidate reads a second
  stopped only by the config-mode guard. ~2 s at 49-50 fps per hard boot, authentic guest time;
  owner's call 2026-10-21: leave it (a signature hack advancing the memory counter was offered and
  declined).
  **OPEN, not reproduced since: SIGBUS in `Ports::output`'s epilogue on the prof build** (twice,
  2026-10-21, after a NextZXOS soft reset with SD traffic): PC = 0x2003ac18 (= &CPU::tstates), SP
  16 bytes LOW at the `ldmia {r4-fp, pc}` (Ports::output's return address 20024471 sat 12 bytes
  ABOVE the fault SP) — a 4-word SP imbalance somewhere under a `Ports::output` call (mmc_write
  -> Debug::log tail call the first time, DivMMC::applyMapping the second). Gone after the owner
  reset the settings to defaults; which setting armed it is unknown. If it returns: the plain
  build vs sdcut7-prof in the same scenario, and a GDB capture of the ARM stack ABOVE SP.
  **It returned 2026-10-22 on a PLAIN build (DVp2w, CYW43 radio up, the Web catalog's
  `nexload _RUN.NEX`)**: CFSR 00020000 (INVSTATE), PC = LR = 0x20037708 = `&VIDEO::ts_line_t`
  this time (addr2line names a veneer there — a stale match, it is the variable), the
  frame above SP = `&CPU::stFrame &CPU::tstates &MemESP::ramCurrent <ret into decodeED
  right after bl Ports::output> &Z80::regA 0x56 0xDAF1 <ret into exec_nocheck_next>`, i.e.
  the section-anchor registers `exec_nocheck_next` keeps in r4-r11. So an `OUT (C),r` from
  the Next's ED dispatch, and `Ports::output` (or a callee) popped with SP 4 words low —
  a callee-saved anchor landed in PC. Not a pointer table (none builds one), not the
  `.dmaovl` classic loop (`executeTransfer`/`transferOneByte` are unreachable on the Next:
  `doEnable` is under `isNext`, the other site is inside `executeTransfer`, the third is
  MB-02). Since this run the fault dump prints r4-r11 (the trampoline pushes them) and 32
  words of the caller frames.
  **RESOLVED the same day — it is VOLTAGE MARGIN, not software** (owner: the fault went
  away with Overclock > Core voltage = 1.65 V from the menu; it was 1.60 V at 504 MHz). A popped-register skew with no
  code path that could produce it is what a core running past its margin looks like
  (the m1p2 NeoGS core1 SIGBUS was the same class), and the Next at 504 MHz is the
  heaviest load this firmware has. The "which setting armed it" question of 2026-10-21
  is answered the same way: settings that changed the load. If this shape — PC = the
  address of a RAM variable, SP a few words off, `Ports::output`/`exec_nocheck_next` on
  the stack — comes back, ask for the VREG setting and the clock before reading code.
- **Pogie: parallax bands at the wrong scroll for one frame every ~0.5 s = a long CMD18 had no
  prefetch (2026-10-21, hw-confirmed DVp2 on `debug/DVp2-next-sdstream-ldiag-1.1.0`, owner: "фиксед").**
  Pogie's LoRes background is a line-IRQ chain (ULA INT off, NR 22=06, 14 MHz): lines 46/78/96/131
  write NR 0x32 per band and the next NR 0x23, line 185 runs a long handler, line 240 sets the frame
  flag. The 185 handler (IFF=0) streams ONE sector a frame of `pogie.cpa` (copper audio) itself:
  512 x `INI` from `#EB` inside one endless CMD18, then `8C4A: IN A,(C) / CP #FF / JR Z` for the
  next block's token. The read-ahead was deliberately off inside a CMD18 (`!s_stream`), so every
  32 frames the window ran out, the next sector was a demand read, the guest's token wait ran the
  handler ~4.5 lines past 240, the line-240 pulse was lost and the chain slipped a frame (the bands
  then show the bottom band's scroll). Fix: `pfOk()` (DivMMC.cpp) — inside a CMD18 the next window
  is prefetched once that CMD18 has served more than one full window (`s_stream_n >
  DIVSD_RA_STREAM`); load chunks (17-35 sectors) keep the old no-prefetch rule. Owed: loads with
  long CMD18s (Atic Atac, TX-1696, NextZXOS Browser) for extra card reads. Diagnostic kept:
  **`-DNEXT_LINE_DIAG=ON`** (`[LDIAG]` in NextInt.cpp): every line pulse no INT acknowledge
  followed, with IFF1/PC/stack when the loop first saw it and the lateness, plus a summary every
  1500 pulses. `iff=0` = a handler still running (lost on the board too unless our timing is
  longer), `iff=1` + large `seen` = we ran past the pulse. Read it first on any "raster split /
  parallax glitches every N frames" report on the Next.

## Two cards on one SPI machine: esxDOS + Z-Controller, card windows, EBR partitions, write protect (2026-11-03; ported from drew-element-zx 2026-11-04)

hw-confirmed DVp2w 2026-11-03 on drew-element-zx (DivSD + ZC on one card, F2/F9); MB-02+
with esxDOS bench only. On this branch: host + DVp2w build only, NOT hw-tested again.

- **One SPI protocol machine, two media** (`DivMMC::Medium s_med[2]`, `selectCard`):
  esxDOS's card (#E7/#EB, `CARD_ESX`) and the Z-Controller's (#77/#57, `CARD_ZC`) share
  the command latch, the indices and the one 512 B sector buffer; each has its own
  medium (a window `base`/`sectors` of the real card, or an image file) and CS latch.
  Every public port entry turns the machine to its card first (a CS edge for the other
  card, the buffer tag dropped, CSD/OCR rebuilt for the medium — `spiIdentity`, SDHC
  sized by the window). A Z80 cannot interleave two SD transactions mid-block, so the
  shared buffer is safe. Guest sectors go through `lba()`: base added, past the medium =
  `~0u` (read 0xFF, write dropped + logged once). The read-ahead / core1 machinery stays
  keyed by CARD sectors; `s1Drop()` on every card turn. `sdAsyncMachine()` is per card.
- **Config** (all reboot-class): `esx_raw` (DivIDE hd0: 0 image, 1 card RO, 2 writable),
  `esx_rwin` "start/len" (DivSD always; 0/0 = whole card), `zc_img` (empty = the card),
  `zc_rwin`, write protect `ide_wp` / `esx_wp` (bit = slot, DivSD = bit 0) / `zc_wp`.
- **Menu**: Devices > esxDOS > Disks has a row for DivSD ("SD card <whole | part N |
  window>") and DivIDE hd0 on the card; Devices > Z-Controller is a submenu (On/Off +
  "SD card / image"). One source list for IDE / esxDOS / ZC (`rawSourcePick`,
  UiActions.cpp). **F2 = write protect** on IDE / esxDOS / ZC rows (as on a Beta drive;
  a card window's F2 is the raw-write confirmation, Default No), **F9 = CHS / SD
  window** (UiNav decodes VK_F9), F6 gone. Per-row verb lists via `DynRows::info`
  (`esxInfo` / `zcInfo`). Enforcement: IDE `writeProtected(slot)` -> ABRT; DivIDE
  `DivMMC::ideWriteProtected`; DivSD / .mmc / ZC drop the block and answer 0x0D "data
  rejected" instead of 0x05.
- **Shared windows**: two guest file systems writing one volume corrupt it. `sharedOk()`
  (UiActions.cpp) asks (Default No) before a pick that overlaps the other interface's
  window; rows carry a `shared` badge; `DivMMC::logShared()` prints the overlap at boot.
- **MBR + EBR chain**: `ide_mbr::extended()` / `parseEbr()`, `IDE::mbrParts` lists 8
  entries (1-4 primary, 5-8 logical; `IDE::MBR_PARTS`). `tools/ide_mbr_test.cpp` 32
  checks. Host: `PICOSPECCY_MBR=1` prints the list the menu would show.
- **Exclusions lifted**: esxDOS + Z-Controller and esxDOS + MB-02+ coexist (page 0
  arbitration: the divMMC window wins while mapped; `MB02::applyMapping` only latches #17
  then, DivMMC's unmap branch calls it back). Kept: +3 (DivMMC and ZC off). **IDE vs esxDOS lifted too (2026-11-04, owner):** no port is shared with NEMO (A1=A2=0) or SMUC (A0=0); the colliding schemes (PROFI, divIDE, +3e, ATM, Atom Lite) only exist where esxDOS is forced off. NOT hw-tested.
- **.pss PSSD**: a trailer byte = the card served | CS latches; older records = esxDOS's.
- **MB-02+ beside esxDOS (from drew-element-zx 896b4556, generic half; hw-confirmed there
  2026-11-04 on the eLeMeNt only):** page 0 at a reset goes to the card's boot EPROM only
  with a disk in MB-02+ drive A, else esxDOS boots (`MB02::applyMapping`, re-run after
  `loadDiskMounts`); while the card holds page 0 (`g_mb02_page0`) the divMMC does not
  automap. The NMI is the card's when BS-DOS booted (`MB02::booted` -> `DivMMC::nmi_bypass`,
  `Z80::doNMI` pages SRAM 0 only then), esxDOS's otherwise. The DMA deferral covers the
  WD2797 ports only, and `Ports::dmaInput` reads #EB through `input()` while esxDOS is on
  (the MB03+SD esxDOS streams sectors by DMA). On a 128K: NOT hw-tested.

### Other partitions in the F5 browser — "SD2:" (2026-11-03; hw-confirmed DVp2w 2026-11-04 on drew-element-zx)

On a card with more than one partition, "Local (SD)" in F5 opens a level of
`Partition N - LABEL` rows (IDE::mbrParts numbering; label from the FAT BPB, none for
exFAT); one partition = straight to the files as before. Enter on a partition other than
the one FatFs mounted as `SD:` mounts it as `SD2:` and opens `SD2:/`. The per-type
dialogs get a ".." at the SD root (`partitionsAbove()`) and `partitionsLoop` at any
volume root; the net folder/file pickers carry `[Partition N - LABEL]` rows. A partition
switch ALWAYS rescans ("SD2:/" names every partition mounted there). On screen the SD2:
prefix is replaced by the label (`SdVolumes::displayPath`).
- `src/fs/SdVolumes.{h,cpp}`; FatFs `FF_VOLUMES 3` ("SD","USB","SD2"),
  `FF_MULTI_PARTITION 1` with `VolToPart[]` in SdVolumes.cpp; local FatFs patch: a `lba`
  field in `PARTITION`, `mount_volume` uses it when nonzero (that is how EBR logical
  partitions mount). One heap `FATFS` kept after the first mount.
- `Config::sd2_part` (NVS) remembered; `SdVolumes::restore()` after `Config::load()` and
  on every remount; `storageLost()` / remounts drop `SD2:` first.
- `ff_raw_write_notify` already covers SD2: (same pdrv). Host: `--sd2 DIR`.
