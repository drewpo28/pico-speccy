# memdump.gdb — dump Z80 address space + all 8 physical RAM pages (128K)
# Usage (in Debug Console): source tools/memdump.gdb
# Then run task "Pico: Memory Dump" (Ctrl+Alt+D)

# Logical Z80 address space via current mapping. Handles BOTH layouts —
# pico-spec (4x16K slots) and pico-speccy (8x8K slots since the Next MMU
# refactor) — in pure GDB command language (the pico toolchain gdb has no
# Python). 8-slot builds write picospec_s{0-7}.bin (8K each) + the Next
# MMU/nextreg state to /tmp/picospec_next.txt; memdump.py picks whichever
# set is present.
# The extension resumes the target and converts only once this marker exists:
# the evaluate reply of `source` can come back before the script has finished.
shell rm -f /tmp/picospec_gdb_done
shell rm -f /tmp/picospec_s0.bin /tmp/picospec_s1.bin /tmp/picospec_s2.bin /tmp/picospec_s3.bin /tmp/picospec_s4.bin /tmp/picospec_s5.bin /tmp/picospec_s6.bin /tmp/picospec_s7.bin /tmp/picospec_mem0.bin /tmp/picospec_mem1.bin /tmp/picospec_mem2.bin /tmp/picospec_mem3.bin /tmp/picospec_next.txt
set $nslots = sizeof(MemESP::ramCurrent)/sizeof(MemESP::ramCurrent[0])
if $nslots == 8
  if MemESP::ramCurrent[0] != 0
    dump binary memory /tmp/picospec_s0.bin MemESP::ramCurrent[0] (MemESP::ramCurrent[0] + 8192)
  end
  if MemESP::ramCurrent[1] != 0
    dump binary memory /tmp/picospec_s1.bin MemESP::ramCurrent[1] (MemESP::ramCurrent[1] + 8192)
  end
  if MemESP::ramCurrent[2] != 0
    dump binary memory /tmp/picospec_s2.bin MemESP::ramCurrent[2] (MemESP::ramCurrent[2] + 8192)
  end
  if MemESP::ramCurrent[3] != 0
    dump binary memory /tmp/picospec_s3.bin MemESP::ramCurrent[3] (MemESP::ramCurrent[3] + 8192)
  end
  if MemESP::ramCurrent[4] != 0
    dump binary memory /tmp/picospec_s4.bin MemESP::ramCurrent[4] (MemESP::ramCurrent[4] + 8192)
  end
  if MemESP::ramCurrent[5] != 0
    dump binary memory /tmp/picospec_s5.bin MemESP::ramCurrent[5] (MemESP::ramCurrent[5] + 8192)
  end
  if MemESP::ramCurrent[6] != 0
    dump binary memory /tmp/picospec_s6.bin MemESP::ramCurrent[6] (MemESP::ramCurrent[6] + 8192)
  end
  if MemESP::ramCurrent[7] != 0
    dump binary memory /tmp/picospec_s7.bin MemESP::ramCurrent[7] (MemESP::ramCurrent[7] + 8192)
  end
  set logging file /tmp/picospec_next.txt
  set logging overwrite on
  set logging redirect on
  set logging enabled on
  printf "mmu: %02X %02X %02X %02X %02X %02X %02X %02X\n", MemESP::mmu[0], MemESP::mmu[1], MemESP::mmu[2], MemESP::mmu[3], MemESP::mmu[4], MemESP::mmu[5], MemESP::mmu[6], MemESP::mmu[7]
  printf "next_rom_mask=%02X nextRamReady=%d divmmc_mapped=%d romInUse=%d\n", MemESP::next_rom_mask, MemESP::nextRamReady, MemESP::divmmc_mapped, MemESP::romInUse
  printf "NextReg: sel=%02X 7FFD=%02X DFFD=%02X 1FFD=%02X 123B=%02X\n", NextReg::selected, NextReg::port7FFD, NextReg::portDFFD, NextReg::port1FFD, NextReg::port123B
  printf "reg05=%02X reg07=%02X reg08=%02X reg22=%02X reg69=%02X\n", NextReg::reg[0x05], NextReg::reg[0x07], NextReg::reg[0x08], NextReg::reg[0x22], NextReg::reg[0x69]
  printf "reg50..57: %02X %02X %02X %02X %02X %02X %02X %02X\n", NextReg::reg[0x50], NextReg::reg[0x51], NextReg::reg[0x52], NextReg::reg[0x53], NextReg::reg[0x54], NextReg::reg[0x55], NextReg::reg[0x56], NextReg::reg[0x57]
  printf "reg12=%02X reg13=%02X reg14=%02X reg15=%02X reg6B=%02X\n", NextReg::reg[0x12], NextReg::reg[0x13], NextReg::reg[0x14], NextReg::reg[0x15], NextReg::reg[0x6B]
  printf "divmmc: automap=%d conmem=%d mapram=%d bank=%d mapped=%d p0lo=%08X p0hi=%08X\n", DivMMC::automap, DivMMC::conmem, DivMMC::mapram, DivMMC::bank, MemESP::divmmc_mapped, (unsigned)MemESP::page0_lo, (unsigned)MemESP::page0_hi
  dump binary memory /tmp/picospec_nxtmmc.bin MemESP::nextDivRomBase (MemESP::nextDivRomBase + 8192)
  dump binary memory /tmp/picospec_nextrom.bin MemESP::nextRomBase (MemESP::nextRomBase + 65536)
  if DivMMC::bank_ptr[0] != 0
    dump binary memory /tmp/picospec_divbank0.bin DivMMC::bank_ptr[0] (DivMMC::bank_ptr[0] + 8192)
  end
  if DivMMC::bank_ptr[1] != 0
    dump binary memory /tmp/picospec_divbank1.bin DivMMC::bank_ptr[1] (DivMMC::bank_ptr[1] + 8192)
  end
  if DivMMC::bank_ptr[2] != 0
    dump binary memory /tmp/picospec_divbank2.bin DivMMC::bank_ptr[2] (DivMMC::bank_ptr[2] + 8192)
  end
  if DivMMC::bank_ptr[3] != 0
    dump binary memory /tmp/picospec_divbank3.bin DivMMC::bank_ptr[3] (DivMMC::bank_ptr[3] + 8192)
  end
  if DivMMC::bank_ptr[4] != 0
    dump binary memory /tmp/picospec_divbank4.bin DivMMC::bank_ptr[4] (DivMMC::bank_ptr[4] + 8192)
  end
  if DivMMC::bank_ptr[5] != 0
    dump binary memory /tmp/picospec_divbank5.bin DivMMC::bank_ptr[5] (DivMMC::bank_ptr[5] + 8192)
  end
  if DivMMC::bank_ptr[6] != 0
    dump binary memory /tmp/picospec_divbank6.bin DivMMC::bank_ptr[6] (DivMMC::bank_ptr[6] + 8192)
  end
  if DivMMC::bank_ptr[7] != 0
    dump binary memory /tmp/picospec_divbank7.bin DivMMC::bank_ptr[7] (DivMMC::bank_ptr[7] + 8192)
  end
  if DivMMC::bank_ptr[8] != 0
    dump binary memory /tmp/picospec_divbank8.bin DivMMC::bank_ptr[8] (DivMMC::bank_ptr[8] + 8192)
  end
  if DivMMC::bank_ptr[9] != 0
    dump binary memory /tmp/picospec_divbank9.bin DivMMC::bank_ptr[9] (DivMMC::bank_ptr[9] + 8192)
  end
  if DivMMC::bank_ptr[10] != 0
    dump binary memory /tmp/picospec_divbank10.bin DivMMC::bank_ptr[10] (DivMMC::bank_ptr[10] + 8192)
  end
  if DivMMC::bank_ptr[11] != 0
    dump binary memory /tmp/picospec_divbank11.bin DivMMC::bank_ptr[11] (DivMMC::bank_ptr[11] + 8192)
  end
  if DivMMC::bank_ptr[12] != 0
    dump binary memory /tmp/picospec_divbank12.bin DivMMC::bank_ptr[12] (DivMMC::bank_ptr[12] + 8192)
  end
  if DivMMC::bank_ptr[13] != 0
    dump binary memory /tmp/picospec_divbank13.bin DivMMC::bank_ptr[13] (DivMMC::bank_ptr[13] + 8192)
  end
  if DivMMC::bank_ptr[14] != 0
    dump binary memory /tmp/picospec_divbank14.bin DivMMC::bank_ptr[14] (DivMMC::bank_ptr[14] + 8192)
  end
  if DivMMC::bank_ptr[15] != 0
    dump binary memory /tmp/picospec_divbank15.bin DivMMC::bank_ptr[15] (DivMMC::bank_ptr[15] + 8192)
  end
  if MemESP::altRomBase != 0
    dump binary memory /tmp/picospec_altrom.bin MemESP::altRomBase (MemESP::altRomBase + 32768)
  end
  printf "reg8C=%02X altrom_rd=%d altrom_wr=%d altrom_sel=%d\n", NextReg::reg[0x8C], MemESP::altrom_rd, MemESP::altrom_wr, MemESP::altrom_sel
  dump binary memory /tmp/picospec_page0B.bin MemESP::nextPg[0x0B] (MemESP::nextPg[0x0B] + 8192)
  dump binary memory /tmp/picospec_page10.bin (MemESP::nextStrip+0x0) (MemESP::nextStrip+0x0+8192)
  dump binary memory /tmp/picospec_page11.bin (MemESP::nextStrip+0x2000) (MemESP::nextStrip+0x2000+8192)
  dump binary memory /tmp/picospec_page20.bin (MemESP::nextStrip+0x20000) (MemESP::nextStrip+0x20000+8192)
  dump binary memory /tmp/picospec_page21.bin (MemESP::nextStrip+0x22000) (MemESP::nextStrip+0x22000+8192)
  dump binary memory /tmp/picospec_page30.bin (MemESP::nextStrip+0x40000) (MemESP::nextStrip+0x40000+8192)
  dump binary memory /tmp/picospec_page31.bin (MemESP::nextStrip+0x42000) (MemESP::nextStrip+0x42000+8192)
  printf "slots: %08X %08X %08X %08X %08X %08X %08X %08X\n", (unsigned)MemESP::ramCurrent[0], (unsigned)MemESP::ramCurrent[1], (unsigned)MemESP::ramCurrent[2], (unsigned)MemESP::ramCurrent[3], (unsigned)MemESP::ramCurrent[4], (unsigned)MemESP::ramCurrent[5], (unsigned)MemESP::ramCurrent[6], (unsigned)MemESP::ramCurrent[7]
  printf "nextRomBase=%08X nextDivRomBase=%08X\n", (unsigned)MemESP::nextRomBase, (unsigned)MemESP::nextDivRomBase
  printf "palCtrl=%02X palIdx=%02X border=%d\n", NEXTVID::palCtrl, NEXTVID::palIdx, VIDEO::borderColor
  printf "rawPal0 ink0-7: %03X %03X %03X %03X %03X %03X %03X %03X\n", NEXTVID::rawPal[0][0], NEXTVID::rawPal[0][1], NEXTVID::rawPal[0][2], NEXTVID::rawPal[0][3], NEXTVID::rawPal[0][4], NEXTVID::rawPal[0][5], NEXTVID::rawPal[0][6], NEXTVID::rawPal[0][7]
  printf "rawPal0 pap16-23: %03X %03X %03X %03X %03X %03X %03X %03X\n", NEXTVID::rawPal[0][16], NEXTVID::rawPal[0][17], NEXTVID::rawPal[0][18], NEXTVID::rawPal[0][19], NEXTVID::rawPal[0][20], NEXTVID::rawPal[0][21], NEXTVID::rawPal[0][22], NEXTVID::rawPal[0][23]
  printf "lut0 ink0-7: %02X %02X %02X %02X %02X %02X %02X %02X\n", NEXTVID::lut[0][0], NEXTVID::lut[0][1], NEXTVID::lut[0][2], NEXTVID::lut[0][3], NEXTVID::lut[0][4], NEXTVID::lut[0][5], NEXTVID::lut[0][6], NEXTVID::lut[0][7]
  printf "lut0 pap16-23: %02X %02X %02X %02X %02X %02X %02X %02X\n", NEXTVID::lut[0][16], NEXTVID::lut[0][17], NEXTVID::lut[0][18], NEXTVID::lut[0][19], NEXTVID::lut[0][20], NEXTVID::lut[0][21], NEXTVID::lut[0][22], NEXTVID::lut[0][23]
  printf "lut4 pap16-23: %02X %02X %02X %02X %02X %02X %02X %02X\n", NEXTVID::lut[4][16], NEXTVID::lut[4][17], NEXTVID::lut[4][18], NEXTVID::lut[4][19], NEXTVID::lut[4][20], NEXTVID::lut[4][21], NEXTVID::lut[4][22], NEXTVID::lut[4][23]
  # Sprites: the renderer's patterns (16 KB), core0's attributes [128][5], the
  # renderer's attribute copy (SprRender.attr, first 640 bytes of sprR), the
  # sprite palettes (rawPal[2], [3]) and the log positions.
  if NEXTVID::sprPatterns != 0
    dump binary memory /tmp/picospec_sprpat.bin NEXTVID::sprPatterns (NEXTVID::sprPatterns + 16384)
  end
  if NEXTVID::sprAttr != 0
    dump binary memory /tmp/picospec_sprattr.bin (char*)NEXTVID::sprAttr ((char*)NEXTVID::sprAttr + 640)
  end
  if NEXTVID::sprR != 0
    dump binary memory /tmp/picospec_sprattr_r.bin (char*)NEXTVID::sprR ((char*)NEXTVID::sprR + 640)
  end
  if NEXTVID::rawPal != 0
    dump binary memory /tmp/picospec_rawpal.bin (char*)NEXTVID::rawPal ((char*)NEXTVID::rawPal + 4096)
  end
  # Copper: the 2 KB program (1024 big-endian instructions) + PC / control.
  if NEXTVID::copperMem != 0
    dump binary memory /tmp/picospec_copper.bin NEXTVID::copperMem (NEXTVID::copperMem + 2048)
    printf "copper: pc=%u index=%u ctrl=%02X\n", (unsigned)NEXTVID::copPC, (unsigned)NEXTVID::copIndex, (unsigned)NEXTVID::copCtrl
  end
  printf "spr: patWrite=%04X attrSlot=%02X nrSlot=%02X reg09=%02X reg43=%02X reg4B=%02X reg75..79: %02X %02X %02X %02X %02X\n", NEXTVID::sprPatWrite, NEXTVID::sprAttrSlot, NEXTVID::sprNrSlot, NextReg::reg[0x09], NextReg::reg[0x43], NextReg::reg[0x4B], NextReg::reg[0x75], NextReg::reg[0x76], NextReg::reg[0x77], NextReg::reg[0x78], NextReg::reg[0x79]
  set logging enabled off
