///////////////////////////////////////////////////////////////////////////////
//
// z80cpp - Z80 emulator core
//
// Copyright (c) 2017, 2018, 2019, 2020 jsanchezv - https://github.com/jsanchezv
//
// Heretic optimizations and minor adaptations
// Copyright (c) 2021 dcrespo3d - https://github.com/dcrespo3d
//
// ESPectrum specific optimizations and code
// Copyright (c) 2023-24 Víctor Iborra [Eremus] - https://github.com/EremusOne
//

// Converted to C++ from Java at
//... https://github.com/jsanchezv/Z80Core
//... commit c4f267e3564fa89bd88fd2d1d322f4d6b0069dbd
//... GPL 3
//... v1.0.0 (13/02/2017)
//    quick & dirty conversion by dddddd (AKA deesix)

#include <stdio.h>
#include <algorithm>

#include "z80.h"
#include "speccy/core/Ports.h"
#include "speccy/video/Video.h"
#include "speccy/machines/TsConf/TsFastMem.h"
#include <string.h>
#include "drivers/graphics/graphics.h"
#include "speccy/core/MemESP.h"
#include "CPU.h"
#include "app/Col16Trace.h"
#include "speccy/machines/Atm.h"
#include "speccy/machines/Sam/Sam.h"
#include "speccy/machines/EvoBase.h"
#include "speccy/devices/tape/Tape.h"
#include "app/Config.h"
#include "fs/FileUtils.h"
#include "ui/OSDMain.h"
#include "app/messages.h"
#include "app/Debug.h"
#include "app/ESPectrum.h"
#include "speccy/devices/disk/wd1793.h"

#include "speccy/devices/storage/DivMMC.h"
#include "speccy/devices/Z80DMA.h"
#include "speccy/devices/disk/MB02.h"
#include "speccy/machines/Timex.h"   // g_timex_mmu + Timex::rd/read8 (TC2068 SCLD window, exec_nocheck fetch)
#include "speccy/machines/TsConf/TsConf.h"
#include "speccy/core/Rzx.h"     // Rzx::mode / onIn: RZX playback substitutes every IN
#include "speccy/machines/Next/NextReg.h"
#include "ui/LEDIndicators.h"     // nextOtirBatch: the DMA indicator
#include "app/CodeOverlay.h"   // NX_OVL_CODE
#include "speccy/machines/Next/NextIdle.h"
#include "speccy/machines/Next/NextVideo.h"
#include "speccy/machines/Next/NextInt.h"
#include "speccy/machines/Next/NextMf.h"
#include "speccy/z80/next_memory.h"
#include "speccy/z80/next_opcode.h"


// #include "speccy/core/Snapshot.h"

// #pragma GCC optimize("O3")

uint8_t page;

#define PEEK8(result,address) \
 page = address >> 13; \
 VIDEO::Draw(3,MemESP::ramContended[page]); \
 if (page <= 1 && MemESP::divmmc_mapped) \
     result = (page == 0) ? MemESP::page0_lo[address] : MemESP::page0_hi[(address) & 0x1FFF]; \
 else \
     result = MemESP::romPeek(page, MemESP::ramCurrent[page], address);

// miembros estáticos

uint8_t Z80::opCode;
uint8_t Z80::prefixOpcode = 0;
bool Z80::execDone;
uint8_t Z80::regA;
uint8_t Z80::sz5h3pnFlags;
bool Z80::carryFlag;
RegisterPair Z80::regBC, Z80::regBCx, Z80::regDE, Z80::regDEx, Z80::regHL, Z80::regHLx;
bool Z80::flagQ;
bool Z80::lastFlagQ;
RegisterPair Z80::regAFx;
RegisterPair Z80::regPC;
RegisterPair Z80::regIX;
RegisterPair Z80::regIY;
RegisterPair Z80::regSP;
uint8_t Z80::regI;
uint32_t Z80::regR;
uint8_t Z80::regRbase;
uint32_t Z80::ldairAt;
bool Z80::regRbit7;
bool Z80::ffIFF1 = false;
bool Z80::ffIFF2 = false;
bool Z80::pendingEI = false;
#if PERF_TRACE
uint32_t g_frm_int_taken = 0, g_frm_int_miss = 0, g_int_last_t = 0;   // see the [PERF] 60f intMiss= field
#endif
bool Z80::activeNMI = false;
bool Z80::activeNMIDOS = false;
bool Z80::nmiDosInProgress = false;
uint8_t Z80::nmiDos_savedRomInUse = 0;
bool Z80::nmiDos_savedTrdos = false;
uint16_t Z80::nmiDos_savedSP = 0;
uint16_t Z80::nmiDos_savedPC = 0;
Z80::IntMode Z80::modeINT = Z80::IntMode::IM0;
bool Z80::halted = false;
bool Z80::pinReset = false;
RegisterPair Z80::memptr;
uint8_t Z80::sz53n_addTable[256];
uint8_t Z80::sz53pn_addTable[256];
uint8_t Z80::sz53n_subTable[256];
uint8_t Z80::sz53pn_subTable[256];

///////////////////////////////////////////////////////////////////////////////

// Constructor de la clase
// Cold half of the core: runs at boot, on a machine reset or when an NMI is
// taken — never per instruction — so it lives in FLASH even with the core in
// SRAM (Z80_CORE_IN_RAM). The linker script sends `.z80cold` to the flash .text
// ahead of the object-wide RAM rule (rp2350-memmap.ld). Z80::interrupt() is NOT
// cold: TS-Conf's LINE INT takes it up to 320 times a frame. (2026-09-22, -824 B)
#define Z80_COLD __attribute__((section(".z80cold"), noinline))
// Base-opcode handlers that Z80::exec_nocheck_next (the Next loop) calls
// directly from its opcode switch (src/speccy/z80/next_dispatch.inc, generated
// by tools/gen_next_dispatch.py): inlined there, while dcOpcode[] keeps the
// out-of-line copy every other loop calls.
#define NX_DISP_INL __attribute__((always_inline)) inline
#if NEXT_TRACE
void nxJumpNote(char t);
#endif
// The INT is accepted at this boundary only with IFF1 set and no EI pending.
Z80_COLD void Z80::rzxLdAirIntBug() {
    if (ldairAt == regR + 1 && ffIFF1 && !pendingEI) sz5h3pnFlags &= ~PARITY_MASK;
}

Z80_COLD void Z80::create() {

    bool evenBits;

    for (uint32_t idx = 0; idx < 256; idx++) {
		sz53n_addTable[idx] = 0;
		sz53pn_addTable[idx] = 0;
		sz53n_subTable[idx] = 0;
		sz53pn_subTable[idx] = 0;

		if (idx > 0x7f) {
            sz53n_addTable[idx] |= SIGN_MASK;
        }

        evenBits = true;
        for (uint8_t mask = 0x01; mask != 0; mask <<= 1) {
            if ((idx & mask) != 0) {
                evenBits = !evenBits;
            }
        }

        sz53n_addTable[idx] |= (idx & FLAG_53_MASK);
        sz53n_subTable[idx] = sz53n_addTable[idx] | ADDSUB_MASK;

        if (evenBits) {
            sz53pn_addTable[idx] = sz53n_addTable[idx] | PARITY_MASK;
            sz53pn_subTable[idx] = sz53n_subTable[idx] | PARITY_MASK;
        } else {
            sz53pn_addTable[idx] = sz53n_addTable[idx];
            sz53pn_subTable[idx] = sz53n_subTable[idx];
        }
    }

    sz53n_addTable[0] |= ZERO_MASK;
    sz53pn_addTable[0] |= ZERO_MASK;
    sz53n_subTable[0] |= ZERO_MASK;
    sz53pn_subTable[0] |= ZERO_MASK;

    execDone = false;

    // reset();

}

Z80_COLD void Z80::destroy(void)
{
}

RegisterPair Z80::getPairIR(void) {
    RegisterPair IR;
    IR.byte8.hi = regI;
    IR.byte8.lo = (uint8_t)(regR + regRbase) & 0x7f;
    if (regRbit7) {
        IR.byte8.lo |= SIGN_MASK;
    }
    return IR;
}

void Z80::setAddSubFlag(bool state) {
    if (state) {
        sz5h3pnFlags |= ADDSUB_MASK;
    } else {
        sz5h3pnFlags &= ~ADDSUB_MASK;
    }
}

void Z80::setParOverFlag(bool state) {
    if (state) {
        sz5h3pnFlags |= PARITY_MASK;
    } else {
        sz5h3pnFlags &= ~PARITY_MASK;
    }
}

void Z80::setBit3Fag(bool state) {
    if (state) {
        sz5h3pnFlags |= BIT3_MASK;
    } else {
        sz5h3pnFlags &= ~BIT3_MASK;
    }
}

void Z80::setHalfCarryFlag(bool state) {
    if (state) {
        sz5h3pnFlags |= HALFCARRY_MASK;
    } else {
        sz5h3pnFlags &= ~HALFCARRY_MASK;
    }
}

void Z80::setBit5Flag(bool state) {
    if (state) {
        sz5h3pnFlags |= BIT5_MASK;
    } else {
        sz5h3pnFlags &= ~BIT5_MASK;
    }
}

void Z80::setZeroFlag(bool state) {
    if (state) {
        sz5h3pnFlags |= ZERO_MASK;
    } else {
        sz5h3pnFlags &= ~ZERO_MASK;
    }
}

void Z80::setSignFlag(bool state) {
    if (state) {
        sz5h3pnFlags |= SIGN_MASK;
    } else {
        sz5h3pnFlags &= ~SIGN_MASK;
    }
}

// Reset
/* Según el documento de Sean Young, que se encuentra en
 * [http://www.myquest.com/z80undocumented], la mejor manera de emular el
 * reset es poniendo PC, IFF1, IFF2, R e IM0 a 0 y todos los demás registros
 * a 0xFFFF.
 *
 * 29/05/2011: cuando la CPU recibe alimentación por primera vez, los
 *             registros PC e IR se inicializan a cero y el resto a 0xFF.
 *             Si se produce un reset a través de la patilla correspondiente,
 *             los registros PC e IR se inicializan a 0 y el resto se preservan.
 *             En cualquier caso, todo parece depender bastante del modelo
 *             concreto de Z80, así que se escoge el comportamiento del
 *             modelo Zilog Z8400APS. Z80A CPU.
 *             http://www.worldofspectrum.org/forums/showthread.php?t=34574
 */
Z80_COLD void Z80::reset(void) {
    if (pinReset) {
        pinReset = false;
    } else {
        regA = 0xff;

        setFlags(0xfd); // The only one flag reset at cold start is the add/sub flag

        REG_AFx = 0xffff;
        REG_BC = REG_BCx = 0xffff;
        REG_DE = REG_DEx = 0xffff;
        REG_HL = REG_HLx = 0xffff;

        REG_IX = REG_IY = 0xffff;

        REG_SP = 0xffff;

        REG_WZ = 0xffff;
    }

    REG_PC = 0;
    regI = 0;
    regR = 0;
    regRbase = 0;
    regRbit7 = false;
    ffIFF1 = false;
    ffIFF2 = false;
    pendingEI = false;
    activeNMI = false;
    activeNMIDOS = false;
    nmiDosInProgress = false;
    halted = false;
    setIM(IntMode::IM0);
    lastFlagQ = false;
    prefixOpcode = 0x00;
}

// Rota a la izquierda el valor del argumento
// El bit 0 y el flag C toman el valor del bit 7 antes de la operación
void Z80::rlc(uint8_t &oper8) {
    carryFlag = (oper8 > 0x7f);
    oper8 <<= 1;
    if (carryFlag) {
        oper8 |= CARRY_MASK;
    }
    sz5h3pnFlags = sz53pn_addTable[oper8];
    flagQ = true;
}
// Rota a la izquierda el valor del argumento
// El bit 7 va al carry flag
// El bit 0 toma el valor del flag C antes de la operación
void Z80::rl(uint8_t &oper8) {
    bool carry = carryFlag;
    carryFlag = (oper8 > 0x7f);
    oper8 <<= 1;
    if (carry) {
        oper8 |= CARRY_MASK;
    }
    sz5h3pnFlags = sz53pn_addTable[oper8];
    flagQ = true;
}

// Rota a la izquierda el valor del argumento
// El bit 7 va al carry flag
// El bit 0 toma el valor 0
void Z80::sla(uint8_t &oper8) {
    carryFlag = (oper8 > 0x7f);
    oper8 <<= 1;
    sz5h3pnFlags = sz53pn_addTable[oper8];
    flagQ = true;
}

// Rota a la izquierda el valor del argumento (como sla salvo por el bit 0)
// El bit 7 va al carry flag
// El bit 0 toma el valor 1
// Instrucción indocumentada
void Z80::sll(uint8_t &oper8) {
    carryFlag = (oper8 > 0x7f);
    oper8 <<= 1;
    oper8 |= CARRY_MASK;
    sz5h3pnFlags = sz53pn_addTable[oper8];
    flagQ = true;
}

// Rota a la derecha el valor del argumento
// El bit 7 y el flag C toman el valor del bit 0 antes de la operación
void Z80::rrc(uint8_t &oper8) {
    carryFlag = (oper8 & CARRY_MASK) != 0;
    oper8 >>= 1;
    if (carryFlag) {
        oper8 |= SIGN_MASK;
    }
    sz5h3pnFlags = sz53pn_addTable[oper8];
    flagQ = true;
}

// Rota a la derecha el valor del argumento
// El bit 0 va al carry flag
// El bit 7 toma el valor del flag C antes de la operación
void Z80::rr(uint8_t &oper8) {
    bool carry = carryFlag;
    carryFlag = (oper8 & CARRY_MASK) != 0;
    oper8 >>= 1;
    if (carry) {
        oper8 |= SIGN_MASK;
    }
    sz5h3pnFlags = sz53pn_addTable[oper8];
    flagQ = true;
}

// Rota a la derecha 1 bit el valor del argumento
// El bit 0 pasa al carry.
// El bit 7 conserva el valor que tenga
void Z80::sra(uint8_t &oper8) {
    uint8_t sign = oper8 & SIGN_MASK;
    carryFlag = (oper8 & CARRY_MASK) != 0;
    oper8 = (oper8 >> 1) | sign;
    sz5h3pnFlags = sz53pn_addTable[oper8];
    flagQ = true;
}

// Rota a la derecha 1 bit el valor del argumento
// El bit 0 pasa al carry.
// El bit 7 toma el valor 0
void Z80::srl(uint8_t &oper8) {
    carryFlag = (oper8 & CARRY_MASK) != 0;
    oper8 >>= 1;
    sz5h3pnFlags = sz53pn_addTable[oper8];
    flagQ = true;
}

/*
 * Half-carry flag:
 *
 * FLAG = (A ^ B ^ RESULT) & 0x10  for any operation
 *
 * Overflow flag:
 *
 * FLAG = ~(A ^ B) & (B ^ RESULT) & 0x80 for addition [ADD/ADC]
 * FLAG = (A ^ B) & (A ^ RESULT) &0x80 for subtraction [SUB/SBC]
 *
 * For INC/DEC, you can use following simplifications:
 *
 * INC:
 * H_FLAG = (RESULT & 0x0F) == 0x00
 * V_FLAG = RESULT == 0x80
 *
 * DEC:
 * H_FLAG = (RESULT & 0x0F) == 0x0F
 * V_FLAG = RESULT == 0x7F
 */
// Incrementa un valor de 8 bits modificando los flags oportunos
void Z80::inc8(uint8_t &oper8) {
    oper8++;

    sz5h3pnFlags = sz53n_addTable[oper8];

    if ((oper8 & 0x0f) == 0) {
        sz5h3pnFlags |= HALFCARRY_MASK;
    }

    if (oper8 == 0x80) {
        sz5h3pnFlags |= OVERFLOW_MASK;
    }

    flagQ = true;
    return;
}

// Decrementa un valor de 8 bits modificando los flags oportunos
void Z80::dec8(uint8_t &oper8) {
    oper8--;

    sz5h3pnFlags = sz53n_subTable[oper8];

    if ((oper8 & 0x0f) == 0x0f) {
        sz5h3pnFlags |= HALFCARRY_MASK;
    }

    if (oper8 == 0x7f) {
        sz5h3pnFlags |= OVERFLOW_MASK;
    }

    flagQ = true;
    return;
}

// The 8-bit add/sub family computes H and V branch-free (2026-10-12): the
// half-carry is bit 4 of A ^ op ^ result (the carry into bit 4), the overflow
// is bit 7 of (A ^ result) & ~(A ^ op) for an add, (A ^ result) & (A ^ op) for
// a subtract, moved to bit 2. Byte-identical to the compare-and-branch forms
// they replace over every (A, op, carry) — checked exhaustively on the host
// (scratch alu_check.cpp, 2026-10-12); ~18 fewer ARM instructions each.
// Suma de 8 bits afectando a los flags
void Z80::add(uint8_t oper8) {
    const uint32_t a = regA, r = a + oper8;
    const uint8_t res = (uint8_t)r;

    carryFlag = r > 0xff;
    sz5h3pnFlags = sz53n_addTable[res] | ((a ^ oper8 ^ res) & HALFCARRY_MASK)
            | ((((a ^ oper8 ^ 0x80) & (a ^ res)) >> 5) & OVERFLOW_MASK);

    regA = res;
    flagQ = true;
}

// Suma con acarreo de 8 bits
void Z80::adc(uint8_t oper8) {
    const uint32_t a = regA, r = a + oper8 + (carryFlag ? 1 : 0);
    const uint8_t res = (uint8_t)r;

    carryFlag = r > 0xff;
    sz5h3pnFlags = sz53n_addTable[res] | ((a ^ oper8 ^ res) & HALFCARRY_MASK)
            | ((((a ^ oper8 ^ 0x80) & (a ^ res)) >> 5) & OVERFLOW_MASK);

    regA = res;
    flagQ = true;
}

// Suma dos operandos de 16 bits sin carry afectando a los flags
void Z80::add16(RegisterPair &reg16, uint16_t oper16) {
    const uint32_t old = reg16.word, tmp = old + oper16;

    REG_WZ = old + 1;
    carryFlag = tmp > 0xffff;
    reg16.word = (uint16_t)tmp;
    // H = the carry out of bit 11 = bit 12 of old ^ op ^ result.
    sz5h3pnFlags = (sz5h3pnFlags & FLAG_SZP_MASK) | ((tmp >> 8) & FLAG_53_MASK)
            | (((old ^ oper16 ^ tmp) >> 8) & HALFCARRY_MASK);

    flagQ = true;
    return;
}

// Suma con acarreo de 16 bits
void Z80::adc16(uint16_t reg16) {
    uint16_t tmpHL = REG_HL;
    REG_WZ = REG_HL + 1;

    uint32_t res = REG_HL + reg16;
    if (carryFlag) {
        res++;
    }

    carryFlag = res > 0xffff;
    res &= 0xffff;
    REG_HL = (uint16_t) res;

    sz5h3pnFlags = sz53n_addTable[REG_H];
    if (res != 0) {
        sz5h3pnFlags &= ~ZERO_MASK;
    }

    if (((res ^ tmpHL ^ reg16) & 0x1000) != 0) {
        sz5h3pnFlags |= HALFCARRY_MASK;
    }

    if (((tmpHL ^ ~reg16) & (tmpHL ^ res)) > 0x7fff) {
        sz5h3pnFlags |= OVERFLOW_MASK;
    }

    flagQ = true;
}

// Resta de 8 bits
void Z80::sub(uint8_t oper8) {
    const uint32_t a = regA, r = a - oper8;
    const uint8_t res = (uint8_t)r;

    carryFlag = (r >> 8) != 0;
    sz5h3pnFlags = sz53n_subTable[res] | ((a ^ oper8 ^ res) & HALFCARRY_MASK)
            | ((((a ^ oper8) & (a ^ res)) >> 5) & OVERFLOW_MASK);

    regA = res;
    flagQ = true;
}

// Resta con acarreo de 8 bits
void Z80::sbc(uint8_t oper8) {
    const uint32_t a = regA, r = a - oper8 - (carryFlag ? 1 : 0);
    const uint8_t res = (uint8_t)r;

    carryFlag = (r >> 8) != 0;
    sz5h3pnFlags = sz53n_subTable[res] | ((a ^ oper8 ^ res) & HALFCARRY_MASK)
            | ((((a ^ oper8) & (a ^ res)) >> 5) & OVERFLOW_MASK);

    regA = res;
    flagQ = true;
}

// Resta con acarreo de 16 bits
void Z80::sbc16(uint16_t reg16) {
    uint16_t tmpHL = REG_HL;
    REG_WZ = REG_HL + 1;

    int32_t res = REG_HL - reg16;
    if (carryFlag) {
        res--;
    }

    carryFlag = res < 0;
    res &= 0xffff;
    REG_HL = (uint16_t) res;

    sz5h3pnFlags = sz53n_subTable[REG_H];
    if (res != 0) {
        sz5h3pnFlags &= ~ZERO_MASK;
    }

    if (((res ^ tmpHL ^ reg16) & 0x1000) != 0) {
        sz5h3pnFlags |= HALFCARRY_MASK;
    }

    if (((tmpHL ^ reg16) & (tmpHL ^ res)) > 0x7fff) {
        sz5h3pnFlags |= OVERFLOW_MASK;
    }
    flagQ = true;
}

// Operación AND lógica
void Z80::and_(uint8_t oper8) {
    regA &= oper8;
    carryFlag = false;
    sz5h3pnFlags = sz53pn_addTable[regA] | HALFCARRY_MASK;
    flagQ = true;
}

// Operación XOR lógica
void Z80::xor_(uint8_t oper8) {
    regA ^= oper8;
    carryFlag = false;
    sz5h3pnFlags = sz53pn_addTable[regA];
    flagQ = true;
}

void Z80::Xor(uint8_t oper8) {
    xor_(oper8);
}

// Operación OR lógica
void Z80::or_(uint8_t oper8) {
    regA |= oper8;
    carryFlag = false;
    sz5h3pnFlags = sz53pn_addTable[regA];
    flagQ = true;
}

// Operación de comparación con el registro A
// es como SUB, pero solo afecta a los flags
// Los flags SIGN y ZERO se calculan a partir del resultado
// Los flags 3 y 5 se copian desde el operando (sigh!)
void Z80::cp(uint8_t oper8) {
    const uint32_t a = regA, r = a - oper8;
    const uint8_t res = (uint8_t)r;

    carryFlag = (r >> 8) != 0;
    // Bits 5/3 come from the operand (the table copies them from its index).
    sz5h3pnFlags = (oper8 & FLAG_53_MASK) | (sz53n_subTable[res] & FLAG_SZHN_MASK)
            | ((a ^ oper8 ^ res) & HALFCARRY_MASK)
            | ((((a ^ oper8) & (a ^ res)) >> 5) & OVERFLOW_MASK);

    flagQ = true;
}

void Z80::Cp(uint8_t oper8) {
    cp(oper8);
}

// DAA
void Z80::daa(void) {
    uint8_t suma = 0;
    bool carry = carryFlag;

    if ((sz5h3pnFlags & HALFCARRY_MASK) != 0 || (regA & 0x0f) > 0x09) {
        suma = 6;
    }

    if (carry || (regA > 0x99)) {
        suma |= 0x60;
    }

    if (regA > 0x99) {
        carry = true;
    }

    if ((sz5h3pnFlags & ADDSUB_MASK) != 0) {
        sub(suma);
        sz5h3pnFlags = (sz5h3pnFlags & HALFCARRY_MASK) | sz53pn_subTable[regA];
    } else {
        add(suma);
        sz5h3pnFlags = (sz5h3pnFlags & HALFCARRY_MASK) | sz53pn_addTable[regA];
    }

    carryFlag = carry;
    // Los add/sub ya ponen el resto de los flags
    flagQ = true;
}

// POP
uint16_t Z80::pop(void) {
    uint16_t word = NextMemory::peek16(REG_SP);
    REG_SP = REG_SP + 2;
    return word;
}

// PUSH
void Z80::push(uint16_t word) {
    Z80Ops::poke8(--REG_SP, word >> 8);
    Z80Ops::poke8(--REG_SP, word);
}

// LDI

// LDD

// Z80N LDIX: LDI without flags; write to (DE) is inhibited when (HL) == A
IRAM_ATTR void Z80::ldix(void) {

    uint8_t work8 = Z80Ops::peek8(REG_HL);

    if (work8 != regA)
        Z80Ops::poke8(REG_DE, work8);
    else
        Z80Ops::addressOnBus(REG_DE, 3); // write cycle still occupies the bus

    Z80Ops::addressOnBus(REG_DE, 2);
    REG_HL++;
    REG_DE++;
    REG_BC--;
}

// Z80N LDDX: like LDIX but HL decrements (DE still increments)
IRAM_ATTR void Z80::lddx(void) {

    uint8_t work8 = Z80Ops::peek8(REG_HL);

    if (work8 != regA)
        Z80Ops::poke8(REG_DE, work8);
    else
        Z80Ops::addressOnBus(REG_DE, 3);

    Z80Ops::addressOnBus(REG_DE, 2);
    REG_HL--;
    REG_DE++;
    REG_BC--;
}

// Z80N LDWS: (DE) = (HL); L++; D++ (flags as INC D)
IRAM_ATTR void Z80::ldws(void) {

    uint8_t work8 = Z80Ops::peek8(REG_HL);
    Z80Ops::poke8(REG_DE, work8);
    REG_L++;
    inc8(REG_D);
}

// Z80N LDPIRX: pattern fill — source is (HL & 0xFFF8) | (E & 7), HL unchanged
IRAM_ATTR void Z80::ldpirx(void) {

    uint8_t work8 = Z80Ops::peek8((REG_HL & 0xFFF8) | (REG_E & 0x07));

    if (work8 != regA)
        Z80Ops::poke8(REG_DE, work8);
    else
        Z80Ops::addressOnBus(REG_DE, 3);

    Z80Ops::addressOnBus(REG_DE, 2);
    REG_DE++;
    REG_BC--;
}

// CPI

// CPD

// INI

// IND

// OUTI

// OUTD

// Pone a 1 el Flag Z si el bit b del registro
// r es igual a 0
/*
 * En contra de lo que afirma el Z80-Undocumented, los bits 3 y 5 toman
 * SIEMPRE el valor de los bits correspondientes del valor a comparar para
 * las instrucciones BIT n,r. Para BIT n,(HL) toman el valor del registro
 * escondido (REG_WZ), y para las BIT n, (IX/IY+n) toman el valor de los
 * bits superiores de la dirección indicada por IX/IY+n.
 *
 * 04/12/08 Confirmado el comentario anterior:
 *          http://scratchpad.wikia.com/wiki/Z80
 */
void Z80::bitTest(uint8_t mask, uint8_t reg) {
    bool zeroFlag = (mask & reg) == 0;

    sz5h3pnFlags = (sz53n_addTable[reg] & ~FLAG_SZP_MASK) | HALFCARRY_MASK;

    if (zeroFlag) {
        sz5h3pnFlags |= (PARITY_MASK | ZERO_MASK);
    }

    if (mask == SIGN_MASK && !zeroFlag) {
        sz5h3pnFlags |= SIGN_MASK;
    }
    flagQ = true;
}

