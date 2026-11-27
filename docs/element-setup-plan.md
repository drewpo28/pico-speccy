# eLeMeNt ZX: SetUp (прошивка), Flash ROM Areas, личности машины — анализ и план (2026-11-02)

Статус 2026-11-02 (вечер): **фазы A и B сделаны, стенд + hw-confirmed DVp2w** (UART-лог владельца
`logs/devttyACM0_2026_11_02.23.03.30.668.txt`: образ собран и записан, FLASH6 обновил сектора, SetUp
менял Machine, четыре F12 без ошибок и без отказа PSRAM). Тестовая прошивка `debug/DVp2w-elm-setup-1.1.1`.
Фазы C-E не начаты. Итоги в §8.

Продолжение
`docs/element-zx-plan.md` (фазы 0-6 там); здесь — то, чего в тех фазах не было: настоящая
прошивка SetUp из ROM-области, 512 КБ FlashRAM с четырьмя областями, FRAM с настройками,
переключение личности `e_zxi_000` при сбросе, Didaktik Gama 80.

## 1. Источники (все прочитаны 2026-11-02)

| Что | Где | Что даёт |
|---|---|---|
| ELZXMBUP.TXT | element.zxfiles.net/DOCS/ELZXMB/, копия `debug/element/` | Порядок обновления: SetUp (FlashSetUp) → «Load defaults + restart» (CS+L) → ElCoreUp |
| FLASHINF.TXT | там же, копия `debug/element/` | Раскладка области: 8 блоков по 16 КБ, какой флешер что пишет |
| ELUSEMAN.PDF | там же; текст `debug/element/ELUSEMAN.txt` | Меню SetUp (все пункты), вход = reset > 1.5 с, Rescue = NMI+Reset+DIP, стр. 15/18/23 |
| Programmer's Reference | Google Doc `1NJB_z3zVorTfmEj2JAS3QtAx_zbpkdCAAptHSnOOzkU` (`/export?format=txt`), копия `debug/element/ElementZX-ProgRef-20261101.txt` (отличия от сегодняшней выгрузки — три уточнения по HGFX/FDMA, не по flash) | `e_zxi_00F`, типы раскладки URAM 6 (FLASH) и E (FRAM), карта FRAM, Gama 80, +2A `#1FFD`, Pentagon 4096 |
| История ядра | Google Doc `1wJZE9nAooqIM4KUVAjAsJzBz177NhNZfGVUUXSLqC9s`, копия `debug/element/ElementZX-CoreHistory-20261012.txt` | Последний SetUp = 20260406, ядро 20261012 |
| INFO.HTM | element.zxfiles.net/DOCS/INFO.HTM | Индекс документов; про Didaktik там ничего нет |
| **UPDATE20261012.ZIP** (владелец) | `~/Downloads/zx/eLeMeNtZx/` | `FlashSetUp_V2.tap` = BASIC + `flash6` (2424 B) + `setup` (15695 B, **образ SetUp ROM**); `setup.tap` = тот же образ как программа; `ElCoreUp.tap`; битстримы ядер |
| FLASHROM.TAR, FL-SETUP.TAR, ELSETUP.TAR | zxfiles.net/download/elzxmb/, распакованы в `debug/element/` | Все флешеры ROM с образами: 48.ROM, 128-0/1.ROM, 128-2A-0..3.ROM, DG.ROM, ESX-SD.ROM (8 КБ, v0.8.9-MB03+SD), BS140ESX, SE-ROM0..3, DIAG.ROM, ZXDIR4.ROM |
| speccy4ever.speccy.org/_DI.htm | | ROM Didaktik Gama 87/88/89, M 91/92/93, Kompakt, D80 1.0/2.0; документация Gama 192K |

Дизассемблирование (`tools/z80disasm.py`): `debug/element/setup/setup-20260406.asm` (org `#C000`),
`debug/element/setup/flash6.asm` (org 25000 = `#61A8`).

**Didaktik с speccy4ever не нужен.** `FLASHDG/DG.ROM` из флешера eLeMeNt побайтно равен нашему
`src/speccy/roms/48k/src/dgama89.bin` (md5 `28287c39…`). Для личности «Didaktik Gama 80» нужен не
ROM, а 80 КБ ОЗУ с пейджингом через i8255 (§3.6). ROM Didaktik M / Kompakt / D80 к eLeMeNt
отношения не имеют — это задел на отдельную машину, если когда-нибудь захочется.

## 2. Что выяснилось

### 2.1 FlashRAM 512 КБ, четыре области

- `e_zxi_00F` = область 0-3 (плата v2; на v1 — DIP 7/8). Область = 128 КБ = 8 блоков по 16 КБ:
  0-3 ROM0-ROM3 (BASIC), 4 esxDOS, 5 резерв, 6 Rescue, 7 SetUp.
- Рекомендованные наборы: область 0 = ZX128/+2, 1 = +2A, 2 = ZX48, 3 = свободная (SE BASIC,
  DiagROM, BSROM, ZXDIR). Личность (`e_zxi_000`) и область независимы: область выбирает
  пользователь, машина берёт из неё ROM0-3 по своим правилам пейджинга.
- Какой блок видит Z80 (вывод из того, что пишут флешеры): `Flash48` кладёт 48.ROM во все четыре
  блока; `Flash128` кладёт 128-0 в блоки 0 и 2, 128-1 в блоки 1 и 3; `Flash2A` кладёт
  128-2A-0..3 в блоки 0..3. То есть блок = `#1FFD` бит 2 (<<1) | `#7FFD` бит 4; для 128/Pentagon
  `#1FFD` не пишется, и дублирование в 2/3 делает любой выбор правильным; для 48 — любой блок.
- Образы ROM: 128-0/128-1 = наши `sinclair_128k_0/1` (md5 совпали), 48.ROM = `sinclair_48k`.
  128-2A-0..3 — это ROM +2A (у нас только +3, md5 другие; +3 отличается в ROM 2). ESX-SD.ROM —
  8 КБ esxDOS 0.8.9 «MB03+SD» сборки (у нас своя сборка в `roms/esxdos/esxdos.c`).
- Rescue ROM (блок 6) и «FPGA (rescue esxDOS)» ни в одном пакете нет — они только во flash
  реальной платы. Для нас роль «rescue esxDOS» играет встроенный образ, а блок 6 — `#FF`.

### 2.2 Как FLASH6 пишет во flash (flash6.asm)

```
e_zxi_000 = 0 (48K), e_zxi_030 = 1 (7 МГц)          ; 61AE
область: e_zxi_00F = цифра                          ; 621A
сохраняет g_zxi_000, #703B/#713B (MB03+), e_zxi_001, затем e_zxi_001 = #FF
g_zxi_010 = 1 (Ultimate Full), OUT (#53),#63        ; тип 6 = FLASH Area0+Area1, запись в обе
окно zone 0: #07/#47 = страница 8К, #27/#67 = 0     ; страницы считаются ОТ НАЧАЛА ОБЛАСТИ: блок*2, блок*2+1
стирание: 4 × 4 КБ сектора: AA→#5555, 55→#2AAA, 80→#5555, AA→#5555, 55→#2AAA, 30→сектор; опрос DQ7
запись:   AA→#5555, 55→#2AAA, A0→#5555, байт→адрес; ждёт, пока чтение не совпадёт
проверка: сравнивает #8000-#BFFF с окном, выводит число ошибок
откат: OUT (#53),0, g_zxi_010 = 0, восстановление сохранённых регистров
```

Адреса команд `#5555/#2AAA` достигаются через страницы 2/3 (`#1555` в окне → `2*#2000+#1555`) и
0/1 (`#2AAA`). Это протокол SST39SF040 (512 КБ, сектор 4 КБ) — ровно то, что нужно смоделировать:
три шага разблокировки, `#30` = стереть 4 КБ в `#FF`, `#A0` = записать байт (только сброс битов),
мгновенное завершение (DQ7 = бит 7 данных сразу верен). Страница в типе 6 = `area*16 + (p & 15)`.