else
  dump binary memory /tmp/picospec_mem0.bin MemESP::ramCurrent[0] (MemESP::ramCurrent[0] + 16384)
  dump binary memory /tmp/picospec_mem1.bin MemESP::ramCurrent[2] (MemESP::ramCurrent[2] + 16384)
  dump binary memory /tmp/picospec_mem2.bin MemESP::ramCurrent[4] (MemESP::ramCurrent[4] + 16384)
  dump binary memory /tmp/picospec_mem3.bin MemESP::ramCurrent[6] (MemESP::ramCurrent[6] + 16384)
end

# All 8 physical RAM pages (128K) via MemESP::ram[N]._int->p
# Note: pages backed by PSRAM_SPI or SWAP read whatever was last synced into the
# cache buffer at _int->p; non-cached PSRAM/SWAP regions cannot be dumped here.
dump binary memory /tmp/picospec_ram0.bin MemESP::ram[0]._int->p (MemESP::ram[0]._int->p + 16384)
dump binary memory /tmp/picospec_ram1.bin MemESP::ram[1]._int->p (MemESP::ram[1]._int->p + 16384)
dump binary memory /tmp/picospec_ram2.bin MemESP::ram[2]._int->p (MemESP::ram[2]._int->p + 16384)
dump binary memory /tmp/picospec_ram3.bin MemESP::ram[3]._int->p (MemESP::ram[3]._int->p + 16384)
dump binary memory /tmp/picospec_ram4.bin MemESP::ram[4]._int->p (MemESP::ram[4]._int->p + 16384)
dump binary memory /tmp/picospec_ram5.bin MemESP::ram[5]._int->p (MemESP::ram[5]._int->p + 16384)
dump binary memory /tmp/picospec_ram6.bin MemESP::ram[6]._int->p (MemESP::ram[6]._int->p + 16384)
dump binary memory /tmp/picospec_ram7.bin MemESP::ram[7]._int->p (MemESP::ram[7]._int->p + 16384)