// ATM-Turbo's check_trdos: the BIOS boot-menu / 128-menu / TR-DOS-prompt hooks and
// the DOS trap (Atm::remap owns every window). FLASH (Z80_COLD), not RAM: it runs only
// while Z80Ops::isAtm, so the RAM-resident check_trdos keeps one test for the rest.
Z80_COLD void Z80::check_trdos_atm() {
    if (EvoBase::nmiClrPending) EvoBase::nmiClrApply();
    const uint16_t menuCall = Atm::atm1 ? Atm::kBios1MenuCall : Atm::kBiosMenuCall;
    const uint16_t menuRet  = Atm::atm1 ? Atm::kBios1MenuRet
                            : (Config::romSetAtm == R_ATM2_106 ? Atm::kBios106MenuRet : Atm::kBiosMenuRet);
    if (Atm::cpmBootArmed && REG_PC == menuCall && (g_atm_ro & 1)) {
        Atm::cpmBootArmed = false;
        const uint16_t ret = (uint16_t)(MemESP::readbyte(REG_SP) |
                                        (MemESP::readbyte((uint16_t)(REG_SP + 1)) << 8));
        Debug::log("[ATM] BIOS boot menu call, ret=%04X: %s", ret,
                   ret == menuRet ? "answering CP/M" : "not the BIOS's, left alone");
        if (ret == menuRet) {             // the BIOS's own call: skip the menu, answer CP/M
            REG_SP += 2;
            REG_PC = ret;
            regA = 0;
            // BIOS 1.06 also takes the menu's other outputs (menu #80E7-#80FD):
            // D = TURBO (the menu starts ON), L = keyboard kind, 0 = ZX matrix /
            // 1 = XT (#00D3 DEC L picks the driver at #145C or #1174). Left to
            // chance, L = 1 installed the XT driver and the keyboard went dead.
            // E (boot options) is returned unchanged by the menu, as here.
            if (Config::romSetAtm == R_ATM2_106) { REG_D = 1; REG_L = 0; }
        }
    }
    if (Atm::trdosMenuArmed && REG_PC == Atm::k128MenuLoop && (g_atm_ro & 1)
        && !(Atm::p7ffd & 0x10) && !ESPectrum::trdos) {
        Atm::trdosMenuArmed = false;       // the 128 menu is up: pick its TR-DOS entry
        REG_SP = Atm::k128MenuSp;
        REG_PC = Atm::k128MenuTrdos;
        Atm::trdosBootState = 1;             // ...and let it run "boot"
        Debug::log("[ATM] 128 menu reached: taking its TR-DOS entry");
    }
    Atm::trdosTrap(REG_PCh);
    if (Atm::trdosBootState && ESPectrum::trdos && REG_PC == Atm::kTrdosEditor) {
        const uint16_t ret = (uint16_t)(MemESP::readbyte(REG_SP) |
                                        (MemESP::readbyte((uint16_t)(REG_SP + 1)) << 8));
        Atm::trdosBootState = 0;
        if (ret == Atm::kTrdosEditorRet) {
            // The first TR-DOS prompt: "type" RUN and press Enter.
            const uint16_t e = (uint16_t)(MemESP::readbyte(0x5C59) | (MemESP::readbyte(0x5C5A) << 8));
            MemESP::writebyte(e, 0xF7);                    // RUN
            MemESP::writebyte((uint16_t)(e + 1), 0x0D);
            MemESP::writebyte((uint16_t)(e + 2), 0x80);
            const uint16_t kcur = (uint16_t)(e + 1), end = (uint16_t)(e + 3);
            MemESP::writebyte(0x5C5B, kcur & 0xFF); MemESP::writebyte(0x5C5C, kcur >> 8);   // K_CUR
            for (uint16_t v : { (uint16_t)0x5C61, (uint16_t)0x5C63, (uint16_t)0x5C65 }) {   // WORKSP, STKBOT, STKEND
                MemESP::writebyte(v, end & 0xFF); MemESP::writebyte((uint16_t)(v + 1), end >> 8);
            }
            REG_SP += 2;
            REG_PC = ret;
            Debug::log("[ATM] TR-DOS prompt: typed RUN (E_LINE=%04X)", e);
        } else {
            Debug::log("[ATM] TR-DOS editor entered from %04X, not the prompt: autorun skipped", ret);
        }
    }
}

// Scorpion-family ROM bank on the TR-DOS exit (trdos=false). FLASH (Z80_COLD), not
// RAM: Scorpion/GMX/ProfROM/KAY/Phoenix only, and a DOS exit is not a hot path.
// Scorpion: 1FFD D1 service override outranks 7FFD D4; GMX/ProfROM keep the bank
// inside the live ProfROM plane (the 0x0100 tap re-arms via scorpionRomUpdate on the
// next port write). Nemo KAY: 1FFD D3 picks the ROM pair, DOS off (Ports.cpp
// scorpionRomUpdate); ZXM-Phoenix: 1FFD D1 forces the service page.
Z80_COLD uint8_t Z80::scorp_dos_exit_rom() {
    if (g_scorp_kay)
        return (g_scorp_kay == 4 && (Ports::port1FFD & 0x02)) ? (uint8_t)2
             : (uint8_t)(((Ports::port1FFD & 0x08) ? 2 : 0) | MemESP::romLatch);
    return (uint8_t)(((Ports::port1FFD & 0x02) ? 2 : MemESP::romLatch)
                     | (g_scorp_banked ? (Ports::gmxPlane << 2) : 0));
}

// True only while exec_nocheck() runs: the INT line is sampled at the slice
// boundary (CPU::stFrame) and nowhere inside it — the one context where the
// Next idle-loop fast-forward (NextIdle) may move the clock to stFrame.
bool z80_in_nocheck = false;   // NextReg::rasterPollFF reads it too

// Next: a taken backward JR NZ/Z (decodeOpcode20/28) — the same idle-loop
// candidate test check_trdos runs for JP/CALL/RET (NextSID's main loop is
// CP A / CP (HL) / JR Z, 96% of the frame).
static inline __attribute__((always_inline)) void nextIdleNote() {
    NextIdle::note(Z80::getRegPC());
}

// noinline: called from ~30 handlers, which Z80::exec_nocheck_next inlines.
IRAM_ATTR __attribute__((noinline)) void Z80::check_trdos() {

    // TS-Conf owns its window-0 mapping (Page0/MemConfig) — the generic code
    // below writes ramCurrent[0]/romInUse directly and calls recoverPage0(),
    // both of which would walk over the TS-Conf banking. Its own trap keeps
    // the DOS-signal semantics (enter at #3Dxx with ROM128+ROM, leave on
    // executing RAM) and remaps through TsConf::setBanks().
    // Only its two edges can act - enter at #3Dxx, leave from #4000+ - so the
    // flash TsConf::trdosTrap runs only then (this is on every jump/call/ret).
    if (Z80Ops::isTsconf) {
        if (ESPectrum::trdos ? REG_PCh >= 0x40 : REG_PCh == 0x3D) TsConf::trdosTrap(REG_PCh);
        return;
    }
    // ATM-Turbo: same shape — the memory manager owns every window (Atm::remap).
    // The flash half runs only when it can act: a boot hook armed, DOS up (the
    // leave-on-RAM test), or PC at #3Dxx (the enter test) — not on every jump of a
    // busy game (Dune II on the ZX-Evo: ~15% of all instructions are jumps).
    if (Z80Ops::isAtm) {
        if (Atm::beta || REG_PCh == 0x3D || Atm::cpmBootArmed || Atm::trdosMenuArmed || Atm::trdosBootState ||
            EvoBase::nmiClrPending)
            check_trdos_atm();
        return;
    }
    // Next has no Betadisk/TR-DOS: the 0x3Dxx window belongs to the divMMC
    // automap there, and rom[4] is never assigned (hw: the NMI-DOS restore
    // below ran on a stale flag and plugged a garbage rom[4] over the MMU —
    // the NextZXOS loader then CALLed into an empty ROM2 region and wedged).
    if (Z80Ops::isSam) {            // SAM Coupe: no Beta, no #3Dxx trap — only the
        if (REG_PC == Sam::TAPE_LDSTRT) Sam::tapeTrap();   // ROM 3.0 tape loader (flash)
        return;
    }
    if (Z80Ops::isNext) {
#if NEXT_TRACE
        nxJumpNote('J');
#endif
        // A target taken twice in a row may be a memory-poll idle loop
        // (NextIdle.h: Pogie's main loop, 55% of the frame).
        NextIdle::note(REG_PC);
        return;
    }

    // Detect NMI-DOS handler return: exact PC and SP match after planted RET at 0x5C00
    if (nmiDosInProgress && REG_PC == nmiDos_savedPC && REG_SP == nmiDos_savedSP) {
        nmiDosInProgress = false;
        MemESP::romInUse = nmiDos_savedRomInUse;
        ESPectrum::trdos = nmiDos_savedTrdos;
        if (ESPectrum::trdos) {
            MemESP::plugPtr16(0, MemESP::rom[Config::arch == A_SCORP
                ? ((g_scorp_banked ? (Ports::gmxPlane << 2) : 0) | 3) : 4].direct());
        } else {
            MemESP::recoverPage0();
        }
        if (g_scorp_banked) Ports::gmxTapRecheck(); // re-arm tap + re-register the bank overlay
        return;
    }

    if (DivMMC::enabled) return; // DivMMC automap handled in fetchOpcode/exec_nocheck
    if (MB02::enabled) return;   // MB-02 uses tape-compatible hooks, not 0x3Dxx trap

    if (!Config::betadisk) return;

    if (!Z80Ops::isALF) {

        if (!ESPectrum::trdos) {

            if (REG_PCh == 0x3D) {

                // TR-DOS Rom can be accessed from 48K machines and from Spectrum 128/+2 and Pentagon if the currently mapped ROM is bank 1.
                // For Profi: TR-DOS is accessed from bank 0 (SYS ROM), bank 2 (128K compat ROM), or bank 3 (SOS ROM) via 0x3Dxx.
                // newSRAM=true means slot 0 is a RAM page (Pentagon Hidden RAM), not the stock 48K ROM — skip TR-DOS automap.
                // For Profi: ZXMAK2 MemoryProfi1024.BusReadMem3D00_M1 triggers DOSEN only when IsRom48 — i.e. SOS bank3 (Sinclair OS). Not from SYS (0), TR-DOS (1), or 128K (2).
                // ...and only when ROM is actually mapped at slot 0: with NOROM
                // (DFFD bit4, page0ram) the CPU sees RAM at 0x0000-0x3FFF, so a
                // program legitimately running code at 0x3Dxx (Kings Valley CP/M
                // keeps a routine + stack there) must NOT trigger the automap —
                // it used to swap page0 for TR-DOS ROM mid-game and crash it.
                // Karabas UNLOCK_128 (#008B bit7): the FPGA trap fires on
                // "rom14=1 OR unlock_128" — with the bit set, TR-DOS is also
                // enterable from the 128K ROM (our bank 2, rom14=0).
                // Scorpion enters TR-DOS only from its 48 BASIC bank (romInUse==1 ⇒
                // romLatch=1 and no 1FFD D1 service override, which forces bank 2) and
                // only with ROM actually mapped at page 0 — 1FFD D0 (RAM0/page0ram)
                // makes 0x3Dxx ordinary RAM, no trap (same reasoning as Profi's
                // page0ram guard). Its TR-DOS is the machine's OWN bank 3, like Profi
                // uses its own bank 1 — not the shared external rom[4].
                if ((Z80Ops::is48 && MemESP::romInUse == 0 && MemESP::ramCurrent[0] != g_alfWindow) ||
                    (Config::arch == A_PROFI && MemESP::romInUse == 3 && !MemESP::newSRAM && !MemESP::page0ram) ||
                    (Config::arch == A_PROFI && MemESP::romInUse == 2 && (Ports::port008B & 0x80) && !MemESP::newSRAM && !MemESP::page0ram) ||
                    (Config::arch == A_SCORP && (MemESP::romInUse & (g_scorp_banked ? 3 : 0xFF)) == 1 && !MemESP::newSRAM && !MemESP::page0ram) ||
                    (!Z80Ops::is48 && Config::arch != A_PROFI && Config::arch != A_SCORP && MemESP::romInUse == 1 && !MemESP::newSRAM)) {
                    // Profi uses its own TR-DOS in ROM bank 1; Scorpion its own bank 3
                    // (on GMX inside the live ProfROM plane); others use the external
                    // TR-DOS ROM (bank 4)
                    uint8_t dosBank = (Config::arch == A_PROFI) ? 1
                                    : (Config::arch == A_SCORP) ? (uint8_t)((Ports::gmxPlane << 2) | 3) : 4;
#if FDD_PORT_TRACE
                    // trdos-transition trace (PQDOS RST8 chain: FE00 stub →
                    // CALL 3D38 automap → bank1 → JP 5C92 exit — chasing where
                    // the BIOS-path boot diverges, 2026-07-09).
                    if (Z80Ops::isProfi) {
                        static int dosMapCnt = 0;
                        if (dosMapCnt < 120)
                            Debug::log("[DOS MAP] pc=%04X romU %d->%d p0ram=%u rom14=%u #%d",
                                       REG_PC, (int)MemESP::romInUse, dosBank,
                                       (unsigned)MemESP::page0ram, (unsigned)MemESP::romLatch,
                                       ++dosMapCnt);
                    }
#endif
#if PAGE_TRACE
                    { static uint16_t n = 0;
                      if (n < 200) { n++;
                        Debug::log("[DOS IN ] pc=%04X romU->%u rom14=%u bank=%u",
                                   REG_PC, (unsigned)dosBank, (unsigned)MemESP::romLatch,
                                   (unsigned)MemESP::bankLatch); } }
#endif
#if GMX_TRACE
                    if (Config::arch == A_SCORP)
                        GMXT("[GMX trap+] pc=%04X romU %u->%u", REG_PC,
                             (unsigned)MemESP::romInUse, (unsigned)dosBank);
#endif
                    MemESP::romInUse = dosBank;
                    MemESP::plugPtr16(0, MemESP::rom[dosBank].direct());
                    ESPectrum::trdos = true;
                    if (g_scorp_banked) Ports::gmxTapRecheck();
                } else if (Config::arch == A_PROFI) {
                }

            }

        } else {

            // Exit trdos/SYSEN when PC leaves ROM space (0x0000-0x3FFF).
            // Profi special: only exit when in TR-DOS bank (bank1) or SYS ROM (bank0).
            // SYS ROM (bank0) SYSEN clears when execution reaches RAM — selects 128K/SOS ROM.
            bool doExit = REG_PCh >= 0x40;
            if (Config::arch == A_PROFI && MemESP::romInUse != 1 && MemESP::romInUse != 0) doExit = false;
            // Karabas ONROM (#008B bit6): the FPGA's forced-DOS level outranks
            // the PC>=0x4000 exit — hold trdos until the guest clears the bit.
            if (Config::arch == A_PROFI && (Ports::port008B & 0x40)) doExit = false;
            // GMX 1FFD D2: the DOS page is hard-wired at 0x0000 (Beta forced on) —
            // hold trdos until the guest clears the bit (same shape as ONROM).
            if (g_scorp_gmx && (Ports::port1FFD & 0x04)) doExit = false;

            if (doExit) {

#if FDD_PORT_TRACE
                // See matching [DOS MAP] trace above.
                if (Z80Ops::isProfi) {
                    static int dosExitCnt = 0;
                    if (dosExitCnt < 120)
                        Debug::log("[DOS EXIT] pc=%04X romU %d->%d p0ram=%u rom14=%u #%d",
                                   REG_PC, (int)MemESP::romInUse,
                                   (int)(MemESP::romLatch ? 3 : 2),
                                   (unsigned)MemESP::page0ram, (unsigned)MemESP::romLatch,
                                   ++dosExitCnt);
                }
#endif
                if (Z80Ops::is48)
                    MemESP::romInUse = 0;
                else if (Config::arch == A_PROFI)
                    // trdos=false: bit4=0→bank2(128K), bit4=1→bank3(SOS/48K)
                    MemESP::romInUse = MemESP::romLatch ? 3 : 2;
                else if (Config::arch == A_SCORP)
                    MemESP::romInUse = scorp_dos_exit_rom();   // flash, see above
                else
                    MemESP::romInUse = MemESP::romLatch;
#if PAGE_TRACE
                // The exit re-derives the ROM from the LATCH, so whatever the
                // TR-DOS ROM last wrote to 7FFD decides what the guest gets
                // back. Its own 128K helpers write 0xC6/0xC7 (bit4 = 0) and are
                // supposed to restore 0x10 — if one of them doesn't, the guest
                // resumes on the 128K ROM instead of 48 BASIC.
                { static uint16_t n = 0;
                  if (n < 200) { n++;
                    Debug::log("[DOS OUT] pc=%04X romU->%u rom14=%u bank=%u",
                               REG_PC, (unsigned)MemESP::romInUse,
                               (unsigned)MemESP::romLatch, (unsigned)MemESP::bankLatch); } }
#endif

                MemESP::recoverPage0();
                ESPectrum::trdos = false;
                if (g_scorp_banked) Ports::gmxTapRecheck();
#if GMX_TRACE
                if (Config::arch == A_SCORP)
                    GMXT("[GMX trap-] pc=%04X romU=%u", REG_PC, (unsigned)MemESP::romInUse);
#endif

            }

        }

    }

}

// IRAM_ATTR void Z80::check_trdos_unpage() {

//     if (ESPectrum::trdos) {

//         if (REG_PCh >= 0x40) {

//             if (Z80Ops::is48)
//                 MemESP::romInUse = 0;
//             else
//                 MemESP::romInUse = MemESP::romLatch;

//             MemESP::ramCurrent[0] = MemESP::rom[MemESP::romInUse];
//             ESPectrum::trdos = false;

//         }

//     } else if (REG_PCh == 0x3D) {

//             // TR-DOS Rom can be accessed from 48K machines and from Spectrum 128/+2 and Pentagon if the currently mapped ROM is bank 1.
//             if ((Z80Ops::is48) && (MemESP::romInUse == 0) || ((!Z80Ops::is48) && MemESP::romInUse == 1)) {
//                 MemESP::romInUse = 4;
//                 MemESP::ramCurrent[0] = MemESP::rom[MemESP::romInUse];
//                 ESPectrum::trdos = true;
//             }

//     }

// }

//Interrupción
/* Desglose de la interrupción, según el modo:
 * IM0:
 *      M1: 7 T-Estados -> reconocer INT y decSP
 *      M2: 3 T-Estados -> escribir byte alto y decSP
 *      M3: 3 T-Estados -> escribir byte bajo y salto a N
 * IM1:
 *      M1: 7 T-Estados -> reconocer INT y decSP
 *      M2: 3 T-Estados -> escribir byte alto PC y decSP
 *      M3: 3 T-Estados -> escribir byte bajo PC y PC=0x0038
 * IM2:
 *      M1: 7 T-Estados -> reconocer INT y decSP
 *      M2: 3 T-Estados -> escribir byte alto y decSP
 *      M3: 3 T-Estados -> escribir byte bajo
 *      M4: 3 T-Estados -> leer byte bajo del vector de INT
 *      M5: 3 T-Estados -> leer byte alto y saltar a la rutina de INT
 */
#if ZC_PORT_TRACE || ATM_PAGE_TRACE
// ATM EI trace (ZC_PORT_TRACE builds): the last 8 EIs with the pages in windows 0/1,
// dumped when an IM1 interrupt is taken INSIDE the NedoOS sys_sysint handler
// (#0909-#0950) — i.e. something re-enabled interrupts under the handler.
static uint16_t s_ei_pc[8];
static const void* s_ei_w0[8];
static const void* s_ei_w1[8];
static uint8_t s_ei_i = 0;
static uint16_t s_nest_logged = 0;
#endif
void Z80::interrupt(void) {
#if PERF_TRACE
    g_frm_int_taken++;   // machine-independent: was an interrupt taken this frame at all
    g_int_last_t = CPU::tstates;   // anchor for brdT's delta (see the [PERF] 60f d= field)
#endif
#if PERF_TRACE && PERF_HIST
    { extern uint32_t nx_int_cnt; nx_int_cnt++; }
#endif

    const bool wasHalted = halted;   // for TsConf::intTrace
    halted = false;

    // TS-Conf: the INT acknowledge cycle clears the source being taken and
    // selects its vector — in EVERY interrupt mode (zint.v latches on intack).
    // It must be evaluated HERE, at the T-state where the line was sampled:
    // the FRAME source is a 32-clock auto-expiring window (32 T at 3.5 MHz),
    // and taking it after the 7 T response + the 6 T push below put the ack
    // 13 T past the sample — on ~40% of frames the window had expired, the ack
    // came back "spurious" (0xFF, source not marked acknowledged) and the
    // frame counted as INT-not-taken (hw 2026-09-07, TS-BIOS/BASIC idle).
    const uint8_t tsVect = Z80Ops::isTsconf ? TsConf::intAck() : 0xFF;
    if (Z80Ops::isTsconf) TsConf::intTrace(REG_PC, REG_SP, tsVect, wasHalted);   // PERF_TRACE ring, before the push
    // ZX-Evo BaseConf (zint.v intend = INTA): the acknowledge ends the pulse, so a
    // handler shorter than the window (ERS's EI / RET) is not interrupted twice.
    if (Z80Ops::isAtm && Atm::evo)
        EvoBase::intAckFrame = EvoBase::intWindowId((uint32_t)((int32_t)CPU::tstates + CPU::latetiming),
                                                   CPU::statesInFrame, CPU::global_tstates);

    // Z80Ops::interruptHandlingTime(7);
    VIDEO::Draw(7, false);

    regRbase++;   // R advances, but the acknowledge is not an opcode fetch (RZX)
    // RZX playback: one acknowledge per recorded frame. A handler shorter than the
    // INT pulse (TR-DOS's `EI / RET` at #0038 is 27 T) is re-entered on real
    // hardware — and the recording has that as a frame of its own (2 fetches), so
    // the line must not stay up here and fire a second time by itself.
    Rzx::intUntil = 0;

    ffIFF1 = ffIFF2 = false;
#if PAGE_TRACE
    const uint16_t pgIntPC = REG_PC;   // interrupted address, for the alarm below
#endif
#if ZC_PORT_TRACE || ATM_PAGE_TRACE
    if (Z80Ops::isAtm && REG_PC >= 0x0909 && REG_PC < 0x0951 && s_nest_logged < 20) {
        s_nest_logged++;
        Debug::log("[ATMEI] INT taken at pc=%04X sp=%04X w0=%p w1=%p", REG_PC, REG_SP,
                   (const void*)MemESP::ramCurrent[0], (const void*)MemESP::ramCurrent[2]);
        for (int j = 0; j < 8; j++) {
            const uint8_t k = (uint8_t)(s_ei_i - 8 + j) & 7;
            Debug::log("[ATMEI]   EI#%d pc=%04X w0=%p w1=%p", j, s_ei_pc[k], s_ei_w0[k], s_ei_w1[k]);
        }
    }
#endif
    push(REG_PC); // el push añadirá 6 t-estados (+contended si toca)
    if (modeINT == IntMode::IM2) {

        // INT-ack bus byte: the Karabas serial-mouse hw_int drives 0xE7
        // (RST20H) while its request is asserted; the ULA default is 0xFF.
        // TS-Conf drives a per-source vector (#FF FRAME / #FD LINE / #FB DMA).
        // Next hardware IM2 (NR 0xC0 bit 0): {NR 0xC0 7:5, source, 0}.
        uint8_t busByte = Z80Ops::isTsconf ? tsVect
                        : (Z80Ops::isNext && NextInt::ex) ? NextInt::ack()
                        : (Ports::serialMouseIntAsserted() ? 0xE7 : 0xFF);
        REG_PC = Z80Ops::peek16((regI << 8) | busByte); // +6 t-estados

        check_trdos();
        // check_trdos_unpage();

    } else {
        // IM0 executes the bus byte: RST20H → 0x0020 for the serial mouse.
        REG_PC = (modeINT == IntMode::IM0 && Ports::serialMouseIntAsserted())
                     ? 0x0020 : 0x0038;
#if PAGE_TRACE
        // Alarm: an IM1 interrupt entering the 128K BASIC ROM at 0x0038. That
        // handler is NOT self-contained — it pushes 0x0048/0x5B00/0x0038 and
        // jumps to a stub the ROM copies into RAM at 0x5B00 during its own
        // boot. TR-DOS software runs with ROM 1 (48 BASIC, self-contained
        // handler) and is entitled to use 0x5B00 as ordinary memory, so if we
        // hand it ROM 0 here the guest dies: NOP sled through 0x5B00 → RST 38
        // off the 0xFF at 0x5C00 → the stack marches down until something wild
        // happens. Exactly the ExTracker 3.07 hang (it EI/HALTs while waiting
        // for the General Sound to answer its detect command 0x23).
        if (REG_PC == 0x0038 && !Z80Ops::is48 && MemESP::romInUse == 0 &&
            !MemESP::page0ram && !MemESP::newSRAM) {
            const uint8_t* p5 = MemESP::ramCurrent[2];   // 8K slot at 0x4000 (page 5)
            if (p5 && p5[0x1B00] == 0x00) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    Debug::log("[PAGE] IM1 -> ROM0 with DEAD 5B00 stub: intPC=%04X "
                               "rom14=%u lock=%u dos=%u bank=%u",
                               (unsigned)pgIntPC, (unsigned)MemESP::romLatch,
                               (unsigned)MemESP::pagingLock, (unsigned)ESPectrum::trdos,
                               (unsigned)MemESP::bankLatch);
                }
            }
        }
#endif
    }
    REG_WZ = REG_PC;

}

//Interrupción NMI, no utilizado por ahora
/* Desglose de ciclos de máquina y T-Estados
 * M1: 5 T-Estados -> extraer opcode (pá ná, es tontería) y decSP
 * M2: 3 T-Estados -> escribe byte alto de PC y decSP
 * M3: 3 T-Estados -> escribe byte bajo de PC y PC=0x0066
 */
Z80_COLD void Z80::nmi(void) {

    halted = false;

    // Esta lectura consigue dos cosas:
    //      1.- La lectura del opcode del M1 que se descarta
    //      2.- Si estaba en un HALT esperando una INT, lo saca de la espera
    // Next: the discarded M1 must not run the divMMC entry-point check. Our
    // NMI is taken at a frame boundary, where the frame INT has usually just
    // been accepted (PC = 0x0038, not fetched): fetchOpcode() there tripped the
    // RST 0x38 entry point, automapped esxDOS, and the NMI then fetched 0x0066
    // from ESXMMC — a RET, the esxDOS NMI browser never opened (hw 2026-10-07,
    // [NMIT] trace: A01@0038 right after the request, no 0x0066 trap). On a
    // real Next the two cannot coincide like that; the cycle's timing is kept.
    if (Z80Ops::isNext) VIDEO::Draw(4, false);
    else Z80Ops::fetchOpcode();

    VIDEO::Draw(1, false);

    regR++;
    ffIFF1 = false;
    // Next: NR 0xC2/0xC3 capture the return address on every NMI; with NR 0xC0
    // bit 3 (stackless NMI) the two stack writes are suppressed, SP still moves
    // (zxnext.vhd 2050-2085).
    if (Z80Ops::isNext && NextInt::nmiStackless(REG_PC)) {
        VIDEO::Draw(6, false);
        REG_SP = (uint16_t)(REG_SP - 2);
    } else
    push(REG_PC); // 3+3 t-estados + contended si procede
    REG_PC = REG_WZ = 0x0066;
    if (Z80Ops::isNext) NextMf::nmiTaken();   // the MF supplies the 0x0066 fetch
#if NEXT_TRACE
    if (Z80Ops::isNext) nxJumpNote('N');
#endif

}

