set pagination off
# Dump Profi DS80 framebuffer and decoding tables.
# Requires profi_ds80_active == 1 for meaningful content,
# but dumps unconditionally so you can run it any time.
printf "profi_ds80_active: %d\n", profi_ds80_active

set $w = VIDEO::vga.xres
set $h = VIDEO::vga.yres
set $fb0 = VIDEO::vga.frameBuffer[0]
printf "screenshot profi: %dx%d fb=%p\n", $w, $h, $fb0

# Framebuffer: w*h bytes, row-major (320*240 = 76800 B)
# Layout per row: 32-byte black pad | 256-byte content (packed pairs) | 32-byte pad
# The main framebuffer is a ROW-POINTER table (VIDEO::vga.frameBuffer[y]) and
# since 2026-09-10 may be 2..8 whole-row chunks scattered over the heap, so a
# single dump of fb0..fb0+w*h reads garbage past the first chunk (hw 2026-09-14:
# a 720x576 capture came out as the top rows + noise). Dump one row at a time.
set $y = 0
while $y < $h
  if $y == 0
    dump binary memory /tmp/picospec_profi_fb.bin VIDEO::vga.frameBuffer[0] (VIDEO::vga.frameBuffer[0] + $w)
  else
    append binary memory /tmp/picospec_profi_fb.bin VIDEO::vga.frameBuffer[$y] (VIDEO::vga.frameBuffer[$y] + $w)
  end
  set $y = $y + 1
end

# Pair lookup table: uint8_t[16][16], row-major [ink][paper] → DS80 slot index
dump binary memory /tmp/picospec_profi_lut.bin \
    &VIDEO::profi_pair_lookup \
    ((char*)(&VIDEO::profi_pair_lookup) + 256)

# Live palette: uint32_t[16], little-endian 0x00RRGGBB
dump binary memory /tmp/picospec_profi_pal.bin \
    &VIDEO::profi_palette_live \
    ((char*)(&VIDEO::profi_palette_live) + 64)

# Write framebuffer dimensions
set logging file /tmp/picospec_profi_dim.txt
set logging overwrite on
set logging redirect on
set logging enabled on
printf "%d %d\n", $w, $h
set logging enabled off
printf "screenshot profi: dump done\n"

# Record the mode and dump the real HDMI palette as screenshot.gdb does, so
# screenshot.sh --profi can decode a STANDARD-mode frame too (ATM/ZX-Evo in ZX
# mode, any plain machine) instead of misreading its bytes as pairs.
set logging file /tmp/picospec_mode.txt
set logging overwrite on
set logging redirect on
set logging enabled on
printf "pair %d\n", profi_ds80_active
set logging enabled off
set logging redirect off
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
else
  dump binary memory /tmp/picospec_pal.bin VIDEO::vga.frameBuffer[0] (VIDEO::vga.frameBuffer[0] + 1024)
end
