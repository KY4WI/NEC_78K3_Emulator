/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 Dave, KY4WI
 */

/* emu78k3 -- instruction-level emulator for the NEC 78K/III core
 * (uPD78312A), built for compiler regression testing: run a small
 * standalone binary produced by vbcc78k3+vasm78k3+vlink78k3, and check
 * its final register/memory state against what the C source says it
 * should be. This is NOT a hardware simulator (no timers, no SFRs, no
 * interrupts, no EEPROM/panel/PLL peripherals) -- it is a CPU-only
 * interpreter for exercising the compiler's own code generation.
 *
 * Decoder instructions come from this repository's own copies of the
 * project's NEC 78K/III opcode definitions. Companion assembler and
 * disassembler repositories maintain corresponding definitions, which
 * should be kept consistent and cross-tested; no shared build dependency
 * ensures that changes propagate between separate repositories.
 *
 * Register architecture (which byte is high/low in AX/BC/DE/HL/VP/UP,
 * and the R0-R15/RP0-RP7 storage-slot numbering) is taken directly from
 * the NEC uPD7831xA/78P31xA datasheet, Figure 2 "Register Designation
 * and Storage" (p.7-10) and the Program Status Word figure (p.7-11),
 * cross-checked against shared/RegisterNames.h's already-verified field
 * tables -- not guessed or inferred from other CPU families. Conditional
 * branch flag conditions (BE/BNE/BLT/BGE/BLE/BGT/BL/BNL/BH/BNH) are taken
 * from vbcc78k3's own machine.c comment, which cites NEC User's Manual
 * sec 6.20 by name.
 *
 * Anywhere this file's behavior is a judgment call rather than a
 * confirmed fact (MULUW/DIVUX's 32-bit AX:DE pairing, RETI not touching
 * PSW, PUSH/POP bit-scan order, register-bank base addressing under
 * FE80h) is flagged in a comment at the point of the choice. None of
 * these affect the vbcc78k3-emitted instruction subset in its normal,
 * single-register-bank (RB0), non-interrupt configuration -- which is
 * what this emulator exists to test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "Types.h"
#include "Mnemonic.h"
#include "Operand.h"
#include "Opcode.h"
#include "Opcodes.h"
#include "Addressing.h"

/* ==================== Memory ==================== */

static byte mem[65536];

/* ==================== CPU state ==================== */

static uint16 g_pc;
/* PSW, split as the datasheet's own figure splits it:
 *   PSWH (bits 15-8): 0 RB2 RB1 RB0 0 0 IE 0
 *   PSWL (bits  7-0): S Z RSS AC UF P/V SUB CY   */
static byte g_pswh;
static byte g_pswl;

#define PSWL_S   0x80
#define PSWL_Z   0x40
#define PSWL_RSS 0x20
#define PSWL_AC  0x10
#define PSWL_UF  0x08
#define PSWL_PV  0x04
#define PSWL_SUB 0x02
#define PSWL_CY  0x01

#define PSWH_RB_SHIFT 4  /* RB2:RB1:RB0 occupy bits 6:5:4 */
#define PSWH_RB_MASK  0x70
#define PSWH_IE       0x02

static long g_insn_count = 0;
static long g_max_insns = 20000000;
static bool g_trace = false;
static bool g_halted = false;
static int  g_halt_reason = 0; /* 0=max-insns, 1=BRK, 2=illegal, 3=unimpl, 4=self-jump */

static inline bool getflag(byte mask) { return (g_pswl & mask) != 0; }
static inline void setflag(byte mask, bool v) { if (v) g_pswl |= mask; else g_pswl &= (byte)~mask; }

static inline int cur_bank(void) { return (g_pswh & PSWH_RB_MASK) >> PSWH_RB_SHIFT; }
static inline bool cur_rss(void) { return getflag(PSWL_RSS); }

/* SP is SFR FFFCh -- genuinely memory-mapped there, not a separate
 * hidden register (this project's own crt0.s sets it up as
 * "movw sfr[0xfc].w,#0xfe20", a real-evidence-backed address from
 * earlier disassembly work). Backing it with actual mem[] storage
 * (rather than a separate C variable) isn't just architectural fidelity:
 * it's required for decode correctness here, because the generic SFRP
 * MOVW opcode-table entry (op=0x0B, subOpMask=0, so it matches ANY SFR
 * address including 0xFC) appears BEFORE the dedicated OT_SP entries in
 * shared/Opcodes.h's opps[] array, so the linear-scan decoder always
 * matches "movw sfr[0xfc].w,#word" as a generic SFRP access, never as
 * the dedicated OP_SP form -- the two must resolve to the same storage
 * or SP updates written the project's own idiomatic way would silently
 * go nowhere. */
static inline uint16 get_sp(void) { return (uint16)(mem[0xFFFC] | (mem[0xFFFD] << 8)); }
static inline void set_sp(uint16 v) { mem[0xFFFC] = (byte)(v & 0xFF); mem[0xFFFD] = (byte)(v >> 8); }

/* Register-bank storage really does live in RAM at FE80h-FEFFh (8 banks
 * x 16 bytes) per the datasheet's own memory map (Figure 1) -- it is not
 * a separate register file we're choosing to model that way, it IS
 * memory on real hardware. Bank 0 is placed at the low end of that
 * window here (FE80h+bank*16); the datasheet's Figure 2 draws bank 7 at
 * FE80h and bank 0 at the FEFFh end, which would reverse this. That
 * ordering has not been independently re-confirmed and is irrelevant to
 * every currently-planned test: the vbcc78k3 code generator never emits
 * SEL (survey of machine.c's emitted mnemonic set), so every
 * compiler-generated program runs entirely in whatever bank RB2-0 resets
 * to (bank 0 here, by this file's reset choice). Only hand-written test
 * code that deliberately exercises SEL could ever observe this choice. */
static inline uint16 reg_bank_base(int bank) { return (uint16)(0xFE80 + bank * 16); }
static inline uint16 reg_addr(int n) { return (uint16)(reg_bank_base(cur_bank()) + (n & 0xF)); }

static inline byte reg_r(int n) { return mem[reg_addr(n)]; }
static inline void set_reg_r(int n, byte v) { mem[reg_addr(n)] = v; }

/* RP slot -> word, high byte = R(2*slot+1), low byte = R(2*slot) --
 * confirmed directly from the datasheet's Figure 2 (e.g. RP0 = A[R1]:X[R0],
 * RP6 "DE" = D[R13]:E[R12] high:low, RP7 "HL" = H[R15]:L[R14] high:low). */