set logging file /tmp/picospec_regs.txt
set logging overwrite on
set logging redirect on
set logging enabled on

printf "AF=%04X\n",  (unsigned short)((Z80::regA << 8) | (Z80::carryFlag ? Z80::sz5h3pnFlags | 1 : Z80::sz5h3pnFlags))
printf "BC=%04X\n",  (unsigned short)Z80::regBC.word
printf "DE=%04X\n",  (unsigned short)Z80::regDE.word
printf "HL=%04X\n",  (unsigned short)Z80::regHL.word
printf "AFx=%04X\n", (unsigned short)Z80::regAFx.word
printf "BCx=%04X\n", (unsigned short)Z80::regBCx.word
printf "DEx=%04X\n", (unsigned short)Z80::regDEx.word
printf "HLx=%04X\n", (unsigned short)Z80::regHLx.word
printf "IX=%04X\n",  (unsigned short)Z80::regIX.word
printf "IY=%04X\n",  (unsigned short)Z80::regIY.word
printf "SP=%04X\n",  (unsigned short)Z80::regSP.word
printf "PC=%04X\n",  (unsigned short)Z80::regPC.word
printf "I=%02X\n",   (unsigned char)Z80::regI
printf "R=%02X\n",   (unsigned char)Z80::regR
printf "IM=%d\n",    (int)Z80::modeINT
printf "IFF1=%d\n",  (int)Z80::ffIFF1
printf "IFF2=%d\n",  (int)Z80::ffIFF2
printf "halted=%d\n",(int)Z80::halted
printf "bankLatch=%d\n",  (int)MemESP::bankLatch
printf "romLatch=%d\n",   (int)MemESP::romLatch
printf "videoLatch=%d\n", (int)MemESP::videoLatch
printf "romInUse=%d\n",   (int)MemESP::romInUse
printf "pagingLock=%d\n", (int)MemESP::pagingLock
printf "page0ram=%d\n",   (int)MemESP::page0ram
printf "tstates=%u\n",    (unsigned int)CPU::tstates
printf "ram0_type=%d\n",  (int)MemESP::ram[0]._int->mem_type
printf "ram1_type=%d\n",  (int)MemESP::ram[1]._int->mem_type
printf "ram2_type=%d\n",  (int)MemESP::ram[2]._int->mem_type
printf "ram3_type=%d\n",  (int)MemESP::ram[3]._int->mem_type
printf "ram4_type=%d\n",  (int)MemESP::ram[4]._int->mem_type
printf "ram5_type=%d\n",  (int)MemESP::ram[5]._int->mem_type
printf "ram6_type=%d\n",  (int)MemESP::ram[6]._int->mem_type
printf "ram7_type=%d\n",  (int)MemESP::ram[7]._int->mem_type

set logging enabled off
set logging redirect off

