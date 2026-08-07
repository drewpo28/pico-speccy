#!/usr/bin/env python3
"""
memdump.py — convert picospec GDB memory dumps to dump.log format.
Reads /tmp/picospec_mem{0-3}.bin (4×16KB Z80 windows),
      /tmp/picospec_ram{0-7}.bin (8×16KB physical RAM pages, 128K),
      /tmp/picospec_regs.txt.
Writes /tmp/picospec_dump.log in the same format as OSD::saveDumpToFile().
The 8 physical RAM pages are appended after the logical Z80 dump.
"""

import sys
import os

MEM_FILES = [f"/tmp/picospec_mem{i}.bin" for i in range(4)]
RAM_FILES = [f"/tmp/picospec_ram{i}.bin" for i in range(8)]
REGS_FILE = "/tmp/picospec_regs.txt"
OUT_FILE  = "/tmp/picospec_dump.log"
# NeoGS card side (written by memdump.gdb only when a card is up)
NGS_LOW_FILE  = "/tmp/picospec_ngs_low.bin"
NGS_REGS_FILE = "/tmp/picospec_ngs.txt"
NGS_BANK_FILES = [f"/tmp/picospec_ngs_b{i}.bin" for i in (4, 5, 6, 7)]

MEM_TYPE_NAME = {0: "SRAM", 1: "PSRAM_SPI", 2: "SWAP"}


def load_mem():
    # pico-speccy (8x8K slots): picospec_s{0-7}.bin; a missing slot file means
    # an accessor-mode slot (not directly readable) — filled with zeros.
    slot_files = [f"/tmp/picospec_s{i}.bin" for i in range(8)]
    if any(os.path.exists(f) for f in slot_files):
        pages = []
        for f in slot_files:
            if os.path.exists(f):
                with open(f, "rb") as fh:
                    data = fh.read()
                if len(data) != 8192:
                    raise ValueError(f"{f}: expected 8192 bytes, got {len(data)}")
            else:
                data = bytes(8192)
            pages.append(data)
        return b"".join(pages)
    pages = []
    for f in MEM_FILES:
        with open(f, "rb") as fh:
            data = fh.read()
        if len(data) != 16384:
            raise ValueError(f"{f}: expected 16384 bytes, got {len(data)}")
        pages.append(data)
    return b"".join(pages)  # 65536 bytes, 0x0000-0xFFFF


def load_ram_pages():
    """Return list of 8 16K page bytes, or None for any page that's missing."""
    out = []
    for f in RAM_FILES:
        if not os.path.exists(f):
            out.append(None)
            continue
        with open(f, "rb") as fh:
            data = fh.read()
        if len(data) != 16384:
            raise ValueError(f"{f}: expected 16384 bytes, got {len(data)}")
        out.append(data)
    return out


def load_kv(path):
    """Parse a `key=value` log written by GDB `printf`. Returns {} if absent."""
    if not os.path.exists(path):
        return {}
    regs = {}
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            # Strip GDB/MI console-stream wrapping: ~"AF=7E2C\n"
            if line.startswith('~"') and line.endswith('"'):
                line = line[2:-1]
                line = line.replace("\\n", "").replace("\\r", "").replace('\\"', '"')
                line = line.strip()
            # Skip MI result/async records (start with =, *, ^, &, @)
            if not line or line[0] in '=*^&@':
                continue
            if "=" in line:
                k, v = line.split("=", 1)
                regs[k.strip()] = v.strip()
    return regs


def load_regs():
    return load_kv(REGS_FILE)


def r16(regs, key):
    return int(regs.get(key, "0"), 16)

def r8(regs, key):
    return int(regs.get(key, "0"), 16)

def ri(regs, key):
    return int(regs.get(key, "0"))


def flags_str(af_lo):
    return (f"S={( af_lo>>7)&1} Z={(af_lo>>6)&1} H={(af_lo>>4)&1} "
            f"P={(af_lo>>2)&1} N={(af_lo>>1)&1} C={af_lo&1}")


def hex_line(addr, mem, off=None):
    """One canonical 16-byte line. `off` is the index into `mem` when it differs
    from the address being printed (the NeoGS fixed window lives at physical
    0xC000 but the GS-Z80 sees it at 0x4000)."""
    if off is None:
        off = addr
    b = [mem[off + i] for i in range(16)]
    hex_part = " ".join(f"{x:02X}" for x in b[:8]) + "  " + " ".join(f"{x:02X}" for x in b[8:])
    asc_part = "".join(chr(x) if 0x20 <= x < 0x7F else "." for x in b)
    return f"{addr:04X}: {hex_part}  |{asc_part}|\n"


