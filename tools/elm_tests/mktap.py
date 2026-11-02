# mktap.py out.tap name code.bin addr  -> BASIC loader (CLEAR addr-1: LOAD ""CODE: RANDOMIZE USR addr) + CODE
import sys, struct
def num(n):
    s = str(n).encode()
    return s + b'\x0e\x00\x00' + struct.pack('<H', n) + b'\x00'
def blk(flag, data):
    d = bytes([flag]) + data
    x = 0
    for b in d: x ^= b
    d += bytes([x])
    return struct.pack('<H', len(d)) + d
out, name, binf, addr = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 0)
code = open(binf, 'rb').read()
line = b'\xfd' + num(addr - 1) + b':\xef""\xaf:\xf9\xc0' + num(addr) + b'\r'
prog = struct.pack('>H', 10) + struct.pack('<H', len(line)) + line
nm = name.encode().ljust(10)[:10]
hdr = bytes([0]) + nm + struct.pack('<HHH', len(prog), 10, len(prog))
chd = bytes([3]) + nm + struct.pack('<HHH', len(code), addr, 32768)
open(out, 'wb').write(blk(0, hdr) + blk(0xff, prog) + blk(0, chd) + blk(0xff, code))
