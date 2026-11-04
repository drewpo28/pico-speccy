<!-- Moved verbatim from CLAUDE.md on 2026-11-04; sections in their original order. -->

## FTP server session memory — everything through Buffer::palloc, everything freed (hw-confirmed 2026-09-02)

"FTP server won't start the SECOND time" (user hw report: 24 KB free after boot,
21 KB after one start/stop, second start fails). Two combined causes, both fixed
(hw-confirmed: free heap identical before/after a start/stop cycle; a large STOR
upload through the possibly-butter-backed wrBuf not separately re-timed):

- **`ftpd_log` (OSDMain.cpp, 40x72 = 2880 B) leaked**: lazily allocated on the
  first logged line, never freed — its old comment even sold that as a feature
  ("no heap churn"). Now `ftpdLogFree()` runs on EVERY exit path of
  `ftpdSessionRun` (normal, begin-failed, page-OOM), always AFTER `Ftpd::stop()`
  (stop may still log; a log line after the free would re-allocate).
- **`Ftpd::begin`'s gate wanted `sizeof(FtpdBuf)+16 KB` as ONE contiguous
  block** (~20.5 KB) — with 21 KB total free that fails even though the ~4.1 KB
  scratch itself fits easily. The 3 KB leak pushed a barely-passing config
  under the gate; hence "second start".

All four FTP-session buffers (FtpdBuf ~4.1 KB, STOR wrBuf 16 KB, ftpd_log
2880 B, s_ftpd_page 2888 B) now go through
`Buffer::palloc(NEED_POINTER | USE_NET_ARENA)` / `pfree`, like the net
alt-stack and the ZiFiSock demux ring always did: `ftpServerRun`'s
NetArenaLease is live for the whole session, so on boards with a dormant
Gigascreen prevFB or butter PSRAM the session costs ~0 heap; palloc's heap
tier keeps its own margin and its last-resort path returns NULL instead of
pico_malloc's OOM panic (the manual `getLargestAllocatable` gates + raw
malloc/calloc these replaced were reimplementing exactly that contract,
more strictly). Session heap profile is now: allocate on entry, free on
exit, zero residue — verify with Memory Info before/after a start/stop
cycle; the two figures must match.

## ZiFi NIC — three host interfaces (all bridge to one ESP UART)

Gated by `Config::zifi_enabled`. First two: port low byte `0xEF`, high address byte = register.
- **ZIFI-API FIFO** (`#00EF`..`#C7EF`): hi ≤ 0xC7. `ZiFi::read/write`. DR data + ZIFR/ZOFR/IMR/CR. High-level FIFO interface.
- **16550 UART window** (`#F8EF`..`#FFEF`): hi ≥ 0xF8. `ZiFi::uart16550Read/Write`. reg = hi&7: 0=RBR/THR(or DLL if DLAB), 1=IER/DLM, 2=IIR/FCR, 3=LCR, 4=MCR, 5=LSR, 6=MSR, 7=SCR. THR/RBR bridge to the SAME `zifi_in_buf`/`zifi_out_buf` as the API. LSR=`0x60 | (rx?1:0)`; baud fixed 115200 8N1 (divisor latches stored, ignored).
- **ZX UNO window** (`#FC3B` addr latch / `#FD3B` data; full 16-bit decode, bit8 = data port): `ZiFi::unoUartRead/Write`. Karabas-Pro's native ESP8266 bridge (dev manual "Порты ZX UNO"). Internal regs: `#C6` UART data (read = accumulator `uno_last_rx`), `#C7` status = **bit7 byte received, bit6 transmitter busy** (= out-FIFO full), rest 0; `#C8/#C9` (UART2) absent → 0xFF. Same FIFOs as the `#xxEF` windows; machine-independent (not Profi-gated). **The status bits were 0/1 until 2026-10-06** (pico-spec issue #59, barszpaw): the Karabas-Pro RTL `rtl/uart/zxunouart.v` says `dout = {data_received, txbusy, 6'h00}` and every ZX-Uno client (BridgeZX, SpecTalkZX, moon-rabbit-zx, karabas-pro net-tools) polls `#80`/`#40` with `ADD A,A` / `AND #40`, so no hardware-targeting client ever saw RX ready here — BridgeZX hung at "Connecting". The reporter hw-confirmed the 0x80/0x40 fix on pico-spec 1.2.30 (RP2350-PiZero, BridgeZX file transfer); fixed in both repos. Known lenient deviation: hardware DROPS a `#C6` write while txbusy, we queue it.
- Most real ZiFi software (e.g. `debug/NET/MRF.TRD` terminal, drivers ZW-64/ZW-64-SC/GZ-80) uses the **16550 window**, not the API. Verified by disasm: `LD B,#Fx; LD C,#EF; OUT (C),A` + `IN A,(#FDEF)` LSR poll. App sends its own AT commands over the bridge.
- Wired in `Ports::input`/`Ports::output` after the API check.

### The NIC is a serial port, not a WiFi layer — UART-only mode (hw-confirmed DVp2, 2026-09-15)