Все флешеры ROM (не SetUp) перед работой делают `OUT 30779,1: OUT 31035,254` — `e_zxi_001 = #FE`
«Rescue mode», и после этого ещё грузят файлы через esxDOS (`LOAD *`). Значит в `#FE` esxDOS
остаётся живым; у нас `#FE` = ничего не менять (DivSD работает).

### 2.3 SetUp ROM (setup-20260406.asm)

Образ 15695 B — это блок 7 области. Начало: `DI; LD SP,0; IM 1; … JR` → id `/LMN128`, на `#0038`
`EI; RET`, на `#003A` копирует себя с `#0048` в `#C048` и прыгает туда. Дальше ROM на `#0000` не
читается до самого выхода. Ядро подставляет блок 7 на `#0000` при удержании reset > 1.5 с.

Что он делает (адреса в asm):

- `#7FFD = #10`, `e_zxi_030 = 1` (7 МГц), чистит экран. `IN A,(#11)` → сохраняет, `OUT (#17),0`
  (страница MB02+; нам: чтение `#FF`, запись в никуда).
- `e_zxi_001 = #FF` (старый «Ultimate»), сохраняет `g_zxi_000`, ставит 1, `OUT (#53),#E0` —
  **тип E: FRAM в Area1 zone 0** (`#2000-#3FFF`, 8 КБ) только чтение; для записи `#E2`.
- **FRAM `#2000+N` = `e_zxi_N`**: при выходе читает `#2030` и `#2001`; «Load defaults» копирует
  таблицу `D3ED` (64 байта: `01 01 1F 01 04 00 … 03 00 01 00 01 …`) в `#2000`. Счётчики
  статистики: `#3FFC` OnTime, `#3FF8` Nmi, `#3FF4` Reset, `#3FF0` Extra, `#3FEC` PowerUp (по 4
  байта, их ведёт ядро, SetUp только показывает и считает «User level»).
- Каждый пункт меню **сразу пишет свой регистр** (`C64A` e_000, `C6BE` e_001, `C765` e_002,
  `C7E4` e_003, … `D1DE` e_00F, `D27A` e_03F, `D37E` e_03E, `D3DD` e_02B — полный список регистров:
  00 01 02 03 04 06 0F 10 11 18 19 1A 1D 1E 1F 20 21 2B 2C 2D 2E 2F 30 31 32 38 39 3E 3F) и в FRAM.
  При старте платы ядро само загружает FRAM `#2000..` в регистры («loaded on startup»).
- Читает `e_zxi_0FE` (дата ядра), `e_zxi_0EF` (тип платы FPGA, строка «FPGA board: … unknown»),
  `e_zxi_039/01A/02B`; RTC72421 на `#0003` — цикл «ждём смены секунд» с `HALT`; UART `#133B`
  (`AT+CWAUTOCONN=1`, все циклы с таймаутом DE); клавиатура через `#FE`.
- Выход (`C508`-`C544`): `#53 = 0`, страницы 0/1, `g_zxi_000 = 1`, `e_zxi_001`, `e_zxi_030` из FRAM,
  `OUT (#17),0`, **`RST 0`**. Аппаратного сброса нет: по `RST 0` на `#0000` уже должен быть
  обычный ROM. Standalone `setup.tap` (тот же код с `#C000`) заканчивается так же. Клавиша `r`
  — перезапуск меню, `D` — диагностика, CS+L — defaults.

Следствие для нас (2026-11-02): образ SetUp не вшивать — его ставит `FlashSetUp_V2.tap`.
**Пересмотрено 2026-11-04 (владелец: «можно сделать автоматическую установку SetUp, сохранив их во
флеше?»):** SetUp 20260406 (15695 Б, payload FlashSetUp_V2.tap) и `ESX-SD.ROM` (esxDOS 0.8.9
MB03+SD, 8 КБ, пакет FLASHESX; отличается от нашего `gb_rom_esxdos` в 810 байтах) вшиты в прошивку
(`src/speccy/roms/element/elm_setup.c`, `elm_esxdos.c`, `elm_roms.h`, только 16 МБ образы —
`ELM_ROMS_BUILTIN`). `ElmFlash::installBuiltin`: свежий образ строится с ними (блок 7 и блок 4
областей 0–2), существующий `flash.bin` получает их в пустые блоки при первом монтировании (лог
«SetUp installed into block 7 of Area N» / «esxDOS MB03+SD installed into block 4 of Area N»,
секторы помечаются грязными и пишутся на карту). Флешеры нужны только для обновлений. hw-confirmed DVp2w 2026-11-04. Стенд: оба
случая, байты блоков равны исходникам, SetUp стартует с Alt+F11 без ленты. Это распространение
прошивки LMN128 внутри нашего образа — решение владельца.
**Исправлено 2026-11-27 (владелец: «автоматическая установка SetUp не работает»; стенд, NOT hw-tested,
`debug/DVp2w-elm-setupall-1.1.4`):** установка шла только в области 0–2, а вход в SetUp грузит блок 7
ТЕКУЩЕЙ области (`e_00F`); с областью 3 (её выбирает сам SetUp) — «SetUp not flashed», и вернуться в
SetUp было нечем. Теперь SetUp ставится в блок 7 всех четырёх областей, и не только в пустой блок, а
в любой без id `LMN128` по +11 (стёртый/чужой); SetUp любой версии с id не трогается. esxDOS MB03+SD —
по-прежнему только области 0–2 (область 3 — ROM пользователя). Стенд: область 3 и испорченный блок 7
области 1 — «SetUp installed into block 7 of Area N», SetUp стартует, блоки побайтно равны образу.
Там же: при первом старте `flash.bin`/`fram.bin` не появлялись на карте — с 2026-11-24 запись ждёт кадр
с запасом 3 мс или 30 с, а выключение раньше оставляло образ только в PSRAM. Теперь `ElmFlash::bootFlush`
(конец `Elm::bootSettings`) пишет недостающие файлы (и пустую FRAM) и секторы установки сразу при загрузке.
Тест: `debug/DVp2w-elm-setupall2-1.1.4`, NOT hw-tested.

### 2.4 Личности `e_zxi_000`

00 ZX48, 01 128/+2, 03 128+2A (`#1FFD`, all-RAM, в all-RAM esxDOS не трапится), 05 Pentagon 128,
06 P512, 07 P1024 v2.2 (`#EFF7`), 08 Pentagon 4096 (пейджинг rdacomp), 09 Didaktik Gama 80.
Заводские defaults SetUp: **Machine = 01 (128/+2)**, не Pentagon 1024. FLASH6 на время работы
ставит `e_zxi_000 = 0` и возвращает — живая смена личности в природе есть, но ей нужен только
пейджинг, не тайминг.

Gama 80 (Reference «Didaktik Gama 80KB memory»): два 32 КБ листа с `#8000`, лист выбирает
i8255: `OUT 127,x` (управляющее слово, BSR → бит PC0) или порт 95. Тайминг 48K.

## 3. Как ложится на эмулятор

Весь код — `src/speccy/machines/Element/` (правило CLAUDE.md), новые файлы `ElmFlash.{h,cpp}`
(образ, JEDEC-модель, файл), `ElmSetup` внутри `Element.cpp` (вход, FRAM), `ElmMachine.{h,cpp}`
(личности). Горячие пути не растут: окно flash/FRAM идёт через уже существующую таблицу
`Elm::wrKind` по старшему байту и шлюз `g_atm_ro` бит 5 → `Elm::memWrite`.