static inline uint16 reg_rp(int slot) {
    byte hi = reg_r(2 * slot + 1);
    byte lo = reg_r(2 * slot);
    return (uint16)((hi << 8) | lo);
}
static inline void set_reg_rp(int slot, uint16 v) {
    set_reg_r(2 * slot + 1, (byte)(v >> 8));
    set_reg_r(2 * slot, (byte)(v & 0xFF));
}

/* Accumulator (A/X/AX): RSS selects RP0 (bank-normal) vs RP2 (RSS=1)
 * storage -- datasheet p.7-10 text: "Setting the RSS bit in the PSW to 1
 * transfers the AX and BC registers from their normal RP0 and RP1
 * positions to RP2 and RP3". BC works the same way via bc_rp(). */
static inline int acc_rp(void) { return cur_rss() ? 2 : 0; }
static inline int bc_rp(void)  { return cur_rss() ? 3 : 1; }
static inline byte get_A(void) { return reg_r(2 * acc_rp() + 1); }
static inline byte get_X(void) { return reg_r(2 * acc_rp()); }
static inline void set_A(byte v) { set_reg_r(2 * acc_rp() + 1, v); }
static inline void set_X(byte v) { set_reg_r(2 * acc_rp(), v); }
static inline uint16 get_AX(void) { return reg_rp(acc_rp()); }
static inline void set_AX(uint16 v) { set_reg_rp(acc_rp(), v); }

/* OT_R / OT_R1 / OT_R2 field values already ARE the R0-R15 storage-slot
 * number (confirmed: shared/RegisterNames.h's REGNAMES_R/R1/R2 tables
 * transcribe 1:1 onto the datasheet's Figure 2 slot numbering -- e.g.
 * R1='a', matching "A[R1]"). OT_RP's field value already IS the RP
 * storage slot too (ax=0=RP0 ... hl=7=RP7). OT_RP1/OT_RP1P and OT_RP2
 * use a DIFFERENT field<->slot mapping (confirmed against
 * RegisterNames.h's REGNAMES_RP1/RP2 tables, which differ from RP's). */
static const int RP1_FIELD_TO_SLOT[8] = { 0, 4, 1, 5, 2, 6, 3, 7 }; /* ax,vp,bc,up,ax',de,bc',hl */
static const int RP2_FIELD_TO_SLOT[4] = { 4, 5, 6, 7 };             /* vp,up,de,hl */
static const int R2_FIELD_TO_SLOT[2]  = { 2, 3 };                   /* c,b */

/* ==================== Fetch/decode ==================== */

typedef struct {
    const Opcode *op;
    byte totalBytes;
    byte bytes[5];
    uint16 addr; /* address this instruction was fetched from */
} Instance;

static uint16 oi_get_word(int byteOffset, const byte bytes[5]) {
    return (uint16)(bytes[byteOffset] | (bytes[byteOffset + 1] << 8));
}
static byte oi_get_masked_byte(const Operand *o, const byte bytes[5]) {
    return (byte)((bytes[o->byteOffset] & o->opMask) >> o->opShift);
}

/* Decoder logic mirrors the companion disassembler's opcode-table scan,
 * instruction filters and variable-length memory operand handling.
 * Since these are separate implementations, verify changes in both. */
static bool fetch_decode(uint16 addr, Instance *inst) {
    byte b0 = mem[addr];
    const Opcode *selected = NULL;
    int modBytes = 0;
    size_t i;

    for (i = 0; i < OPPS_COUNT; i++) {
        const Opcode *op = &opps[i];
        if ((b0 & op->opMask) != op->op)
            continue;
        if (op->specialOnebyteInstr) {
            byte m = (byte)(b0 & 0x07);
            if (m >= 6) continue;
        }
        if (op->subOpMask != 0) {
            byte b1 = mem[(uint16)(addr + 1)];
            if ((b1 & op->subOpMask) != op->subOp) continue;

            if (op->bytes == 0) {
                byte mod = (byte)(b0 & 0x1f);
                if (mod == 0x16 || mod == 0x17) modBytes = 2;
                else if (mod == 0x06) modBytes = 3;
                else if (mod == 0x0a) modBytes = 4;
                else continue;
            }
            selected = op;
            break;
        } else {
            selected = op;
            break;
        }
    }

    if (!selected) return false;

    inst->op = selected;
    inst->addr = addr;
    inst->totalBytes = (byte)(selected->bytes + modBytes);
    for (i = 0; i < inst->totalBytes; i++)
        inst->bytes[i] = mem[(uint16)(addr + i)];
    return true;
}

/* ==================== MEM (OT_MEM) effective address ==================== */
/* Reproduces OpcodeInstance.h's oi_print_reg_op() OT_MEM case exactly,
 * computing an address instead of printing one. Always paired with A in
 * the opcode table (MOV/XCH/ADD/ADDC/SUB/SUBC/AND/OR/XOR/CMP a,[mem] or
 * [mem],a), so this is always an 8-bit access. Post-increment/decrement
 * forms ([de+]/[hl+]/[de-]/[hl-]) apply their side effect via
 * post_rp/post_delta output params, which the caller applies AFTER the
 * memory access completes (so e.g. "mov [de+],a" stores to the
 * pre-increment address, then increments DE). */
static uint16 mem_effective_addr(const byte bytes[5], int *out_post_rp, int *out_post_delta) {
    byte mod = (byte)(bytes[0] & 0x1f);
    Operand modOp = OP_MEM_MOD;
    byte b = oi_get_masked_byte(&modOp, bytes);
    *out_post_rp = -1;
    *out_post_delta = 0;

    switch (mod) {
    case 0x16: /* [de+] [hl+] [de-] [hl-] [de] [hl] [vp] [up] */
        switch (b) {
        case 0: *out_post_rp = 6; *out_post_delta = +1; return reg_rp(6); /* de */
        case 1: *out_post_rp = 7; *out_post_delta = +1; return reg_rp(7); /* hl */
        case 2: *out_post_rp = 6; *out_post_delta = -1; return reg_rp(6);
        case 3: *out_post_rp = 7; *out_post_delta = -1; return reg_rp(7);
        case 4: return reg_rp(6); /* de */
        case 5: return reg_rp(7); /* hl */
        case 6: return reg_rp(4); /* vp */
        case 7: return reg_rp(5); /* up */
        }
        break;
    case 0x17: /* [de+a] [hl+a] [de+b] [hl+b] [vp+de] [vp+hl] */
        switch (b) {
        case 0: return (uint16)(reg_rp(6) + get_A());
        case 1: return (uint16)(reg_rp(7) + get_A());
        case 2: return (uint16)(reg_rp(6) + reg_r(R2_FIELD_TO_SLOT[1])); /* +b */
        case 3: return (uint16)(reg_rp(7) + reg_r(R2_FIELD_TO_SLOT[1]));
        case 4: return (uint16)(reg_rp(4) + reg_rp(6)); /* vp+de */
        case 5: return (uint16)(reg_rp(4) + reg_rp(7)); /* vp+hl */
        }
        break;
    case 0x06: { /* [de+off8] [sp+off8] [hl+off8] [up+off8] [vp+off8] */
        byte off = bytes[2];
        switch (b) {
        case 0: return (uint16)(reg_rp(6) + off);
        case 1: return (uint16)(get_sp() + off);
        case 2: return (uint16)(reg_rp(7) + off);
        case 3: return (uint16)(reg_rp(5) + off);
        case 4: return (uint16)(reg_rp(4) + off);
        }
        break;
    }
    case 0x0a: { /* [word+de] [word+a] [word+hl] [word+b] */
        uint16 w = oi_get_word(2, bytes);
        switch (b) {
        case 0: return (uint16)(w + reg_rp(6));
        case 1: return (uint16)(w + get_A());
        case 2: return (uint16)(w + reg_rp(7));
        case 3: return (uint16)(w + reg_r(R2_FIELD_TO_SLOT[1]));
        }
        break;
    }
    }
    fprintf(stderr, "emu78k3: unrecognized OT_MEM mod/b at %04x (mod=%02x b=%d)\n",
            g_pc, mod, b);
    g_halted = true; g_halt_reason = 2;
    return 0;
}