# NeoGS card side. A two-CPU deadlock is undebuggable from the ZX half alone:
# it only ever shows "waiting on #BB", while the code the card is stuck in lives
# in card RAM (hw 2026-08-07, TheLink froze with the ZX in `IN A,(#BB)/RLCA/JR NC`
# and the card looping at 0x59C5, which is demo code uploaded into the card).
# s_ngs_low_ram is physical RAM pages 0+1 = the GS-Z80's 0x0000-0x3FFF (when
# NOROM is set) plus the fixed 0x4000-0x7FFF window at phys 0xC000-0xFFFF, and
# it is always pointer-backed, so a plain dump is exact. memdump.py re-splits it
# into the two CPU-address windows.
shell rm -f /tmp/picospec_ngs_low.bin /tmp/picospec_ngs.txt /tmp/picospec_ngs_b4.bin /tmp/picospec_ngs_b5.bin /tmp/picospec_ngs_b6.bin /tmp/picospec_ngs_b7.bin
if GS::neogs && GS::enabled
  if s_ngs_low_ram != 0
    dump binary memory /tmp/picospec_ngs_low.bin s_ngs_low_ram (s_ngs_low_ram + 65536)
  end
  set logging file /tmp/picospec_ngs.txt
  set logging overwrite on
  set logging redirect on
  set logging enabled on
  printf "PC=%04X\n", (unsigned short)s_cpu.pc.uint16_value
  printf "SP=%04X\n", (unsigned short)s_cpu.sp.uint16_value
  printf "AF=%04X\n", (unsigned short)s_cpu.af.uint16_value
  printf "BC=%04X\n", (unsigned short)s_cpu.bc.uint16_value
  printf "DE=%04X\n", (unsigned short)s_cpu.de.uint16_value
  printf "HL=%04X\n", (unsigned short)s_cpu.hl.uint16_value
  printf "IX=%04X\n", (unsigned short)s_cpu.ix_iy[0].uint16_value
  printf "IY=%04X\n", (unsigned short)s_cpu.ix_iy[1].uint16_value
  printf "GSCFG0=%02X\n", (unsigned char)s_ngs_cfg0
  printf "MPAG=%02X\n",   (unsigned char)s_ngs_mpag
  printf "MPAGEX=%02X\n", (unsigned char)s_ngs_mpagex
  printf "INTENA=%02X\n", (unsigned char)s_ngs_intena
  printf "INTREQ=%02X\n", (unsigned char)s_ngs_intreq
  printf "status=%02X\n", (unsigned char)GS::reg_status
  printf "command=%02X\n",(unsigned char)GS::reg_command
  printf "zxdma=%d\n",    (int)g_ngs_zxdma
  printf "dma_addr=%06X\n", (unsigned int)s_ngs_dma_pos
  printf "mpag_slots=%08X %08X %08X %08X\n", (unsigned)s_fetch_page[4], (unsigned)s_fetch_page[5], (unsigned)s_fetch_page[6], (unsigned)s_fetch_page[7]
  set logging enabled off
  set logging redirect off
  # Banked window 8000-FFFF as currently mapped. The demo's own command
  # handlers live here (the Q tags caught them popping commands at PCs that
  # exist nowhere in the 0000-7FFF dump), so without it half the card's code
  # is invisible. Pointer-backed on butter PSRAM; a null slot (SPI-PSRAM
  # boards) is simply skipped and memdump.py fills 0xFF.
  if s_fetch_page[4] != 0
    dump binary memory /tmp/picospec_ngs_b4.bin s_fetch_page[4] (s_fetch_page[4] + 8192)
  end
  if s_fetch_page[5] != 0
    dump binary memory /tmp/picospec_ngs_b5.bin s_fetch_page[5] (s_fetch_page[5] + 8192)
  end
  if s_fetch_page[6] != 0
    dump binary memory /tmp/picospec_ngs_b6.bin s_fetch_page[6] (s_fetch_page[6] + 8192)
  end
  if s_fetch_page[7] != 0
    dump binary memory /tmp/picospec_ngs_b7.bin s_fetch_page[7] (s_fetch_page[7] + 8192)
  end
end

echo \nMemory dump files written to /tmp/picospec_mem{0-3}.bin, /tmp/picospec_ram{0-7}.bin and /tmp/picospec_regs.txt\n

# TS-Conf: register file, the 16 ZX-bank CRAM cells and TS-BIOS's NVRAM config
# cells (#B0..#E7, CRC16 at #E6/#E7 — see RTC::tsBiosSeed). Last on purpose:
# these symbols exist only in builds that carry the machine.
shell rm -f /tmp/picospec_tsconf.txt /tmp/picospec_atm.txt /tmp/picospec_intring.bin /tmp/picospec_dmaring.bin /tmp/picospec_symprobe.txt /tmp/picospec_symprobe.gdb

# The INT-accept ring exists only in PERF_TRACE builds, and GDB's command
# language has no try/catch: an unknown symbol ABORTS the sourced file, and
# because these blocks run with `logging redirect on` the MI error record goes
# into the log file instead of the console — the VS Code extension's `evaluate`
# request then never gets its reply and the dump hangs on "Dumping via GDB..."
# for ever with the target still paused (hit 2026-09-10 on a PERF_TRACE=OFF
# build; the dump had already written every .bin and died on ts_int_late).
# So PROBE first: `info variables <re>` prints nothing and errors on nothing
# when there is no match, and an untaken `if` body is never evaluated.
set logging file /tmp/picospec_symprobe.txt
set logging overwrite on
set logging redirect on
set logging enabled on
info variables ^ts_int_ring$
info variables ^ts_dma_ring$
set logging enabled off
set logging redirect off
shell grep -v "regular expression" /tmp/picospec_symprobe.txt | grep -q ts_int_ring && echo 'set $has_intring = 1' > /tmp/picospec_symprobe.gdb || echo 'set $has_intring = 0' > /tmp/picospec_symprobe.gdb
shell grep -v "regular expression" /tmp/picospec_symprobe.txt | grep -q ts_dma_ring && echo 'set $has_dmaring = 1' >> /tmp/picospec_symprobe.gdb || echo 'set $has_dmaring = 0' >> /tmp/picospec_symprobe.gdb
source /tmp/picospec_symprobe.gdb

