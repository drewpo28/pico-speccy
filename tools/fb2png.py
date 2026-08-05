#!/usr/bin/env python3
"""Convert pico-spec framebuffer dump to PNG.

Usage:
    python3 fb2png.py fb.bin pal.bin WIDTH HEIGHT out.png

Optional check: if a sibling rowptrs.bin exists in /tmp, we report whether the
framebuffer rows are contiguous (i.e. row[k] == row[0] + k*width). If they are
NOT contiguous, the dump-by-(fb0..fb0+w*h) approach will produce garbage; the
script warns so you can switch to per-row dumping.

The framebuffer is 8-bit palette indices, width*height bytes.
HDMI driver reads bytes with `input_buffer[(x++) ^ 2]`, so adjacent
4-byte groups are swapped pairwise. We undo that to get visual order.
The palette is 256 x uint32_t little-endian, format 0x00RRGGBB.
"""
import os
import re
import struct
import sys
from PIL import Image


def check_contiguous(width: int, height: int) -> None:
    path = '/tmp/picospec_rowptrs.bin'
    if not os.path.exists(path):
        return
    data = open(path, 'rb').read()
    if len(data) < height * 4:
        return
    ptrs = [struct.unpack_from('<I', data, i * 4)[0] for i in range(height)]
    base = ptrs[0]
    contig = all(ptrs[k] == base + k * width for k in range(height))
    if contig:
        print(f"rows: contiguous (base=0x{base:08x})")
    else:
        diffs = [ptrs[k] - base - k * width for k in range(min(8, height))]
        print(f"rows: NON-contiguous! base=0x{base:08x}")
        print(f"  first 8 row offsets vs expected: {diffs}")
        print("  WARNING: per-row dump needed; current image will likely be wrong.")


# Standard ZX Spectrum palette for indices 0..15 (BRIGHT 0 then BRIGHT 1).
ZX_PALETTE = [
    (0x00, 0x00, 0x00), (0x00, 0x00, 0xCD), (0xCD, 0x00, 0x00), (0xCD, 0x00, 0xCD),
    (0x00, 0xCD, 0x00), (0x00, 0xCD, 0xCD), (0xCD, 0xCD, 0x00), (0xCD, 0xCD, 0xCD),
    (0x00, 0x00, 0x00), (0x00, 0x00, 0xFF), (0xFF, 0x00, 0x00), (0xFF, 0x00, 0xFF),
    (0x00, 0xFF, 0x00), (0x00, 0xFF, 0xFF), (0xFF, 0xFF, 0x00), (0xFF, 0xFF, 0xFF),
]

# Fallback copy of the new menu's palette block (src/ui/UiGfx.cpp kUiPalette at
# UI_PAL_BASE). Used only if the source file can't be parsed; ui_palette() below
# reads the real values so a colour tweak in the firmware needs no edit here.
UI_PAL_BASE_FALLBACK = 152
UI_PALETTE_FALLBACK = [
    0x0F1218, 0x1E2431, 0x262D3C, 0x333B4D, 0xE6EBF2, 0x8B95A7, 0xFFFFFF, 0x3B6EF5,
    0x2B3346, 0x4ADE80, 0x191F2A, 0x0B0E14, 0xE23B3B, 0xF2C43B, 0x3BC7E2, 0x5A6478,
]


