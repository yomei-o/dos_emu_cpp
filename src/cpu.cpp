// 8086 real-mode interpreter core. Covers the integer subset a DOS program and a
// C compiler emit: MOV, the ALU group, INC/DEC, stack, Jcc/JMP/CALL/RET/LOOP,
// string ops with REP, shifts/rotates, MUL/DIV, INT, LEA/LES/LDS, flag ops. An
// unhandled opcode stops with its byte and address, the way a new guest is brought
// up. Not modelled: protected mode, the x87 FPU (LSI C's small model uses IEEE
// software float in libraries, so integer + calls are enough to start).
#include "cpu.h"
#include <cstdio>

namespace dosemu {

void Cpu::fail(const std::string& msg, uint8_t op) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s (opcode 0x%02X) at %04X:%04X", msg.c_str(), op,
                  sreg[CS], static_cast<uint16_t>(ip - 1));
    throw CpuError{buf, sreg[CS], static_cast<uint16_t>(ip - 1)};
}

// ---- flag helpers ---------------------------------------------------------
static bool parity(uint8_t v) { v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return !(v & 1); }

namespace {
struct Modrm {
    uint8_t mod, reg, rm;
    bool is_reg;
    uint16_t seg, off;   // effective address for memory operands
};
}

// Decode context lives in the step() frame; pass what the helpers need.
struct Decode {
    uint16_t seg_ovr = 0xFFFF;   // segment override, or 0xFFFF
    bool have_ovr = false;
};

// The interpreter is one big method to keep the hot state in locals.
void Cpu::run() { while (!halted) step(); }

void Cpu::interrupt(uint8_t n) {
    if (on_int && on_int(n)) return;   // serviced by DOS/BIOS layer
    // Fall back to the real interrupt vector table (rarely used by our guests).
    push16(flags);
    push16(sreg[CS]);
    push16(ip);
    set_flag(IF, false);
    set_flag(TF, false);
    ip = mem_.r16(n * 4);
    sreg[CS] = mem_.r16(n * 4 + 2);
}

