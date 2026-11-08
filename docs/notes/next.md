<!-- Moved verbatim from CLAUDE.md on 2026-11-04; sections in their original order. -->

## ZX Spectrum Next (arch A_NEXT, 2026-08-03, NOT hw-confirmed yet)

Ported from the pico-next donor branch (`~/github/pico-next`,
`claude/zx-next-emulator-plan-fn2go6`); reference emulator for semantics:
`~/github/zesarux/src/machines/tbblue.c`. All Next code lives in
`src/speccy/machines/Next/` (moved there by the 2026-10-07 rebase onto main). Plan:
`~/.claude/plans/spectrum-next-in-pico-speccy.md`. Four commits, in order:

- **8×8K slots (was 4×16K)** — `ramCurrent/ramContended/bank_dirty/bank_access`
  are 8-wide (slot = `addr>>13`, mask `0x1FFF`); legacy 16K paging goes through
  `MemESP::plug16()` (pointer+contention pair) / `plugPtr16()` (pointer-only —
  recoverPage0, TR-DOS automap, Profi SCO, debugger: sites that historically
  never touched contention). Descriptor machinery (`plugged_in[4]`,
  `acc_bank[4]`, sync/materialize/promoteBank) stays 16K-grained. THREE traps
  vs the donor version: (1) `romPeek` looks up ROM overlays by the pair BASE
  pointer (`ramCurrent[0]`) — slot 1 holds base+0x2000 and would miss the
  registry; (2) accessor banks null BOTH slots (`plugPtr16(b, nullptr)`), a
  naive `p+0x2000` would make slot1 = (uint8_t*)0x2000; (3) both slots of a
  pair share one dirty/access hook. Tape flashload walks 8K slots.
  **What the 2026-09-08 rebase onto the TS-Conf line had to extend** (the 8K
  refactor predates +3/+3e, TS-Conf and Scorpion/GMX/ProfROM, and every one of
  those machines writes the slot arrays itself — a 16K-indexed store lands in the
  wrong slot silently, so this list is the audit, not a diff): `MemESP::plus3Remap`
  (both the four all-RAM configurations and the normal map) and `TsConf::setBanks`
  now go through plug16/plugPtr16; so do the GMX #78FD / #DFFD / #1FFD page writes
  and the Scorpion TR-DOS + NMI-DOS `rom[]` plugs. `Z80::blockRepeat` (the
  LDIR/LDDR batch) bounds its run inside ONE 8K slot at each end, treats slots 0
  AND 1 as page 0, and its `dbase == VIDEO::grmem` guard tests `grmem + 0x2000`
  too — slot 3 of the screen page would otherwise batch past the beam. The
  TS-Conf fast-memory helpers in CPU.cpp (`peek8_tick`, `tsPoke8Store`,
  `tsPeek16Load`, the exec_nocheck fetch) index by `>>13` with `& 0x1FFF`, while
  `mem_desc_t::bank_dirty[]` was already slot-indexed; the IM1-ROM0 dead-stub
  check reads page 5 through slot 2; and the PERF_HIST page histograms keep
  16K-grained sources (`TsConf::r.page[pg >> 1]`, `TS_PAGE_HIT` on `>>14`) because
  a TS-Conf window IS 16K. Two collisions were decided rather than merged: the
  Next branch's reference-DVI `tmds_ref_pair` was dropped in favour of this line's
  hw-confirmed `tmds_balanced_pair` (same root cause — see the note in
  drivers/hdmi/hdmi.c, and recheck the NextZXOS backdrop on hardware), and the
  divSD read-ahead burst buffer moved out of SCRATCH_Y — full here, hdmi.c's
  `conv_color_b` owns all 4 KB of it — into a lazy HOT_SRAM Buffer, which is safe
  only because the framebuffer is now reserved at the top of setup() (see the
  framebuffer-first section) and degrades to the single-sector path if the SRAM
  is not there.
- **Machine**: gate = RP2350 + butter ≥2MB (`p_showNext`, MachineSwitch,
  setup() fallback to 128K). MMU `nextRamPtr[224]`: banks 0-7 = static SRAM
  `ram[0..7]`, banks 8-111 + 72K ROM region carved from butter bottom-up in
  `MemESP::buildNextRam()`, capped by `Buffer::pageBudgetButter()` and
  accounted via `butter_pages` so DivMMC/GS/arena compose on top. Next slots
  detach pool dirty hooks (`applyMMU` → dirty_sink). Legacy 7FFD (+3 decode)/
  DFFD/1FFD/0x8E implemented ON TOP of the MMU in NextReg. NextZXOS ROMs from
  SD (`/roms/next/enNextZX.rom` 64K + `enNxtmmc.rom` 8K → after RAM pages,
  write-protected via `next_rom_mask` — the "ROM" is writable PSRAM); MMC ROM
  replaces esxdos in the divMMC automap; 128K ROM pair = fallback. Crossing
  the Next boundary ALWAYS hard-reboots (MachineSwitch) — the layout is decided
  once in setup(). Murmuzavr stays off via the existing non-Pentagon clamp;
  contention/floating-bus off; snapshots are `.pss` since 2026-10-21, hw-confirmed the same day, not itemised (docs/pss-snapshot-plan.md, "Spectrum Next").
- **Z80N** in `decodeED()` under `Z80Ops::isNext` (NOPs elsewhere). NEXTREG
  opcodes → `NextReg::write`. Line IRQ (0x22/0x23) in `isActiveINT`; while
  armed, CPU::loop skips the exec_nocheck/FlushOnHalt fast paths. zxnDMA #6B
  always on in Next mode. Turbo = nextreg 0x07 → `ESPectrum::multiplicator`
  (28 MHz runs at full speed on the 504 MHz host — owner, hw DVp2, 2026-11;
  the old "NOT real-time" note predates the 2026-10-16 Videoland work). 60Hz via
  nextreg 0x05 bit2 → 228×262 frame.
- **Video** (`src/speccy/machines/Next/NextVideo.*`, NEXTVID): scanline renderer — VIDEO::Draw
  swaps to `NEXTVID::Tick` in Next mode, EndFrame short-circuits. Layers:
  ULA/ULANext/LoRes → tilemap → Layer2 (256×192 row-major, 320×256
  column-major) → 128 sprites (scale/rotate/mirror, 4/8bpp, collision) → L2
  palette-priority pass; copper with line-granular WAIT. RGB333 → nearest
  CLUT G3R3B2 (indices 17-219 only; ~203 colours — accepted "Next Lite"
  ceiling, like ≤360px: no 640×256×4bpp, tilemap-80 drawn half-res). Sprite/
  copper/palette state = ONE ~25KB heap block allocated at first
  NEXTVID::Reset — do NOT make these static arrays again, that cost 29KB BSS
  on every legacy build. F8 stats rows skipped by the renderer.
- **Sound**: chip2 = third AY, heap at Next boot (`ESPectrum::nextChip2Init`, from setup() AND reset() — it was reset()-only until 2026-10-21, so after a cold boot / RUN button the third AY did not exist and `NextPorts::ayWrite` sent its writes to chip0: TurboSound music garbled until F11, hw-confirmed fixed); 0xFFFD control 111LRxCC
  (select + `ay_next_pan`, applied in the frame mixer); TurboSubsys forced on
  under Next. DAC ports 0F/1F/4F/5F/DF/FB/B3 → stereo Covox (A/B left, C/D
  right), CovoxSubsys requested when arch==A_NEXT.
- **.NEX loader** (`src/speccy/machines/Next/FileNEX.cpp`): V1.0-1.3, palette + L2/ULA loading screens,
  112 banks; from another machine → save as `Config::ram_file` + reboot
  (setup() skips the pref_arch override when a boot snapshot is pending).
- **Not done yet** (gap list in the plan file): relative/unified sprites
  (anchors only), ULA scroll 0x26/0x27, LoRes radastan 4bpp, horizontal copper
  WAIT, peripheral regs 0x06/0x08/0x09 as working bits, interrupt ctl 0xC0+,
  Next-UART 0x133B→ZiFi bridge, zxnDMA prescaler mode, .SNX.

## ZX Spectrum Next (ветка drew-zx-next, 2026-08-09) — TBBLUE-цепочка hw-confirmed

Machine `A_NEXT` (RP2350 + butter ≥2MB): Z80N, NextReg (`src/NextReg.cpp`),
8K MMU, NEXTVID scanline renderer, .NEX loader. План/гэп-анализ/тест-план:
`~/.claude/plans/spectrum-next-in-pico-speccy.md`. Референс конформности —
**zx_go** (conorarmstrong/zx_go, VHDL_CONFORMANCE.md; их pkg/memory/next.go +
pkg/next/wire.go = разжёванный zxnext.vhd — читать ПЕРЕД гаданием).

- **CPU speed cap**: nextreg 0x07 клампится `Config::next_clk_cap`, который
  с 2026-10-30 зафиксирован на 3 = 28 MHz (без потолка): пункта меню нет, в NVS
  не хранится (`Config.cpp`). Хук `NextReg::applySpeed()` остался (близнец
  TS-Conf `tsconf_clk_cap`); программное значение хранится, read-back отдаёт
  actual в битах 5:4. 28 MHz на 504 MHz идёт в полную скорость (владелец, hw
  DVp2, 2026-11). История: потолок по умолч. 14 MHz из NVS + пункт меню до
  2026-10-30; compile-time `NEXTREG_MAX_TURBO=2` до 2026-09-25.
- **TBBLUE boot chain** (2026-08-09): с `/TBBLUE.FW` на SD бут идёт через
  вшитый 8K bootrom (`src/roms/next/tbblue_loader.c`, GPLv3, из zx_go) →
  config mode → NextZXOS; без файла — прямой бут enNextZX.rom (fallback).
  NR 0x03 — полная VHDL-семантика sub-полей (machine_type только в config mode
  и только 1..4; `v&7==7` re-enter, прочее non-zero — exit; bootrom гаснет на
  ПЕРВОЙ записи 0x03; timing под XOR-замком bit3). NR 0x04 RAMPAGE →
  `MemESP::applyNextConfigWindow()`: чтения слотов 0/1 — bootrom (8K×2) или
  backing страницы (0-3 ROM, 4 divMMC ROM, 6/7 AltROM, 8-F divMMC RAM,
  10-7F RAM), записи — ВСЕГДА в backing через wrOverlay (bootrom гейтит только
  чтение, zxnext.vhd:1856 — у zx_go на этом висел cold boot). Пока окно живо:
  applyMMU слотов 0/1 только латчит, L2-окно 123B не трогает оверлей, divMMC
  automap выключен. Hard reset перевзводит bootrom, soft — нет. **.NEX-лоадер
  принудительно гасит окно** — путь ram_file (ребут с другой машины) грузит
  .NEX сразу после setup(), где bootrom взведён.
- **DivSD на Next — умолчание, не форс** (esxdos=3, реальная карта): MachineSwitch
  ставит его только при ВХОДЕ в Next; clamp'ы в setup сняты 2026-11-02 (владелец:
  Devices > esxDOS должен перенастраиваться на этой машине). Off / DivMMC / DivIDE —
  выбор пользователя, переживает перезагрузку. В .mmc-режиме гость видит образ вместо
  карты: NextZXOS загрузится, если в образе всё, что ему нужно. Хост всё равно читает
  РЕАЛЬНУЮ карту: `/roms/next/*.rom` и `f_stat /TBBLUE.FW` (взводит bootrom). Запуск
  .nex/.bas из F5 (NexLaunch::start) требует divsd_mode. Next с образом не запускался
  ни разу. NOT hw-tested.
- **Порт 0xFF** (Timex/SCLD, полный декод lo-байта на Next): bit6 = запрет ULA
  frame INT (`Z80Ops::isActiveINT`, рядом с NR 0x22 bit2 — гэп из матрицы
  zx_go), биты 5:0 ↔ NR 0x69 биты 5:0; чтение gated NR 0x08 bit2.
  **Запись в #FF НЕ гейтится NR 0x82 bit 0 — сознательное отклонение
  (владелец, 2026-10-08).** zxnext.vhd: `port_ff_wr <= iowr and port_ff and
  port_ff_io_en`, а TBBLUE.FW v1.44 гасит этот бит во ВСЕХ личностях
  48K/128K/+3/Pentagon (включая TC2048 и сам Next = mode 2, NR 82 = DA) —
  ни одна Timex-программа на плате режим не включит (wiki про это молчит,
  zx_go не моделирует). `nextPortFFOverride` (Ports.cpp) пишет всё равно и
  логирует первый такой OUT. Режимы экрана в NEXTVID по zxula.vhd: бит 0 —
  пиксели +0x2000 (второй экран 0x6000/0x7800), бит 1 — атрибуты по адресу
  пикселя в 0x6000 (hi-colour 8×1), при показе теневого экрана (NR 69 b6)
  режим = 000. Тест: `debug/Timex/next_timex_modes.tap` (клавиши 0/1/2/3/6).
  hw-confirmed 2026-10-08 (владелец: «работает», личность TC2048, не детализировано).
- **Timex hi-res на Next: прозрачность paper/ink по ПОЛУпикселям + бордюр по VHDL**
  (2026-10-15, hw-confirmed тем же днём — владелец: «фиксед» на
  `debug/DVp2-next-hires-tr-1.0.10.elf`, тест MrKWatkins `LmxHiRes.snx`; не детализировано —
  LmxHiCol / LmixLoRs после правки на железе не перепроверялись). Было: `nxHiresUlaRow` метил всю ULA-строку `HIRES_MARK` (непрозрачно),
  поэтому в порядках SUL/USL/ULS Layer 2 и спрайты ПОД ULA исчезали целиком (на тесте —
  пропадала вся серая подложка L2 и подписи в этих фазах; priority-зелёный оставался).
  Теперь ink/paper/border идут через `nxRgb` (NR 0x14): lores-пиксель = две половины —
  обе непрозрачны → `HIRES_MARK` + слот пары; обе прозрачны → `NX_TR` (микшер отдаёт слой
  ниже); одна → `HIRES_MARK2` (0xF0, sync-слот, индексом не бывает) + в `hiresRow` «какая
  половина + её ZX-цвет», вторая половина — `nxHiresUnder(x)` = правило порядка NR 0x15
  по буферам слоёв с прозрачной ULA (`s_hrMix` заполняет RenderLayers; orders 6/7 ≈ 1).
  Бордюр (zxula.vhd 419/492-500): ULANext = `128 + attr(5:3)` = paper БЕЗ bright (тест
  сам пишет: ядро 2.00.25+ показывает [130]; раньше брали 138), формат FF → fallback;
  ULA+ в hi-res и ink, и paper — в группе paper (`"11" & attr(7:6) & 1` → 0xD8|c; было
  0xD0|ink); классика — 16+8|paper. Все три функции во flash; `.nxovl` 50 808 из 51 200.
  **Скриншот-тулы врут на Next hi-res**: `profi2png` красит через `profi_palette_live`,
  а Timex-путь пишет ZX-16 прямо в драйвер (`timexHiresDriverPalette`), так что в
  `profi_palette_live` лежит палитра прошлого 640-кадра (`nextPairPalette`) — декодировать
  надо ZX-палитрой; и `profi_lut.bin` = `lut[ink][paper]→slot`, НЕ обратная таблица.