`ZiFi::enabled` used to be `zifi_enabled && wifi_enabled && arch != A_PROFI`, the
menu row was greyed while WiFi was off (`p_nicAvail` = `p_wifiOn() && p_espSerial()`)
and `act_wifi`'s disconnect branch turned `Config::zifi_enabled` off as well. All
three are gone: **the NIC is available with WiFi off, and that is the point** — the
ZiFi UART is an ordinary 115200 8N1 port, so with WiFi off the guest owns it end to
end and can talk to an Arduino or anything else through the 16550 window (user's
report, `MRF/drivers/uart-zxwifi.asm` as the guest-side driver). Requiring WiFi was
what made `Uart.read` answer 0xFF in exactly that configuration — the ports gate on
`Config::zifi_enabled` (Ports.cpp) but nothing had called `ZiFi::init()`, so there
was no UART behind them. Profi (heap) and the +3e (`#xxEF` collision) still force
it off, and on-chip-WiFi transport still greys the row (`p_espSerial` — there is no
UART there at all).

- **`act_wifi` must not `ZiFi::deinit()` with the NIC on**: that is the guest's UART
  now, not WiFi's plumbing.
- **What the firmware puts on that UART by itself, and when**: nothing, once WiFi is
  off — `netBackgroundTick` (boot join + SNTP) and `netStatusRefresh` (`AT+CWJAP?` /
  `AT+CIFSR`) both already gate on `wifi_enabled || ZiFiAT::connected`. The one
  exception is `zifi_set_baud`'s `AT+UART_CUR=...`, sent from `ZiFi::init()`/`deinit()`
  whenever Network → Baud is not 115200 — so UART-only mode wants the default rate.
  Turning WiFi off itself still costs one `AT+CWQAP` at that moment.
- **`LED::NET` is visible for either half** (`wifi_enabled || zifi_enabled`), so the
  lamp blinks on guest serial traffic with no WiFi — the one-glance "is the program
  even talking to the port" check.
- **Hw 2026-09-15, owner: checked on DVp2, verdict not itemised.** The reported
  case is the one this exists for — a guest talking to an Arduino over the 16550
  window with WiFi off — but the run was on a GPIO-UART transport only: the
  **USB-CDC** transport and the **on-chip CYW43** boards (where the row is greyed by
  `p_espSerial` and there is no UART at all) are still covered by inspection.

### Boot SNTP is a switch: Network → Sync at boot (`Config::sntp_auto`, hw-confirmed DVp2 2026-09-15)

The boot state machine is the only thing that talks to the link unasked, and its
tail is the clock: `AT+CIPSNTPCFG` plus up to 15 `AT+CIPSNTPTIME?` one second apart
(`AS_MAX_ATTEMPTS`), which is noise for anything on that UART that is not an ESP —
and it ran even with the RTC off. `autoSyncBegin(ssid, pass, tz, sntp)` /
`WifiNet::autoBegin(..., sntp)` take a **join-only** flag: the ESP FSM ends at
`AS_DONE` inside `as_to_sntpcfg()` (which is also the CWJAP-timeout target, so a
failed join ends there too) and the on-chip one at `A_DONE` as soon as `linkUp()`,
including the already-associated fast path in `autoBegin`. With it off a boot costs
exactly two AT commands, `CWMODE` and `CWJAP`, and then silence.

- `wifi.cfg` key `sntp`; **absent = on**, so existing cards keep today's behaviour
  (`loadWifiConfig` resets it to true before parsing, like `wifi_tz`).
- The row is `NM_BOOL`, indented under `Sync time (SNTP)`, and deliberately **not**
  greyed with WiFi off (a setting for the next boot, same class as Time zone) — a
  user who turns WiFi off to free the UART must still be able to reach it.
  `SET_SNTP_AUTO` is AC_LIVE without F_PREVIEW: nothing is live to preview, and
  F_PREVIEW would rewrite wifi.cfg on every keypress of an edit that may be discarded.
- Hardware Info reports `SNTP at boot` whenever WiFi is on — "why is my serial
  device seeing AT commands" is answered there.
- The manual `Sync time (SNTP)` action is untouched: it is user-invoked.

### The guest NIC on CYW43 = an ESP-AT emulator over lwIP (2026-10-12; MRF hw-confirmed)

On Transport = On-chip WiFi there is no ESP and no UART, and every guest network
program speaks ESP-AT itself, so the NIC used to be greyed there. Now `ZiFi::init()`
(NIC on + transport 2) allocates the usual rings and starts `AtEmu` instead of a
UART: the guest windows (`#xxEF` 16550/ZIFI-API, ZX-UNO `#FC3B/#FD3B`) and FIFOs are
untouched, only the far end changed. Plan + command inventory:
`~/.claude/plans/zifi-at-emulator-cyw43.md`.

- **`src/net/AtEmuCore.{h,cpp}` is the protocol and depends on nothing** (sockets
  through `AtTransport`). ESP8266 NONOS AT 1.7 framing, because the guests are
  byte-scanners: MRF's `checkOkErr` (`OK\r` / `ERROR` / `FAIL` anywhere), its
  `getPacket` (`+IPD,` -> length to `:`, `...OSED\r` ends the stream), NedoOS
  `esp-com.c` `recvHead` (first `,` then length — so mux 0 has NO id after
  `+IPD,`) and its `espReBoot`, which waits for `WIFI GOT IP` after `AT+RST`.
  Done: AT, ATE0/1, RST/RESTORE (banner + `ready` after 300 ms), GMR, CWMODE*,
  CWJAP* (owner's rule: the guest never moves the host WiFi — up = "connected"
  whatever SSID, down = FAIL), CWQAP (OK, nothing dropped), CIFSR, CIPSTATUS,
  CIPMUX 0/1 (5 links), CIPDINFO, CIPSERVER=0, CIPRECVMODE=0, CIPMODE=0, CIPSTO,
  UART*, CIPSNTPCFG/CIPSNTPTIME? (from our RTC), CIPSTART TCP, CIPSEND (`> `,
  `Recv N bytes`, `SEND OK`, ≤2048), CIPCLOSE[=id|5]; `busy p...` while a connect
  or the RST banner is pending. Not yet: UDP, SSL, CIPSERVER=1, CIPRECVMODE=1,
  CWLAP (all answer ERROR).
