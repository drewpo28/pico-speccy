#!/usr/bin/env python3
"""Print the "Downloads" table for a GitHub release body.

Usage: release_downloads.py REPO TAG FILE.uf2 [FILE.uf2 ...]

Rows are carrier boards, columns are video outputs, each cell lists the
module variants that were built (m1p2 / m1p2w / m1p2ws) as direct links to
the release assets. A column that has both 4 MB and 16 MB images is split in
two ("... 4 MB" / "... 16 MB"); the W modules are 16 MB boards. Only the files
passed in appear; an empty cell is "—".
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
    ("PC2p2", "Olimex RP2350pc"),
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
        if disp.endswith("-16MB"):          # the 16 MB flash type
            disp, flash = disp[:-5], "-16MB"
        elif disp.endswith("-4MB"):         # the 4 MB flash type
            disp = disp[:-4]
        col, suffix = DISPLAYS.get(disp, (disp, ""))
        if board not in rows:
            rows.append(board)
        if col not in cols:
            cols.append(col)
        big = 16 if (flash or module) else 4    # W / WS modules are 16 MB only
        if not flash and module:
            flash = "-16MB"
        key = (1 if suffix else 0, MODULE_ORDER.get(module, 9), module)
        cells.setdefault((board, col, big), []).append(
            (key, m.group("tag") + suffix, flash, f"{base}{name}"))

    def used(c, f=None):
        return any(k[1] == c and (f is None or k[2] == f) for k in cells)

    cols = [c for c in cols if used(c)]
    split = {c for c in cols if used(c, 4) and used(c, 16)}
    heads, keys = [], []
    for c in cols:
        if c in split:
            heads += [f"{c} 4 MB", f"{c} 16 MB"]
            keys += [(c, 4), (c, 16)]
        else:
            heads.append(c)
            keys.append((c, None))
    rows = [r for r in rows if any(k[0] == r for k in cells)]

    out = ["## Downloads", "",
           "| Board | " + " | ".join(heads) + " |",
           "|---|" + "---|" * len(heads)]
    for r in rows:
        line = [labels.get(r, r)]
        for c, f in keys:
            items = []
            for big in ((f,) if f else (4, 16)):
                items += cells.get((r, c, big), [])
            # the -16MB tag is redundant under a "16 MB" header
            links = [f"[{lbl}{'' if f else fl}]({url})" for _, lbl, fl, url in sorted(items)]
            line.append(" / ".join(links) if links else "—")
        out.append("| " + " | ".join(line) + " |")
    out += ["",
            "`*p2` — Raspberry Pi Pico 2 / Pimoroni Pico Plus 2, "
            "`*p2w` — Pimoroni Pico Plus 2 W, "
            "`*p2ws` — Waveshare RP2350B-Plus-W. "
            "`*p2w` and `*p2ws` firmwares are not interchangeable. "
            "**16 MB** columns — images for a 16 MB flash module (built-in YRW801 "
            "MoonSound ROM and GM.DLS bank); **4 MB** — for 4 MB modules.",
            ""]
    print("\n".join(out))


if __name__ == "__main__":
    main()