### 3.1 Образ flash

- 512 КБ в butter PSRAM (A_ELM и так требует ≥ 4 МБ; `Buffer::pageBudget` резерв как у HGFX;
  **измерить** остаток PSRAM после URAM 2 МБ + HGFX 128 КБ + очередь + гистограмма).
- Файл `/pico-speccy/roms/element/flash.bin` (решение владельца 2026-11-02: рядом с
  `/pico-speccy/roms/yrw801.rom`, не `/roms/` и не `/.config/`; чтение как `readFileToButter`). Нет файла →
  собрать: область 0 = `sinclair_128k_0/1` ×2 + esxDOS (8 КБ встроенный + `#FF`) + `#FF` + `#FF` +
  `#FF` (SetUp пуст); область 1 = наши ROM +3 как заменитель +2A (или добавить `roms/plus2a` из
  FLASH2A — 64 КБ flash, решает владелец); область 2 = `sinclair_48k` ×4 + esxDOS; область 3 = `#FF`.
- Запись обратно: грязные сектора по 4 КБ; сброс в файл сразу при закрытии окна flash
  (`OUT (#53),0` в конце сеанса FLASH6), отложенный как `Nvram24::flush` в остальное время и
  принудительный на пути F12 / смены машины (рядом с `Nvram24::flush()` в ESPectrum.cpp). Так
  прошитый SetUp переживает F12: PSRAM теряется, образ читается из файла. Полный дамп с реальной платы (если появится) кладётся тем же файлом
  и даёт Rescue ROM и настоящие +2A.

### 3.2 ROM из области

- `rom[0..3]` = `flash + area*128K + block*16K`; `MemESP::rom` их уже 64, `plus3Remap` умеет `#1FFD`.
  Выбор блока: `#1FFD` бит 2 <<1 | `#7FFD` бит 4; для личностей без `#1FFD` старший бит = 0.
  `e_zxi_00F` при записи перепривязывает ROM (вступает в силу сразу, как на плате после reset —
  здесь достаточно «при следующем сбросе» + немедленно для standalone-проверок).
- esxDOS: `e_zxi_02D` бит 0 = 1 → `DivMMC::esxdos_rom` = первые 8 КБ блока 4 области; 0 → встроенный
  (роль «FPGA rescue»). `e_zxi_001`: 00 = DivSD выключен, 03 = только SD (ZC), FE = ничего не менять.

### 3.3 Окна типа 6 и E (zone 0)

- `Elm::remap`: тип 6 → `ramCurrent[0/1]` = страницы flash (`area*16 + (p & 15)`), слоты в `wpSlots`
  (запись только через `memWrite` → `W_FLASH`); тип E → Area1 = FRAM 8 КБ (куча, `tryMalloc`),
  запись по биту 1 `#53`. Чтение — прямой указатель, цена 0.
- `W_FLASH`: автомат SST39SF040 (состояния unlock1/unlock2/cmd/erase-unlock), `#30` = сектор в `#FF`
  + dirty, `#A0` = `AND`, `#F0`/прочее = сброс. `tools/elm_flash_test.cpp` — прогон сценария FLASH6
  (стирание 4 секторов, запись 16 КБ, проверка) без прошивки, как `elm_sdma_test`.

### 3.4 FRAM и вход в SetUp

- FRAM 8 КБ → `/pico-speccy/roms/element/fram.bin`, рядом с flash (`Nvram24`-образец: грязный флаг, отложенная
  запись, принудительная при выходе). Нет файла → `#2000..#203F` = таблица defaults из SetUp (§2.3),
  **кроме** `e_000`: владелец решает, оставить ли Pentagon 1024 (сейчас) или заводской 128/+2.
- Старт машины (`requestMachine` A_ELM): FRAM `#2000+N` → `eWrite(N, v)` для N из списка §2.3
  (через `eWrite`, чтобы сработали скорость, SID, remap). F11 FRAM не перечитывает (как плата).
- Вход: пункт Machine → eLeMeNt ZX → «SetUp» и горячая клавиша на сбросе (механизм TS-Conf «Reset
  to…», `ESPectrum.h:149`; какую клавишу держать — выбрать владельцу; на плате Space при сбросе =
  esxDOS reboot, это уже совпадает). Вход = `rom` на `#0000` := блок 7 области, `CPU::reset`.
  Блок 7 пуст (`#FF`) → сообщение «SetUp не прошит: запустите FlashSetUp_V2.tap».
- Снятие SetUp ROM: при первой записи в `#7FFD` после входа (SetUp делает её первой командой из
  ОЗУ, заглушка на `#0000` портов не трогает; `Ports.cpp` уже зовёт `Elm::remap()` на `#7FFD`).
  После этого `RST 0` попадает в обычный ROM, как на плате. Standalone `setup.tap` ничего этого
  не требует.
- Счётчики FRAM: PowerUp++ при старте машины, Reset++ на F11, Nmi++ на NMI, OnTime раз в минуту —
  дёшево и даёт «User level» на первом экране.
- Ответы регистров: `e_zxi_0EF` — значение неизвестно (строка «FPGA board: … unknown» в SetUp);
  посмотреть на стенде, что он печатает для 0/1/2, выбрать «Au». `#11` → `#FF`, `#17` → ничего.
  `g_zxi_0FF` (flash ядра для ElCoreUp) → 0: проверить, что ElCoreUp честно говорит «не найдено».

### 3.5 Личности при сбросе

Самая дорогая часть. Сейчас A_ELM = флаги и тайминг Pentagon 1024 (`Z80Ops::isPentagon/is1024`),
тайминг выбирается по `Config::arch` в `CPU.cpp:189/378`, `Video.cpp:5569/8126`. Нужно вынести
«профиль тайминга» (`tStatesPerLine`, таблицы contention, `isPentagon`-ветки) в переменную,
которую A_ELM ставит при сбросе из `e_zxi_000`, а остальные арки — из `arch`, как сейчас.
Порядок: 48 → 128/+2 → +2A (`plus3Remap`, all-RAM без трапов esxDOS, `#1FFD` блок ROM) →
Pentagon 128/512/1024 (маски страниц, `#EFF7`) → Gama 80 (§3.6) → Pentagon 4096 (пейджинг
rdacomp `#7FFD/#DFFD/#1FFD/#EFF7/#FDFD`, 4 МБ только с `e_zxi_038` и butter ≥ 8 МБ).
Живая запись `e_000` (FLASH6) меняет только пейджинг ROM/RAM, тайминг — со сброса; в журнале
одна строка, как сейчас.

### 3.6 Didaktik Gama 80

48K тайминг + ROM блок 0 области (DG.ROM = Gama 89 — уже есть как overlay, ставится `FlashDG`) +
i8255 на `#1F/#3F/#5F/#7F`: запись `#7F` с битом 7 = 0 → BSR: `(v>>1)&7 == 0` → PC0 = `v&1`;
запись `#5F` → PC0 = `v&1`; PC0 = лист 32 КБ на `#8000-#FFFF` (страницы ram 2-3 / 4-5). Порт `#1F`
на запись и `#5F` на чтение уже «принтер Gama 89» (ничего не делают) — не ломать Kempston.
Проверка: `OUT 127,1: POKE 32768,1: OUT 127,0: PRINT PEEK 32768` → 0.

### 3.7 Остальные пункты SetUp → существующие настройки

Однострочники при старте и при записи регистра: `e_002` биты (beeper, TSFM, Covox, SID, SAA, tape
input), `e_003` джойстик, `e_01D` GigaScreen → `Config::gigascreen`, `e_010/011` Timex/ULA+,
`e_02C` Z-Controller, `e_02E` DMA, `e_02F` LIFO, `e_031/032` разгон (у нас потолок 14 МГц),
`e_038` AA-RAM (только как «есть/нет»). Видеопункты (`e_01E/01F`, scan line, DVI) — хранить.