- **An announced `+IPD` goes out whole before any queued text** (own header buffer),
  and a link's `CLOSED` waits until its data is drained — MRF ends the page on it.
  A CIPCLOSE mid-`+IPD` pads the promised bytes with zeros so the guest's parser
  stays in step.
- **Host test `tools/atemu_test.cpp`** plays the guest with C++ copies of MRF's and
  esp-com's parsers, output drained in random slices (recipe in its header).
  Four mutations (mux-0 header with id, CLOSED before drain, text inside +IPD data,
  ATE0 ignored) each fail it. **Re-run after any change to AtEmuCore.**
- **`src/net/AtEmu.cpp` = lwIP glue** (own 5 links, never the host's WifiSock
  slots). Guest window = **2×MSS**, set right after `tcp_connect()` (which itself
  sets TCP_WND; the SYN goes out with 5840 but the server sends after our ACK,
  which carries 2920); `tcp_recved` only for what the guest took, and only while
  all guest links together hold < 6 KB. DNS async with a generation tag.
- **Paced at the NIC baud in guest time**: once a frame `credit += ΔT × baud/10 /
  (3.5 MHz << multiplicator)`, capped at 2 KB; that is the rate guest drivers saw
  from a real ESP (the wire was capped at 230400 by `ZIFI_NIC_MAX_BAUD`). Network →
  Baud is enabled on transport 2 for this and changing it does NOT re-init (would
  drop the guest's links). RX ring 2 KB there (data waits in pbufs).
- **TX backpressure**: the ring is drained once a frame, so a full TX ring reads as
  a busy transmitter (16550 LSR THRE = 0 — MRF's zxwifi `write` waits on it; ZX-UNO
  bit 6 already did), and the port write paths call `AtEmu::drainTx()` once the
  ring passes 128 bytes.
- **Cost**: non-W boards unchanged in SRAM (DVp2 `.data`/`.bss` byte-identical to
  HEAD; flash −8 B from the removed `p_espSerial` menu predicate). DVp2ws: +160 B
  static (the `atEmu` tests in the RAM-resident ZiFi handlers), +5.9 KB flash, heap
  only while the NIC runs: `Ctx` (~1.5 KB) + 2 KB RX ring + 0.8 KB TX ring + up to
  ~3.3 KB of pbufs per active link.
- Hardware Info: `ZiFi NIC : On (AT emu, mux N, K links)`. AT traffic is logged
  (`ZiFi AT< ...`) only in `-DZIFI_TRACE=ON` builds.
- Test ELFs `debug/{DVp2w,PCp2ws}-atemu-1.0.9.elf` (+ .uf2). **Hw 2026-10-12, owner:
  MRF works — browsed and downloaded an SCL through it; the zxwifi and EVO UART driver
  builds (16550 window `#F8EF..#FFEF`) and the real UN-64 / UNO UART build (ZX-UNO
  `#FC3B/#FD3B`) all work** — every guest window the emulator serves is confirmed. The AYWiFi56 build
  does NOT, and that is not this emulator: it is the ZX128 RS232 bit-banged through
  AY register 14 at 57600 baud, which nothing in the firmware emulates (it would not
  reach a real ESP either) — a separate feature, see the plan file. ATM-64 does not
  work either, for the same kind of reason: it talks to the ATM Turbo 2+ COM port
  behind the KEYBOARD CONTROLLER (`IN (#55FE)` = command mode, then the command in
  the high address byte: #C3 baud, #C2 RX count, #02 read, #42 status, #03 write),
  which is not emulated. Builds in MRF.TRD: ZW-64/ZW-64-SC/GZ-80 = zxwifi 16550,
  EVO-64 = 16550, UN-64 = ZX-UNO, AY-64/AY-80/AY-64-SC = AY bit-bang, ATM-64 = ATM
  keyboard-controller COM. **Beware UN-64 in distributed MRF.TRDs**: the MRF Makefile's
  `godzilla` target builds the AY56 driver (`-DAY56`) under `BINNAME "UN-64.C"`, so a
  TRD built with it has an UN-64 that is really AYWiFi56 (banner `[AYWiFi56]`) — the
  owner's copy is one. The real ZX-UNO build says `[UNO UART]`; the MRF.TRD in
  `debug/NedoOS/src.r2698/mrabbit-fusion/data/` has it. **Still owed**: NedoOS
  time2/gopher/girc,
  a long CIPSEND, the host FTP server while a guest link is open, NIC off/on and a
  transport switch with links open, `[ZiFi] AT emulator up` in the boot log.

### UART TX/RX pins — runtime, per-board (`src/BoardPins.*`)
- **No compile-time pin define** anymore (old `-DZIFI_TX_PIN` removed). `ZiFi::init()` reads `Config::zifi_tx_pin`/`zifi_rx_pin` and resolves via `BoardPins::resolveZifiPins()` + `uartInstanceForTx()` (authoritative RP2350 pinmux from `rp2350[ab]_interface_pins.json` — the old `(pin/4)%2` heuristic was WRONG for GPIO 8/10/24/26). UART instance/funcsel chosen at runtime; `g_uart`/`g_uart_irq` statics.
- Config sentinels: `0xFE` = board default, `0xFF` = OFF (no UART, FIFO-only), else explicit TX (RX = odd partner). Stored in NVS (`zifi_tx_pin`/`zifi_rx_pin`).
- **Per-board candidate pairs** + defaults live in `BoardPins.cpp` (`#if PICO_DV/MURM2/PICO_PC/ZERO2/#else MURM1_P2`). Defaults: PICO_DV 0/1, MURM2 20/21, PICO_PC 20/21, ZERO2 24/25, MURM1_P2 16/17.
- **Picker**: Network → first row `GPIO x/y` (or `GPIO Off`, `(def)` suffix when unset) → submenu listing `Off` + each board pair with a note (what it displaces, e.g. "off: NESPAD"). On select: save + `ZiFi::deinit()/init()` if NIC on. `BoardPins` is the reusable home for board pin-maps (extend for MIDI etc.).
- **Yield-at-boot**: when a chosen pair shares pins with a peripheral (non-empty note), ZiFi has priority — at boot each conflicting peripheral calls `BoardPins::zifiOwnsPin(pin)` and **skips its own init** if ZiFi owns it: NESPAD (`main.cpp`, moved after `Config::load`, gated by `nespad_active`), MIDI (`ESPectrum.cpp` `Midi::enabled=0`), WAV (`pwm_audio.cpp` skip `inInit`), PCM5122 (`pwm_audio.cpp` skip I2S), AY-clock (`PinSerialData_595.c` via `extern "C" board_zifi_owns_pin`). The displaced peripheral only releases pins at boot, so selecting a conflicting pair **or** enabling the NIC with a conflicting default prompts `OSD_DLG_APPLYREBOOT` (`BoardPins::zifiActiveNote()` non-empty). Defaults that conflict by design: MURM2/PICO_PC 20/21 = NESPAD, MURM1_P2 16/17 = NESPAD.

### +CIPRECVDATA header comes in two dialects (hw-confirmed 2026-09-03)

Passive-mode pulls (`AT+CIPRECVMODE=1`, FTP STOR path) are answered by
`+CIPRECVDATA,<len>:<data>` on ESP8266 NONOS AT 1.7 but by
`+CIPRECVDATA:<len>,<data>` on stock ESP-AT 2.x/4.x — the keyword separator and
the length terminator are swapped. `ZiFiSock.cpp` used to match only the NONOS
form, so a stock ESP-AT module streamed every pulled chunk through as garbage
status lines (Mike V73 had to patch his esp-at fork to emit the old form). Both
are accepted now (`g_hdr_recvdata_v4`); `+IPD` parsing is untouched. Assumes
`AT+CIPDINFO=0` (default, never sent) — with it on ESP-AT inserts `"<ip>",<port>,`
before the data and the ESP-AT form would need two more fields.

### The menu owns the CPU — anything asynchronous must be pumped from `nm::uiIdle` (2026-09-12, NOT hw-tested)

"WiFi connects in 5-10 s if you leave the menu alone, and NEVER connects while you
sit on the Network page." Nothing was wrong with the radio: `ESPectrum::loop` does
not run while the OSD is open, and it was the only caller of `WifiNet::poll()`
(the CYW43/lwIP poll that delivers the join result, DHCP and the SNTP reply) and
of the boot join/SNTP state machine. Both simply froze for as long as a menu was
on screen — including the one page where the user is actually watching for the
result.

- **`ESPectrum::netBackgroundTick()`** is that work, extracted out of `loop()`
  (its two `static`s went with it, so the 4 s boot timer is unchanged). loop()
  calls it once per frame; **`nm::uiIdle(ms)` (UiRender.cpp) calls it and then
  sleeps**, and every key loop in `src/ui/` uses `uiIdle()` in place of
  `sleep_ms(5)`. The classic dialogs the nm:: UI still calls into —
  `showTextDialog`, `msgDialog`, `inlineTextEdit` — pump it directly. Deliberately
  NOT pumped: `errorHalt` (fatal) and the debugger's key loops.
- **`ZiFiAT::uiBusy()`** (its log sink is armed = a WiFi/SNTP page is on screen)
  gates the FSM half. Those flows call `WifiNet::autoCancel()` on purpose — the
  user is driving the radio — and without the gate the now-always-running boot
  timer could fire `autoSyncBegin` while the user is mid-password. `WifiNet::poll()`
  still runs there, which is what the log pane's own wait needs.
- **`WifiNet::autoBegin` starts at SNTP when the link is already up** instead of
  re-issuing `connect_async`: joining by hand from the menu before the 4 s timer
  fires is now reachable, and a blind re-join would drop the association just made.
- `netStatusTick()` (UiActions.cpp) is the nav loop's cheap "did the link move?"
  test — built only from `ZiFiAT::connected` / `autoSyncBusy()`, never a status
  query — so the WiFi row repaints "connecting..." → "On <ip>" live, while the ESP
  path's BLOCKING `getStatus` still runs at most once per transition (lazily, in
  the next `vl_wifi()`).

The general rule this leaves behind: **a menu key loop is a second main loop.**
Anything the firmware advances one step at a time — a radio join, a transfer, a
state machine — needs a call from `uiIdle()`, or it stops the moment F1 is pressed.

### The RX ring is sized by what the link is doing (2026-10-01; hw: owner "все работает", z0p2, not itemised)

The IRQ landing ring was a flat 8 KB (+256 B TX) on the heap for as long as the
link was up — i.e. for the whole session of anyone with WiFi on through an ESP —
plus 512 B of spill staging in `.bss` on every board; `FEAT_ZIFI` budgeted 12 KB.
The 8 KB depth is only ever used by a paused host transfer (SD-write / TLS stalls
between drains at the boosted rate). Now `rxWantSize()` (ZiFi.cpp):

| state | ring |
|---|---|
| link up, NIC off | 2 KB |
| guest NIC on | 4 KB |
| ZiFiSock session (FTP/HTTPS/SSH) | 8 KB |

- **Grow is explicit, shrink is lazy.** `ZiFi::rxSession(on)` from
  `ZiFiSock::ensureRxBuf/freeRxBuf`, and `ZiFi::init()` (idempotent, so the NIC
  toggle reaches it) call `rxRingFit(true)`; the per-frame `rxSpillTick` only ever
  SHRINKS, and only a quiet ring (no spill mode, fill <= want/4). A failed grow
  keeps the current size (logged) and is never retried per frame — the probe is
  `getLargestAllocatable()`. A failed shrink backs off 2 s.
- `rxRingResize` swaps the block with the UART IRQ masked (the only other writer;
  CDC feeds the ring from `tuh_task`, same context) and renormalises the indices.
  Plain `NEED_POINTER`, deliberately NOT the net arena: the IRQ writes into it,
  and a lazy shrink would leave it in the lent prevFB after the session.
- `ZIFI_SWAP_HI` is a quarter of the ring (2048 for the session ring, as before).
  The staging block is the tail of the TX ring's allocation (`g_out_buf` is a
  pointer). `FEAT_ZIFI` = 5 KB.