if Z80Ops::isTsconf
  set logging file /tmp/picospec_tsconf.txt
  set logging overwrite on
  set logging redirect on
  set logging enabled on
  printf "memconf=%02X p7ffd=%02X page0=%02X page1=%02X page2=%02X page3=%02X trdos=%d\n", (unsigned char)TsConf::r.memconf, (unsigned char)TsConf::r.p7ffd, (unsigned char)TsConf::r.page[0], (unsigned char)TsConf::r.page[1], (unsigned char)TsConf::r.page[2], (unsigned char)TsConf::r.page[3], (int)ESPectrum::trdos
  printf "sysconf=%02X intmask=%02X hsint=%02X vsint=%03X pwr_up=%02X fmaddr=%02X fddvirt=%02X vdos=%d\n", (unsigned char)TsConf::r.sysconf, (unsigned char)TsConf::r.intmask, (unsigned char)TsConf::r.hsint, (unsigned)TsConf::r.vsint, (unsigned char)TsConf::r.pwr_up, (unsigned char)TsConf::r.fmaddr, (unsigned char)TsConf::r.fddvirt, (int)TsConf::vdosLive
  printf "vconf=%02X vpage=%02X tsconf=%02X palsel=%02X border=%02X gxoffs=%03X gyoffs=%03X\n", (unsigned char)TsConf::r.vconf, (unsigned char)TsConf::r.vpage, (unsigned char)TsConf::r.tsconf, (unsigned char)TsConf::r.palsel, (unsigned char)TsConf::r.border, (unsigned)TsConf::r.g_xoffs, (unsigned)TsConf::r.g_yoffs
  printf "dma: saddr=%06X daddr=%06X len=%02X num=%02X ctrl=%02X\n", (unsigned)TsConf::r.saddr, (unsigned)TsConf::r.daddr, (unsigned char)TsConf::r.dmalen, (unsigned char)TsConf::r.dmanum, (unsigned char)TsConf::r.dmactrl
  printf "video: render=%d vmode=%d tsu=%d pal256=%d rres=%d crop=%d ds80_drv=%d ds80_gfx=%d lin_end=%d..%d tmpage=%02X t0g=%02X t1g=%02X sg=%02X t0=%03X,%03X t1=%03X,%03X\n", (int)VIDEO::ts_render_live, (int)VIDEO::ts_vmode_live, (int)VIDEO::ts_tsu_live, (int)VIDEO::ts_pal256_live, (int)VIDEO::ts_rres_live, (int)VIDEO::ts_crop_top, (int)profi_ds80_active, (int)Graphics8BitPalette::ds80_active, (int)'Video.cpp'::lin_end, (int)'Video.cpp'::lin_end2, (unsigned char)TsConf::r.tmpage, (unsigned char)TsConf::r.t0gpage, (unsigned char)TsConf::r.t1gpage, (unsigned char)TsConf::r.sgpage, (unsigned)TsConf::r.t0_xoffs, (unsigned)TsConf::r.t0_yoffs, (unsigned)TsConf::r.t1_xoffs, (unsigned)TsConf::r.t1_yoffs
  printf "gigascreen: cfg=%d live=%d crt=%d\n", (int)Config::gigascreen_enabled, (int)VIDEO::gigascreen_enabled, (int)Config::crt_filter
  set $i = 0
  while $i < 256
    printf "cram[%02X]=%04X\n", $i, (unsigned)TsConf::cram[$i]
    set $i = $i + 1
  end
  set $i = 0xB0
  while $i < 0xE8
    printf "nv[%02X]=%02X\n", $i, (unsigned char)RTC::regs[$i]
    set $i = $i + 1
  end
  # INT-accept ring (PERF_TRACE builds only — see the probe above): the last
  # TS_INT_RING_N interrupts TAKEN, oldest first. pc/sp = the interrupted
  # program (the frame is pushed at sp-2..sp-1 and below), t = frame T-state
  # (line = t/224), lat = T-states after the window opened (bit 7 = woke from
  # HALT), src FF FRAME / FD LINE / FB DMA. It goes out as ONE binary transfer
  # instead of a printf per entry: each printf is a separate target read over
  # OpenOCD and that is what made a dump of the old 64-entry ring slow (and
  # twice appear to hang). Decode with tools/intring.py, which reads the header
  # line below for w/n. LAST in the block so that a build whose ring layout
  # ever changes cannot cost the register/CRAM/NVRAM dump above it.
  if $has_intring
    printf "intring: late=%u miss=%u frozen=%u w=%u n=%u\n", (unsigned)ts_int_late, (unsigned)ts_int_miss, (unsigned)ts_int_frozen, (unsigned)ts_int_ring_w, (unsigned)(sizeof(ts_int_ring)/sizeof(ts_int_ring[0]))
  end
  # DMA event ring (TS_VIDEO_TRACE builds): every DMACtrl write with its raster
  # position, modelled duration and the renderer's state. Decode with
  # tools/dmaring.py. Probed like the INT ring; last, for the same reason.
  if $has_dmaring
    printf "dmaring: w=%u n=%u\n", (unsigned)ts_dma_ring_w, (unsigned)(sizeof(ts_dma_ring)/sizeof(ts_dma_ring[0]))
  end
  set logging enabled off
  set logging redirect off
  if $has_intring
    dump binary memory /tmp/picospec_intring.bin &ts_int_ring[0] (&ts_int_ring[0] + sizeof(ts_int_ring)/sizeof(ts_int_ring[0]))
  else
    echo \nintring: not in this build (PERF_TRACE=OFF)\n
  end
  if $has_dmaring
    dump binary memory /tmp/picospec_dmaring.bin &ts_dma_ring[0] (&ts_dma_ring[0] + sizeof(ts_dma_ring)/sizeof(ts_dma_ring[0]))
  end
end

