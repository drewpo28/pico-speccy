#!/usr/bin/env python3
"""
hil_bench.py - hardware-in-the-loop FPS / IDL benchmark over SWD.

Drives a board running a HIL_BENCH=ON firmware through OpenOCD (debug probe):
flash an ELF, load a scenario (persist slot or snapshot file) through the g_hil
mailbox (src/app/HilBench.h), let it warm up, measure a window, read the
accumulators back. Nothing goes through the UART. See tools/hil/README.md.

  hil_bench.py build                       # cmake+ninja a HIL_BENCH=ON image
  hil_bench.py flash  [--elf E]
  hil_bench.py run    [--elf E] [--flash] -s scenarios.json [-o out.json]
  hil_bench.py ab     --elf-a A --elf-b B -s scenarios.json [--rounds 2]
  hil_bench.py compare base.json new.json
  hil_bench.py peek   [--elf E]            # dump the live mailbox once

Standard library only.
"""
import argparse
import glob
import json
import os
import shutil
import socket
import statistics
import struct
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HIL_MAGIC = 0x4C494850
HIL_VERSION = 1
CMD_RESET, CMD_LOAD_SLOT, CMD_LOAD_FILE = 1, 2, 3
ST_NAMES = {0: "idle", 1: "busy", 2: "ok", 3: "fail"}

# struct HilBench (src/app/HilBench.h), little-endian, natural alignment.
HIL_FMT = "<6I" "I i i I" "q Q I I Q Q Q Q Q I I"
HIL_FIELDS = ("magic version cmd arg status seq "
              "frames idle_min idle_max neg_frames "
              "idle_sum elapsed_sum elapsed_max target_us t_first t_last "
              "cpu_sum mix_sum kbd_sum frames_total pad0").split()
HIL_SIZE = struct.calcsize(HIL_FMT)          # 112
OFF = {"cmd": 8, "arg": 12, "status": 16, "seq": 20, "path": HIL_SIZE}
PATH_LEN = 128
assert HIL_SIZE == 112


def log(*a):
    print(*a, file=sys.stderr, flush=True)


# ---------------------------------------------------------------- tool lookup
def newest(pattern):
    c = sorted(glob.glob(os.path.expanduser(pattern)))
    return c[-1] if c else None


def find_openocd(arg):
    if arg:
        return arg
    p = newest("~/.pico-sdk/openocd/*/openocd") or shutil.which("openocd")
    if not p:
        sys.exit("openocd not found: pass --openocd (the Pico VS Code extension puts it in ~/.pico-sdk/openocd)")
    return p


def find_scripts(openocd, arg):
    if arg:
        return arg
    d = os.path.join(os.path.dirname(openocd), "scripts")
    return d if os.path.isdir(d) else None


def find_nm():
    return (newest("~/.pico-sdk/toolchain/*/bin/arm-none-eabi-nm")
            or shutil.which("arm-none-eabi-nm") or shutil.which("nm"))


def find_elf(arg, build_dir):
    if arg:
        return os.path.abspath(arg)
    c = [e for e in glob.glob(os.path.join(build_dir, "**", "*.elf"), recursive=True)
         if "bs2_default" not in e]
    if not c:
        sys.exit(f"no ELF under {build_dir}: run `hil_bench.py build` or pass --elf")
    return max(c, key=os.path.getmtime)