- **CDC backpressure**: `usbCdcRx` used to pull the whole TinyUSB FIFO and DROP
  what the ring could not take; it now reads only `room` bytes and `rxSpillTick`
  re-pulls (top and bottom) — a full FIFO stops the IN endpoint being re-armed,
  so no callback would ever come for the leftovers.
- **The cost**: with the NIC live the headroom between per-frame drains is 3 KB
  instead of 6 (~130 ms of a late frame at 230400 instead of ~260) before
  `rxDrop` counts. If a title ever shows it, the NIC figure in `rxWantSize()` is
  the one constant to raise.

### Baud ceilings (transport-dependent, `src/speccy/devices/zifi/ZiFi.cpp`)

- Menu (Network → Baud) offers 115200/230400/460800/921600; the link idles at
  `nicSafeBaud()` and paused host sessions (FTP/HTTPS/SSH) `boostBaud()` to the
  configured rate.
- **GPIO UART**: NIC (live Z80) ceiling `ZIFI_NIC_MAX_BAUD=230400` (hw-found: RX-IRQ
  starvation above). Boost unclamped — 921600 (~92 KB/s) hw-verified.
- **USB-CDC** (`Config::zifi_transport==1`): boost ceiling `ZIFI_CDC_MAX_BAUD` is
  **921600** since the vendored TinyUSB 0.21 (host bulk drain ~0.9 MB/s — the old
  ≤0.20 driver drained ~64 KB/s, which overran the CH340's ~256 B internals above
  460800 and was the original clamp reason; 460800 hw-confirmed working, incl. FTP
  download). 921600-over-CDC pending hw-confirm — a CH340 drop shows as Ftp::get
  rx_dropped/short-transfer retries; revert to 460800 if it flakes. NIC (live Z80)
  ceiling on CDC = **230400, same as UART** (`ZIFI_NIC_MAX_BAUD_USB`) — the
  hw-proven value; higher untested since cdcPump landed (possible follow-up).
