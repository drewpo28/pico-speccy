set pagination off
set $w = VIDEO::vga.xres
set $h = VIDEO::vga.yres
set $fb0 = VIDEO::vga.frameBuffer[0]
printf "screenshot: %dx%d fb=%p\n", $w, $h, $fb0
# The framebuffer is NOT one block any more (2026-09-10): a thin or fragmented heap
# splits it into 2-8 whole-row chunks, and FB_FORCE_CHUNKS builds always do. A
# dump of fb0..fb0+w*h then holds garbage past the first chunk (hw 2026-09-14:
# a 720x576 capture came out as one clean band over noise). Walk the row pointer
# table instead -- every driver reads rows through it, so this is the picture.
# `dump` creates the file, `append` extends it; each row is exactly $w bytes, so
# the result is the same contiguous w*h image fb2png.py has always expected.
dump binary memory /tmp/picospec_fb.bin $fb0 ($fb0 + $w)
set $y = 1
while $y < $h
  set $row = VIDEO::vga.frameBuffer[$y]
  append binary memory /tmp/picospec_fb.bin $row ($row + $w)
  set $y = $y + 1
end
set logging file /tmp/picospec_dim.txt
set logging overwrite on
set logging redirect on
set logging enabled on
printf "%d %d\n", $w, $h
set logging enabled off
printf "screenshot: dump done\n"

# The HDMI driver keeps an RGB888 shadow of every slot it programmed
# (`static uint32_t palette[256]` in hdmi.c) — 256 x 0x00RRGGBB, exactly the
# layout fb2png.py expects. Dumping THAT instead of a placeholder is the only
# way to see what a runtime palette really put on the wire: on TS-Conf the
# framebuffer holds ts256 slot numbers and the ZX fallback decodes them as
# nonsense (the CLAUDE.md warning that "both screenshot decoders lie in these
# modes"). Decode with `fb2png.py ... --raw-pal` to keep it.
#
# PROBE first: a missing symbol aborts the whole sourced file, and with logging
# redirected the MI error never reaches the extension — the dump then hangs with
# the target paused (the memdump.gdb lesson). `info variables` prints nothing and
# errors on nothing when there is no match, and an untaken `if` body is never
# evaluated. VGA-only / SOFTTV / TFT builds have no such symbol and keep the
# placeholder.
set logging file /tmp/picospec_sympal.txt
set logging overwrite on
set logging redirect on
set logging enabled on
info variables ^palette$
set logging enabled off
set logging redirect off
shell grep -v "regular expression" /tmp/picospec_sympal.txt | grep -q "uint32_t palette\[256\]" && echo 'set $has_hdmipal = 1' > /tmp/picospec_sympal.gdb || echo 'set $has_hdmipal = 0' > /tmp/picospec_sympal.gdb
source /tmp/picospec_sympal.gdb
if $has_hdmipal
  dump binary memory /tmp/picospec_pal.bin &'hdmi.c'::palette[0] &'hdmi.c'::palette[256]
  printf "screenshot: real HDMI palette dumped (decode with --raw-pal)\n"
else
  dump binary memory /tmp/picospec_pal.bin $fb0 ($fb0 + 1024)
  printf "screenshot: palette placeholder dumped (ZX fallback will be used)\n"
end

# Pair-slot mode (Profi DS80, GMX 640x200, Timex hi-res, ATM/ZX-Evo EGA/hires/text):
# one fb byte is TWO pixels and fb2png cannot decode it. Dump what profi2png
# needs and record the mode, so screenshot.sh picks the decoder by itself —
# either keybinding then gives the right picture in either mode. Probed, last:
# profi_ds80_active lives in the video drivers (absent on some builds).
set logging file /tmp/picospec_symmode.txt
set logging overwrite on
set logging redirect on
set logging enabled on
info variables ^profi_ds80_active$
set logging enabled off
set logging redirect off
shell grep -q "profi_ds80_active;" /tmp/picospec_symmode.txt && echo 'set $has_pairflag = 1' > /tmp/picospec_symmode.gdb || echo 'set $has_pairflag = 0' > /tmp/picospec_symmode.gdb
source /tmp/picospec_symmode.gdb
set logging file /tmp/picospec_mode.txt
set logging overwrite on
set logging redirect on
set logging enabled on
if $has_pairflag
  printf "pair %d\n", profi_ds80_active
else
  printf "pair 0\n"
end
set logging enabled off
set logging redirect off
dump binary memory /tmp/picospec_profi_lut.bin &VIDEO::profi_pair_lookup ((char*)(&VIDEO::profi_pair_lookup) + 256)
dump binary memory /tmp/picospec_profi_pal.bin &VIDEO::profi_palette_live ((char*)(&VIDEO::profi_palette_live) + 64)