- **NReg0x69.snx: 8/10 → NR 0x69 не зеркалил биты 5:0 в `portFF`; hi-res — per LINE**
  (2026-10-15, hw-confirmed тем же днём — владелец: «работает» на
  `debug/DVp2-next-nr69-1.0.10.elf`, не детализировано). (1) Запись NR 0x69 теперь
  пишет биты 5:0 в `NextReg::portFF` (бит 6 = INT-disable, не трогается): два «write»-теста с
  Timex-битами читали `IN (#FF)` устаревшим. (2) Копер ставит NR 0x69 по областям: hi-res на
  двух строках посреди классического экрана. Раньше `applyTimexMode` звал `timexHiresRequest`
  на КАЖДОЙ записи и побеждала последняя — пар-драйвер не включался, а строки hi-res
  рисовались как hi-colour (чёрная полоса с мусором). Теперь запись только ставит
  `s_hiresSeen`, `NextReg::timexHiresFramePoll()` (EndFrame, перед `pairPoll`) просит драйвер,
  если hi-res был на любой строке кадра; `RenderUlaLine` решает по СВОЕМУ `r69` бит 2
  (`hrMode`): классическая строка в пар-кадре = сплошные пары через `pairMap`, hi-res строка
  без пар-кадра (первый кадр, отказ драйвера, SOFTTV/TFT) = 256-px OR-merge
  (`nxHiresUlaRowMerge`). `.nxovl` 50 904 из 51 200 — **следующее добавление требует поднять
  терм**. Остаток по тесту — артефакты у левого края при переключении (ожидаемы, ядро их тоже
  даёт); бит shadow из NR 0x69 тест кодом не проверяет.
- **SprBig4b.snx — в порядке** (2026-10-15, сверено покадрово с фото платы: ориентации всех
  суб-спрайтов и чередование «||» совпадают). Скобки `[B3:5A:AC:5E]` в строке mID — это NR
  03:05:06:08 (личность/тайминг/периферия), не результат. Попутно: **бит 7 чтения NR 0x03 =
  `nr_palette_sub_idx`** (zxnext.vhd 5894 — ждёт ли NR 0x44 второй байт), у нас был бит 7
  последней записи («config mode»); `reg03Compose` берёт `NEXTVID::pal9Second`, `s_reg03TopBit`
  удалён. `debug/DVp2-next-nr03-1.0.10.elf`, hw-confirmed тем же днём: скриншот `[33:5A:AC:7E]`,
  спрайты без изменений.
- **!NextReg.snx (2026-10-15): единственная красная клетка — NR 0x0A, и это тест, а не мы.**
  Тест пишет 07 и ждёт 07 назад (его таблица — «по ядру 3.1.5»), а `nextreg.txt` и zxnext.vhd
  5912 говорят: бит 2 Reserved, читается 0 → 03, что мы и отдаём (`& 0xFB`). На ядре 3.02
  плата дала бы ту же красную клетку. Жёлтые (09/12, 09/13, 0B/8E) совпадают с фото платы; NR
  98-9B у нас «any» (тёмно-синий) — таблица теста так и говорит, на фото старая версия теста.
  Остальная сетка клетка в клетку. Не править.
- **NEXT_TRACE** (CMake, по умолч. ON в текущем кэше): 1 Hz `NEXT:` state-line
  + `NEXT ev:` кольцо (64) nextreg/легаси-портов/automap с PC. Дизайн — уроки
  NGS-кольца: новейшие в конце, `..` при усечении, флуд (палитра 0x40/41/44,
  копер 0x60-63, спрайты 0x34-39) считается, не пишется. memdump
  (Ctrl+Alt+D → tools/memdump.gdb, секция В КОНЦЕ) дампит NextReg raw + mmu +
  слоты + окно TBBLUE + divMMC — без него зависание в 0x0000-0x3FFF нечитаемо
  (пять кандидатов на владельца слота).
- **divMMC automap на Next**: entry-точки ПРОГРАММИРУЕМЫЕ (NR 0xB8-0xBB);
  NextZXOS ставит B8=0x82 B9=0x00 BB=0xF2 — т.е. tape-трапы это 0x04D7/0x056A
  (биты 4/5), НЕ классические 0x0562/0x04C6 (биты 2/3 у него НУЛИ). rom3-gate:
  трап вне RST-масок только при ROM3 (48K). `$0000`-трап disarmed до выхода из
  reset-вектора (`g_next_automap_armed`).
- **TBBLUE boot chain HW-CONFIRMED 2026-08-09**: bootrom → TBBLUE.FW →
  конфиг-меню → тест видеорежимов (ENTER подтверждает; наш вывод одинаков во
  всех «режимах», NR 0x11 только латчится) → запись config.ini → ROM-ы через
  RAMPAGE → NextZXOS. Понадобились: MB02-фикс, CMD18-континуация, токен-
  фрейминг CMD24, per-block 0x05 у CMD25 и **CMD13 SEND_STATUS** (R2=00 00) —
  FW верифицирует им КАЖДЫЙ записанный сектор; найден дизассемблированием его
  write-процедуры (файл+0x5E00=адрес; командный хелпер получает сырой номер и
  сам делает OR 0x40 — поиск `3E 4D` по бинарнику ложно-отрицателен).
- **SD-протокол: «esxDOS прощает — TBBLUE.FW нет»** (hw 2026-08-09, оба из
  бут-цепочки): (1) CMD18 continuation — R1 отдаётся ОДИН раз на команду; наш
  автопереход между блоками повторял R1=0x00 в 0xFF-паузе перед следующим 0xFE
  («Error reading TBBLUE.FW data!»); (2) CMD24 — после токена 0x05 карта должна
  дойти до 0xFF «готово»; вечный 0x05 = таймаут busy-полла у FW («Error saving
  configuration!» при фактически записанном секторе — в логе `ZC/DivSD wr ...
  res=0` прямо перед ошибкой). esxDOS поллит «не 0» и оба бага не видел.
- **MB02::init() бежит в setup даже при выключенном MB-02** и безусловно звал
  `recoverPage0()` — затирал TBBLUE-plug слотов 0/1 через 30 строк после
  взведения (холодный бут = чёрный экран/128K editor при boot=1; F11 «чинил»,
  т.к. MB02::init не бежит). Теперь Next-ветка: applyMMU+applyNextConfigWindow.
  Поле `s0=`/владелец в NEXT:-строке добавлено ровно этим расследованием —
  флаги врут, указатель нет.
- **divMMC ROM в butter обязан быть write-protected** (hw 2026-08-09, «баг
  №1»): writebyte-путь окна divMMC разрешал запись в page0_lo по эвристике
  «указатель ≥ 0x11000000 = RAM-банк» (она для MB02/MAPRAM). На классике MMC
  ROM во флеше и записи падали сами; на Next enNxtmmc живёт в ПИСАБЕЛЬНОМ
  butter — залётные записи гостя через CONMEM-окно копили порчу: «enNxtmmc.rom
  invalid» после F11 и зависание .tap на 0x0038 (ISR NextZXOS каждым
  прерыванием уходит в enNxtmmc через OUT (0xE3),0x80 — порченый ROM =
  дерейл). Теперь `MemESP::divmmc_lo_rom` ставится в applyMapping/MB02 явно.
  После порчи вылечивает только перезагрузка MCU (setup перечитает ROM с SD).
- **.tap из Browser NextZXOS + личности BIOS (hw-confirmed 2026-10-07)** — бывший
  «известный баг .tap из LFN Browser виснет». Источники: wiki.specnext.dev
  (Specifications, Reference_machines), zxnext.vhd (gitlab SpectrumNext FPGA,
  curl raw), исходник TBBLUE.FW (gitlab.com/thesmog358/tbblue `src/firmware`).
  Четыре дефекта:
  1. **Наш in-ROM трап ленты воровал блоки у NextZXOS.** Automap на `#056A`
     ОТЛОЖЕННЫЙ (после fetch), поэтому `CP A` на `#056A` исполняется из ПЗУ и наш
     трап на PC=`#056B` всегда успевал первым, подсовывая LD-BYTES образ из НАШЕЙ
     деки (чужой заголовок → CLEAR с аргументом 2 → «M RAMTOP no good»; лечилось
     извлечением образа из деки). Теперь наш трап и `Tape::tapeFastMachineOk()`
     уступают, когда `NextReg::osTapeTrapArmed()` (NR 0A b4, NR 83 b0, NR BB b5,
     ROM3 в слоте 0); личности BIOS 48K/128K/Pentagon без divMMC — наш fast load.
     Авто-запуск (loader128) на Next выключен: снимка нет, он только ребутил.
  2. **ROM3 зависит от типа машины** (zxnext.vhd ~2981, `NextReg applyRomSelect`
     → `MemESP::next_rom3`): 48K — ROM 0 всегда и ROM3; +3/Next — `1FFD.2:7FFD.4`,
     ROM3 = оба; 128K/Pentagon — `0:7FFD.4`, ROM3 = 7FFD.4; лок-биты NR 8C
     перекрывают, половина AltROM = alt_128_n. TAP Loader NextZXOS в режимах
     48K/128K/Pentagon переписывает ROM0/1 128-й парой (48 BASIC = ROM1) — по
     старому «romInUse==3» его трап `#056A` не взводился, грузилось в реальном времени.
  3. **Automap гейтится NR 0x0A бит 4 и NR 0x83 бит 0** (divmmc_automap_reset);
     без этого личность BIOS 128K/Pentagon (`divmmc=0`) уходила в enNxtmmc на
     каждом IM1 `#0038` — чёрный paper, белый бордюр.
  4. **Мягкий сброс НЕ трогает NR 05/06/08/09/0A и 82-85** (кроме hotkey-битов 06,
     contention-disable 08, sprite-tie 09; 82-85 — только при NR 85 b7). TBBLUE.FW
     пишет их перед soft reset в личность — мы обнуляли. При power-on 82-85 = FF,
     0A = 0 (при загрузке без TBBLUE.FW — 0x10, некому выставить).
  Диагностика `[NXERR]` (NEXT_TRACE): кольцо 64 RST 8 + опрос ERR_NR раз в кадр,
  срабатывает на M (`#15`) — оставлена. PC-брейкпоинт не различает банки ПЗУ
  (`#34C1` в ROM0 — цикл Browser), для ошибок ROM пользоваться ею.
