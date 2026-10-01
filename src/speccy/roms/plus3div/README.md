# +3 (divIDE) ROM banks

Garry Lancaster's +3e / IDEDOS ROM built for a **divIDE** card — the `div` build of
`p3eroms`, v1.43, **English** (`diven3e0..3.rom`). This is what reads the **16-bit**
IDEDOS disks (Workbench and friends), because divIDE puts a whole 512-byte sector
through one data port; the +3e's own 8-bit interface wants half-sector images and the
two are not interchangeable. The port map is `src/speccy/devices/storage/DivideIde.h`.

`src/rom{0,1,2,3}.bin` are the four banks as shipped in the archive, CRC32-verified
against its own headers (`7F4A5482`, `5B028C73`, `1645C2DC`, `04448EAA`).

## Regenerating

    python3 tools/rom_pack.py plus3div

The packer measures every bank against the +3 / +3e banks already in flash and writes
`plus3div_roms.{c,h}` — which of them ride an existing array is published as the
`PLUS3DIV_*` macros `Config::requestMachine` binds through, so another revision of the
ROM moves the macros and not the firmware. For this image:

| bank | | cost |
|---|---|---|
| 0 editor | byte-identical to the +3e's bank 0 | 0 (reuses `gb_overlay_plus3e_rom0`) |
| 1 syntax | overlay over the stock +3 bank 1 | 12768 B |
| 2 +3DOS + IDEDOS | overlay over the +3e's raw bank 2 | 6215 B |
| 3 48 BASIC | byte-identical to the +3's bank 3 | 0 (reuses `gb_overlay_plus3_rom3`) |

**18.5 KB of flash in total** — banks 1 and 2 differ from the `sm8` build by exactly
6794 bytes, which is the figure the archive's own Readme quotes for `div`.

## Another language

The archive also carries the Spanish set (`dives3e0..3.rom`) — that is the one the
Workbench author's Fuse guide names. To use it instead, copy those four files over
`src/rom{0,1,2,3}.bin` and re-run the packer; nothing else changes. English is shipped
here to match the +3 and +3e romsets beside it.

## Checking

    g++ -O2 -Wall -Wextra -Isrc -o /tmp/divide tools/divide_ide_test.cpp && /tmp/divide

With `rom2.bin` present that test also scans the driver: it must report **30 port
setups, 8 distinct registers of 8**, every one decoding as Fuse's own table says.
