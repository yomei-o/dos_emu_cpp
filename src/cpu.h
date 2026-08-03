// A 16-bit 8086 real-mode interpreter. Register state is 16-bit; byte registers
// (AL/AH/...) are views into the word registers. INT n is handed to a callback so
// the DOS/BIOS layer can service it.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include "memory.h"

namespace dosemu {

// General registers in 8086 ModRM order.
enum Reg16 { AX, CX, DX, BX, SP, BP, SI, DI };
// Segment registers in 8086 ModRM order.
enum Seg { ES, CS, SS, DS };

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

    uint16_t r[8] = {0};       // AX,CX,DX,BX,SP,BP,SI,DI
    uint16_t sreg[4] = {0};    // ES,CS,SS,DS
    uint16_t ip = 0;
    uint16_t flags = 0x0002;   // bit 1 is always set on the 8086
    bool halted = false;
    int exit_code = 0;

    // Byte-register access (AL/CL/DL/BL/AH/CH/DH/BH by ModRM index 0..7).
    uint8_t  gb(int i) const { return i < 4 ? (r[i] & 0xFF) : (r[i - 4] >> 8); }
    void     sb(int i, uint8_t v) { if (i < 4) r[i] = (r[i] & 0xFF00) | v; else r[i-4] = (r[i-4] & 0x00FF) | (v << 8); }

    // INT n service: the handler inspects/updates registers. Return false to let
    // the CPU fall through to the (usually unused) real IVT behaviour.
    std::function<bool(uint8_t)> on_int;

    void run();          // until halted
    void step();         // one instruction
    void interrupt(uint8_t n);   // push flags/cs/ip and vector, or call on_int

    Memory& mem() { return mem_; }
    uint64_t insns = 0;

private:
    Memory& mem_;

    // instruction fetch
    uint8_t  fetch8()  { return mem_.rb(sreg[CS], ip++); }
    uint16_t fetch16() { uint16_t v = mem_.rw(sreg[CS], ip); ip += 2; return v; }

    // stack
    void push16(uint16_t v) { r[SP] -= 2; mem_.ww(sreg[SS], r[SP], v); }
    uint16_t pop16() { uint16_t v = mem_.rw(sreg[SS], r[SP]); r[SP] += 2; return v; }

    void set_flag(uint32_t f, bool on) { if (on) flags |= f; else flags &= ~f; }
    bool get_flag(uint32_t f) const { return (flags & f) != 0; }

    [[noreturn]] void fail(const std::string& msg, uint8_t op);
};

}  // namespace dosemu