- **Live NIC over CDC requires `ZiFi::cdcPump()` (hw-confirmed 2026-07-06, fixed
  "MRF hangs on USB")**: CDC has no RX IRQ — every 64 B IN transfer needs a
  tuh_task() pass to re-arm the endpoint, and the CH340 holds only ~256 B ≈ 11 ms
  at 230400. With only the per-frame ZiFi::tick (20 ms) MRF's AT handshake missed
  every poll window and +IPD bursts were truncated. cdcPump (~1 kHz,
  self-rate-limited) has THREE call sites and **ALL THREE are load-bearing**
  (removing any one re-broke MRF on hw):
  (1) guest ZiFi port reads (`ZiFi::read`/`uart16550Read`) — the ONLY pump inside
  `Z80::exec_nocheck()`, which runs MOST of each frame with no per-instruction
  checks (removing this as "redundant with the CPU::loop hook" was exactly the
  regression);
  (2) CPU::loop every ~3500 T-states (`cdcNicActive`) — covers the checked
  while-loops (INT window + frame tail) where exec_nocheck doesn't run;
  (3) ESPectrum::loop frame-pacing waits (v-sync spin / idle delay, up to ~13 ms).
  No-op on GPIO UART.
- **Do NOT raise `CFG_TUH_CDC_RX_EPSIZE` above 64 for serial dongles**
  (hw 2026-07-06): 512 made multi-packet IN transfers chain through the
  double-buffered EPX, but the CH340's constant SHORT packets through the
  ping-pong buffers delivered CORRUPTED data (MRF page = garbage, counters
  clean — no drops, wrong bytes). Multi-packet RX is for full-packet sources
  (MSC) only.