# Timex TC2068 SCLD. Everything about this machine goes through two registers and
# eight pointers, and the ROM's own bank switcher READS BOTH BACK to build the next
# value (EX-ROM 0x64BE-0x64F4: IN A,(#F4) / CPL / OR E / CPL / OUT (#F4),A), so a
# wrong read-back sends the machine into the wrong bank and the ZX half only ever
# says "PC is somewhere odd". rd[i]: 0 = HOME, 2 = open bus (nothing in that DOCK
# chunk), anything else = the 8 KB block serving that slot. Placed after the blocks
# above for the same reason the TS-Conf one is: an unresolved symbol here must not
# cost the main dump.
if Z80Ops::isTc2068
  set logging file /tmp/picospec_timex.txt
  set logging overwrite on
  set logging redirect on
  set logging enabled on
  printf "== Timex TC2068 ==\n"
  printf "hsr(#F4)=%02X  dec(#FF)=%02X  exromSel=%d  int_inhibit=%d  mmu=%d\n", (unsigned char)Timex::hsr, (unsigned char)VIDEO::timex_port_ff, (int)Timex::exromSel, (int)VIDEO::timex_int_inhibit, (unsigned char)g_timex_mmu
  printf "timex_mode=%d  hires_ink=%d  ay_reg=%d\n", (unsigned char)VIDEO::timex_mode, (unsigned char)VIDEO::timex_hires_ink, (unsigned char)Timex::ayReg
  # The main dump walks MemESP::ramCurrent[] and knows nothing about the SCLD, so
  # while any slot is windowed the logical dump shows the HOME bank there and a PC
  # inside the window decodes as unrelated HOME-ROM bytes. Say so, loudly: reading
  # it the other way costs an analysis round (hw 2026-09-12 — PC=0x0194 looked like
  # the BASIC keyword table and was really LD-SAMPLE in the EX-ROM).
  if Timex::hsr != 0
    printf "WARNING: the logical dump above shows HOME for every slot; the slots\n"
    printf "         marked rd!=0 below are really %s — add 0x045A to a Sinclair\n", Timex::exromSel ? "the EX-ROM" : "the DOCK"
    printf "         ROM address to find its EX-ROM twin.\n"
  end
  set $i = 0
  while $i < 8
    printf "slot%d %04X-%04X  rd=%p  wr=%p\n", $i, $i*0x2000, $i*0x2000+0x1FFF, Timex::rd[$i], Timex::wr[$i]
    set $i = $i + 1
  end
  set logging enabled off
  set logging redirect off
end

# Enterprise 64/128: the main dump walks the four windows as Dave pages them, but
# says nothing about WHY the screen is what it is — Nick's LPT walk (a stuck LPT
# never reaches its VSYNC block and the renderer then has no row to draw into:
# s_row runs to 1000 and stays), Dave's interrupt latches, the line clock. The LPT
# itself goes to /tmp/picospec_ep_lpt.bin (512 B from lptBase in VRAM) — 16 bytes
# a block: 256-lines, mode byte (b0 reload, b1-3 video mode 0 = VSYNC, b7 IRQ),
# margins, LD1, LD2, 8 palette bytes. After the TS-Conf/Timex blocks for the same
# reason: a missing symbol here must not lose the main dump.
if Z80Ops::isEnt
  set logging file /tmp/picospec_ep.txt
  set logging overwrite on
  set logging redirect on
  set logging enabled on
  printf "== Enterprise ==\n"
  printf "Dave pages %02X %02X %02X %02X  kbRow=%d sysconf=%02X  en snd/hz/1/2=%d%d%d%d act=%d%d%d%d st1=%d clk1000=%d\n", (unsigned char)Ep::s_dave.page[0], (unsigned char)Ep::s_dave.page[1], (unsigned char)Ep::s_dave.page[2], (unsigned char)Ep::s_dave.page[3], (int)Ep::s_dave.kbRow, (unsigned char)Ep::s_dave.sysconf, (int)Ep::s_dave.enSnd, (int)Ep::s_dave.enHz, (int)Ep::s_dave.en1, (int)Ep::s_dave.en2, (int)Ep::s_dave.actSnd, (int)Ep::s_dave.actHz, (int)Ep::s_dave.act1, (int)Ep::s_dave.act2, (int)Ep::s_dave.st1_, (int)Ep::s_dave.clk1000
  printf "Nick lptBase=%04X lptCur=%04X port0=%02X border=%02X port3=%02X lptFlags=%02X reload=%d vsync=%d dispEn=%d videoMode=%d nLines=%d linesRemaining=%d irqFlag=%d\n", (unsigned)Ep::s_nick.lptBase, (unsigned)Ep::s_nick.lptCur, (unsigned char)Ep::s_nick.port0, (unsigned char)Ep::s_nick.border, (unsigned char)Ep::s_nick.port3, (unsigned char)Ep::s_nick.lptFlags, (int)Ep::s_nick.reload, (int)Ep::s_nick.vsync, (int)Ep::s_nick.displayEnabled, (int)Ep::s_nick.videoMode, (int)Ep::s_nick.nLines, (int)Ep::s_nick.linesRemaining, (int)Ep::s_nick.irqFlag
  printf "glue lineT=%u frac57=%u tstates=%u s_row=%d prevVs=%d daveT=%u hiSeen=%d pair=%d ps=%p injOn=%d\n", (unsigned)Ep::lineT, (unsigned)Ep::frac57, (unsigned)CPU::tstates, (int)Ep::s_row, (int)Ep::s_prevVs, (unsigned)Ep::s_daveT, (int)Ep::s_hiSeen, (int)VIDEO::sam_pair_live, Ep::s_ps, (int)Ep::injOn
  printf "vram %p %p %p %p\n", Ep::s_nick.vram[0], Ep::s_nick.vram[1], Ep::s_nick.vram[2], Ep::s_nick.vram[3]
  printf "EXDOS cmd=%02X st=%02X trk=%u sec=%u data=%02X irq=%d drq=%d ncmd=%u  A: tracks=%u cur=%u side=%u motor=%d bufLba=%d dirty=%d  B: tracks=%u cur=%u motor=%d  sel=%s\n", (unsigned char)EpFdc::s_cmd, (unsigned char)EpFdc::s_status, (unsigned)EpFdc::s_track, (unsigned)EpFdc::s_sector, (unsigned char)EpFdc::s_data, (int)EpFdc::s_irq, (int)EpFdc::s_drq, (unsigned)EpFdc::s_ncmd, (unsigned)EpFdc::s_drv[0].tracks, (unsigned)EpFdc::s_drv[0].curTrack, (unsigned)EpFdc::s_drv[0].side, (int)EpFdc::s_drv[0].motor, (int)EpFdc::s_drv[0].bufLba, (int)EpFdc::s_drv[0].dirty, (unsigned)EpFdc::s_drv[1].tracks, (unsigned)EpFdc::s_drv[1].curTrack, (int)EpFdc::s_drv[1].motor, EpFdc::s_cur == &EpFdc::s_drv[0] ? "A" : EpFdc::s_cur == &EpFdc::s_drv[1] ? "B" : "-"
  set logging enabled off
  set logging redirect off
  set $eplpt = Ep::s_nick.vram[(Ep::s_nick.lptBase >> 14)] + (Ep::s_nick.lptBase & 0x3FFF)
  dump binary memory /tmp/picospec_ep_lpt.bin $eplpt ($eplpt + 512)
end

# ── cores (ARM) ─────────────────────────────────────────────────────────────