/* ==================== Byte/word operand read/write ==================== */

static byte read_mem_s(const Operand *o, const Instance *in);

static byte read_byte_operand(const Operand *o, const Instance *in) {
    switch (o->opType) {
    case OT_A: return get_A();
    case OT_X: return get_X();
    case OT_R:  return reg_r(oi_get_masked_byte(o, in->bytes));
    case OT_R1: return reg_r(oi_get_masked_byte(o, in->bytes));
    case OT_R2: return reg_r(R2_FIELD_TO_SLOT[oi_get_masked_byte(o, in->bytes) & 1]);
    case OT_BYTE: return oi_get_masked_byte(o, in->bytes);
    case OT_SADDR: return mem[addr_from_saddr_byte(oi_get_masked_byte(o, in->bytes))];
    case OT_SADDR_FE: return mem[addr_from_saddr_fe_byte(oi_get_masked_byte(o, in->bytes))];
    case OT_ADDR16: return mem[oi_get_word(o->byteOffset, in->bytes)];
    case OT_SFR: return mem[addr_from_sfr_byte(oi_get_masked_byte(o, in->bytes))];
    case OT_PSWL: return g_pswl;
    case OT_PSWH: return g_pswh;
    case OT_MEM_S: return read_mem_s(o, in);
    case OT_MEM: {
        int prp, pd;
        uint16 a = mem_effective_addr(in->bytes, &prp, &pd);
        byte v = mem[a];
        if (prp >= 0) set_reg_rp(prp, (uint16)(reg_rp(prp) + pd));
        return v;
    }
    default: break;
    }
    fprintf(stderr, "emu78k3: read_byte_operand: unhandled OperandType %d at pc=%04x\n",
            o->opType, in->addr);
    g_halted = true; g_halt_reason = 3;
    return 0;
}

static void write_byte_operand(const Operand *o, const Instance *in, byte v) {
    switch (o->opType) {
    case OT_A: set_A(v); return;
    case OT_X: set_X(v); return;
    case OT_R:  set_reg_r(oi_get_masked_byte(o, in->bytes), v); return;
    case OT_R1: set_reg_r(oi_get_masked_byte(o, in->bytes), v); return;
    case OT_R2: set_reg_r(R2_FIELD_TO_SLOT[oi_get_masked_byte(o, in->bytes) & 1], v); return;
    case OT_SADDR: mem[addr_from_saddr_byte(oi_get_masked_byte(o, in->bytes))] = v; return;
    case OT_SADDR_FE: mem[addr_from_saddr_fe_byte(oi_get_masked_byte(o, in->bytes))] = v; return;
    case OT_ADDR16: mem[oi_get_word(o->byteOffset, in->bytes)] = v; return;
    case OT_SFR: mem[addr_from_sfr_byte(oi_get_masked_byte(o, in->bytes))] = v; return;
    case OT_PSWL: g_pswl = v; return;
    case OT_PSWH: g_pswh = v; return;
    case OT_MEM_S: {
        byte b = oi_get_masked_byte(o, in->bytes);
        uint16 a;
        switch (b) { case 0: case 2: case 4: a = reg_rp(6); break; default: a = reg_rp(7); break; }
        mem[a] = v;
        switch (b) {
        case 0: set_reg_rp(6, (uint16)(reg_rp(6) + 1)); break;
        case 1: set_reg_rp(7, (uint16)(reg_rp(7) + 1)); break;
        case 2: set_reg_rp(6, (uint16)(reg_rp(6) - 1)); break;
        case 3: set_reg_rp(7, (uint16)(reg_rp(7) - 1)); break;
        default: break;
        }
        return;
    }
    case OT_MEM: {
        int prp, pd;
        uint16 a = mem_effective_addr(in->bytes, &prp, &pd);
        mem[a] = v;
        if (prp >= 0) set_reg_rp(prp, (uint16)(reg_rp(prp) + pd));
        return;
    }
    default: break;
    }
    fprintf(stderr, "emu78k3: write_byte_operand: unhandled OperandType %d at pc=%04x\n",
            o->opType, in->addr);
    g_halted = true; g_halt_reason = 3;
}

/* MEM_S read needs the same post-increment/decrement side effect as
 * write; give it its own helper so read call sites get it too. */
static byte read_mem_s(const Operand *o, const Instance *in) {
    byte b = oi_get_masked_byte(o, in->bytes);
    uint16 a;
    switch (b) { case 0: case 2: case 4: a = reg_rp(6); break; default: a = reg_rp(7); break; }
    byte v = mem[a];
    switch (b) {
    case 0: set_reg_rp(6, (uint16)(reg_rp(6) + 1)); break;
    case 1: set_reg_rp(7, (uint16)(reg_rp(7) + 1)); break;
    case 2: set_reg_rp(6, (uint16)(reg_rp(6) - 1)); break;
    case 3: set_reg_rp(7, (uint16)(reg_rp(7) - 1)); break;
    default: break;
    }
    return v;
}