def symbol_addr(elf, name="g_hil"):
    out = subprocess.run([find_nm(), "-C", elf], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[-1] == name:
            return int(parts[0], 16)
    sys.exit(f"{name} not in {elf}: not a HIL_BENCH=ON build")


# ---------------------------------------------------------------- OpenOCD
class OpenOCD:
    """One OpenOCD server for the whole run, driven over its TCL RPC port."""

    def __init__(self, a, program=None):
        self.exe = find_openocd(a.openocd)
        self.scripts = find_scripts(self.exe, a.scripts)
        self.port = a.tcl_port
        self.base = [self.exe] + (["-s", self.scripts] if self.scripts else []) + [
            "-f", f"interface/{a.interface}.cfg", "-f", f"target/{a.target}.cfg",
            "-c", f"adapter speed {a.adapter_speed}"]
        self.logf = open(os.path.join(a.workdir, "openocd.log"), "a")
        if program:
            log(f"flashing {program}")
            r = subprocess.run(self.base + ["-c", f"program {{{program}}} verify reset exit"],
                               stdout=self.logf, stderr=subprocess.STDOUT)
            if r.returncode:
                sys.exit(f"flashing failed, see {self.logf.name}")
        cmd = self.base + ["-c", f"tcl_port {self.port}", "-c", "gdb_port disabled",
                           "-c", "telnet_port disabled"]
        self.proc = subprocess.Popen(cmd, stdout=self.logf, stderr=subprocess.STDOUT)
        self.sock = None
        for _ in range(50):
            if self.proc.poll() is not None:
                sys.exit(f"openocd exited, see {self.logf.name}")
            try:
                self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=2)
                break
            except OSError:
                time.sleep(0.2)
        if not self.sock:
            self.close()
            sys.exit("openocd TCL port did not open")
        self.sock.settimeout(120)

    def cmd(self, c):
        self.sock.sendall(c.encode() + b"\x1a")
        buf = b""
        while not buf.endswith(b"\x1a"):
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("openocd closed the TCL connection")
            buf += chunk
        return buf[:-1].decode(errors="replace")

    def read(self, addr, n):
        words = (n + 3) // 4
        r = self.cmd(f"read_memory 0x{addr:08x} 32 {words}").split()
        try:
            return struct.pack(f"<{words}I", *[int(x, 16) for x in r])[:n]
        except ValueError:
            raise RuntimeError(f"read_memory failed: {' '.join(r)[:200]}")

    def write32(self, addr, v):
        self.cmd(f"write_memory 0x{addr:08x} 32 {{0x{v & 0xffffffff:08x}}}")

    def write_bytes(self, addr, data):
        self.cmd(f"write_memory 0x{addr:08x} 8 {{{' '.join('0x%02x' % b for b in data)}}}")

    def reset(self):
        self.cmd("reset run")

    def close(self):
        try:
            if self.sock:
                self.sock.close()
        finally:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()


# ---------------------------------------------------------------- mailbox
class Board:
    def __init__(self, ocd, elf):
        self.ocd, self.elf = ocd, elf
        self.addr = symbol_addr(elf)

    def snap(self):
        """A consistent copy: two identical reads in a row (the struct changes once a frame)."""
        prev = None
        for _ in range(20):
            raw = self.ocd.read(self.addr, HIL_SIZE)
            if raw == prev:
                return dict(zip(HIL_FIELDS, struct.unpack(HIL_FMT, raw)))
            prev = raw
        raise RuntimeError("mailbox never read the same twice")

    def wait_boot(self, timeout=60, frames=50):
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                s = self.snap()
                if s["magic"] == HIL_MAGIC and s["frames_total"] >= frames:
                    if s["version"] != HIL_VERSION:
                        sys.exit(f"firmware HIL_VERSION {s['version']}, tool {HIL_VERSION}")
                    return s
            except RuntimeError:
                pass
            time.sleep(0.3)
        raise RuntimeError("board did not reach the main loop (magic / frames_total)")

    def command(self, cmd, arg=0, path=None, timeout=60):
        s0 = self.snap()
        if s0["cmd"]:
            raise RuntimeError(f"a command is still pending: {s0['cmd']}")
        if path is not None:
            b = path.encode("cp1251")[:PATH_LEN - 1] + b"\0"
            self.ocd.write_bytes(self.addr + OFF["path"], b)
        self.ocd.write32(self.addr + OFF["arg"], arg)
        self.ocd.write32(self.addr + OFF["cmd"], cmd)
        t0 = time.time()
        while time.time() - t0 < timeout:
            time.sleep(0.05)
            try:
                s = self.snap()
            except RuntimeError:
                continue          # mid-reboot
            if s["frames_total"] < s0["frames_total"]:
                log("  board rebooted during the command (machine switch); waiting for it")
                return self.wait_boot()
            if s["seq"] != s0["seq"] and s["cmd"] == 0:
                if s["status"] != 2:
                    raise RuntimeError(f"command {cmd} -> {ST_NAMES.get(s['status'])}")
                return s
        raise RuntimeError(f"command {cmd} timed out")