# ATM-Turbo: CP/M and Beta-128 share the DOS/FDC signal, but the generic dump
# does not record the ATM latches that produce it. Keep this after every other
# block so a missing machine-specific symbol cannot lose the main dump.
if Z80Ops::isAtm
  set logging file /tmp/picospec_atm.txt
  set logging overwrite on
  set logging redirect on
  set logging enabled on
  printf "== ATM-Turbo ==\n"
  printf "atm1=%d aFE=%02X aFB=%02X pFDFD=%02X p7ffd=%02X beta=%d cpm=%d trdos=%d ro=%02X\n", (int)Atm::atm1, (unsigned char)Atm::aFE, (unsigned char)Atm::aFB, (unsigned char)Atm::pFDFD, (unsigned char)Atm::p7ffd, (int)Atm::beta, (int)(Atm::atm1 ? !(Atm::aFE & 0x80) : !(Atm::a77 & 0x200)), (int)ESPectrum::trdos, (unsigned char)g_memgates.b.atm_ro
  printf "slots=%08X %08X %08X %08X\n", (unsigned)MemESP::ramCurrent[0], (unsigned)MemESP::ramCurrent[2], (unsigned)MemESP::ramCurrent[4], (unsigned)MemESP::ramCurrent[6]
  printf "p77=%02X a77=%04X pBF=%02X shaden=%d atm3=%d videoLatch=%d gmx_ext_live=%d pend_on=%d pend_off=%d ds80=%d mult=%d\n", (unsigned char)Atm::p77, (unsigned short)Atm::a77, (unsigned char)Atm::pBF, (int)Atm::shaden, (int)Atm::atm3, (int)MemESP::videoLatch, (int)VIDEO::gmx_ext_live, (int)VIDEO::gmx_ext_pending_on, (int)VIDEO::gmx_ext_pending_off, (int)profi_ds80_active, (int)ESPectrum::multiplicator
  printf "pF7=%03X %03X %03X %03X | %03X %03X %03X %03X\n", (unsigned)Atm::pF7[0], (unsigned)Atm::pF7[1], (unsigned)Atm::pF7[2], (unsigned)Atm::pF7[3], (unsigned)Atm::pF7[4], (unsigned)Atm::pF7[5], (unsigned)Atm::pF7[6], (unsigned)Atm::pF7[7]
  printf "pal=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X border=%02X\n", (unsigned char)Atm::pal[0], (unsigned char)Atm::pal[1], (unsigned char)Atm::pal[2], (unsigned char)Atm::pal[3], (unsigned char)Atm::pal[4], (unsigned char)Atm::pal[5], (unsigned char)Atm::pal[6], (unsigned char)Atm::pal[7], (unsigned char)Atm::pal[8], (unsigned char)Atm::pal[9], (unsigned char)Atm::pal[10], (unsigned char)Atm::pal[11], (unsigned char)Atm::pal[12], (unsigned char)Atm::pal[13], (unsigned char)Atm::pal[14], (unsigned char)Atm::pal[15], (unsigned char)VIDEO::borderColor
  printf "FDC: disk=%d state=%u step=%u cmd=%02X trk=%u sec=%u data=%02X status=%04X ctrl=%05X\n", (int)ESPectrum::fdd.diskS, (unsigned)ESPectrum::fdd.state, (unsigned)ESPectrum::fdd.stepState, (unsigned char)ESPectrum::fdd.command, (unsigned char)ESPectrum::fdd.track, (unsigned char)ESPectrum::fdd.sector, (unsigned char)ESPectrum::fdd.data, (unsigned short)ESPectrum::fdd.status, (unsigned)ESPectrum::fdd.control
  printf "disk_present=%d\n", (int)(ESPectrum::fdd.disk[ESPectrum::fdd.diskS] != 0)
  if Atm::evo
    printf "evo: eff7=%02X fddMask=%X vgSys=%02X inNmi=%d nmiClr=%d inTrdemu=%d fnt=%d\n", (unsigned char)EvoBase::pEFF7, (unsigned char)EvoBase::fddMask, (unsigned char)EvoBase::vgSys, (int)EvoBase::inNmi, (int)EvoBase::nmiClrPending, (int)EvoBase::inTrdemu, (int)g_atm_fnt
  end
  set logging enabled off
  set logging redirect off
end