## 4. Фазы и проверки

Каждая фаза — стенд `build-all/host`, потом DVp2w (`debug/DVp2w-elm-setup-N`).

### Фаза A. Flash: образ, области, ROM, FLASH6
Проверка: `FLASHESX` (блок 4), `FLASH48`/`FLASHDG` (область 2), `FLASH128` (0), `FLASH2A` (1),
`FLASHBS`/`FLASHSE`/`FLASHZXD`/`FLASHDIA` (3) все заканчиваются «All successfully done» и
проверка 0 ошибок; `e_zxi_00F = 3` + сброс → BSROM/SE BASIC грузятся; `e_zxi_02D` бит 0 переключает
esxDOS между блоком 4 и встроенным; файл `flash.bin` после перезапуска стенда тот же.
`tools/elm_flash_test.cpp` зелёный.

### Фаза B. FRAM, вход в SetUp, применение при старте
Проверка: `FlashSetUp_V2.tap` прошивает блок 7; вход через меню → первый экран (статистика,
«FPGA Core 26/10/12», версия SetUp, тип платы); стрелки/Enter меняют пункты; CPU speed → 14 МГц
действует сразу; `r` → BASIC; CS+L → defaults; `setup.tap` даёт то же без прошивки; `fram.bin`
переживает перезапуск, и при старте машины регистры равны FRAM (`OUT 30779,N: PRINT IN 31035`).
`ElCoreUp.tap` не вешает машину.

### Фаза C. Личности 48 / 128 / +2A / Pentagon 128-1024

**Сделано 2026-11-03 (стенд, NOT hw-tested):** `g_elm_personality` взводится в `CPU::reset`
(`Elm::armPersonality`: FRAM при включении, живой `e_zxi_000` потом), предикаты
`Config::isElm48Raster/isElm128Raster/isElmPentRaster/isElmPlus3` стоят рядом с `isEvo*Raster`
в цепочках такта кадра (CPU.cpp), флагов (`Elm::cpuFlags`: is48/is128/isP3/isPentagon/is512/is1024,
плавающая шина, `target`), растра и класса видеорежима (Video.cpp), звука и contention
(ESPectrum.cpp), таблицы ожиданий +2A (`wait_st`). Применяется только при сбросе. Личность 48
оставляет `#7FFD` бит 4 как выбор блока ROM (esxDOS выходит в 48 BASIC через `OUT #7FFD,#10`;
FLASH48 заполняет все четыре блока) — иначе с областью 0 машина молчала. +2A: `Plus3::portWrite`
после `plus3Remap` возвращает окна URAM (`Elm::remap`). 08 (P4096) пока = 1024, 09 (Gama) = 48K
тайминг + страницы i8255 (фаза D).
Проверка на стенде (2026-11-03, против стоковых машин, область экрана попиксельно):
ULA 128E +3 test — равен стоковым 128K и +3; Float48K — таблица плавающей шины равна
стоковому 48K; 48K NEC Contention (IR contention) — равен стоковому 48K; +2A-+3 Paging Tests
(Woodmass), тест 1 «7FFD then 1FFD» — все строки `expected` совпали на личности +2A (стоковый
+3 на стенде этот тест даже не признал). Timing Tests 48K не грузится и на стоковом 48K
(свой лоадер), не показатель.

**ROM-окно было записываемым (hw 2026-11-03, владелец: ula48.tap на личности 00 «рисует
только на папере и виснет»; воспроизведено и исправлено на стенде):** блоки ROM лежат в
образе флеша в butter PSRAM, а `MemESP::writebyte` отбрасывает запись только по указателям
ниже базы PSRAM — каждая запись в #0000-#3FFF ложилась в блок области. У ula48.tap вектор IM2
(таблица #FE00, JR в #FFFF поверх байта ROM) попадал в мусор, RAM-fill ROM проходил все 64 КБ.
Фикс: `Elm::windowsRebuild` ставит бит 0 `g_atm_ro` (правило ATM «окно 0 показывает ROM»),
пока зона 0 на штатной раскладке; хук записи в CPU.cpp пропускает через этот бит RAM divMMC и
страницу 0 Pentagon (`romWindowDrops`, вынесен из строки: inline стоил 532 Б RAM, хелпер 96 Б).
Диагностика, которая это нашла: `PICOSPECCY_PCLOG=from:count` и `PICOSPECCY_PEEK=addr:len`
в host_main.cpp. Стенд после фикса: ula48.tap на личности 00 (и через `.tapein` esxDOS)
попиксельно равен стоковому 48K.

**Shift+R в SetUp = сброс машины (стенд 2026-11-03):** SetUp пишет регистры и уходит через
`RST 0` (C544). Эмулятор к этому моменту уже вернул обычный ROM (первая запись в порт), и
`RST 0` просто продолжал ROM с #0000 в ТЁПЛЫЙ esxDOS (память divMMC и состояние браузера
целы), а личность, применяемая только при сбросе, не менялась. У владельца после такого
рестарта NMI-браузер иногда отвечал «No such FILE or DIR, 0:1» (тот же симптом описан в
заметках Next: esxDOS не прошёл холодный init в $0000). Теперь `Elm::rst0` (из
`Z80::decodeRST`, только RST 0) пишет одну строку `[ELM] SetUp RST 0 at C544: e_000=.. e_001=..
e_00F=.. e_02D=.. divmmc map= con= -> machine reset`, а `Elm::takeReset` в конце кадра делает
`DivMMC::reset()` + `ESPectrum::reset()` (как «Soft Reset» меню): esxDOS грузится холодно
(«Detecting Devices»), затем `[ELM] personality NN at reset`. Вход в SetUp логируется строкой
`[ELM] SetUp ROM (block 7 of Area n) boots at this reset`. Тестовая прошивка
`debug/DVp2w-elm-setupr-1.1.1` (build 2026-11-03 12:42, RAM 191840 Б: +32 Б за два флага и хук в decodeRST; она же несёт фикс ROM-окна).

### Фаза D. Gama 80, Pentagon 4096

**Gama 80 сделано 2026-11-03 (стенд, NOT hw-tested):** личность 09 = 48K тайминг + i8255:
`OUT 127,x` (управляющее слово BSR на бит 0 порта C) или `OUT 95,x` выбирают 32 КБ страницу на
`#8000-#FFFF` (страница 1 = `ram[1]`/`ram[3]`), `Elm::gamaOut` перед DAC-портами, `legacyZone`
знает про страницу. Проверка: `GAMATEST.TAP` (CLEAR 32767; OUT 127,1: POKE 32768,1: OUT 127,0:
POKE 32768,2; печать PEEK при 127,1 / 127,0 / 95,1) даёт 1 2 1. Без CLEAR стек BASIC лежит в
переключаемой половине и машина падает — как и на настоящей Gama.
**Pentagon 4096 (08) сделано 2026-11-03 (hw-confirmed DVp2w: AWORLD на 08 + AA-SRAM, `debug/DVp2w-elm-aasram2`):** раскладка «Combined memory
variants» из Reference (ядро rdacomp Pentagon-4096): `#7FFD` биты 0-2 и 6-7 = биты страницы 0-4
(как Pentagon 512), бит 5 = бит 4096K (страница ≥ 2 МБ, половина AA-SRAM, которой нет —
отбрасывается, блокировки нет), биты 5-6 страницы = `#1FFD` биты 4 и 7 (Kay/Scorpion) ИЛИ
`#DFFD` биты 0-1 (Profi) ИЛИ `#FDFD` биты 0-1 (ATM1); `#1FFD` бит 0 = RAM страница 0 в #0000.
Реализация в `Elm::p4kOut`: `#7FFD` уходит в штатный декодер без бита 5 (`is512`), биты
расширения едут на штатной защёлке `#AFF7` (32 страницы на единицу у 512) через штатный
обработчик `#AFF7`, который переставляет банк при каждом изменении. `#EFF7` в 08 не действует
(`is1024` = 0). Проверка: `P4KTEST.TAP` (стенд, `mkbas.py` из scratchpad): страницы по
`#1FFD`=0/16/128/144 и `#DFFD`=0..3 печатают 1 2 3 4; `#7FFD`=16/80/144 — 1 5 6, `#FDFD`=1
даёт другую страницу (48); `#7FFD`=48 = страница 0 (бит 5 отброшен). **Ловушка теста:** в
48 BASIC 128-го ROM прерывание переписывает `#7FFD` из BANKM (23388), поэтому `OUT 32765,x`
без `POKE 23388,x` откатывается к следующему кадру (расширение через `#AFF7` при этом
остаётся) — первые прогоны показали «6662» именно из-за этого. На Pentagon 1024 (07) `OUT
8189` попадает в `#7FFD` по свободному декодеру (A15=A1=0) — так и на настоящем Pentagon.
Тестовая прошивка `debug/DVp2w-elm-p4k-1.1.1` (build 2026-11-03 13:32, RAM 191968 Б: +128 Б,
из них 108 в `Ports::output` за четыре сравнения в `isPort`).
Проверка: SetUp → Machine → 48 → `r` → 48 BASIC с таймингом 48K (тест бордюра/contention из
`debug/next-tests` или Overscan), Machine → +2A → `#1FFD` all-RAM, `OUT 30779,0: OUT 31035,x` из
манула (стр. 23) + сброс.