void Cpu::step() {
    ++insns;
    if (max_insns && insns > max_insns)
        throw CpuError{"instruction limit exceeded (runaway program?)", sreg[CS], ip};
    Decode d;
    uint8_t op;

    // ---- prefixes ----
    uint8_t rep = 0;   // 0, 0xF2 (REPNE), 0xF3 (REP/REPE)
    for (;;) {
        op = fetch8();
        switch (op) {
            case 0x26: d.seg_ovr = sreg[ES]; d.have_ovr = true; continue;
            case 0x2E: d.seg_ovr = sreg[CS]; d.have_ovr = true; continue;
            case 0x36: d.seg_ovr = sreg[SS]; d.have_ovr = true; continue;
            case 0x3E: d.seg_ovr = sreg[DS]; d.have_ovr = true; continue;
            case 0xF0: continue;                 // LOCK: no-op here
            case 0xF2: rep = 0xF2; continue;
            case 0xF3: rep = 0xF3; continue;
        }
        break;
    }

    // ---- ModRM decode (16-bit addressing) ----
    auto decode_modrm = [&](Modrm& m) {
        uint8_t b = fetch8();
        m.mod = b >> 6; m.reg = (b >> 3) & 7; m.rm = b & 7;
        m.is_reg = (m.mod == 3);
        if (m.is_reg) return;
        uint16_t base = 0, seg = sreg[DS];
        switch (m.rm) {
            case 0: base = r[BX] + r[SI]; break;
            case 1: base = r[BX] + r[DI]; break;
            case 2: base = r[BP] + r[SI]; seg = sreg[SS]; break;
            case 3: base = r[BP] + r[DI]; seg = sreg[SS]; break;
            case 4: base = r[SI]; break;
            case 5: base = r[DI]; break;
            case 6: if (m.mod == 0) { base = fetch16(); } else { base = r[BP]; seg = sreg[SS]; } break;
            case 7: base = r[BX]; break;
        }
        if (m.mod == 1) base += static_cast<int16_t>(static_cast<int8_t>(fetch8()));
        else if (m.mod == 2) base += fetch16();
        m.seg = d.have_ovr ? d.seg_ovr : seg;
        m.off = base;
    };

    // operand read/write via a decoded ModRM
    auto r8m  = [&](const Modrm& m) -> uint8_t  { return m.is_reg ? gb(m.rm) : mem_.rb(m.seg, m.off); };
    auto w8m  = [&](const Modrm& m, uint8_t v)  { if (m.is_reg) sb(m.rm, v); else mem_.wb(m.seg, m.off, v); };
    auto r16m = [&](const Modrm& m) -> uint16_t { return m.is_reg ? r[m.rm] : mem_.rw(m.seg, m.off); };
    auto w16m = [&](const Modrm& m, uint16_t v) { if (m.is_reg) r[m.rm] = v; else mem_.ww(m.seg, m.off, v); };

    // ---- ALU with flags (op: 0=ADD 1=OR 2=ADC 3=SBB 4=AND 5=SUB 6=XOR 7=CMP) ----
    auto alu8 = [&](int aluop, uint8_t a, uint8_t b) -> uint8_t {
        uint32_t res; int carry = get_flag(CF) ? 1 : 0;
        switch (aluop) {
            case 0: res = a + b; break;
            case 2: res = a + b + carry; break;
            case 5: case 7: res = a - b; break;
            case 3: res = a - b - carry; break;
            case 1: res = a | b; break;
            case 4: res = a & b; break;
            case 6: res = a ^ b; break;
            default: res = 0;
        }
        uint8_t r8v = res & 0xFF;
        if (aluop == 1 || aluop == 4 || aluop == 6) { set_flag(CF, false); set_flag(OF, false); set_flag(AF, false); }
        else {
            bool sub = (aluop == 5 || aluop == 7 || aluop == 3);
            set_flag(CF, res & 0x100);
            set_flag(AF, (((a ^ b ^ r8v) & 0x10) != 0));
            bool of;
            if (sub) of = (((a ^ b) & (a ^ r8v)) & 0x80) != 0;
            else     of = ((~(a ^ b) & (a ^ r8v)) & 0x80) != 0;
            set_flag(OF, of);
        }
        set_flag(ZF, r8v == 0); set_flag(SF, r8v & 0x80); set_flag(PF, parity(r8v));
        return r8v;
    };
    auto alu16 = [&](int aluop, uint16_t a, uint16_t b) -> uint16_t {
        uint32_t res; int carry = get_flag(CF) ? 1 : 0;
        switch (aluop) {
            case 0: res = a + b; break;
            case 2: res = a + b + carry; break;
            case 5: case 7: res = a - b; break;
            case 3: res = a - b - carry; break;
            case 1: res = a | b; break;
            case 4: res = a & b; break;
            case 6: res = a ^ b; break;
            default: res = 0;
        }
        uint16_t rv = res & 0xFFFF;
        if (aluop == 1 || aluop == 4 || aluop == 6) { set_flag(CF, false); set_flag(OF, false); set_flag(AF, false); }
        else {
            bool sub = (aluop == 5 || aluop == 7 || aluop == 3);
            set_flag(CF, res & 0x10000);
            set_flag(AF, (((a ^ b ^ rv) & 0x10) != 0));
            bool of;
            if (sub) of = (((a ^ b) & (a ^ rv)) & 0x8000) != 0;
            else     of = ((~(a ^ b) & (a ^ rv)) & 0x8000) != 0;
            set_flag(OF, of);
        }
        set_flag(ZF, rv == 0); set_flag(SF, rv & 0x8000); set_flag(PF, parity(rv & 0xFF));
        return rv;
    };

    auto jcc = [&](bool cond) { int8_t rel = static_cast<int8_t>(fetch8()); if (cond) ip += rel; };

    // ---- opcode dispatch ----
    switch (op) {
        // ALU group: 00-3D, pattern (op<<3)|dir/size
        case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38: {
            Modrm m; decode_modrm(m); int a = op >> 3; uint8_t v = alu8(a, r8m(m), gb(m.reg)); if (a != 7) w8m(m, v); break; }
        case 0x01: case 0x09: case 0x11: case 0x19: case 0x21: case 0x29: case 0x31: case 0x39: {
            Modrm m; decode_modrm(m); int a = op >> 3; uint16_t v = alu16(a, r16m(m), r[m.reg]); if (a != 7) w16m(m, v); break; }
        case 0x02: case 0x0A: case 0x12: case 0x1A: case 0x22: case 0x2A: case 0x32: case 0x3A: {
            Modrm m; decode_modrm(m); int a = op >> 3; uint8_t v = alu8(a, gb(m.reg), r8m(m)); if (a != 7) sb(m.reg, v); break; }
        case 0x03: case 0x0B: case 0x13: case 0x1B: case 0x23: case 0x2B: case 0x33: case 0x3B: {
            Modrm m; decode_modrm(m); int a = op >> 3; uint16_t v = alu16(a, r[m.reg], r16m(m)); if (a != 7) r[m.reg] = v; break; }
        case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C: {
            int a = op >> 3; uint8_t v = alu8(a, gb(AX), fetch8()); if (a != 7) sb(AX, v); break; }
        case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D: {
            int a = op >> 3; uint16_t v = alu16(a, r[AX], fetch16()); if (a != 7) r[AX] = v; break; }

        // PUSH/POP segment and general regs
        case 0x06: push16(sreg[ES]); break;
        case 0x07: sreg[ES] = pop16(); break;
        case 0x0E: push16(sreg[CS]); break;
        case 0x16: push16(sreg[SS]); break;
        case 0x17: sreg[SS] = pop16(); break;
        case 0x1E: push16(sreg[DS]); break;
        case 0x1F: sreg[DS] = pop16(); break;
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
            if ((op & 7) == SP) { uint16_t s = r[SP]; push16(s); } else push16(r[op & 7]); break;
        case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            r[op & 7] = pop16(); break;

        // INC/DEC reg16 (40-4F) — preserve CF
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: {
            bool c = get_flag(CF); r[op & 7] = alu16(0, r[op & 7], 1); set_flag(CF, c); break; }
        case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
            bool c = get_flag(CF); r[op & 7] = alu16(5, r[op & 7], 1); set_flag(CF, c); break; }

        // Jcc rel8 (70-7F)
        case 0x70: jcc(get_flag(OF)); break;
        case 0x71: jcc(!get_flag(OF)); break;
        case 0x72: jcc(get_flag(CF)); break;
        case 0x73: jcc(!get_flag(CF)); break;
        case 0x74: jcc(get_flag(ZF)); break;
        case 0x75: jcc(!get_flag(ZF)); break;
        case 0x76: jcc(get_flag(CF) || get_flag(ZF)); break;
        case 0x77: jcc(!(get_flag(CF) || get_flag(ZF))); break;
        case 0x78: jcc(get_flag(SF)); break;
        case 0x79: jcc(!get_flag(SF)); break;
        case 0x7A: jcc(get_flag(PF)); break;
        case 0x7B: jcc(!get_flag(PF)); break;
        case 0x7C: jcc(get_flag(SF) != get_flag(OF)); break;
        case 0x7D: jcc(get_flag(SF) == get_flag(OF)); break;
        case 0x7E: jcc(get_flag(ZF) || (get_flag(SF) != get_flag(OF))); break;
        case 0x7F: jcc(!get_flag(ZF) && (get_flag(SF) == get_flag(OF))); break;

        // Group 1: ALU rm, imm  (80/81/83)
        case 0x80: { Modrm m; decode_modrm(m); uint8_t imm = fetch8(); uint8_t v = alu8(m.reg, r8m(m), imm); if (m.reg != 7) w8m(m, v); break; }
        case 0x81: { Modrm m; decode_modrm(m); uint16_t imm = fetch16(); uint16_t v = alu16(m.reg, r16m(m), imm); if (m.reg != 7) w16m(m, v); break; }
        case 0x83: { Modrm m; decode_modrm(m); uint16_t imm = static_cast<int16_t>(static_cast<int8_t>(fetch8())); uint16_t v = alu16(m.reg, r16m(m), imm); if (m.reg != 7) w16m(m, v); break; }

        // TEST rm, reg
        case 0x84: { Modrm m; decode_modrm(m); alu8(4, r8m(m), gb(m.reg)); break; }
        case 0x85: { Modrm m; decode_modrm(m); alu16(4, r16m(m), r[m.reg]); break; }
        // XCHG rm, reg
        case 0x86: { Modrm m; decode_modrm(m); uint8_t t = r8m(m); w8m(m, gb(m.reg)); sb(m.reg, t); break; }
        case 0x87: { Modrm m; decode_modrm(m); uint16_t t = r16m(m); w16m(m, r[m.reg]); r[m.reg] = t; break; }
        // MOV
        case 0x88: { Modrm m; decode_modrm(m); w8m(m, gb(m.reg)); break; }
        case 0x89: { Modrm m; decode_modrm(m); w16m(m, r[m.reg]); break; }
        case 0x8A: { Modrm m; decode_modrm(m); sb(m.reg, r8m(m)); break; }
        case 0x8B: { Modrm m; decode_modrm(m); r[m.reg] = r16m(m); break; }
        case 0x8C: { Modrm m; decode_modrm(m); w16m(m, sreg[m.reg & 3]); break; }   // MOV rm, sreg
        case 0x8D: { Modrm m; decode_modrm(m); r[m.reg] = m.off; break; }           // LEA
        case 0x8E: { Modrm m; decode_modrm(m); sreg[m.reg & 3] = r16m(m); break; }  // MOV sreg, rm
        case 0x8F: { Modrm m; decode_modrm(m); w16m(m, pop16()); break; }           // POP rm

        case 0x90: break;   // NOP (XCHG AX,AX)
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
            uint16_t t = r[AX]; r[AX] = r[op & 7]; r[op & 7] = t; break; }          // XCHG AX, r
        case 0x98: r[AX] = static_cast<int16_t>(static_cast<int8_t>(r[AX] & 0xFF)); break;  // CBW
        case 0x99: r[DX] = (r[AX] & 0x8000) ? 0xFFFF : 0x0000; break;               // CWD
        case 0x9C: push16(flags); break;                                           // PUSHF
        case 0x9D: flags = pop16() | 0x0002; break;                                // POPF
        case 0x9E: flags = (flags & 0xFF00) | (r[AX] >> 8); break;                 // SAHF
        case 0x9F: r[AX] = (r[AX] & 0x00FF) | ((flags & 0xFF) << 8); break;        // LAHF

        // MOV AL/AX <-> moffs
        case 0xA0: { uint16_t o = fetch16(); sb(AX, mem_.rb(d.have_ovr?d.seg_ovr:sreg[DS], o)); break; }
        case 0xA1: { uint16_t o = fetch16(); r[AX] = mem_.rw(d.have_ovr?d.seg_ovr:sreg[DS], o); break; }
        case 0xA2: { uint16_t o = fetch16(); mem_.wb(d.have_ovr?d.seg_ovr:sreg[DS], o, gb(AX)); break; }
        case 0xA3: { uint16_t o = fetch16(); mem_.ww(d.have_ovr?d.seg_ovr:sreg[DS], o, r[AX]); break; }
        // TEST AL/AX, imm
        case 0xA8: alu8(4, gb(AX), fetch8()); break;
        case 0xA9: alu16(4, r[AX], fetch16()); break;

        // String ops
        case 0xA4: case 0xA5: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xA6: case 0xA7: case 0xAE: case 0xAF: {
            bool word = op & 1; int delta = get_flag(DF) ? -(word?2:1) : (word?2:1);
            uint16_t dseg = d.have_ovr ? d.seg_ovr : sreg[DS];
            auto once = [&]() {
                switch (op & 0xFE) {
                    case 0xA4: if (word) mem_.ww(sreg[ES], r[DI], mem_.rw(dseg, r[SI])); else mem_.wb(sreg[ES], r[DI], mem_.rb(dseg, r[SI])); r[SI]+=delta; r[DI]+=delta; break;   // MOVS
                    case 0xAA: if (word) mem_.ww(sreg[ES], r[DI], r[AX]); else mem_.wb(sreg[ES], r[DI], gb(AX)); r[DI]+=delta; break;   // STOS
                    case 0xAC: if (word) r[AX]=mem_.rw(dseg, r[SI]); else sb(AX, mem_.rb(dseg, r[SI])); r[SI]+=delta; break;            // LODS
                    case 0xA6: if (word) alu16(7, mem_.rw(dseg,r[SI]), mem_.rw(sreg[ES],r[DI])); else alu8(7, mem_.rb(dseg,r[SI]), mem_.rb(sreg[ES],r[DI])); r[SI]+=delta; r[DI]+=delta; break; // CMPS
                    case 0xAE: if (word) alu16(7, r[AX], mem_.rw(sreg[ES],r[DI])); else alu8(7, gb(AX), mem_.rb(sreg[ES],r[DI])); r[DI]+=delta; break;   // SCAS
                }
            };
            bool cmp = (op & 0xFE) == 0xA6 || (op & 0xFE) == 0xAE;
            if (rep) {
                while (r[CX]) { --r[CX]; once(); if (cmp) { bool z = get_flag(ZF); if (rep==0xF3 && !z) break; if (rep==0xF2 && z) break; } }
            } else once();
            break;
        }

        // MOV reg, imm
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7: sb(op & 7, fetch8()); break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF: r[op & 7] = fetch16(); break;

        // Group 2: shifts/rotates
        case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
            Modrm m; decode_modrm(m);
            uint8_t cnt;
            if (op == 0xC0 || op == 0xC1) cnt = fetch8();
            else if (op == 0xD0 || op == 0xD1) cnt = 1;
            else cnt = gb(CX);
            bool word = op & 1; cnt &= 0x1F;
            uint32_t val = word ? r16m(m) : r8m(m); uint32_t msb = word ? 0x8000 : 0x80; uint32_t mask = word ? 0xFFFF : 0xFF;
            for (uint8_t k = 0; k < cnt; ++k) {
                switch (m.reg) {
                    case 0: { bool c = val & msb; val = ((val << 1) | (c?1:0)) & mask; set_flag(CF, c); break; }               // ROL
                    case 1: { bool c = val & 1; val = ((val >> 1) | (c?msb:0)) & mask; set_flag(CF, c); break; }               // ROR
                    case 2: { bool c = val & msb; val = ((val << 1) | (get_flag(CF)?1:0)) & mask; set_flag(CF, c); break; }    // RCL
                    case 3: { bool c = val & 1; val = ((val >> 1) | (get_flag(CF)?msb:0)) & mask; set_flag(CF, c); break; }    // RCR
                    case 4: case 6: { set_flag(CF, val & msb); val = (val << 1) & mask; break; }                              // SHL/SAL
                    case 5: { set_flag(CF, val & 1); val = (val >> 1) & mask; break; }                                        // SHR
                    case 7: { set_flag(CF, val & 1); uint32_t s = val & msb; val = ((val >> 1) | s) & mask; break; }          // SAR
                }
            }
            if (cnt) { set_flag(ZF, (val & mask) == 0); set_flag(SF, val & msb); set_flag(PF, parity(val & 0xFF)); }
            if (word) w16m(m, val & mask); else w8m(m, val & mask);
            break;
        }

        // RET / RET imm16 (near)
        case 0xC2: { uint16_t n = fetch16(); ip = pop16(); r[SP] += n; break; }
        case 0xC3: ip = pop16(); break;
        // LES / LDS
        case 0xC4: { Modrm m; decode_modrm(m); r[m.reg] = mem_.rw(m.seg, m.off); sreg[ES] = mem_.rw(m.seg, m.off + 2); break; }
        case 0xC5: { Modrm m; decode_modrm(m); r[m.reg] = mem_.rw(m.seg, m.off); sreg[DS] = mem_.rw(m.seg, m.off + 2); break; }
        // MOV rm, imm
        case 0xC6: { Modrm m; decode_modrm(m); w8m(m, fetch8()); break; }
        case 0xC7: { Modrm m; decode_modrm(m); w16m(m, fetch16()); break; }
        // RET far
        case 0xCA: { uint16_t n = fetch16(); ip = pop16(); sreg[CS] = pop16(); r[SP] += n; break; }
        case 0xCB: { ip = pop16(); sreg[CS] = pop16(); break; }
        // INT
        case 0xCC: interrupt(3); break;
        case 0xCD: { uint8_t n = fetch8(); interrupt(n); break; }
        case 0xCE: if (get_flag(OF)) interrupt(4); break;
        case 0xCF: { ip = pop16(); sreg[CS] = pop16(); flags = pop16() | 0x0002; break; }   // IRET

        // CALL / JMP
        case 0xE8: { int16_t rel = static_cast<int16_t>(fetch16()); push16(ip); ip += rel; break; }   // CALL near rel
        case 0xE9: { int16_t rel = static_cast<int16_t>(fetch16()); ip += rel; break; }               // JMP near rel
        case 0xEA: { uint16_t no = fetch16(); uint16_t ns = fetch16(); ip = no; sreg[CS] = ns; break; } // JMP far
        case 0xEB: { int8_t rel = static_cast<int8_t>(fetch8()); ip += rel; break; }                  // JMP short
        case 0x9A: { uint16_t no = fetch16(); uint16_t ns = fetch16(); push16(sreg[CS]); push16(ip); ip = no; sreg[CS] = ns; break; }  // CALL far

        // LOOP / JCXZ
        case 0xE0: { int8_t rel = static_cast<int8_t>(fetch8()); if (--r[CX] != 0 && !get_flag(ZF)) ip += rel; break; }  // LOOPNZ
        case 0xE1: { int8_t rel = static_cast<int8_t>(fetch8()); if (--r[CX] != 0 &&  get_flag(ZF)) ip += rel; break; }  // LOOPZ
        case 0xE2: { int8_t rel = static_cast<int8_t>(fetch8()); if (--r[CX] != 0) ip += rel; break; }                   // LOOP
        case 0xE3: { int8_t rel = static_cast<int8_t>(fetch8()); if (r[CX] == 0) ip += rel; break; }                     // JCXZ

        // IN/OUT (no hardware; read 0, ignore writes)
        case 0xE4: fetch8(); sb(AX, 0); break;
        case 0xE5: fetch8(); r[AX] = 0; break;
        case 0xE6: fetch8(); break;
        case 0xE7: fetch8(); break;
        case 0xEC: sb(AX, 0); break;
        case 0xED: r[AX] = 0; break;
        case 0xEE: break;
        case 0xEF: break;

        // Flag ops
        case 0xF5: set_flag(CF, !get_flag(CF)); break;   // CMC
        case 0xF8: set_flag(CF, false); break;
        case 0xF9: set_flag(CF, true); break;
        case 0xFA: set_flag(IF, false); break;
        case 0xFB: set_flag(IF, true); break;
        case 0xFC: set_flag(DF, false); break;
        case 0xFD: set_flag(DF, true); break;
        case 0xF4: halted = true; break;                 // HLT

        // Group 3: F6/F7 (TEST/NOT/NEG/MUL/IMUL/DIV/IDIV)
        case 0xF6: { Modrm m; decode_modrm(m);
            switch (m.reg) {
                case 0: case 1: alu8(4, r8m(m), fetch8()); break;                               // TEST
                case 2: w8m(m, ~r8m(m)); break;                                                 // NOT
                case 3: { uint8_t v = r8m(m); bool c = v != 0; w8m(m, alu8(5, 0, v)); set_flag(CF, c); break; }  // NEG
                case 4: { uint16_t p = (uint16_t)gb(AX) * r8m(m); r[AX] = p; set_flag(CF, (p>>8)!=0); set_flag(OF, (p>>8)!=0); break; }   // MUL
                case 5: { int16_t p = (int16_t)(int8_t)gb(AX) * (int8_t)r8m(m); r[AX] = p; bool s = (int8_t)(p&0xFF) != p; set_flag(CF,s); set_flag(OF,s); break; } // IMUL
                case 6: { uint16_t dv = r[AX]; uint8_t b = r8m(m); if (!b) { interrupt(0); break; } uint8_t q = dv / b, rem = dv % b; r[AX] = q | (rem << 8); break; }  // DIV
                case 7: { int16_t dv = (int16_t)r[AX]; int8_t b = (int8_t)r8m(m); if (!b) { interrupt(0); break; } int8_t q = dv / b; int8_t rem = dv % b; r[AX] = (uint8_t)q | ((uint8_t)rem << 8); break; }  // IDIV
            } break; }
        case 0xF7: { Modrm m; decode_modrm(m);
            switch (m.reg) {
                case 0: case 1: alu16(4, r16m(m), fetch16()); break;
                case 2: w16m(m, ~r16m(m)); break;
                case 3: { uint16_t v = r16m(m); bool c = v != 0; w16m(m, alu16(5, 0, v)); set_flag(CF, c); break; }
                case 4: { uint32_t p = (uint32_t)r[AX] * r16m(m); r[AX] = p & 0xFFFF; r[DX] = p >> 16; set_flag(CF, r[DX]!=0); set_flag(OF, r[DX]!=0); break; }   // MUL
                case 5: { int32_t p = (int32_t)(int16_t)r[AX] * (int16_t)r16m(m); r[AX] = p & 0xFFFF; r[DX] = (p >> 16) & 0xFFFF; bool s = (int16_t)(p&0xFFFF) != p; set_flag(CF,s); set_flag(OF,s); break; }  // IMUL
                case 6: { uint32_t dv = ((uint32_t)r[DX] << 16) | r[AX]; uint16_t b = r16m(m); if (!b) { interrupt(0); break; } r[AX] = dv / b; r[DX] = dv % b; break; }   // DIV
                case 7: { int32_t dv = ((uint32_t)r[DX] << 16) | r[AX]; int16_t b = (int16_t)r16m(m); if (!b) { interrupt(0); break; } r[AX] = (int16_t)(dv / b); r[DX] = (int16_t)(dv % b); break; }  // IDIV
            } break; }

        // Group 4/5: FE/FF (INC/DEC/CALL/JMP/PUSH)
        case 0xFE: { Modrm m; decode_modrm(m); bool c = get_flag(CF);
            if (m.reg == 0) w8m(m, alu8(0, r8m(m), 1)); else w8m(m, alu8(5, r8m(m), 1)); set_flag(CF, c); break; }
        case 0xFF: { Modrm m; decode_modrm(m); bool c = get_flag(CF);
            switch (m.reg) {
                case 0: w16m(m, alu16(0, r16m(m), 1)); set_flag(CF, c); break;   // INC
                case 1: w16m(m, alu16(5, r16m(m), 1)); set_flag(CF, c); break;   // DEC
                case 2: push16(ip); ip = r16m(m); break;                        // CALL near rm
                case 3: { uint16_t no = mem_.rw(m.seg, m.off); uint16_t ns = mem_.rw(m.seg, m.off+2); push16(sreg[CS]); push16(ip); ip = no; sreg[CS] = ns; break; }  // CALL far rm
                case 4: ip = r16m(m); break;                                    // JMP near rm
                case 5: { uint16_t no = mem_.rw(m.seg, m.off); uint16_t ns = mem_.rw(m.seg, m.off+2); ip = no; sreg[CS] = ns; break; }  // JMP far rm
                case 6: push16(r16m(m)); break;                                 // PUSH rm
            } break; }

        // x87 FPU escape opcodes (D8-DF). The FPU is not modelled; LSI C's small
        // model does floating point in software and only touches the FPU to probe
        // and initialise it at startup. Consume the ModRM/operand and no-op, but
        // answer FNSTSW AX (DF /4, mod=3) so a CRT believes the FPU came up clean.
        case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
            Modrm m; decode_modrm(m);
            if (op == 0xDF && m.mod == 3 && m.reg == 4) r[AX] = 0;   // FNSTSW AX
            break;
        }

        default:
            fail("unimplemented instruction", op);
    }
}

}  // namespace dosemu
