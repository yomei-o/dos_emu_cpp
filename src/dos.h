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
#include "dpmi.h"

namespace dosemu {

class Dos {
public:
    Dos(Cpu& cpu, Memory& mem, std::string root = ".")
        : cpu_(cpu), mem_(mem), files_(std::move(root)), dpmi_(cpu, mem) {
        cpu_.on_int = [this](uint8_t n) { return handle(n); };
        // INT 31h function 0300h and DOS calls made from protected mode both need to
        // land back in this handler, so the DPMI host reflects through it.
        dpmi_.real_int = [this](uint8_t n) { return handle(n); };
        install_ivt_stubs();
        dpmi_.get_psp = [this] { return psp_seg; };
        blocks_.push_back({kArenaLo, static_cast<uint16_t>(heap_end_ - kArenaLo), false});
        dpmi_.alloc_dos = [this](uint16_t paras) { return mem_alloc(paras); };
    }

    // Guest output (fd 1/2) goes here; defaults to stdout/stderr.
    std::function<void(int fd, const char* data, size_t len)> output;
    // Guest input (INT 21h AH=01/07/08/0A). Returns the next byte, or -1 at EOF.
    // A '\n' is treated as the DOS Enter key ('\r').
    std::function<int()> input;

    uint16_t psp_seg = 0;   // set by the loader
    // The current program's environment *segment*. Kept here rather than read back
    // from PSP:2Ch, because the DPMI mode switch rewrites that field into a selector
    // for the protected-mode client — after which it is no longer a segment and the
    // DOS layer cannot use it to build a child's environment.
    uint16_t env_seg = 0;
    // The DOS path of the running program, as it appears after the environment
    // strings. A child inherits *this*, not its own name — see make_child_env().
    std::string prog_path;

    // Fill in the PSP fields the loader cannot know: who the parent is, and where this
    // program's memory block ends. Call after load_program(). A program that only ever
    // reads its command tail never looks at these; one that has to find the program
    // that launched it does, and Watcom's W32RUN is exactly that.
    void init_psp(uint16_t psp, uint16_t parent, const std::string& path);

private:
    uint16_t make_child_env(uint16_t parent_env, const std::string& child_name);
    bool load_overlay(const std::string& name, uint16_t pb_seg, uint16_t pb_off);
public:
    DosFiles& files() { return files_; }

    bool handle(uint8_t n);

    // DOSEMU_DOS_TRACE=1 logs every INT 21h call and what it answered. The DPMI trace
    // only sees calls a protected-mode client reflects; a real-mode extender stub does
    // its DOS work directly, and this is the only way to watch that.
    static bool trace;

private:
    Cpu& cpu_;
    Memory& mem_;
    DosFiles files_;
    Dpmi dpmi_;

    std::string read_asciiz(uint16_t seg, uint16_t off) {
        std::string s;
        for (int i = 0; i < 128; ++i) { char c = static_cast<char>(mem_.rb(seg, off + i)); if (!c) break; s += c; }
        return s;
    }

    // Conventional memory, as a list of blocks in address order.
    //
    // This was a bump pointer, which is enough for a program that starts, mallocs and
    // exits. It is not enough for a DOS extender, because the way one sizes itself is to
    // ask for *everything* and then release what it does not need — and a bump pointer
    // cannot express releasing. Worse, the rule that kept it from handing out memory a
    // program already owned ("if the block is above the mark, move the mark past it")
    // destroyed the whole arena the moment a stub relocated itself high and resized
    // there: wlink's does, and every later allocation failed with "insufficient memory"
    // while 600 KiB sat unused. So: blocks, first fit, and a free that frees.
    struct Block { uint16_t seg, paras; bool used; };
    std::vector<Block> blocks_;                    // contiguous, ordered, covers the arena
    static constexpr uint16_t kArenaLo = 0x0100;   // the first program's PSP
    uint16_t heap_end_ = 0x9F00;                   // just below the video/BIOS area (~640 KiB)

    void     mem_split(uint16_t seg);              // ensure a block boundary at seg
    void     mem_own(uint16_t seg, uint16_t paras); // mark a range used (a program's block)
    uint16_t mem_alloc(uint16_t paras);             // 0 if it does not fit
    bool     mem_free(uint16_t seg);
    bool     mem_resize(uint16_t seg, uint16_t paras);
    uint16_t mem_largest() const;
    void     mem_coalesce();
    void     mem_dump(const char* why) const;

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
    void install_ivt_stubs();
    bool int21();
    bool int21_default(uint8_t n);
    void terminate(int code);
};

}  // namespace dosemu