### Фаза D. Gama 80, Pentagon 4096
Проверка: §3.6; для 4096 — тест пейджинга rdacomp (искать на zxfiles.net).

### Фаза E′. EXTRA button и долгий reset (2026-11-04, hw-confirmed DVp2w «работает», `debug/DVp2w-elm-extra-1.1.1`)

Две кнопки платы на PC-клавиатуре: **EXTRA = Win + F11** («ZX + F11» в мануале; владелец,
2026-11-04), **RESET = горячая клавиша hard reset (F11)**: отпущена раньше 1.5 с — сброс, держится
дольше — SetUp (мануал: «A long press of reset button (at least 1.5 sec) results into the SetUp»).
`Elm::extraKey` из `ESPectrum::processKeyboard` до горячих клавиш (иначе голая F11 сработала бы
как hard reset на нажатии), `Elm::extraTick` из `frameEnd`. Пока кнопка нажата, CPU стоит
(`CPU::paused`, как на плате), поэтому цифры не доходят до ZX, а кадр не перерисовывается.
Регистры меняются живьём, FRAM не трогается (где хранить выбор — дело SetUp); счётчик «Extra»
FRAM `#3FF0` растёт на каждое нажатие.
- **Короткое нажатие EXTRA** по `e_020`: 01 CPU speed — `e_030` по кругу 3.5 → 7 → 14 → 20; 02
  Machine — следующая личность по кругу 00 01 03 05 06 07 08 09 (`e_000` + restart с холодным
  esxDOS, как RST 0 SetUp); 03 GigaScreen — `e_01D` 0 ↔ последний ненулевой режим (по умолчанию
  03 auto) + `applyRegs` live; 04 Warm reset — `takeReset()` = 2: `ESPectrum::reset` без
  `DivMMC::init` (RAM и divSD остаются, как в мануале); 05 Joystick — `e_003` по кругу 0..4 +
  `applyRegs`. Каждое с тостом «EXTRA: …» и строкой `[ELM] EXTRA: …` в логе.
- **Долгое (≥ 1.5 с) EXTRA при `e_021 = 01`** — «Spectrum menu»: радуга из четырёх косых полос
  (red/yellow/green/cyan, `drawRainbow`: `vga.fillRect` построчно в нижний бордюр справа, только в
  стандартной геометрии бордюра; HGFX/большие режимы — один тост) плюс тост «Spectrum menu: 1-8 =
  CPU speed»; 1–4 пишут `e_030` 0–3, 5–8 только при `e_031` бит 0 (OverClock; у нас всё ≥ 14 МГц
  идёт на 14). Отпускание закрывает меню без действия короткого нажатия и просит перерисовать
  бордюр (`brdChange` + `brdnextframe`).
- **RESET**: отпускание <1.5 с = тот же путь, что HK_HARD_RESET (ram_file сброшен, `ESPectrum::reset`);
  ≥1.5 с = `Elm::enterSetup()` + reset, без прошитого блока 7 — тост «SetUp not flashed…».
  Alt+F11 «Reset to…» остаётся.
- Стенд: FRAM `e_020 = 01`, `e_021 = 01`; хостовый инжектор клавиш теперь умеет длительность
  (`frame:Key:N` кадров). `400:Left GUI:120,402:F11:110,500:3,700:F11:12,900:F11:110` → лог «EXTRA
  held: Spectrum menu», «CPU speed: 14 MHz», «reset button released: reset», «reset button held:
  SetUp», «SetUp ROM (block 7 of Area 0) boots at this reset»; скриншоты: радуга в правом нижнем
  углу бордюра, первый экран SetUp.
- Не сделано: «CPU speed» в диагностике (цветная рамка по скорости).

### Фаза F. SetUp ↔ настройки: личности в меню, двусторонняя синхронизация, откат неподдерживаемого (2026-11-04, hw-confirmed DVp2w «работает», `debug/DVp2w-elm-setup3-1.1.1`)

Владелец (2026-11-04): «1 добавить все машины в меню eLeMeNt; 2 все опции меняются синхронно в
роме и наших настройках; 3 максимально закрыть настройки SetUp vs мы — где не поддерживается,
показывать сообщение и возвращать предыдущее значение».
- **Личности в меню.** Machine → eLeMeNt ZX теперь 8 строк (48, 128/+2, +2A, Pentagon 128/512/
  1024/4096, Gama 80) = `NM_MACH_P(A_ELM, R_ELM, e_000)`: личность в битах 16–23 композита
  SET_MACHINE (все декодеры и так маскируют арку `& 0xFF`; один в `stagedIsSam` поправлен).
  `get_machine` отдаёт `Elm::personality()` (живой регистр или FRAM). Выбор на работающем
  eLeMeNt → `Elm::setPersonality`: `e_000` + FRAM + рестарт платы (как RST 0 SetUp, холодный
  esxDOS), без смены машины. Выбор с другой машины → `Config::elm_pers_req` (NVS), который
  `Elm::bootSettings` пишет в FRAM `e_000` при следующем старте eLeMeNt и стирает.
- **Наши настройки → SetUp** (`Elm::syncFromSettings`, в конце `Stage::commit` при A_ELM): джойстик
  (`e_003`: none/Kempston/Sinclair 2→02/Sinclair 1→03/Cursor→04), Timex `e_010`.0, ULAplus
  `e_011`.0, GigaScreen `e_01D` (0/1-2/3), TSFM `e_002`.1, Covox `e_002`.2, SAA `e_002`.4,
  Z-Controller `e_02C`, esxDOS `e_001` (0↔1, 02/03 сохраняются), AA-SRAM `e_038`.0, AY-стерео
  `e_018`.1 (ABC=1/ACB=0, другие раскладки не трогают), scan lines `e_01E` (0–4), DMA `e_02E`.0,
  пояс `e_03E` = `wifi_tz`, UART `e_039` (ZiFi off = 03). Отличия пишутся `eWrite` + FRAM, лог
  `[ELM] settings -> SetUp: e_xxx=..`.
