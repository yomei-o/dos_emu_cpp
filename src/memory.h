// Physical memory. The low 1 MiB is real-mode space addressed as segment:offset,
// where the physical address is (segment << 4) + offset. Above that is extended
// memory reachable only by 32-bit linear address in protected mode (for DPMI /
// DOS-extender guests) — there is no paging, the host presents a flat identity map.
#pragma once
#include <cstdint>
#include <vector>
#include <cstring>

namespace dosemu {

class Memory {
public:
    Memory() : ram_(kSize, 0) {}

    // 64 MiB. Sized by the largest real client: DJGPP's cc1 asks the DPMI host for
    // three blocks totalling ~19 MiB to compile a trivial file, and gcc's driver has
    // its own on top. Undersize it and 0501h either fails or, worse, succeeds and the
    // writes wrap at kMask into memory that is already in use.
    static constexpr uint32_t kSize = 0x4000000;

    static constexpr uint32_t kMask = kSize - 1;

    // Real-mode address wrap stays at 1 MiB; extended access uses r*/w* directly.
    static uint32_t phys(uint16_t seg, uint16_t off) {
        return ((static_cast<uint32_t>(seg) << 4) + off) & 0xFFFFF;
    }

    // DOSEMU_MEMCHK=1 reports accesses past the end of RAM instead of letting kMask
    // fold them silently back into memory that is in use. This is the A20 problem in
    // miniature: an address that wraps rather than faults turns a wild pointer into a
    // quiet corruption somewhere else, and the crash surfaces far from the cause.
    static bool check;
    void oob(uint32_t a, const char* what) const;
    void chk(uint32_t a, const char* what) const { if (check && a >= kSize) oob(a, what); }

    uint8_t  r8(uint32_t a)  const { chk(a, "r8"); return ram_[a & kMask]; }
    uint16_t r16(uint32_t a) const { chk(a, "r16"); a &= kMask; return static_cast<uint16_t>(ram_[a] | (ram_[a + 1] << 8)); }
    uint32_t r32(uint32_t a) const { chk(a, "r32"); a &= kMask; return ram_[a] | (ram_[a+1] << 8) | (ram_[a+2] << 16) | (static_cast<uint32_t>(ram_[a+3]) << 24); }

    void w8(uint32_t a, uint8_t v)   { chk(a, "w8"); ram_[a & kMask] = v; }
    void w16(uint32_t a, uint16_t v) { chk(a, "w16"); a &= kMask; ram_[a] = v & 0xFF; ram_[a + 1] = v >> 8; }
    void w32(uint32_t a, uint32_t v) { chk(a, "w32"); a &= kMask; ram_[a]=v&0xFF; ram_[a+1]=(v>>8)&0xFF; ram_[a+2]=(v>>16)&0xFF; ram_[a+3]=(v>>24)&0xFF; }

    // segment:offset convenience
    uint8_t  rb(uint16_t s, uint16_t o) const { return r8(phys(s, o)); }
    uint16_t rw(uint16_t s, uint16_t o) const { return r16(phys(s, o)); }
    uint32_t rd(uint16_t s, uint16_t o) const { return r32(phys(s, o)); }
    void     wb(uint16_t s, uint16_t o, uint8_t v)  { w8(phys(s, o), v); }
    void     ww(uint16_t s, uint16_t o, uint16_t v) { w16(phys(s, o), v); }
    void     wd(uint16_t s, uint16_t o, uint32_t v) { w32(phys(s, o), v); }

    void write(uint32_t a, const void* src, uint32_t len) {
        a &= kMask; if (static_cast<uint64_t>(a) + len <= kSize) std::memcpy(&ram_[a], src, len);
    }
    uint8_t* ptr(uint32_t a) { return &ram_[a & kMask]; }

private:
    std::vector<uint8_t> ram_;
};

}  // namespace dosemu