Z80_COLD void Z80::doNMI(void) {

    activeNMI = false;
    lastFlagQ = false;
    // ZEsarUX approach: reset DivMMC state before NMI so automap trap at 0x0066
    // fires correctly. Without this, if automap is already ON, preOpcFetch
    // won't set trap_after (it checks !automap) and 0x0066 reads C9=RET from
    // ESXDOS ROM instead of F5=PUSH AF from Spectrum ROM.
    // Not on the Next: there the NMI does not touch the divMMC state — the
    // 0x0066 entry point maps it (NR 0xBB) and RETN unmaps (DivMMC::nextRetn).
    if (DivMMC::enabled && !Z80Ops::isNext) {
        DivMMC::conmem = false;
        DivMMC::automap = false;
        DivMMC::applyMapping();
    }
    // MB-02+: ensure SRAM page 0 is mapped for NMI handler (BS-ROM 118 NMI menu)
    if (MB02::enabled) {
        MB02::writePort17(0x60); // SRAM page 0, write enable
    }
    // Scorpion magic button: the hardware asserts 1FFD D1 (service page) with
    // the NMI pulse, so the 0x0066 handler always executes from the service
    // monitor (MAME scorpion nmi_check_callback: port_1ffd |= 0x02 + pulse).
    // A bare NMI landed at 0x0066 of whatever ROM happened to be paged — the
    // Sinclair banks have no handler there, hence "enters the monitor only
    // sometimes". Done at the ack point so not a single opcode is fetched from
    // the swapped page before the NMI vectors; the monitor exits by clearing
    // D1 itself.
    // (Nemo KAY has no such line: 1FFD D1 is a printer output there, and its
    // service page is reached through D3 by the ROM itself.)
    if (Z80Ops::isScorpion && !g_scorp_kay) {
        Ports::port1FFD |= 0x02;
        Ports::scorpionRomUpdate();
    }
    // ZX-Evo BaseConf (znmi.v): the M1 at #0066 reads #00 — the FPGA drives a NOP
    // onto the bus (drive_00) — and RAM page #FF replaces window 0 only after that
    // M1 (in_nmi rises at its refresh), until OUT (#BE). So the handler in page #FF
    // starts at #0067. Switching before the #0066 fetch ran whatever byte page #FF
    // holds there (EVO Reset Service's Magic button did nothing, hw 2026-10-02).
    if (Z80Ops::isAtm && Atm::evo) {
        // znmi.v: nmi_start && !in_nmi — a request while the handler runs is dropped.
        if (EvoBase::inNmi) return;
        nmi();
        VIDEO::Draw(4, false);   // the NOP at #0066
        regR++;
        REG_PC = REG_WZ = 0x0067;
        EvoBase::nmiEnter();
        return;
    }
    nmi();

}

Z80_COLD void Z80::doNMIDOS(void) {

    activeNMIDOS = false;
    if (Z80Ops::isNext) return;   // no TR-DOS Magic Button on Next
    lastFlagQ = false;

    // ATM-Turbo: the magic-button DOS entry maps rom[] banks over window 0, which on
    // this machine belongs to the memory manager — take a plain NMI instead.
    if (Z80Ops::isAtm) { nmi(); return; }

    // Save current state
    nmiDos_savedRomInUse = MemESP::romInUse;
    nmiDos_savedTrdos = ESPectrum::trdos;
    nmiDos_savedSP = REG_SP;
    nmiDos_savedPC = REG_PC; // Save PC — the interrupted address

    // Switch to TR-DOS ROM (slot 4) for Magic Button NMI
    // Note: Gluk ROM has no NMI handler (0x0066 = 0xFF filler) — it's RESET-only
    // Scorpion: TR-DOS is the machine's own bank 3 (rom[4] is unused there);
    // on GMX inside the live ProfROM plane.
    uint8_t nmiDosBank = (Config::arch == A_SCORP)
                             ? (uint8_t)((g_scorp_banked ? (Ports::gmxPlane << 2) : 0) | 3) : 4;
    MemESP::romInUse = nmiDosBank;
    MemESP::plugPtr16(0, MemESP::rom[nmiDosBank].direct());
    ESPectrum::trdos = true; // Protect ROM from 7FFD changes
    if (g_scorp_banked) Ports::gmxTapRecheck(); // re-arm tap + re-register the bank overlay

    // Mark NMI-DOS in progress
    nmiDosInProgress = true;

    // Standard NMI sequence (pushes PC, SP becomes savedSP-2)
    nmi();

}

IRAM_ATTR void Z80::checkINT(void) {

    // Comprueba si está activada la señal INT
    if (ffIFF1 && !pendingEI && Z80Ops::isActiveINT()) {
        lastFlagQ = false;
        interrupt();
    }

}

IRAM_ATTR void Z80::incRegR(uint32_t inc) {

    regR += inc;

}

#if PERF_TRACE && PERF_HIST
// PERF_HIST: base-opcode histogram from exec_nocheck (prefix bytes CB/DD/ED/FD
// count as themselves — the prefixed instruction is dispatched by decodeXX, not
// by dcOpcode). Read + cleared by the [PERF] dump in Video.cpp every 600 frames.
uint32_t z80_op_hist[256];
uint32_t z80_chk_cnt;
// Next profile (PERF_HIST, exec_nocheck_next): second byte of every prefixed
// instruction ([0] CB, [1] DD, [2] ED, [3] FD) and guest PC in 32-byte buckets.
// nxprof_dump() logs and clears them once a second (ESPectrum::loop).
uint32_t nx_pfx_hist[4][256];
uint32_t nx_pc_hist[2048];
uint32_t nx_int_cnt;

static void nxprofTop(const char* tag, const uint32_t* h, int n, int show, int shift, uint64_t total) {
    char line[240]; int p = snprintf(line, sizeof line, "%s", tag);
    static uint8_t used[2048];
    memset(used, 0, n);
    for (int k = 0; k < show && p < (int)sizeof line - 16; k++) {
        int best = -1; uint32_t bv = 0;
        for (int i = 0; i < n; i++) if (!used[i] && h[i] > bv) { bv = h[i]; best = i; }
        if (best < 0) break;
        used[best] = 1;
        p += snprintf(line + p, sizeof line - p, " %0*X:%u", shift ? 4 : 2, best << shift,
                      (unsigned)(total ? (uint64_t)bv * 1000 / total : 0));
    }
    Debug::log("%s", line);
}

void nxprof_dump(uint32_t frames) {
    uint64_t total = 0, ptot = 0;
    for (int i = 0; i < 256; i++) total += z80_op_hist[i];
    for (int i = 0; i < 2048; i++) ptot += nx_pc_hist[i];
    if (!frames) frames = 1;
    Debug::log("NXPROF %u fr/s, %u ops/fr (incl. prefix bytes), %u INT/fr, MMU %02X %02X %02X %02X %02X %02X %02X %02X",
               (unsigned)frames, (unsigned)(total / frames), (unsigned)(nx_int_cnt / frames),
               NextReg::reg[0x50], NextReg::reg[0x51], NextReg::reg[0x52], NextReg::reg[0x53],
               NextReg::reg[0x54], NextReg::reg[0x55], NextReg::reg[0x56], NextReg::reg[0x57]);
    {
        // PERF z80: CPU::loop time of this second against the ops it ran (cpu_frame_us
        // is cumulative on the Next). ns/op includes the histogram's own cost.
        extern volatile uint32_t cpu_frame_us;
        static uint32_t s_prevCpu = 0;
        const uint32_t c = cpu_frame_us, d = c - s_prevCpu;
        s_prevCpu = c;
        Debug::log("NXPROF z80: %u ops/fr, %u ns/op, cpu %u.%u ms/fr, mult %u",
                   (unsigned)(total / frames), (unsigned)(total ? (uint64_t)d * 1000 / total : 0),
                   (unsigned)(d / frames / 1000), (unsigned)(d / frames % 1000 / 100),
                   (unsigned)ESPectrum::multiplicator);
    }
    nxprofTop("NXPROF op:", z80_op_hist, 256, 24, 0, total);
    static const char* const tags[4] = { "NXPROF CB:", "NXPROF DD:", "NXPROF ED:", "NXPROF FD:" };
    for (int k = 0; k < 4; k++) nxprofTop(tags[k], nx_pfx_hist[k], 256, 14, 0, total);
    nxprofTop("NXPROF pc:", nx_pc_hist, 2048, 20, 5, ptot);
    {
        // A heavy second (> 35k ops a frame): the guest's own code around its four
        // hottest 32-byte PC buckets, the registers and the stack, as the memory map
        // shows them right now — the dump of a stall nobody can catch by hand.
        // At most 4 such dumps, re-armed after a quiet second (< 15k ops a frame).
        static uint8_t s_cdumps = 0, s_armed = 1;
        const uint32_t opf = (uint32_t)(total / frames);
        if (opf < 15000) s_armed = 1;
        if (opf > 35000 && s_armed && s_cdumps < 4) {
            s_armed = 0; s_cdumps++;
            Debug::log("NXCODE #%u PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X IFF1=%u IM=%u R=%02X",
                       (unsigned)s_cdumps, Z80::getRegPC(), Z80::getRegSP(), Z80::getRegAF(), Z80::getRegBC(),
                       Z80::getRegDE(), Z80::getRegHL(), Z80::getRegIX(), Z80::getRegIY(),
                       (unsigned)Z80::isIFF1(), (unsigned)Z80::getIM(), (unsigned)Z80::getRegR());
            auto line = [](const char* tag, uint16_t a) {
                char b[160]; int p = snprintf(b, sizeof b, "%s %04X:", tag, a);
                for (int i = 0; i < 32; i++) {
                    const uint16_t x = (uint16_t)(a + i);
                    const uint8_t* s = MemESP::ramCurrent[x >> 13];
                    if ((uintptr_t)s < 0x10000000u) p += snprintf(b + p, sizeof b - p, " --");
                    else p += snprintf(b + p, sizeof b - p, " %02X", s[x & 0x1FFF]);
                }
                Debug::log("%s", b);
            };
            line("NXCODE stk", Z80::getRegSP());
            uint8_t used[2048 / 8] = {};
            for (int k = 0; k < 8; k++) {
                int best = -1; uint32_t bv = 0;
                for (int i = 0; i < 2048; i++) if (!(used[i >> 3] & (1 << (i & 7))) && nx_pc_hist[i] > bv) { bv = nx_pc_hist[i]; best = i; }
                if (best < 0) break;
                used[best >> 3] |= (uint8_t)(1 << (best & 7));
                const uint16_t a = (uint16_t)(best << 5);
                line("NXCODE", (uint16_t)(a - 32));
                line("NXCODE", a);
                line("NXCODE", (uint16_t)(a + 32));
            }
            line("NXCODE pc", (uint16_t)(Z80::getRegPC() - 16));
        }
    }
    {
        extern uint32_t g_nxps[7];
        Debug::log("NXPROF pollskip: calls %u newpc %u regs %u long %u (min %u T) skip %u passes %u",
                   (unsigned)g_nxps[0], (unsigned)g_nxps[1], (unsigned)g_nxps[2], (unsigned)g_nxps[3],
                   (unsigned)(g_nxps[6] == 0xFFFFFFFFu ? 0 : g_nxps[6]), (unsigned)g_nxps[4], (unsigned)g_nxps[5]);
        for (int i = 0; i < 6; i++) g_nxps[i] = 0;
        g_nxps[6] = 0xFFFFFFFFu;
    }
    for (int i = 0; i < 256; i++) z80_op_hist[i] = 0;
    memset(nx_pfx_hist, 0, sizeof nx_pfx_hist);
    memset(nx_pc_hist, 0, sizeof nx_pc_hist);
    nx_int_cnt = 0;
    NextInt::ctcProfDump();
    extern void pcprof_dump();
    pcprof_dump();
}
#endif

IRAM_ATTR void Z80::execute() {

    // CPU::tstates_diff += (CPU::tstates - CPU::prev_tstates);
    // if (CPU::tstates_diff >= 14) {
    //     while (1) {
    //         // printf("CPU::tstates_diff: %d\n",CPU::tstates_diff);
    //         rvmWD1793Step(&ESPectrum::fdd); // FDD
    //         CPU::tstates_diff -= 14;
    //         if (CPU::tstates_diff < 14) break;
    //     }
    // }
    // CPU::prev_tstates = CPU::tstates;

    // // if (!(CPU::tstates & 0xf)) {
    //     if (Tape::tapeStatus == TAPE_LOADING && !Tape::tapFlashCont) {
    //     // if (Tape::tapePhase == TAPE_PHASE_DATA) {
    //         Tape::Read();
    //     }
    // // }

    // uint8_t pg = REG_PC >> 14;
    // VIDEO::Draw_Opcode(MemESP::ramContended[pg]);
    // opCode = MemESP::ramCurrent[pg][REG_PC & 0x3fff];

    // uint8_t pg = REG_PC >> 14;
    // opCode = MemESP::ramCurrent[pg][REG_PC & 0x3fff];
    // if (MemESP::ramContended[pg]) {
    //     MemESP::lastContendedMemReadWrite = opCode;
    //     VIDEO::Draw_Opcode(true);
    // } else {
    //     VIDEO::Draw_Opcode(false);
    // };

    // Ports::FDDStep();

#if NEO8_TRAP
    Debug::neo8TrapStep(REG_PC, REG_SP, REG_IX, REG_IY);
#endif
    opCode = Z80Ops::fetchOpcode();

    regR++;

    if (!halted) {

        REG_PC++;
#if PERF_TRACE && PERF_HIST
        z80_op_hist[opCode]++;
        z80_chk_cnt++;
#endif

        if (prefixOpcode == 0) {
            flagQ = pendingEI = false;
            dcOpcode[opCode]();
        } else if (prefixOpcode == 0xDD) {
            prefixOpcode = 0;
            decodeDDFD(regIX);
        } else if (prefixOpcode == 0xED) {
            prefixOpcode = 0;
            decodeED();
        } else if (prefixOpcode == 0xFD) {
            prefixOpcode = 0;
            decodeDDFD(regIY);
        } else return;

        if (prefixOpcode != 0) return;

        lastFlagQ = flagQ;

    }

    // Ahora se comprueba si está activada la señal INT
    checkINT();

}

// z80_in_nocheck: defined near check_trdos (the Next idle-loop hook reads it).

// Whole-address fast memory path (g_ts_fastmem == 2: TS-Conf, ATM/Evo whole-line
// modes, Pentagon 16col), the inner loop of exec_nocheck on its own: every bank
// is a plain POINTER page, the video hook is a T-state counter (TsFastMem.h), and
// no DivMMC / ROM overlay / Timex window can be live, so the fetch is one table
// load. Split out so the compiler keeps the loop's addresses in registers (the
// shared loop's register pressure cost ~25 ARM instructions per Z80 instruction,
// measured with tools/z80bench on the ccr-05d4d17f-wh3s7v branch). Semantics are
// the shared loop's, in the same order. Returns when the slice ends or the fast
// path drops mid-slice (NeoGS ZX-DMA window, a memory breakpoint, a mode change),
// so the shared loop carries on from the very next instruction.
IRAM_ATTR __attribute__((noinline, optimize("O2"))) void Z80::exec_nocheck_ts() {
    while (CPU::tstates < CPU::stFrame) {
        if (__builtin_expect(g_ts_fastmem != 2, 0)) return;
        const uint16_t pc = REG_PC;
        tsFastTick(4);
        if (__builtin_expect(g_ts_memcyc != 0, 0) && !tsMemNoDram(pc)) tsFastTick(TsConf::cpuMemMiss(pc, true));   // DRAM model: a cache miss
        opCode = MemESP::ramCurrent[pc >> 13][pc & 0x1fff];
        C16_FE(pc);
        regR++;
        REG_PC = pc + 1;
#if PERF_TRACE && PERF_HIST
        z80_op_hist[opCode]++;
        if (Z80Ops::isTsconf) {
            extern uint32_t ts_page_hist[257];
            const uintptr_t bp = (uintptr_t)MemESP::ramCurrent[pc >> 13];
            ts_page_hist[(bp >= 0x10000000u && bp < 0x11000000u) ? 256 : TsConf::r.page[pc >> 14]]++;
        }
#endif
        if (__builtin_expect(prefixOpcode == 0, 1)) {
            flagQ = pendingEI = false;
            dcOpcode[opCode]();
            lastFlagQ = flagQ;
            continue;
        }
        if (prefixOpcode == 0xDD) { prefixOpcode = 0; decodeDDFD(regIX); }
        else if (prefixOpcode == 0xED) { prefixOpcode = 0; decodeED(); }
        else if (prefixOpcode == 0xFD) { prefixOpcode = 0; decodeDDFD(regIY); }
        else continue;
        if (prefixOpcode == 0) lastFlagQ = flagQ;
    }
}

// Separate from the multi-machine executor so its register allocation and
// hot loop do not carry the generic memory gates or inlined automap machinery.
// PC breakpoints use the original checked-per-PC path below.
// The unprefixed opcode is dispatched by a SWITCH, not through dcOpcode[]
// (2026-10-11, NXModPlayer at 28 MHz): the handlers are inlined into it, the
// LD r,r' / ALU r / INC / DEC families with the register fixed per case. Saves
// the indirect call + return and the generic families' reg8[] lookups on every
// instruction. In the .nxovl window (Next only): the inlined copies cost SRAM
// on a Next boot alone. ZEXDOC through tools/z80bench (`next` mode) checks it
// bit for bit against the table dispatch.
// check_trdos() for the Next opcode switch: its Next branch inline (the
// NextIdle candidate test, every JP/CALL/RET/JR). The full call otherwise:
// a trace build (nxJumpNote) and z80bench's non-Next harness.
// check_trdos() for the Next opcode switch: its Next branch inline (the
// NextIdle candidate test, every JP/CALL/RET/JR). The full call otherwise:
// a trace build (nxJumpNote) and z80bench's non-Next harness. The loop keeps
// CPU::tstates in nx_t (below), and NextIdle::jump() reads AND advances the
// global, so it is published around the call; nxIdleNote() = the JR hook.
#define nxSync(expr) do { CPU::tstates = nx_t; regR = nx_r; expr; nx_t = CPU::tstates; nx_r = regR; } while (0)
#if NEXT_TRACE
#define nxCheckTrdos() nxSync(check_trdos())
#else
#define nxCheckTrdos() do { if (__builtin_expect(Z80Ops::isNext, 1)) nxSync(NextIdle::note(REG_PC)); \
        else nxSync(check_trdos()); } while (0)
#endif
#define nxIdleNote() nxSync(NextIdle::note(REG_PC))
// push()/pop() with the gate-free Next-inline access (next_dispatch.inc pastes
// handler bodies): one slot lookup for the word when it does not straddle a slot.
#define nxPush(w) do { const uint16_t nx_w_ = (uint16_t)(w); \
    REG_SP -= 2; NextMemory::poke16_nx(nx_t, REG_SP, nx_w_); } while (0)
#define nxPop() ([&nx_t]() __attribute__((always_inline)) { const uint16_t nx_w_ = NextMemory::peek16_nx(nx_t, REG_SP); REG_SP += 2; return nx_w_; }())
NX_OVL_CODE __attribute__((optimize("O2"))) void Z80::exec_nocheck_next(bool divmmc) {
    // The T-state counter lives here for the whole slice (2026-10-14): the
    // global's load/store on every tick and every memory access was ~10% of
    // the loop on NXModPlayer. Everything that reads or moves CPU::tstates —
    // the raster tick, the table handlers, the prefix decoders, NextIdle, the
    // automap trap pages, the accessors' fallback paths — gets it published
    // first and reloads it after (nxSync / next_memory.h).
    uint32_t nx_t = CPU::tstates, nx_r = regR;   // regR too: NextIdle reads/moves it (nxSync)
    uint8_t nx_op;                                 // opCode: published before the table handlers
    do {
        const uint16_t pc = REG_PC;
        nx_t += 4;
        if (__builtin_expect(nx_t >= VIDEO::ts_line_t, 0)) nxSync(VIDEO::tsDrawTick());
        if (divmmc && __builtin_expect(DivMMC::opcodeTrapPage(pc), 0)) nxSync(nx_op = DivMMC::nextOpcodeSlow(pc));
        else nx_op = MemESP::ramCurrent[pc >> 13][pc & 0x1fff];
        nx_r++;
        REG_PC = pc + 1;
#if PERF_TRACE && PERF_HIST
        z80_op_hist[nx_op]++;
        if (prefixOpcode == 0) {
            nx_pc_hist[pc >> 5]++;
            if (nx_op == 0xCB || nx_op == 0xDD || nx_op == 0xED || nx_op == 0xFD) {
                const uint16_t q = pc + 1;
                const uint8_t* sp = MemESP::ramCurrent[q >> 13];
                if (sp) nx_pfx_hist[nx_op == 0xCB ? 0 : nx_op == 0xDD ? 1 : nx_op == 0xED ? 2 : 3][sp[q & 0x1fff]]++;
            }
        }
#endif
        if (__builtin_expect(prefixOpcode == 0, 1)) {
            flagQ = pendingEI = false;
            uint8_t nx_v; uint8_t* nx_p; bool nx_cc;   // next_dispatch.inc: shared ALU / INC / DEC / cc tails
            switch (nx_op) {
#include "next_dispatch.inc"
            }
            lastFlagQ = flagQ;
            continue;
        }
        opCode = nx_op;
        CPU::tstates = nx_t; regR = nx_r;
        if (prefixOpcode == 0xDD) { prefixOpcode = 0; decodeDDFD(regIX); }
        else if (prefixOpcode == 0xED) { prefixOpcode = 0; decodeED(); }
        else if (prefixOpcode == 0xFD) { prefixOpcode = 0; decodeDDFD(regIY); }
        else continue;
        nx_t = CPU::tstates; nx_r = regR;
        if (prefixOpcode == 0) lastFlagQ = flagQ;
        // A port write in a prefixed handler (OUT (C),r / NEXTREG) may have dropped the
        // gate: the pasted bodies assume it (NextMemory::*_nx), so leave at once.
        if (__builtin_expect(!g_next_fastrd, 0)) return;
    } while (nx_t < CPU::stFrame);   // a g_next_fastrd drop ends the slice (Video.cpp nextFastrdSet)
    regR = nx_r;
    CPU::tstates = nx_t;
}

IRAM_ATTR void Z80::exec_nocheck() {

    int nbp = Config::numPcBP;
    z80_in_nocheck = true;
    // Loop-invariant: DivMMC enable only changes from the menu (DivMMC::init),
    // never from guest execution — hoist the global load out of the hot loop.
    // (VIDEO::Draw_Opcode canNOT be hoisted: the video state machine swaps it
    // mid-frame at blank/paper boundaries.)
    const bool divmmc = DivMMC::enabled;

    while (CPU::tstates < CPU::stFrame) {

#if !NEO8_TRAP
        if (g_next_fastrd && nbp <= 0) {
            exec_nocheck_next(divmmc);
            continue;
        }
        // Whole-address fast path (TS-Conf / ATM / 16col): its own loop. Not with
        // DivMMC (its automap is decided per fetch) or a PC breakpoint.
        if (g_ts_fastmem == 2 && nbp <= 0 && !divmmc) {
            exec_nocheck_ts();
            if (g_ts_fastmem == 2) continue;   // slice ended (the while test exits)
        }
#endif

        if (nbp > 0 && Config::pcBreakHit(REG_PC)) { z80_in_nocheck = false; return; }
#if NEO8_TRAP
        Debug::neo8TrapStep(REG_PC, REG_SP, REG_IX, REG_IY);
#endif
        uint8_t pg = REG_PCh >> 5;
        if (g_ts_fastmem > tsFastLo8(REG_PC)) {   // TsFastMem.h: TS-Conf / Next above 0x4000, plain POINTER banks, counter video
            tsFastTick(4);
            if (__builtin_expect(g_ts_memcyc != 0, 0) && !tsMemNoDram(REG_PC)) tsFastTick(TsConf::cpuMemMiss(REG_PC, true));   // DRAM model: a cache miss
            opCode = MemESP::ramCurrent[pg][REG_PC & 0x1fff];
        } else if (g_ts_fastrd == 2) {
            // Next below 0x4000 (TS-Conf is fully served by the branch above):
            // ROM, MMU RAM or the divMMC window, all plain pointers in
            // ramCurrent[0/1]. The automap hooks may re-plug the slot, so the
            // load follows them.
            tsFastTick(4);
            if (divmmc) DivMMC::preOpcFetch(REG_PC);
            pg = REG_PCh >> 5;
            opCode = MemESP::ramCurrent[pg][REG_PC & 0x1fff];
            if (divmmc) DivMMC::postOpcFetch();
        } else {
        VIDEO::Draw_Opcode(MemESP::ramContended[pg]);
        if (__builtin_expect(g_ts_memcyc != 0, 0) && !tsMemNoDram(REG_PC)) {   // TS-Conf DRAM model (TsConf.cpp)
            const uint32_t w = TsConf::cpuMemMiss(REG_PC, true);
            if (w) VIDEO::Draw(w, false);
        }
        if (divmmc) {
            DivMMC::preOpcFetch(REG_PC);
            // Fetch opcode from currently mapped memory
            pg = REG_PCh >> 5; // re-read in case 0x3Dxx instant map changed it
            if (pg <= 1 && MemESP::divmmc_mapped) {
                opCode = (pg == 0) ? MemESP::page0_lo[REG_PC] : MemESP::page0_hi[REG_PC & 0x1FFF];
            } else {
                opCode = MemESP::romPeek(pg, MemESP::ramCurrent[pg], REG_PC);
            }
            DivMMC::postOpcFetch();
        } else if (pg <= 1 && MemESP::divmmc_mapped) {
            opCode = (REG_PC < 0x2000) ? MemESP::page0_lo[REG_PC] : MemESP::page0_hi[REG_PC & 0x1FFF];
        } else if (__builtin_expect(g_timex_mmu != 0, 0) && Timex::rd[REG_PC >> 13]) {
            // Timex TC2068 SCLD window (Timex.h). This fetch is a SECOND copy of
            // Z80Ops::fetchOpcode() and it is the one that matters: exec_nocheck
            // runs every instruction outside the INT window, so hooking only
            // fetchOpcode() left the machine executing the HOME ROM the moment it
            // jumped into the EX-ROM — which its own boot does (HOME 0x0E05:
            // LD HL,0x08E7 / CALL 0x6815 -> pages the EX-ROM in and JP (HL)).
            // hw 2026-09-12: that is why the TC2068 reached its copyright screen
            // and then fell apart on its own with no input.
            opCode = Timex::read8(REG_PC);
        } else
        opCode = MemESP::romPeek(pg, MemESP::ramCurrent[pg], REG_PC);
        }
        C16_FE(REG_PC);

        regR++;
        REG_PC++;
#if PERF_TRACE && PERF_HIST
        z80_op_hist[opCode]++;
        if (Z80Ops::isTsconf) {
            extern uint32_t ts_page_hist[257];
            const uintptr_t bp = (uintptr_t)MemESP::ramCurrent[pg];
            ts_page_hist[(bp >= 0x10000000u && bp < 0x11000000u) ? 256 : TsConf::r.page[pg >> 1]]++;
        }
#endif

        if (prefixOpcode == 0) {
            flagQ = pendingEI = false;
            dcOpcode[opCode]();
            lastFlagQ = flagQ;
            continue;
        }

        if (prefixOpcode == 0xDD) {
            prefixOpcode = 0;
            decodeDDFD(regIX);
        } else if (prefixOpcode == 0xED) {
            prefixOpcode = 0;
            decodeED();
        } else if (prefixOpcode == 0xFD) {
            prefixOpcode = 0;
            decodeDDFD(regIY);
        } else continue;

        if (prefixOpcode == 0) lastFlagQ = flagQ;

    }
    z80_in_nocheck = false;

}