def window_stats(s):
    n = s["frames"]
    if n < 2:
        raise RuntimeError("window holds < 2 frames (paused? OSD open?)")
    span = (s["t_last"] - s["t_first"]) / 1e6
    return {
        "frames": n,
        "fps": (n - 1) / span if span > 0 else 0.0,
        "idl_avg": s["idle_sum"] / n,
        "idl_min": s["idle_min"],
        "idl_max": s["idle_max"],
        "neg_frames": s["neg_frames"],
        "el_avg": s["elapsed_sum"] / n,
        "el_max": s["elapsed_max"],
        "cpu_avg": s["cpu_sum"] / n,
        "mix_avg": s["mix_sum"] / n,
        "kbd_avg": s["kbd_sum"] / n,
        "target_us": s["target_us"],
    }


def load_scenarios(a):
    if a.scenarios:
        with open(a.scenarios) as f:
            sc = json.load(f)
    else:
        sc = []
    if a.slot is not None:
        sc.append({"name": f"slot{a.slot}", "slot": a.slot})
    if a.file:
        sc.append({"name": os.path.basename(a.file), "file": a.file})
    if not sc:
        sys.exit("no scenarios: -s FILE, --slot N or --file PATH")
    for i, s in enumerate(sc):
        s.setdefault("name", f"s{i}")
        s.setdefault("warmup", a.warmup)
        s.setdefault("measure", a.measure)
        s.setdefault("repeat", a.repeat)
    return sc


def run_scenarios(board, scenarios):
    results = []
    for sc in scenarios:
        reps = []
        for r in range(sc["repeat"]):
            if "slot" in sc:
                board.command(CMD_LOAD_SLOT, arg=int(sc["slot"]))
            elif "file" in sc:
                board.command(CMD_LOAD_FILE, path=sc["file"])
            time.sleep(sc["warmup"])
            board.command(CMD_RESET)
            time.sleep(sc["measure"])
            st = window_stats(board.snap())
            reps.append(st)
            log(f"  {sc['name']} #{r + 1}: FPS {st['fps']:.2f}  IDL avg {st['idl_avg']:.0f} "
                f"min {st['idl_min']}  neg {st['neg_frames']}/{st['frames']}  cpu {st['cpu_avg']:.0f}")
        agg = {}
        for k in ("fps", "idl_avg", "idl_min", "el_avg", "cpu_avg", "neg_frames"):
            v = [x[k] for x in reps]
            agg[k] = statistics.median(v)
            agg[k + "_sd"] = statistics.pstdev(v) if len(v) > 1 else 0.0
        results.append({"name": sc["name"], "scenario": sc, "median": agg, "reps": reps})
    return results


def summary(results, out=sys.stdout):
    print(f"{'scenario':24} {'FPS':>7} {'IDL avg':>8} {'IDL min':>8} {'neg':>5} {'cpu us':>7}", file=out)
    for r in results:
        m = r["median"]
        print(f"{r['name'][:24]:24} {m['fps']:7.2f} {m['idl_avg']:8.0f} {m['idl_min']:8.0f} "
              f"{m['neg_frames']:5.0f} {m['cpu_avg']:7.0f}", file=out)


