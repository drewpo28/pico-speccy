#!/usr/bin/env bash
# Convert pico-speccycy framebuffer dump (made by screenshot.gdb or screenshot_profi.gdb) to PNG.
#
# Usage:
#   screenshot.sh           -- standard ZX framebuffer (uses screenshot.gdb output)
#   screenshot.sh --profi   -- Profi DS80 512x240 (uses screenshot_profi.gdb output)
set -euo pipefail

PROFI=0
for arg in "$@"; do
    [[ "$arg" == "--profi" ]] && PROFI=1
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ $PROFI -eq 1 ]]; then
    FB=/tmp/picospeccycy_profi_fb.bin
    LUT=/tmp/picospeccycy_profi_lut.bin
    PAL=/tmp/picospeccycy_profi_pal.bin
    DIM=/tmp/picospeccycy_profi_dim.txt
    OUT=/tmp/picospeccycy_profi_screen.png

    if [[ ! -f "$FB" || ! -f "$LUT" || ! -f "$PAL" || ! -f "$DIM" ]]; then
        echo "Missing Profi dump files. Run screenshot_profi.gdb in an active GDB session first."
        exit 1
    fi

    DIMLINE=$(grep -oE '[0-9]+ [0-9]+' "$DIM" | head -1)
    if [[ -z "$DIMLINE" ]]; then
        echo "Bad dim file: $DIM"
        cat "$DIM"
        exit 1
    fi
    read -r W H <<< "$DIMLINE"

    python3 "$SCRIPT_DIR/profi2png.py" "$FB" "$LUT" "$PAL" "$W" "$H" "$OUT"
    echo "$OUT"
else
    FB=/tmp/picospeccycy_fb.bin
    PAL=/tmp/picospeccycy_pal.bin
    DIM=/tmp/picospeccycy_dim.txt
    OUT=/tmp/picospeccycy_screen.png

    if [[ ! -f "$FB" || ! -f "$PAL" || ! -f "$DIM" ]]; then
        echo "Missing dump files. Run screenshot.gdb in an active GDB session first."
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

    python3 "$SCRIPT_DIR/fb2png.py" "$FB" "$PAL" "$W" "$H" "$OUT"
    echo "$OUT"
fi
