// Real-mode physical memory: a flat 1 MiB array addressed as segment:offset,
// where the physical address is (segment << 4) + offset. Everything the 8086 can
// reach lives here — there is no paging and no protection in real mode.
#pragma once
#include <cstdint>
#include <vector>
#include <cstring>

namespace dosemu {

class Memory {
public:
    Memory() : ram_(kSize, 0) {}

    static constexpr uint32_t kSize = 0x110000;  // 1 MiB + a little for HMA wrap

    static uint32_t phys(uint16_t seg, uint16_t off) {
        return ((static_cast<uint32_t>(seg) << 4) + off) & 0xFFFFF;
    }

    uint8_t  r8(uint32_t a)  const { return ram_[a & 0xFFFFF]; }
    uint16_t r16(uint32_t a) const { a &= 0xFFFFF; return static_cast<uint16_t>(ram_[a] | (ram_[a + 1] << 8)); }
    uint32_t r32(uint32_t a) const { a &= 0xFFFFF; return ram_[a] | (ram_[a+1] << 8) | (ram_[a+2] << 16) | (static_cast<uint32_t>(ram_[a+3]) << 24); }

    void w8(uint32_t a, uint8_t v)   { ram_[a & 0xFFFFF] = v; }
    void w16(uint32_t a, uint16_t v) { a &= 0xFFFFF; ram_[a] = v & 0xFF; ram_[a + 1] = v >> 8; }
    void w32(uint32_t a, uint32_t v) { a &= 0xFFFFF; ram_[a]=v&0xFF; ram_[a+1]=(v>>8)&0xFF; ram_[a+2]=(v>>16)&0xFF; ram_[a+3]=(v>>24)&0xFF; }

    // segment:offset convenience
    uint8_t  rb(uint16_t s, uint16_t o) const { return r8(phys(s, o)); }
    uint16_t rw(uint16_t s, uint16_t o) const { return r16(phys(s, o)); }
    uint32_t rd(uint16_t s, uint16_t o) const { return r32(phys(s, o)); }
    void     wb(uint16_t s, uint16_t o, uint8_t v)  { w8(phys(s, o), v); }
    void     ww(uint16_t s, uint16_t o, uint16_t v) { w16(phys(s, o), v); }
    void     wd(uint16_t s, uint16_t o, uint32_t v) { w32(phys(s, o), v); }

    void write(uint32_t a, const void* src, uint32_t len) {
        if ((a & 0xFFFFF) + len <= kSize) std::memcpy(&ram_[a & 0xFFFFF], src, len);
    }
    uint8_t* ptr(uint32_t a) { return &ram_[a & 0xFFFFF]; }

private:
    std::vector<uint8_t> ram_;
};

}  // namespace dosemu