static uint16 read_word_operand(const Operand *o, const Instance *in) {
    switch (o->opType) {
    case OT_AX: return get_AX();
    case OT_SP: return get_sp();
    case OT_WORD: return oi_get_word(o->byteOffset, in->bytes);
    case OT_RP: return reg_rp(oi_get_masked_byte(o, in->bytes));
    case OT_RP1: return reg_rp(RP1_FIELD_TO_SLOT[oi_get_masked_byte(o, in->bytes) & 7]);
    case OT_RP1P: {
        uint16 a = reg_rp(RP1_FIELD_TO_SLOT[oi_get_masked_byte(o, in->bytes) & 7]);
        return (uint16)(mem[a] | (mem[(uint16)(a + 1)] << 8));
    }
    case OT_RPL: return reg_rp(RP1_FIELD_TO_SLOT[oi_get_masked_byte(o, in->bytes) & 7]);
    case OT_RP2: return reg_rp(RP2_FIELD_TO_SLOT[oi_get_masked_byte(o, in->bytes) & 3]);
    case OT_SADDRP: {
        uint16 a = addr_from_saddr_byte(oi_get_masked_byte(o, in->bytes));
        return (uint16)(mem[a] | (mem[(uint16)(a + 1)] << 8));
    }
    case OT_SADDRP_FE: {
        uint16 a = addr_from_saddr_fe_byte(oi_get_masked_byte(o, in->bytes));
        return (uint16)(mem[a] | (mem[(uint16)(a + 1)] << 8));
    }
    case OT_SFRP: {
        uint16 a = addr_from_sfr_byte(oi_get_masked_byte(o, in->bytes));
        return (uint16)(mem[a] | (mem[(uint16)(a + 1)] << 8));
    }
    case OT_ADDR16: {
        uint16 a = oi_get_word(o->byteOffset, in->bytes);
        return (uint16)(mem[a] | (mem[(uint16)(a + 1)] << 8));
    }
    case OT_PSW: return (uint16)((g_pswh << 8) | g_pswl);
    default: break;
    }
    fprintf(stderr, "emu78k3: read_word_operand: unhandled OperandType %d at pc=%04x\n",
            o->opType, in->addr);
    g_halted = true; g_halt_reason = 3;
    return 0;
}

static void write_word_operand(const Operand *o, const Instance *in, uint16 v) {
    switch (o->opType) {
    case OT_AX: set_AX(v); return;
    case OT_SP: set_sp(v); return;
    case OT_RP: set_reg_rp(oi_get_masked_byte(o, in->bytes), v); return;
    case OT_RP1: set_reg_rp(RP1_FIELD_TO_SLOT[oi_get_masked_byte(o, in->bytes) & 7], v); return;
    case OT_RPL: set_reg_rp(RP1_FIELD_TO_SLOT[oi_get_masked_byte(o, in->bytes) & 7], v); return;
    case OT_RP2: set_reg_rp(RP2_FIELD_TO_SLOT[oi_get_masked_byte(o, in->bytes) & 3], v); return;
    case OT_SADDRP: {
        uint16 a = addr_from_saddr_byte(oi_get_masked_byte(o, in->bytes));
        mem[a] = (byte)(v & 0xFF); mem[(uint16)(a + 1)] = (byte)(v >> 8);
        return;
    }
    case OT_SADDRP_FE: {
        uint16 a = addr_from_saddr_fe_byte(oi_get_masked_byte(o, in->bytes));
        mem[a] = (byte)(v & 0xFF); mem[(uint16)(a + 1)] = (byte)(v >> 8);
        return;
    }
    case OT_SFRP: {
        uint16 a = addr_from_sfr_byte(oi_get_masked_byte(o, in->bytes));
        mem[a] = (byte)(v & 0xFF); mem[(uint16)(a + 1)] = (byte)(v >> 8);
        return;
    }
    case OT_ADDR16: {
        uint16 a = oi_get_word(o->byteOffset, in->bytes);
        mem[a] = (byte)(v & 0xFF); mem[(uint16)(a + 1)] = (byte)(v >> 8);
        return;
    }
    case OT_PSW: g_pswh = (byte)(v >> 8); g_pswl = (byte)(v & 0xFF); return;
    default: break;
    }
    fprintf(stderr, "emu78k3: write_word_operand: unhandled OperandType %d at pc=%04x\n",
            o->opType, in->addr);
    g_halted = true; g_halt_reason = 3;
}

/* [[saddr]] double-indirect byte access (MOV a,[[saddr]] / [[saddr]],a):
 * a 16-bit pointer stored AT saddr, dereferenced once more. */
static uint16 saddrpp_target(const Operand *o, const Instance *in) {
    uint16 a = addr_from_saddr_byte(oi_get_masked_byte(o, in->bytes));
    return (uint16)(mem[a] | (mem[(uint16)(a + 1)] << 8));
}

/* ==================== Flags ==================== */

static void set_flags_logic8(byte result) {
    setflag(PSWL_Z, result == 0);
    setflag(PSWL_S, (result & 0x80) != 0);
    /* P/V after a logical op (AND/OR/XOR) is parity: 1 if the number of
     * set bits is even. Standard 8080/Z80-lineage P/V dual meaning. */
    int bits = 0, i;
    for (i = 0; i < 8; i++) if (result & (1 << i)) bits++;
    setflag(PSWL_PV, (bits % 2) == 0);
}

static void set_flags_add8(byte a, byte b, int result, bool withCarry) {
    (void)withCarry;
    setflag(PSWL_CY, result > 0xFF);
    setflag(PSWL_AC, ((a & 0xF) + (b & 0xF)) > 0xF);
    byte r8 = (byte)result;
    setflag(PSWL_Z, r8 == 0);
    setflag(PSWL_S, (r8 & 0x80) != 0);
    bool ovf = (((a ^ r8) & (b ^ r8) & 0x80) != 0);
    setflag(PSWL_PV, ovf);
    setflag(PSWL_SUB, false);
}

static void set_flags_sub8(byte a, byte b, int result) {
    setflag(PSWL_CY, result < 0);
    setflag(PSWL_AC, (a & 0xF) < (b & 0xF));
    byte r8 = (byte)result;
    setflag(PSWL_Z, r8 == 0);
    setflag(PSWL_S, (r8 & 0x80) != 0);
    bool ovf = (((a ^ b) & (a ^ r8) & 0x80) != 0);
    setflag(PSWL_PV, ovf);
    setflag(PSWL_SUB, true);
}

static void set_flags_add16(uint16 a, uint16 b, uint32 result) {
    setflag(PSWL_CY, result > 0xFFFF);
    uint16 r16 = (uint16)result;
    setflag(PSWL_Z, r16 == 0);
    setflag(PSWL_S, (r16 & 0x8000) != 0);
    bool ovf = (((a ^ r16) & (b ^ r16) & 0x8000) != 0);
    setflag(PSWL_PV, ovf);
    setflag(PSWL_SUB, false);
}