- **Fast load из меню NextZXOS «Tape Loader»** (2026-10-07, hw-confirmed, owner
  «работает», не детализировано; `debug/DVp2-next-tapeldr-1.0.9.elf`): без .tapein трап ОС (divMMC RAM bank 0
  #387F в 2.09: `LD A,(#2F1C) / AND A / JR NZ` = tapein есть) сам отдаёт загрузку
  деке — `LD A,C / OUT (#FE),A / CP A / PUSH #056B / JP #1FF9` (off-zone RET в ROM3
  #056B, мимо нашего CP A-трапа). `Z80::next_os_tape_decline()` (flash; в RAM —
  тест `isNext && divmmc_mapped` в CP A, +44 B) ловит `CP A` + `ED 8A 05 6B` из
  divMMC, делает FlashLoad и выходит тем же `JP` с #05E2 (RET ROM3) на стеке.
  Сигнатура в divMMC памяти единственная, tapein-ветка (#3896) её не проходит.
  `tapeFastMachineOk()` больше не исключает Next — дека после монтирования стоит
  (иначе Play прокрутил бы заголовок до выбора Tape Loader).
- **60 Гц (NR 05 b2): верхний бордюр на 26 строк короче** (Reference_machines: 128K
  311/261 строк, экран через 63/37 строк после INT). `NEXTVID::computeGeometry`,
  `NextReg::paperLine()` (line IRQ, NR 1E/1F); без этого 17 нижних строк fb не
  перерисовывались (остался футер меню). hw-confirmed 2026-10-07.
- **Тайминг машины NR 03 b6:4 применяется** (`NextReg::raster()`: 48K 224×312/262,
  128K/+3 228×311/261, Pentagon 224×320 — CPU, видео, звук; коммит 32d6bb8b).
- **ULA contention на Next** (2026-10-07, hw-confirmed: FPGA48 / цветные полосы / halt2int в 48K и 128K, `debug/DVp2-next-halt-1.0.9.elf`;
  zxnext.vhd 4481-4492, zxula.vhd 583-600): включена при тайминге 48K/128K/+3, CPU
  ровно 3.5 МГц и NR 08 b6 = 0. Банки по ФИЗИЧЕСКОЙ странице MMU (`NextReg::
  contendBanks`/`pageContended`): 48K — банк 5, 128K — нечётные, +3 — 4-7, 8+ никогда;
  ROM и окно divMMC — нет (`MemESP::applyMMU` / `nextContendRefresh`). Ожидание —
  `NEXTVID::Tick`/`Tick_Opcode` по колонке строки (`contLine/contCol` = tStatesScreen,
  на 60 Гц −26 строк), паттерн 6,5,4,3,2,1,0,0 / +3 1,0,7,6,5,4,3,2 (как wait_st_tab).
  Порты ULA задерживаются только в 48K/128K (на +3 по VHDL лишь WAIT памяти): общий
  флаг `g_ula_io_contend` (`Ports::ulaIoContendRecalc`, из CPU::reset / EvoBase /
  NextReg::contendRecalc) заменил машинное выражение в 4 местах Ports. Быстрый путь
  памяти Next выключен, пока contention активна (он не вызывает Draw). Пересчёт:
  NR 03/07/08, сброс, смена растра. Цена +95 B RAM. Не моделировано: +3-окно на 3 T
  раньше (как у классического +3), I/O-ранняя задержка по старшему байту на +3,
  вечная contention BF3B/FF3B. Мягкий сброс NR 02 (так BIOS уходит в личность ИЗ
  ТУРБО) обязан звать `NextReg::contendRecalc()` после обнуления multiplicator —
  иначе contention выключена до F11 (hw); так же оба пути ESPectrum reset и
  VIDEO::Reset. HALT в contended-памяти: `NEXTVID::haltAdvanceTo` шагает по M1
  (`Tick_Opcode(true)` + R+1), как классический `FlushOnHaltTo` (halt2int HALT).
- **Потактовый бордюр Next** (hw-confirmed тем же прогоном): `VIDEO::DrawBorder` на
  Next = `NEXTVID::borderMark` — кольцо {T смены, СТАРЫЙ цвет} (32); `emitRow`
  собирает до 15 отрезков строки (`NextLineState::brd`, 24 B, в очереди core1),
  окно строки = `L*tpl + s_bcol + x/2` из `VIDEO::tStatesBorder` (константы
  классического бордюра с их +5), квантование ячейками `s_bstep` = 4 T (48K/128K/+3)
  / 1 T (Pentagon): ячейка, начавшаяся не позже смены, ещё старого цвета (как
  TopBorder). Раскраска — `nxPaintBorderRuns` во flash. 7 отрезков было мало: тест
  цветных полос меняет #FE 8 раз за строку. Debug > Paper off работает и на Next
  (paper-строки рисуются как бордюр).
  **720x576 / 720x480 (fb 360): бордюр 48K/128K был на 2 T (4 px) мимо бумаги** (hw-confirmed
  2026-10-16, тест цветных полос, `debug/DVp2-next-bcol576b-1.0.10`). Классические якоря
  `TS_BORDER_360x*` для 48K/128K на 8 T раньше 320-х (подогнаны под 8-px ячейки того
  рендерера), а бумага сдвигается на 10 T (x 52 vs 32); у Pentagon разница ровно 10. Теперь
  `s_bcol` = 320-й якорь − `s_bpad` (доп. бордюр в T), и сетка `s_bstep` в `nxBorderSegs`
  считается от x 0 320-й раскладки (= по растру). 640x480 и Pentagon не изменились.
- **DivMMC NMI + esxDOS-личности** (2026-10-07, hw-confirmed: esxDOS 0.8.9 NMI-браузер
  и LFN Browser v1.02 из личности Pentagon с `ESXMMC.BIN`). По VHDL (zxnext.vhd 2091-2170,
  divmmc.vhd): кнопка Drive = Alt+F10 / NR 02 b2, гейт NR 06 b4 (+ NR 83 b0 для клавиши) —
  `NextReg::divNmiRequest`; `$0066` мапит только при взведённой кнопке, NR BB b1 instant /
  b0 delayed (esxDOS = delayed, его `#0066` = RET); RETN снимает automap (`DivMMC::nextRetn`);
  `doNMI` на Next DivMMC не трогает. Пять дефектов, каждый глушил NMI молча: (1) холостой M1
  в `Z80::nmi()` по PC=#0038 (INT принят на хвосте кадра) срабатывал как entry RST 38 — на
  Next он только тикает; (2) NMI брался после `execute()`; (3) мягкий сброс NR 02 оставлял
  `#E3` (FW работает с MAPRAM → банк 3 вместо ESXMMC) — VHDL обнуляет его на любом сбросе,
  плюс NR 09 b3; (4) `$0000`-трап разоружён после сброса из TBBLUE.FW и после F11 — esxDOS
  не делал холодный init (теперь armed при `nextBootRom`); (5) RAM Next при старте — нули
  (как Karabas/Evo; к NMI не относилось, решение владельца). NextZXOS гасит NR 06 b4 (ROM p2 #0852) — тост «DivMMC NMI disabled» там верный; b3
  (M1) в NextZXOS включён, её NMI-меню — Multiface: Alt+F10 на Next шлёт NMI Multiface, если не
  «только Drive» (NR 06 b4=1, b3=0 — личности с esxDOS), hw 2026-10-08. Карта: FW ищет `/machines/next/esxmmc.bin`, нужен `divmmc=1`; LFN Browser
  выходит сразу без `/bin/browse.cfg` (`.brwscfg` → Advanced → Auto detect device).
  **LFN Browser v1.02 показывает один каталог «`TBUFILE <DIR>`» в корне — баг БРАУЗЕРА, не
  DivSD (hw-confirmed 2026-10-20, патч работает).** Он считает корень = первый сектор данных
  (кластер 2), а BPB карты владельца держит root cluster 798935 (корень перенесён
  дефрагментатором); кластер 2 = файл TBBLUE, разобранный как каталог. esxDOS читает поле
  BPB и потому в порядке. Патченный `BROWSE.BIN` + разбор — `debug/LFN-browser/`
  (README там). Диагноз дал Ctrl+Alt+M: `#BF8E-#BFA6` = разобранный BPB, `#C000` = сектор.
  Исходника браузера нет (bob_fossil публикует только плагины) — патч бинарный.
  Диагностика `[NMIT]` (NEXT_TRACE): после NMI 1500 событий в UART — map/unmap, `#E3`,
  RST 8 с кодом функции, сектора SD, AF/HL.
- **VHDL-сверка 2026-10-08, первая партия исправлений** (`debug/DVp2-next-vhdlfix1-1.0.9.elf`,
  NOT hw-tested): (1) NR 0x43 — активные палитры бит 3 спрайты / бит 2 L2 / бит 1 ULA
  (zxnext.vhd 5391-5393), у нас 3 и 1 были перепутаны; (2) замок #7FFD (бит 5) снимает
  только NR 0x08 бит 7 (3654), чтение NR 0x08 бит 7 = «не заперто» (5906); замок держит и
  #1FFD/#DFFD (3691/3718), NR 0x8E — нет; раньше проверялся NR 0x08 бит 6 (это contention);
  (3) копер: смена режима сбрасывает PC только в 01/11, 10 = продолжить (copper.vhd 70-77);
  (4) **мягкий сброс NR 0x02 = полный NR reset-блок** (zxnext_top_issue5.vhd 880 `reset <=
  reset_hard or reset_soft`, zxnext.vhd 4930-5111 и MMU 4610-4617): `NextReg::reset(false)`
  + `MemESP::resetNextMapping()` + `NEXTVID::softReset()` (клипы, индекс палитры/NR 43,
  копер — RAM палитр/спрайтов/копера переживает); раньше оставались line IRQ, спрайты,
  L2, тайлмап. NR 0x7F reset не трогает. (5) DMA WR3: байты mask/match потребляются, бит 6
  на Next стартует передачу (dma.vhd 572-591); (6) NR 0x8E: 7FFD бит 4 <- d0 при d2=0, 1FFD
  = {d1,d0,d2}. Полный список расхождений с VHDL со статусами (сделано / hw-confirmed) —
  **`~/.claude/plans/next-vhdl-gaps.md`**; отмечать там каждый пункт по мере работы.
- **Контроллер прерываний + CTC** (2026-10-08, NOT hw-tested, `debug/DVp2-next-int1-1.0.9.elf`;
  план `~/.claude/plans/next-vhdl-gaps.md` §1): `machines/Next/NextInt.{h,cpp}` (flash) — 14
  источников по VHDL (0 line, 3-6 CTC 0-3, 11 ULA; UART нет), pulse-режим (32/36 CPU-тактов)
  и hw IM2 (NR 0xC0 b0: защёлка, вектор {C0 7:5, источник, 0}, daisy chain, RETI ED 4D), NR
  0x20/0xC0-0xCE, CTC 0x183B-0x1F3B (28 МГц, /16 /256, цепочка счётчиков, trigger),
  stackless NMI (C2/C3 ловят адрес на каждом NMI). Время — абсолютный 28 МГц такт из
  `CPU::tstates * (8 >> multiplicator)`, переживает смену скорости. Line IRQ теперь на
  hc_ula 255 строки ПЕРЕД заданной (~100 T раньше) с NR 0x64. NR 0x22 b2 = #FF b6 = NOT C4 b0
  — одна защёлка (`NextReg::portFFIntDisable`, `ulaIrqDisabled` удалён). CPU::loop: середина
  кадра Next — один цикл слайсов до `NextInt::nextEventT` при `NextInt::ex` (бывшая только-line
  нарезка); EI/RETI/RETN и взводящие записи режут слайс (`NextInt::wake`). Хост-тест
  `tools/nextint_test.cpp` (рецепт в шапке, стабы `tools/nextint_stub/`) — гонять после
  любых правок NextInt. Не сделано: DMA уступает прерываниям (NR 0xCC-0xCE только хранятся).
- **Декод #7FFD/#1FFD/#DFFD по VHDL** (2026-10-08, hw-confirmed: EYEACHE через esxDOS в
  личности Pentagon; zxnext.vhd 2578-2599): port_fd =
  A1:0 = 01; #7FFD = A15=0 и (A14=1 ИЛИ тайминг не +3) и не #1FFD; #1FFD = A15:12 = 0001;
  #DFFD = A15:12 = 1101. A14 — только в тайминге +3 (NextZXOS); в личностях
  48K/128K/Pentagon декод неполный, как на самих машинах. Было «всегда A14» — EYEACHE
  (IM2: `LD A,#16 / OUT (#FD),A` = #16FD) не подключал банк плеера и падал.
- **Multiface Next = запуск снапшотов из Browser** (2026-10-08, hw-confirmed: DIHalt.sna из
  Browser NextZXOS; `debug/DVp2-next-mf4-1.0.9.elf`): `machines/Next/NextMf.{h,cpp}` по
  multiface.vhd + проводке zxnext.vhd. Загрузчик банка 2 ставит NR 06 b3 (#0809) и пишет
  NR 02 = 08 (#0AB6) — MF NMI; обработчик `enNextMf.rom` на #0066 восстанавливает снапшот,
  пишет адрес в NR C2/C3 (stackless RETN) и NR 8C = A0. Без MF OUT проваливался в #0AB9,
  `RET NZ` брал со стека 5BFF данные снапшота `00 FF` → PC=FF00 (строка пути) → 0000 в ROM 2.
  Три правила, каждое стоило раунда: (1) **NMI MF принимается сразу**, не на границе кадра
  (`NextInt::wake` + проверка `isNMI` в цикле слайсов Next и в двух проверяемых циклах):
  обработчик идёт по пути запуска только при прерванном PC = #0AB8/#0AB9, иначе открывает
  свой отладчик ("Navigate to register…"); (2) **пока окно MF подключено, точки входа divMMC
  молчат** (`sram_divmmc_automap_en` = 0, `g_nxmf_mem` в `DivMMC::preOpcFetch`) — код MF
  проходит по ним, и автомап esxDOS сделал бы RETN в RAM divMMC (белый экран); (3) RETN
  при активном MF не трогает divMMC (zxnext.vhd 4111). Память: RAMPAGE 5 = 8K ROM + 8K RAM
  после AltROM, ROM с SD (`/machines/next/enNextMf.rom`) или из TBBLUE.FW; окно MF идёт через
  поля окна divMMC (`page0_lo/hi`, `divmmc_mapped`) и перекрывает divMMC, L2 write-over и
  AltROM. Порты по NR 0A 7:6 (+3: 3F вкл / BF выкл), чтение порта вкл отдаёт 7FFD/1FFD/DFFD/
  бордюр. `rom=` в строке NEXT: после запуска врёт честно: NR 8C = A0 → читается AltROM (48 BASIC).
  Цена: +320 B RAM (проверка встроена в preOpcFetch). Не сделано: кнопка M1 (NMI-меню MF),
  повторное включение окна по выборке #0066 внутри активного NMI. `[NXJMP]` под NEXT_TRACE
  печатает кольцо на записи сброса в NR 02 (перевзводится каждым запуском MF). Проверено
  на железе 2026-10-08: DIHalt.sna, LmixLoRs/L2Port/L2Scroll/L2Colour.snx, .z80. Ещё не
  проверено: переход в 128 BASIC из меню NextZXOS (тот же путь по zx_go).
- **Окно divMMC главнее окна записи Layer 2** (2026-10-08, hw-confirmed: LmixLoRs.snx из
  Browser, `debug/DVp2-next-mf6-1.0.9.elf`): zxnext.vhd ~3085 обслуживает `divmmc_rom_en` /
  `divmmc_ram_en` раньше `sram_layer2_map_en`. В `MemESP::writebyte` было наоборот: IM1
  esxDOS под игрой с портом 0x123B сохранял SP в 25B8 — в Layer 2, на выходе `LD SP,(25B8)`
  брал старое 5BD3, `RET` уходил на 0000, а ROM3 NextZXOS по 0000 = `JP 3BE8` = NR 02 = 01 →
  меню. Признак «тест запускается и выходит в меню» = падение на 0000 в 48 BASIC; кольцо
  `[NXJMP]` (NEXT_TRACE) печатается на записи сброса в NR 02 и перевзводится каждым запуском MF.
- **Смена частоты посреди кадра обязана двигать курсор бипера** (2026-10-08, hw-confirmed:
  Browser NextZXOS, курсор за край каталога = BEEP; `debug/DVp2-next-beep-1.0.9.elf`).
  NextZXOS пишет NR 07 = 0 вокруг BEEPER и 3 после; `applySpeed` сдвигал `CPU::tstates`, а
  `ESPectrum::lastBeeperTstates` оставался в старых единицах → delta < 0 = ~4e9 → цикл
  `BeeperGetSample` писал далеко за `overSamplebuf` (640 B) и портил указатель кольца
  HDMI-аудио и таблицу строк кадра: SIGBUS на ОБОИХ ядрах сразу (под GDB обработчик не
  успевает в UART — смотреть BFAR в перемешанных строках или Call Stack). Теперь
  `ESPectrum::beeperClockRescale(old,new)` из `NextReg::applySpeed` и `tsClockRescale`
  (TS-Conf — та же дыра), и цикл ограничен размером буфера (в условии цикла: проверка
  внутри раздувала RAM-функцию на 992 B). Pentagon #EFF7 / Profi #028B / GMX #7EFD
  tstates не пересчитывают вовсе — там курсор бипера согласован, но см. TS-Conf ZCLK.
- **Gigascreen на Next** (2026-10-08, hw: «работает»): свой, в `NEXTVID` — только строки
  с одним классическим ULA (Layer 2 / спрайты / тайлмап / ULANext / LoRes / hi-res его
  выключают), prev = 4 бита/пиксель paper (24 КБ PSRAM), смесь 16x16 → ближайший индекс
  NEXTVID (`gigsBlend`, без записи слотов палитры), Auto по флипу #7FFD b3 / NR 69 b6.
  Общий Gigascreen (prev-FB + слоты 17..136) на Next подавлен: те слоты — решётка NEXTVID.
- **Santa / Pogie до 50/60 FPS** (2026-10-10, hw-confirmed: Santa «теперь хорошо» на
  santa4, Pogie 60 Hz «отлично» на pogie1). Santa (L2 + тайлмап + спрайты, ULA off) был
  core1-bound (`c1` 24 ms): микшер = один буфер ula_final (`nxUlaMerge` только при ULA И
  тайлмапе, иначе указатель на слой), `__restrict` + локалы, 12 шаблонов вместо 18
  (хост-тест 19.2 M пикселей против старой логики = 0); `SprRender` — статик в окне
  `.nxovl` (`HOT_SRAM` на тонкой куче отдавал его в PSRAM); быстрый путь целого тайла
  (n==8: слово -> полубайты, X-зеркало = bswap); L2 256x192 по 4 px — без эффекта, строка
  L2 упирается в PSRAM (банк 8+). Pogie был core0-bound: главный цикл не HALT, а опрос
  флага ISR `EI / LD A,(nn) / OR A / JP Z` (55% кадра). `NextIdle` (machines/Next, флеш,
  хук = 2 сравнения в Next-ветке `check_trdos`): после двух равных витков такого цикла
  время уходит к концу среза (следующее событие INT/DMA) целыми витками,
  `NEXTVID::idleAdvanceTo` рисует строки, R += выборки. Только в `exec_nocheck`, IFF1=1,
  не contended, не RZX, PC >= 0x4000; тело — только чтения в A и флаги (LD A,(nn)/(HL)/(BC)/(DE), CP r/(HL)/n,
  OR/AND A/n, BIT), конец — `JP cc` или `JR NZ/Z` назад (хук взятого обратного JR в
  decodeOpcode20/28, только Next). NextSID (`CP A / CP (HL) / JR Z` на E093, 96%
  кадра): 42 -> 50 FPS, hw-confirmed 2026-10-10. NEXT_TRACE:
  `NEXT rlff: ... | idle loop N/fr skipped X%`. Trace-сборка сама тормозит core0
  (`nxJumpNote` на каждом JP/RET) — FPS только по обычной сборке.
- **NXModPlayer (CTC 633 INT/кадр, 4 DAC OUT на каждое) и окно `.nxovl`** (2026-10-10,
  NOT hw-tested кроме FPS 36 -> 41 на nxmod4). Путь прерывания `NextInt` (sync, CTC,
  nextEventT, intLine, ack/reti, dmaDelay; `NXI_HOT` = `.time_critical.nxint`) и
  `NextPorts::output` + `dacWrite` (`NXP_HOT`) — в окне `.nxovl` (объекты
  `NextInt.cpp.o`/`NextPorts.cpp.o` в правиле окна и в OVL_EXCLUDE). CTC без 64-бит
  делений (предделитель/f28 = сдвиги). Микшер с ula_final внутри (`nxMixT<MODE,true,
  true>` при ULA+тайлмап+L2 без stencil). Журналы спрайтов/палитры — PSRAM (общий XIP-кэш
  = когерентно). Окно 29 184 B. **`.nxovl` ДЕЛИТ адреса с `.tsovl` + `.ftovl`**
  (rp2350-memmap.ld, верх под `.dmaovl`): до `CodeOverlay::apply` куча = всё ниже
  САМОГО НИЖНЕГО окна (`__ovl_low_start`) на ЛЮБОЙ машине, и туда должны влезть
  кадровый буфер + `Config::load`; рост `.nxovl` на 6 KB уронил trace-сборку паникой
  malloc в Config::load. Перекрытие вернуло ~29 KB ранней кучи (DVp2 обычная 102 -> 131
  KB). Правило: Next и TS-Conf/VDAC2 никогда не резидентны вместе — `apply` гасит
  tsconf/vdac2 при next, `claimForTsconf`/`claimForNext` отказывают при резидентном
  соседе, `regionsRebuild` сортирует окна по адресу. Растить любое окно = отнимать
  раннюю кучу у всех машин: считать `__ovl_low_start - __end__` против кадрового буфера.
- **NXModPlayer, режим нот: 42-43 -> 49-50 FPS, IDL +2200 (2026-10-11, hw-confirmed на
  `debug/DVp2-nxmod8-1.0.9`, владелец: «отлично, можно остановиться»).** Три шага, каждый
  замерен: (1) `Z80::exec_nocheck_next` в `.nxovl` с `switch` по опкоду, тела обработчиков
  ВСТАВЛЕНЫ в него генератором `tools/gen_next_dispatch.py` -> `src/speccy/z80/
  next_dispatch.inc` (НЕ править руками; `--check` проверяет `NX_DISP_INL` у вызываемых
  напрямую) с аксессорами `NextMemory::` (next_memory.h: запись >= 0x4000 и
  addressOnBusIR инлайн, Next-ветка check_trdos/NextIdle — `nxCheckTrdos`); через таблицу
  идут только префиксы, HALT, CP A, IN/OUT (n), DI/EI, RST. QEMU-стенд: -12% ARM-инструкций,
  ZEXDOC 67/67, memcrc бит в бит. (2) Попытка убрать проверку prefixOpcode из головы цикла
  дала +1% (GCC перезагружал адрес таблицы страниц) — откачено. (3) **CTC: быстрый путь для
  «все активные каналы — работающие таймеры»** (`s_ctcIndep`, `s_ctcZc[4]` в NextInt.cpp):
  у плеера ТРИ таймера (ch0 /16 TC56 = 31250 Гц, ch1 /256 TC35, ch2 /256 TC250, ~706 INT/кадр),
  полный пересчёт всех каналов на каждом событии стоил ~9% core0. Первая версия (один таймер)
  не сработала ни разу — `fast=0`, поэтому сначала мерить конфигурацию. Любая запись в CTC
  (s_ctcNext = -kNever) возвращает полный путь. Дифф-тест fast/full (1000 сценариев, до 4
  каналов, таймеры+счётчики, `-DNXI_NO_SOLO` выключает быстрый путь) + 4 мутации — в
  сессионном scratch, не в tools/. Окно `.nxovl` AUTO 48 128 (содержимое 47 848, trace 48 120).
  **Профилирующая сборка** (`-DPERF_TRACE=ON -DPERF_HIST=ON -DNEXT_TRACE=OFF`, окно 49152):
  раз в секунду `NXPROF` — гистограмма опкодов, второй байт CB/DD/ED/FD, guest PC (32 B),
  INT/кадр, MMU, строка `NXPROF ctc:` (fast/full, записи/чтения по каналам, состояние) и
  `PROF` (SysTick PC core0). Адреса `.nxovl` резолвить ТОЛЬКО по символам этой секции
  (окна делят VMA, addr2line отдаёт символ `.tsovl`). Остаток в режиме нот: ядро Z80 ~40%,
  ED-инструкции ~3% (NEXTREG n,A, RETI, SWAPNIB, TEST n, SBC HL,DE, LD rr,(nn) — следующий
  кандидат на вставку в switch), OUT/DAC ~3%, аудио-ISR ~2%.
- **NextBench / NEXTEST 640: рендер на двух ядрах, 31 -> 50 FPS (2026-10-11/12, hw-confirmed:
  NEXTEST «отлично» на `debug/DVp2-next-split17-1.0.9`, LED-индикаторы и цвет под ними
  работают; split3 «работает» на NextBench).** Очередь строк общая: `nx_w` (core0 пишет),
  `nx_c` (claim), `nx_r` (release = самая старая строка в работе); hardware spinlock без
  запрета IRQ (HDMI ISR core1 его не ждёт). Правило claim: журналы спрайтов/палитры
  применяются при claim; строку можно взять, пока другое ядро рендерит, только если ей
  нечего применять (`seq <= spr_app/pal_app`, `!sprR->dirty`); core0 не берёт Timex
  hi-res строки. `c0Help()` (flash) рендерит самую старую строку, пока core0 ждёт в
  `nxWaitUntil`/`c1Drain`, только со СВОИМ scratch'ем: 5 376 B строк + 2 560 B полосы
  Layer 2, два блока из кучи (`tryMalloc`, запас 3 КБ — правило HOT_SRAM «8 КБ запаса»
  не проходило на trace-сборке и core1 рендерил один), берутся в `pairPoll` пока жив
  640-режим, отдаются в `pairForceOff` и `nm::gfxBegin` (меню/F5). Контекст рендера
  (`NxCtx`, выбор по SIO CPUID) на ядро; состояние кэша полосы L2 — у буфера (`NxBand`),
  не у ядра. fb при переключении 320<->640 и смене 16 цветов ПЕРЕКОДИРУЕТСЯ на месте
  (`p80RemapFb`/`p80RemapPal`, `getPairSlotReverse`), а не чистится — мусор при смене
  режима был наш; подъём окна на 8 строк — по содержимому кадра (`s_tall`), а не по
  25-кадровой задержке pair-off (картинка «уезжала вверх»). 16 цветов на 640: байт =
  `profi_pair_lookup[l][r]`, 16x16 = все 256 слотов; динамический аллокатор пар (как
  ts256) — отложен. Шаги по NEXTEST (109 спрайтов 16x16 4bpp, 80-кол. текст, L2 вкл.;
  замеры — trace `NEXT vid:`): текстовый тайлмап 640 за ОДИН проход (обе фазы, цвета
  пересчитываются при смене атрибута; нечётный X-скролл — старый `tmTextSpan`) 13.7 ->
  5.1 мс; гистограмма пар каждую 4-ю строку; строка спрайта без поворота читается по
  порядку, палитра один раз; **Layer 2 пропускается, когда вся его палитра — один
  прозрачный цвет** (`s_palMono[8]`, лениво по `s_palMonoDirty`, флаг ставит каждый
  писатель `rawPal[]`): у NEXTEST палитра L2 вся чёрная над NR 0x14 = 0, а в банках
  идёт тест памяти — 192 строки PSRAM за кадр впустую, 5.1 -> 0.3 мс (пропуск нулевых
  и однотонных строк НЕ срабатывал — строки не такие, дамп палитры это и показал);
  блит спрайтов: 32-битное покрытие, немасштабированный спрайт — маска строки одним
  словом, `-O2 no-unroll` (4 308 -> 1 208 B) — дало лишь 5.4 -> 5.0 мс: стоимость —
  чтение 8 байт паттерна из PSRAM на строку спрайта (1 744/кадр) в очереди за XIP-
  трафиком гостя (NEXTEST гоняет PSRAM, `xip=490k/кадр`), паттерны (16 КБ) в SRAM не
  влезают; Timex hi-res путь ULA (`nxHiresInkPaper`/`nxHiresUlaRow`/`nxHiresPairRow`)
  — во flash: окно `.nxovl` 50 952 -> 47 248 B (trace 47 608). Тупики: слитый
  pair-encode+копия в fb (7.2 -> 9.6 мс, откачено), буферы SD в SRAM (не причина IDL).
  **RAMTEST (1MBTEST.NEX) — предел core0**: Z80 ~18 мс/кадр на 28 МГц, т.к. тест бьёт
  PSRAM через XIP; рендером не лечится. **LED-индикаторы на Next не рисовались
  никогда**: Next-ветка `VIDEO::EndFrame` возвращается до `LED::draw()`; теперь
  `LED::decay/draw` из неё, панель вырезается из копии строки (`VIDEO::setLedCarve`,
  у неё свой фон), полоска НАКЛАДЫВАЕТСЯ на уже отрендеренную строку в `RenderLine`
  (`LED::stampStripRow`, `setLedStrip`) — вырез замораживал бордюр под ней. Trace:
  `NEXT rows/mix/spr`; замер слоёв — каждый 16-й job со сдвигом на кадр (фиксированные
  строки давали `out=` от 2.9 до 7.2 на одном коде); trace-сборка сама стоит ~2 мс/кадр
  core0 — FPS только по обычной. `tools/memdump.gdb`: у блока `if Z80Ops::isAtm`
  потерялся `end` (b25db6b3) — Next-блок и `picospec_gdb_done` оказались внутри, дамп
  «висел»; `MemESP::nextStrip + x` с пробелами и `wr_overlay_active` (теперь `_mask`)
  роняли его под `logging redirect`; блок ядер — только PC/LR/SP текущего ядра:
  `thread apply all` (даже printf) переключается на ядро в `.nxovl` и вешает GDB.
- **Ядро Z80 под RAMTEST-цикл (2026-10-12, NOT hw-tested, `debug/DVp2-next-core1-1.0.9`
  + `-trace-`).** QEMU-стенда `tools/z80bench` на машине нет (нет qemu-system-arm/glib) —
  замер статический: `exec_nocheck_next` дизассемблирован (`objdump --disassemble=
  _ZN3Z8017exec_nocheck_nextEb`, таблица `tbh` индексируется `opcode-1`), по каждому
  опкоду цикла RAMTEST (bank 5 #6E63: PUSH HL x8/ADD HL,DE/DJNZ, затем POP DE/CP E/JP NZ/
  ADD A,C/CP D/JP NZ/ADD A,C x128) посчитаны ARM-инструкции. Вывод: **на инструкцию Z80
  ~29 ARM фиксированных** (tsFastTick 5, divmmc 2, fetch 4, regR 3, PC 3, prefixOpcode 3,
  opCode/flagQ/pendingEI/lastFlagQ 4, диспетч 3, хвост `tstates < stFrame` 4) + тело
  30-35 — т.е. >50% времени вне тел. Сделано: (1) **флаги add/adc/sub/sbc/cp/add16 без
  ветвлений** (H = бит 4 `a^op^res`, V = бит 7 `(a^res)&~(a^op)` >> 5; `cp` берёт биты
  5/3 из операнда, не из таблицы) — ДЛЯ ВСЕХ машин, бит в бит со старыми формулами на всех
  (A, op, carry) (`alu_check.cpp` в сессионном scratch: исчерпывающе 8-бит + 60M add16,
  две мутации ловит); (2) `JP/RET/CALL cc`: условие вычисляется в case (`nx_cc = ...`,
  генератор `CC[]`), общее тело по метке — вместо `bl Z80::condMet` на каждом условном
  переходе; (3) **gate-free аксессоры `NextMemory::*_nx`** только для вставленных тел
  (`g_next_fastrd` в цикле инвариант: табличные обработчики и префиксные декодеры —
  единственные, кто может его уронить портовой записью — выходят из цикла сразу
  (`lastFlagQ = flagQ; return;`), так что тест `ldr/ldrb/cmp/beq` на каждый доступ не
  платится; `peek16_nx`/`poke16_nx` — один поиск слота, если слово не пересекает границу
  8K; `nxPush`/`nxPop` через них). Итог по статике: PUSH HL 50 -> 33, DJNZ 47 -> 35,
  ADD A,C 40 -> 34, CP E 39 -> 36, JP NZ ~48 -> ~41 (без вызова), POP без изменений;
  на блок RAMTEST ≈ −6..8% ARM-инструкций, `.nxovl` 47 248 -> 46 576 (trace 46 952),
  `.data` −72 B. Честно: мало — главный остаток это 29 фиксированных на инструкцию и
  регистры Z80 в памяти (каждый PC/SP/HL/F — load/store); дальше только ядро с
  регистрами Z80 в регистрах ARM внутри цикла (спиллы вокруг вызовов) — отдельная
  большая работа. Не взято: `regR` в локальной переменной (−2 insn/instr; `NextIdle`
  читает `Z80::regR` — нужна синхронизация вокруг `nxCheckTrdos`), объединение теста
  `stFrame` с line-tick. Hw owed: NEXTEST FPS не хуже split17, RAMTEST FPS, любая игра
  на 48K/Pentagon (флаги — общие).
- **NXModPlayer, сессия 2026-10-14/15 (hw-confirmed на `debug/DVp2-next-opt6-1.0.10`, owner:
  «работает», IDL вырос).** Четыре шага, каждый с железным вердиктом:
  (1) **Регрессия 48 FPS / IDL −1000 (было 49-50 / +2200) — перенос ZX-страниц 0-4/6 в butter
  13-го числа**: на Next это банки MMU 0/1 = слоты 6/7 (`#C000-#FFFF`), где у плеера главный цикл
  (`#E004`), CTC ISR (`#FA00`) и микшер — ~66k инструкций/кадр через XIP-порт, который core1 и так
  забивает Layer 2. Улика: `MEMORY SETUP begin, freeHeap=` 75 512 (11-е, FB в куче) против 141 744
  (FB в полосе). Фикс в `donateZxPagesToFb`: на Next (`arch == A_NEXT`, или `pref_arch == A_NEXT`
  без ram_file — предикат `CodeOverlay::apply`) из блока pages0123 отдаются только страницы 2/3
  (+4/6); банки 0/1 в SRAM. 480p: 204 строки из полосы, 11.5 KB из кучи. Hw: 50 FPS, IDL +1500
  (480p) / +500 (576p). **Правило: Next-банки 0/1 — это верхние 16K каждой .nex-программы; их
  место в SRAM.**
  (2) **Профиль opt4 (`DVp2-next-opt4-prof`, PERF_HIST, окно 53248)**: core0 = голова
  `exec_nocheck_next` ~34% (PERF_HIST раздувает её), idle 10%, NextInt sync/ctc/pulse ~5%,
  dacWrite+Ports ~2%. Гость: 63.7k ops/fr, 706 INT/fr, ED 8.4% (NEXTREG r,A 1.9% = перемапка
  MMU слота 5 в микшере `#D83D`/`#C940`, RETI 1.1, TEST n 0.7, SBC HL,DE 0.7, LD DE,(nn) 0.6),
  OUT (n),A 3.9%, CB 2.6% (SRL A ×3 в `#D675`). Два ложных следа: **`nextLdirBatch` НЕ горяч** —
  бакет `20071A40` накрывает его эпилог и начало головы цикла (LDIR-ы плеера — инициализация);
  **«4 вынесенных сравнения CB/DD/ED/FD перед `tbh`» — артефакт prof-сборки**, в обычной их нет.
  Бинарник плеера: `~/Downloads/zxnext/apps/audio/NXModPlayer/nxmodplayer.nex` (bank 0 = `#C000`,
  старт `#C004`; дизассемблировать банк через `tools.z80disasm.disasm_bytes_text`).
  (3) **opt5: `CPU::tstates` (`nx_t`), `regR` (`nx_r`) и `opCode` (`nx_op`) живут в регистрах на
  весь срез `exec_nocheck_next`**: аксессоры `_nx` в next_memory.h получают `uint32_t& t`,
  `NextOpcode::fetch_nx`, `nxSync(expr)` публикует/перечитывает глобалы вокруг КАЖДОГО выхода
  наружу (tsDrawTick, табличные обработчики, decodeED/DDFD, NextIdle::note, nextOpcodeSlow,
  fallback-пути аксессоров); генератор добавляет `nx_t` первым аргументом и `Z80Ops::poke16` →
  `poke16_nx` (LD (nn),HL ходил через generic). В ED-switch добавлены TEST n, LD BC/DE,(nn),
  SBC HL,DE, RETI, NEXTREG n,A / n,n (через nxSync + повторный тест gate — writeHot может уронить
  его). z80bench (режим Next, база = HEAD через `git stash` объектов): ZEXDOC 300M T 2242.7M →
  2042.3M ARM-инструкций (**−8.9%**), mix −3.8%, регистры и memcrc бит-в-бит; полный ZEXDOC
  4 шардами — 0 ошибок (шард 0 добивал aluop на момент записи). **Правило для любого вызова из
  этого цикла: всё, что читает или двигает `CPU::tstates`/`regR`, обязано идти через `nxSync`.**
  (4) **opt6: собственный Gigascreen NEXTVID выключен на не-ULA кадрах.** Общий Gigascreen на Next
  и так подавлен (`gigascreenModeIncompatible`), а `gigsFrame` при On/Auto гнал каждую ULA-строку
  по побитовой ветке + 128 B prev в PSRAM даже в режиме «только запись». Теперь кадр со
  спрайтами/LoRes (NR 15), тайлмапом (6B), Layer 2 (69 / 123B), ULA off / ULA+ (68), ULANext
  (43), Timex hi-res или 640 → `s_gigs = 0`, при возврате в чистый ULA-кадр — seed. Hw: FPS
  вернулся.
  Побочное: `.nxovl` 50 648 из AUTO 51 200 (**552 B запаса** — следующее добавление туда требует
  поднять терм); **NEXT_TRACE-сборка больше не влезает во флеш** (жёсткий пол GM.DLS, 4 KB
  сверх — opt3-trace стояла ровно на границе); prof-сборка живёт в `build-all/PICO_DV-nxprof`
  (`-DPERF_TRACE=ON -DPERF_HIST=ON -DNEXT_TRACE=OFF -DNXOVL_WIN_SIZE=53248`). Остаток на core0 —
  голова цикла ~24 ARM-инструкции на Z80-инструкцию (tick 5, divmmc 2, fetch 4, R 1, PC 3,
  prefix 3, flagQ/pendingEI 2, dispatch 4); следующий уровень — PC в регистре (переписывание
  `REG_PC` в телах генератором), дальше только регистровое ядро.
- **STNICCC Filled8bit: 27 -> 50 FPS, zxnDMA-заливки (2026-10-15, hw-confirmed на
  `debug/DVp2-next-otir2-1.0.10`, owner: «теперь всё отлично» — STNICCC 50, TX-1696 в
  порядке).** Демо заливает КАЖДУЮ строку полигона одним DMA-блоком: `8834: EXX / LD
  HL,8841 / LD BC,0D6B / OTIR` = 13-байтная программа (A = фикс. байт цвета, B = L2 ++,
  burst, LOAD, ENABLE) ~540 раз/кадр, ~49 KB в Layer 2 через окно записи 123B (бит 3 =
  теневой L2, NR 13; двойная буферизация флипом NR 12/13). Четыре правки:
  (1) **`nxVideoDrain` при 123B.0 делал ПОЛНЫЙ `c1Drain(0)` на каждый блок** — 7 мс/кадр
  core0 в ожидании core1 (`drain=4200 x 7ms/fr` в трассе). Теперь окно резолвится в
  реальную L2-страницу (как в `NextReg` для 123B) и дренаж — по диапазону и только если
  банк «видео»: отображаемый (NR 12) ИЛИ тот, что читает **самая старая строка в очереди
  core1** (`NEXTVID::c1OldestL2Bank()`, два сравнения) — без второго условия полигон
  появлялся на строку раньше (hw: после флипа демо заливает банк, который core1 ещё
  дорисовывает). (2) **`nxBatch`: память→память идёт ПРОГОНАМИ** (`nxWrRun`/`nxRdRun`:
  назначение резолвится раз на 8K-слот по правилам `writebyte` — divMMC, wrOverlay, ROM,
  accessor → побайтно; memset для фикс. источника, memcpy/байтовый цикл при перекрытии);
  память→порт (паттерны TX-1696/NEXTEST) осталась побайтно в SRAM, редкие случаи (ROM-
  назначение, порт-источник, декремент) — `nxByteSlow` во flash, иначе окно не влезало.
  **SUPERSEDED 2026-10-19 (c4bc9438, владелец: «надо делать по спеке»): длина
  блока зависит от РЕЖИМА чипа, как в VHDL** — zxnext.vhd 1816 берёт режим из
  последнего тронутого DMA-порта, dma.vhd 482/664 грузит счётчик 0 в zxn-режиме
  (#6B на Next) и -1 только в Z80-режиме (#0B), передача идёт пока counter <
  length: #6B = ровно `length` байт, #0B = `length + 1`; вне Next (MB-02+,
  DATAGEAR) всегда +1. С +1 на #6B неправильно рендерится Atic Atac Remake.
  Демо из `/!4TEST/DMA/` (0x1F через #6B) теряет правую колонку — так же, как на
  настоящем Next; это не баг. (2026-10-20: «+1 на #6B в личностях 48K/128K/Pentagon»
  было собрано и ОТКАЧЕНО по решению владельца — zxnext.vhd 1817 берёт режим только
  из адреса порта, личность на него не влияет; не повторять.) **Вне Next меню DMA
  выбирает ЧИП (2026-10-20, hw-confirmed DVp2 тем же днём на `debug/DVp2-zxndma-real-1.0.10`,
  владелец: «работает», не детализировано):** «Z80 DMA»
  (1) = Zilog на обоих портах, всегда `+1`; «zxnDMA» (2) = контроллер Next с его
  правилом порта (#6B ровно `length`, #0B `+1`). До этого значение 2 вне Next ничего не
  меняло — `z80Mode()` возвращал true для любой не-Next машины. Защёлка `nextZ80Mode`
  ставится на каждом чтении и записи DMA-порта на всех машинах (VHDL 1816 — чтение тоже;
  раньше чтения её не трогали и на Next), сброс — zxn. Не сделано: read-back счётчика
  (RR1/RR2) вне Next отдаёт обратный `byte_counter`, не счётчик VHDL. Текст ниже про «+1 на
  обоих портах» — история. — остаток
  считать в uint16 (`(uint16_t)(block_length - nx_counter)`); первая версия сравнивала
  в 32 битах и пропускала блоки TX-1696 целиком (спрайты появлялись/исчезали случайно,
  hw). Раньше `nx_counter` стартовал с 0xFFFF только для #0B, а на zxnDMA (#6B) — с 0,
  т.е. блок шёл ровно `length` байт; это стоило ПОСЛЕДНЕГО байта каждого блока zxnDMA.
  Нашёл демо атрибутного мультиколора из `/!4TEST/DMA/` на Next: `LD C,#6B` / `OTIR`
  пишут WR0 A=источник, WR4 B=0x5800+r*32, `CF`(LOAD), `B3`(FORCE READY), `87`(ENABLE),
  а длину задают один раз в начале кадра — `1F 00`, т.е. 0x1F на 32-байтную строку
  атрибутов (WR3 = N-1, как у Z80 DMA). У нас 31 байт → последняя клетка каждой из 24
  строк оставалась очищенной (0x38): дамп подтвердил побайтово — dest 0x5AE0[0..30] ==
  src 0xFDE0[0..30], dest[31]=0x38 при src[31]=0x08, и в остальных 23 строках так же
  (`cnt == block_length` при `cur == base+31` — старт с 0). Теперь 0xFFFF для обоих
  портов (`doLoad`/`doContinue`/`nxReload`), `nextZ80Mode` остался только как метка
  порта. **hw 2026-10-17, владелец: «фиксед»** — демо из `/!4TEST/DMA/` рисует правую
  колонку (`debug/DVp2-dma-zxnlen-1.0.10`); не детализировано, так что регрессия
  owed: STNICCC Filled8bit (#6B, заливки спанов — лишний байт цвета в конце спана),
  NEXTEST (паттерны через #6B), TX-1696 (#0B — не менялся). (3) **`OUT (#6B)` — быстрый декод в `Ports::output`** рядом с 243B/253B/DAC (7000
  OUT/кадр шли через всю цепочку, 11% core0). (4) **`Z80::nextOtirBatch`** (ED B3, C =
  6B/0B, порт разрешён NR 82-85, B > 2, источник ≥ 0x4000): итерации кроме последней
  одним циклом, 21 T ДО каждого `Z80DMA::writePort` (ENABLE запускает заливку с
  правильного T), выход при `tstates >= stFrame` (запущенный DMA дошёл до конца кадра /
  `NextInt::wake`), последняя — штатный `otx`. Профиль до: `nxBatch` 10-24%, Ports::output
  11%, OTIR 16% инструкций; после: memset 1.3%, OTIR-путь ~3%. Hot-page pool (bank 2 с
  кодом в SRAM) НЕ помог ни до, ни после — выборка кода из PSRAM тут не узкое место.
  Диагностика: `tools/memdump.gdb` + `tools/z80disasm.py` по дампу (строки с ДВОЙНЫМ
  пробелом после 8-го байта!) назвали порт и программу DMA за один шаг, где `NEXT io:`
  выпадала из-за UART; `NEXT dma: n=0` в трассе ВРЁТ — считается только `nxRunSync`, не
  `nxService`. PERF_HIST: `PCPROF_SHIFT` (CMake, 6/8) — 256-байтовые бакеты, иначе
  13 KB `exec_nocheck_next` размазан по 200 бакетам и 40% семплов «unlisted»; 4 строки
  `PROF`; SysTick reload перечитывается при смене clk_sys (старт до переключения на
  `cpu_mhz` печатал 252 на плате 504). Окно `.nxovl` 51 112 из 51 200 (88 B). Trace/prof-
  сборки больше не влезают во флеш на 16 KB (жёсткий пол GM.DLS): в их build-dir
  (`PICO_DV-nxtrace`/`-nxprof`) `__gm_bank_hard_min` в СГЕНЕРИРОВАННОМ ld опущен руками до
  1600 KB — только для диагностики на DVp2 (банк в PSRAM), пересоздаётся при reconfigure.
- **Alien Neoplasma intro: 47 FPS / IDL −1000 → 50 (2026-10-15, hw-confirmed, owner: «теперь
  50»).** Статичный экран ждёт клавишу busy-wait'ом `IN (#FE) / ... / CALL → IN (#1F) /
  JR Z` — ~5.6k витков и ~11k `IN` за кадр на 28 MHz. `#FE` на Next уже имел ранний путь
  (`nextKeyboardRead`, только без вставленной ленты), `#1F` шёл через всю цепочку декода
  (~40 проверок других машин) до `NextPorts::input` во flash. Теперь ранний путь `IN
  (#1F/#37)` → `NextPorts::joyPortRead` (обёртка над `joyRead`, тот же ответ/тайминг),
  сразу после `#EB` и `#FE` в `Ports::input`; RAM +96 B. Дамп читается так: `SP+02 = B936`
  = адрес возврата в главный цикл, `PC=84C7` внутри Kempston-чтения. Остальное в этом демо:
  IM2 `85D9` — NEXTREG MMU slot 0 + TurboSound (2 AY, `#FFFD` select FF/FE) + спрайты
  `#57/#303B`; Layer 2 + ULA + спрайты одновременно (core1). Общий урок трёх правок дня:
  **на Next любой порт, который программа читает/пишет в цикле, должен иметь ранний путь
  в `Ports::input/output` (образец `#EB` / `#243B`) — генеральная цепочка стоит ~0.5-1 мкс
  на доступ.**
  **2026-10-19, повтор с лентой в деке (hw-confirmed DVp2 `debug/DVp2-next-tapekbd4-1.0.10`):**
  заставка — `DI` + `IN A,(#FE) / CPL` + `CALL` Kempston, `JR Z` назад. Без ленты `kbdPollSkip`
  её перематывал, с лентой `Tape::stoppedReadIdle` отказывал любому `IN A,(n)` (`0xDB` по pc-2).
  Теперь отказ только если следом проверка фронта — `Tape::edgeTestOps`, общая с
  `isTapeEdgePoll` автостарта. Это открыло перемотке TBBLUE.FW: его меню (6B6D-6C3B) опрашивает
  клавиши 65535 проходов со счётчиком в ПАМЯТИ (`(#CCE7)`), каждый проход стал срезом —
  загрузка ползла минутами. `pollSkip` теперь (а) сравнивает хеш BC/DE/HL/SP/IX/IY/альт.
  набора между чтениями (счётчик в регистре = не опрос), (б) не перематывает в config mode
  (`MemESP::nextConfigMode`; счётчик в памяти не виден — хука на быстрой записи нет).
  Там же: **F11 в config mode = жёсткий сброс цепочки** (`NextReg::reset(true)` +
  `applyNextConfigWindow()` в `ESPectrum::reset`): мягкий оставлял config mode без bootrom,
  прошивка висла. Hw: TBBLUE с лентой и без, заставка с лентой 50, «2048», F11 в меню
  TBBLUE — «всё работает».
- **GetOut (Richard "Dicky" Dodds), титульный экран 30 FPS / IDL −1300 — тот же класс
  (2026-10-19, NOT hw-tested, `debug/DVp2-next-getout-dma6b-1.0.10`).** Дамп: PC=2125 в
  divMMC RAM bank 2 — WAV-плеер: zxnDMA память→порт #DF (burst + prescaler 26 ≈ 33.6 кГц,
  auto-restart, 2 КБ с #8000), CPU на 28 МГц крутит `IN A,(#6B) / BIT 2,A / JR Z` — маска
  чтения 0x10, т.е. старший байт адреса порта A: бит 2 = DMA во второй половине буфера,
  можно перезаписывать первую. ~18k `IN` за кадр, каждый через всю цепочку до
  `NextPorts::input` (core0 PC сидел в его veneer). Ранний путь для #6B/#0B (чтение) рядом
  с #1F в `Ports::inputImpl` — тот же `Z80DMA::readPort` + LED + late contention, что и в
  блоке ниже. +64 B `.data`. Срезы DMA-байтом уже ограничены (`dmaT` в `CPU::loop`), это
  не узкое место. Попутно `[SD] sync` ушёл под `PERF_TRACE || NEXT_TRACE` (−128 B `.bss`).
  **Ранний путь дал только 40 FPS / IDL −5500 (hw)** — сама петля на 28 МГц стоит: 26 витков
  на каждый DMA-байт. Второй шаг (`debug/DVp2-next-getout-dmapoll-1.0.10`, **hw-confirmed
  тем же днём — owner: «отлично», IDL +8-10k**; что IDE предлагало на лоадере — не проверялось):
  `NextPorts::dmaPollSkip` (`.nxovl`, из раннего пути при `nxPending`) — второе чтение DMA-порта
  с того же PC в пределах `128 << mult` T при burst + prescaler перематывает время целыми
  витками (арифметика NextIdle: период d, выборки dR, k витков, `idleAdvanceTo` + `incRegR`)
  до `min(nxNextT, stFrame, frame−IntEnd)`; не при NMI / RZX / в окне ULA INT. Окно `.nxovl`
  50 848 из 51 200.
- **«2048»: 38-45 FPS на 28 МГц -> 50, IDL сильно в плюсе (2026-10-19, hw-confirmed DVp2,
  `debug/DVp2-next-kbdpoll3-1.0.10`; регрессия .tap / турбо-автостарт / Browser — «работает»).**
  Игровое поле ждёт клавишу без HALT: `CALL` сканера (IN #1F + 13 `IN (C)` по рядам #FE),
  `JP` назад, ~800 проходов / 56k инструкций за кадр. NextIdle не берёт (CALL/RET, запись
  результата). Три правки, все нужны: (1) `NextPorts::kbdPollSkip` — форма `dmaPollSkip`
  (общий `pollSkip`): то же PC + тот же ряд + тот же #FE-латч дважды в пределах `256 << mult`
  T -> `idleAdvanceTo` целыми проходами до конца среза (матрица внутри кадра не меняется;
  латч в ключе оставляет бипер-циклам реальные проходы); (2) память — таблица 16 ячеек по PC:
  одна ячейка затиралась каждым из 13 чтений прохода и перемотка не срабатывала ни разу
  (так выглядел первый «не помогло»); (3) ранний путь #FE берётся и с ОСТАНОВЛЕННОЙ лентой в
  деке (`Tape::stoppedReadIdle`: не 0xFE00+, не LD-EDGE 05E3-05F5, не `IN A,(n)`-опрос фронта,
  нет PZX/JJ-продолжения — правила автостарта `TapePortRead`; проверка во flash,
  `nextKbdStoppedTapeIdle` — инлайн `readbyte` в `Ports::input` стоил +560 B `.data`). У
  владельца была смонтирована .tap, и каждый IN шёл через всю цепочку + `TapePortRead` во flash
  (prof-лог: `Ports::input`/`TapePortRead` в топе). Побочно: пропущенные чтения не сбрасывают
  счётчик автостарта ленты (200 подряд с одного PC). `.nxovl` 50 952 из 51 200.
- **SRAM-проход 2026-10-10 (NOT hw-tested): `.data` DVp2 102 260 -> 93 020 B.** Таблица
  видеорежимов `video_mode` — `static const` во флеше (была 4.4 KB .data): изменяемые поля
  в RAM-массивах `s_div` (pio_clk_div) / `s_vtot` (v_total) в graphics.c, ISR VGA читает
  RAM-копию активной записи `graphics_live_video_mode()` (пересборка только при смене
  индекса; писатели правят поле копии 32-бит записью). `_arm_addsubdf3` (libgcc double
  add/sub/f2d) — во флеш в rp2350-memmap.ld. `process_kbd_report` — флеш (зовётся только
  из главного цикла). Next из общего RAM-кода: хвост кадра `CPU::frameRest<bool NEXT>`
  (общий loop = `<false>`, `CPU::nextFrameLoop` = `<true>` в окне, `NX_OVL_CODE` =
  секция `.nxovl_code`, собирается правилом окна) и Z80N-опкоды `Z80::decodeZ80N()` в
  окне (в `decodeED` остался один case-список). `.nxovl` AUTO 31 744 (31 312 trace).
  Не сделано и почему: `flash_qe_fix` (656 B, должен жить в RAM при выключенном XIP),
  Next-ветки `Ports` (~600 B, горячий путь, разбросаны), Gigascreen (~3.5 KB, включается
  на лету — нужен захват окна посреди сеанса), SAA/MIDI/FM генераторы (3.6 KB, per-sample).
- **DS1307 на I2C** (2026-10-07, hw-confirmed: часы в NextZXOS): `machines/Next/
  NextRtc.cpp` (flash) — честный I2C-слейв на #103B/#113B (START/STOP по SDA при
  SCL=1, ACK на каждый байт, автоинкремент 0..63, ACK/NACK мастера), адрес 0x68,
  отвечает только при «CMOS + NVRAM». Время латчится из `RTC::now` на START, запись
  регистров 0-6 (`.TIME/.DATE`) → `RTC::setDateTime` на STOP. Подпись 'ZX' в
  #3E/#3F преднастроена; RAM 8-63 не сохраняется. Сверено: wiki RTC / I2C clock,
  zxnext.vhd 3236-3266, дистрибутив TBBLUE `src/asm/rtc/rtcack.asm` (= nextzxos/
  RTC.SYS — ТРЕБУЕТ ACK: 4 опроса SDA при SCL=1) и `rtcsig.asm` (10 байт с #3E).
  ZEsarUX ds1307.c слишком упрощён (без ACK и записи) — не брать. Хост-тест
  `tools/nextrtc_test.cpp` (+ заглушки `tools/nextrtc_stub/`), рецепт в шапке.
- **Floating bus Next** (hw: halt2int Float): только порт с младшим байтом #FF и
  только в тайминге 48K/128K на 3.5 МГц (zxnext.vhd 2813/4513), окно как у
  `getFloatBusData48/128`, на 60 Гц paper на 26 строк раньше; Timex-чтение (NR 08 b2
  + PE_FF) имеет приоритет. +3-float `#0FFD` не сделан.
- **NR 0x82-0x85 = разрешения декодирования портов** (2026-10-07,
  `debug/DVp2-next-portdec-1.0.9.elf`; hw-confirmed AY-часть: личность 48K без
  «AY in 48K» — AY молчит, с ней — играет, 128K — играет; 7FFD в 48K-личности
  отключён — 128K-игра видит 48K; Pentagon, режим Next (спрайты/Layer 2/SD) и
  переход Next → Pentagon — тоже ок, owner 2026-10-07): `NextReg::portEn` = zxnext.vhd
  internal_port_enable(27:0), биты `NextReg::PE_*`; каждый порт Next (7FFD/DFFD/1FFD/
  FF/DMA 6B+0B/Kempston 1F+37/divMMC E3/SPI E7+EB/мышь/спрайты/Layer2/AY/все DAC
  включая недостававшие 3F и Soundrive-2 F1/F3/F9/ULA+) проверяет свой бит;
  выключенный порт не декодируется. TBBLUE.FW `init_registers` гасит их по
  личности (48K: FF/7FFD/DFFD/1FFD/6B и AY без «AY в 48K»; 128K: FF/DFFD/1FFD;
  Pentagon: FF/1FFD/SD2; спрайты и Layer2 — во всех), NextZXOS пишет 0xFF при
  загрузке (ROM0 #00FB) и сам гасит 7FFD для своих 48K/128K-режимов (bank 2 #0DE0).
  Next-декодирование вынесено в `machines/Next/NextPorts.cpp` (flash); вне Next
  `portEn = ~0` (CPU::reset), поэтому общие гейты — один тест бита без isNext.
  Не сделано: +3 floating bus (бит 4), multiface, UART, CTC, EFF7; dac_hw_en (NR 08
  b3) не учитывается — прямая загрузка без BIOS осталась бы без Covox.
- **gm_bank** ужат 0x260000→0x25C000 (16K прошивке; банк перепровиженится).
- **Цена ветки: +14 KB статического RAM** (231.9K → 246.2K) — на PICO_DV это
  съело весь запас кучи у VIDEO::Init: P1024+NeoGS перестал грузиться (OOM-
  паника), лечится headroom-гейтом Helix-арены (NgsMp3: куча только при
  getLargestAllocatable ≥ арена+110K, иначе butter). Follow-up: найти и срезать
  эти 14K (кандидаты: Z80N-таблицы в RAM-резидентном ядре, NEXTVID-статики).
- **Тест-сьют**: `debug/next-tests/` (gitignored) — MrKWatkins/
  ZXSpectrumNextTests release (.snx запускаются Browser'ом самого NextZXOS,
  поддержка .snx в прошивке не нужна) + `!PICO-SPECCY-CHECKLIST.md`.

## Next: Adventures in Videoland at 576p — five levers (2026-10-16, hw-confirmed DVp2: owner "теперь хорошо")

Layer 2 320x256 + an 80-column text tilemap at 28 MHz, plus a video player that streams
off the SD card. Start: game 34-37 / 41-44 FPS, video 30 (576p, NeoGS on). End: game 50,
video ~50. Test ELFs `debug/DVp2-nxbulk-{plain,nxtrace}-1.0.10`. What did it, in order:

- **NextIdle verdict table + global give-up** (NextIdle.cpp): this game rotates through
  more loops of the right SHAPE than any small table holds, none of which ever repeats,
  so every switch re-ran `validate()` from flash — ~45% of core0 for zero skips. Now a
  32-entry direct-mapped VALID/REJECTED table checked in SRAM (`jumpGate`) before the
  flash `jump()`, a per-target give-up after 32 non-skipping activations/passes, and the
  whole detector off for the epoch (2^25 T) after 512 validations without one skip. Two
  weaker first cuts (a bigger reject list; per-validated-loop counter) were hw-refuted.
- **Layer 2 320x256 row LUT** (`nxL2LutSpan`, NextVideo.cpp): 256 colours per (palette,
  NR 0x70 offset, NR 0x14) in the band buffer's tail (`NX_L2_BAND` = band + 512 B), rebuilt
  on `s_palGen[pal]` (every rawPal writer goes through `nxPalDirty`), 4 px per unaligned
  32-bit band read. The band FILL (8-byte XIP line per column) is ~20% of core1 and at the
  PSRAM floor; XIP streaming is out (the cache is write-back, a stream reads stale PSRAM).
- **Adaptive render skip** (`NEXTVID::EndFrame`, now flash): core0 waited > 1 ms for the
  core1 queue (`g_nx_qwait_us` delta) -> the next frame is emulated, not drawn, never two
  in a row (`g_nx_autoskip`). Queue occupancy is the WRONG signal: a light frame posts all
  its rows at once and idles with the ring full. Trace: `fps=` vs `draw=` in `NEXT cpu`.
- **Hot-page pool on, allocated late and grown** (MemESP.cpp, `NEXT_HOT_FRAMES` 8): first
  try 150 frames into the session (after the renderer's own SRAM blocks — taking the heap
  first is why it was off since 10-13), then every 250 frames while an 8K block exists AND
  total free >= 8K + 16K (asking for 24K contiguous got 1 frame out of 46 KB). At 576p in
  this game it still gets 1 frame — the in-game heap is ~15-24 KB, not the 46 KB Hardware
  Info shows before the game switches Layer 2 320 on (menu log: `runInternal heap=15616`).
  **Default is 0 again since 2026-10-16** (owner): ~1 frame of gain in this game is not worth
  the SRAM; the code stays, `-DNEXT_HOT_FRAMES=8` turns it back on. **Default 1 since
  2026-10-19** — see the game-stage subsection below (the 16 KB heap reserve was what had
  kept it from ever taking a frame in the game).
- **Unrolled INI runs from the SD port + bulk sector copy** (Z80_JLS.cpp
  `nextIniRunBatch`, DivMMC `mmc_read_bulk`): the video reads with 32 x `ED A2` (C = #EB,
  entered mid-run through a self-modified JP at #EE14), 74k instructions a frame. After an
  INI the rest of the run but the last goes as one loop (13 T, R += 2 each, same bounds as
  `nextInirBatch`); the data bytes of a CMD17/18 block are memcpy'd from `mmc_sector_buf`
  (both batches), protocol bytes stay per byte. Video 35 -> ~50.
- **hw-REFUTED, reverted: stepping core1's async SD read** (sdcard.c `sd_async_pump` as a
  state machine, polling the token without blocking). core1 was ~35% in that spin, but the
  stepped read finished later, core0 (the bottleneck) waited for sectors and stepped them
  itself: FPS and IDL got worse. Leave the read blocking.
- **Instruments added**: `NEXT hostpc1` (core1 PC sampler, NxFrameProf::initCore1, 100 us
  alarm on core1's NVIC, 512-sample ring — NEXT_TRACE only). Overlay-window addresses must
  be resolved against the `.nxovl`/`.gsovl` section symbols, not addr2line (shared VMAs).
- **Window budget**: `.nxovl` 50 928 of 51 200 B (plain DVp2). The NEXT_TRACE build needs
  ~51.8 KB of window and is ~20 KB over the GM.DLS hard floor on 4 MB DVp2 — the test ELFs
  were linked with `NXOVL_WIN_SIZE=52224` and the floor temporarily at 1600 KB in the .ld
  (reverted every time). PERF_TRACE now takes NEXT_TRACE's window term (51 208 B measured).

## Next: a row takes the ULA page that was on show at ITS paper, not at its end — Insult (2026-10-20, hw-confirmed DVp2 the same day; owner: Insult "пофиксился", then the three follow-ups below each confirmed in turn)

Insult (Code Busters 1994, `insult.scl`): multicolour right on Pentagon + DivSD, wrong on
the Next in the Pentagon personality + DivSD — so not esxDOS. The engine, read out of a
Ctrl+Alt+D dump (`/tmp/picospec_dump.log`, 64K view + the 8 pages; the IM2 vector
3BFF/3C00 = FFFF lands on a `C9` in bank 7, IFF1 stays 0 — the INT pulse is irrelevant):
`988B..A1A6`, 63 blocks of exactly 448 T = two lines each: `LD SP,D8xx` + 16 PUSH = 32
attribute bytes into the OFF-screen page, `DJNZ` pad, `XOR #0A / OUT (#7FFD)` = screen
5<->7 AND bank 7<->5 (`0x17`<->`0x1D`). It never races the beam in RAM; it flips the
displayed page every two lines, with the flip at one fixed column. `NEXTVID::ScanlineWork`
runs for raster line L at T = (L+1)*tpl and `captureLine` took `VIDEO::grmem` THEN — a flip
in the right border gave the row the next pair's page. Pentagon's MainScreen reads the
page per column and never saw it. Two changes, both in NextVideo.cpp:
- **`NEXTVID::screenFlipMark()`** (from both `videoLatch` writers in NextReg.cpp — #7FFD
  bit 3 and NR 0x69 bit 6) = the `borderMark` ring: {T, page on show BEFORE the flip};
  `nxScreenFlipRow` resolves each posted row at its mid-paper T (`s_bcol + xres/4` into
  the line), dropping flips that are already past. Reset with the border ring in EndFrame.
- **`nxAttrSnap`**: with >= 2 flips in this or the previous frame, the row carries its 32
  attribute bytes in `st.attr`/`attOvr` (the DMA shadow's field) — the demo rewrites the
  page it just took off show every four lines and core1 reads RAM later. GATED on the
  flip count on purpose: an unconditional copy makes every row a new queue state
  (`nxSameState`) on every ULA game. Timex modes (NR 69 bits 2:0) and DMA-shadow rows skip it.
`nxRowFixups(st, row, fv, pt)` is the one flash call the three posting sites make (flip
page, #FE segments, DMA shadow, attribute copy) — the `.nxovl` window had 64 B left and
this SHRANK it by 96 B. Cost +44 B .bss. Checked against the VHDL on the way:
zxula_timing.vhd puts the Pentagon INT at vc 319 / hc 439 and the paper at vc 80 — our
17983 anchor — where the wiki's Reference_machines table ("2 lines before blanking", i.e.
81 lines) is loose; the VHDL stands. What still differs from a real Pentagon in that
personality and is NOT modelled: EAR idles 0 (Pentagon 1), no #EFF7, no Beta (esxDOS only).
Three follow-ups the same day, all hw-confirmed on DVp2 (owner), each found by a one-shot
log of what the renderer actually did rather than by reasoning about it:
- **The adaptive render skip killed the Next's Gigascreen and halved flip effects**
  (Across the Edge hip-hop part: "the gigascreen does not work in places and the picture
  looks shifted"; Insult's first effect "stutters, as if short of T-states" — the same
  mechanism). A `[NXGS]` log of every row's page verdict for two consecutive frames showed
  EVERY other frame posting no rows at all: `g_nx_autoskip` (core0 waited > 1 ms for the
  core1 queue → the next frame is emulated, not drawn, never two in a row). The demo
  inverts its page phase every frame, so drawing frames N and N+2 gives the blend the SAME
  page twice — pure colours in bands, the eye reads it as a shift. Not a wait problem
  (`waitState=0 waitRing=0`, core1 ~25-36 us a row with the blend): the gate is now off
  while `s_gigs` or ≥ 2 flips in this/the previous frame (`coherent`), the guest slows
  instead. The flips themselves were right all along (all in bucket 196-209 T = right
  border; the per-row verdict inverts per frame).
- **The Next Gigascreen blends the BORDER too** (`s_gigsPrevBrd`, one nibble per fb pixel,
  xres/2 bytes a row over the whole fb in PSRAM, best effort; `nxBorderGigs` in flash walks
  the row's #FE runs like `nxPaintBorderRuns` and blends through the same `gigsBlend` — the
  border colours are the first eight ULA colours). Across gigascreens its per-line border
  colours; on a Pentagon the shared Gigascreen's prev-FB covers the whole row, here only
  the 192x128 paper did. `.nxovl` AUTO term 51 200 → 51 456 (51 240 used).
- **`pollSkip` measured its period on the FRAME-relative `CPU::tstates`** (NextPorts.cpp).
  Insult's effect-1 ISR (`880C`) does everything per frame — erase, draw with #7FFD bank
  switches per pixel, music — and reads `#FE` once at `88EC` for O/P, at the same frame
  position with the same registers every frame: two reads a frame apart differed by a few
  T, read as a poll pass, and the "skip whole passes to the slice end" ran INSIDE the ISR:
  `[NXINT]` (INT accept → first EI) measured the ISR at 127k T = ~55k real + one frame,
  `ints=80-83 missed=17-20` per 100. The period is `global_tstates + tstates` now (a read a
  frame later = 71680 ≫ 256). "2048" (real poll loops) unaffected. Any program reading the
  keyboard once per ISR with unchanged registers had this.
Still owed: a game that flips #7FFD bit 3 once a frame (no attribute copy must engage);
Timex hi-colour + a flip; a 28 MHz title flipping screens (the ring's T are turbo-scaled).

## Next: the attribute row is captured at the BEAM, not at the row's post — Shock multicolour (2026-10-23, hw-confirmed DVp2w on `debug/DVp2w-next-mcattr3-1.1.0` — owner: Shock, Insult and other demos right in the 128K and Pentagon personalities; `mcattr2` had confirmed the attribute half, the scroller still twitched)

Shock (ESI 1992; `Shock (1992-06)(ESI).tap` / `.trd` in the 128K personality, `SHOCKPEN.SCL`
in the Pentagon one): the "multicolor" part's 8x1 attribute picture came out as stripes in
the wrong colours — paper only, border right, both personalities, and right on the plain
128K / Pentagon machines (owner's screenshot + report). A row is POSTED at the end of its
raster line and core1 reads its 32 attribute bytes from RAM later still; an 8x1 writer
rewrites the same 32 bytes every line, so every row showed a later line's colours. The
Pentagon's MainScreen reads per column and never had this; the Insult fix of 10-20 (page at
mid-paper, attribute copy gated on page flips) did not reach a writer that never flips.
- **The Next line clock has MORE THAN ONE boundary per line** (`s_capPhase`,
  `NEXTVID::nxLineArm`, flash): phase 0 = the paper start of line L (`s_pStartT` =
  `s_bcol` + (xres-256)/4 T into the line, set beside `s_bcol`; 48K/128K: ~4 T — on those
  rasters the paper is at the START of the INT-anchored line and the left border at its
  end — Pentagon: 67), where the flash `nxPaperStart()` copies the row's 32 attribute
  bytes (`s_capAttr`, the page in `s_capGrmem`); while a racing writer is on, phases
  1..15 follow 8 T apart and each re-copies the two cells the beam is about to read
  (`NX_CAP_GROUPS`); the last phase = the line end, the old `ScanlineWork` body.
  `contendWait` takes the line start from `lineIdx * tpl` now (it was `nextLineTstate -
  tpl`); `onClockChange`, `Reset`, `EndFrame`'s restart and `snapLineSync` go through
  `nxLineArm`. The DMA batch and `idleAdvanceTo`/`haltAdvanceTo` just stop at more
  boundaries. `.nxovl` 51 640 of 51 712 (AUTO term +128 for this).
- **The BITMAP line travels the same way while the writer is on** (`mcattr3`): the owner
  saw the multicolour right but the scroller TWITCHING on the Pentagon personality, smooth
  on a Pentagon. The sim says why: the scroller's 14 bitmap lines (paper 167-180) are
  rewritten at T≈63k, i.e. AFTER the beam passed them, for the next frame — a row core1
  read after that rewrite showed the next frame's position, and when it read it varied.
  So in racing mode phase 0 also copies the line's 32 bitmap bytes into a per-line
  PSRAM shadow (`s_bmpSh`, 192 x 32 B, `Buffer::palloc` on first use, one attempt per
  frame), the groups refine it 2 B at a time like the attributes, `st.bmpOvr` (a former
  `_pad6A` byte) tells the renderer to read `s_bmpSh + y*32` (both `bmp` sites, `sy == y`
  only — no NR 0x27 scroll). Single-buffered on purpose: row y is rewritten a frame
  later and core1 never lags a frame (the 64 state slots cap it at 63 rows). And the
  adaptive frame skip is off while `s_mcActive` (the `coherent` test, with Gigascreen and
  the flips): a dropped frame of a per-frame effect reads as a twitch.
- **`nxAttrBeam` (replaces `nxAttrSnap`) hands the row that copy** (`st.attr`, `attOvr`)
  while a writer is RACING the beam: the capture differs from RAM at the row's post
  (written inside this line's paper), or two paper-start captures of adjacent lines of the
  same cell row differ (`s_mcSeen`, `s_mcActive` = the rest of the frame + two more), or the
  screen flips inside the frame (Insult's case as before — the page on show at the paper
  start is the one the old post-time snapshot resolved at mid-paper). Still gated, because
  every copied row is a new queue state (64 in flight instead of 320); the 16 extra
  boundaries a line are paid only while it is on.
- **Why 16 points: Shock's writer was MEASURED, not guessed.** `shock_sim.c` (session
  scratch; the `z80dma_sim` shape: 48K ROM + the TAP part at 0x639C, `CALL 0x639C` =
  the depacker, then the master loader's table entry — FA00 F61C F744 FA00 E8A4 9F00 9C40
  F618 for parts 1-8; part 2 is the multicolour one, ~2950 attribute writes + 1160 border
  OUTs a frame) logs every attribute write with its T and PC. The writer is a PUSH racer
  going RIGHT TO LEFT (cells 31→16 on one line, 15→0 on the next) that also writes the
  NEXT cell row ahead of the beam, so within one line some cells land before the beam
  and some after. Against exact beam sampling (cell k of line y = the last write before
  paper + 224y + 4k) on the 48K raster: one point per line 2.6 % of cells wrong (147 a
  frame, scattered), 4 points 0.6 %, 16 points 0.01 %; on the Pentagon raster even one
  point is 0.0 % (the TAP's writes all fall behind the later paper there — the Pentagon
  SCL version has its own timing and was not simulated). So a screenshot of the
  Pentagon personality that is grossly wrong on this build is not this mechanism.
- Hw 2026-10-23 (DVp2w): `mcattr2` — 128K personality works, Pentagon multicolour right,
  scroller twitching; `mcattr3` — Shock (both personalities), Insult and the other demos
  the owner has all right (not itemised). Still not separately exercised: a plain ULA
  game's cost (`NEXT` trace: queue states must NOT climb), Timex hi-colour (excluded from
  the copy — other bytes), the 48K personality (contention on, `contendWait`'s new line
  start), a title scrolling the ULA with NR 0x27 (the copies are skipped there).

## Next: tilemap map rows are copied at POST time — International Karate+ menu (2026-10-20, hw-confirmed DVp2 on `debug/DVp2-next-mapsnap-1.0.10`, owner: "фиксед")

The menu (tilemap 40x32 + Layer 2 320x256, ULA off) showed only its title. The main loop
waits for NR 0x1F = 208 (`#81AE`), clears the whole tilemap with LDIR `#4000-#49FF`
(`#BD55`) and redraws the text (`#A042`, title first, "FIRE TO FIGHT" last = map rows
28-29). On hardware the beam has passed every row by line 208; core1 renders queued rows
later than their raster time and read them after the clear. `nxMapSnapPost` (flash, from
emitRow when the tilemap is on) copies the map row a posted line reads into the map-shadow
pool (key bit 14 = `NX_MAP_POST`, never matched by the per-key DMA scan), reused for the
following lines of the same tile row while unchanged (`until` bumped), and
`nx_mapOf[job % NEXT_C1_RING]` names it; `nxMapRowFor`/`nxMapRowForJob` take it first.
No free entry = live read, as before. +320 B of the queue heap block. Owed: TX-1696 /
Atic Atac / NEXTEST FPS against the previous build (more live shadows, one flash call per
posted tilemap line). Tile DEFINITIONS rewritten by the CPU behind the raster are still
read live.

## Next: the ULA INT pulse is 32/36 CPU CYCLES at any speed — 4K Race Next (2026-10-20, hw-confirmed DVp2 on `debug/DVp2-next-4krace-1.0.10`, owner: "фиксед")

`4krace-next.tap` (Ferraris/Costarelli, runs at 28 MHz) showed coloured stripes from a
varying paper line to the bottom of the screen. A `[4KR]` trace of its NR 0x12/0x13 flips
and copper control writes (a temporary diag, removed after the verdict — re-add
`Debug::log` in `copperControl` and the NR 0x12/0x13 case if it is ever needed again)
settled where: the game's swap + copper restart (`83B8 CALL 8A52`, then `NEXTREG 62,00`,
a rewrite through NR 0x60, `NEXTREG 62,C0` at `841C`) DRIFTED through the frame — lines
0, 101, 0 (1.5 frames apart), later 0, 89, 178 (1.33) — and the stripes began exactly at
the paper row of the restart. The game waits `LD A,(IX+0B) / SUB 2 / JR C` for two frame
interrupts (25 FPS) and its IM2 handler at `8B1C` is `INC (IX+0B) / EI / RETI`. A period
under 2 frames means the counter reached 2 within ONE frame: the handler was taken
TWICE per interrupt. Cause: `CPU::updateStatesInFrame` did `IntEnd <<= m` for every
machine (the classic fixed-wall-time pulse: 32 base T = 256 CPU cycles at 28 MHz), while
zxnext.vhd ~2033 counts `pulse_count` on `i_CLK_CPU` — **32 CPU cycles for the 48K/+3
timings, 36 for 128K/Pentagon, at ANY speed** — so a 60-cycle handler (19 ack + 23 + 4 +
14) re-takes the interrupt here and never on the board. `NextInt::pulseLen28` already had
the CPU-cycle rule for the line/CTC sources; the ULA frame window (`classicUla()` =
`CPU::IntStart..IntEnd`) did not. Now: no `<< m` on IntEnd for `A_NEXT`, and
`NextReg::raster().intEnd` = the VHDL rule (was INT_END48/128/PENTAGON per raster kind:
+3 36 → 32, Pentagon 32 → 36). Expected on the trace: every B/S/C=C0 line at paper ≈ -60
(right after the INT), never mid-frame; the game at a steady 25 FPS. The `[4KR]` diag is
to be removed once confirmed. Not re-checked: anything else that returns from its ISR
within 36..256 CPU cycles at turbo (NextZXOS itself does not).

## Next: the paging PORTS reset MMU0/1 to the ROM (2026-10-16, hw-confirmed DVp2: Aliens Neoplasma II starts)

zxnext.vhd 4636-4644 / ports.txt: a guest write to #7FFD/#DFFD/#1FFD/#EFF7 sets MMU0=MMU1=#FF (RAM 0/1
with #EFF7 bit 3). Only #EFF7 did it here. Aliens Neoplasma II leaves its data page #44 in MMU0
(`NEXTREG #50,#44` in its SFX routine) and its file loader writes #7FFD then RST 8 — the call ran
into the data page and the game hung at start. `NextReg::write7FFDPort/writeDFFDPort/write1FFDPort`
(guest ports only: Ports.cpp, NextPorts.cpp) = latch write + the reset; a locked write changes
nothing, and under the +3 all-RAM overlay only `mmu[]` is latched. NR 0x8E and the .nex/launch
paths keep the plain writers (zx_go keeps 8E out because of the NextZXOS trampolines; not checked
against the VHDL). Test ELF `debug/DVp2-next-anp2-1.0.10`. Owner checked the regression set the same
day (NextZXOS Browser, .nex/.snx launch, 48K/128K personalities): no problems.

## Next: STNICCC WireFrame garbage is the demo's own (2026-10-16, hw-tested DVp2: gone with bank 7 zeroed)

Dotted "data" in the lower half of every other frame of `STNICCC_WireFrame8Bit.nex`, launched
from the NextZXOS Browser. NOT ours (decided to leave as the hardware would): on frames without
the "clear" flag (#04, most of them) the demo "clears" the back buffer at #86DF by DMA-copying
16K from #4000 with `NEXTREG #52/#53 = (#8849)+C` — 16K-bank numbers (9/12, the defaults
.nexload sets) used as 8K pages, step 1 instead of 2 — i.e. it copies pages #09-#0C or #0C-#0F
and relies on zeros there. The file carries banks 0-6 only, .nexload never touches bank 7, and
the Browser leaves its command line (`cls:.nexload STNICC~4.NEX`, from browser.cfg) plus code in
bank 7 — that is the garbage (L2 pages 3/4 = page #0E, 5 = #0F, byte for byte). A test build
zeroing bank 7 at .nexload's `NR 0x13 = 12` removed it. tools/memdump.gdb now dumps both Layer 2
buffers (`/tmp/picospec_l2a_*/l2b_*.bin`, hot pool honoured), zxnDMA state and the L2 window
masks — read those before blaming the DMA or core1 for Layer 2 garbage.

## Next: F5 launch of a .bas must clear what the Browser clears (2026-10-16, hw-confirmed DVp2)

Crater Crawler (NextBASIC, `TILE` backgrounds) kept its title screen under the game when
started by F5, never from the NextZXOS Browser. The Browser's BAS line (`browser.cfg`) is
`clear 65367:sprite clear:palette clear:layer clear:restore:load "|"`; our native launch
(`NexLaunch::start`) typed only `LOAD "c:/…":RUN`, so the program inherited the menu's
layer/palette state and `TILE` (AltROM page 1 #10FB: 16 x `LDIX`, A = NR 0x14 read at
#0A99) skipped every black pixel. The host-tokenised line now carries the same prefix
(`SPRITE CLEAR:PALETTE CLEAR:LAYER CLEAR:RESTORE:`); `CLEAR 65367` stays the direct
RAMTOP write. Which of the three state resets the game actually needed was not isolated.
Diagnosed by rendering the game's map + tiles on the host against the L2 dump: the star
pixels of tile 3 were drawn over the logo, its zero pixels were not.

## Next: Atic Atac Remake — a copper raster NMI plays the sound (2026-10-16; 24 -> 42-43 FPS hw DVp2, screen garbage FIXED, FPS still OPEN)

`ATICATAC.NEX` (9bitcolor, 111 MB; the header loads 3 banks, the rest is streamed). The
game opens its own file, takes `DISK_FILEMAP` (it refuses a fragmented file) and reads raw
sectors through port `#EB` — CMD12 + CMD18 per chunk, unrolled `INI`. Its sound is a
**copper raster NMI**: the copper (mode 01, `NR #62 = #40`) writes `NR #02 = 4` on every
line, enNxtmmc's `#0066` is `JP (IY)`, and the handler in divMMC RAM banks 12-15 outputs 4
DAC samples (`#0F/#4F/#DF/#FE`), steps IY and `RETN`s. At the end of a bank it moves `#E3`
to the next one, and the main loop streams the next chunk only when `#E3` bit 1 changes.
Three fixes, all ours:

- **divMMC NMI is taken at once** (`NextInt::wake()` in `NextReg::divNmiRequest`, as
  `NextMf::request` always did). Left pending until the slice ended, ~300 requests a frame
  collapsed into ~2: the intro "loaded" for minutes with the stream at a crawl.
- **`Z80::nextIniRunBatch`**: every refusal that does not depend on the run is tested BEFORE
  the scan for `ED A2`, and the scan stops at the limit. It runs after every INI from `#EB`,
  and the loader reads to `HL = 0` (MMU slot 0 = RAM): rescanning a 512-INI block only to
  refuse it was quadratic, 76% of core0. The destination is now resolved like Z80DMA's
  `nxWrRun` (divMMC window / Next ROM -> byte path, Layer 2 write-over -> its buffer — the
  batch used to write the MMU page under it — any RAM slot batched).
- **Copper runs** (`copSkipRun`, flash): a run of NOPs or of identical `MOVE NR #7F,v` (a
  plain latch) costs one step; the run is found once and cached per kind until a copper
  memory write. This game pads its list with `MOVE 7F,0`: 2.2M copper writes a second, and
  `copperRun` + `NextReg::write` were ~75% of core0. `.nxovl` 51 072 of 51 200 B.

The garbage — fixed the same evening (hw-confirmed DVp2, owner: "работает"); three causes in turn:
- **The DMA was held for good after the first NMI** (`NextInt::dmaDelay`): the NMI term is
  `next_nmi_btn || (next_nmi_hold && automap)` — zxnext.vhd 2007/2093, `nmi_activated` drops at
  S_NMI_END. `next_nmi_hold` alone stays set until the next request, so no DMA ever ran again
  (sprite patterns never uploaded).
- **One main instruction ran after every RETN while a DMA block was still held**
  (`NextInt::wakeNow()`, used after RETN/RETI): `wake()` leaves `stFrame = tstates + 1`. With a
  copper NMI on every line the game's page-fill loop (`#10BA`: `NEXTREG #54,A` + an 8K DMA fill)
  advanced under its own held fill, the last 8K landed on code page `#0C` and the machine slid into
  zeros. `[DMAHELD]` (NEXT_TRACE, Z80DMA::nxHeldNote) logs DMA/MMU writes while a block is held —
  it must print nothing.
- **SD Nac = 0: the intro asset loaded one sector late** (DivMMC.cpp `mmc_nac`). The game's loader
  (`#D1ED`) reads ONE byte past R1 blindly; when the sector was already in hand (read-ahead hit,
  core1 fast) our CMD17/18 put the 0xFE token right after R1, the loader swallowed it, waited for
  0xFE in the data and caught the NEXT block's token — the first block of the asset was lost, the
  map shifted 512 bytes (tile definitions in the bottom rows, striped tiles). A real card always
  sends at least one 0xFF there (SD NAC >= 1 byte); now we do too. Found by deriving the asset base
  from the game's own table at `#DAC2` (asset 1 = file sector 0): matching a dump against the file
  WITHOUT a base made the shifted data look "correct" for a whole round.
- A DeepSeek MMU-freeze per DMA block (frozen slot pointers at ENABLE) was tried and dropped: it
  treated the symptom and departs from hardware (a handler may remap a slot while the DMA is held).
  Kept in `debug/deepseek-staged-2026-10-16.patch`.
- Still open, separate task: **42-43 FPS** in game. `tools/memdump.gdb` now also dumps the copper
  program (`/tmp/picospec_copper.bin`) and the live clip windows.

Trace-build changes from the same session: guest-raised NMIs no longer re-arm the 1500-line
`[NMIT]` UART trace (only the hot key does — the trace build ran at 0-1 FPS), copper writes
are counted not recorded, the 1 Hz block drains the UART in parts (the `NEXT hostpc` lines
were being lost), and `tools/memdump.gdb` dumps all 16 divMMC banks and 10 pages of each
Layer 2 buffer (320x256 / 640x256 take 80K).

### Intro at a stable 50 FPS, no jitter, IDL ~+3000 (2026-10-19, hw-confirmed DVp2 on `debug/DVp2-next-aticatac-plain15-1.0.10`; the game is the next stage)

Start of the day: 42-44 FPS with dips, and "подергивания" that looked like a core hand-off
bug. A five-build bisect (every change of the day off, one at a time, then all) showed
none of them caused it: **the jitter was TEARING** — single-buffered fb, both cores at
render capacity (vid ~25 ms/frame split over two cores at 576p), so rows were drawn
beside the beam. With core0's help off (`c0Help` returning false) IDL rose to 10000 but
core1 alone dropped frames and the text flickered. Every lever is render cost:

- **JR C / JR NC taken backward now call the idle hook** (decodeOpcode30/38 + the
  generator): the intro pause is `LD HL,(nn) / XOR A / SBC HL,BC / JR C` — the old
  JR NZ/Z-only hook never saw it (`idle loop ... skipped 46%` afterwards). The earlier
  "the copper NMI blocks the skip" reading was wrong (cop/s=0 in that scene).
- **Layer 2 band sharing between cores** (`nxL2BandLoad`): core0 takes the OLDEST row,
  so both cores needed the same 8-row band and each filled it (320 PSRAM line fills).
  A band the other core filled for the same block within ±7 queue positions is copied
  (seqlock `gen`, `pendKey/pendRow/pendJob`, 2 ms wait cap). `NEXT l2band: fill N
  borrowed M`.
- **`nxMixL`**: Layer 2 on, no opaque sprite pixel — the six orders collapse to two
  loops (UOVER), read through `kNxMixNoSpr`; the full `nxMixT` runs over the sprite
  span only. **`c1Drain` in `applyTimexMode` only on a real ink/mode change** (it ran
  on every NR 0x69 write).
- **Tilemap 4bpp whole tile**: a 16-entry colour table per tile palette (transparent
  = NX_TR and the NX_BELOW mark folded in, rebuilt only when `palHi | below` changes),
  two pixels per word store — 12 -> 5.5 instructions a pixel, no branch
  (`tm=` 5.2 ms/fr before). **Layer 2 LUT span**: the "any opaque" accumulator is
  two ANDs on the packed words instead of 8 XOR/OR ops per 4 pixels.
- **Adaptive render skip** stays (`fps=` vs `draw=` in `NEXT cpu`).
- **SD DMA: tried and REVERTED** (owner: no visible gain). The stepped core1 burst +
  PIO-SPI DMA data phase cost two bugs (a DMA TX source in flash starving the HDMI
  feeders; a DMA write into the cached PSRAM alias = stale reads, blank assets) and the
  card is read 2-4 times a second for 1-3 ms in this game. core1 reads the burst in
  one blocking call, as committed on 2026-10-16.
- Scene numbers (trace13, 576p, the 11A0 raster-wait scene): `vid: ula=0.5 tm=5.2
  l2=9.0 spr=3.2 mix=6.5 out=0.6 ms/fr`, `rows: core0 90/fr 7 ms, core1 197/fr`,
  cpu 12 ms/fr. Config: NR 6B=82 (40-col 4bpp, attrs, 512 tiles), 70=17 (320x256),
  15=0B (sprites, order S U L), 68=90 (ULA off), 14=E3. Next levers for the game:
  the full mixer path, the Layer 2 band fill (PSRAM floor), ~47 sprite tests a row.

### The game: ~50 FPS, rooms included (2026-10-19 evening, hw-confirmed DVp2 on `debug/DVp2-next-aticatac-plain26-1.0.10`)

Start: 42-47 FPS with IDL swinging -8000..+5000 on one screen (the autoskip alternating
drawn and skipped frames), rooms 43. Read in this order from the trace build, each one
a round: `NEXT perf: drain=`, `NEXT rows: core0`, `NEXT vidpre: sprlog=`, `NEXT z80:
ns/m1`. Six changes, every one measured:

- **Sprite log 1024 -> 32768 entries** (128 KB PSRAM, `SPR_LOG`): the game streams its
  sprite PATTERNS by DMA into port 0x5B, ~5-6k log entries a frame (`vidpre: sprlog=`), so
  1024 overflowed 19 times a second and 4096 still every frame — each a full `c1Drain`
  of up to 7 ms (3.5 ms of core0 a frame).
- **Tilemap map-row shadows 32 -> 192**, headers 8 B each in the hot SRAM block (core1
  scanned 32 of them 172 B apart in PSRAM per tilemap line), scan bounded by a
  high-water mark; and **the cover test honours 512-tile mode**: `nxLineReads` calls the
  definitions "the whole bank" there, so every queued line read a map-row DMA as a
  definitions write and the copy was taken only with an EMPTY queue (340 waits/s, 2.5 ms
  a frame; the 1024-log drains had been emptying the queue and hiding it). Per-STATE
  verdict cache in `nxMapShadowCover` (the full test ran on all ~300 queued rows per
  DMA block: 27 us per `OUT (6B)`, 8 ms a frame -> 2.2).
- **core0's OWN sprite render state** (`s_sprR0`, in the helper's SRAM block): the one
  shared copy meant core0 could take a row beside core1 only when it needed nothing
  applied, and this game writes attributes all frame long — core0 rendered 0-5 rows a
  frame. Taken from core1's copy at core1's log position (`applying` flag + position
  re-check), then replayed to each row's own position; `res[]` zeroed at the sync or
  `resolveSprites` (which rebuilds the visible list only on a VIS flag CHANGE) left it
  empty — striped sprites, two hw rounds. **Pattern bytes are ONE memory**: a core may
  take a row only if no pattern entry lies between the other core's position and the
  row's (`nxSprPatClear`, `s_sprPatW`), else it waits as before. (This was credited with
  fixing the intro's jumping ticker — WRONG, the ticker is not sprites and kept jumping;
  see the next section.)
- **The row helper's block**: retry every 50 frames (was 250), and the queue waits
  (`nxWaitUntil`, `c1Drain`) do not call the flash `c0Help` while there is no helper —
  core0 spinning through flash cost core1's PSRAM stream (XIP 5M accesses a frame).
- **Hot-page pool ON by default, 1 frame** (`NEXT_HOT_FRAMES` 1, `NEXT_HOT_RESERVE` 4 KB
  in MemESP.cpp): the 16 KB reserve never let it take a frame in the game (~20 KB free),
  which is why the 10-16 verdict read "~1 frame of gain". With page 06 (80% of PC
  samples) in SRAM: ns per M1 380-430 -> 285-300, core0 22-24 -> 15.7 ms a frame, and the
  room transitions (core1 reading the SD ~250 ms/s) no longer dip. **The UI gets the
  frame back**: `MemESP::nextHotRelease()` from `nm::gfxBegin` beside the queue drain
  (F5 PANICKED on the 11 KB left), re-taken ~2 s later by `nextHotTick`. Churn in play:
  1-4 promotions a second.
- What the game is now: render ~19 ms/frame on core1 + 40-46 rows on core0, Z80
  ~13 ms, DMA 2.4 ms, NR 1F polls 2.2 ms. Left: the full mixer path (~22 us/row), the
  Layer 2 band fill (PSRAM floor), SD reads blocking core1 during room loads.

### The intro ticker jumped because the guest waited for a WHOLE SD burst (2026-10-20, hw-confirmed DVp2: Atic Atac + Dune on BaseConf)

"9bitcolor presents..." showed, about once in 5-10 s, one frame with the text ~200 px
left. Frame-by-frame video: that frame is the ticker at Layer 2 scroll 0 — the text lies
at x 30..160 of the 320x256 Layer 2 buffer (castle above it, same buffer) and is put on
screen by NR 0x16/0x71. The intro's main loop (`#19E2..#1A08`, no HALT, no INT sync) ends
in `#1B46` -> `#1C00`: `OUT #243B,#1E / IN / JR NZ`, `OUT #243B,#1F / IN / CP (#20EC=184)
/ JR NZ`, then `NEXTREG #16/#71` = scroll; the line interrupt at 224 (NR 22=06, 23=E0)
puts scroll back to 0 for the next frame's top. A loop pass that reaches the wait after
line 184 leaves the ticker at scroll 0 for that frame. Music is streamed from the card
(CMD18 via `#EB`, 17-35 sectors a chunk) and a CMD18 asked core1 for a window of up to 32
sectors and gave the guest its FIRST data token only when the whole window was in:
185-199k T of token polling per read (~7 ms at 28 MHz, 1/3 of a 567k-T frame). Fix
(`sdcard.c` `g_sd_as_done` + DivMMC `asIdx`/`asInstall`): core1 publishes each block as
it arrives, the guest's sector is handed over as soon as ITS block is in, a sector the
running burst covers waits for its block only, and a finished burst nobody took becomes
the read-ahead window instead of being thrown away. Measured: per-read wait 18-41k T,
per chunk 70-97k T (was 210-270k), no missed frame. Off-Next ZC machines take the same
path (Dune on BaseConf checked). How it was found, worth repeating: the copper/NMI and
the NR 1F fast-forward were checked against the VHDL first and were right; what named it
was a per-frame line counter (where the wait began, where the scroll was written) plus
guest T spent in the token poll — a frame without its scroll write printed beside the SD
figures.

## Next: Layer 2 640x256x4 goes through the 640 pair frame — SymbOS ZNX 4.0 (2026-10-20, hw-confirmed DVp2 on `debug/DVp2-next-l2hi640-1.0.10`: owner "текст читаемый, картинка хорошая")

SymbOS draws its desktop in Layer 2 640x256x4 (dump: NR 70=20, 69=80, 6B=00, 15=03,
12=00, 14=E2), and the pair frame engaged only for the 80-column tilemap
(`pairPoll`'s `want` = NR 6B C0), so each byte was shown as its left pixel at 320
width: unreadable icon labels and taskbar. Now `want` also takes Layer 2 on with NR
0x70 bit 5 (modes 2/3); `nxL2SlowRow(..., o2)` puts the high nibble into nxL and the
low one into `nxL2nd` (a 4th per-core 768 B line: +768 B in the P80 block and in
core0's helper block), each with its own transparency/priority, and RenderLayers
merges the L2 [dx0, dx1) difference with the tilemap's and mixes the right pixels
with `nxL = nxL2nd` (and `nxT = nxT2` only when the tilemap is also 80-column).
The Layer 2 640 path is still the per-pixel slow row (flash) — FPS was not measured.
`.nxovl` 51 432 of 51 456. Owed: an L2 640 title with sprites/tilemap over it, X scroll.

## Next 640 pair frame: a two-colour screen lost half its glyph pixels (2026-10-19, hw-confirmed DVp2: ZXZVM readable, NEXTEST unchanged)

ZXZVM v1.17 (Garry Lancaster's Next module, 80-column 4bpp tilemap with attributes,
NR 6B=C3) came up with every other half of each letter missing at 576p, while the
log said `[NXV] 640-wide pair frame on` / `2 colours on screen, 16 picked`. The fb
dump had pair slots (8,0) and (8,8) and NOT ONE (0,8): `p80Choose`'s first pick laid
the 16 colours out as families f/f+8 with the most shown colour at f and its nearest
colour at f+8 — on a two-colour screen the text colour landed at pair index 8, and the
DS80 pair table folds `(ink 0..5, paper 8) -> (ink, 0)` (`init_profi_pair_lookup`'s
slot budget; harmless on a Profi, where 8 is bright black), so every "paper left, ink
right" pair went black. NEXTEST never showed it: with 16 colours the partner of black
is another dark colour. Now the 8 most shown colours are the family HEADS in 0..7
(every combination among them exact — up to 8 colours never fold), and only the picks
past the 8th become partners of the nearest head at f+8. Reading a Next 640 fb dump:
`profi_lut.bin` + the (ink,paper) reverse of each slot, counted per slot, names a fold
in one line — a missing mirror pair is the signature. Test ELF
`debug/DVp2-next-p80fam-1.0.10`.