// LDIR/LDDR block fast path: run every iteration but the LAST as one block copy,
// then let the normal ldx(1)/ldx(-1) do the final one so flags, WZ and the exit
// timing are exactly the core's. Each batched iteration accounts 21 T (16 + the
// 5-T repeat) and two opcode fetches (R += 2); WZ = the repeat opcode's address,
// as the repeat path sets it. Only inside exec_nocheck (z80_in_nocheck) and
// never across CPU::stFrame, where the INT is sampled. What may NOT be batched,
// because the per-byte path has a side effect the batch would skip:
//  - page 0 (ROM/overlays/DivMMC/NeoGS ZX-DMA window) as source or destination,
//  - contended pages (the contention is per access) and the ULA snow machine,
//  - a destination in the page the beam renderer reads (grmem, the DS80 pair,
//    GMX 640x200, the 16col planes): those bytes must land per beam position,
//  - memory breakpoints, a ROM destination, an accessor (SPI-PSRAM) bank,
//  - TS-Conf: the FMAddr/W0_WE write gate (g_tsconf_wr).
// Beam-raced machines take at most one video line per batch (Draw() handles a
// single line crossing per call); the TS fast path (TsFastMem.h) has no such
// limit. TMNT: 21% of its instructions are LDIR iterations (hw 2026-09-07).
// LDIR/LDDR batching (Z80::blockRepeat) was REMOVED on 2026-09-09 after being
// measured a net LOSS on the machine it was written for. It ran the repeated
// iterations as one memcpy bounded by CPU::stFrame, and on TS-Conf that meant
// one VIDEO::tsRenderDrainOverlap over the WHOLE batch range instead of one
// byte at a time: a wide range overlaps far more lines the core1 renderer has
// not drawn yet, so core0 stalled waiting for it. Measured on fishbone at
// 14 MHz, same scene (identical c1/base/tsu): cpu 21.6 -> 19.9 ms, core1 wait
// 4.4 -> 3.7 ms, realFPS 43.5 -> 47.1 WITH THE BATCHING OFF. It also had no
// effect whatsoever on the hang it was suspected of (see the fishbone section).
// If it is ever reinstated, the drain is the thing to fix first, and it must be
// re-measured per machine — the win it was introduced for was never isolated
// from the other changes in its commit.

NX_DISP_INL void Z80::decodeOpcode00()
{ /* NOP */
}

NX_DISP_INL void Z80::decodeOpcode01()
{ /* LD BC,nn */
    REG_BC = Z80Ops::peek16(REG_PC);
    REG_PC = REG_PC + 2;
}

NX_DISP_INL void Z80::decodeOpcode02()
{ /* LD (BC),A */
    Z80Ops::poke8(REG_BC, regA);
    REG_W = regA;
    REG_Z = REG_C + 1;
    //REG_WZ = (regA << 8) | (REG_C + 1);
}

NX_DISP_INL void Z80::decodeOpcode03()
{ /* INC BC */
    Z80Ops::addressOnBusIR(2);
    REG_BC++;
}



NX_DISP_INL void Z80::decodeOpcode06()
{ /* LD B,n */
    REG_B = Z80Ops::peek8(REG_PC);
    // PEEK8(REG_B,REG_PC);
    REG_PC++;
}

NX_DISP_INL void Z80::decodeOpcode07()
{ /* RLCA */
    carryFlag = (regA > 0x7f);
    regA <<= 1;
    if (carryFlag) {
        regA |= CARRY_MASK;
    }
    sz5h3pnFlags = (sz5h3pnFlags & FLAG_SZP_MASK) | (regA & FLAG_53_MASK);
    flagQ = true;
}

NX_DISP_INL void Z80::decodeOpcode08()
{ /* EX AF,AF' */
    uint8_t work8 = regA;
    regA = REG_Ax;
    REG_Ax = work8;

    work8 = getFlags();
    setFlags(REG_Fx);
    REG_Fx = work8;
}

NX_DISP_INL void Z80::decodeOpcode09()
{ /* ADD HL,BC */
    Z80Ops::addressOnBusIR(7);
    add16(regHL, REG_BC);
}

NX_DISP_INL void Z80::decodeOpcode0a()
{ /* LD A,(BC) */
    regA = Z80Ops::peek8(REG_BC);
    // PEEK8(regA,REG_BC);
    REG_WZ = REG_BC + 1;
}

NX_DISP_INL void Z80::decodeOpcode0b()
{ /* DEC BC */
    Z80Ops::addressOnBusIR(2);
    REG_BC--;

}



NX_DISP_INL void Z80::decodeOpcode0e()
{ /* LD C,n */
    REG_C = Z80Ops::peek8(REG_PC);
    // PEEK8(REG_C,REG_PC);
    REG_PC++;
}

NX_DISP_INL void Z80::decodeOpcode0f()
{ /* RRCA */
    carryFlag = (regA & CARRY_MASK) != 0;
    regA >>= 1;
    if (carryFlag) {
        regA |= SIGN_MASK;
    }
    sz5h3pnFlags = (sz5h3pnFlags & FLAG_SZP_MASK) | (regA & FLAG_53_MASK);
    flagQ = true;
}

NX_DISP_INL void Z80::decodeOpcode10()
//         case 0x10:
{ /* DJNZ e */
    Z80Ops::addressOnBusIR(1);
    int8_t offset = Z80Ops::peek8(REG_PC);
    // PEEK8(int8_t offset,REG_PC);
    if (--REG_B != 0) {
        Z80Ops::addressOnBus(REG_PC, 5);
        REG_PC = REG_WZ = REG_PC + offset + 1;
    } else {
        REG_PC++;
    }
}

NX_DISP_INL void Z80::decodeOpcode11()
//         case 0x11:
{ /* LD DE,nn */
    REG_DE = Z80Ops::peek16(REG_PC);
    REG_PC = REG_PC + 2;
}

NX_DISP_INL void Z80::decodeOpcode12()
//         case 0x12:
{ /* LD (DE),A */
    Z80Ops::poke8(REG_DE, regA);
    REG_W = regA;
    REG_Z = REG_E + 1;
    //REG_WZ = (regA << 8) | (REG_E + 1);
}

NX_DISP_INL void Z80::decodeOpcode13()
//         case 0x13:
{ /* INC DE */
    Z80Ops::addressOnBusIR(2);
    REG_DE++;
}



NX_DISP_INL void Z80::decodeOpcode16()
//         case 0x16:
{ /* LD D,n */
    REG_D = Z80Ops::peek8(REG_PC);
    // PEEK8(REG_D,REG_PC);
    REG_PC++;
}

NX_DISP_INL void Z80::decodeOpcode17()
//         case 0x17:
{ /* RLA */
    bool oldCarry = carryFlag;
    carryFlag = regA > 0x7f;
    regA <<= 1;
    if (oldCarry) {
        regA |= CARRY_MASK;
    }
    sz5h3pnFlags = (sz5h3pnFlags & FLAG_SZP_MASK) | (regA & FLAG_53_MASK);
    flagQ = true;
}

NX_DISP_INL void Z80::decodeOpcode18()
//         case 0x18:
{ /* JR e */
    int8_t offset = Z80Ops::peek8(REG_PC);
    // PEEK8(int8_t offset,REG_PC);
    Z80Ops::addressOnBus(REG_PC, 5);
    REG_PC = REG_WZ = REG_PC + offset + 1;

}

NX_DISP_INL void Z80::decodeOpcode19()
//         case 0x19:
{ /* ADD HL,DE */
    Z80Ops::addressOnBusIR(7);
    add16(regHL, REG_DE);
}

NX_DISP_INL void Z80::decodeOpcode1a()
//         case 0x1A:
{ /* LD A,(DE) */
    regA = Z80Ops::peek8(REG_DE);
    // PEEK8(regA,REG_DE);
    REG_WZ = REG_DE + 1;
}

NX_DISP_INL void Z80::decodeOpcode1b()
//         case 0x1B:
{ /* DEC DE */
    Z80Ops::addressOnBusIR(2);
    REG_DE--;
}



NX_DISP_INL void Z80::decodeOpcode1e()
//         case 0x1E:
{ /* LD E,n */
    REG_E = Z80Ops::peek8(REG_PC);
    // PEEK8(REG_E,REG_PC);
    REG_PC++;
}

NX_DISP_INL void Z80::decodeOpcode1f()
//         case 0x1F:
{ /* RRA */
    bool oldCarry = carryFlag;
    carryFlag = (regA & CARRY_MASK) != 0;
    regA >>= 1;
    if (oldCarry) {
        regA |= SIGN_MASK;
    }
    sz5h3pnFlags = (sz5h3pnFlags & FLAG_SZP_MASK) | (regA & FLAG_53_MASK);
    flagQ = true;
}

NX_DISP_INL void Z80::decodeOpcode20()
{ /* JR NZ,e */
    int8_t offset = Z80Ops::peek8(REG_PC);
    // PEEK8(int8_t offset,REG_PC);
    if ((sz5h3pnFlags & ZERO_MASK) == 0) {
        Z80Ops::addressOnBus(REG_PC, 5);
        REG_PC += offset;
        REG_WZ = REG_PC + 1;
        REG_PC++;
        if (Z80Ops::isNext && offset < 0) nextIdleNote();
        return;
    }
    REG_PC++;
}

NX_DISP_INL void Z80::decodeOpcode21()
{ /* LD HL,nn */
    REG_HL = Z80Ops::peek16(REG_PC);
    REG_PC = REG_PC + 2;
}

NX_DISP_INL void Z80::decodeOpcode22()
{ /* LD (nn),HL */
    REG_WZ = Z80Ops::peek16(REG_PC);
    Z80Ops::poke16(REG_WZ, regHL);
    REG_WZ++;
    REG_PC = REG_PC + 2;
}

NX_DISP_INL void Z80::decodeOpcode23()
{ /* INC HL */
    Z80Ops::addressOnBusIR(2);
    REG_HL++;
}



NX_DISP_INL void Z80::decodeOpcode26()
{ /* LD H,n */
    REG_H = Z80Ops::peek8(REG_PC);
    // PEEK8(REG_H, REG_PC);
    REG_PC++;
}

NX_DISP_INL void Z80::decodeOpcode27()
{ /* DAA */
    daa();
}

NX_DISP_INL void Z80::decodeOpcode28()
{ /* JR Z,e */
    int8_t offset = Z80Ops::peek8(REG_PC);
    // PEEK8(int8_t offset, REG_PC);
    if ((sz5h3pnFlags & ZERO_MASK) != 0) {
        Z80Ops::addressOnBus(REG_PC, 5);
        REG_PC += offset;
        REG_WZ = REG_PC + 1;
        REG_PC++;
        if (Z80Ops::isNext && offset < 0) nextIdleNote();
        return;
    }
    REG_PC++;
}

NX_DISP_INL void Z80::decodeOpcode29()
{ /* ADD HL,HL */
    Z80Ops::addressOnBusIR(7);
    add16(regHL, REG_HL);
}

NX_DISP_INL void Z80::decodeOpcode2a()
{ /* LD HL,(nn) */
    REG_WZ = Z80Ops::peek16(REG_PC);
    REG_HL = Z80Ops::peek16(REG_WZ);
    REG_WZ++;
    REG_PC = REG_PC + 2;
}

NX_DISP_INL void Z80::decodeOpcode2b()
{ /* DEC HL */
    Z80Ops::addressOnBusIR(2);
    REG_HL--;
}



NX_DISP_INL void Z80::decodeOpcode2e()
{ /* LD L,n */
    REG_L = Z80Ops::peek8(REG_PC);
    // PEEK8(REG_L, REG_PC);
    REG_PC++;
}

NX_DISP_INL void Z80::decodeOpcode2f()
{ /* CPL */
    regA ^= 0xff;
    sz5h3pnFlags = (sz5h3pnFlags & FLAG_SZP_MASK) | HALFCARRY_MASK
            | (regA & FLAG_53_MASK) | ADDSUB_MASK;
    flagQ = true;
}

NX_DISP_INL void Z80::decodeOpcode30()
{ /* JR NC,e */
    int8_t offset = Z80Ops::peek8(REG_PC);
    // PEEK8(int8_t offset, REG_PC);
    if (!carryFlag) {
        Z80Ops::addressOnBus(REG_PC, 5);
        REG_PC += offset;
        REG_WZ = REG_PC + 1;
        REG_PC++;
        // Atic Atac Remake's intro pause: LD HL,(nn) / XOR A / SBC HL,BC / JR C
        if (Z80Ops::isNext && offset < 0) nextIdleNote();
        return;
    }
    REG_PC++;
}

NX_DISP_INL void Z80::decodeOpcode31()
{ /* LD SP,nn */
    REG_SP = Z80Ops::peek16(REG_PC);
    REG_PC = REG_PC + 2;
}

NX_DISP_INL void Z80::decodeOpcode32()
{ /* LD (nn),A */
    REG_WZ = Z80Ops::peek16(REG_PC);
    Z80Ops::poke8(REG_WZ, regA);
    REG_WZ = (regA << 8) | ((REG_WZ + 1) & 0xff);
    REG_PC = REG_PC + 2;
}

NX_DISP_INL void Z80::decodeOpcode33()
{ /* INC SP */
    Z80Ops::addressOnBusIR(2);
    REG_SP++;
}



NX_DISP_INL void Z80::decodeOpcode36()
{ /* LD (HL),n */
    // PEEK8(uint8_t value, REG_PC);
    // Z80Ops::poke8(REG_HL, value);
    Z80Ops::poke8(REG_HL, Z80Ops::peek8(REG_PC));
    REG_PC++;
}

NX_DISP_INL void Z80::decodeOpcode37()
{ /* SCF */
    uint8_t regQ = lastFlagQ ? sz5h3pnFlags : 0;
    carryFlag = true;
    sz5h3pnFlags = (sz5h3pnFlags & FLAG_SZP_MASK) | (((regQ ^ sz5h3pnFlags) | regA) & FLAG_53_MASK);
    flagQ = true;
}

NX_DISP_INL void Z80::decodeOpcode38()
{ /* JR C,e */
    int8_t offset = Z80Ops::peek8(REG_PC);
    // PEEK8(int8_t offset, REG_PC);
    if (carryFlag) {
        Z80Ops::addressOnBus(REG_PC, 5);
        REG_PC += offset;
        REG_WZ = REG_PC + 1;
        REG_PC++;
        // Atic Atac Remake's intro pause: LD HL,(nn) / XOR A / SBC HL,BC / JR C
        if (Z80Ops::isNext && offset < 0) nextIdleNote();
        return;
    }
    REG_PC++;
}

NX_DISP_INL void Z80::decodeOpcode39()
{ /* ADD HL,SP */
    Z80Ops::addressOnBusIR(7);
    add16(regHL, REG_SP);
}

NX_DISP_INL void Z80::decodeOpcode3a()
{ /* LD A,(nn) */
    REG_WZ = Z80Ops::peek16(REG_PC);
    regA = Z80Ops::peek8(REG_WZ);
    // PEEK8( regA, REG_WZ);
    REG_WZ++;
    REG_PC = REG_PC + 2;
}

NX_DISP_INL void Z80::decodeOpcode3b()
{ /* DEC SP */
    Z80Ops::addressOnBusIR(2);
    REG_SP--;
}



NX_DISP_INL void Z80::decodeOpcode3e()
{ /* LD A,n */
    regA = Z80Ops::peek8(REG_PC);
    // PEEK8(regA, REG_PC);
    REG_PC++;
}

NX_DISP_INL void Z80::decodeOpcode3f()
{ /* CCF */
    uint8_t regQ = lastFlagQ ? sz5h3pnFlags : 0;
    sz5h3pnFlags = (sz5h3pnFlags & FLAG_SZP_MASK) | (((regQ ^ sz5h3pnFlags) | regA) & FLAG_53_MASK);
    if (carryFlag) {
        sz5h3pnFlags |= HALFCARRY_MASK;
    }
    carryFlag = !carryFlag;
    flagQ = true;
}



























































void Z80::decodeOpcode76()
{ /* HALT */

    halted = true;

    // Signal HALT to CPU Loop
    CPU::stFrame = 0;

}









































/*IRAM_ATTR*/ void Z80::decodeOpcodebf()
{ /* CP A */

    cp(regA);

    // Spectrum Next: NextZXOS's tape trap declining (no .tapein file attached —
    // the NextZXOS menu's "Tape Loader") repeats the CP A in divMMC RAM and
    // returns to ROM3 at #056B, past our in-ROM trap. See next_os_tape_decline().
    if (__builtin_expect(Z80Ops::isNext, 0) && MemESP::divmmc_mapped &&
        next_os_tape_decline()) return;

    // LOAD trap: CP A in ROM LD-BYTES routine
    // PC after fetch depends on ROM variant:
    //   0x056B = Spanish 48K ROM (CP A at 0x056A) -> RET at 0x05E2
    //   0x056D = Byte 48K ROM    (CP A at 0x056C) -> RET at 0x05E4
    //   0x057D = Sinclair 48K ROM(CP A at 0x057C) -> RET at 0x0606
    // ROM FlashLoad traps. These are bare PC values, so they are only meaningful
    // while a ROM whose LD-BYTES lives there is paged in — which on a TC2068 the
    // HOME ROM is not (0x056B there is the middle of an unrelated routine).
    //
    // The TC2068 gets its OWN trap instead, because its loader is the same routine
    // relocated into the EX-ROM by 0x045A: CP A at 0x0110 (so PC after fetch
    // 0x0111, against the Sinclair 0x056B) and the RET after `LD A,H / CP 0x01` at
    // 0x0188 (against 0x05E2). 157 of the 202 bytes of LD-BYTES + LD-EDGE are
    // byte-identical to the Sinclair ROM's and the two only diverge past that exit.
    // Gated on the SCLD actually having the EX-ROM in slot 0, so HOME ROM code at
    // 0x0111 can never trigger it — and that gate is what makes a bare PC value
    // safe on a machine whose page 0 is not one fixed ROM.
    const bool tcTrap = __builtin_expect(g_timex_mmu != 0, 0) &&
                        Timex::exromSel && Timex::rd[0] && REG_PC == 0x0111;
#if TIMEX_PORT_TRACE
    // The trap is a bare PC value, so the FIRST question is whether the ROM reaches
    // it at all and with what paged — not whether the trap body is right. Log every
    // CP A executed in ROM space on this machine (there are few, and they collapse),
    // so a trap that never fires says WHY: wrong address, EX-ROM not mapped, or the
    // routine never entered.
    if (Z80Ops::isTc2068 && REG_PC < 0x4000) {
        static uint32_t budget = 40; static uint16_t last = 0xFFFF; static uint32_t runs = 0;
        if (REG_PC == last) runs++;
        else if (budget) {
            if (runs) { Debug::log("[TMXLD]   ... x%u", (unsigned)runs); runs = 0; }
            last = REG_PC; budget--;
            Debug::log("[TMXLD] CP A pc=%04X mmu=%u ex=%d rd0=%p hsr=%02X trap=%d "
                       "ftype=%d name=%s fl=%d st=%d blk=%d",
                       (unsigned)REG_PC, (unsigned)g_timex_mmu, (int)Timex::exromSel,
                       (void*)Timex::rd[0], (unsigned)Timex::hsr, (int)tcTrap,
                       (int)Tape::tapeFileType, Tape::tapeFileName.c_str(),
                       (int)Config::flashload, (int)Tape::tapeStatus,
                       (int)Tape::tapeCurBlock);
        }
    }
#endif
    // On the Next, NextZXOS's own tape trap (automap at #056A) gets the block
    // whenever it is armed — see NextReg::osTapeTrapArmed(). BIOS personalities
    // without it (48K/128K/Pentagon, divmmc off) keep our fast load.
    if ((((REG_PC == 0x56b || REG_PC == 0x56d || REG_PC == 0x57d)) && !Z80Ops::isTc2068 && !Z80Ops::isSam
         && !(Z80Ops::isNext && NextReg::osTapeTrapArmed()))
        || tcTrap) {

        if ((Tape::tapeFileType == TAPE_FTYPE_TAP || Tape::tapeFileType == TAPE_FTYPE_TZX || Tape::tapeFileType == TAPE_FTYPE_PZX) && (Tape::tapeFileName != "none")) {
              // Skip ROM FlashLoad while JJ screen animation is in progress —
              // the loader's edge detection timeout can briefly return to ROM,
              // and we must not let ROM FlashLoad consume tape blocks.
              // Tape wear (Config::tape_wear) ignores fast load: the trap fills the
              // block straight out of the file without generating a single pulse, so
              // a chewed tape would always load perfectly. See Tape.cpp fastLoadOn().
              if (Config::flashload && !Config::tape_wear && !Rzx::mode && !Tape::jjScreenAnimating) {
                // Save return PC before FlashLoad (it doesn't modify REG_PC)
                uint16_t trapPC = REG_PC;
                const bool flOk = Tape::FlashLoad();
#if TIMEX_PORT_TRACE
                if (tcTrap)
                    Debug::log("[TMXLD] FlashLoad -> %d  blk=%d/%d  IX=%04X DE=%04X A'=%02X",
                               (int)flOk, (int)Tape::tapeCurBlock, (int)Tape::tapeNumBlocks,
                               (unsigned)REG_IX, (unsigned)REG_DE, (unsigned)REG_Ax);
#endif
                if (flOk) {
                    // Stop tape if it was auto-started.
                    // Preserve tapePhase if pzxFlashCont is set (partial PZX load
                    // set up DATA1 phase for real-mode continuation by auto-start).
                    if (Tape::tapeStatus == TAPE_LOADING && !Tape::tapFlashCont) {
                        Tape::tapeStatus = TAPE_STOPPED;
                        if (!Tape::pzxFlashCont)
                            Tape::tapePhase = TAPE_PHASE_STOPPED;
                    }
                    // Jump to RET after CP 0x01 in the active ROM's LD-BYTES
                    if (tcTrap)               REG_PC = 0x0188; // TC2068 EX-ROM
                    else if (trapPC == 0x56d) REG_PC = 0x5e4; // Byte ROM
                    else if (trapPC == 0x57d) REG_PC = 0x606; // Sinclair ROM
                    else                      REG_PC = 0x5e2; // Spanish ROM
                }
            }
        }
    }
}


NX_DISP_INL void Z80::decodeOpcodec1()
{ /* POP BC */
    REG_BC = pop();

}


NX_DISP_INL void Z80::decodeOpcodec3()
{ /* JP nn */
    REG_WZ = REG_PC = Z80Ops::peek16(REG_PC);

    check_trdos();

}


NX_DISP_INL void Z80::decodeOpcodec5()
{ /* PUSH BC */
    Z80Ops::addressOnBusIR(1);
    push(REG_BC);
}

NX_DISP_INL void Z80::decodeOpcodec6()
{ /* ADD A,n */

    // PEEK8(uint8_t value,REG_PC);
    // add(value);

    add(Z80Ops::peek8(REG_PC));

    REG_PC++;

}



NX_DISP_INL void Z80::decodeOpcodec9()
{ /* RET */
    REG_PC = REG_WZ = pop();

    check_trdos();
    // check_trdos_unpage();

}


void Z80::decodeOpcodecb()
{ /* 0xCB prefix */
    opCode = NextOpcode::fetch();
    REG_PC++;
    regR++;
    decodeCB();
}


NX_DISP_INL void Z80::decodeOpcodecd()
{ /* CALL nn */
    REG_WZ = Z80Ops::peek16(REG_PC);
    Z80Ops::addressOnBus(REG_PC + 1, 1);
    push(REG_PC + 2);
    REG_PC = REG_WZ;

    check_trdos();

}

NX_DISP_INL void Z80::decodeOpcodece()
{ /* ADC A,n */

    // PEEK8(uint8_t value,REG_PC);
    // adc(value);

    adc(Z80Ops::peek8(REG_PC));
    REG_PC++;
}



NX_DISP_INL void Z80::decodeOpcoded1()
{ /* POP DE */
    REG_DE = pop();
}


void Z80::decodeOpcoded3()
{ /* OUT (n),A */
    uint8_t work8 = Z80Ops::peek8(REG_PC);
    // PEEK8(uint8_t work8,REG_PC);
    REG_PC++;
    REG_WZ = regA << 8;
    Ports::output(REG_WZ | work8, regA);
    REG_WZ |= (work8 + 1);
}


NX_DISP_INL void Z80::decodeOpcoded5()
{ /* PUSH DE */
    Z80Ops::addressOnBusIR(1);
    push(REG_DE);
}

NX_DISP_INL void Z80::decodeOpcoded6()
{ /* SUB n */
    // PEEK8(uint8_t value,REG_PC);
    // sub(value);

    sub(Z80Ops::peek8(REG_PC));
    REG_PC++;
}



NX_DISP_INL void Z80::decodeOpcoded9()
{ /* EXX */
    uint16_t tmp;
    tmp = REG_BC;
    REG_BC = REG_BCx;
    REG_BCx = tmp;

    tmp = REG_DE;
    REG_DE = REG_DEx;
    REG_DEx = tmp;

    tmp = REG_HL;
    REG_HL = REG_HLx;
    REG_HLx = tmp;
}


void Z80::decodeOpcodedb()
{ /* IN A,(n) */
    REG_W = regA;
    REG_Z = Z80Ops::peek8(REG_PC);
    // PEEK8(REG_Z,REG_PC);
    //REG_WZ = (regA << 8) | Z80Ops::peek8(REG_PC);
    REG_PC++;
    // if (REG_PC == 0x60BC) printf("IN A,(n). Adress -> %04x\n", REG_WZ);
    regA = Ports::input(REG_WZ);
    if (__builtin_expect(Rzx::mode != 0, 0)) regA = Rzx::onIn(regA);
    REG_WZ++;
    // ROM 2's token poll (0x1928) and enNxtmmc's (0x1F40): see nextSdPollBatch
    if (Z80Ops::isNext && regA == 0xFF && (REG_PC == 0x192A || REG_PC == 0x1F42)) nextSdPollBatch();
}


void Z80::decodeOpcodedd()
{ /* Subconjunto de instrucciones */
    // // opCode = Z80Ops::fetchOpcode(REG_PC++);
    // uint8_t pg = REG_PC >> 14;
    // VIDEO::Draw_Opcode(MemESP::ramContended[pg]);
    // opCode = MemESP::ramCurrent[pg][REG_PC & 0x3fff];
    // // FETCH_OPCODE(opCode,REG_PC);

    // uint8_t pg = REG_PC >> 14;
    // opCode = MemESP::ramCurrent[pg][REG_PC & 0x3fff];
    // if (MemESP::ramContended[pg]) {
    //     MemESP::lastContendedMemReadWrite = opCode;
    //     VIDEO::Draw_Opcode(true);
    // } else {
    //     VIDEO::Draw_Opcode(false);
    // };

    opCode = NextOpcode::fetch();

    REG_PC++;
    regR++;
    decodeDDFD(regIX);
}

NX_DISP_INL void Z80::decodeOpcodede()
{ /* SBC A,n */

    // PEEK8(uint8_t value,REG_PC);
    // sbc(value);

    sbc(Z80Ops::peek8(REG_PC));
    REG_PC++;
}



NX_DISP_INL void Z80::decodeOpcodee1() /* POP HL */
{
    REG_HL = pop();
}


NX_DISP_INL void Z80::decodeOpcodee3()
{ /* EX (SP),HL */
    // Instrucción de ejecución sutil.
    RegisterPair work = regHL;
    REG_HL = Z80Ops::peek16(REG_SP);
    Z80Ops::addressOnBus(REG_SP + 1, 1);
    // No se usa poke16 porque el Z80 escribe los bytes AL REVES
    Z80Ops::poke8(REG_SP + 1, work.byte8.hi);
    Z80Ops::poke8(REG_SP, work.byte8.lo);
    Z80Ops::addressOnBus(REG_SP, 2);
    REG_WZ = REG_HL;
}


