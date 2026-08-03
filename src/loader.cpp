// Loads a DOS program into real-mode memory and sets up the PSP and initial
// registers. .COM is a flat image loaded at PSP:0x100; .EXE (MZ) has a header, a
// relocation table, and its own initial CS:IP / SS:SP relative to the load segment.
#include <cstdint>
#include <vector>
#include <string>
#include "cpu.h"
#include "memory.h"

namespace dosemu {

// Where programs load. Segment 0x0100 (=64 KiB) leaves the interrupt vectors, BIOS
// data and a nominal DOS below it.
static constexpr uint16_t kLoadSeg = 0x0100;

static void build_psp(Memory& mem, uint16_t psp_seg, const std::string& cmdline) {
    // Minimal PSP. INT 20h at offset 0, and the command tail at 0x80.
    mem.wb(psp_seg, 0x00, 0xCD); mem.wb(psp_seg, 0x01, 0x20);   // INT 20h
    // command tail: length byte then the text then CR
    std::string tail = cmdline;
    if (tail.size() > 126) tail.resize(126);
    mem.wb(psp_seg, 0x80, static_cast<uint8_t>(tail.size()));
    for (size_t i = 0; i < tail.size(); ++i) mem.wb(psp_seg, 0x81 + i, tail[i]);
    mem.wb(psp_seg, 0x81 + tail.size(), 0x0D);
}

// Returns true on success; fills the CPU's initial state.
bool load_program(const std::vector<uint8_t>& f, Cpu& cpu, uint16_t& out_psp,
                  const std::string& cmdline, std::string& err) {
    Memory& mem = cpu.mem();
    uint16_t psp_seg = kLoadSeg;
    uint16_t load_seg = psp_seg + 0x10;   // program starts 256 bytes (one PSP) above

    build_psp(mem, psp_seg, cmdline);
    out_psp = psp_seg;

    bool is_mz = f.size() >= 2 && f[0] == 'M' && f[1] == 'Z';
    if (is_mz) {
        auto rd16 = [&](size_t o) { return static_cast<uint16_t>(f[o] | (f[o + 1] << 8)); };
        uint16_t bytes_last = rd16(2), pages = rd16(4), nreloc = rd16(6), hdr_paras = rd16(8);
        uint16_t ss = rd16(14), sp = rd16(16), ip = rd16(20), cs = rd16(22), reloc_off = rd16(24);
        uint32_t hdr_bytes = static_cast<uint32_t>(hdr_paras) * 16;
        uint32_t image_bytes = static_cast<uint32_t>(pages) * 512;
        if (bytes_last) image_bytes = image_bytes - 512 + bytes_last;
        if (image_bytes > f.size()) image_bytes = static_cast<uint32_t>(f.size());
        uint32_t body = image_bytes - hdr_bytes;

        // Load the image body at load_seg:0.
        mem.write(Memory::phys(load_seg, 0), f.data() + hdr_bytes, body);
        // Apply relocations: each entry is an offset:segment pointing at a word that
        // needs load_seg added to it.
        for (uint16_t i = 0; i < nreloc; ++i) {
            uint16_t ro = rd16(reloc_off + i * 4);
            uint16_t rs = rd16(reloc_off + i * 4 + 2);
            uint16_t cur = mem.rw(load_seg + rs, ro);
            mem.ww(load_seg + rs, ro, cur + load_seg);
        }
        cpu.sreg[CS] = load_seg + cs; cpu.ip = ip;
        cpu.sreg[SS] = load_seg + ss; cpu.r[SP] = sp;
        cpu.sreg[DS] = psp_seg; cpu.sreg[ES] = psp_seg;
        (void)err;
        return true;
    }

    // .COM: flat load at PSP:0x100, all segment registers = PSP, SP at top of segment.
    if (f.size() > 0xFF00) { err = "COM image too large"; return false; }
    mem.write(Memory::phys(psp_seg, 0x100), f.data(), static_cast<uint32_t>(f.size()));
    cpu.sreg[CS] = cpu.sreg[DS] = cpu.sreg[ES] = cpu.sreg[SS] = psp_seg;
    cpu.ip = 0x100;
    cpu.r[SP] = 0xFFFE;
    mem.ww(psp_seg, 0xFFFE, 0x0000);   // a return address (INT 20h at PSP:0)
    return true;
}

}  // namespace dosemu