static void set_flags_sub16(uint16 a, uint16 b, int32_t result) {
    setflag(PSWL_CY, result < 0);
    uint16 r16 = (uint16)result;
    setflag(PSWL_Z, r16 == 0);
    setflag(PSWL_S, (r16 & 0x8000) != 0);
    bool ovf = (((a ^ b) & (a ^ r16) & 0x8000) != 0);
    setflag(PSWL_PV, ovf);
    setflag(PSWL_SUB, true);
}

/* Conditional-branch flag tests. Source: vbcc78k3's machine.c own
 * comment, citing "NEC User's Manual sec 6.20" by name for the exact
 * condition of each mnemonic (not assumed from other CPU families). */
static bool cond_be(void)  { return getflag(PSWL_Z); }
static bool cond_bne(void) { return !getflag(PSWL_Z); }
static bool cond_bl(void)  { return getflag(PSWL_CY); }
static bool cond_bnl(void) { return !getflag(PSWL_CY); }
static bool cond_bh(void)  { return !(getflag(PSWL_Z) || getflag(PSWL_CY)); }
static bool cond_bnh(void) { return (getflag(PSWL_Z) || getflag(PSWL_CY)); }
static bool cond_blt(void) { return (getflag(PSWL_PV) != getflag(PSWL_S)); }
static bool cond_bge(void) { return (getflag(PSWL_PV) == getflag(PSWL_S)); }
static bool cond_ble(void) { return (getflag(PSWL_PV) != getflag(PSWL_S)) || getflag(PSWL_Z); }
static bool cond_bgt(void) { return !((getflag(PSWL_PV) != getflag(PSWL_S)) || getflag(PSWL_Z)); }
static bool cond_bn(void)  { return getflag(PSWL_S); }
static bool cond_bp(void)  { return !getflag(PSWL_S); }
static bool cond_bpe(void) { return getflag(PSWL_PV); }
static bool cond_bpo(void) { return !getflag(PSWL_PV); }

/* ==================== Stack helpers ==================== */

static void push_word(uint16 v) {
    uint16 sp = (uint16)(get_sp() - 2);
    set_sp(sp);
    mem[sp] = (byte)(v & 0xFF);
    mem[(uint16)(sp + 1)] = (byte)(v >> 8);
}
static uint16 pop_word(void) {
    uint16 sp = get_sp();
    uint16 v = (uint16)(mem[sp] | (mem[(uint16)(sp + 1)] << 8));
    set_sp((uint16)(sp + 2));
    return v;
}

/* PUSH/POP multi-register post-byte: bit i (0-7) selects RP slot i
 * directly (ax=0 ... hl=7 -- confirmed: OT_POST_BYTE's pRegId string in
 * OpcodeInstance.h is the SAME ordering as OT_RP's direct slot mapping).
 * PUSH scans bit7->bit0 (matches the disassembler's print-order scan);
 * POP scans the reverse (bit0->bit7) so it pops back in exactly the
 * mirror order PUSH pushed in, which is what makes a PUSH{..}/POP{..}
 * pair round-trip correctly regardless of which scan direction real
 * silicon actually uses internally. */
static void do_push_regs(byte mask) {
    int i;
    for (i = 7; i >= 0; i--)
        if (mask & (1 << i))
            push_word(reg_rp(i));
}
static void do_pop_regs(byte mask) {
    int i;
    for (i = 0; i <= 7; i++)
        if (mask & (1 << i))
            set_reg_rp(i, pop_word());
}

/* ==================== Execute ==================== */