NX_DISP_INL void Z80::decodeOpcodee5() /* PUSH HL */
{
    Z80Ops::addressOnBusIR(1);
    push(REG_HL);
}

NX_DISP_INL void Z80::decodeOpcodee6() /* AND n */
{
    // PEEK8(uint8_t value,REG_PC);
    // and_(value);

    and_(Z80Ops::peek8(REG_PC));
    REG_PC++;
}



NX_DISP_INL void Z80::decodeOpcodee9() /* JP (HL) */
{
    REG_PC = REG_HL;

    check_trdos();

}


NX_DISP_INL void Z80::decodeOpcodeeb()
{ /* EX DE,HL */
    uint16_t tmp = REG_HL;
    REG_HL = REG_DE;
    REG_DE = tmp;
}


void Z80::decodeOpcodeed() /*Subconjunto de instrucciones*/
{
    // // opCode = Z80Ops::fetchOpcode(REG_PC++);
    // uint8_t pg = REG_PC >> 14;
    // VIDEO::Draw_Opcode(MemESP::ramContended[pg]);
    // opCode = MemESP::ramCurrent[pg][REG_PC & 0x3fff];
    // // FETCH_OPCODE(opCode,REG_PC);

    // uint8_t pg = REG_PC >> 14;
    // opCode = MemESP::ramCurrent[pg][REG_PC & 0x3fff];
    // if (MemESP::ramContended[pg]) {
    //     MemESP::lastContendedMemReadWrite = opCode;
    //     VIDEO::Draw_Opcode(true);
    // } else {
    //     VIDEO::Draw_Opcode(false);
    // };

    opCode = NextOpcode::fetch();

    REG_PC++;
    regR++;
    decodeED();
}

NX_DISP_INL void Z80::decodeOpcodeee() /* XOR n */
{

    // PEEK8(uint8_t value,REG_PC);
    // xor_(value);

    xor_(Z80Ops::peek8(REG_PC));
    REG_PC++;
}



// Byte ROM LOAD trap, cold half (flash): see decodeOpcodef1. Returns true when
// FlashLoad took the block and the POP AF / RET has been emulated.
// ZX-Evo BaseConf, zxevo_fe.rom page 28 (the 48 BASIC ERS boots): LD-BYTES is the
// Sinclair routine except that #0569 holds RST 8 / DEFB #45 — a hook into the ROM's
// #3C97 extension — instead of LD C,A / CP A. Do what those two did and run the same
// FlashLoad the CP A trap does; on success leave by the RET at #05E2 (identical to the
// Sinclair ROM's). Anything else (no tape, fast load off, RAM there) takes the RST.
Z80_COLD bool Z80::evo_tape_trap() {
    if (!Atm::evo || !(g_atm_ro & 1) || MemESP::ramCurrent[0][0x056A] != 0x45) return false;
    if (!(Config::flashload && !Config::tape_wear && !Rzx::mode && !Tape::jjScreenAnimating &&
          (Tape::tapeFileType == TAPE_FTYPE_TAP || Tape::tapeFileType == TAPE_FTYPE_TZX ||
           Tape::tapeFileType == TAPE_FTYPE_PZX) && Tape::tapeFileName != "none")) return false;
    const uint8_t a = regA; const uint8_t f = getFlags(); const uint8_t c = REG_C;
    REG_C = regA;            // LD C,A
    cp(regA);                // CP A
    if (Tape::FlashLoad()) {
        if (Tape::tapeStatus == TAPE_LOADING && !Tape::tapFlashCont) {
            Tape::tapeStatus = TAPE_STOPPED;
            if (!Tape::pzxFlashCont) Tape::tapePhase = TAPE_PHASE_STOPPED;
        }
        REG_PC = 0x05E2;
        return true;
    }
    regA = a; setFlags(f); REG_C = c;
    return false;
}

// Spectrum Next, NextZXOS's divMMC tape trap (automap at #056A, handler at
// divMMC RAM #387F in NextZXOS 2.09) with NO .tapein file attached — the menu's
// "Tape Loader": it restores the border, repeats the ROM's CP A and leaves for
// the real tape via `PUSH #056B / JP #1FF9` (the divMMC off-zone RET), i.e.
// back into ROM3 LD-BYTES past the CP A our in-ROM trap sits on. That CP A
// followed by PUSH #056B (ED 8A 05 6B) occurs nowhere else in the divMMC
// memory, and the .tapein branch never takes it — so it means "the OS gave the
// load to the tape deck", which is ours. FlashLoad the block like the 48K trap
// and leave by the OS's own exit, returning to ROM3's RET at #05E2 instead of
// #056B. Registers are the ROM's (IX/DE/A' untouched by the handler).
Z80_COLD bool Z80::next_os_tape_decline() {
    const uint16_t pc = REG_PC;
    if (pc >= 0x4000 ||
        MemESP::readbyte(pc) != 0xED || MemESP::readbyte(pc + 1) != 0x8A ||
        MemESP::readbyte(pc + 2) != 0x05 || MemESP::readbyte(pc + 3) != 0x6B ||
        MemESP::readbyte(pc + 4) != 0xC3) return false;
    if (!(Config::flashload && !Config::tape_wear && !Rzx::mode && !Tape::jjScreenAnimating &&
          (Tape::tapeFileType == TAPE_FTYPE_TAP || Tape::tapeFileType == TAPE_FTYPE_TZX ||
           Tape::tapeFileType == TAPE_FTYPE_PZX) && Tape::tapeFileName != "none")) return false;
    if (!Tape::FlashLoad()) return false;
    if (Tape::tapeStatus == TAPE_LOADING && !Tape::tapFlashCont) {
        Tape::tapeStatus = TAPE_STOPPED;
        if (!Tape::pzxFlashCont) Tape::tapePhase = TAPE_PHASE_STOPPED;
    }
    push(0x05E2);   // ROM3 LD-BYTES' RET -> LD-RET (#053F) already on the stack
    REG_PC = (uint16_t)(MemESP::readbyte(pc + 5) | (MemESP::readbyte(pc + 6) << 8));
    return true;
}

Z80_COLD bool Z80::byte_tape_trap() {
    if (!(Config::flashload && !Config::tape_wear && !Rzx::mode &&
        !Tape::jjScreenAnimating &&
        (Tape::tapeFileType == TAPE_FTYPE_TAP || Tape::tapeFileType == TAPE_FTYPE_TZX || Tape::tapeFileType == TAPE_FTYPE_PZX) &&
        Tape::tapeFileName != "none")) return false;
    // Simulate EX AF,AF': swap A/F with A'/F' so FlashLoad sees flag in A'
    uint8_t tmpA = regA;       uint8_t tmpAx = REG_Ax;
    uint8_t tmpF = getFlags(); uint8_t tmpFx = REG_Fx;
    regA = tmpAx;   REG_Ax = tmpA;
    setFlags(tmpFx); REG_Fx = tmpF;

    // For JP 0x0556 (e.g. from JJ hidden code), DE may be stale/zero.
    // FlashLoad uses DE as expected byte count. Set DE to 0xFFFF so
    // FlashLoad loads the full block (it uses min(DE, blockLen)).
    uint16_t origDE = getRegDE();
    if (Tape::FlashLoad()) {
        if (Tape::tapeStatus == TAPE_LOADING && !Tape::tapFlashCont) {
            Tape::tapeStatus = TAPE_STOPPED;
            if (!Tape::pzxFlashCont)
                Tape::tapePhase = TAPE_PHASE_STOPPED;
        }
        // Skip POP AF;RET — pop return address from stack.
        // CALL 0x0556: pops CALL return addr → back to caller
        // JP 0x0556: pops game entry addr → starts game
        REG_PC = pop();
        // Set carry flag = success (standard LD-BYTES convention)
        carryFlag = true;
        return true;
    }
    // FlashLoad failed — restore original A/F and A'/F' and DE
    REG_Ax = regA;   regA = tmpA;
    REG_Fx = getFlags(); setFlags(tmpF);
    setRegDE(origDE);
    return false;
}

NX_DISP_INL void Z80::decodeOpcodef1() /* POP AF */
{
    // Byte ROM LOAD trap: POP AF at 0x0556 (PC=0x0557 after fetch).
    // Byte ROM has POP AF;RET at 0x0556-0x0557 instead of LD-BYTES entry
    // (LD-BYTES is at 0x0558 on Byte ROM, shifted 16 bytes earlier).
    // Two call patterns reach here:
    //   CALL 0x0556 from ROM: stack has CALL return addr (< 0x4000)
    //   JP 0x0556 from user code: stack has game entry addr (>= 0x4000)
    // Both need FlashLoad. After FlashLoad, pop() gives the correct return:
    //   CALL case: pops CALL return addr → back to ROM caller
    //   JP case: pops game entry addr → starts game
    if (REG_PC == 0x557 && Z80Ops::isByte && byte_tape_trap()) return;
    setRegAF(pop());
}


void Z80::decodeOpcodef3() /* DI */
{
    ffIFF1 = ffIFF2 = false;
}


NX_DISP_INL void Z80::decodeOpcodef5() /* PUSH AF */
{
    Z80Ops::addressOnBusIR(1);
    push(getRegAF());
}

NX_DISP_INL void Z80::decodeOpcodef6() /* OR n */
{

    // PEEK8(uint8_t value,REG_PC);
    // or_(value);

    or_(Z80Ops::peek8(REG_PC));
    REG_PC++;
}



NX_DISP_INL void Z80::decodeOpcodef9() /* LD SP,HL */
{
    Z80Ops::addressOnBusIR(2);
    REG_SP = REG_HL;
}


void Z80::decodeOpcodefb() /* EI */
{
#if ZC_PORT_TRACE || ATM_PAGE_TRACE
    if (Z80Ops::isAtm) {
        const uint8_t k = s_ei_i++ & 7;
        s_ei_pc[k] = (uint16_t)(REG_PC - 1);
        s_ei_w0[k] = MemESP::ramCurrent[0];
        s_ei_w1[k] = MemESP::ramCurrent[2];
    }
#endif
    ffIFF1 = ffIFF2 = true;
    pendingEI = true;
    if (Z80Ops::isTsconf) TsConf::intEnableHook();
    if (Z80Ops::isNext && NextInt::ex) NextInt::wake();
}


void Z80::decodeOpcodefd() /* Subconjunto de instrucciones */
{
    // // opCode = Z80Ops::fetchOpcode(REG_PC++);
    // uint8_t pg = REG_PC >> 14;
    // VIDEO::Draw_Opcode(MemESP::ramContended[pg]);
    // opCode = MemESP::ramCurrent[pg][REG_PC & 0x3fff];
    // // FETCH_OPCODE(opCode,REG_PC);

    // uint8_t pg = REG_PC >> 14;
    // opCode = MemESP::ramCurrent[pg][REG_PC & 0x3fff];
    // if (MemESP::ramContended[pg]) {
    //     MemESP::lastContendedMemReadWrite = opCode;
    //     VIDEO::Draw_Opcode(true);
    // } else {
    //     VIDEO::Draw_Opcode(false);
    // };

    opCode = NextOpcode::fetch();

    REG_PC++;
    regR++;
    decodeDDFD(regIY);
}

NX_DISP_INL void Z80::decodeOpcodefe() /* CP n */
{

    // PEEK8(uint8_t value,REG_PC);
    // cp(value);

    cp(Z80Ops::peek8(REG_PC));

    REG_PC++;
}


void (*Z80::dcOpcode[256])() = {
    &decodeOpcode00, &decodeOpcode01, &decodeOpcode02, &decodeOpcode03,
    &decodeINCDEC8, &decodeINCDEC8, &decodeOpcode06, &decodeOpcode07,
    &decodeOpcode08, &decodeOpcode09, &decodeOpcode0a, &decodeOpcode0b,
    &decodeINCDEC8, &decodeINCDEC8, &decodeOpcode0e, &decodeOpcode0f,

    &decodeOpcode10, &decodeOpcode11, &decodeOpcode12, &decodeOpcode13,
    &decodeINCDEC8, &decodeINCDEC8, &decodeOpcode16, &decodeOpcode17,
    &decodeOpcode18, &decodeOpcode19, &decodeOpcode1a, &decodeOpcode1b,
    &decodeINCDEC8, &decodeINCDEC8, &decodeOpcode1e, &decodeOpcode1f,

    &decodeOpcode20, &decodeOpcode21, &decodeOpcode22, &decodeOpcode23,
    &decodeINCDEC8, &decodeINCDEC8, &decodeOpcode26, &decodeOpcode27,
    &decodeOpcode28, &decodeOpcode29, &decodeOpcode2a, &decodeOpcode2b,
    &decodeINCDEC8, &decodeINCDEC8, &decodeOpcode2e, &decodeOpcode2f,

    &decodeOpcode30, &decodeOpcode31, &decodeOpcode32, &decodeOpcode33,
    &decodeINCDEC8, &decodeINCDEC8, &decodeOpcode36, &decodeOpcode37,
    &decodeOpcode38, &decodeOpcode39, &decodeOpcode3a, &decodeOpcode3b,
    &decodeINCDEC8, &decodeINCDEC8, &decodeOpcode3e, &decodeOpcode3f,

    &decodeLD8, &decodeLD8, &decodeLD8, &decodeLD8,
    &decodeLD8, &decodeLD8, &decodeLD8, &decodeLD8,
    &decodeLD8, &decodeLD8, &decodeLD8, &decodeLD8,
    &decodeLD8, &decodeLD8, &decodeLD8, &decodeLD8,

    &decodeLD8, &decodeLD8, &decodeLD8, &decodeLD8,
    &decodeLD8, &decodeLD8, &decodeLD8, &decodeLD8,
    &decodeLD8, &decodeLD8, &decodeLD8, &decodeLD8,
    &decodeLD8, &decodeLD8, &decodeLD8, &decodeLD8,

    &decodeLD8, &decodeLD8, &decodeLD8, &decodeLD8,
    &decodeLD8, &decodeLD8, &decodeLD8, &decodeLD8,
    &decodeLD8, &decodeLD8, &decodeLD8, &decodeLD8,
    &decodeLD8, &decodeLD8, &decodeLD8, &decodeLD8,

    &decodeLD8, &decodeLD8, &decodeLD8, &decodeLD8,
    &decodeLD8, &decodeLD8, &decodeOpcode76, &decodeLD8,
    &decodeLD8, &decodeLD8, &decodeLD8, &decodeLD8,
    &decodeLD8, &decodeLD8, &decodeLD8, &decodeLD8,

    &decodeALU8, &decodeALU8, &decodeALU8, &decodeALU8,
    &decodeALU8, &decodeALU8, &decodeALU8, &decodeALU8,
    &decodeALU8, &decodeALU8, &decodeALU8, &decodeALU8,
    &decodeALU8, &decodeALU8, &decodeALU8, &decodeALU8,

    &decodeALU8, &decodeALU8, &decodeALU8, &decodeALU8,
    &decodeALU8, &decodeALU8, &decodeALU8, &decodeALU8,
    &decodeALU8, &decodeALU8, &decodeALU8, &decodeALU8,
    &decodeALU8, &decodeALU8, &decodeALU8, &decodeALU8,

    &decodeALU8, &decodeALU8, &decodeALU8, &decodeALU8,
    &decodeALU8, &decodeALU8, &decodeALU8, &decodeALU8,
    &decodeALU8, &decodeALU8, &decodeALU8, &decodeALU8,
    &decodeALU8, &decodeALU8, &decodeALU8, &decodeALU8,

    &decodeALU8, &decodeALU8, &decodeALU8, &decodeALU8,
    &decodeALU8, &decodeALU8, &decodeALU8, &decodeALU8,
    &decodeALU8, &decodeALU8, &decodeALU8, &decodeALU8,
    &decodeALU8, &decodeALU8, &decodeALU8, &decodeOpcodebf,

    &decodeRETcc, &decodeOpcodec1, &decodeJPcc, &decodeOpcodec3,
    &decodeCALLcc, &decodeOpcodec5, &decodeOpcodec6, &decodeRST,
    &decodeRETcc, &decodeOpcodec9, &decodeJPcc, &decodeOpcodecb,
    &decodeCALLcc, &decodeOpcodecd, &decodeOpcodece, &decodeRST,

    &decodeRETcc, &decodeOpcoded1, &decodeJPcc, &decodeOpcoded3,
    &decodeCALLcc, &decodeOpcoded5, &decodeOpcoded6, &decodeRST,
    &decodeRETcc, &decodeOpcoded9, &decodeJPcc, &decodeOpcodedb,
    &decodeCALLcc, &decodeOpcodedd, &decodeOpcodede, &decodeRST,

    &decodeRETcc, &decodeOpcodee1, &decodeJPcc, &decodeOpcodee3,
    &decodeCALLcc, &decodeOpcodee5, &decodeOpcodee6, &decodeRST,
    &decodeRETcc, &decodeOpcodee9, &decodeJPcc, &decodeOpcodeeb,
    &decodeCALLcc, &decodeOpcodeed, &decodeOpcodeee, &decodeRST,

    &decodeRETcc, &decodeOpcodef1, &decodeJPcc, &decodeOpcodef3,
    &decodeCALLcc, &decodeOpcodef5, &decodeOpcodef6, &decodeRST,
    &decodeRETcc, &decodeOpcodef9, &decodeJPcc, &decodeOpcodefb,
    &decodeCALLcc, &decodeOpcodefd, &decodeOpcodefe, &decodeRST

};

//Subconjunto de instrucciones 0xCB


// 8-bit register pointer table shared by the regular opcode groups (index r of the
// opcode: B C D E H L (HL) A; nullptr = (HL), the caller does the memory access).
// Read on every LD r,r' / ALU r / INC r / CB op: it MUST live in SRAM with the core —
// a plain `static const` goes to .rodata, which the linker script sends to FLASH
// (the Z80_CORE_IN_RAM rule covers .text only), i.e. one XIP fetch per instruction.
uint8_t* const Z80::reg8[8] __attribute__((section(".time_critical.z80"))) = {
    &regBC.byte8.hi, &regBC.byte8.lo, &regDE.byte8.hi, &regDE.byte8.lo,
    &regHL.byte8.hi, &regHL.byte8.lo, nullptr, &regA };

void Z80::decodeLD8() {   /* LD r,r' 0x40-0x7F (0x76 = HALT keeps its own handler) */
    const uint8_t op = opCode;
    uint8_t* src = reg8[op & 7];
    const uint8_t v = src ? *src : NextMemory::peek8(REG_HL);
    uint8_t* dst = reg8[(op >> 3) & 7];
    if (dst) *dst = v; else Z80Ops::poke8(REG_HL, v);
}

void Z80::decodeALU8() {  /* ADD/ADC/SUB/SBC/AND/XOR/OR/CP A,r 0x80-0xBE (0xBF = CP A keeps the tape LOAD trap) */
    const uint8_t op = opCode;
    uint8_t* src = reg8[op & 7];
    const uint8_t v = src ? *src : NextMemory::peek8(REG_HL);
    switch ((op >> 3) & 7) {
        case 0: add(v); break;
        case 1: adc(v); break;
        case 2: sub(v); break;
        case 3: sbc(v); break;
        case 4: and_(v); break;
        case 5: xor_(v); break;
        case 6: or_(v); break;
        default: cp(v); break;
    }
}

// Condition cc = (opcode >> 3) & 7: NZ Z NC C PO PE P M
bool Z80::condMet(uint8_t cc) {
    bool f;
    switch (cc >> 1) {
        case 0:  f = (sz5h3pnFlags & ZERO_MASK) != 0; break;
        case 1:  f = carryFlag; break;
        case 2:  f = (sz5h3pnFlags & PARITY_MASK) != 0; break;
        default: f = (sz5h3pnFlags & SIGN_MASK) != 0; break;
    }
    return f == (bool)(cc & 1);
}

NX_DISP_INL void Z80::decodeRETcc() {  /* RET cc */
    Z80Ops::addressOnBusIR(1);
    if (condMet((opCode >> 3) & 7)) {
        REG_PC = REG_WZ = pop();
        check_trdos();
    }
}

NX_DISP_INL void Z80::decodeJPcc() {   /* JP cc,nn */
    REG_WZ = NextMemory::peek16(REG_PC);
    if (condMet((opCode >> 3) & 7)) {
        REG_PC = REG_WZ;
        check_trdos();
        return;
    }
    REG_PC = REG_PC + 2;
}

NX_DISP_INL void Z80::decodeCALLcc() { /* CALL cc,nn */
    REG_WZ = NextMemory::peek16(REG_PC);
    if (condMet((opCode >> 3) & 7)) {
        Z80Ops::addressOnBus(REG_PC + 1, 1);
        push(REG_PC + 2);
        REG_PC = REG_WZ;
        check_trdos();
        return;
    }
    REG_PC = REG_PC + 2;
}

#if NEXT_TRACE
// ---- Next: who raised error M? -------------------------------------------
// Each RST 8 (PC, the byte after it = the error code or the esxDOS call id,
// DE/HL/SP, the ROM/MMU state) lands in a 64-entry ring. An RST 8 followed by
// #15 is logged on the spot with the BASIC sysvars; nxErrPoll() (once a frame,
// NEXTVID::EndFrame) catches ERR_NR turning #15 by any other route (JP to an
// error routine, RAM code) and dumps the ring. One-shot budgets: a trace, not
// a flood.
struct NxRst8 { uint16_t pc, de, hl, sp; uint8_t b, rom, m0, m1, p7, p1; };
static NxRst8 s_nx_rst[64];
static uint32_t s_nx_rst_w = 0;
static uint16_t nxW(uint16_t a) {
    return (uint16_t)(MemESP::dbgPeek(a) | (MemESP::dbgPeek((uint16_t)(a + 1)) << 8));
}
static Z80_COLD void nxSysvars(const char* tag) {
    Debug::log("[NXERR] %s STKEND=%04X P_RAMT=%04X RAMTOP=%04X VARS=%04X PROG=%04X "
               "E_LINE=%04X WORKSP=%04X CH_ADD=%04X PPC=%04X SUB=%u ERR_SP=%04X",
               tag, nxW(0x5C65), nxW(0x5CB4), nxW(0x5CB2), nxW(0x5C4B), nxW(0x5C53),
               nxW(0x5C59), nxW(0x5C61), nxW(0x5C5D), nxW(0x5C45),
               (unsigned)MemESP::dbgPeek(0x5C47), nxW(0x5C3D));
    char hx[140]; int p = 0;
    const uint16_t e = nxW(0x5C59);
    for (int i = 0; i < 48 && p < (int)sizeof(hx) - 3; i++)
        p += snprintf(hx + p, sizeof(hx) - p, "%02X", MemESP::dbgPeek((uint16_t)(e + i)));
    Debug::log("[NXERR]   E_LINE: %s", hx);
}
static Z80_COLD void nxRst8Note() {
    NxRst8& r = s_nx_rst[s_nx_rst_w++ & 63];
    r.pc = (uint16_t)(Z80::getRegPC() - 1); r.de = Z80::getRegDE(); r.hl = Z80::getRegHL(); r.sp = Z80::getRegSP();
    r.b = MemESP::dbgPeek(Z80::getRegPC()); r.rom = (uint8_t)MemESP::romInUse;
    r.m0 = MemESP::mmu[0]; r.m1 = MemESP::mmu[1];
    r.p7 = NextReg::port7FFD; r.p1 = NextReg::port1FFD;
    static uint8_t n = 0;
    if (r.b == 0x15 && n < 4) {
        n++;
        Debug::log("[NXERR] RST 8/#15 at %04X rom=%u mmu=%02X.%02X 7FFD=%02X 1FFD=%02X "
                   "DE=%04X HL=%04X BC=%04X SP=%04X [sp]=%04X %04X %04X %04X",
                   r.pc, r.rom, r.m0, r.m1, r.p7, r.p1, Z80::getRegDE(), Z80::getRegHL(), Z80::getRegBC(), Z80::getRegSP(),
                   nxW(Z80::getRegSP()), nxW(Z80::getRegSP() + 2), nxW(Z80::getRegSP() + 4), nxW(Z80::getRegSP() + 6));
        nxSysvars("  at RST:");
    }
}
// ---- Next: who jumped to 0x0000 in ROM 2? ----------------------------------
// NextZXOS's SPECTRUM snapshot executor (.sna/.z80/.snx from the Browser) ends
// in ROM 2's reset trap (NOP / JR 0) on a black screen (2026-10-08). Every
// control transfer the core routes through check_trdos (JP/RET/CALLcc/JP(HL)/
// RETI), every NMI and every RETN lands in a 256-entry ring; the first time a
// transfer lands on 0x0000 with ROM 2 paged the ring is printed once.
struct NxJmp { uint16_t to, sp, af, bc, de, hl; uint8_t t, rom, m0, m1, p7, p1, iff, dm; };
static constexpr uint32_t NX_JMP_N = 256;
static NxJmp s_nx_jmp[NX_JMP_N];
static uint32_t s_nx_jmp_w = 0;
static bool s_nx_jmp_done = false;
static int32_t s_nx_after = -1;   // >0: dump the ring this many transfers later
static bool s_nx_bas = false;     // NexLaunch BAS: dump when the guest derails
static const char* s_nx_dump_why = nullptr;
static uint32_t s_nx_dump_first, s_nx_dump_count, s_nx_dump_pos;
static uint8_t s_nx_dump_c0, s_nx_dump_c2, s_nx_dump_c3;
static bool s_nx_dump_retn, s_nx_dump_header;
// NextMf::retn(): the snapshot launch ends in the Multiface's RETN — what runs
// after it is the question, so dump the ring 192 transfers later.
// Every Multiface launch re-arms the ring: n > 0 dumps n transfers later,
// n == 0 only waits for the next landing / NR 02 reset write.
// NexLaunch BAS: re-arm the ring and dump it at the first transfer whose
// stack is in the attribute file or whose target is the screen bitmap.
Z80_COLD void nxJumpArmBas() {
    s_nx_dump_why = nullptr;
    s_nx_jmp_done = false;
    s_nx_after = -1;
    s_nx_bas = true;
}
Z80_COLD void nxJumpArmAfter(int n) {
    // A new snapshot launch supersedes any old dump still queued for output.
    s_nx_dump_why = nullptr;
    s_nx_jmp_done = false;
    s_nx_after = n > 0 ? n : -1;
}
static void nxJumpDump(const char* why);
// A nextreg write into the same ring: to = reg<<8 | value, sp = PC.
Z80_COLD void nxNrNote(uint8_t r, uint8_t v) {
    if (s_nx_jmp_done) return;
    NxJmp& j = s_nx_jmp[s_nx_jmp_w++ & (NX_JMP_N - 1)];
    j.t = 'n'; j.to = (uint16_t)(r << 8 | v); j.sp = Z80::getRegPC();
    j.af = Z80::getRegAF(); j.bc = Z80::getRegBC(); j.de = Z80::getRegDE(); j.hl = Z80::getRegHL();
    j.rom = (uint8_t)MemESP::romInUse; j.m0 = MemESP::mmu[0]; j.m1 = MemESP::mmu[1];
    j.p7 = NextReg::port7FFD; j.p1 = NextReg::port1FFD;
    j.iff = (uint8_t)(Z80::isIFF1() | (Z80::isIFF2() << 1));
    j.dm = (uint8_t)(MemESP::divmmc_mapped | (DivMMC::automap << 1) | (DivMMC::conmem << 2));
    // NextZXOS's 48 BASIC turns 0x0000 into NR 02 = 01 (ROM3 #0000 -> #3BE8):
    // a snapshot that crashes returns to the menu through here.
    if (r == 0x02 && (v & 0x03)) nxJumpDump("NR 02 reset request");
}
Z80_COLD void nxJumpNote(char t) {
    if (s_nx_jmp_done) return;
    NxJmp& j = s_nx_jmp[s_nx_jmp_w++ & (NX_JMP_N - 1)];
    j.t = (uint8_t)t; j.to = Z80::getRegPC(); j.sp = Z80::getRegSP();
    j.af = Z80::getRegAF(); j.bc = Z80::getRegBC(); j.de = Z80::getRegDE(); j.hl = Z80::getRegHL();
    j.rom = (uint8_t)MemESP::romInUse; j.m0 = MemESP::mmu[0]; j.m1 = MemESP::mmu[1];
    j.p7 = NextReg::port7FFD; j.p1 = NextReg::port1FFD;
    j.iff = (uint8_t)(Z80::isIFF1() | (Z80::isIFF2() << 1));
    j.dm = (uint8_t)(MemESP::divmmc_mapped | (DivMMC::automap << 1) | (DivMMC::conmem << 2));
    if (s_nx_after > 0 && --s_nx_after == 0) { nxJumpDump("192 transfers after the Multiface RETN"); return; }
    if (s_nx_bas && ((j.sp >= 0x5800 && j.sp < 0x5B00) || (j.to >= 0x4000 && j.to < 0x5800))) {
        s_nx_bas = false;
        // An error unwinds through ERR_SP: which RST 8 (code byte) came last.
        {
            char r8[200]; int p = 0;
            const uint32_t cnt = s_nx_rst_w < 10 ? s_nx_rst_w : 10;
            for (uint32_t i = 0; i < cnt && p < (int)sizeof(r8) - 16; i++) {
                const NxRst8& r = s_nx_rst[(s_nx_rst_w - cnt + i) & 63];
                p += snprintf(r8 + p, sizeof(r8) - p, "%04X/%u:%02X ", r.pc, r.rom, r.b);
            }
            Debug::log("[NXJMP] last RST 8 (pc/rom:code): %s ERR_NR=%02X", r8, MemESP::dbgPeek(0x5C3A));
        }
        nxJumpDump("BAS launch: SP in attributes / PC in the screen");
        return;
    }
    if (j.to != 0 || j.rom != 2) return;
    nxJumpDump("landed on 0000 in ROM 2");
}
static void nxJumpDump(const char* why) {
    s_nx_jmp_done = true;
    s_nx_dump_why = why; // callers pass string literals
    s_nx_dump_count = s_nx_jmp_w < NX_JMP_N ? s_nx_jmp_w : NX_JMP_N;
    s_nx_dump_first = s_nx_jmp_w - s_nx_dump_count;
    s_nx_dump_pos = 0;
    s_nx_dump_header = true;
    s_nx_dump_c0 = NextReg::reg[0xC0];
    s_nx_dump_c2 = NextReg::reg[0xC2];
    s_nx_dump_c3 = NextReg::reg[0xC3];
    s_nx_dump_retn = NextInt::retnArmed;
}

