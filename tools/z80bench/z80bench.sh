#!/bin/bash
# tools/z80bench — the REAL Z80 core (Z80_JLS.cpp.o + CPU.cpp.o out of a firmware
# build) on a Cortex-M33 under QEMU (mps2-an505), on the TS-Conf fast memory path
# (MemGates.h: g_ts_fastmem on, nothing armed), running a CP/M .COM. A QEMU TCG
# plugin counts the ARM instructions the core executes, so a change to the hot
# path can be measured — and checked bit for bit: the bench prints the T-states,
# every register and a CRC of the 64 KB at the end, which must not move.
#
# What it is NOT: a cycle count. It does not model SRAM/XIP wait states, the
# butter-PSRAM line fills the real guest pages cost, or core1 traffic. Use it to
# compare two cores, then take the firmware to hardware with PERF_TRACE.
#
# Needs: arm-none-eabi-gcc 14.2 on PATH (the firmware's own), qemu-system-arm,
# gcc + glib-2.0 headers (for the plugins), curl (once, for qemu-plugin.h).
# ZEXDOC/ZEXALL are not in the tree: anotherlin/z80emu testfiles/zexdoc.com.
# They must be the CP/M builds (BDOS call 5, `LD HL,tests` at 0x11F): a NedoOS
# build prints through its own OS and the bench shows no OK / ERROR line.
#
#   z80bench.sh plugins                      build the two plugins (once)
#   z80bench.sh build <fw build dir> <out.elf>
#   z80bench.sh run   <elf> <file.com> <M T-states, 0 = to the end> [skip] [ntests]
#   z80bench.sh prof  <elf> <file.com> <M T-states> [skip]   per-function profile
#   z80bench.sh zex   <elf> <out-prefix> [file.com]   all 67 tests in 4 parallel shards
#   z80bench.sh lock  <elf> <file.com> <M T-states> <out.log>   state hash per 65536 slices
#   z80bench.sh fuzz  <elf> <n pairs> <seed> <out.log>          random instruction pairs
# Z80BENCH_MODE=next in the environment runs any of them on the Next loop.
#   z80bench.sh mix   <out.com> [high|game]          the game-like workloads (mkmix.py)
#
# skip/ntests carve a window out of the ZEXDOC test table (the operand of the
# `LD HL,tests` at 0x11F). Numbers from the 2026-10-10 session are in CLAUDE.md
# ("Z80 core for 28 MHz").
set -e
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
WORK="${Z80BENCH_WORK:-/tmp/z80bench}"
mkdir -p "$WORK"
QEMU="qemu-system-arm -M mps2-an505 -nographic"

plugins() {
    [ -f "$WORK/qemu-plugin.h" ] || curl -sSL -o "$WORK/qemu-plugin.h" \
        "https://raw.githubusercontent.com/qemu/qemu/v$(qemu-system-arm --version | head -1 | grep -oP '\d+\.\d+\.\d+' | head -1)/include/qemu/qemu-plugin.h"
    for p in insn prof; do
        gcc -shared -fPIC -O2 -I"$WORK" $(pkg-config --cflags glib-2.0) -o "$WORK/lib$p.so" "$HERE/$p.c"
    done
}