# ── ZX Spectrum Next state (appended LAST — the rule of this file: an
# unresolved symbol must not lose the main dump). NextReg register file +
# MMU/paging/divMMC-automap/TBBLUE-window snapshot: a Browser or .tap hang
# with only the main dump reads as "PC somewhere in ROM"; this names the
# mapping that put it there. arch enum: 11 == A_NEXT (ArchRom.h X-macro).
shell rm -f /tmp/picospec_nextregs.bin /tmp/picospec_next.txt
if Config::arch == 11
  dump binary memory /tmp/picospec_nextregs.bin NextReg::reg (NextReg::reg + 256)
  set logging file /tmp/picospec_next.txt
  set logging overwrite on
  set logging redirect on
  set logging enabled on
  printf "mmu=%02X %02X %02X %02X %02X %02X %02X %02X\n", MemESP::mmu[0], MemESP::mmu[1], MemESP::mmu[2], MemESP::mmu[3], MemESP::mmu[4], MemESP::mmu[5], MemESP::mmu[6], MemESP::mmu[7]
  printf "romInUse=%d romLatch=%d rom_mask=%02X altrom_rd=%d altrom_wr=%d altrom_sel=%d\n", (int)MemESP::romInUse, (int)MemESP::romLatch, (unsigned char)MemESP::next_rom_mask, (int)MemESP::altrom_rd, (int)MemESP::altrom_wr, (int)MemESP::altrom_sel
  printf "port7FFD=%02X port1FFD=%02X portDFFD=%02X port123B=%02X portFF=%02X\n", NextReg::port7FFD, NextReg::port1FFD, NextReg::portDFFD, NextReg::port123B, NextReg::portFF
  printf "bootrom=%d configMode=%d configPage=%02X\n", (int)MemESP::nextBootRomActive, (int)MemESP::nextConfigMode, MemESP::nextConfigPage
  printf "divmmc: automap=%d conmem=%d mapram=%d bank=%d mapped=%d armed=%d\n", (int)DivMMC::automap, (int)DivMMC::conmem, (int)DivMMC::mapram, (int)DivMMC::bank, (int)MemESP::divmmc_mapped, (int)g_next_automap_armed
  printf "wr_overlay_mask=%02X multiplicator=%d\n", (unsigned char)MemESP::wr_overlay_mask, (int)ESPectrum::multiplicator
  printf "slots: %08X %08X %08X %08X %08X %08X %08X %08X\n", (unsigned)MemESP::ramCurrent[0], (unsigned)MemESP::ramCurrent[1], (unsigned)MemESP::ramCurrent[2], (unsigned)MemESP::ramCurrent[3], (unsigned)MemESP::ramCurrent[4], (unsigned)MemESP::ramCurrent[5], (unsigned)MemESP::ramCurrent[6], (unsigned)MemESP::ramCurrent[7]
  printf "wrOverlay: %08X %08X %08X %08X %08X %08X l2_rd_mask=%02X l2_wr_mmu_mask=%02X\n", (unsigned)MemESP::wrOverlay[0], (unsigned)MemESP::wrOverlay[1], (unsigned)MemESP::wrOverlay[2], (unsigned)MemESP::wrOverlay[3], (unsigned)MemESP::wrOverlay[4], (unsigned)MemESP::wrOverlay[5], (unsigned char)MemESP::l2_rd_mask, (unsigned char)MemESP::l2_wr_mmu_mask
  printf "reg70=%02X reg71=%02X reg16=%02X reg17=%02X reg69=%02X reg6B=%02X reg6C=%02X reg6E=%02X reg6F=%02X reg4C=%02X\n", NextReg::reg[0x70], NextReg::reg[0x71], NextReg::reg[0x16], NextReg::reg[0x17], NextReg::reg[0x69], NextReg::reg[0x6B], NextReg::reg[0x6C], NextReg::reg[0x6E], NextReg::reg[0x6F], NextReg::reg[0x4C]
  # Live clip windows (NR 0x18-0x1B x 4 coordinates + the per-window write
  # index, NR 0x1C).  The register grid above only shows the LAST byte written
  # to each of 0x18-0x1B, which says nothing: a window is four writes through
  # its own register and the index decides which coordinate each one lands in.
  # window 0=Layer2 1=sprites 2=ULA 3=tilemap; x1,x2 are half-columns
  # (2 px), y1,y2 are LINES.
  printf "clip L2 %02X %02X %02X %02X | spr %02X %02X %02X %02X\n", NEXTVID::clip[0][0], NEXTVID::clip[0][1], NEXTVID::clip[0][2], NEXTVID::clip[0][3], NEXTVID::clip[1][0], NEXTVID::clip[1][1], NEXTVID::clip[1][2], NEXTVID::clip[1][3]
  printf "clip ula %02X %02X %02X %02X | tm %02X %02X %02X %02X | idx %d %d %d %d\n", NEXTVID::clip[2][0], NEXTVID::clip[2][1], NEXTVID::clip[2][2], NEXTVID::clip[2][3], NEXTVID::clip[3][0], NEXTVID::clip[3][1], NEXTVID::clip[3][2], NEXTVID::clip[3][3], (int)NEXTVID::clipIdx[0], (int)NEXTVID::clipIdx[1], (int)NEXTVID::clipIdx[2], (int)NEXTVID::clipIdx[3]
  printf "dma: dir=%d A=%04X inc%d io%d B=%04X inc%d io%d len=%04X cnt=%04X curA=%04X curB=%04X\n", (int)Z80DMA::transfer_dir, Z80DMA::port_a_addr, (int)Z80DMA::port_a_inc, (int)Z80DMA::port_a_is_io, Z80DMA::port_b_addr, (int)Z80DMA::port_b_inc, (int)Z80DMA::port_b_is_io, Z80DMA::block_length, Z80DMA::nx_counter, Z80DMA::cur_port_a, Z80DMA::cur_port_b
  set logging enabled off
  set logging redirect off
  # Both Layer 2 buffers (NR 0x12 and NR 0x13, 10 x 8K each: 320x256 and
  # 640x256 take 80K, 256x192 the first 6), resolved the way
  # nextPgPtr does (hot-page pool first), plus the pool's page table.
  shell rm -f /tmp/picospec_l2a_*.bin /tmp/picospec_l2b_*.bin
  set $nxk = 0
  while $nxk < 20
    set $nxbank = ($nxk < 10) ? (NextReg::reg[0x12] & 0x7F) : (NextReg::reg[0x13] & 0x7F)
    set $nxpg = ($nxbank * 2 + ($nxk % 10)) & 0xFF
    set $nxf = 255
    if MemESP::nextHotIdx != 0
      set $nxf = MemESP::nextHotIdx[$nxpg]
    end
    if $nxf != 255
      set $nxp = (unsigned long)MemESP::nextHotFrame[$nxf]
    else
      if $nxpg < 16
        set $nxp = (unsigned long)MemESP::nextPg[$nxpg]
      else
        set $nxp = (unsigned long)MemESP::nextStrip + ($nxpg - 16) * 8192
      end
    end
    if $nxk < 10
      eval "dump binary memory /tmp/picospec_l2a_%d.bin %lu %lu", $nxk, $nxp, $nxp + 8192
    else
      eval "dump binary memory /tmp/picospec_l2b_%d.bin %lu %lu", $nxk - 10, $nxp, $nxp + 8192
    end
    set $nxk = $nxk + 1
  end
  if MemESP::nextHotIdx != 0
    dump binary memory /tmp/picospec_hotidx.bin MemESP::nextHotIdx (MemESP::nextHotIdx + 224)
    # The SRAM frames themselves (frame f holds the page whose hotidx entry is f):
    # a hot page's home copy in PSRAM is stale until eviction.
    shell rm -f /tmp/picospec_hotf_*.bin
    set $nxf = 0
    while $nxf < 8
      if MemESP::nextHotFrame[$nxf] != 0
        set $nxp = (unsigned long)MemESP::nextHotFrame[$nxf]
        eval "dump binary memory /tmp/picospec_hotf_%d.bin %lu %lu", $nxf, $nxp, $nxp + 8192
      end
      set $nxf = $nxf + 1
    end
  end
end

# Where the selected ARM core is: PC / LR / SP only — no backtrace and NO thread
# switch: `thread apply all` (bt or even a plain printf) selects the other core's
# frame, and with a PC in .nxovl that alone hung GDB (2026-10-11, twice).
# `thread apply all bt` hung the whole dump on a Next session (2026-10-11): core0
# was parked in c1Drain inside .nxovl, a window that shares its addresses with
# .tsovl/.ftovl, GDB unwound it through TS-Conf/FT812 symbols into garbage and
# never came back, so nothing after this point ran. Resolve PCs by hand against
# the overlay that is actually loaded. Kept LAST, after every other block.
set logging file /tmp/picospec_cores.txt
set logging overwrite on
set logging redirect on
set logging enabled on
printf "== cores (ARM) ==\n"
printf "selected core: pc=%08X lr=%08X sp=%08X\n", $pc, $lr, $sp
set logging enabled off
set logging redirect off

# Last line: tell the VS Code extension the dump is complete.
shell touch /tmp/picospec_gdb_done