// Once per frame, with the ring frozen by s_nx_jmp_done. Whole-line enqueue
// avoids truncation under UART backpressure; never wait for bytes to transmit.
Z80_COLD void nxJumpPump() {
    if (!s_nx_dump_why) return;
    if (s_nx_dump_header) {
        if (!Debug::tryLog("[NXJMP] %s, C0=%02X C2=%02X C3=%02X retnArmed=%d (dm: 1 window 2 automap 4 conmem); last transfers:",
                          s_nx_dump_why, s_nx_dump_c0, s_nx_dump_c2, s_nx_dump_c3, (int)s_nx_dump_retn)) return;
        s_nx_dump_header = false;
    } else if (s_nx_dump_pos < s_nx_dump_count) {
        const NxJmp& r = s_nx_jmp[(s_nx_dump_first + s_nx_dump_pos) & (NX_JMP_N - 1)];
        if (!Debug::tryLog("[NXJMP] %c %04X sp=%04X af=%04X bc=%04X de=%04X hl=%04X rom=%u m=%02X.%02X 7FFD=%02X 1FFD=%02X iff=%u dm=%u",
                          r.t, r.to, r.sp, r.af, r.bc, r.de, r.hl, r.rom, r.m0, r.m1, r.p7, r.p1, r.iff, r.dm)) return;
        ++s_nx_dump_pos;
    }
    if (s_nx_dump_pos == s_nx_dump_count) {
        s_nx_dump_why = nullptr;
        // Printed: re-arm the ring, whatever froze it (NextZXOS's own boot
        // soft-reset used to freeze it for the whole session).
        s_nx_after = -1;
        s_nx_jmp_done = false;
    }
}

Z80_COLD void nxErrPoll() {
    static uint8_t last = 0xFF, n = 0;
    const uint8_t e = MemESP::dbgPeek(0x5C3A);
    if (e == last) return;
    last = e;
    if (e != 0x15 || n >= 3) return;
    n++;
    Debug::log("[NXERR] ERR_NR=#15 (M) seen at frame end, PC=%04X rom=%u, last RST 8s (oldest first):",
               (unsigned)Z80::getRegPC(), (unsigned)MemESP::romInUse);
    const uint32_t cnt = s_nx_rst_w < 24 ? s_nx_rst_w : 24;
    for (uint32_t i = 0; i < cnt; i++) {
        const NxRst8& r = s_nx_rst[(s_nx_rst_w - cnt + i) & 63];
        Debug::log("[NXERR]   %04X b=%02X rom=%u mmu=%02X.%02X 7FFD=%02X 1FFD=%02X DE=%04X HL=%04X SP=%04X",
                   r.pc, r.b, r.rom, r.m0, r.m1, r.p7, r.p1, r.de, r.hl, r.sp);
    }
    nxSysvars("  now:");
}
#endif

void Z80::decodeRST() {    /* RST p */
#if NEXT_TRACE
    // Every RST 8 on the Next goes into a ring for nxErrPoll() (error M hunt,
    // 2026-10-07: NextZXOS's TAP loader ends in "M RAMTOP no good").
    if (Z80Ops::isNext && opCode == 0xCF) nxRst8Note();
#endif
#if ZC_PORT_TRACE || ATM_PAGE_TRACE
    // ATM: an RST #38 executed with interrupts ENABLED enters the IM1 handler without
    // the IFF1 clear an accepted INT does — the NedoOS sys_sysint nesting (2026-09-28).
    {
        static uint16_t n = 0;
        if (Z80Ops::isAtm && opCode == 0xFF && ffIFF1 && n < 30) {
            n++;
            const uint16_t at = (uint16_t)(REG_PC - 1);
            Debug::log("[ATMRST] RST38 at %04X sp=%04X [sp]=%04X w=%p %p %p %p", at, REG_SP,
                       Z80Ops::peek16(REG_SP),
                       (const void*)MemESP::ramCurrent[0], (const void*)MemESP::ramCurrent[2],
                       (const void*)MemESP::ramCurrent[4], (const void*)MemESP::ramCurrent[6]);
            if (n <= 2) {
                const uint8_t* w0 = MemESP::ramCurrent[0];
                Debug::log("[ATMRST]   w0[40..57]=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                           w0[0x40],w0[0x41],w0[0x42],w0[0x43],w0[0x44],w0[0x45],w0[0x46],w0[0x47],
                           w0[0x48],w0[0x49],w0[0x4A],w0[0x4B],w0[0x4C],w0[0x4D],w0[0x4E],w0[0x4F],
                           w0[0x50],w0[0x51],w0[0x52],w0[0x53],w0[0x54],w0[0x55],w0[0x56],w0[0x57]);
                Debug::log("[ATMRST]   pF7=%03X %03X %03X %03X | %03X %03X %03X %03X p7ffd=%02X",
                           Atm::pF7[0], Atm::pF7[1], Atm::pF7[2], Atm::pF7[3],
                           Atm::pF7[4], Atm::pF7[5], Atm::pF7[6], Atm::pF7[7], Atm::p7ffd);
                extern uint16_t g_atm_pg_port[16], g_atm_pg_pc[16];
                extern uint8_t g_atm_pg_val[16], g_atm_pg_i;
                for (int j = 0; j < 16; j++) {
                    const uint8_t k = (uint8_t)(g_atm_pg_i - 16 + j) & 15;
                    Debug::log("[ATMRST]   pg#%02d %04X=%02X pc=%04X", j, g_atm_pg_port[k],
                               g_atm_pg_val[k], g_atm_pg_pc[k]);
                }
            }
        }
    }
#endif
    // ZX-Evo: the 48 BASIC of the EVO Reset Service set hooks LD-BYTES with RST 8 /
    // DEFB #45 in place of LD C,A / CP A at #0569, so the CP A trap never fires there.
    if (__builtin_expect(opCode == 0xCF && REG_PC == 0x056A, 0) && Z80Ops::isAtm &&
        evo_tape_trap()) return;
    Z80Ops::addressOnBusIR(1);
    push(REG_PC);
    REG_PC = REG_WZ = opCode & 0x38;
}

void Z80::decodeINCDEC8() { /* INC r (bit 0 clear) / DEC r (bit 0 set), incl. (HL) */
    const uint8_t op = opCode;
    uint8_t* p = reg8[(op >> 3) & 7];
    if (p) {
        if (op & 1) dec8(*p); else inc8(*p);
        return;
    }
    uint8_t work8 = NextMemory::peek8(REG_HL);
    if (op & 1) dec8(work8); else inc8(work8);
    Z80Ops::addressOnBus(REG_HL, 1);
    Z80Ops::poke8(REG_HL, work8);
}

// Generic CB decoder. Opcode bits: [7:6] group, [5:3] n, [2:0] r.
// group 0 = rotate/shift #n (RLC RRC RL RR SLA SRA SLL SRL), 1 = BIT n,r,
// 2 = RES n,r, 3 = SET n,r; r = B C D E H L (HL) A. One routine replaces the
// 256 per-opcode handlers + their 1 KB dispatch table (-6.2 KB of SRAM, 2026-09-07)
// with the same semantics and the same bus timing (addressOnBus before the (HL) write-back).
void Z80::decodeCB() {
    const uint8_t op = opCode;
    const uint8_t r = op & 7;
    const uint8_t n = (op >> 3) & 7;
    const uint8_t mask = 1u << n;
    uint8_t* reg = reg8[r];
    uint8_t work8 = reg ? *reg : NextMemory::peek8(REG_HL);
    switch (op >> 6) {
        case 0:
            switch (n) {
                case 0: rlc(work8); break;
                case 1: rrc(work8); break;
                case 2: rl(work8);  break;
                case 3: rr(work8);  break;
                case 4: sla(work8); break;
                case 5: sra(work8); break;
                case 6: sll(work8); break;
                default: srl(work8); break;
            }
            break;
        case 1:
            bitTest(mask, work8);
            if (!reg) {
                sz5h3pnFlags = (sz5h3pnFlags & FLAG_SZHP_MASK) | (REG_W & FLAG_53_MASK);
                Z80Ops::addressOnBus(REG_HL, 1);
            }
            return;
        case 2: work8 &= (uint8_t)~mask; break;
        default: work8 |= mask; break;
    }
    if (reg) {
        *reg = work8;
    } else {
        Z80Ops::addressOnBus(REG_HL, 1);
        Z80Ops::poke8(REG_HL, work8);
    }
}



// // Trim from end (in place) (for SAVE trap, clean this up later)
// static inline void rtrim(std::string &s) {
//     s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
//         return !std::isspace(ch);
//     }).base(), s.end());
// }

//Subconjunto de instrucciones 0xDD / 0xFD
/*
 * Hay que tener en cuenta el manejo de secuencias códigos DD/FD que no
 * hacen nada. Según el apartado 3.7 del documento
 * [http://www.myquest.nl/z80undocumented/z80-documented-v0.91.pdf]
 * secuencias de códigos como FD DD 00 21 00 10 NOP NOP NOP LD HL,1000h
 * activan IY con el primer FD, IX con el segundo DD y vuelven al
 * registro HL con el código NOP. Es decir, si detrás del código DD/FD no
 * viene una instrucción que maneje el registro HL, el código DD/FD
 * "se olvida" y hay que procesar la instrucción como si nunca se
 * hubiera visto el prefijo (salvo por los 4 t-estados que ha costado).
 * Naturalmente, en una serie repetida de DDFD no hay que comprobar las
 * interrupciones entre cada prefijo.
 */
// DD/FD register operand: H and L stand for IXh/IXl. Never called with r == 6.
inline __attribute__((always_inline)) uint8_t* Z80::ixyReg8(int r, RegisterPair& ixy) {
    return r == 4 ? &ixy.byte8.hi : r == 5 ? &ixy.byte8.lo : reg8[r];
}

void Z80::decodeDDFDLD8(RegisterPair& regIXY) {   /* LD r,r' block under DD/FD, 0x76 excluded */
    const uint8_t op = opCode;
    const int s = op & 7, d = (op >> 3) & 7;
    if (s == 6) {                 // LD r,(IX+d): r is the REAL register, H/L included
        REG_WZ = regIXY.word + (int8_t) NextMemory::peek8(REG_PC);
        NextMemory::addressOnBus(REG_PC, 5);
        REG_PC++;
        *reg8[d] = NextMemory::peek8(REG_WZ);
    } else if (d == 6) {          // LD (IX+d),r: likewise
        REG_WZ = regIXY.word + (int8_t) NextMemory::peek8(REG_PC);
        NextMemory::addressOnBus(REG_PC, 5);
        REG_PC++;
        Z80Ops::poke8(REG_WZ, *reg8[s]);
    } else {
        *ixyReg8(d, regIXY) = *ixyReg8(s, regIXY);
    }
}

void Z80::decodeDDFDALU8(RegisterPair& regIXY) {  /* ADD..CP A,r / A,(IX+d) under DD/FD, 0x80-0xBE */
    const uint8_t op = opCode;
    const int s = op & 7;
    uint8_t v;
    if (s == 6) {
        REG_WZ = regIXY.word + (int8_t) NextMemory::peek8(REG_PC);
        NextMemory::addressOnBus(REG_PC, 5);
        REG_PC++;
        v = NextMemory::peek8(REG_WZ);
    } else {
        v = *ixyReg8(s, regIXY);
    }
    switch ((op >> 3) & 7) {
        case 0: add(v); break;
        case 1: adc(v); break;
        case 2: sub(v); break;
        case 3: sbc(v); break;
        case 4: and_(v); break;
        case 5: xor_(v); break;
        case 6: or_(v); break;
        default: cp(v); break;
    }
}

void Z80::decodeDDFD(RegisterPair& regIXY) {
    switch (opCode) {
        case 0x09:
        { /* ADD IX,BC */
            Z80Ops::addressOnBusIR(7);
            add16(regIXY, REG_BC);
            break;
        }
        case 0x19:
        { /* ADD IX,DE */
            Z80Ops::addressOnBusIR(7);
            add16(regIXY, REG_DE);
            break;
        }
        case 0x21:
        { /* LD IX,nn */
            regIXY.word = NextMemory::peek16(REG_PC);
            REG_PC = REG_PC + 2;
            break;
        }
        case 0x22:
        { /* LD (nn),IX */
            REG_WZ = NextMemory::peek16(REG_PC);
            Z80Ops::poke16(REG_WZ++, regIXY);
            REG_PC = REG_PC + 2;
            break;
        }
        case 0x23:
        { /* INC IX */
            Z80Ops::addressOnBusIR(2);
            regIXY.word++;
            break;
        }
        case 0x24:
        { /* INC IXh */
            inc8(regIXY.byte8.hi);
            break;
        }
        case 0x25:
        { /* DEC IXh */
            dec8(regIXY.byte8.hi);
            break;
        }
        case 0x26:
        { /* LD IXh,n */
            regIXY.byte8.hi = NextMemory::peek8(REG_PC);
            REG_PC++;
            break;
        }
        case 0x29:
        { /* ADD IX,IX */
            Z80Ops::addressOnBusIR(7);
            add16(regIXY, regIXY.word);
            break;
        }
        case 0x2A:
        { /* LD IX,(nn) */
            REG_WZ = NextMemory::peek16(REG_PC);
            regIXY.word = NextMemory::peek16(REG_WZ++);
            REG_PC = REG_PC + 2;
            break;
        }
        case 0x2B:
        { /* DEC IX */
            Z80Ops::addressOnBusIR(2);
            regIXY.word--;

            if (REG_PC == 0x04d4) { // Save trap

                static bool SaveFileExists;

                if (REG_HL == 0x1F80) {
                    SaveFileExists = (Tape::tapeFileType == TAPE_FTYPE_TAP);
                    if (!SaveFileExists)
                        OSD::notify(OSD_TAPE_SELECT_ERR, LEVEL_WARN);
                }

                if (SaveFileExists) {
                    regA = REG_HL == 0x1F80 ? 0x00 : 0xFF;
                    regIXY.word++;
                    REG_DE--;
                    Tape::Save();
                    REG_PC = 0x555;
                }

                // static uint8_t SaveRes;

                // if (REG_HL == 0x1F80) {

                //     // printf("Saving header!\n");

                //     regIXY.word++;

                //     // remove .tap output file if exists

                //     // Get save name
                //     string name;
                //     uint16_t header_data = REG_IX;
                //     for (int i=0; i < 10; i++)
                //         name += MemESP::ramCurrent[header_data++ >> 14][header_data & 0x3fff];
                //     rtrim(name);

                //     SaveRes = DLG_YES;

                //     struct stat stat_buf;

                //     printf("Tapesavename: %s\n",Tape::tapeSaveName.c_str());
                //     if ( Tape::tapeSaveName == "" || Tape::tapeSaveName == "none" || !FileUtils::hasTAPextension(Tape::tapeSaveName) || stat(Tape::tapeSaveName.c_str(), &stat_buf) ) {
                //         OSD::osdCenteredMsg(OSD_TAPE_SELECT_ERR, LEVEL_WARN);
                //         SaveRes = DLG_NO;
                //     } else {
                //         REG_DE--;
                //         regA = 0x00;

                //         Tape::Save();

                //         REG_PC = 0x555;
                //     }

                // } else {

                //     // printf("Saving data!\n");

                //     // Call Save function

                //     // printf("Saving %s block.\n",Tape::tapeSaveName.c_str());

                //     if (SaveRes == DLG_YES) {

                //         REG_DE--;
                //         regIXY.word++;
                //         regA = 0xFF;

                //         Tape::Save();

                //         REG_PC = 0x555;

                //     }

                // }

            }

            break;
        }
        case 0x2C:
        { /* INC IXl */
            inc8(regIXY.byte8.lo);
            break;
        }
        case 0x2D:
        { /* DEC IXl */
            dec8(regIXY.byte8.lo);
            break;
        }
        case 0x2E:
        { /* LD IXl,n */
            regIXY.byte8.lo = NextMemory::peek8(REG_PC);
            REG_PC++;
            break;
        }
        case 0x34:
        { /* INC (IX+d) */
            REG_WZ = regIXY.word + (int8_t) NextMemory::peek8(REG_PC);
            Z80Ops::addressOnBus(REG_PC, 5);
            REG_PC++;
            uint8_t work8 = NextMemory::peek8(REG_WZ);
            Z80Ops::addressOnBus(REG_WZ, 1);
            inc8(work8);
            Z80Ops::poke8(REG_WZ, work8);
            break;
        }
        case 0x35:
        { /* DEC (IX+d) */
            REG_WZ = regIXY.word + (int8_t) NextMemory::peek8(REG_PC);
            Z80Ops::addressOnBus(REG_PC, 5);
            REG_PC++;
            uint8_t work8 = NextMemory::peek8(REG_WZ);
            Z80Ops::addressOnBus(REG_WZ, 1);
            dec8(work8);
            Z80Ops::poke8(REG_WZ, work8);
            break;
        }
        case 0x36:
        { /* LD (IX+d),n */
            REG_WZ = regIXY.word + (int8_t) NextMemory::peek8(REG_PC);
            REG_PC++;
            uint8_t work8 = NextMemory::peek8(REG_PC);
            Z80Ops::addressOnBus(REG_PC, 2);
            REG_PC++;
            Z80Ops::poke8(REG_WZ, work8);
            break;
        }
        case 0x39:
        { /* ADD IX,SP */
            Z80Ops::addressOnBusIR(7);
            add16(regIXY, REG_SP);
            break;
        }
        // LD r,r' / LD r,(IX+d) / LD (IX+d),r — one handler for the whole 0x40-0x7F
        // block (0x76 = HALT keeps the default path) and ALU A,r / A,(IX+d) for
        // 0x80-0xBE (0xBF = CP A keeps its tape-trap handler via the default path).
        // Under DD/FD, H and L mean IXh/IXl EXCEPT in the (IX+d) forms, where the
        // other operand is the real H/L — decodeDDFDLD8 / decodeDDFDALU8 encode
        // exactly that rule (2026-09-22: 60 hand-written cases, ~1.3 KB of SRAM).
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
        case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:            case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        {
            decodeDDFDLD8(regIXY);
            break;
        }
        case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
        case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xA8: case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE:
        {
            decodeDDFDALU8(regIXY);
            break;
        }
        case 0xCB:
        { /* Subconjunto de instrucciones */
            REG_WZ = regIXY.word + (int8_t) NextMemory::peek8(REG_PC);
            REG_PC++;
            opCode = NextMemory::peek8(REG_PC);
            Z80Ops::addressOnBus(REG_PC, 2);
            REG_PC++;
            decodeDDFDCB(REG_WZ);
            break;
        }
        case 0xDD:
            prefixOpcode = 0xDD;
            break;
        case 0xE1:
        { /* POP IX */
            regIXY.word = pop();
            break;
        }
        case 0xE3:
        { /* EX (SP),IX */
            // Instrucción de ejecución sutil como pocas... atento al dato.
            RegisterPair work16 = regIXY;
            regIXY.word = NextMemory::peek16(REG_SP);
            Z80Ops::addressOnBus(REG_SP + 1, 1);
            // I can't call to poke16 from here because the Z80 do the writes in inverted order
            // Same for EX (SP), HL
            Z80Ops::poke8(REG_SP + 1, work16.byte8.hi);
            Z80Ops::poke8(REG_SP, work16.byte8.lo);
            Z80Ops::addressOnBus(REG_SP, 2);
            REG_WZ = regIXY.word;
            break;
        }
        case 0xE5:
        { /* PUSH IX */
            Z80Ops::addressOnBusIR(1);
            push(regIXY.word);
            break;
        }
        case 0xE9:
        { /* JP (IX) */
            REG_PC = regIXY.word;

            check_trdos();

            break;
        }
        case 0xED:
        {
            prefixOpcode = 0xED;
            break;
        }
        case 0xF9:
        { /* LD SP,IX */
            Z80Ops::addressOnBusIR(2);
            REG_SP = regIXY.word;
            break;
        }
        case 0xFD:
        {
            prefixOpcode = 0xFD;
            break;
        }
        default:
        {
            // Detrás de un DD/FD o varios en secuencia venía un código
            // que no correspondía con una instrucción que involucra a
            // IX o IY. Se trata como si fuera un código normal.
            // Sin esto, además de emular mal, falla el test
            // ld <bcdexya>,<bcdexya> de ZEXALL.
#ifdef WITH_BREAKPOINT_SUPPORT
            if (breakpointEnabled && prefixOpcode == 0) {
                opCode = Z80Ops::breakpoint(REG_PC, opCode);
            }
#endif
            dcOpcode[opCode]();
            // decodeOpcode();
            break;
        }
    }
}

// Subconjunto de instrucciones 0xDDCB
void Z80::decodeDDFDCB(uint16_t address) {
    // One body for all 256 DD/FD CB opcodes (2026-09-22; was a 32-block switch,
    // ~700 B): bits [7:6] pick rotate / BIT / RES / SET, [5:3] the rotate kind or
    // the bit, [2:0] the register the undocumented forms copy the result to
    // (6 = none). Same sequence as every block had: read, operate, one bus
    // cycle, write back, copy — BIT reads, tests, takes its 5/3 flags from the
    // address high byte and takes the bus cycle without writing.
    const uint8_t op = opCode;
    const uint8_t n = (op >> 3) & 7;
    const uint8_t mask = 1u << n;
    uint8_t work8 = NextMemory::peek8(address);
    switch (op >> 6) {
        case 0:
            switch (n) {
                case 0: rlc(work8); break;
                case 1: rrc(work8); break;
                case 2: rl(work8);  break;
                case 3: rr(work8);  break;
                case 4: sla(work8); break;
                case 5: sra(work8); break;
                case 6: sll(work8); break;
                default: srl(work8); break;
            }
            break;
        case 1:
            bitTest(mask, work8);
            sz5h3pnFlags = (sz5h3pnFlags & FLAG_SZHP_MASK) | ((address >> 8) & FLAG_53_MASK);
            Z80Ops::addressOnBus(address, 1);
            return;
        case 2: work8 &= (uint8_t)~mask; break;
        default: work8 |= mask; break;
    }
    Z80Ops::addressOnBus(address, 1);
    Z80Ops::poke8(address, work8);
    copyToRegister(work8);
}