def compare(base, new, out=sys.stdout):
    """Rows: delta new-base, and whether it is outside 2x the larger rep spread."""
    bi = {r["name"]: r["median"] for r in base}
    print(f"{'scenario':24} {'dFPS':>7} {'dIDL avg':>9} {'dIDL min':>9}  verdict", file=out)
    score = 0.0
    for r in new:
        b = bi.get(r["name"])
        if not b:
            continue
        m = r["median"]
        d_fps = m["fps"] - b["fps"]
        d_idl = m["idl_avg"] - b["idl_avg"]
        d_min = m["idl_min"] - b["idl_min"]
        noise = 2 * max(m["idl_avg_sd"], b["idl_avg_sd"], 20.0)
        verdict = "better" if d_idl > noise else "worse" if d_idl < -noise else "same"
        if abs(d_fps) > 2 * max(m["fps_sd"], b["fps_sd"], 0.05):
            verdict += " (FPS %+.2f)" % d_fps
        score += d_idl
        print(f"{r['name'][:24]:24} {d_fps:+7.2f} {d_idl:+9.0f} {d_min:+9.0f}  {verdict}", file=out)
    print(f"total dIDL avg: {score:+.0f} us", file=out)
    return score


# ---------------------------------------------------------------- commands
def cmd_build(a):
    bdir = os.path.abspath(a.build_dir)
    env = dict(os.environ)
    if not shutil.which("cmake", path=env.get("PATH")):
        cb = newest("~/.pico-sdk/cmake/*/bin")
        if cb:
            env["PATH"] = cb + os.pathsep + env["PATH"]
    if not env.get("PICO_TOOLCHAIN_PATH"):
        tc = newest("~/.pico-sdk/toolchain/*")
        if tc:
            env["PICO_TOOLCHAIN_PATH"] = tc
            env["PATH"] = os.path.join(tc, "bin") + os.pathsep + env["PATH"]
    nj = shutil.which("ninja", path=env.get("PATH")) or newest("~/.pico-sdk/ninja/*/ninja")
    args = ["cmake", "-S", ROOT, "-B", bdir, "-G", "Ninja",
            f"-D{a.board}=ON", f"-DHDMI_HSTX={a.hstx}", "-DFB_FORCE_CHUNKS=0",
            f"-DFLASH_16MB={'ON' if a.flash16 else 'OFF'}", "-DZERO2_PIO_USB=OFF",
            "-DHIL_BENCH=ON", f"-DCMAKE_BUILD_TYPE={a.build_type}"]
    if nj:
        args.append(f"-DCMAKE_MAKE_PROGRAM={nj}")
    args += a.cmake_arg or []
    subprocess.run(args, check=True, env=env)
    subprocess.run(["cmake", "--build", bdir, "--parallel"], check=True, env=env)
    log(f"built: {find_elf(None, bdir)}")


def with_board(a, elf, fn, flash=False):
    ocd = OpenOCD(a, program=elf if flash else None)
    try:
        b = Board(ocd, elf)
        log("waiting for the main loop...")
        b.wait_boot()
        return fn(b)
    finally:
        ocd.close()


def cmd_flash(a):
    elf = find_elf(a.elf, a.build_dir)
    with_board(a, elf, lambda b: log("up"), flash=True)


def cmd_peek(a):
    elf = find_elf(a.elf, a.build_dir)
    def f(b):
        s = b.snap()
        print(json.dumps(s, indent=1))
        if s["frames"] >= 2:
            print(json.dumps(window_stats(s), indent=1))
    with_board(a, elf, f)


def cmd_run(a):
    elf = find_elf(a.elf, a.build_dir)
    sc = load_scenarios(a)
    res = with_board(a, elf, lambda b: run_scenarios(b, sc), flash=a.flash)
    summary(res)
    if a.out:
        with open(a.out, "w") as f:
            json.dump({"elf": elf, "time": time.strftime("%Y-%m-%d %H:%M:%S"), "results": res}, f, indent=1)
        log(f"wrote {a.out}")