def ui_palette():
    """(base, [(r,g,b), ...]) for the new menu, parsed from src/ui/UiGfx.cpp.

    The menu owns palette slots UI_PAL_BASE..UI_PAL_BASE+15 and draws with
    nothing else, so without these entries a menu screenshot comes out in the
    G3R3B2 cube's colours (uniform orange wash) whenever the dumped palette is
    the placeholder one.
    """
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       '..', 'src', 'ui', 'UiGfx.cpp')
    base, colors = UI_PAL_BASE_FALLBACK, list(UI_PALETTE_FALLBACK)
    try:
        text = open(src, 'r', encoding='utf-8', errors='replace').read()
        m = re.search(r'#define\s+UI_PAL_BASE\s+(\d+)', text)
        if m:
            base = int(m.group(1))
        m = re.search(r'kUiPalette\s*\[\s*C_COUNT\s*\]\s*=\s*\{(.*?)\}\s*;', text, re.S)
        if m:
            vals = [int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]{6})\b', m.group(1))]
            if len(vals) == len(UI_PALETTE_FALLBACK):
                colors = vals
    except OSError:
        pass
    return base, [((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF) for c in colors]


def main():
    if len(sys.argv) < 6:
        print(__doc__)
        sys.exit(1)

    fb_path, pal_path, w, h, out_path = sys.argv[1:6]
    w, h = int(w), int(h)
    use_zx = '--zx' in sys.argv[6:]
    no_xor = '--no-xor' in sys.argv[6:]

    check_contiguous(w, h)

    fb = open(fb_path, 'rb').read()
    if len(fb) < w * h:
        print(f"fb too small: {len(fb)} < {w*h}")
        sys.exit(1)

    pal_raw = open(pal_path, 'rb').read()
    pal = []
    for i in range(256):
        v = struct.unpack_from('<I', pal_raw, i * 4)[0]
        r = (v >> 16) & 0xFF
        g = (v >> 8) & 0xFF
        b = v & 0xFF
        pal.append((r, g, b))

    # Heuristic: detect when the dumped palette doesn't look like a real
    # color table for ZX — pure gradient (picodvi-shim default), or random
    # framebuffer bytes (HDMI build optimizes palette away). In both cases
    # substitute the standard ZX palette for indices 0..15.
    looks_default = all(pal[i] == (i, i, i) for i in range(3, 16))
    # ZX-like: indices 1..6 should have R/G/B in {0, 0xCD}. If almost none
    # of indices 1..15 match that pattern, palette is bogus.
    zx_match = sum(
        1 for i in range(1, 16)
        if all(c in (0x00, 0xCD, 0xFF) for c in pal[i]) and pal[i] != (0, 0, 0)
    )
    looks_bogus = zx_match < 4
    if use_zx or looks_default or looks_bogus:
        for i in range(16):
            pal[i] = ZX_PALETTE[i]
        # 16 = OSD orange; 17..239 = the stock G3R3B2 CLUT cube (grb_to_rgb888
        # in Video.cpp). Needed for anything beyond plain ULA: ULA+ (64..127)
        # and the pico-speccy Spectrum Next renderer (17..219) index into the
        # cube, and the placeholder palette dump leaves those slots as garbage.
        pal[16] = (0xFF, 0x7F, 0x00)
        for i in range(17, 240):
            g3 = (i >> 5) & 7
            r3 = (i >> 2) & 7
            b2 = i & 3
            pal[i] = ((r3 << 5) | (r3 << 2) | (r3 >> 1),
                      (g3 << 5) | (g3 << 2) | (g3 >> 1),
                      (b2 << 6) | (b2 << 4) | (b2 << 2) | b2)
        # The new menu draws ONLY with its own block (152..167 by default), which
        # the cube above would otherwise paint as orange/teal G3R3B2 entries.
        ui_base, ui_pal = ui_palette()
        for i, c in enumerate(ui_pal):
            pal[ui_base + i] = c
        print("palette: using ZX standard 0..15 + G3R3B2 cube 16..239 + UI block "
              f"{ui_base}..{ui_base + len(ui_pal) - 1} (dumped palette looked default)")

    # XOR is not a guess — it is the buffer's storage convention. EVERY video
    # backend that is actually compiled in emits visual pixel x by reading
    # `input_buffer[x ^ 2]`: hdmi.c:645, vga.c:350, tv.c:477, tv-software.c:927,
    # st7789.c:329. So recovering the image from a raw dump is always
    # px[x] = fb[x ^ 2]. (external/PicoDVI, the one shim that reads linearly, is
    # not linked into the firmware — nothing in CMakeLists references it.)
    #
    # This used to be auto-detected by scoring both layouts for row smoothness,
    # which silently picked the wrong one: with the UI palette in place the two
    # scores land within a few percent (menu text is full of 1px strokes, so
    # "smoothness" barely separates them), and the tie-break defaulted to
    # NO-XOR — 2-byte-scrambled text. --no-xor stays as an escape hatch for a
    # dump from some future linear-reading backend.
    use_xor = not no_xor
    print(f"layout: {'XOR (driver convention)' if use_xor else 'NO-XOR (forced)'}")

    img = Image.new('RGB', (w, h))
    px = img.load()
    for y in range(h):
        row = fb[y * w:(y + 1) * w]
        for x in range(w):
            if use_xor and (x ^ 2) < w:
                idx = row[x ^ 2]
            else:
                idx = row[x]
            px[x, y] = pal[idx]

    img.save(out_path)
    print(f"saved {out_path} ({w}x{h})")


if __name__ == '__main__':
    main()