// Z80N extended opcodes (ZX Spectrum Next), dispatched from decodeED under
// Z80Ops::isNext — in the .nxovl window (2026-10-10). Briefly in flash on
// 2026-10-11 and moved back the same day: it is entered on EVERY Z80N
// instruction (NEXTREG, MUL, ADD rr,A... in guest loops), and flash code there
// queues behind the guest's own PSRAM fetches on the shared XIP port.
NX_OVL_CODE void Z80::decodeZ80N() {
    switch (opCode) {
        case 0x23:
        { /* SWAPNIB */
            regA = (regA << 4) | (regA >> 4);
            break;
        }
        case 0x24:
        { /* MIRROR A */
            uint8_t v = regA;
            v = ((v & 0xF0) >> 4) | ((v & 0x0F) << 4);
            v = ((v & 0xCC) >> 2) | ((v & 0x33) << 2);
            v = ((v & 0xAA) >> 1) | ((v & 0x55) << 1);
            regA = v;
            break;
        }
        case 0x27:
        { /* TEST n */
            uint8_t value = Z80Ops::peek8(REG_PC);
            REG_PC++;
            carryFlag = false;
            sz5h3pnFlags = sz53pn_addTable[regA & value] | HALFCARRY_MASK;
            flagQ = true;
            break;
        }
        case 0x28:
        { /* BSLA DE,B */
            uint8_t sh = REG_B & 0x1F;
            REG_DE = (sh > 15) ? 0 : (uint16_t)(REG_DE << sh);
            break;
        }
        case 0x29:
        { /* BSRA DE,B */
            uint8_t sh = REG_B & 0x1F;
            if (sh > 15) sh = 15;
            REG_DE = (uint16_t)(((int16_t)REG_DE) >> sh);
            break;
        }
        case 0x2A:
        { /* BSRL DE,B */
            uint8_t sh = REG_B & 0x1F;
            REG_DE = (sh > 15) ? 0 : (uint16_t)(REG_DE >> sh);
            break;
        }
        case 0x2B:
        { /* BSRF DE,B */
            uint8_t sh = REG_B & 0x1F;
            REG_DE = (sh > 15) ? 0xFFFF : (uint16_t)~((uint16_t)(~REG_DE) >> sh);
            break;
        }
        case 0x2C:
        { /* BRLC DE,B */
            uint8_t sh = REG_B & 0x0F;
            REG_DE = (uint16_t)((REG_DE << sh) | (REG_DE >> (16 - sh)));
            break;
        }
        case 0x30:
        { /* MUL D,E */
            REG_DE = (uint16_t)(REG_D * REG_E);
            break;
        }
        case 0x31:
        { /* ADD HL,A */
            REG_HL += regA;
            break;
        }
        case 0x32:
        { /* ADD DE,A */
            REG_DE += regA;
            break;
        }
        case 0x33:
        { /* ADD BC,A */
            REG_BC += regA;
            break;
        }
        case 0x34:
        { /* ADD HL,nn */
            uint16_t nn = Z80Ops::peek8(REG_PC);
            REG_PC++;
            nn |= Z80Ops::peek8(REG_PC) << 8;
            REG_PC++;
            Z80Ops::addressOnBusIR(2);
            REG_HL += nn;
            break;
        }
        case 0x35:
        { /* ADD DE,nn */
            uint16_t nn = Z80Ops::peek8(REG_PC);
            REG_PC++;
            nn |= Z80Ops::peek8(REG_PC) << 8;
            REG_PC++;
            Z80Ops::addressOnBusIR(2);
            REG_DE += nn;
            break;
        }
        case 0x36:
        { /* ADD BC,nn */
            uint16_t nn = Z80Ops::peek8(REG_PC);
            REG_PC++;
            nn |= Z80Ops::peek8(REG_PC) << 8;
            REG_PC++;
            Z80Ops::addressOnBusIR(2);
            REG_BC += nn;
            break;
        }
        case 0x8A:
        { /* PUSH nn — immediate is big-endian (high byte first) */
            uint16_t nn = Z80Ops::peek8(REG_PC) << 8;
            REG_PC++;
            nn |= Z80Ops::peek8(REG_PC);
            REG_PC++;
            Z80Ops::addressOnBusIR(3);
            push(nn);
            break;
        }
        case 0x90:
        { /* OUTINB — like OUTI but B unchanged, no flags */
            Z80Ops::addressOnBusIR(1);
            uint8_t work8 = Z80Ops::peek8(REG_HL);
            Ports::output(REG_BC, work8);
            REG_HL++;
            break;
        }
        case 0x91:
        { /* NEXTREG reg,val */
            uint8_t r = Z80Ops::peek8(REG_PC);
            REG_PC++;
            uint8_t v = Z80Ops::peek8(REG_PC);
            REG_PC++;
            Z80Ops::addressOnBusIR(6);
            NextReg::writeHot(r, v);
            break;
        }
        case 0x92:
        { /* NEXTREG reg,A */
            uint8_t r = Z80Ops::peek8(REG_PC);
            REG_PC++;
            Z80Ops::addressOnBusIR(6);
            NextReg::writeHot(r, regA);
            break;
        }
        case 0x93:
        { /* PIXELDN — advance HL one line down in ULA screen layout */
            if ((REG_HL & 0x0700) != 0x0700)
                REG_HL += 0x0100;
            else if ((REG_HL & 0x00E0) != 0x00E0)
                REG_HL = (REG_HL & 0xF8FF) + 0x0020;
            else
                REG_HL = (REG_HL & 0xF81F) + 0x0800;
            break;
        }
        case 0x94:
        { /* PIXELAD — HL = ULA address of pixel (D=y, E=x) */
            REG_HL = 0x4000 | ((REG_D & 0xC0) << 5) | ((REG_D & 0x07) << 8)
                            | ((REG_D & 0x38) << 2) | (REG_E >> 3);
            break;
        }
        case 0x95:
        { /* SETAE — A = pixel mask for x-coordinate in E */
            regA = 0x80 >> (REG_E & 0x07);
            break;
        }
        case 0x98:
        { /* JP (C) — jump within current 16K bank via port read */
            uint8_t in8 = Ports::input(REG_BC);
            Z80Ops::addressOnBusIR(1);
            REG_PC = (REG_PC & 0xC000) | ((uint16_t)in8 << 6);
            break;
        }
        case 0xA4:
        { /* LDIX */
            ldix();
            break;
        }
        case 0xA5:
        { /* LDWS */
            ldws();
            break;
        }
        case 0xAC:
        { /* LDDX */
            lddx();
            break;
        }
        case 0xB4:
        { /* LDIRX */
            ldix();
            if (REG_BC != 0) {
                REG_PC = REG_PC - 2;
                Z80Ops::addressOnBus(REG_DE - 1, 5);
            }
            break;
        }
        case 0xB7:
        { /* LDPIRX */
            ldpirx();
            if (REG_BC != 0) {
                REG_PC = REG_PC - 2;
                Z80Ops::addressOnBus(REG_DE - 1, 5);
            }
            break;
        }
        case 0xBC:
        { /* LDDRX */
            lddx();
            if (REG_BC != 0) {
                REG_PC = REG_PC - 2;
                Z80Ops::addressOnBus(REG_DE - 1, 5);
            }
            break;
        }
        default: break;
    }
}

//Subconjunto de instrucciones 0xED

void Z80::decodeED(void) {
    switch (opCode) {
        // ===== Z80N (ZX Spectrum Next) extended opcodes =====
        // On non-Next machines these remain 8T NOPs (as unhandled ED opcodes).
        case 0x23: case 0x24: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x8A: case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x98: case 0xA4: case 0xA5: case 0xAC: case 0xB4: case 0xB7: case 0xBC:
            if (Z80Ops::isNext) decodeZ80N();   // .nxovl window; an 8T NOP elsewhere
            break;
        // IN r,(C) / OUT (C),r — one body per direction for the eight registers
        // (2026-09-22; was 16 cases). r = 6 is the undocumented IN (C) (flags only,
        // nothing stored) and OUT (C),0 (an NMOS Z80 puts 0x00 on the bus).
        case 0x40: case 0x48: case 0x50: case 0x58: case 0x60: case 0x68: case 0x70: case 0x78:
        { /* IN r,(C) */
            REG_WZ = REG_BC;
            uint8_t v = Ports::input(REG_WZ++);
            if (__builtin_expect(Rzx::mode != 0, 0)) v = Rzx::onIn(v);
            uint8_t* r = reg8[(opCode >> 3) & 7];
            if (r) *r = v;
            sz5h3pnFlags = sz53pn_addTable[v];
            flagQ = true;
            break;
        }
        case 0x41: case 0x49: case 0x51: case 0x59: case 0x61: case 0x69: case 0x71: case 0x79:
        { /* OUT (C),r */
            uint8_t* r = reg8[(opCode >> 3) & 7];
            Ports::output(REG_BC, r ? *r : 0x00);
            REG_WZ = REG_BC + 1;
            return;
        }
        case 0x42:
        { /* SBC HL,BC */
            Z80Ops::addressOnBusIR(7);
            sbc16(REG_BC);
            break;
        }
        case 0x43:
        { /* LD (nn),BC */
            REG_WZ = Z80Ops::peek16(REG_PC);
            Z80Ops::poke16(REG_WZ, regBC);
            REG_WZ++;
            REG_PC = REG_PC + 2;
            break;
        }
        case 0x44:
        case 0x4C:
        case 0x54:
        case 0x5C:
        case 0x64:
        case 0x6C:
        case 0x74:
        case 0x7C:
        { /* NEG */
            uint8_t aux = regA;
            regA = 0;
            carryFlag = false;
            sbc(aux);
            break;
        }
        case 0x4D:
        { /* RETI */
            ffIFF1 = ffIFF2;
            REG_PC = REG_WZ = pop();
            check_trdos();
            if (Z80Ops::isTsconf && ffIFF1) TsConf::intEnableHook();
            if (Z80Ops::isNext && NextInt::ex) NextInt::reti();   // the source in service leaves it
            break;
        }
        case 0x45:
        case 0x55:
        case 0x5D:
        case 0x65:
        case 0x6D:
        case 0x75:
        case 0x7D:
        { /* RETN */
            ffIFF1 = ffIFF2;
            // zxnext.vhd 4111: while the Multiface is active its RETN pages the
            // MF out and does NOT reach the divMMC (divmmc_retn_seen).
            const bool mfRetn = Z80Ops::isNext && NextMf::active();
            if (Z80Ops::isNext && NextInt::retnArmed) {
                // stackless NMI (NR 0xC0 bit 3): the address comes from NR
                // 0xC2/0xC3, the stack read is suppressed, SP still moves
                VIDEO::Draw(6, false);
                REG_SP = (uint16_t)(REG_SP + 2);
                REG_PC = REG_WZ = NextInt::stacklessRetn();
#if NEXT_TRACE
                nxJumpNote('S');
#endif
            } else {
                REG_PC = REG_WZ = pop();
#if NEXT_TRACE
                if (Z80Ops::isNext) nxJumpNote('R');
#endif
            }
            if (mfRetn) NextMf::retn();
            else if (Z80Ops::isNext && DivMMC::enabled) DivMMC::nextRetn();
            check_trdos();
            if (Z80Ops::isTsconf && ffIFF1) TsConf::intEnableHook();
            // a zxnDMA paused for the NMI (NR 0xCC bit 7) resumes after RETN —
            // BEFORE the next instruction: wake() let one more run, and Atic
            // Atac Remake's copper NMI (every line) then advanced its page-fill
            // loop one instruction per NMI while the fill was still held, until
            // NEXTREG #54 remapped the slot under it and the last 8K landed on
            // its own code page (hw 2026-10-16, [DMAHELD] trace).
            if (Z80Ops::isNext && ((ffIFF1 && NextInt::ex) || Z80DMA::nxPending)) NextInt::wakeNow();
            break;
        }
        case 0x46:
        case 0x4E:
        case 0x66:
        case 0x6E:
        { /* IM 0 */
            modeINT = IntMode::IM0;
            break;
        }
        case 0x47:
        { /* LD I,A */
            /*
             * El par IR se pone en el bus de direcciones *antes*
             * de poner A en el registro I. Detalle importante.
             */
            Z80Ops::addressOnBusIR(1);
            regI = regA;
            break;
        }
        case 0x4A:
        { /* ADC HL,BC */
            Z80Ops::addressOnBusIR(7);
            adc16(REG_BC);
            break;
        }
        case 0x4B:
        { /* LD BC,(nn) */
            REG_WZ = Z80Ops::peek16(REG_PC);
            REG_BC = Z80Ops::peek16(REG_WZ);
            REG_WZ++;
            REG_PC = REG_PC + 2;
            break;
        }
        case 0x4F:
        { /* LD R,A */
            /*
             * El par IR se pone en el bus de direcciones *antes*
             * de poner A en el registro R. Detalle importante.
             */
            Z80Ops::addressOnBusIR(1);
            setRegR(regA);
            break;
        }
        case 0x52:
        { /* SBC HL,DE */
            Z80Ops::addressOnBusIR(7);
            sbc16(REG_DE);
            break;
        }
        case 0x53:
        { /* LD (nn),DE */
            REG_WZ = Z80Ops::peek16(REG_PC);
            Z80Ops::poke16(REG_WZ++, regDE);
            REG_PC = REG_PC + 2;
            break;
        }
        case 0x56:
        case 0x76:
        { /* IM 1 */
            modeINT = IntMode::IM1;
            break;
        }
        case 0x57:
        { /* LD A,I */
            Z80Ops::addressOnBusIR(1);
            regA = regI;
            sz5h3pnFlags = sz53n_addTable[regA];
            if (ffIFF2 && !Z80Ops::isActiveINT()) {
                sz5h3pnFlags |= PARITY_MASK;
            }
            flagQ = true;
            ldairAt = regR + 1;
            break;
        }
        case 0x5A:
        { /* ADC HL,DE */
            Z80Ops::addressOnBusIR(7);
            adc16(REG_DE);
            break;
        }
        case 0x5B:
        { /* LD DE,(nn) */
            REG_WZ = Z80Ops::peek16(REG_PC);
            REG_DE = Z80Ops::peek16(REG_WZ++);
            REG_PC = REG_PC + 2;
            break;
        }
        case 0x5E:
        case 0x7E:
        { /* IM 2 */
            modeINT = IntMode::IM2;
            break;
        }
        case 0x5F:
        { /* LD A,R */
            Z80Ops::addressOnBusIR(1);
            regA = getRegR();
            sz5h3pnFlags = sz53n_addTable[regA];
            if (ffIFF2 && !Z80Ops::isActiveINT()) {
                sz5h3pnFlags |= PARITY_MASK;
            }
            flagQ = true;
            ldairAt = regR + 1;
            break;
        }
        case 0x62:
        { /* SBC HL,HL */
            Z80Ops::addressOnBusIR(7);
            sbc16(REG_HL);
            break;
        }
        case 0x63:
        { /* LD (nn),HL */
            REG_WZ = Z80Ops::peek16(REG_PC);
            Z80Ops::poke16(REG_WZ++, regHL);
            REG_PC = REG_PC + 2;
            break;
        }
        case 0x67:
        { /* RRD */
            // A = A7 A6 A5 A4 (HL)3 (HL)2 (HL)1 (HL)0
            // (HL) = A3 A2 A1 A0 (HL)7 (HL)6 (HL)5 (HL)4
            // Los bits 3,2,1 y 0 de (HL) se copian a los bits 3,2,1 y 0 de A.
            // Los 4 bits bajos que había en A se copian a los bits 7,6,5 y 4 de (HL).
            // Los 4 bits altos que había en (HL) se copian a los 4 bits bajos de (HL)
            // Los 4 bits superiores de A no se tocan. ¡p'habernos matao!
            uint8_t aux = regA << 4;
            REG_WZ = REG_HL;
            uint16_t memHL = Z80Ops::peek8(REG_WZ);
            regA = (regA & 0xf0) | (memHL & 0x0f);
            Z80Ops::addressOnBus(REG_WZ, 4);
            Z80Ops::poke8(REG_WZ++, (memHL >> 4) | aux);
            sz5h3pnFlags = sz53pn_addTable[regA];
            flagQ = true;
            break;
        }
        case 0x6A:
        { /* ADC HL,HL */
            Z80Ops::addressOnBusIR(7);
            adc16(REG_HL);
            break;
        }
        case 0x6B:
        { /* LD HL,(nn) */
            REG_WZ = Z80Ops::peek16(REG_PC);
            REG_HL = Z80Ops::peek16(REG_WZ++);
            REG_PC = REG_PC + 2;
            break;
        }
        case 0x6F:
        { /* RLD */
            // A = A7 A6 A5 A4 (HL)7 (HL)6 (HL)5 (HL)4
            // (HL) = (HL)3 (HL)2 (HL)1 (HL)0 A3 A2 A1 A0
            // Los 4 bits bajos que había en (HL) se copian a los bits altos de (HL).
            // Los 4 bits altos que había en (HL) se copian a los 4 bits bajos de A
            // Los bits 3,2,1 y 0 de A se copian a los bits 3,2,1 y 0 de (HL).
            // Los 4 bits superiores de A no se tocan. ¡p'habernos matao!
            uint8_t aux = regA & 0x0f;
            REG_WZ = REG_HL;
            uint16_t memHL = Z80Ops::peek8(REG_WZ);
            regA = (regA & 0xf0) | (memHL >> 4);
            Z80Ops::addressOnBus(REG_WZ, 4);
            Z80Ops::poke8(REG_WZ++, (memHL << 4) | aux);
            sz5h3pnFlags = sz53pn_addTable[regA];
            flagQ = true;
            break;
        }
        case 0x72:
        { /* SBC HL,SP */
            Z80Ops::addressOnBusIR(7);
            sbc16(REG_SP);
            break;
        }
        case 0x73:
        { /* LD (nn),SP */
            REG_WZ = Z80Ops::peek16(REG_PC);
            Z80Ops::poke16(REG_WZ++, regSP);
            REG_PC = REG_PC + 2;
            break;
        }
        case 0x7A:
        { /* ADC HL,SP */
            Z80Ops::addressOnBusIR(7);
            adc16(REG_SP);
            break;
        }
        case 0x7B:
        { /* LD SP,(nn) */
            REG_WZ = Z80Ops::peek16(REG_PC);
            REG_SP = Z80Ops::peek16(REG_WZ++);
            REG_PC = REG_PC + 2;
            break;
        }
        case 0xA0:
        { /* LDI */
            ldx(1);
            break;
        }
        case 0xA1:
        { /* CPI */
            cpx(1);
            break;
        }
        case 0xA2:
        { /* INI */
#if NEXT_TRACE
            if (Z80Ops::isNext && REG_C == 0xEB) { extern void nxIniBank5Note(uint16_t hl, char k); nxIniBank5Note(REG_HL, 'i'); }
#endif
            inx(1);
            // Next: an unrolled ED A2 chain from the SD data port (the video
            // player in Adventures in Videoland reads 32 INI a pass, ~74k
            // instructions a frame): the following INIs but the last as one loop
            if (Z80Ops::isNext && g_ts_fastmem == 1 && REG_C == 0xEB) nextIniRunBatch();
            break;
        }
        case 0xA3:
        { /* OUTI */
            otx(1);
            break;
        }
        case 0xA8:
        { /* LDD */
            ldx(-1);
            break;
        }
        case 0xA9:
        { /* CPD */
            cpx(-1);
            break;
        }
        case 0xAA:
        { /* IND */
            inx(-1);
            break;
        }
        case 0xAB:
        { /* OUTD */
            otx(-1);
            break;
        }
        case 0xB0:
        { /* LDIR */
            if (Z80Ops::isNext && g_ts_fastmem == 1 && REG_BC > 2) nextLdirBatch(1);
            ldx(1);
            if (REG_BC != 0) {
                REG_PC = REG_PC - 2;
                REG_WZ = REG_PC + 1;
                Z80Ops::addressOnBus(REG_DE - 1, 5);
                sz5h3pnFlags &= ~FLAG_53_MASK;
                sz5h3pnFlags |= (REG_PCh & FLAG_53_MASK);
            }
            break;
        }
        case 0xB1:
        { /* CPIR */
            cpx(1);
            if ((sz5h3pnFlags & PARITY_MASK) == PARITY_MASK
                    && (sz5h3pnFlags & ZERO_MASK) == 0) {
                REG_PC = REG_PC - 2;
                REG_WZ = REG_PC + 1;
                Z80Ops::addressOnBus(REG_HL - 1, 5);
                sz5h3pnFlags &= ~FLAG_53_MASK;
                sz5h3pnFlags |= (REG_PCh & FLAG_53_MASK);
            }
            break;
        }
        case 0xB2:
        { /* INIR */
            if (Z80Ops::isNext && g_ts_fastmem == 1 && REG_C == 0xEB && (REG_B == 0 || REG_B > 2)) nextInirBatch();
            else if (__builtin_expect(DivMMC::zc_enabled, 0) && REG_C == 0x57) zcInirBatch();
            inx(1);
            if (REG_B != 0) {
                REG_PC = REG_PC - 2;
                REG_WZ = REG_PC + 1;
                Z80Ops::addressOnBus(REG_HL - 1, 5);
                SetAbortedINxR_OTxRFlags();
            }
            break;
        }
        case 0xB3:
        { /* OTIR */
            if (Z80Ops::isNext && g_ts_fastmem == 1 && (REG_C == 0x6B || REG_C == 0x0B) && (REG_B == 0 || REG_B > 2)) nextOtirBatch();
            otx(1);
            if (REG_B != 0) {
                REG_PC = REG_PC - 2;
                REG_WZ = REG_PC + 1;
                Z80Ops::addressOnBus(REG_BC, 5);
                SetAbortedINxR_OTxRFlags();
            }
            break;
        }
        case 0xB8:
        { /* LDDR */
            if (Z80Ops::isNext && g_ts_fastmem == 1 && REG_BC > 2) nextLdirBatch(-1);
            ldx(-1);
            if (REG_BC != 0) {
                REG_PC = REG_PC - 2;
                REG_WZ = REG_PC + 1;
                Z80Ops::addressOnBus(REG_DE + 1, 5);
                sz5h3pnFlags &= ~FLAG_53_MASK;
                sz5h3pnFlags |= (REG_PCh & FLAG_53_MASK);
            }
            break;
        }
        case 0xB9:
        { /* CPDR */
            cpx(-1);
            if ((sz5h3pnFlags & PARITY_MASK) == PARITY_MASK
                    && (sz5h3pnFlags & ZERO_MASK) == 0) {
                REG_PC = REG_PC - 2;
                REG_WZ = REG_PC + 1;
                Z80Ops::addressOnBus(REG_HL + 1, 5);
                sz5h3pnFlags &= ~FLAG_53_MASK;
                sz5h3pnFlags |= (REG_PCh & FLAG_53_MASK);
            }
            break;
        }
        case 0xBA:
        { /* INDR */
            inx(-1);
            if (REG_B != 0) {
                REG_PC = REG_PC - 2;
                REG_WZ = REG_PC + 1;
                Z80Ops::addressOnBus(REG_HL + 1, 5);
                SetAbortedINxR_OTxRFlags();
            }
            break;
        }
        case 0xBB:
        { /* OTDR */
            otx(-1);
            if (REG_B != 0) {
                REG_PC = REG_PC - 2;
                REG_WZ = REG_PC + 1;
                Z80Ops::addressOnBus(REG_BC, 5);
                SetAbortedINxR_OTxRFlags();
            }
            break;
        }
        // case 0xDD:
            // prefixOpcode = 0xDD;
            // break;
        // case 0xED:
            // prefixOpcode = 0xED;
            // break;
        // case 0xFD:
            // prefixOpcode = 0xFD;
            // break;
        // default:
        //     break;
    }
}

IRAM_ATTR void Z80::copyToRegister(uint8_t value)
{
    uint8_t* r = reg8[opCode & 0x07];   // 6 = (HL): the documented form copies nowhere
    if (r) *r = value;
}

// LDI/LDD, CPI/CPD, INI/IND, OUTI/OUTD share one body each; d = +1 / -1.
// Spectrum Next, LDIR/LDDR on the fast memory path (g_ts_fastmem == 1): run
// every repeat but the LAST as one byte loop, then the normal ldx() + repeat
// code does the final one, so flags, WZ and the exit timing are the core's.
// Each batched iteration is 21 T and two M1 (R += 2). Bounded by: the 8K slot of
// source and destination, CPU::stFrame (the next interrupt / DMA event inside
// exec_nocheck), and one raster line (the line tick runs at the next M1, so a
// batch may cross at most one boundary). Destination must be a plain RAM page
// (>= 0x4000) or the Layer 2 write window below 0x4000 (no divMMC, no NeoGS
// ZX-DMA); anything else falls back to the per-byte path. Bomb Jack's menu at
// 28 MHz blits its tiles with 16-byte LDIRs into Layer 2 (hw log 2026-10-10).
// Removed general blockRepeat (2026-09-09) was a TS-Conf drain problem; the Next
// renderer has no such queue drain on CPU writes.
// In the .nxovl window: it holds the batch's own per-byte copy loop, and when it
// declines (source in slots 0/1, slot or frame boundary, divMMC slot 0) it is
// called on EVERY LDIR iteration. nextInirBatch / nextSdPollBatch are rare
// (an SD sector, ROM 2's CMD17 poll) and stay in flash (2026-10-11).
NX_OVL_CODE void Z80::nextLdirBatch(int d) {
    if (!z80_in_nocheck || Config::numPcBP > 0) return;
    const uint32_t now = CPU::tstates;
    if (now >= CPU::stFrame) return;
    uint32_t n = (uint32_t)REG_BC - 1;
    const uint32_t room = (CPU::stFrame - now) / 21;
    if (n > room) n = room;
    const uint32_t tpl = VIDEO::tStatesPerLine ? VIDEO::tStatesPerLine : 224;
    const uint32_t line = ((tpl << ESPectrum::multiplicator) / 21);
    if (line > 1 && n > line - 1) n = line - 1;
    const uint16_t hl = REG_HL, de = REG_DE;
    const uint32_t srcLeft = d > 0 ? 0x2000u - (hl & 0x1FFF) : (uint32_t)(hl & 0x1FFF) + 1;
    const uint32_t dstLeft = d > 0 ? 0x2000u - (de & 0x1FFF) : (uint32_t)(de & 0x1FFF) + 1;
    if (n > srcLeft) n = srcLeft;
    if (n > dstLeft) n = dstLeft;
    if (n < 2) return;
    const uint8_t ss = hl >> 13, ds = de >> 13;
    if (ss < 2 && g_ts_fastrd != 2) return;
    const uint8_t* sp = MemESP::ramCurrent[ss];
    uint8_t* dp;
    if (ds >= 2) {
        dp = MemESP::ramCurrent[ds];
        if ((uintptr_t)dp < 0x11000000u) return;          // ROM / accessor
        *mem_desc_t::bank_dirty[ds] = true;
    } else {
        extern volatile uint8_t g_ngs_zxdma;
        if (g_ngs_zxdma) return;
        if (MemESP::divmmc_mapped) {
            // divMMC RAM at 0x2000-0x3FFF (writebyte's rule for slot 1): enNxtmmc
            // moves its FAT/file buffers there with LDIR (0x1939 / 0x1943, ~38%
            // of the PC samples while NextZXOS boots). Slot 0 (ROM / MAPRAM) stays
            // per-byte.
            if (ds != 1) return;
            if (MemESP::page0_hi >= (uint8_t*)0x11000000 && !MemESP::mb02_write_gate) return;
            dp = MemESP::page0_hi;
            if (MemESP::divmmc_hi_dirty) *MemESP::divmmc_hi_dirty = true;
            else if (MemESP::mb02_page_dirty) *MemESP::mb02_page_dirty = true;
        } else {
            if (!((MemESP::wr_overlay_mask >> ds) & 1)) return;
            dp = MemESP::wrOverlay[ds];
        }
    }
    if (!sp) return;
    uint32_t so = hl & 0x1FFF, dof = de & 0x1FFF;
    for (uint32_t i = 0; i < n; i++) {
        dp[dof] = sp[so];
        so += d; dof += d;
    }
    REG_HL = (uint16_t)(hl + (int)n * d);
    REG_DE = (uint16_t)(de + (int)n * d);
    REG_BC = (uint16_t)(REG_BC - n);
    CPU::tstates = now + 21 * n;
    regR += 2 * n;
}