- **SetUp → наши** (`applyRegs`, при старте и при выходе из SetUp): к прежним добавлены Covox,
  AY-стерео, scan lines (`graphics_set_scanlines` живьём), пояс; DMA и UART — reboot-класс:
  `Config::save` + перезагрузка платы при рестарте SetUp (`rebootWanted` сравнивает с тем, с чем
  загрузились). **Засев один раз:** первый раз, когда прошивка видит FRAM без метки `#3FE8 = A5`,
  эти шесть опций пишутся из NVS в FRAM (иначе нетронутая таблица SetUp включила бы UART/DMA и
  сбила пояс); дальше FRAM — единственный источник, как и для прежней группы.
- **Неподдерживаемое → сообщение + откат** (`validateRegs` при RST 0 SetUp, снимок регистров
  `s_preSetup` берётся в `enterSetup`): `e_000` 02/04/>09; `e_002`.0 = 0 (ULA audio off) → 1; `e_01A` ≠ 0 (Sega / DE9 UART) → 0; `e_01F` DVI →
  0; `e_02C = 02` (второй слот) → 01; `e_02D`.1 swap SD → 0; `e_030 ≥ 3` (20 МГц/OverClock) → 02;
  `e_031`.0 → 0; `e_03F` Diagnostic → 0. Регистр и FRAM возвращаются, тост «SetUp: not here: …»
  поднимается на первом кадре после сброса (внутри сброса его стирала перерисовка), лог «[ELM]
  SetUp: not supported here, kept as before: …». Хранятся без сообщений: `e_004` мышь/клавиатура,
  `e_002`.5 tape input, `e_010`.1, `e_011`.1, `e_019` SID, `e_02B` RTC (он уже гейтит RTC72421),
  `e_02D`.0/.7, `e_032`, `e_020/021`.
- Стенд: FRAM с `e_01F=01, e_030=03, e_031=01`, `PICOSPECCY_ELM_SETUP=1`, Shift+R
  (`900:Left Shift:14,904:R`) → лог «SetUp RST 0», «not supported here, kept as before: DVI,
  20 MHz / OverClock, OverClock», засев «FRAM seeded from the settings once». Меню личностей и
  `syncFromSettings` на стенде не прогонялись (навигация меню клавишами), только сборка.

### Фаза F′. divSD + MB02+ (2026-11-04, hw-confirmed DVp2w «работает», `debug/DVp2w-elm-mb02-1.1.1`)

Владелец: «а почему нельзя выбрать divSD+MB?». Причина была наша: `MB02::init` брал 512 КБ как
последние 32 страницы полосы `MemESP::ram`, а на eLeMeNt вся полоса — URAM.
- **Память по Reference** («512 KB DivSD RAM is shared with 512 KB MB02+ RAM»): на eLeMeNt у
  divMMC 64 банка (`DivMMC::banks()`, 16 на остальных машинах; таблица `DIVMMC_NUM_BANKS` = 64),
  и `Elm::mb02Ram` отдаёт карте те же 512 КБ (`MB02::setExternalRam`, прямой указатель, без пула).
  Без esxDOS или в swap-режиме карта получает свой блок из butter PSRAM. `MB02::init` на eLeMeNt без
  блока отказывает (его вызов в `setup()` до пулов), `Elm::bootSettings` после FRAM зовёт init сам.
- `e_001 = 02` ↔ `Config::mb02` в обе стороны (applyRegs / syncFromSettings), откат «MB-02+» снят,
  строка MB-02+ в Devices на eLeMeNt показана, MachineSwitch не выключает карту для A_ELM.
- **hw 2026-11-04, «вставляю диск — ничего не происходит»**, три причины: (1) бэкстопы в `CPU::reset`
  и `setup()` для A_ELM ставили `Config::mb02 = false` сразу после bootSettings (нашлось watchpoint'ом
  gdb на хосте); (2) WD2797 `#0F/#2F/#4F/#6F` совпадает с ЦАП SounDrive — `Elm::portOut` забирал
  записи, теперь при MB-02+ они идут контроллеру; (3) `Ports::dmaInput/dmaOutput` на eLeMeNt шли
  коротким путём `input()/output()` без ожидания DRQ: DMA забирал 1023 байта раньше WD, статус lost
  data, загрузчик BS-DOS (`#97B3`: Restore, DMA в `#8000`, `CP #18`) перечитывал сектор вечно.
- **hw, «по F11 / Alt+F11 не попасть в SetUp»**: EPROM MB-02+ ложился на `#0000` поверх ROM SetUp;
  `MB02::applyMapping` уступает страницу 0, пока `g_elm_nodiv`.
- **Страница 0 вдвоём с esxDOS:** при сбросе EPROM берёт её только с диском в дисководе A (иначе
  грузится esxDOS); пока страница 0 у карты (`g_mb02_page0`), divMMC не автомапится. Повторный
  `applyMapping` после `loadDiskMounts` (диски грузятся после сброса).
- **hw, «divSD+MB без диска: LFN Browser не открывается»**, две причины: (1) `Z80::doNMI` при MB-02+
  всегда включал SRAM страницу 0 карты (вход в NMI-меню BS-ROM), это прятало ловушку esxDOS на
  `#0066`; теперь только когда NMI карты (`!DivMMC::enabled || nmi_bypass`); (2) отложенный старт DMA
  для MB-02+ касался любого I/O-порта, а esxDOS MB03+SD читает SD через DMA с `#EB`; теперь только
  порты WD2797. Стенд: карта с SYS/NMI.SYS (`cardsys.img`), NMI → `.browse v1.02 NMI` в обоих режимах.
- **NMI (владелец):** BS-DOS загрузился (`MB02::booted`, EPROM был на `#0000` после сброса) → NMI
  его: `DivMMC::nmi_bypass` пропускает ловушку `#0066` один раз; иначе NMI esxDOS. Меню не нужно.
- Стенд: System512KB.mbd в A → BS-DOS BASIC (`Prog:0 Vars:0 Free:41606`) на eLeMeNt и на 128K
  (с esxDOS и без); без диска esxDOS и его NMI-браузер; с BS-DOS NMI даёт красный бордюр с
  полосками — что делает NMI MB-02+ на самом деле, проверить на железе. BS-DOS 512KB занимает всю
  общую память, рабочая память esxDOS при этом затирается (так и на плате по Reference).

### Фаза E. Хвосты §3.7, счётчики, (опц.) `roms/plus2a`, импорт полного дампа flash.

**Сделано 2026-11-03 (hw-confirmed DVp2w: строки `SetUp -> settings` в логе, `e_001`, AA-SRAM):** регистры SetUp → настройки эмулятора
(`Elm::applyRegs`): `e_003` джойстик (00 none, 01 Kempston, 02 Sinclair left = Sinclair 2,
03 Sinclair right = Sinclair 1; 04 «клавиши» не трогает), `e_010` бит 0 Timex, `e_011` бит 0
ULAplus (при выключении — хуки меню: сброс регистров Timex / `ulaPlusDisable`), `e_01D`
GigaScreen (00 off, 01/02 on, 03 auto), `e_002` бит 1 TSFM, бит 4 SAA1099 (бит 3 SID — как
раньше `ElmSid::mute`, бит 2 MonsterBlaster — теперь гасит собственные DAC-порты `#0F..#5F`),
`e_02C` Z-Controller (вкл/выкл + `zc_init`/`zc_shutdown`). Только во время работы, без
`Config::save()`: SetUp — конфигурация самого eLeMeNt (FRAM), NVS остаётся пользовательским.
Две точки входа: `Elm::bootSettings()` из `ESPectrum::setup` сразу после `Buffer::initPools`
(FRAM лежит в butter PSRAM; до VIDEO/AUDIO, чтобы загрузка выделила подсистемы по SetUp) и
конец `Elm::reset` при живом сбросе (после первого кадра, `s_running`) — с хуками и тихим
`nm::Stage::reconcileQuiet()` (новая функция UiStage: `reconcileSubsystems` без правок меню и
без диалогов; отказ бюджета только логируется). Лог: `[ELM] boot settings from FRAM: ...`,
`[ELM] SetUp -> settings (boot): ...`, `[ELM] SetUp -> settings: ...`. Проверка на стенде:
FRAM с `e_01D=03`, `e_002=1F`, `e_02C=00` даёт при загрузке «timex gigascreen tsfm saa zc-off»;
`OUT 30779,3: OUT 31035,0` + `e_002=#23` + F11 даёт «joystick saa-off». Ловушка: первый
вариант вызывал живой apply на сбросе ещё внутри `setup()` (там `Elm::reset` идёт дважды) —
`reconcileQuiet` до инициализации подсистем; отсюда гейт `s_running`.
Не привязано (хранятся): `e_002` биты 0 (beeper) и 5 (tape input), `e_02E` DMA (reboot-класс,
у eLeMeNt свой DMA), `e_031/032` разгон, `e_038` AA-RAM, видеопункты `e_01E/01F`, `e_004`
мышь/клавиатура, `e_018` модель TSFM, `e_01A` DE9.