- FTP **upload** over CDC is much slower than download at the same baud — that's
  not the link rate: `sock_send`/chanSend pays an AT+CIPSEND `>`-prompt +
  "SEND OK" round-trip per ~2 KB chunk, so upload is handshake-bound. Raising
  baud barely moves it; fixing it means bigger send chunks or pipelining CIPSEND.

## USB flash stick (MSC host → FatFs volume "USB:")

NOT hw-confirmed yet, except the hotplug toasts (below, 2026-09-11).

- **Hotplug is a LATCH, not a probe** (`UsbMsc::takeEvent`, hw-confirmed
  2026-09-11): a stick is the one storage that comes and goes without changing
  the ROOT volume, so `FileUtils::storageTick()` can never see it — with a card
  mounted it returns at its first line (`if (fsMount) return sdStillThere()...`),
  and a pulled stick has already had `fsMount` cleared by `tuh_msc_umount_cb`,
  so `storageLost()` is not reached either. Hence: the TinyUSB callbacks latch
  (they run inside `tuh_task` and must not paint), `ESPectrum::loop` drains the
  latch BEFORE the storage switch and toasts `MSG_USB_AUTOMOUNT` /
  `MSG_USB_REMOVED`. `Mounted` is latched only after the `f_mount` (the two
  early returns leave a stick that never became a volume) and `Removed` only
  when `ready()` held. Two details that are not cosmetic: `usb_announced`
  suppresses the second toast when the stick is ALSO about to become the root
  volume (`storageTick` reports that as `Online` up to 2 s later, with the same
  text), and `UsbMsc::armHotplug()` swallows everything for 5 s so a permanently
  fitted stick does not greet the user on every boot. **That call belongs at the
  end of `ESPectrum::setup()`, NOT in `initFileSystem()`** (hw 2026-09-11 — it
  greeted on every boot and every F12): `tuh_task()` is pumped only from the
  emulator loop, so with a card present the stick cannot even BEGIN enumerating
  until setup has returned, and a window anchored at the mount expires before
  the event it exists to swallow. Deliberately NOT routed through `StorageEvent`: the menu and
  browser idle loops call `storageTick()` too and discard its verdict, so the
  message would be eaten by whichever ticked first — and a toast raised while
  the menu owns the screen is invisible anyway (`do_OSD` cancels it).

- **FatFs two volumes**: `FF_VOLUMES=2`, `FF_STR_VOLUME_ID=1`, `VolumeStr {"SD","USB"}`
  (ffconf.h). Unprefixed paths → current volume (normally SD) — zero changes for
  existing code; `"USB:/..."` paths flow through `fopen2`/`f_open` everywhere
  (TAP/TRD/SNA/ROM/ZIP loaders work from the stick untouched).
