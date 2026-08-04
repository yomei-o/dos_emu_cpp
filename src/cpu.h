// An 8086/80386 interpreter with real mode and (for DPMI / DOS-extender guests)
// 32-bit protected mode. The low 16 bits of each general register live in r[]; the
// upper 16 bits (EAX..EDI) live in the parallel rhi[], so 16-bit instructions touch
// only r[] and preserve the upper halves the way a real 386 does — the 16-bit paths
// stay byte-for-byte unchanged. The 0x66/0x67 prefixes select 32-bit operand/address
// size on top (relative to the code segment's default size).
//
// Addressing goes through lin(): in real mode a segment is base = value<<4; in
// protected mode (cr0.PE) it is the cached descriptor base loaded by set_seg() from
// the GDT/LDT. Real-mode paths compute the same (value<<4)+offset as before, so
// enabling protected mode does not disturb them. INT n is handed to a callback (the
// DOS/BIOS layer, and the DPMI host in protected mode).
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
    uint16_t sreg[6] = {0};    // ES,CS,SS,DS,FS,GS (selector values)
    uint32_t ip = 0;           // 16- or 32-bit instruction pointer
    uint16_t flags = 0x0002;   // bit 1 is always set on the 8086
    bool halted = false;
    int exit_code = 0;

    // 386 system + protected-mode state.
    uint32_t cr[8] = {0};
    uint32_t dr[8] = {0};
    uint32_t gdt_base = 0, idt_base = 0, ldt_base = 0;
    uint16_t gdt_limit = 0, idt_limit = 0;
    uint16_t ldtr = 0, tr = 0;
    // Descriptor cache: base/limit per segment register, and the default-size (D/B)
    // bits for CS and SS. Maintained by set_seg(); only consulted in protected mode.
    uint32_t sbase[6] = {0};
    uint32_t slimit[6] = {0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF};
    bool cs_d = false, ss_d = false;
    // How wide the instruction pointer is *in the current code segment*. When ip was a
    // uint16_t this was implicit: every ip += rel wrapped inside the 64 KiB segment, which
    // is what a 16-bit code segment does. Widening ip to 32 bits silently removed that, so
    // a backward branch from a low offset became 0xFFFFxxxx and the linear address landed
    // 64 KiB below the segment — in zeroed memory. Every write to ip goes through this
    // mask; it is 0xFFFFFFFF only inside a D=1 (32-bit) code segment.
    uint32_t ip_mask = 0xFFFFu;

    bool pe() const { return (cr[0] & 1) != 0; }
    // Linear address for a segment index + offset. Real mode wraps at 1 MiB exactly
    // as the old segment:offset arithmetic did; protected mode uses the descriptor
    // base into the full extended address space.
    uint32_t lin(int s, uint32_t off) const {
        if (pe()) return sbase[s] + off;
        return ((static_cast<uint32_t>(sreg[s]) << 4) + off) & 0xFFFFF;
    }
    // A decoded GDT/LDT descriptor. `ar` is the access-rights byte (descriptor byte 5)
    // and `hi` the flags nibble (byte 6), which is what LAR reports.
    struct Desc { uint32_t base, limit; uint8_t ar, hi; bool big, present; };
    Desc read_desc(uint16_t sel) const {
        const uint32_t tbl = (sel & 4) ? ldt_base : gdt_base;
        const uint32_t da = tbl + (sel & ~7u);
        Desc d{};
        d.base = mem_.r16(da + 2) | (static_cast<uint32_t>(mem_.r8(da + 4)) << 16)
               | (static_cast<uint32_t>(mem_.r8(da + 7)) << 24);
        d.ar = mem_.r8(da + 5);
        d.hi = mem_.r8(da + 6);
        d.limit = mem_.r16(da) | ((d.hi & 0x0Fu) << 16);
        if (d.hi & 0x80) d.limit = (d.limit << 12) | 0xFFF;    // 4 KiB granularity
        d.big = (d.hi & 0x40) != 0;
        d.present = (d.ar & 0x80) != 0;
        return d;
    }

    // Load a segment register. In protected mode this reads the descriptor from the
    // GDT/LDT into the cache; in real mode base is just selector<<4.
    void set_seg(int i, uint16_t sel) {
        sreg[i] = sel;
        if (!pe()) { sbase[i] = static_cast<uint32_t>(sel) << 4; slimit[i] = 0xFFFF;
                     if (i == CS) { cs_d = false; ip_mask = 0xFFFFu; }
                     if (i == SS) ss_d = false;
                     return; }
        const Desc d = read_desc(sel);
        sbase[i] = d.base; slimit[i] = d.limit;
        if (i == CS) { cs_d = d.big; ip_mask = d.big ? 0xFFFFFFFFu : 0xFFFFu; }
        if (i == SS) ss_d = d.big;
    }

    // Every near control transfer goes through this: a 16-bit code segment wraps EIP at
    // 64 KiB, a 32-bit one does not. Far transfers set CS first, so the new segment's
    // width is already in ip_mask by the time they land here.
    void jump(uint32_t v) { ip = v & ip_mask; }

    // Byte-register access (AL/CL/DL/BL/AH/CH/DH/BH by ModRM index 0..7).
    uint8_t  gb(int i) const { return i < 4 ? (r[i] & 0xFF) : (r[i - 4] >> 8); }
    void     sb(int i, uint8_t v) { if (i < 4) r[i] = (r[i] & 0xFF00) | v; else r[i-4] = (r[i-4] & 0x00FF) | (v << 8); }
    // 32-bit register access (EAX..EDI by index 0..7).
    uint32_t gd(int i) const { return (static_cast<uint32_t>(rhi[i]) << 16) | r[i]; }
    void     sd(int i, uint32_t v) { r[i] = static_cast<uint16_t>(v); rhi[i] = static_cast<uint16_t>(v >> 16); }

    // Stack pointer honours the SS default-size (B) bit in protected mode.
    uint32_t sp_get() const { return (pe() && ss_d) ? gd(SP) : r[SP]; }
    void     sp_set(uint32_t v) { if (pe() && ss_d) sd(SP, v); else r[SP] = static_cast<uint16_t>(v); }
    void push16(uint16_t v) { sp_set(sp_get() - 2); mem_.w16(lin(SS, sp_get()), v); }
    uint16_t pop16() { uint16_t v = mem_.r16(lin(SS, sp_get())); sp_set(sp_get() + 2); return v; }
    void push32(uint32_t v) { sp_set(sp_get() - 4); mem_.w32(lin(SS, sp_get()), v); }
    uint32_t pop32() { uint32_t v = mem_.r32(lin(SS, sp_get())); sp_set(sp_get() + 4); return v; }

    // INT n service: the handler inspects/updates registers. Return false to let
    // the CPU fall through to the (usually unused) real IVT behaviour.
    std::function<bool(uint8_t)> on_int;
    // The DPMI mode-switch entry: when execution reaches this linear address, the
    // host performs the real→protected switch instead of executing an instruction.
    std::function<void()> on_pm_switch;
    uint32_t pm_switch_addr = 0xFFFFFFFFu;

    // Everything an interruption has to preserve: the register file plus the mode and
    // descriptor-cache state that says how those registers are interpreted. Saving the
    // registers alone is not enough once protected mode exists — the DPMI host drops to
    // real mode to service a reflected DOS call, and cr0/cs_d/ip_mask/sbase have to come
    // back with the rest or the client resumes with real-mode addressing.
    struct State {
        uint16_t r[8], rhi[8], sreg[6];
        uint32_t sbase[6], slimit[6];
        uint32_t ip, cr0, ip_mask;
        uint16_t flags;
        bool cs_d, ss_d;
    };
    State save() const {
        State s{};
        for (int i = 0; i < 8; ++i) { s.r[i] = r[i]; s.rhi[i] = rhi[i]; }
        for (int i = 0; i < 6; ++i) { s.sreg[i] = sreg[i]; s.sbase[i] = sbase[i]; s.slimit[i] = slimit[i]; }
        s.ip = ip; s.cr0 = cr[0]; s.ip_mask = ip_mask; s.flags = flags;
        s.cs_d = cs_d; s.ss_d = ss_d;
        return s;
    }
    void restore(const State& s) {
        for (int i = 0; i < 8; ++i) { r[i] = s.r[i]; rhi[i] = s.rhi[i]; }
        for (int i = 0; i < 6; ++i) { sreg[i] = s.sreg[i]; sbase[i] = s.sbase[i]; slimit[i] = s.slimit[i]; }
        ip = s.ip; cr[0] = s.cr0; ip_mask = s.ip_mask; flags = s.flags;
        cs_d = s.cs_d; ss_d = s.ss_d;
    }

    void run();          // until halted
    void step();         // one instruction
    void interrupt(uint8_t n);   // push flags/cs/ip and vector, or call on_int

    Memory& mem() { return mem_; }
    uint64_t insns = 0;
    uint64_t max_insns = 0;   // 0 = unlimited; otherwise stop with an error (runaway guard)

    void set_flag(uint32_t f, bool on) { if (on) flags |= f; else flags &= ~f; }
    bool get_flag(uint32_t f) const { return (flags & f) != 0; }

private:
    Memory& mem_;

    // Instruction fetch (via lin() so it follows the code segment's base). The advance is
    // masked for the same reason as jump(): in a 16-bit segment ip wraps at 0xFFFF.
    uint8_t  fetch8()  { uint8_t v = mem_.r8(lin(CS, ip)); ip = (ip + 1) & ip_mask; return v; }
    uint16_t fetch16() { uint16_t v = mem_.r16(lin(CS, ip)); ip = (ip + 2) & ip_mask; return v; }
    uint32_t fetch32() { uint32_t v = mem_.r32(lin(CS, ip)); ip = (ip + 4) & ip_mask; return v; }

    [[noreturn]] void fail(const std::string& msg, uint8_t op);
};

}  // namespace dosemu
