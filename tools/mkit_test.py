import struct, math, sys
out = sys.argv[1]
name = b"PICO-SPECCY IT TEST".ljust(26, b"\0")
nchan = 4; rows = 64; norders = 2
# sample: 256-sample sine, looped
smp = bytes((int(127*math.sin(2*math.pi*i/256)) & 0xFF) for i in range(256))
# pattern: arpeggio over 4 channels, a note every 4 rows
notes = [60, 64, 67, 72, 67, 64]
pat = bytearray()
for r in range(rows):
    if r % 4 == 0:
        ch = (r // 4) % nchan
        n = notes[(r // 4) % len(notes)] + (12 if ch == 3 else 0)
        pat += bytes([(ch + 1) | 0x80, 0x03, n, 1])
    pat += b"\0"
hdr_len = 0xC0
orders = bytes([0, 0, 0xFF])          # 2 orders + end
ord_off = hdr_len
smpoff_pos = ord_off + len(orders)
patoff_pos = smpoff_pos + 4
smp_hdr_off = patoff_pos + 4
smp_data_off = smp_hdr_off + 0x50
pat_off = smp_data_off + len(smp)
hdr = b"IMPM" + name + struct.pack("<HHHHHHHHH", 0, len(orders), 0, 1, 1, 0x0214, 0x0200, 0x0001, 0)
hdr += bytes([128, 48, 6, 125, 128, 0]) + struct.pack("<HI", 0, 0) + b"\0"*4
hdr += bytes([32]*64) + bytes([64]*64)
assert len(hdr) == hdr_len
smphdr = b"IMPS" + b"sine.raw".ljust(12, b"\0") + bytes([0, 64, 0x11, 64]) + b"sine".ljust(26, b"\0")
smphdr += bytes([0x01, 32]) + struct.pack("<IIIIIII", len(smp), 0, len(smp), 8363, 0, 0, smp_data_off) + bytes([0,0,0,0])
assert len(smphdr) == 0x50
pathdr = struct.pack("<HH", len(pat), rows) + b"\0"*4
data = hdr + orders + struct.pack("<I", smp_hdr_off) + struct.pack("<I", pat_off) + smphdr + smp + pathdr + pat
open(out, "wb").write(data)
print(out, len(data), "bytes")
