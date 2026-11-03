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

Следствие для нас: образ SetUp **не надо вшивать** — его ставит `FlashSetUp_V2.tap` из пакета
владельца, как на железе, а `setup.tap` работает без прошивки вообще. Лицензионный вопрос снят.

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
**Pentagon 4096 (08)** пока = 1024: пейджинг rdacomp (`#7FFD/#DFFD/#1FFD/#EFF7/#FDFD`) не начат.
Проверка: SetUp → Machine → 48 → `r` → 48 BASIC с таймингом 48K (тест бордюра/contention из
`debug/next-tests` или Overscan), Machine → +2A → `#1FFD` all-RAM, `OUT 30779,0: OUT 31035,x` из
манула (стр. 23) + сброс.

### Фаза D. Gama 80, Pentagon 4096
Проверка: §3.6; для 4096 — тест пейджинга rdacomp (искать на zxfiles.net).

### Фаза E. Хвосты §3.7, счётчики, (опц.) `roms/plus2a`, импорт полного дампа flash.

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
цикле и на пути перезагрузки (OSDMain), пункт Machine → eLeMeNt ZX → «SetUp (firmware)»
(`act_elmSetup`). Тест `tools/elm_flash_test.cpp` (модель без прошивки) зелёный.

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
  `e_02D` бит 0 = 1 (esxDOS из прошитого блока, если он есть).
- **`e_zxi_00F` живьём меняет только окно flash, не ROM**: FLASH2A/FLASHZXD пишут 4 блока из
  BASIC с возвратом в него между блоками; при живой смене ROM BASIC умирал. ROM области и
  esxDOS берутся при сбросе.
- **Включение vs сброс**: FRAM `#2000+N → e_zxi_N` только при первом сбросе сессии (включение);
  F11 сохраняет регистры как есть (как плата). Пустой FRAM получает таблицу умолчаний и
  нулевые счётчики; PowerUp/Reset/Nmi/OnTime ведутся (`framCount`).
- **Вход в SetUp**: Alt+F11 («Reset to…» → «SetUp (firmware)», аналог долгого reset на плате) или
  Machine → eLeMeNt ZX → «SetUp (firmware)». Блок 7 как `rom[0..3]` + `g_elm_nodiv` (esxDOS не трапит #0000) до первого
  OUT в порт eLeMeNt после сброса (SetUp начинает с `#7FFD`, затем `e_zxi_030`); заглушка на
  `#0000` портов не трогает. Пустой блок 7 → тост «SetUp not flashed: run FlashSetUp_V2.tap».
  На стенде вход — `PICOSPECCY_ELM_SETUP=1`.
- `e_zxi_0EF = 0` SetUp и ElCoreUp показывают как плату «Au».

Проверка на DVp2w (владелец): esxDOS грузится как раньше; `.cd` в папку FLASHROM/FLASHESX,
`LOAD *"FLASHESX.BAS"`, area 0 → «All successfully done», F12 → заставка esxDOS `MB03+SD`; `FlashSetUp_V2.tap` (с ленты или
`.tapein`); Alt+F11 → SetUp (firmware) (или Machine → eLeMeNt ZX → SetUp) → первый экран; F12 → SetUp на месте; в логе
UART `[ELM] flash image …` без «no PSRAM for the 512 KB flash image».