static void execute(const Instance *in) {
    const Opcode *op = in->op;
    const Operand *o1 = &op->operand1;
    const Operand *o2 = &op->operand2;
    const Operand *o3 = &op->operand3;

    switch (op->mnemonic) {

    case M_NOP: break;

    case M_MOV: {
        if (o2->opType == OT_UNK) { /* OP_UNK never legitimately matches */ break; }
        if (o1->opType == OT_UNK) break;
        /* PSWL/PSWH via OP_A special-cased already by read/write helpers. */
        if (o2->opType == OT_SADDRPP) {
            byte v = mem[saddrpp_target(o2, in)];
            write_byte_operand(o1, in, v);
        } else if (o1->opType == OT_SADDRPP) {
            byte v = read_byte_operand(o2, in);
            mem[saddrpp_target(o1, in)] = v;
        } else {
            byte v = read_byte_operand(o2, in);
            write_byte_operand(o1, in, v);
        }
        break;
    }

    case M_XCH: {
        byte a = read_byte_operand(o1, in);
        byte b;
        if (o2->opType == OT_SADDRPP) b = mem[saddrpp_target(o2, in)];
        else b = read_byte_operand(o2, in);
        if (o2->opType == OT_SADDRPP) mem[saddrpp_target(o2, in)] = a;
        else write_byte_operand(o2, in, a);
        write_byte_operand(o1, in, b);
        break;
    }

    case M_MOVW: {
        uint16 v = read_word_operand(o2, in);
        write_word_operand(o1, in, v);
        break;
    }
    case M_XCHW: {
        uint16 a = read_word_operand(o1, in);
        uint16 b = read_word_operand(o2, in);
        write_word_operand(o2, in, a);
        write_word_operand(o1, in, b);
        break;
    }

    case M_ADD: case M_ADDC: {
        byte a = read_byte_operand(o1, in);
        byte b = read_byte_operand(o2, in);
        int cin = (op->mnemonic == M_ADDC && getflag(PSWL_CY)) ? 1 : 0;
        int r = a + b + cin;
        set_flags_add8(a, b, r, op->mnemonic == M_ADDC);
        write_byte_operand(o1, in, (byte)r);
        break;
    }
    case M_SUB: case M_SUBC: {
        byte a = read_byte_operand(o1, in);
        byte b = read_byte_operand(o2, in);
        int cin = (op->mnemonic == M_SUBC && getflag(PSWL_CY)) ? 1 : 0;
        int r = (int)a - (int)b - cin;
        set_flags_sub8(a, b, r);
        write_byte_operand(o1, in, (byte)r);
        break;
    }
    case M_CMP: {
        byte a = read_byte_operand(o1, in);
        byte b = read_byte_operand(o2, in);
        set_flags_sub8(a, b, (int)a - (int)b);
        break;
    }
    case M_AND: {
        byte a = read_byte_operand(o1, in);
        byte b = read_byte_operand(o2, in);
        byte r = (byte)(a & b);
        set_flags_logic8(r);
        write_byte_operand(o1, in, r);
        break;
    }
    case M_OR: {
        byte a = read_byte_operand(o1, in);
        byte b = read_byte_operand(o2, in);
        byte r = (byte)(a | b);
        set_flags_logic8(r);
        write_byte_operand(o1, in, r);
        break;
    }
    case M_XOR: {
        byte a = read_byte_operand(o1, in);
        byte b = read_byte_operand(o2, in);
        byte r = (byte)(a ^ b);
        set_flags_logic8(r);
        write_byte_operand(o1, in, r);
        break;
    }

    case M_ADDW: {
        uint16 a = read_word_operand(o1, in);
        uint16 b = read_word_operand(o2, in);
        uint32 r = (uint32)a + b;
        set_flags_add16(a, b, r);
        write_word_operand(o1, in, (uint16)r);
        break;
    }
    case M_SUBW: {
        uint16 a = read_word_operand(o1, in);
        uint16 b = read_word_operand(o2, in);
        int32_t r = (int32_t)a - (int32_t)b;
        set_flags_sub16(a, b, r);
        write_word_operand(o1, in, (uint16)r);
        break;
    }
    case M_CMPW: {
        uint16 a = read_word_operand(o1, in);
        uint16 b = read_word_operand(o2, in);
        set_flags_sub16(a, b, (int32_t)a - (int32_t)b);
        break;
    }

    case M_MULU: {
        /* a * r1 (unsigned 8x8->16) -> AX. */
        byte r1 = read_byte_operand(o1, in);
        set_AX((uint16)(get_A() * (uint16)r1));
        break;
    }
    case M_DIVUW: {
        /* AX / r1 (unsigned) -> AX=quotient, r1=remainder (destructive
         * divisor register -- matches machine.c's own documented
         * semantics for this instruction). */
        byte divisor = read_byte_operand(o1, in);
        if (divisor == 0) {
            fprintf(stderr, "emu78k3: DIVUW by zero at pc=%04x\n", in->addr);
            g_halted = true; g_halt_reason = 2; break;
        }
        uint16 dividend = get_AX();
        uint16 q = (uint16)(dividend / divisor);
        byte r = (byte)(dividend % divisor);
        set_AX(q);
        write_byte_operand(o1, in, r);
        break;
    }
    case M_MULUW: {
        /* AX * rp1 (unsigned 16x16->32): high word -> AX, low word ->
         * rp1 itself (78K/III Instructions manual, operation list:
         * "AX (high-order 16 bits), rp1 (low-order 16 bits) <- AX x rp1").
         * Kenwood's AB00 relies on this: "muluw bc" then "movw ax, bc". */
        uint16 rp1 = read_word_operand(o1, in);
        uint32 r = (uint32)get_AX() * (uint32)rp1;
        write_word_operand(o1, in, (uint16)(r & 0xFFFF));
        set_AX((uint16)(r >> 16));
        break;
    }

    case M_INC: case M_DEC: {
        /* S, Z, AC, P/V (overflow) change; CY does not. */
        byte v = read_byte_operand(o1, in);
        byte r = (byte)(op->mnemonic == M_INC ? v + 1 : v - 1);
        setflag(PSWL_Z, r == 0);
        setflag(PSWL_S, (r & 0x80) != 0);
        if (op->mnemonic == M_INC) {
            setflag(PSWL_AC, (v & 0xF) == 0xF);
            setflag(PSWL_PV, v == 0x7F);
            setflag(PSWL_SUB, false);
        } else {
            setflag(PSWL_AC, (v & 0xF) == 0x0);
            setflag(PSWL_PV, v == 0x80);
            setflag(PSWL_SUB, true);
        }
        write_byte_operand(o1, in, r);
        break;
    }
    case M_INCW: case M_DECW: {
        /* rp2 or saddrp; no flags change. */
        uint16 v = read_word_operand(o1, in);
        write_word_operand(o1, in, (uint16)(op->mnemonic == M_INCW ? v + 1 : v - 1));
        break;
    }
    case M_DIVUX: {
        /* AXDE (32-bit, AX high/DE low) / rp1 -> AXDE=quotient (32-bit),
         * rp1=remainder (destructive divisor register). Matches
         * machine.c's own documented semantics and divmod_word_tail's
         * usage (movw ax,#0 ; divux <divisor> for a plain 16-bit
         * divide). */
        uint16 divisor = read_word_operand(o1, in);
        if (divisor == 0) {
            fprintf(stderr, "emu78k3: DIVUX by zero at pc=%04x\n", in->addr);
            g_halted = true; g_halt_reason = 2; break;
        }
        uint32 dividend = ((uint32)get_AX() << 16) | reg_rp(6);
        uint32 q = dividend / divisor;
        uint16 r = (uint16)(dividend % divisor);
        set_AX((uint16)(q >> 16));
        set_reg_rp(6, (uint16)(q & 0xFFFF));
        write_word_operand(o1, in, r);
        break;
    }

    case M_SHR: case M_SHL: {
        byte v = read_byte_operand(o1, in);
        byte n = oi_get_masked_byte(o2, in->bytes) & 7;
        byte r; bool lastout = false;
        if (op->mnemonic == M_SHR) {
            r = v; int i;
            for (i = 0; i < n; i++) { lastout = (r & 1) != 0; r = (byte)(r >> 1); }
        } else {
            r = v; int i;
            for (i = 0; i < n; i++) { lastout = (r & 0x80) != 0; r = (byte)(r << 1); }
        }
        if (n > 0) setflag(PSWL_CY, lastout);
        setflag(PSWL_Z, r == 0);
        setflag(PSWL_S, (r & 0x80) != 0);
        write_byte_operand(o1, in, r);
        break;
    }
    case M_SHRW: case M_SHLW: {
        uint16 v = read_word_operand(o1, in);
        byte n = oi_get_masked_byte(o2, in->bytes) & 7;
        uint16 r; bool lastout = false;
        if (op->mnemonic == M_SHRW) {
            r = v; int i;
            for (i = 0; i < n; i++) { lastout = (r & 1) != 0; r = (uint16)(r >> 1); }
        } else {
            r = v; int i;
            for (i = 0; i < n; i++) { lastout = (r & 0x8000) != 0; r = (uint16)(r << 1); }
        }
        if (n > 0) setflag(PSWL_CY, lastout);
        setflag(PSWL_Z, r == 0);
        setflag(PSWL_S, (r & 0x8000) != 0);
        write_word_operand(o1, in, r);
        break;
    }

    case M_SET1: case M_CLR1: case M_NOT1: {
        if (o1->opType == OT_CY && op->operand2.opType == OT_NONE) {
            if (op->mnemonic == M_SET1) setflag(PSWL_CY, true);
            else if (op->mnemonic == M_CLR1) setflag(PSWL_CY, false);
            else setflag(PSWL_CY, !getflag(PSWL_CY));
            break;
        }
        /* bit-in-byte forms: operand1 is the byte location, N_BIT is the
         * bit number (operand2 for SADDR/SFR/A/X/PSWL/PSWH 1-bit-field
         * forms per the opcode table). */
        const Operand *bitOp = o2;
        byte bit = oi_get_masked_byte(bitOp, in->bytes) & 7;
        byte v = read_byte_operand(o1, in);
        byte nv = v;
        if (op->mnemonic == M_SET1) nv = (byte)(v | (1 << bit));
        else if (op->mnemonic == M_CLR1) nv = (byte)(v & ~(1 << bit));
        else nv = (byte)(v ^ (1 << bit));
        write_byte_operand(o1, in, nv);
        break;
    }

    case M_MOV1: {
        /* MOV1 CY, loc.bit / MOV1 loc.bit, CY. The CY_BIT operand is CY
         * and also carries the bit number; the other operand is the byte
         * (saddr, sfr, A, X, PSWL or PSWH). No other flags change. */
        bool to_cy = (o1->opType == OT_CY_BIT);
        const Operand *cyOp = to_cy ? o1 : o2;
        const Operand *loc  = to_cy ? o2 : o1;
        byte bit = oi_get_masked_byte(cyOp, in->bytes) & 7;
        byte v = read_byte_operand(loc, in);
        if (to_cy) {
            setflag(PSWL_CY, (v & (1 << bit)) != 0);
        } else {
            byte nv = getflag(PSWL_CY) ? (byte)(v | (1 << bit))
                                       : (byte)(v & ~(1 << bit));
            write_byte_operand(loc, in, nv);
        }
        break;
    }

    case M_BT: case M_BF: {
        /* operand1=byte location, operand3=bit number, operand2=target.
         * (see OP_..._2/3/4, false, OP_N_BIT_x rows in Opcodes.h) */
        byte bit = oi_get_masked_byte(o3, in->bytes) & 7;
        byte v = read_byte_operand(o1, in);
        bool set = (v & (1 << bit)) != 0;
        bool take = (op->mnemonic == M_BT) ? set : !set;
        if (take) {
            byte lo = in->bytes[o2->byteOffset];
            g_pc = (uint16)(g_pc + (int8_t)lo);
        }
        break;
    }
    case M_BTCLR: case M_BFSET: {
        /* Same as BT/BF, and the tested bit is cleared (BTCLR) or set
         * (BFSET) when the branch is taken. */
        byte bit = oi_get_masked_byte(o3, in->bytes) & 7;
        byte v = read_byte_operand(o1, in);
        bool set = (v & (1 << bit)) != 0;
        if (op->mnemonic == M_BTCLR ? set : !set) {
            write_byte_operand(o1, in, (byte)(op->mnemonic == M_BTCLR
                ? v & ~(1 << bit) : v | (1 << bit)));
            byte lo = in->bytes[o2->byteOffset];
            g_pc = (uint16)(g_pc + (int8_t)lo);
        }
        break;
    }

    case M_BR: {
        if (o1->opType == OT_ADDR16) { g_pc = oi_get_word(o1->byteOffset, in->bytes); }
        else if (o1->opType == OT_ADDR16R) {
            byte lo = in->bytes[o1->byteOffset];
            g_pc = (uint16)(g_pc + (int8_t)lo);
        } else if (o1->opType == OT_RP1) {
            g_pc = read_word_operand(o1, in);
        } else if (o1->opType == OT_RP1P) {
            g_pc = read_word_operand(o1, in);
        }
        break;
    }

#define COND_BRANCH_CASE(M, fn) \
    case M: { \
        if (fn()) { byte lo = in->bytes[o1->byteOffset]; g_pc = (uint16)(g_pc + (int8_t)lo); } \
        break; \
    }
    COND_BRANCH_CASE(M_BE, cond_be)
    COND_BRANCH_CASE(M_BNE, cond_bne)
    COND_BRANCH_CASE(M_BL, cond_bl)
    COND_BRANCH_CASE(M_BNL, cond_bnl)
    COND_BRANCH_CASE(M_BH, cond_bh)
    COND_BRANCH_CASE(M_BNH, cond_bnh)
    COND_BRANCH_CASE(M_BLT, cond_blt)
    COND_BRANCH_CASE(M_BGE, cond_bge)
    COND_BRANCH_CASE(M_BLE, cond_ble)
    COND_BRANCH_CASE(M_BGT, cond_bgt)
    COND_BRANCH_CASE(M_BN, cond_bn)
    COND_BRANCH_CASE(M_BP, cond_bp)
    COND_BRANCH_CASE(M_BPE, cond_bpe)
    COND_BRANCH_CASE(M_BPO, cond_bpo)
#undef COND_BRANCH_CASE

    case M_DBNZ: {
        byte v;
        if (o1->opType == OT_SADDR) {
            uint16 a = addr_from_saddr_byte(oi_get_masked_byte(o1, in->bytes));
            v = (byte)(mem[a] - 1);
            mem[a] = v;
        } else { /* OT_R2 (c/b) */
            int slot = R2_FIELD_TO_SLOT[oi_get_masked_byte(o1, in->bytes) & 1];
            v = (byte)(reg_r(slot) - 1);
            set_reg_r(slot, v);
        }
        if (v != 0) {
            byte lo = in->bytes[o2->byteOffset];
            g_pc = (uint16)(g_pc + (int8_t)lo);
        }
        break;
    }

    case M_CALL: {
        if (o1->opType == OT_ADDR16) {
            push_word(g_pc);
            g_pc = oi_get_word(o1->byteOffset, in->bytes);
        } else if (o1->opType == OT_RP1) {
            uint16 t = read_word_operand(o1, in);
            push_word(g_pc);
            g_pc = t;
        } else if (o1->opType == OT_RP1P) {
            uint16 t = read_word_operand(o1, in);
            push_word(g_pc);
            g_pc = t;
        }
        break;
    }
    case M_CALLF: {
        /* target = 0x0800 + raw 11-bit field (bytes[0]&7)<<8 | bytes[1] */
        uint16 target = (uint16)(0x0800 + (((in->bytes[0] & 0x07) << 8) | in->bytes[1]));
        push_word(g_pc);
        g_pc = target;
        break;
    }
    case M_CALLT: {
        byte idx = (byte)(in->bytes[0] & 0x1f);
        uint16 tbl = (uint16)(0x0040 + idx * 2);
        uint16 target = (uint16)(mem[tbl] | (mem[(uint16)(tbl + 1)] << 8));
        push_word(g_pc);
        g_pc = target;
        break;
    }

    case M_RET: {
        g_pc = pop_word();
        break;
    }
    case M_RETI: {
        /* Simplification: RETI here only restores PC (same as RET). The
         * vbcc78k3 interrupt prologue/epilogue manually saves/restores
         * PSW and general registers with explicit PUSH/POP around RETI
         * (confirmed in machine.c), so this is sufficient for every
         * compiler-emitted use -- whether real RETI hardware ALSO
         * restores something beyond PC has not been independently
         * confirmed here. */
        g_pc = pop_word();
        break;
    }
    case M_BRK: {
        g_halted = true; g_halt_reason = 1;
        break;
    }

    case M_PUSH: {
        if (o1->opType == OT_PSW) { push_word((uint16)((g_pswh << 8) | g_pswl)); }
        else { byte mask = oi_get_masked_byte(o1, in->bytes); do_push_regs(mask); }
        break;
    }
    case M_POP: {
        if (o1->opType == OT_PSW) { uint16 v = pop_word(); g_pswh = (byte)(v >> 8); g_pswl = (byte)(v & 0xFF); }
        else { byte mask = oi_get_masked_byte(o1, in->bytes); do_pop_regs(mask); }
        break;
    }

    default:
        fprintf(stderr, "emu78k3: unimplemented mnemonic %s at pc=%04x (bytes:",
                mnemonic_names[(int)op->mnemonic], in->addr);
        { int i; for (i = 0; i < in->totalBytes; i++) fprintf(stderr, " %02x", in->bytes[i]); }
        fprintf(stderr, ")\n");
        g_halted = true; g_halt_reason = 3;
        break;
    }
}