def cmd_ab(a):
    sc = load_scenarios(a)
    runs = {"A": [], "B": []}
    order = ["A", "B"]
    for rnd in range(a.rounds):                 # ABBA... so drift hits both
        for k in (order if rnd % 2 == 0 else order[::-1]):
            elf = os.path.abspath(a.elf_a if k == "A" else a.elf_b)
            log(f"round {rnd + 1}: {k} = {elf}")
            runs[k].append(with_board(a, elf, lambda b: run_scenarios(b, sc), flash=True))

    def merge(rr):
        out = []
        for i, r in enumerate(rr[0]):
            reps = [x for run in rr for x in run[i]["reps"]]
            agg = {}
            for k in ("fps", "idl_avg", "idl_min", "el_avg", "cpu_avg", "neg_frames"):
                v = [x[k] for x in reps]
                agg[k] = statistics.median(v)
                agg[k + "_sd"] = statistics.pstdev(v) if len(v) > 1 else 0.0
            out.append({"name": r["name"], "scenario": r["scenario"], "median": agg, "reps": reps})
        return out
    A, B = merge(runs["A"]), merge(runs["B"])
    print("A:"); summary(A)
    print("B:"); summary(B)
    print("B - A:")
    score = compare(A, B)
    if a.out:
        with open(a.out, "w") as f:
            json.dump({"A": A, "B": B, "score_didl": score}, f, indent=1)


def cmd_compare(a):
    with open(a.base) as f:
        base = json.load(f)
    with open(a.new) as f:
        new = json.load(f)
    compare(base.get("results", base), new.get("results", new))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--openocd"); p.add_argument("--scripts")
    p.add_argument("--interface", default="cmsis-dap")
    p.add_argument("--target", default="rp2350")
    p.add_argument("--adapter-speed", type=int, default=5000)
    p.add_argument("--tcl-port", type=int, default=6666)
    p.add_argument("--build-dir", default=os.path.join(ROOT, "build-all", "hil"))
    p.add_argument("--workdir", default=os.path.join(ROOT, "build-all", "hil-logs"))
    sp = p.add_subparsers(dest="cmd", required=True)

    b = sp.add_parser("build")
    b.add_argument("--board", default="PICO_DV_W")
    b.add_argument("--hstx", default="OFF", help="OFF (PIO HDMI) or TMDS")
    b.add_argument("--flash16", action="store_true")
    b.add_argument("--build-type", default="MinSizeRel")
    b.add_argument("--cmake-arg", action="append", help="extra -D..., repeatable")

    for name in ("flash", "peek", "run"):
        q = sp.add_parser(name)
        q.add_argument("--elf")
        if name == "run":
            q.add_argument("--flash", action="store_true", help="program the ELF first")
            q.add_argument("-o", "--out")

    for q in (sp.choices["run"], sp.add_parser("ab")):
        q.add_argument("-s", "--scenarios", help="JSON list of scenarios")
        q.add_argument("--slot", type=int); q.add_argument("--file")
        q.add_argument("--warmup", type=float, default=5.0)
        q.add_argument("--measure", type=float, default=10.0)
        q.add_argument("--repeat", type=int, default=3)
    ab = sp.choices["ab"]
    ab.add_argument("--elf-a", required=True); ab.add_argument("--elf-b", required=True)
    ab.add_argument("--rounds", type=int, default=2)
    ab.add_argument("-o", "--out")

    c = sp.add_parser("compare"); c.add_argument("base"); c.add_argument("new")

    a = p.parse_args()
    os.makedirs(a.workdir, exist_ok=True)
    {"build": cmd_build, "flash": cmd_flash, "peek": cmd_peek, "run": cmd_run,
     "ab": cmd_ab, "compare": cmd_compare}[a.cmd](a)


if __name__ == "__main__":
    main()
