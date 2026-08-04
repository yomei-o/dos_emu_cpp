// A built-in DPMI 0.90 host, so 32-bit DOS-extended programs (DJGPP's go32, and
// later DOS/4GW) run without a real CWSDPMI underneath.
//
// The shape of the thing: an extender is a 16-bit real-mode stub wrapping a 32-bit
// image. The stub asks "is a DPMI host present?" via INT 2Fh AX=1687h. We answer yes
// and hand back a real-mode far address. The stub far-calls it; we switch the CPU to
// protected mode and far-return into the stub, which is now 16-bit protected-mode
// code. From there it drives everything through INT 31h — allocate descriptors,
// allocate extended memory, install handlers, reflect DOS calls back down to real
// mode — and finally jumps to the 32-bit image.
//
// Descriptors live in an LDT we own (DPMI hands clients LDT selectors), in extended
// memory above the 1 MiB real-mode area. Clients are supposed to treat selectors as
// opaque, so the only contract is that set_seg() can read them back.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>
#include "cpu.h"
#include "memory.h"

namespace dosemu {

class Dpmi {
public:
    Dpmi(Cpu& cpu, Memory& mem);

    // INT 2Fh AX=1687h — "get DPMI host entry point". Returns true if it consumed
    // the call. Everything else on 2Fh is left to the DOS layer.
    bool int2f();
    // INT 31h — the DPMI service call. Only meaningful in protected mode.
    bool int31();

    bool active() const { return cpu_.pe(); }

    // Reflect an interrupt down to the real-mode handler (the DOS layer). Set by the
    // owner; used by INT 31h function 0300h and by DOS calls made from protected mode.
    std::function<bool(uint8_t)> real_int;
    // Allocate conventional memory (paragraphs -> segment, 0 on failure) for function
    // 0100h. It has to come from the DOS layer's allocator: the block the client asks
    // for here is the same conventional memory INT 21h AH=48h hands out, and two
    // allocators over one heap would overlap.
    std::function<uint16_t(uint16_t)> alloc_dos;
    // The current PSP segment. The mode switch must hand the client ES = a selector for
    // its PSP, which is how an extender finds the command tail and the environment.
    std::function<uint16_t()> get_psp;

    // Log every INT 31h call and its outcome. On by default when DOSEMU_DPMI_TRACE is
    // set; the point is to find out what a client actually needs rather than guessing
    // at the 60-odd functions in the spec.
    bool trace = false;

private:
    Cpu& cpu_;
    Memory& mem_;

    // Where the client far-calls to enter protected mode, and the other entry points we
    // hand out. A real-mode paragraph nothing else uses: the IVT stubs end at 0x008F,
    // the environment block is allocated out of the arena above, and programs load from
    // 0x0100 upward. Seven paragraphs are reserved for this and the list-of-lists.
    //
    //   +0x00  mode-switch entry (INT 2Fh/1687h)
    //   +0x10  raw real -> protected mode switch      (0306h)
    //   +0x20  raw protected -> real mode switch      (0306h)
    //   +0x30  save/restore state, real mode          (0305h) -- a bare far RET
    //   +0x40  save/restore state, protected mode     (0305h) -- ditto
    //   +0x50  where a callback's IRETD lands, to come back to real mode
    //   +0x60  32 real-mode callback addresses, one byte apart (0303h)
    static constexpr uint16_t kEntrySeg   = 0x00E0;
    static constexpr uint16_t kOffRawToPm = 0x10;
    static constexpr uint16_t kOffRawToRm = 0x20;
    static constexpr uint16_t kOffSaveRm  = 0x30;
    static constexpr uint16_t kOffSavePm  = 0x40;
    static constexpr uint16_t kOffCbRet   = 0x50;
    static constexpr uint16_t kOffCbBase  = 0x60;
    static constexpr int      kCallbacks  = 32;

    // Our LDT, in extended memory just above the 1 MiB real-mode window.
    static constexpr uint32_t kLdtBase  = 0x00110000;
    static constexpr int      kLdtCount = 1024;          // 8 KiB of descriptors
    // Extended memory handed out by function 0501h starts here.
    static constexpr uint32_t kHeapBase = 0x00200000;    // 2 MiB
    uint32_t heap_next_ = kHeapBase;

    // Allocated blocks, so 0502h can actually give memory back. A bump allocator was
    // fine while clients allocated once, but DJGPP's sbrk grows the heap by allocating
    // a bigger block, copying, and freeing the old one — so with a no-op free, every
    // growth step leaks the previous heap and consumption goes quadratic. cc1plus
    // walked off the end of 64 MiB doing that.
    struct Block { uint32_t base, len; bool used; };
    std::vector<Block> blocks_;
    uint32_t alloc_mem(uint32_t len);
    bool free_mem(uint32_t base);

    int next_sel_ = 1;                                   // LDT index, 0 is the null one

    uint16_t scratch_ss_ = 0;

    // Protected-mode exception and interrupt handlers the client installed. Stored and
    // returned faithfully; nothing dispatches to them yet, because nothing in the
    // emulator raises a fault or an IRQ.
    struct Handler { uint16_t sel = 0; uint32_t off = 0; };
    Handler exc_[32], pmint_[256];

    // A real-mode callback (0303h): a real-mode address that, when called, runs a
    // protected-mode procedure of the client's with the real-mode register state laid out
    // in a structure it nominated.
    struct Callback {
        uint16_t proc_sel = 0; uint32_t proc_off = 0;    // the client's procedure
        uint16_t str_sel = 0;  uint32_t str_off = 0;     // its register structure
        bool used = false;
    };
    Callback cb_[kCallbacks];
    uint16_t low_sel_ = 0, cb_ss_ = 0;
    uint32_t cb_sp_ = 0;
    uint16_t low_sel();              // base 0, the whole first megabyte
    void cb_stack(uint16_t& sel, uint32_t& sp);   // a stack for a callback to run on
    void call_back(int slot);
    void cb_return();

    void switch_to_pm();
    void raw_switch(bool to_pm);
    void simulate_real_int();
    uint16_t entry_sel();            // a selector over kEntrySeg, for the PM entry points
    uint16_t entry_sel_ = 0;
    uint16_t scratch_stack_seg();
    // Allocate `count` consecutive LDT descriptors; returns the first selector.
    uint16_t alloc_sel(int count = 1);
    void set_desc(uint16_t sel, uint32_t base, uint32_t limit, bool code, bool big);
    uint32_t desc_addr(uint16_t sel) const { return kLdtBase + (sel & ~7u); }
};

}  // namespace dosemu