- **USB-as-root fallback**: no SD card at boot → `FileUtils::initFileSystem` waits
  up to 3 s for a stick (`UsbMsc::waitReady` pumps tuh_task — nothing else pumps
  that early) then `f_chdrive("USB:")` + `FileUtils::usbRoot=true` — all unprefixed
  paths (CONFIG_DIR, /tmp, /pico-speccy, storage.nvs) transparently land on the stick.
  SD always wins when a card is present. Requires `FF_FS_RPATH=1` +
  `FF_PATH_DEPTH=16`. **exFAT depth trap** (hw-confirmed, was "video-mode switch
  fails only on USB"): with RPATH on, FatFs's follow_path() on an **exFAT** volume
  records every descended sub-dir into a `tbl[FF_PATH_DEPTH+1]` chain and returns
  `FR_NOT_ENOUGH_CORE` once the path is deeper — so FF_PATH_DEPTH caps EVERY
  absolute path on exFAT, not just f_chdir. `FF_PATH_DEPTH=1` broke every write to
  a big (exFAT) USB stick: the 4-deep config dir couldn't be created, so
  `Config::save()` silently fell back to a RAM buffer and nothing persisted (SD is
  usually FAT32, where this code never runs — hence "USB only"). Keep it ≥ the
  deepest path (config tree is 4; 16 also covers deep browsing).
  **CAUTION**: with RPATH a volume name without a colon parses as "no prefix" =
  current volume — always spell `"SD:"`/`"USB:"` in f_mount/f_unmount. In usbRoot
  mode the F5 chooser hides the USB row and relabels Local→"USB Drive"; stick
  unplug/replug toggles `fsMount` (menus degrade like no-SD); `remountSD()` skips
  the SD reinit and just re-verifies `UsbMsc::ready()`.
- **Boot-race guard for remembered "USB:/..." paths**: boot-time reopeners
  (`Config::loadDiskMounts`, `Tape::LoadRemembered`, DivMMC/IDE image opens) run
  in `ESPectrum::setup` BEFORE anything pumps tuh_task — the stick hasn't
  enumerated, the open fails, and the next `Config::save()` would persist the
  empty live state (paths "not saved"). Every such site calls
  `FileUtils::waitVolumeReady(path)` first (pumps up to 3 s for "USB:" paths,
  no-op otherwise). Stick truly absent → reopen skipped, like a deleted SD file.
- **Throughput: ~64 KB/s cap SOLVED by TinyUSB 0.21 (hw-confirmed 2026-07-06,
  PICO_DV)**. The old cap: TinyUSB ≤0.20 (SDK 2.2.0 bundles 0.18) moves bulk data
  at ~1 packet (64 B) per 1 ms SOF frame → USB MSC hard-capped at 0.05–0.06 MB/s
  regardless of block size (one 32 KB read10 = 558 ms ≈ 64 single-sector reads;
  not the block size / FatFs / stick — the HCD). TinyUSB **0.21.0** reworked the
  RP2 HCD ("EPX for non-interrupt endpoints + ping-pong double buffering") and
  Speed Test jumped to **0.89 MB/s rd / 0.75 MB/s wr** (~15×, near the FS-bulk
  theoretical ≈1.2 MB/s). TinyUSB 0.21 is **vendored at `external/tinyusb`**
  (subset: LICENSE, src/, hw/bsp/rp2040 + family_support) and is the default via
  `PICO_TINYUSB_PATH` set before `pico_sdk_init` — every build gets the fast HCD.
  **The vendored copy DIVERGES from the 0.21.0 tag — grep `PICO-SPEC PATCH` and
  read this before re-vendoring/upgrading!**
  (a) `hcd_rp2040.c` + `rp2040_usb.c` are REPLACED with Rumbledethumps' rewritten
  host driver from picocomputer/rp6502 (`vendor/tinyusb_rp6502`, the author of
  upstream issue #3533) — fixes the silicon quirk family the stock 0.21 driver
  panics on: shared EPX/interrupt-EP handshake latches (false RX_TIMEOUT /
  DATA_SEQ_ERROR while a keyboard poll is in flight → `panic("Data Seq Error")`
  and dongle re-enumeration loops), interrupt-poll suppression around EPX
  transactions, DATA_SEQ-before-BUFF_STATUS ordering, working abort/close, EP0
  MPS tracking per device. Both hw-hit here as whole-firmware panics with ZiFi
  CDC streaming + machine reset + keyboard.
  (b) On top, `rp2040_usb.c` `bufctrl_write32/16` are patched to
  disarm-and-continue in HOST mode on a stale AVAILABLE (leftover of an
  errored/aborted EPX transfer, upstream #3533/#3602 — was
  `panic("buf_ctrl ... already available")`, whole firmware down);
  `rp2usb_stale_avail_fixups` counts, device mode keeps the panic.
  (c) TU_ASSERT's bkpt is routed to a counting no-op (`CFG_TUSB_DEBUG_BREAKPOINT`
  in tusb_config.h → `g_tusb_assert_count` in main.cpp) — otherwise every
  recoverable assert freezes attached-debugger sessions.
  (d) `cdc_host.c` allocates the CDC stream FIFOs instead of embedding them in
  `cdch_interface_t`: upstream's arrays made `cdch_data` **16 KB of permanent
  .bss** (2×`CFG_TUH_CDC_*_BUFSIZE`, sized for 921600 baud) on boards where the
  ESP usually arrives over GPIO UART and no serial dongle is ever plugged in —
  120 B now. Allocated in `make_new_itf` (a NULL return declines the interface),
  released in `cdch_close` **and** in `set_config_complete`'s failure branch
  (that path frees the slot without ever calling close — leak + double-alloc if
  missed). Buffers come from `picospeccy_usb_fifo_alloc/free` (main.cpp →
  `Buffer::palloc`, NOT malloc: pico_malloc panics on OOM). **Consequence: a CDC
  dongle plugged into a full heap does not mount** (logged, not fatal).
  Ported from pico-spec `313b289` (2026-08-10), NOT hw-tested on either side.
  Diagnostics: the ZiFi 1 Hz `ZiFi CDC:` console line reports tx/rx/drop/queues +
  the stale/assert counters.
  Source compat: `usbh_class_driver_t::open` returns consumed length on 0.21+
  (version-gated shim in `xinput_host.h`) and `ps2kbd_mrmltr.cpp` needs its own
  `<cstdio>` (printf leaked transitively from ≤0.18 tusb headers). Still pending:
  long-run regression keyboard+pad+CDC together (the `usbService` pumping model
  was tuned on the old driver); revisiting `ZIFI_CDC_MAX_BAUD=460800` — the
  host-side ~64 KB/s drain limit is gone, but the CH340's own sustained-RX
  ceiling is a separate constraint, so re-test before raising.
- **diskio dispatch**: `src/drivers/sdcard/sdcard.c` routes `pdrv==1` to `usb_disk_*`
  in `src/drivers/usbhost/UsbMsc.cpp` (TinyUSB `tuh_msc_read10/write10` made synchronous by pumping
  a guarded `tuh_task()` — same re-entrancy rules as ZiFi's `usbService()`; NEVER
  pump from a tuh callback). Odd-aligned FatFs buffers bounce per-sector.
- **Mount flow**: `tuh_msc_mount_cb` does NO bus traffic — capacity is cached by
  the host stack at enumeration; it lazily heap-allocs `UsbFsMem` (FATFS + 512B
  bounce, ~1.1KB, `getLargestAllocatable()` gated, never freed) and registers a
  deferred `f_mount("USB:", 0)`. First real FS access initializes from main-loop
  context. Sticks with sector size ≠512 are ignored. `umount_cb` resets a stale
  `ALL_Path` to `/`.
- **UI**: F5 locations chooser gains a "USB Drive" row while a stick is enumerated
  (`f5HasChooser()` = WiFi OR `UsbMsc::ready()`, so it works without a saved SSID);
  SD⇄USB each remember their last dir (`s_f5_sd_dir`/`s_f5_usb_dir`, ALL_Path holds
  the active one and is the one persisted). Per-type paths (TAP_Path etc.) inherit
  USB paths naturally; in their dialogs ".." at `USB:/` exits to the SD root, in F5
  it returns to the chooser. `sorted_files::init` replaces ':' in .idx names (the
  index always lives on SD `/tmp`, so a read-only/removed stick can't break it).
- Stale `"USB:/..."` in NVS self-heals: fileDialog's entry `f_opendir` check resets
  fdir to `/`.

## Internet archive downloader (WIP — TR-DOS/tape images over HTTPS to SD)

Goal: browse/download disk & tape images (vtrd.in, then zxart.ee, worldofspectrum)
over the ESP-01 and save to SD, with minimal SRAM. RP2350-only, behind
`#if ZIFI_NET_CLIENT`. Reuses `RemoteFs` + `OSD::remoteFileDialog`.

### Architecture: host-TLS on RP2350 + serverless GitHub Pages catalog
- **Catalog** built by a **GitHub Action (cron)** → static per-site index files
  (`vtrd.tsv`, `zxart.tsv`, `wos.tsv` + `sites.tsv`) served over HTTPS from
  GitHub Pages. No always-on server. Index line = `type \t name \t size \t locator`
  (`D`=dir/category, `F`=file → absolute download URL). Per-site logic lives in the
  Action; the device stays generic. Fallback for a Cloudflare-hard site (vtrd 403s
  bots): the Action mirrors extracted `.trd` to Pages. **Local :80 proxy** remains
  the documented fallback if host-TLS proves unworkable.
- **Device does HTTPS itself** (the load-bearing decision): TLS runs on the RP2350,
  the ESP-01 is a dumb plain-TCP pipe — **same host-crypto/dumb-ESP split as SSH**
  (`Ssh.cpp`). ESP-AT's own SSL is NOT used (ESP-01S lacks heap for a ~40-50 KB
  handshake and stock AT firmware has no GCM ciphers).

### TLS layer (`src/TlsSock.{h,cpp}`)
- mbedTLS TLS 1.2 client over `ZiFiSock::sock_open(host,443,/*tls=*/false,..)`,
  wired via `mbedtls_ssl_set_bio` (`bioSend`/`bioRecv` → `ZiFiSock::sock_send/recv`).
- `bioRecv` uses new `ZiFiSock::isClosed(id)` to tell clean EOF from a transient
  no-data timeout (returns `MBEDTLS_ERR_SSL_WANT_READ`).
- f_rng from RP2350 hardware RNG (`pico/rand.h` `get_rand_32`), like `Ssh.cpp`'s
  `ssh_rng` — no entropy/CTR_DRBG modules.
- CA verification: `loadCaFile()` parses PEM from `cacert.pem` on SD →
  `VERIFY_REQUIRED`; no CA → `VERIFY_NONE` (bring-up spike only, logs a warning).
  SNI always set via `mbedtls_ssl_set_hostname`.
- **mbedTLS config** (`src/drivers/board/mbedtls_config_picospeccy.h`): TLS stack added on top of the
  SSH crypto primitives — `MBEDTLS_SSL_TLS_C/SSL_CLI_C/SSL_PROTO_TLS1_2/SNI`,
  ECDHE-RSA/ECDSA key exch, `GCM_C`, X.509 (`PK_C/PK_PARSE_C/X509_USE_C/X509_CRT_PARSE_C/PEM_PARSE_C`).
  TLS 1.2 only (no 1.3 → no version pinning needed). Buffers trimmed:
  `SSL_IN_CONTENT_LEN=16384` (cert chains), `SSL_OUT_CONTENT_LEN=4096` (tiny GETs).

### HTTP layer (`src/HttpsGet.{h,cpp}`)
- Minimal HTTP/1.1 GET: `https://`→`TlsSock`, `http://`→plain `ZiFiSock`. Streams
  body to a `SinkCb` (or `getToFile()` straight to SD via `fopen2`/`f_write`),
  static 1 KB buffer off the stack (like `Ftp.cpp` `g_ftp_buf`). Handles
  Content-Length + connection-close bodies; **chunked T-E is rejected** (Pages sends
  Content-Length). Browser `User-Agent` (for vtrd's 403 filter).
- **`HttpsGet::selfTest(url[,caPath])`** — bring-up spike: GET + log status /
  Content-Length / first body bytes. Use to validate TLS-over-ESP on hardware
  before building `HttpCatalogFs`/menu (next: Commit 1).

### On-device "curl" test (`netHttpTest` in `OSDMain.cpp`)
- Network menu row **"HTTP test (curl)"** (RP2350, under `#if ZIFI_NET_CLIENT`).
  Prompts scheme (https/http) + host[:port] + path as **separate fields** (because
  `inlineTextEdit` caps text at the field width — a full URL won't fit one field;
  `netAskField` gained an optional `field` width arg). GETs via `HttpsGet`, shows a
  summary (status / Content-Length / received / ok / **free-heap before→after**) +
  body preview in `OSD::showTextDialog`. This is the on-hardware trigger for the
  TLS-over-ESP spike. Output is summary-only (no SD file).
- **Memory**: curl's preview/summary buffers are alt-stack locals (no permanent
  BSS). mbedTLS stays on the libc heap (heap-first; SSH already runs there). The
  free-heap log lets us see if a TLS handshake (IN16K+OUT4K + alt-stack, all from
  heap) is too tight — if so, route mbedTLS allocs to PSRAM (`PSRAM_DATA`) later.
  SD-swap is NOT usable for TLS buffers (no MMU/demand-paging on RP2350).