def main():
    # Either the 8x8K slot set (pico-speccy) or the 4x16K legacy set must be
    # present; load_mem() picks whichever exists.
    have_slots = any(os.path.exists(f"/tmp/picospec_s{i}.bin") for i in range(8))
    need = [REGS_FILE] if have_slots else MEM_FILES + [REGS_FILE]
    for f in need:
        if not os.path.exists(f):
            print(f"Missing: {f}", file=sys.stderr)
            sys.exit(1)

    mem  = load_mem()
    ram_pages = load_ram_pages()
    regs = load_regs()

    af   = r16(regs, "AF");  af_lo = af & 0xFF
    bc   = r16(regs, "BC");  de  = r16(regs, "DE");  hl  = r16(regs, "HL")
    afx  = r16(regs, "AFx"); bcx = r16(regs, "BCx"); dex = r16(regs, "DEx"); hlx = r16(regs, "HLx")
    ix   = r16(regs, "IX");  iy  = r16(regs, "IY")
    sp   = r16(regs, "SP");  pc  = r16(regs, "PC")
    rI   = r8(regs,  "I");   rR  = r8(regs, "R")
    im   = ri(regs,  "IM");  iff1 = ri(regs, "IFF1"); iff2 = ri(regs, "IFF2")
    halt = ri(regs,  "halted")
    bank = ri(regs,  "bankLatch"); rom_latch = ri(regs, "romLatch")
    vid  = ri(regs,  "videoLatch"); rom_in_use = ri(regs, "romInUse")
    plock = ri(regs, "pagingLock"); p0ram = ri(regs, "page0ram")
    tst  = int(regs.get("tstates", "0"))

    with open(OUT_FILE, "w") as out:
        out.write("=" * 40 + "\n")
        out.write(f"Dump: #0000 - #FFFF\n")
        out.write(f"ROM in use: {rom_in_use}  romLatch: {rom_latch}  bankLatch: {bank}  videoLatch: {vid}\n")
        out.write(f"pagingLock: {plock}  page0ram: {p0ram}\n")

        out.write("--- Registers ---\n")
        out.write(f"AF={af:04X}  BC={bc:04X}  DE={de:04X}  HL={hl:04X}\n")
        out.write(f"AF'={afx:04X} BC'={bcx:04X} DE'={dex:04X} HL'={hlx:04X}\n")
        out.write(f"IX={ix:04X}  IY={iy:04X}  SP={sp:04X}  PC={pc:04X}\n")
        out.write(f"I={rI:02X} R={rR:02X}  IM={im}  IFF1={iff1} IFF2={iff2}  Halted={halt}\n")
        out.write(f"Flags: {flags_str(af_lo)}\n")

        out.write("--- Stack (top 8) ---\n")
        for i in range(8):
            addr = (sp + i * 2) & 0xFFFF
            val  = mem[addr] | (mem[(addr + 1) & 0xFFFF] << 8)
            out.write(f"  SP+{i*2:02X} [{addr:04X}] = {val:04X}\n")

        out.write(f"CPU T-states: {tst}\n")

        out.write("--- Memory dump ---\n")
        for a in range(0, 0x10000, 16):
            out.write(hex_line(a, mem))

        out.write("\n")

        # 128K physical RAM pages 0..7 (raw page contents, independent of mapping)
        out.write("=" * 40 + "\n")
        out.write("Physical RAM pages (128K)\n")
        for n, page in enumerate(ram_pages):
            type_id = ri(regs, f"ram{n}_type")
            type_name = MEM_TYPE_NAME.get(type_id, f"?{type_id}")
            if page is None:
                out.write(f"--- RAM page {n} ({type_name}) [missing] ---\n")
                continue
            out.write(f"--- RAM page {n} ({type_name}) ---\n")
            for a in range(0, 0x4000, 16):
                out.write(hex_line(a, page))
        out.write("\n")

        # NeoGS card side, when memdump.gdb found a live card. The GS-Z80's
        # 0x0000-0x3FFF is physical page 0 (with NOROM set, which is how every
        # loaded firmware runs) and its fixed 0x4000-0x7FFF window is physical
        # 0xC000-0xFFFF, so the single 64K s_ngs_low_ram dump carries both —
        # printed here under the CPU addresses the GS-Z80 actually sees, which
        # is what a disassembly of a stuck PC needs.
        ngs = load_kv(NGS_REGS_FILE)
        if ngs:
            out.write("=" * 40 + "\n")
            out.write("NeoGS GS-Z80\n")
            out.write("PC={PC} SP={SP} AF={AF} BC={BC} DE={DE} HL={HL} "
                      "IX={IX} IY={IY}\n".format(**{k: ngs.get(k, "????")
                      for k in ("PC","SP","AF","BC","DE","HL","IX","IY")}))
            out.write("GSCFG0={} MPAG={} MPAGEX={} INTENA={} INTREQ={} "
                      "status={} command={} ZXDMA={}@{}\n".format(
                          ngs.get("GSCFG0","??"), ngs.get("MPAG","??"),
                          ngs.get("MPAGEX","??"), ngs.get("INTENA","??"),
                          ngs.get("INTREQ","??"), ngs.get("status","??"),
                          ngs.get("command","??"), ngs.get("zxdma","?"),
                          ngs.get("dma_addr","??????")))
            low = None
            if os.path.exists(NGS_LOW_FILE):
                with open(NGS_LOW_FILE, "rb") as fh:
                    low = fh.read()
            if low and len(low) >= 0x10000:
                out.write("--- NeoGS 0000-3FFF (phys page 0; ROM window "
                          "unless NOROM) ---\n")
                for a in range(0, 0x4000, 16):
                    out.write(hex_line(a, low))
                out.write("--- NeoGS 4000-7FFF (fixed window = phys "
                          "C000-FFFF) ---\n")
                win = low[0xC000:0x10000]
                for a in range(0, 0x4000, 16):
                    out.write(hex_line(0x4000 + a, win, a))
            else:
                out.write("--- NeoGS memory [missing] ---\n")
            # Banked window 8000-FFFF, four 8K slots as currently mapped. This
            # is where a demo's own card-side handlers usually live, and none of
            # it is reachable through s_ngs_low_ram.
            bank = bytearray()
            for f in NGS_BANK_FILES:
                if os.path.exists(f):
                    with open(f, "rb") as fh:
                        d = fh.read()
                    bank += d if len(d) == 8192 else bytes(8192)
                else:
                    bank += b"\xFF" * 8192
            if any(b != 0xFF for b in bank):
                out.write("--- NeoGS 8000-FFFF (banked: MPAG/MPAGEX as mapped) "
                          "---\n")
                for a in range(0, 0x8000, 16):
                    out.write(hex_line(0x8000 + a, bank, a))
            out.write("\n")

    print(f"Dump written to {OUT_FILE}")


if __name__ == "__main__":
    main()
