// An 8086/80386 real-mode interpreter. The low 16 bits of each general register
// live in r[]; the upper 16 bits (for the 386's EAX..EDI) live in the parallel
// rhi[]. 16-bit instructions touch only r[], so they preserve the upper halves the
// way a real 386 does — the working 16-bit paths stay byte-for-byte unchanged, and
// the 0x66/0x67 prefixes select 32-bit operand/address size on top. INT n is handed
// to a callback so the DOS/BIOS layer can service it. Protected mode is not entered
// yet, but the system registers below are populated by LGDT/LMSW/MOV CRn so the
// mode switch (for DPMI/DOS-extender guests) can be wired up next.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include "memory.h"

namespace dosemu {

// General registers in 8086 ModRM order.
enum Reg16 { AX, CX, DX, BX, SP, BP, SI, DI };
// Segment registers in 386 ModRM order (FS/GS added for the flat 32-bit models).
enum Seg { ES, CS, SS, DS, FS, GS };

// FLAGS bits.
enum { CF = 1u << 0, PF = 1u << 2, AF = 1u << 4, ZF = 1u << 6, SF = 1u << 7,
       TF = 1u << 8, IF = 1u << 9, DF = 1u << 10, OF = 1u << 11 };

struct CpuError {
    std::string what;
    uint16_t cs, ip;
};

class Cpu {
public:
    explicit Cpu(Memory& mem) : mem_(mem) {}

    uint16_t r[8] = {0};       // low 16 bits of AX,CX,DX,BX,SP,BP,SI,DI
    uint16_t rhi[8] = {0};     // upper 16 bits (EAX..EDI); 16-bit ops leave these alone
    uint16_t sreg[6] = {0};    // ES,CS,SS,DS,FS,GS
    uint16_t ip = 0;
    uint16_t flags = 0x0002;   // bit 1 is always set on the 8086
    bool halted = false;
    int exit_code = 0;

    // 386 system registers. Populated by the protected-mode/system instructions;
    // the mode switch that consumes them is the next milestone (see resume.md).
    uint32_t cr[8] = {0};
    uint32_t dr[8] = {0};
    uint32_t gdt_base = 0, idt_base = 0;
    uint16_t gdt_limit = 0, idt_limit = 0;
    uint16_t ldtr = 0, tr = 0;

    // Byte-register access (AL/CL/DL/BL/AH/CH/DH/BH by ModRM index 0..7).
    uint8_t  gb(int i) const { return i < 4 ? (r[i] & 0xFF) : (r[i - 4] >> 8); }
    void     sb(int i, uint8_t v) { if (i < 4) r[i] = (r[i] & 0xFF00) | v; else r[i-4] = (r[i-4] & 0x00FF) | (v << 8); }
    // 32-bit register access (EAX..EDI by index 0..7).
    uint32_t gd(int i) const { return (static_cast<uint32_t>(rhi[i]) << 16) | r[i]; }
    void     sd(int i, uint32_t v) { r[i] = static_cast<uint16_t>(v); rhi[i] = static_cast<uint16_t>(v >> 16); }

    // INT n service: the handler inspects/updates registers. Return false to let
    // the CPU fall through to the (usually unused) real IVT behaviour.
    std::function<bool(uint8_t)> on_int;

    void run();          // until halted
    void step();         // one instruction
    void interrupt(uint8_t n);   // push flags/cs/ip and vector, or call on_int

    Memory& mem() { return mem_; }
    uint64_t insns = 0;
    uint64_t max_insns = 0;   // 0 = unlimited; otherwise stop with an error (runaway guard)

private:
    Memory& mem_;

    // instruction fetch
    uint8_t  fetch8()  { return mem_.rb(sreg[CS], ip++); }
    uint16_t fetch16() { uint16_t v = mem_.rw(sreg[CS], ip); ip += 2; return v; }
    uint32_t fetch32() { uint32_t v = mem_.rd(sreg[CS], ip); ip += 4; return v; }

    // stack (real-mode: SP is the 16-bit offset into SS)
    void push16(uint16_t v) { r[SP] -= 2; mem_.ww(sreg[SS], r[SP], v); }
    uint16_t pop16() { uint16_t v = mem_.rw(sreg[SS], r[SP]); r[SP] += 2; return v; }
    void push32(uint32_t v) { r[SP] -= 4; mem_.wd(sreg[SS], r[SP], v); }
    uint32_t pop32() { uint32_t v = mem_.rd(sreg[SS], r[SP]); r[SP] += 4; return v; }

    void set_flag(uint32_t f, bool on) { if (on) flags |= f; else flags &= ~f; }
    bool get_flag(uint32_t f) const { return (flags & f) != 0; }

    [[noreturn]] void fail(const std::string& msg, uint8_t op);
};

}  // namespace dosemu
