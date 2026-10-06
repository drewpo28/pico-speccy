#!/usr/bin/env bash
# Convert a pico-speccy framebuffer dump (made by screenshot.gdb or screenshot_profi.gdb) to PNG.
#
# Usage:
#   screenshot.sh           -- standard ZX framebuffer (uses screenshot.gdb output)
#   screenshot.sh --profi   -- the screenshot_profi.gdb dump
# Either way the decoder follows the firmware's mode (pair slots or standard).
set -euo pipefail

PROFI=0
for arg in "$@"; do
    [[ "$arg" == "--profi" ]] && PROFI=1
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Which GDB script ran decides which framebuffer dump to read; the MODE the
# firmware was in decides how to decode it. Both scripts now dump everything
# needed for either decoder (pair tables + the real HDMI palette) and record
# profi_ds80_active in picospec_mode.txt, so either keybinding gives the right
# picture: a pair-slot frame (Profi DS80, GMX 640x200, Timex hi-res, ATM/ZX-Evo
# EGA/hires/text) goes to profi2png, anything else to fb2png.
if [[ $PROFI -eq 1 ]]; then
    FB=/tmp/picospec_profi_fb.bin
    DIM=/tmp/picospec_profi_dim.txt
    OUT=/tmp/picospec_profi_screen.png
else
    FB=/tmp/picospec_fb.bin
    DIM=/tmp/picospec_dim.txt
    OUT=/tmp/picospec_screen.png
fi
PAL=/tmp/picospec_pal.bin
LUT=/tmp/picospec_profi_lut.bin
PPAL=/tmp/picospec_profi_pal.bin

if [[ ! -f "$FB" || ! -f "$DIM" ]]; then
    echo "Missing dump files. Run the screenshot GDB script in an active GDB session first."
    exit 1
fi

# GDB via Cortex-Debug writes file logs in MI format, e.g. ~"320 240\n".
# Strip MI noise and grab the first line that looks like "<num> <num>".
DIMLINE=$(grep -oE '[0-9]+ [0-9]+' "$DIM" | head -1)
if [[ -z "$DIMLINE" ]]; then
    echo "Bad dim file: $DIM"
    cat "$DIM"
    exit 1
fi
read -r W H <<< "$DIMLINE"

PAIR=$([[ $PROFI -eq 1 ]] && echo 1 || echo 0)   # old dumps: trust the keybinding
if [[ -f /tmp/picospec_mode.txt ]] && grep -qE 'pair [01]' /tmp/picospec_mode.txt; then
    PAIR=$(grep -oE 'pair [01]' /tmp/picospec_mode.txt | head -1 | cut -d' ' -f2)
fi

if [[ $PAIR -eq 1 ]]; then
    if [[ ! -f "$LUT" || ! -f "$PPAL" ]]; then
        echo "Pair-slot frame but no pair tables dumped."
        exit 1
    fi
    echo "mode: pair slots (profi2png)"
    python3 "$SCRIPT_DIR/profi2png.py" "$FB" "$LUT" "$PPAL" "$W" "$H" "$OUT"
else
    if [[ ! -f "$PAL" ]]; then
        echo "Missing palette dump: $PAL"
        exit 1
    fi
    # The real HDMI palette shadow (screenshot*.gdb probe) is the truth: keep it.
    # fb2png's "looks like ZX?" heuristic would throw away any palette off the
    # 00/CD/FF grid — ATM/ZX-Evo (00/55/AA/FF), ULA+, TS-Conf CRAM, presets.
    RAW=()
    if [[ -f /tmp/picospec_sympal.gdb ]] && grep -q 'has_hdmipal = 1' /tmp/picospec_sympal.gdb; then
        RAW=(--raw-pal)
    fi
    echo "mode: standard (fb2png ${RAW[*]:-})"
    python3 "$SCRIPT_DIR/fb2png.py" "$FB" "$PAL" "$W" "$H" "$OUT" "${RAW[@]}"
fi
echo "$OUT"