build() {
    local B="$1" OUT="$2"
    local O="$B/CMakeFiles/speccy.dir/src/speccy/z80"
    # The harness is compiled with the firmware's own flags (defines, includes).
    local CMD
    CMD=$(python3 -c "
import json,sys
for e in json.load(open('$B/compile_commands.json')):
    if e['file'].endswith('Z80_JLS.cpp'): print(e['command']); break")
    local FLAGS
    FLAGS=$(echo "$CMD" | sed -e 's/^[^ ]* //' -e 's/ -o [^ ]*//' -e 's/ -c [^ ]*//' -e 's/-Os//')
    arm-none-eabi-ld -r -o "$WORK/core.o" "$O/Z80_JLS.cpp.o" "$O/CPU.cpp.o"
    arm-none-eabi-nm -u "$WORK/core.o" | awk '{print $2}' > "$WORK/undef.txt"
    arm-none-eabi-readelf -sW "$(ls "$B"/bin/*/*.elf | grep -v bs2_default | head -1)" | awk 'NF>=8 {print $8, $4, $3}' > "$WORK/fw.sym"
    (cd "$WORK" && python3 "$HERE/gen_stubs.py")
    arm-none-eabi-as -mcpu=cortex-m33 -mthumb -o "$WORK/stubs.o" "$WORK/stubs.s"
    (cd "$REPO" && eval arm-none-eabi-g++ $FLAGS -O2 -c "$HERE/harness.cpp" -o "$WORK/harness.o")
    arm-none-eabi-g++ -mcpu=cortex-m33 -mthumb -mfloat-abi=softfp --specs=rdimon.specs -nostartfiles \
        -T "$HERE/link.ld" "$WORK/harness.o" "$WORK/core.o" "$WORK/stubs.o" -o "$OUT" \
        -Wl,--start-group -lc -lrdimon -lstdc++ -lgcc -Wl,--end-group 2>&1 | grep -v warning || true
    [ -f "$OUT" ]
}

# Z80BENCH_MODE=next selects the Spectrum Next loop (harness argv[5]); anything
# else (default "ts") is the TS-Conf fast path.
MODE="${Z80BENCH_MODE:-ts}"

run() {
    $QEMU -semihosting-config enable=on,target=native,arg=bench,arg="$2",arg="$3",arg="${4:-0}",arg="${5:-0}",arg="$MODE" \
        -kernel "$1" -plugin "$WORK/libinsn.so" 2>&1
}

prof() {
    PROF_OUT="$WORK/prof.txt" $QEMU -semihosting-config enable=on,target=native,arg=bench,arg="$2",arg="$3",arg="${4:-0}",arg=0,arg="$MODE" \
        -kernel "$1" -plugin "$WORK/libprof.so" 2>&1 | tail -3
    python3 "$HERE/symprof.py" "$1" "$WORK/prof.txt"
}

# Lockstep across two builds: the harness runs in slices of a pseudo-random
# length (1..400 T, each carried on to an instruction boundary) and prints a
# hash of the whole Z80 state every 65536 slices; the first differing line of
# two logs brackets the divergence. `fuzz` runs <n> random tests (an instruction
# with any prefix, PC/SP/HL on 8K slot edges, SCF/CCF after it half the time so
# the Q flag shows; alternately as two single steps and as one 40..103 T slice)
# and prints a hash every 4096.
lock() {   # lock <elf> <file.com> <M T-states> <out.log>
    $QEMU -semihosting-config enable=on,target=native,arg=bench,arg="$2",arg="$3",arg=0,arg=0,arg="$MODE",arg=lock \
        -kernel "$1" > "$4" 2>&1
    tail -2 "$4"
}
fuzz() {   # fuzz <elf> <n pairs> <seed> <out.log>
    $QEMU -semihosting-config enable=on,target=native,arg=bench,arg=-,arg="$2",arg="$3",arg=0,arg="$MODE",arg=fuzz \
        -kernel "$1" > "$4" 2>&1
    tail -1 "$4"
}

zex() {
    local S=(0 17 34 51) N=(17 17 17 16)
    for i in 0 1 2 3; do
        $QEMU -semihosting-config enable=on,target=native,arg=bench,arg="${3:-$WORK/zexdoc.com}",arg=0,arg=${S[$i]},arg=${N[$i]},arg="$MODE" \
            -kernel "$1" > "$2.$i.log" 2>&1 &
    done
    wait
    cat "$2".[0-3].log | grep -E "OK|ERROR" | sort | uniq -c | sort -rn | head -3
    echo "tests OK: $(cat "$2".[0-3].log | grep -c '  OK'), ERROR: $(cat "$2".[0-3].log | grep -c ERROR)"
}

cmd="$1"; shift || true
case "$cmd" in
    plugins) plugins ;;
    build)   build "$@" ;;
    run)     run "$@" ;;
    prof)    prof "$@" ;;
    zex)     zex "$@" ;;
    lock)    lock "$@" ;;
    fuzz)    fuzz "$@" ;;
    mix)     python3 "$HERE/mkmix.py" "$@" ;;
    *) sed -n 2,30p "$0"; exit 1 ;;
esac
