// The DOS personality: it services INT 21h (and a few BIOS interrupts) so a real
// DOS program runs with no DOS underneath. This first cut covers console output,
// program termination, the DOS version query, and the interrupt-vector calls; file
// I/O, memory allocation and EXEC come next.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include "cpu.h"
#include "memory.h"
#include "files.h"

namespace dosemu {

class Dos {
public:
    Dos(Cpu& cpu, Memory& mem, std::string root = ".") : cpu_(cpu), mem_(mem), files_(std::move(root)) {
        cpu_.on_int = [this](uint8_t n) { return handle(n); };
    }

    // Guest output (fd 1/2) goes here; defaults to stdout/stderr.
    std::function<void(int fd, const char* data, size_t len)> output;
    // Guest input (INT 21h AH=01/07/08/0A). Returns the next byte, or -1 at EOF.
    // A '\n' is treated as the DOS Enter key ('\r').
    std::function<int()> input;

    uint16_t psp_seg = 0;   // set by the loader
    DosFiles& files() { return files_; }

    bool handle(uint8_t n);

private:
    Cpu& cpu_;
    Memory& mem_;
    DosFiles files_;

    std::string read_asciiz(uint16_t seg, uint16_t off) {
        std::string s;
        for (int i = 0; i < 128; ++i) { char c = static_cast<char>(mem_.rb(seg, off + i)); if (!c) break; s += c; }
        return s;
    }

    // A bump allocator over conventional memory (paragraphs). Real DOS keeps an MCB
    // chain; a growing pointer is enough to satisfy a program's startup and malloc.
    uint16_t heap_next_ = 0x3000;   // ~192 KiB, above where programs load
    uint16_t heap_end_  = 0x9F00;   // just below the video/BIOS area (~640 KiB)

    // EXEC (AH=4Bh): a child runs on the same CPU via a nested loop. terminate()
    // ends the whole process at depth 0 but only the child at depth > 0.
    int  exec_depth_ = 0;
    bool child_exited_ = false;
    int  child_code_ = 0;
    int  last_child_code_ = 0;
    bool exec(const std::string& name, uint16_t pb_seg, uint16_t pb_off);

    // Disk Transfer Area + FindFirst/FindNext state (for DIR and friends).
    uint16_t dta_seg_ = 0, dta_off_ = 0x80;
    struct Found { std::string name; uint32_t size; bool is_dir; uint16_t date, time; };
    std::vector<Found> find_;
    size_t find_pos_ = 0;
    bool find_first(const std::string& spec, uint16_t attr);
    void write_dta_entry();

    void out(int fd, char c) { char b = c; if (output) output(fd, &b, 1); }
    int  getch() { int c = input ? input() : -1; return c == '\n' ? '\r' : c; }   // -1 at EOF
    bool int21();
    bool int21_default(uint8_t n);
    void terminate(int code);
};

}  // namespace dosemu
