#!/usr/bin/env python3
"""Print the "Downloads" table for a GitHub release body.

Usage: release_downloads.py REPO TAG FILE.uf2 [FILE.uf2 ...]

Rows are carrier boards, columns are video outputs, each cell lists the
module variants that were built (m1p2 / m1p2-16MB / m1p2w / m1p2ws) as direct links to
the release assets. Only the files passed in appear; an empty cell is "—".
A board tag or display missing from the tables below still gets a row or
column of its own, named after the raw token.
"""
import os
import re
import sys

# board prefix of the firmware tag -> row label (row order = this order)
BOARDS = [
    ("m1p2", "Murmulator 1.x"),
    ("m2p2", "Murmulator 2.0"),
    ("PCp2", "Olimex PICO-PC"),
    ("DVp2", "Pimoroni Pico DV"),
    ("z0p2", "ZERO2"),
]
# display token -> (column label, suffix added to the link label)
DISPLAYS = {
    "VGA-HDMI":        ("VGA / HDMI", ""),
    "VGA-HDMI-PIOUSB": ("VGA / HDMI", "-PIOUSB"),
    "VGA-HDMI-HSTX":   ("HDMI HSTX", ""),
    "ILI9341":         ("ILI9341", ""),
    "ST7789":          ("ST7789", ""),
    "TV-SOFT":         ("TV", ""),
}
COLUMNS = ["VGA / HDMI", "HDMI HSTX", "ILI9341", "ST7789", "TV"]
MODULE_ORDER = {"": 0, "w": 1, "ws": 2}

NAME_RE = re.compile(r"^(?P<tag>[A-Za-z0-9]+)-speccy-(?P<disp>.+)-(?P<ver>\d+\.\d+\.\d+)\.uf2$")


def split_tag(tag):
    for prefix, _ in BOARDS:
        if tag.startswith(prefix):
            return prefix, tag[len(prefix):]
    m = re.match(r"^(.*?p2)(w|ws)?$", tag)
    return (m.group(1), m.group(2) or "") if m else (tag, "")


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    repo, tag_name, files = sys.argv[1], sys.argv[2], sys.argv[3:]
    base = f"https://github.com/{repo}/releases/download/{tag_name}/"
    labels = dict(BOARDS)
    rows, cols = [p for p, _ in BOARDS], list(COLUMNS)
    cells = {}  # (board, column) -> [(sort key, link)]

    for path in files:
        name = os.path.basename(path)
        m = NAME_RE.match(name)
        if not m:
            print(f"release_downloads: skipping {name}", file=sys.stderr)
            continue
        board, module = split_tag(m.group("tag"))
        disp, flash = m.group("disp"), ""
        if disp.endswith("-16MB"):          # the 16 MB flash type of a 4 MB board
            disp, flash = disp[:-5], "-16MB"
        col, suffix = DISPLAYS.get(disp, (disp, ""))
        if board not in rows:
            rows.append(board)
        if col not in cols:
            cols.append(col)
        key = (1 if suffix else 0, 1 if flash else 0, MODULE_ORDER.get(module, 9), module)
        cells.setdefault((board, col), []).append(
            (key, f"[{m.group('tag')}{suffix}{flash}]({base}{name})"))

    rows = [r for r in rows if any((r, c) in cells for c in cols)]
    cols = [c for c in cols if any((r, c) in cells for r in rows)]

    out = ["## Downloads", "",
           "| Board | " + " | ".join(cols) + " |",
           "|---|" + "---|" * len(cols)]
    for r in rows:
        line = [labels.get(r, r)]
        for c in cols:
            links = [l for _, l in sorted(cells.get((r, c), []))]
            line.append(" / ".join(links) if links else "—")
        out.append("| " + " | ".join(line) + " |")
    out += ["",
            "`*p2` — Raspberry Pi Pico 2 / Pimoroni Pico Plus 2, "
            "`*p2w` — Pimoroni Pico Plus 2 W, "
            "`*p2ws` — Waveshare RP2350B-Plus-W. "
            "`*p2w` and `*p2ws` firmwares are not interchangeable. "
            "`-16MB` — the same board with a 16 MB flash module (adds ZXM-MoonSound); "
            "the plain images are for 4 MB modules.",
            ""]
    print("\n".join(out))


if __name__ == "__main__":
    main()