// The data-token poll of a sector read: `IN A,(#EB) / CP #FF / RET NZ / DJNZ` —
// ROM 2 at 0x1928, enNxtmmc (the divMMC ROM, NextZXOS's SD driver) at 0x1F40.
// While core1 fetches the sector (DivMMC c17) every pass reads 0xFF, and at 28 MHz
// that is ~15k IN a frame, each through Ports::input and DivMMC: TX-1696's intro
// load ran at 37-44 FPS with the guest in this loop (PERF_HIST, hw 2026-10-21).
// A real card answers in WALL time, so wait for it in wall time here
// (DivMMC::sdTokenWait) and advance the guest by the passes that time covers —
// the guest still sees the card's latency, it only stops costing an emulated IN
// per 33 T. The pattern is checked in the mapped bytes (the same addresses hold
// other code elsewhere); B counts the passes (DJNZ), so they stop at B = 1 and
// leave the fall-through, the outer counter and any non-FF reply to the
// interpreter. Never across stFrame (the next DMA / NMI / line or CTC interrupt
// event) or the frame end, and not inside the ULA INT window — with EI the loop
// may be interrupted only there, so EI is fine (the driver polls with
// interrupts on: DI only skipped nothing on TX-1696, sdcut5 log). idleAdvanceTo
// renders the lines the skipped time spans, and refuses under ULA contention.
// g_sd_poll[]: calls / passes skipped / us waited / refusals (the [SD2] line).
volatile uint32_t g_sd_poll[4];
// Why a call skipped nothing, and who called the poll (the return address on the
// stack: enNxtmmc 1F39 = R1 after a command, 1F50 = data token, 1FB3 = write data
// response, 1FBC = write busy; other) — the [SD3] line (PERF_TRACE / NEXT_TRACE).
// why: 0 pattern/page, 1 slice/frame end, 2 token wait not applicable (card state),
// 3 answered at once (k 0), 4 idleAdvanceTo refused.
volatile uint32_t g_sd_poll_why[5], g_sd_poll_who[5], g_sd_poll_whok[5];
extern volatile uint8_t g_sd_tw_why;   // DivMMC::sdTokenWait: 0 ok, 1..5 the failing precondition
volatile uint32_t g_sd_tw_whyn[6];
Z80_COLD void Z80::nextSdPollBatch() {
    g_sd_poll[0] = g_sd_poll[0] + 1;
    uint32_t who = 4;
    {
        const uint16_t sp = REG_SP;
        const uint8_t* s0 = MemESP::ramCurrent[sp >> 13];
        const uint8_t* s1 = MemESP::ramCurrent[(uint16_t)(sp + 1) >> 13];
        const uint16_t ret = ((uintptr_t)s0 < 0x10000000u || (uintptr_t)s1 < 0x10000000u) ? 0 :
            (uint16_t)(s0[sp & 0x1FFF] | (s1[(uint16_t)(sp + 1) & 0x1FFF] << 8));
        who = ret == 0x1F39 ? 0 : ret == 0x1F50 ? 1 : ret == 0x1FB3 ? 2 : ret == 0x1FBC ? 3 : 4;
        g_sd_poll_who[who] = g_sd_poll_who[who] + 1;
    }
    if (!z80_in_nocheck || !g_next_fastrd || Z80::isNMI() || (ffIFF1 && (int32_t)CPU::tstates < CPU::IntEnd) ||
        Config::numPcBP || Config::numPortReadBP || Config::numMemReadBP || Rzx::mode ||
        !DivMMC::enabled || DivMMC::divide_mode || !NextReg::portOn(NextReg::PE_SPI)) { g_sd_poll[3] = g_sd_poll[3] + 1; return; }
    const uint16_t in = (uint16_t)(REG_PC - 2);
    if ((in & 0x1FFF) > 0x1FF9) { g_sd_poll_why[0] = g_sd_poll_why[0] + 1; return; }
    const uint8_t* p = MemESP::ramCurrent[in >> 13];
    if ((uintptr_t)p < 0x10000000u) { g_sd_poll_why[0] = g_sd_poll_why[0] + 1; return; }
    p += in & 0x1FFF;
    if (p[0] != 0xDB || p[1] != 0xEB || p[2] != 0xFE || p[3] != 0xFF ||
        p[4] != 0xC0 || p[5] != 0x10 || p[6] != 0xF9) { g_sd_poll_why[0] = g_sd_poll_why[0] + 1; return; }
    // One pass: CP FF 7, RET NZ not taken 5, DJNZ taken 13, IN A,(n) M1 4 +
    // immediate 3 + the early #EB port path's 1 = 33 T, 4 fetches.
    constexpr uint32_t P = 33;
    const uint32_t t = CPU::tstates;
    uint32_t lim = CPU::statesInFrame - (uint32_t)CPU::IntEnd;   // NextPorts::pollSkip's bound
    if (CPU::stFrame && CPU::stFrame < lim) lim = CPU::stFrame;
    if (lim <= t + P) { g_sd_poll_why[1] = g_sd_poll_why[1] + 1; return; }
    uint32_t kmax = (REG_B ? (uint32_t)REG_B : 256u) - 1;
    if ((lim - t) / P < kmax) kmax = (lim - t) / P;
    // The card answers 0xFF to every read from here on (enNxtmmc's write-busy
    // wait, 12800 passes after each sector write that only a timeout ends): the
    // passes are known without waiting — skip all that B and the slice allow.
    if (kmax && DivMMC::sdReadsIdleFF()) {
        if (!NEXTVID::idleAdvanceTo(t + kmax * P)) { g_sd_poll_why[4] = g_sd_poll_why[4] + 1; return; }
        g_sd_poll[1] = g_sd_poll[1] + kmax;
        g_sd_poll_whok[who] = g_sd_poll_whok[who] + kmax;
        cp(0xFF);
        REG_B = (uint8_t)(REG_B - kmax);
        REG_PC = (uint16_t)(in + 2);
        REG_WZ = 0xFFEC;
        regR += 4 * kmax;
        flagQ = false;
        opCode = 0xDB;
        return;
    }
    // A short span is not worth a wait: the call itself costs about one pass
    // (sdcut6 log: 65k calls a second skipping 0.8 pass each).
    if (kmax < 16) { g_sd_poll[3] = g_sd_poll[3] + 1; return; }
    // T-states per microsecond in Q8: 3.5 MHz << multiplicator (32-bit: kmax*P*256
    // < 2.2M, waited*tq8 < 2^32 for any wait this bound allows)
    const uint32_t tq8 = 896u << ESPectrum::multiplicator;
    const uint32_t waited = DivMMC::sdTokenWait(kmax * P * 256u / tq8);
    g_sd_tw_whyn[g_sd_tw_why] = g_sd_tw_whyn[g_sd_tw_why] + 1;
    uint32_t k = (waited * (tq8 >> 3) / 32u + P - 1) / P;
    if (k > kmax) k = kmax;
    g_sd_poll[2] = g_sd_poll[2] + waited;
    if (!k) { const uint32_t w = g_sd_tw_why ? 2 : 3; g_sd_poll_why[w] = g_sd_poll_why[w] + 1; return; }
    if (!NEXTVID::idleAdvanceTo(t + k * P)) { g_sd_poll_why[4] = g_sd_poll_why[4] + 1; return; }
    g_sd_poll[1] = g_sd_poll[1] + k;
    g_sd_poll_whok[who] = g_sd_poll_whok[who] + k;
    cp(0xFF);
    REG_B = (uint8_t)(REG_B - k);
    REG_PC = (uint16_t)(in + 2);
    REG_WZ = 0xFFEC;
    regR += 4 * k;
    flagQ = false;
    opCode = 0xDB;
}

// Spectrum Next, INIR from the SD SPI data port 0xEB (enNxtmmc / NextZXOS read a
// sector as INIR x2 into RAM, ~9700 port reads a frame while booting at 28 MHz):
// every repeat but the last as one loop of DivMMC::mmc_read() straight into the
// destination page; the normal inx() + repeat code does the final one (flags,
// WZ, exit timing). Each batched iteration costs what the per-byte path charges
// now: M1 4 + 4, IR 1, the early-return 0xEB read 1 (Ports::input does not add
// the late 3 T there), the write 3, the repeat 5 = 18 T, R += 2. Same bounds as
// nextLdirBatch; the port must answer as Ports::input's early 0xEB branch does
// (DivMMC SPI on, NR 0x83 SPI enable), no port breakpoints, no RZX playback.
Z80_COLD void Z80::nextInirBatch() {
    if (!z80_in_nocheck || Config::numPcBP > 0 || Config::numPortReadBP > 0 || Rzx::mode) return;
    if (!DivMMC::enabled || DivMMC::divide_mode || !NextReg::portOn(NextReg::PE_SPI)) return;
    const uint32_t now = CPU::tstates;
    if (now >= CPU::stFrame) return;
    constexpr uint32_t T = 18;
    uint32_t n = (REG_B ? (uint32_t)REG_B : 256u) - 1;   // B = 0: 256 bytes (enNxtmmc: BC = 0x00EB)
    const uint32_t room = (CPU::stFrame - now) / T;
    if (n > room) n = room;
    const uint32_t tpl = VIDEO::tStatesPerLine ? VIDEO::tStatesPerLine : 224;
    const uint32_t line = (tpl << ESPectrum::multiplicator) / T;
    if (line > 1 && n > line - 1) n = line - 1;
    const uint16_t hl = REG_HL;
    const uint32_t dstLeft = 0x2000u - (hl & 0x1FFF);
    if (n > dstLeft) n = dstLeft;
    if (n < 2) return;
    const uint8_t ds = hl >> 13;
    uint8_t* dp;
    if (ds >= 2) {
        dp = MemESP::ramCurrent[ds];
        if ((uintptr_t)dp < 0x11000000u) return;          // ROM / accessor
        *mem_desc_t::bank_dirty[ds] = true;
    } else {
        // divMMC RAM at 0x2000-0x3FFF (writebyte's rule for slot 1, as
        // nextLdirBatch): enNxtmmc reads sectors into its own buffer there
        // (0x1F6E INIR x2, HL = IX) — 512 trips through the whole port path a
        // sector otherwise (TX-1696's load, hw 2026-10-21). Slot 0 stays per byte.
        if (ds != 1 || !MemESP::divmmc_mapped) return;
        { extern volatile uint8_t g_ngs_zxdma; if (g_ngs_zxdma) return; }
        if (MemESP::page0_hi >= (uint8_t*)0x11000000 && !MemESP::mb02_write_gate) return;
        dp = MemESP::page0_hi;
        if (MemESP::divmmc_hi_dirty) *MemESP::divmmc_hi_dirty = true;
        else if (MemESP::mb02_page_dirty) *MemESP::mb02_page_dirty = true;
    }
    uint32_t o = hl & 0x1FFF;
    for (uint32_t i = 0; i < n; ) {           // data bytes in runs, the protocol bytes one by one
        const uint32_t k = DivMMC::mmc_read_bulk(dp + o, n - i);
        if (k) { o += k; i += k; }
        else { dp[o++] = DivMMC::mmc_read(); i++; }
    }
    REG_HL = (uint16_t)(hl + n);
    REG_B = (uint8_t)(REG_B - n);
    CPU::tstates = now + T * n;
    regR += 2 * n;
}

// Z-Controller, INIR from the SD data port #57 (ZX-Evo TS-Conf / BaseConf, ATM,
// Pentagon / Scorpion with ZC): TS-BIOS, esxDOS's ZC driver, NedoOS and most
// loaders read a sector as INIR x2, i.e. 512 trips through the whole
// Ports::input decode. Every repeat but the last as one loop of
// DivMMC::mmc_read() straight into the destination page (mmc_read_bulk for the
// data bytes); inx() runs the final one, so flags, WZ and the exit timing are
// the core's own — the nextInirBatch shape.
// The per-iteration cost is MEASURED, not assumed: it differs by machine (ATM/Evo
// add the late 3 T of the I/O cycle, the generic decode does not) and it is
// exactly the CPU::tstates distance between two consecutive entries into this
// INIR — when the previous iteration was this same instruction (PC, B - 1), it
// read #57 once (zc_in_bytes + 1) and nothing else ran in between (16..40 T).
// The first two iterations of every INIR therefore run normally. Only where that
// cost repeats exactly: no contended memory anywhere, no DRAM wait model (TS-Conf
// 14 MHz), no even-M1 (Scorpion Yellow); destinations in slots 2..7 without a
// write gate (ROM, TS-Conf FMAddr, ATM read-only window / Evo font, Timex MMU);
// one raster line at most per call (Draw handles one line crossing), not past the
// slice end; not into the beam-raced ZX screen page.
static uint16_t s_zcPc = 0;
static uint8_t  s_zcB = 0;
static uint32_t s_zcT = 0;
static uint32_t s_zcIn = 0;

Z80_COLD void Z80::zcInirBatch() {
    const uint32_t now = CPU::tstates;
    const uint16_t pc = REG_PC;
    uint32_t T = 0;
    if (pc == s_zcPc && (uint8_t)(s_zcB - 1) == REG_B && DivMMC::zc_in_bytes == s_zcIn + 1 && now > s_zcT)
        T = now - s_zcT;
    do {
        if (T < 16 || T > 40) break;
        if (!z80_in_nocheck || Config::numPcBP > 0 || Config::numPortReadBP > 0 ||
            Config::numMemWriteBP > 0 || Rzx::mode) break;
        if (!DivMMC::cardSelected() || Z80Ops::isProfi || Z80Ops::isNext) break;
        if (g_ts_memcyc || g_scorp_even_m1 || g_timex_mmu || (g_tsconf_wr & 0x10)) break;
        bool cont = false;
        for (int i = 0; i < 8; i++) cont |= MemESP::ramContended[i];
        if (cont) break;
        if (now >= CPU::stFrame) break;
        uint32_t n = (REG_B ? (uint32_t)REG_B : 256u) - 1;
        const uint32_t room = (CPU::stFrame - now) / T;
        if (n > room) n = room;
        const uint32_t tpl = VIDEO::tStatesPerLine ? VIDEO::tStatesPerLine : 224;
        const uint32_t line = (tpl << ESPectrum::multiplicator) / T;
        if (line > 1 && n > line - 1) n = line - 1;
        const uint16_t hl = REG_HL;
        const uint32_t dstLeft = 0x2000u - (hl & 0x1FFF);
        if (n > dstLeft) n = dstLeft;
        if (n < 2) break;
        const uint8_t ds = hl >> 13;
        if (ds < 2) break;
        if ((g_atm_ro & 0x80) || (g_atm_ro & (1u << (hl >> 14)))) break;
        uint8_t* dp = MemESP::ramCurrent[ds];
        if ((uintptr_t)dp < 0x11000000u) break;       // ROM / accessor bank
        if (!g_ts_fastmem && VIDEO::grmem && dp >= VIDEO::grmem && dp < VIDEO::grmem + 0x4000) break;
        *mem_desc_t::bank_dirty[ds] = true;
        uint32_t o = hl & 0x1FFF;
        for (uint32_t i = 0; i < n; ) {           // data bytes in runs, the protocol bytes one by one
            const uint32_t k = DivMMC::mmc_read_bulk(dp + o, n - i);
            if (k) { o += k; i += k; }
            else { dp[o++] = DivMMC::mmc_read(); i++; }
        }
        DivMMC::zc_in_bytes += n;
        REG_HL = (uint16_t)(hl + n);
        REG_B = (uint8_t)(REG_B - n);
        regR += 2 * n;
        const uint32_t total = T * n;
        if (g_ts_fastmem) tsFastTick(total);
        else VIDEO::Draw(total, false);
    } while (0);
    // The state the next entry measures against: this one, after any batch.
    s_zcPc = pc; s_zcB = REG_B; s_zcT = CPU::tstates; s_zcIn = DivMMC::zc_in_bytes;
}

// Spectrum Next, an unrolled run of INI from the SD data port 0xEB (ED A2 x N):
// called after the INI that started it, executes the following INIs of the run
// but the LAST one as one loop of DivMMC::mmc_read() into the destination page,
// and leaves PC on that last INI, which runs through decodeED as before (its
// flags are the only ones the program can see: each INI overwrites them). Each
// batched INI costs what the per-instruction path charges: M1 4 + 4, IR 1, the
// early-return 0xEB read 1, the write 3 = 13 T, R += 2; B decrements per byte.
// Same bounds and preconditions as nextInirBatch (slice end, one raster line,
// the destination's 8K slot, the code in a plain RAM slot).
#if NEXT_TRACE
// [INI5]: SD sectors (INI from #EB) landing in 16K bank 5 — one line per new
// 512-byte block of the destination, with the card address being read.
void nxIniBank5Note(uint16_t hl, char k) {
    static uint16_t left = 160;
    static uint32_t last = 0xFFFFFFFFu;
    const uint8_t pg = MemESP::mmu[hl >> 13];
    if (!left || (pg != 0x0A && pg != 0x0B)) return;
    const uint32_t key = ((uint32_t)pg << 16) | ((hl & 0x1FFF) >> 9);
    if (key == last) return;
    last = key;
    left--;
    Debug::log("[INI5] %c pc=%04X hl=%04X pg%02X off=%04X t=%lu", k, Z80::getRegPC(), hl, pg, hl & 0x1FFF,
               (unsigned long)CPU::tstates);
}
#endif

Z80_COLD void Z80::nextIniRunBatch() {
    if (!z80_in_nocheck || Config::numPcBP > 0 || Config::numPortReadBP > 0 || Rzx::mode) return;
    if (!DivMMC::enabled || DivMMC::divide_mode || !NextReg::portOn(NextReg::PE_SPI)) return;
    const uint16_t pc = REG_PC;
    const uint8_t cs = pc >> 13;
    const uint8_t* cp = MemESP::ramCurrent[cs];
    if (cs < 2 || (uintptr_t)cp < 0x11000000u) return;   // ROM / accessor / divMMC area
    // Every refusal that does not depend on the run comes BEFORE the scan: this
    // runs after EVERY INI from 0xEB, and scanning the rest of a 512-INI block
    // only to refuse it made a sector cost ~130k byte reads of PSRAM-backed code
    // (Atic Atac Remake loads into MMU slot 0 with HL = 0: 76% of core0, 300 ms
    // frames, hw 2026-10-16).
    const uint16_t hl = REG_HL;
#if NEXT_TRACE
    { extern void nxIniBank5Note(uint16_t hl, char k); nxIniBank5Note(hl, 'b'); }
#endif
    const uint8_t ds = hl >> 13;
    // The destination resolved the way MemESP::writebyte resolves it (as
    // Z80DMA's nxWrRun): the divMMC window in slots 0/1 and the Next ROM take
    // the byte path, the Layer 2 write-over writes its own buffer, plain RAM in
    // any slot is batched — the loader above reads sectors to HL = 0.
    uint8_t* dp;
    if (ds <= 1 && MemESP::divmmc_mapped) return;
    if ((MemESP::wr_overlay_mask >> ds) & 1) dp = MemESP::wrOverlay[ds];
    else {
        if (ds <= 1 && (MemESP::next_rom_mask & (1u << ds))) return;
        dp = MemESP::ramCurrent[ds];
        if ((uintptr_t)dp < 0x11000000u) return;          // ROM / accessor / open bus
        *mem_desc_t::bank_dirty[ds] = true;
    }
    const uint32_t now = CPU::tstates;
    if (now >= CPU::stFrame) return;
    constexpr uint32_t T = 13;
    uint32_t lim = (CPU::stFrame - now) / T;               // batched INIs this call may take
    const uint32_t tpl = VIDEO::tStatesPerLine ? VIDEO::tStatesPerLine : 224;
    const uint32_t line = (tpl << ESPectrum::multiplicator) / T;
    if (line > 1 && lim > line - 1) lim = line - 1;
    const uint32_t dstLeft = 0x2000u - (hl & 0x1FFF);
    if (lim > dstLeft) lim = dstLeft;
    if (lim < 1) return;
    // the run: consecutive ED A2 from PC, inside the code's 8K slot, counted no
    // further than lim + 1 (the last one of the run executes normally)
    uint32_t run = 0;
    uint32_t co = pc & 0x1FFF;
    while (run <= lim && co + 1 < 0x2000u && cp[co] == 0xED && cp[co + 1] == 0xA2) { run++; co += 2; }
    if (run < 2) return;
    uint32_t n = run - 1;
    if (n > lim) n = lim;
    LED::touchR(LED::SD);
    uint32_t o = hl & 0x1FFF;
    for (uint32_t i = 0; i < n; ) {           // data bytes in runs, the protocol bytes one by one
        const uint32_t k = DivMMC::mmc_read_bulk(dp + o, n - i);
        if (k) { o += k; i += k; }
        else { dp[o++] = DivMMC::mmc_read(); i++; }
    }
    REG_HL = (uint16_t)(hl + n);
    REG_B = (uint8_t)(REG_B - n);
    REG_PC = (uint16_t)(pc + 2 * n);
    CPU::tstates = now + T * n;
    regR += 2 * n;
}

// OTIR into the zxnDMA (port 0x6B, or 0x0B in Zilog mode): the repeated
// iterations as one loop, the LAST one left to otx() (flags, WZ, exit timing).
// STNICCC Filled8bit programs one 13-byte DMA fill per polygon span, ~540 a
// frame, ~7000 OUTs — each through the dispatcher, otx, the port decode and the
// repeat's flag fix-up, ~7% of core0 (hw 2026-10-15). Each batched iteration
// costs what the per-byte path charges: M1 4 + 4, IR 1, the read 3, the OUT 4,
// the repeat 5 = 21 T, R += 2; the T-states are advanced BEFORE each byte so a
// LOAD/ENABLE that starts a transfer sizes it from the right time. Same bounds
// as nextLdirBatch (slice end, one line, the source's 8K slot); the source must
// be a plain RAM page at 0x4000+ and the port decoded (NR 0x82-0x85).
NX_OVL_CODE void Z80::nextOtirBatch() {
    if (!z80_in_nocheck || Config::numPcBP > 0 || Config::numPortWriteBP > 0 || Rzx::mode) return;
    const bool zilog = REG_C == 0x0B;
    if (!NextReg::portOn(zilog ? NextReg::PE_DMA0B : NextReg::PE_DMA6B)) return;
    const uint32_t now = CPU::tstates;
    if (now >= CPU::stFrame) return;
    constexpr uint32_t T = 21;
    uint32_t n = (REG_B ? (uint32_t)REG_B : 256u) - 1;
    const uint32_t room = (CPU::stFrame - now) / T;
    if (n > room) n = room;
    const uint32_t tpl = VIDEO::tStatesPerLine ? VIDEO::tStatesPerLine : 224;
    const uint32_t line = (tpl << ESPectrum::multiplicator) / T;
    if (line > 1 && n > line - 1) n = line - 1;
    const uint16_t hl = REG_HL;
    const uint32_t srcLeft = 0x2000u - (hl & 0x1FFF);
    if (n > srcLeft) n = srcLeft;
    if (n < 2) return;
    const uint8_t ss = hl >> 13;
    if (ss < 2) return;
    const uint8_t* sp = MemESP::ramCurrent[ss];
    if ((uintptr_t)sp < 0x11000000u) return;          // ROM / accessor
    Z80DMA::nextZ80Mode = zilog;
    LED::touchW(LED::DMA);
    uint32_t o = hl & 0x1FFF;
    uint32_t i = 0;
    while (i < n) {
        CPU::tstates += T;
        Z80DMA::writePort(sp[o++]);
        i++;
        // A LOAD/ENABLE may have run a transfer to the frame end or pulled the
        // slice end down (NextInt::wake): the rest goes through the dispatcher.
        if (CPU::tstates >= CPU::stFrame) break;
    }
    n = i;
    REG_HL = (uint16_t)(hl + n);
    REG_B = (uint8_t)(REG_B - n);
    regR += 2 * n;
}

IRAM_ATTR void Z80::ldx(int d) {
    uint8_t work8 = Z80Ops::peek8(REG_HL);
    Z80Ops::poke8(REG_DE, work8);
    Z80Ops::addressOnBus(REG_DE, 2);
    REG_HL += d; REG_DE += d; REG_BC--;
    work8 += regA;
    sz5h3pnFlags = (sz5h3pnFlags & FLAG_SZ_MASK) | (work8 & BIT3_MASK);
    if ((work8 & ADDSUB_MASK) != 0) sz5h3pnFlags |= BIT5_MASK;
    if (REG_BC != 0) sz5h3pnFlags |= PARITY_MASK;
    flagQ = true;
}
IRAM_ATTR void Z80::cpx(int d) {
    uint8_t memHL = Z80Ops::peek8(REG_HL);
    bool carry = carryFlag;
    cp(memHL);
    carryFlag = carry;
    Z80Ops::addressOnBus(REG_HL, 5);
    REG_HL += d; REG_BC--;
    memHL = regA - memHL - ((sz5h3pnFlags & HALFCARRY_MASK) != 0 ? 1 : 0);
    sz5h3pnFlags = (sz5h3pnFlags & FLAG_SZHN_MASK) | (memHL & BIT3_MASK);
    if ((memHL & ADDSUB_MASK) != 0) sz5h3pnFlags |= BIT5_MASK;
    if (REG_BC != 0) sz5h3pnFlags |= PARITY_MASK;
    REG_WZ += d;
    flagQ = true;
}
IRAM_ATTR void Z80::inx(int d) {
    REG_WZ = REG_BC;
    Z80Ops::addressOnBusIR(1);
    uint8_t work8 = Ports::input(REG_WZ);
    if (__builtin_expect(Rzx::mode != 0, 0)) work8 = Rzx::onIn(work8);
    REG_WZ += d;
    Z80Ops::poke8(REG_HL, work8);
    REG_B--; REG_HL += d;
    sz5h3pnFlags = sz53pn_addTable[REG_B];
    if (work8 > 0x7f) sz5h3pnFlags |= ADDSUB_MASK;
    carryFlag = false;
    uint16_t tmp = work8 + ((REG_C + d) & 255);
    if (tmp > 0xff) { sz5h3pnFlags |= HALFCARRY_MASK; carryFlag = true; }
    if ((sz53pn_addTable[((tmp & 0x07) ^ REG_B)] & PARITY_MASK) == PARITY_MASK) sz5h3pnFlags |= PARITY_MASK;
    else sz5h3pnFlags &= ~PARITY_MASK;
    flagQ = true;
}
IRAM_ATTR void Z80::otx(int d) {
    Z80Ops::addressOnBusIR(1);
    REG_B--;
    REG_WZ = REG_BC;
    uint8_t work8 = Z80Ops::peek8(REG_HL);
    Ports::output(REG_WZ, work8);
    REG_WZ += d; REG_HL += d;
    carryFlag = false;
    sz5h3pnFlags = (work8 > 0x7f) ? sz53n_subTable[REG_B] : sz53n_addTable[REG_B];
    if ((REG_L + work8) > 0xff) { sz5h3pnFlags |= HALFCARRY_MASK; carryFlag = true; }
    if ((sz53pn_addTable[(((REG_L + work8) & 0x07) ^ REG_B)] & PARITY_MASK) == PARITY_MASK) sz5h3pnFlags |= PARITY_MASK;
    flagQ = true;
}

void Z80::SetAbortedINxR_OTxRFlags() {

    sz5h3pnFlags &= ~FLAG_53_MASK;
    sz5h3pnFlags |= (REG_PCh & FLAG_53_MASK);

    // // 1st implementation
    // uint8_t pf = sz5h3pnFlags & PARITY_MASK;
    // if (carryFlag) {
    //     int addsub = 1 - (sz5h3pnFlags & ADDSUB_MASK);
    //     pf = pf ^ sz53pn_addTable[(REG_B + addsub) & 0x07] ^ PARITY_MASK;
    //     if ((REG_B & 0x0F) == (addsub != 1 ? 0x00 : 0x0F )) sz5h3pnFlags |= HALFCARRY_MASK; else sz5h3pnFlags &= ~HALFCARRY_MASK;
    // } else {
    //     pf = pf ^ sz53pn_addTable[REG_B & 0x07] ^ PARITY_MASK;
    // }
    // if (pf & PARITY_MASK) sz5h3pnFlags |= PARITY_MASK; else sz5h3pnFlags &= ~PARITY_MASK;

    // // 2nd implementation
    // uint8_t pf = sz5h3pnFlags & PARITY_MASK;
    // int addsub = carryFlag ? 1 - (sz5h3pnFlags & ADDSUB_MASK): 0;
    // if (addsub)
    //     if ((REG_B & 0x0F) == (addsub > 0 ? 0xF : 0x0 )) sz5h3pnFlags |= HALFCARRY_MASK; else sz5h3pnFlags &= ~HALFCARRY_MASK;
    // pf ^= sz53pn_addTable[(REG_B + addsub) & 0x07] ^ PARITY_MASK;
    // if (pf & PARITY_MASK) sz5h3pnFlags |= PARITY_MASK; else sz5h3pnFlags &= ~PARITY_MASK;

    // 3rd implementation
    uint8_t cpyB = REG_B;
    if (carryFlag) {
        cpyB += sz5h3pnFlags & ADDSUB_MASK ? -1 : 1;
        sz5h3pnFlags = (cpyB ^ REG_B) & HALFCARRY_MASK ? sz5h3pnFlags | HALFCARRY_MASK : sz5h3pnFlags & ~HALFCARRY_MASK;
    }
    uint8_t pf = (sz5h3pnFlags & PARITY_MASK) ^ sz53pn_addTable[cpyB & 0x07] ^ PARITY_MASK;
    sz5h3pnFlags = pf & PARITY_MASK ? sz5h3pnFlags | PARITY_MASK : sz5h3pnFlags & ~PARITY_MASK;

}