/* ==================== Driver ==================== */

static void print_state(FILE *f) {
    fprintf(f, "pc=%04x sp=%04x pswh=%02x pswl=%02x (S=%d Z=%d RSS=%d AC=%d PV=%d SUB=%d CY=%d) bank=%d\n",
            g_pc, get_sp(), g_pswh, g_pswl,
            getflag(PSWL_S), getflag(PSWL_Z), getflag(PSWL_RSS), getflag(PSWL_AC),
            getflag(PSWL_PV), getflag(PSWL_SUB), getflag(PSWL_CY), cur_bank());
    fprintf(f, "a=%02x x=%02x ax=%04x bc=%04x de=%04x hl=%04x vp=%04x up=%04x\n",
            get_A(), get_X(), get_AX(), reg_rp(bc_rp()), reg_rp(6), reg_rp(7), reg_rp(4), reg_rp(5));
}

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s <binary> [--base ADDR] [--entry ADDR] [--sp ADDR] "
        "[--max-insns N] [--trace] [--dump-mem ADDR:LEN]\n"
        "  Runs until BRK, an unimplemented/illegal instruction, a branch\n"
        "  to itself (self-loop -- typical \"for(;;);\" test-done marker),\n"
        "  or --max-insns is reached, then prints final CPU state.\n",
        prog);
}