**Shift обязателен для R и L** (владелец, 2026-11-03): подсказка SetUp пишет «R-restart /
L-load BIOS defaults+restart», но его клавиатурная процедура (E4FC) отдаёт ROM-коды, и
сравнение идёт с #52/#4C — заглавные, т.е. Caps Shift + R / L. На стенде простое L ничего
не делает, Shift+L перезапускает. Shift+L копирует таблицу D3ED в FRAM (#2000..#203F через
окно E) и уходит по тому же пути C508 → RST 0, живые регистры не меняет: после него FRAM =
дефолты (`e_000=01`, `e_02D=00`, `e_030=00`), а машина до следующего включения живёт на
старых значениях, кроме `e_001` и `e_030`, которые код перезапуска пишет из FRAM.
**«No such FILE or DIR, 0:1» после Shift+L на DVp2w (владелец) — на стенде не
воспроизводится** (SYS/BIN его карты, `debug/DVp2w-elm-setupr`): в UART-логе путь тот же, что
у работающего Shift+R (`e_000=07 e_001=01 e_02D=01`), единственное отличие регистров — CPU
3.5 МГц вместо 7 (`mult=0`). Экран — чёрный папер и иконки нижней строки LFN-браузера: ошибку
печатает NMI-браузер. Для следующего раза собрана `debug/DVp2w-elm-zctrace3-1.1.1`
(ZC_PORT_TRACE: кадры SPI-команд divSD и вызовы esxDOS RST 8 с результатами, первые 400) —
по логу будет видно, какой вызов esxDOS возвращает ошибку.
**Причина найдена по трассе zctrace3 (2026-11-03):** после Shift+L esxDOS стартовал ТЁПЛЫМ:
в трассе нет чтения сектора 0 (MBR), только 2 сектора корня, 2 сектора SYS и чужой кластер
(282912 вместо 282960 у ESXDOS.SYS в удачной загрузке 2026-11-02), затем «No such FILE or
DIR». `DivMMC::reset()` сохраняет память divMMC, и esxDOS продолжает с состоянием монтирования
прежней личности/ROM (Shift+L: 07 → 01 из FRAM, `e_02D` 1 → 0 — встроенный ROM вместо блока 4).
Shift+R не менял ни того, ни другого, потому и работал. Фикс: рестарт из SetUp делает
`DivMMC::init()` (холодный esxDOS, как «Hard Reset» и как «reset + Space» на плате): в логе
`reset after the SetUp (...): esxDOS cold`, на стенде «Detecting Devices» после каждого
рестарта. Файлы SYS владельца и стенда совпадают побайтно (ESXDOS.SYS, NMI.SYS).
**AWORLD (Another World, LnxPckg APP, 1.38 МБ):** лоадер требует число свободных 8 КБ страниц
URAM в Protect-режиме ≥ слову из заголовка APP (#1C = #0180 = 384 = 3 МБ) — это только
машина с AA-SRAM (`e_zxi_038 = 01`, 4 МБ URAM), на голом eZX с 2 МБ лоадер честно пишет
«Out of Memory» на любой личности (стенд: 05, 07, 08 одинаково). AA-SRAM не эмулируется;
на DVp2w бюджет butter 7552 КБ (pages=128) позволил бы +2 МБ (MEM_PG_CNT 256 для eLeMeNt,
FULL_PAGES 512, Protect-пул по формуле платы) — отдельная задача, если владелец захочет.
**Z-Controller у eLeMeNt:** на плате это второй SD-интерфейс рядом с divSD; в эмуляторе одна
карта и общие SD-порты с esxDOS, меню держит esxDOS / MB-02+ / ZC взаимоисключающими, поэтому
ZC «не включается», пока esxDOS включён. Фаза E ставит ZC из `e_02C` только при выключенном
esxDOS, иначе пишет `zc-held-by-esxdos` в строку лога.
**`e_zxi_001` 02/03 (вопрос владельца):** 02 = divSD + MB02+ — те же 512 КБ памяти доступны
и через регистры страниц MB02+ (порт #17, интерфейс MB-02+); 03 = «SD only» — слоты SD есть,
а памяти divSD/MB02+ нет (для внешнего divIDE/MB-03+ со своей памятью). У нас привязан
только 01 (esxDOS через DivSD); 00/03 (без divSD) и 02 (MB-02+) — reboot-класс, хранятся.
**Обновление в тот же день:** `e_001` теперь привязан как reboot-класс: 00 и 03 → `Config::esxdos = 0`,
01 и 02 → `Config::esxdos = 3` (DivSD), запись в NVS + лог «esxDOS (divSD) on/off at the next
boot»; FE (rescue режим флешеров) и FF ничего не меняют. 02 (divSD+MB02+) — те же 512 КБ видны
и через регистры страниц MB-02+ (порт #17); наша эмуляция MB-02+ (BS-DOS) отдельная и в меню
взаимоисключаема с esxDOS, поэтому 02 = 01 для нас (MB02+ не поднимается).

**AA-SRAM (`e_zxi_038` бит 0) сделано 2026-11-03 (hw-confirmed DVp2w: `URAM 4 MB`, AWORLD на 08 + AA-SRAM грузится, `debug/DVp2w-elm-aasram2`):** 4 МБ URAM = 512
страниц по 8 КБ, `MEM_PG_CNT` 256 для eLeMeNt (`Config::wantedPages`: `Config::elm_aasram`
из NVS, только с butter PSRAM). Protect-пул по таблице Reference: ZX ≤ 512 КБ → 128 / 384
страниц с базы 128; Pentagon 1024 → 64 (база 128) / 256 (база 256); 4096 → вся URAM
(Reference: «there is no Protect mode» — hw 2026-11-03: с 64 на вершине AWORLD на 08 + AA-SRAM
писал OOM; его лоадер проверяет g_011/012 ДО того, как сам ставит `e_000 = 06`, см. COD #122/#149). Пул не степени двойки берётся по модулю (`poolIndex`). Full-режим адресует
`fullPages()`. Pentagon 4096 с AA-SRAM получает бит 4096K (`#7FFD` бит 5, `#DFFD`/`#FDFD`
бит 2) как бит 2 защёлки `#AFF7` → 256 страниц по 16 КБ. Размер страниц решается в `setup()`,
поэтому SetUp пишет выбор в NVS (`aasramWanted`), а рестарт из SetUp при `Elm::rebootWanted()`
(изменился размер URAM или включение esxDOS) перезагружает плату (`esp_hard_reset`), как
смена машины. Лог при загрузке: `[ELM] URAM 4 MB (AA-SRAM)`. Проверка: AWORLD на личности
05 с AA-SRAM доходит до заставки и меню (384 свободных страниц ≥ #0180). Бюджет PSRAM на
DVp2w: после 4 МБ страниц в арене butter остаётся ~3.9 МБ (образ флеша 512 КБ, divMMC 512 КБ,
HGFX, LCU, read-ahead) — смотреть строку `Buffer::initPools` в логе платы.

**ZC на втором разделе карты — сделано 2026-11-03 (стенд, hw не проверено, `debug/DVp2w-elm-zc2-1.1.1`):**
один SPI-автомат DivMMC и два носителя (`DivMMC::Medium`, `selectCard`): карта esxDOS (#E7/#EB)
и карта ZC (#77/#57), у каждой своё окно реальной карты (раздел, primary или логический из цепочки
EBR, либо вручную start/len) или файл-образ; буфер сектора общий (Z80 не может перемежать две
SD-транзакции внутри блока). Меню: Devices > Z-Controller (вкл + «SD card / image»), Devices >
esxDOS > «Disks hd0 / hd1» (для DivSD — строка «SD card: whole card / part N / window»; для DivIDE
hd0 — образ или карта с RO/F6 как у IDE). Взаимоисключение esxDOS / MB-02+ / ZC снято; `e_02C`
ставит ZC без оглядки на esxDOS. На стенде: 48K + esxDOS на разделе 1 + ZC на разделе 2 (и на
логическом 5, и на образе) — esxDOS грузит ленту с раздела 1, BASIC-тест через #77/#57 читает
метку раздела ZC. MB-02+ на eLeMeNt по-прежнему выключен (его 32 страницы легли бы на URAM), так что
`e_001 = 02` всё ещё «= 01». Подробности: docs/notes/storage-tape.md, раздел «Two cards on one SPI machine».

## 5. Риски и решения владельца

1. **PSRAM**: +512 КБ на DVp2w — измерить до фазы A; если не влезает, flash только на 8 МБ butter.
2. **Личности = правка выбора тайминга в общих горячих путях** (CPU.cpp/Video.cpp) — делать через
   один профиль, выставляемый при сбросе, без новых `if (isElm)` в RAM-функциях.
3. **Default личности** при пустом FRAM: Pentagon 1024 (сейчас) или заводской 128/+2 — решить.
4. **+2A ROM**: заменитель +3 или 64 КБ `roms/plus2a` из FLASH2A (образы Amstrad, как +3).
5. **SetUp не вшиваем** — ставится `FlashSetUp_V2.tap`; альтернатива (вшить 16 КБ) не нужна.
6. **Rescue mode** (NMI+Reset) без дампа реальной платы невозможен — только с импортом `flash.bin`.
7. Клавиша «долгий reset» на DVp2w — выбрать.

## 8. Сделано 2026-11-02 (стенд `build-all/host`; hw-confirmed DVp2w тем же вечером по UART-логу, не по пунктам)

Код: `src/speccy/machines/Element/ElmFlash.{h,cpp}` (образ, автомат SST39SF040, FRAM, файлы),
правки `Element.cpp` (раскладки 6 и E, `W_FLASH`/`W_FRAM`, `romsBind`, `divRomApply`, вход в SetUp,
умолчания, FRAM при включении, счётчики), `DivMMC::setRom` + `g_elm_divrom`, `Elm::flush` в главном
цикле и на пути перезагрузки (OSDMain), вход в SetUp из Alt+F11 «Reset to…» (пункт
Machine → eLeMeNt ZX → «SetUp (firmware)» владелец убрал 2026-11-03). Тест `tools/elm_flash_test.cpp` (модель без прошивки) зелёный.

Проверено на стенде:

- Образ собирается из ROM прошивки и пишется в `/pico-speccy/roms/element/flash.bin`;
  блоки области 0 побайтно равны `128-0.ROM`/`128-1.ROM` флешера, область 2 — `48.ROM`.
- `FLASHESX` (блок 4 области 0), `FLASHZXD` (4 блока области 3), `FlashSetUp_V2.tap` с ленты
  (блок 7) — «All successfully done», проверка 0 ошибок, содержимое блоков равно образам.
- Прошитый esxDOS 0.8.9-MB03+SD грузится (`e_zxi_02D` бит 0); область 3 из FRAM грузит
  «ZXDIR CustomROM»; встроенный esxDOS подставляется, когда блок 4 области пуст.
- SetUp: первый экран (статистика, версия 20260406, ядро 20261012, плата Au, часы), меню
  Machine, смена значения, Shift+R (restart → BASIC), Shift+L (defaults → FRAM = таблица SetUp).
  Стандартный `setup.tap` не проверялся отдельно (тот же код).
- `ElCoreUp.tap`: находит eLeMeNt, ядро 2026/10/12, «FLASH chip type: unknown», выходит по
  ошибке файла — не виснет.

Что узналось сверх §2 (всё из прогонов, не из документов):

- **URAM включён и при `e_zxi_001 = #FF`** (устаревший «Ultimate»): FLASH6 v20240215 из
  FLASHROM.TAR и SetUp не трогают `g_zxi_010`, только `e_001 = FF`. Считается режимом Full
  (`uramMode()`).
- **esxDOS MB03+SD держит байт управления divMMC в LIFO**: `IN A,(#2B)` → `OUT (#E3)`. Без
  LIFO он висит на «Detecting Devices». Умолчания регистров взяты из таблицы defaults SetUp
  (LIFO, DMA, RTC, …), поверх — `e_000 = 07` (Pentagon 1024, решение владельца) и
  `e_02D` бит 0 = 1 (esxDOS из прошитого блока, если он есть). **С 2026-11-20 (владелец)
  `e_02D` бит 0 = 0, как в таблице SetUp: по умолчанию наш встроенный esxDOS (версия из
  `Config::esxdos_ver`); MB03+SD из блока 4 — выбором в SetUp. Действует только при пустой
  FRAM. NOT hw-tested.**
- **`e_zxi_00F` живьём меняет только окно flash, не ROM**: FLASH2A/FLASHZXD пишут 4 блока из
  BASIC с возвратом в него между блоками; при живой смене ROM BASIC умирал. ROM области и
  esxDOS берутся при сбросе.
- **Включение vs сброс**: FRAM `#2000+N → e_zxi_N` только при первом сбросе сессии (включение);
  F11 сохраняет регистры как есть (как плата). Пустой FRAM получает таблицу умолчаний и
  нулевые счётчики; PowerUp/Reset/Nmi/OnTime ведутся (`framCount`).
- **Вход в SetUp**: Alt+F11 («Reset to…» → «SetUp (firmware)», аналог долгого reset на плате;
  пункт в меню Machine убран 2026-11-03). Блок 7 как `rom[0..3]` + `g_elm_nodiv` (esxDOS не трапит #0000) до первого
  OUT в порт eLeMeNt после сброса (SetUp начинает с `#7FFD`, затем `e_zxi_030`); заглушка на
  `#0000` портов не трогает. Пустой блок 7 → тост «SetUp not flashed: run FlashSetUp_V2.tap».
  На стенде вход — `PICOSPECCY_ELM_SETUP=1`.
- `e_zxi_0EF = 0` SetUp и ElCoreUp показывают как плату «Au».

Проверка на DVp2w (владелец): esxDOS грузится как раньше; `.cd` в папку FLASHROM/FLASHESX,
`LOAD *"FLASHESX.BAS"`, area 0 → «All successfully done», F12 → заставка esxDOS `MB03+SD`; `FlashSetUp_V2.tap` (с ленты или
`.tapein`); Alt+F11 → SetUp (firmware) → первый экран; F12 → SetUp на месте; в логе
UART `[ELM] flash image …` без «no PSRAM for the 512 KB flash image».