int main(int argc, char **argv) {
    const char *path = NULL;
    uint16 base = 0, entry = 0xFFFF /* sentinel: default to base */;
    uint16 sp0 = 0xFE00;
    uint16 dump_addr = 0; int dump_len = 0; bool do_dump = false;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--base") && i + 1 < argc) base = (uint16)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--entry") && i + 1 < argc) entry = (uint16)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--sp") && i + 1 < argc) sp0 = (uint16)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--max-insns") && i + 1 < argc) g_max_insns = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--trace")) g_trace = true;
        else if (!strcmp(argv[i], "--dump-mem") && i + 1 < argc) {
            const char *arg = argv[++i];
            char *colon = strchr(arg, ':');
            dump_addr = (uint16)strtoul(arg, NULL, 0);
            dump_len = colon ? (int)strtol(colon + 1, NULL, 0) : 1;
            do_dump = true;
        } else if (!path) path = argv[i];
        else { usage(argv[0]); return 2; }
    }
    if (!path) { usage(argv[0]); return 2; }
    if (entry == 0xFFFF) entry = base;

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "emu78k3: cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || (long)base + sz > 65536) {
        fprintf(stderr, "emu78k3: image too large (%ld bytes at base %04x)\n", sz, base);
        fclose(f); return 2;
    }
    if (fread(mem + base, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "emu78k3: short read on %s\n", path);
        fclose(f); return 2;
    }
    fclose(f);

    g_pc = entry;
    set_sp(sp0);
    g_pswh = 0;
    g_pswl = 0;

    while (!g_halted && g_insn_count < g_max_insns) {
        Instance in;
        uint16 start_pc = g_pc;
        if (!fetch_decode(g_pc, &in)) {
            fprintf(stderr, "emu78k3: illegal opcode %02x at pc=%04x\n", mem[g_pc], g_pc);
            g_halt_reason = 2;
            break;
        }
        g_pc = (uint16)(g_pc + in.totalBytes);
        if (g_trace) {
            fprintf(stderr, "%04x: ", start_pc);
            int j; for (j = 0; j < in.totalBytes; j++) fprintf(stderr, "%02x", in.bytes[j]);
            fprintf(stderr, " %s\n", mnemonic_names[(int)in.op->mnemonic]);
        }
        execute(&in);
        g_insn_count++;

        /* Detect an unconditional self-branch (BR to its own start
         * address, e.g. compiler-emitted "for(;;);") as a clean
         * "program has reached its steady-state end" signal, distinct
         * from actually running out of the instruction budget. */
        if (!g_halted && in.op->mnemonic == M_BR && g_pc == start_pc) {
            g_halt_reason = 4;
            break;
        }
    }

    fprintf(stderr, "emu78k3: halted after %ld instructions, reason=%d (%s)\n",
            g_insn_count, g_halt_reason,
            g_halt_reason == 0 ? "max-insns" :
            g_halt_reason == 1 ? "BRK" :
            g_halt_reason == 2 ? "illegal/fault" :
            g_halt_reason == 3 ? "unimplemented" :
            g_halt_reason == 4 ? "self-loop (program end)" : "?");
    print_state(stderr);

    if (do_dump) {
        int k;
        fprintf(stdout, "mem %04x:", dump_addr);
        for (k = 0; k < dump_len; k++)
            fprintf(stdout, " %02x", mem[(uint16)(dump_addr + k)]);
        fprintf(stdout, "\n");
    }

    /* Exit status: 0 only for the two "clean" halts (self-loop or BRK);
     * nonzero for anything a test harness should treat as a failure. */
    return (g_halt_reason == 4 || g_halt_reason == 1) ? 0 : 1;
}
