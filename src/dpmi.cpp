#include "dpmi.h"
#include <cstdio>
#include <cstdlib>

namespace dosemu {

Dpmi::Dpmi(Cpu& cpu, Memory& mem) : cpu_(cpu), mem_(mem) {
    trace = getenv("DOSEMU_DPMI_TRACE") != nullptr;

    // The entry the client far-calls. The CPU intercepts the address rather than
    // executing anything, but put a far RET there so a stray call still returns
    // instead of running whatever happens to be in memory.
    mem_.wb(kEntrySeg, 0, 0xCB);
    cpu_.pm_switch_addr = Memory::phys(kEntrySeg, 0);
    cpu_.on_pm_switch = [this] { switch_to_pm(); };

    cpu_.ldt_base = kLdtBase;
    for (int i = 0; i < 8; ++i) mem_.w8(kLdtBase + i, 0);   // null descriptor
}

uint16_t Dpmi::alloc_sel(int count) {
    if (next_sel_ + count > kLdtCount) return 0;
    uint16_t sel = static_cast<uint16_t>(next_sel_ * 8 + 4);   // bit 2 = LDT, RPL 0
    next_sel_ += count;
    return sel;
}

// Write one 8-byte descriptor. Limits above 1 MiB switch on the granularity bit, so
// the byte limit the caller asked for is rounded up to the next 4 KiB page.
void Dpmi::set_desc(uint16_t sel, uint32_t base, uint32_t limit, bool code, bool big) {
    uint32_t a = desc_addr(sel);
    uint8_t g = 0;
    if (limit > 0xFFFFF) { limit >>= 12; g = 0x80; }
    mem_.w16(a, static_cast<uint16_t>(limit));
    mem_.w16(a + 2, static_cast<uint16_t>(base));
    mem_.w8(a + 4, static_cast<uint8_t>(base >> 16));
    // present, DPL 3, S=1, then executable/read or data/write, accessed
    mem_.w8(a + 5, static_cast<uint8_t>(0xF0 | (code ? 0x0B : 0x03)));
    mem_.w8(a + 6, static_cast<uint8_t>(g | (big ? 0x40 : 0) | ((limit >> 16) & 0x0F)));
    mem_.w8(a + 7, static_cast<uint8_t>(base >> 24));
}

// INT 2Fh AX=1687h: yes, there is a DPMI host here.
bool Dpmi::int2f() {
    if (cpu_.r[AX] != 0x1687) return false;
    cpu_.r[AX] = 0;                       // 0 = DPMI present
    cpu_.r[BX] = 1;                       // bit 0: 32-bit programs supported
    cpu_.sb(1 /*CL*/, 3);                 // 80386
    cpu_.r[DX] = 0x005A;                  // DPMI version 0.90 (DH=0, DL=90)
    cpu_.r[SI] = 0;                       // paragraphs of private data we need: none
    cpu_.set_seg(ES, kEntrySeg);
    cpu_.r[DI] = 0;                       // ES:DI = the mode-switch entry
    cpu_.set_flag(CF, false);
    if (trace) printf("[dpmi] 2F/1687 -> host at %04X:0000\n", kEntrySeg);
    return true;
}

// The client far-called our entry. Build descriptors for the segments it is using,
// turn on protected mode, and far-return into it.
//
// The return address is still on the real-mode stack: the far CALL pushed CS then IP.
// We consume it here and resume at that address with CS reloaded as a selector, which
// is what the client's `retf` would have done had it stayed in real mode.
void Dpmi::switch_to_pm() {
    const uint16_t ret_ip = mem_.rw(cpu_.sreg[SS], cpu_.r[SP]);
    const uint16_t ret_cs = mem_.rw(cpu_.sreg[SS], cpu_.r[SP] + 2);
    cpu_.r[SP] += 4;

    const bool is32 = (cpu_.r[AX] & 1) != 0;
    const uint16_t rm_ds = cpu_.sreg[DS], rm_ss = cpu_.sreg[SS];
    // ES must come out as a selector for the *PSP*, not an alias of whatever ES the
    // client happened to be holding when it called. That is the spec, and it is not a
    // formality: it is how an extender locates the command tail at PSP:0x80 and the
    // environment segment at PSP:0x2C. Get it wrong and the client starts with argc 0
    // and an empty environment, with nothing else visibly broken.
    const uint16_t psp = get_psp ? get_psp() : cpu_.sreg[ES];

    // Four 16-bit descriptors over the segments the client resumes with.
    const uint16_t cs_sel = alloc_sel();
    const uint16_t ds_sel = alloc_sel();
    const uint16_t es_sel = alloc_sel();
    const uint16_t ss_sel = alloc_sel();
    set_desc(cs_sel, static_cast<uint32_t>(ret_cs) << 4, 0xFFFF, true,  false);
    set_desc(ds_sel, static_cast<uint32_t>(rm_ds)  << 4, 0xFFFF, false, false);
    set_desc(es_sel, static_cast<uint32_t>(psp)    << 4, 0x00FF, false, false);   // the PSP is 256 bytes
    set_desc(ss_sel, static_cast<uint32_t>(rm_ss)  << 4, 0xFFFF, false, false);

    cpu_.cr[0] |= 1;                      // PE — from here set_seg() reads descriptors
    cpu_.set_seg(DS, ds_sel);
    cpu_.set_seg(ES, es_sel);
    cpu_.set_seg(SS, ss_sel);
    cpu_.set_seg(CS, cs_sel);
    cpu_.ip = ret_ip;
    cpu_.set_flag(CF, false);

    if (trace)
        printf("[dpmi] switch to %s protected mode: cs=%04X(%04X) ds=%04X es=%04X(psp %04X) ss=%04X ip=%04X\n",
               is32 ? "32-bit" : "16-bit", cs_sel, ret_cs, ds_sel, es_sel, psp, ss_sel, ret_ip);
}

// INT 31h 0300h — "simulate real-mode interrupt". This is the hinge of the whole
// design: it is how a protected-mode client does file and console I/O. The client
// fills a 50-byte register frame at ES:EDI, names an interrupt in BL, and expects the
// host to run it as though in real mode and hand the registers back.
//
// We satisfy it by *actually* dropping the CPU to real mode for the duration. The DOS
// layer reads its arguments as segment:offset (read_asciiz(seg, off) and friends), so
// with cr0.PE clear and the frame's segment values loaded, every INT 21h handler works
// unmodified — no second, protected-mode-aware copy of the DOS layer. The frame's
// segments are real-mode paragraphs by definition, which is exactly what it wants.
//
// The client's own protected-mode state is saved and restored around the call; only
// the frame is written back.
void Dpmi::simulate_real_int() {
    const uint8_t vec = static_cast<uint8_t>(cpu_.r[BX] & 0xFF);   // BL
    const uint32_t f = cpu_.lin(ES, cpu_.gd(DI));

    // Snapshot everything the call is allowed to disturb.
    const Cpu::State saved = cpu_.save();

    cpu_.cr[0] &= ~1u;                                   // back to real mode
    cpu_.ip_mask = 0xFFFFu; cpu_.cs_d = cpu_.ss_d = false;

    cpu_.sd(DI, mem_.r32(f + 0x00)); cpu_.sd(SI, mem_.r32(f + 0x04));
    cpu_.sd(BP, mem_.r32(f + 0x08)); cpu_.sd(BX, mem_.r32(f + 0x10));
    cpu_.sd(DX, mem_.r32(f + 0x14)); cpu_.sd(CX, mem_.r32(f + 0x18));
    cpu_.sd(AX, mem_.r32(f + 0x1C));
    cpu_.flags = mem_.r16(f + 0x20);
    cpu_.set_seg(ES, mem_.r16(f + 0x22)); cpu_.set_seg(DS, mem_.r16(f + 0x24));
    cpu_.set_seg(FS, mem_.r16(f + 0x26)); cpu_.set_seg(GS, mem_.r16(f + 0x28));

    // SS:SP zero means "host, provide a stack". Nothing we service actually runs guest
    // code on it, but a handler that pushes would otherwise scribble on segment 0.
    uint16_t ss = mem_.r16(f + 0x30), sp = mem_.r16(f + 0x2E);
    if (!ss && !sp) { ss = scratch_stack_seg(); sp = 0x0100; }
    cpu_.set_seg(SS, ss); cpu_.r[SP] = sp;

    if (trace) {
        printf("[dpmi]   reflect INT %02X ah=%02X al=%02X ds=%04X dx=%04X cx=%04X",
               vec, cpu_.r[AX] >> 8, cpu_.r[AX] & 0xFF, cpu_.sreg[DS], cpu_.r[DX], cpu_.r[CX]);
        // Show what a write actually writes. Whether a client's output is empty is the
        // difference between "the DOS call failed" and "the client had nothing to say",
        // and those two have completely different causes.
        if (vec == 0x21 && (cpu_.r[AX] >> 8) == 0x40) {
            printf("  \"");
            for (uint16_t i = 0; i < cpu_.r[CX] && i < 60; ++i) {
                const uint8_t c = mem_.rb(cpu_.sreg[DS], static_cast<uint16_t>(cpu_.r[DX] + i));
                printf("%c", (c >= 32 && c < 127) ? c : '.');
            }
            printf("\"");
        }
        printf("\n");
    }
    if (real_int) real_int(vec);

    // Hand the results back through the frame.
    mem_.w32(f + 0x00, cpu_.gd(DI)); mem_.w32(f + 0x04, cpu_.gd(SI));
    mem_.w32(f + 0x08, cpu_.gd(BP)); mem_.w32(f + 0x10, cpu_.gd(BX));
    mem_.w32(f + 0x14, cpu_.gd(DX)); mem_.w32(f + 0x18, cpu_.gd(CX));
    mem_.w32(f + 0x1C, cpu_.gd(AX));
    mem_.w16(f + 0x20, cpu_.flags);
    mem_.w16(f + 0x22, cpu_.sreg[ES]); mem_.w16(f + 0x24, cpu_.sreg[DS]);
    mem_.w16(f + 0x26, cpu_.sreg[FS]); mem_.w16(f + 0x28, cpu_.sreg[GS]);

    cpu_.restore(saved);                                 // back to the client's world
    cpu_.set_flag(CF, false);                            // 0300h itself succeeded
}

// A scratch real-mode stack for reflected interrupts, allocated on first use so a
// program that never reflects anything does not pay for it.
uint16_t Dpmi::scratch_stack_seg() {
    if (!scratch_ss_ && alloc_dos) scratch_ss_ = alloc_dos(0x20);   // 512 bytes
    return scratch_ss_;
}

bool Dpmi::int31() {
    const uint16_t fn = cpu_.r[AX];
    auto ok   = [&] { cpu_.set_flag(CF, false); };
    auto fail = [&](uint16_t code) { cpu_.r[AX] = code; cpu_.set_flag(CF, true); };

    switch (fn) {
        case 0x0000: {                                   // allocate LDT descriptors
            uint16_t sel = alloc_sel(cpu_.r[CX] ? cpu_.r[CX] : 1);
            if (!sel) { fail(0x8011); break; }
            for (int i = 0; i < (cpu_.r[CX] ? cpu_.r[CX] : 1); ++i)
                set_desc(static_cast<uint16_t>(sel + i * 8), 0, 0, false, false);
            cpu_.r[AX] = sel; ok(); break;
        }
        case 0x0001: ok(); break;                        // free descriptor: we never reuse
        case 0x0003: cpu_.r[AX] = 8; ok(); break;        // selector increment
        case 0x0006: {                                   // get segment base -> CX:DX
            uint32_t b = mem_.r16(desc_addr(cpu_.r[BX]) + 2)
                       | (static_cast<uint32_t>(mem_.r8(desc_addr(cpu_.r[BX]) + 4)) << 16)
                       | (static_cast<uint32_t>(mem_.r8(desc_addr(cpu_.r[BX]) + 7)) << 24);
            cpu_.r[CX] = static_cast<uint16_t>(b >> 16); cpu_.r[DX] = static_cast<uint16_t>(b); ok(); break;
        }
        case 0x0007: {                                   // set segment base = CX:DX
            uint32_t a = desc_addr(cpu_.r[BX]);
            uint32_t b = (static_cast<uint32_t>(cpu_.r[CX]) << 16) | cpu_.r[DX];
            mem_.w16(a + 2, static_cast<uint16_t>(b));
            mem_.w8(a + 4, static_cast<uint8_t>(b >> 16));
            mem_.w8(a + 7, static_cast<uint8_t>(b >> 24));
            ok(); break;
        }
        case 0x0008: {                                   // set segment limit = CX:DX
            uint32_t a = desc_addr(cpu_.r[BX]);
            uint32_t lim = (static_cast<uint32_t>(cpu_.r[CX]) << 16) | cpu_.r[DX];
            uint8_t g = mem_.r8(a + 6) & 0xC0;
            if (lim > 0xFFFFF) { lim >>= 12; g |= 0x80; }
            mem_.w16(a, static_cast<uint16_t>(lim));
            mem_.w8(a + 6, static_cast<uint8_t>(g | ((lim >> 16) & 0x0F)));
            ok(); break;
        }
        case 0x0009: {                                   // set access rights
            uint32_t a = desc_addr(cpu_.r[BX]);
            mem_.w8(a + 5, static_cast<uint8_t>(cpu_.r[CX] & 0xFF));
            mem_.w8(a + 6, static_cast<uint8_t>((mem_.r8(a + 6) & 0x0F) | (cpu_.r[CX] >> 8 & 0xF0)));
            ok(); break;
        }
        case 0x000A: {                                   // create a data alias for a selector
            uint16_t sel = alloc_sel();
            if (!sel) { fail(0x8011); break; }
            uint32_t a = desc_addr(cpu_.r[BX]), b = desc_addr(sel);
            for (int i = 0; i < 8; ++i) mem_.w8(b + i, mem_.r8(a + i));
            mem_.w8(b + 5, static_cast<uint8_t>(mem_.r8(b + 5) & ~0x08u));   // clear "code"
            cpu_.r[AX] = sel; ok(); break;
        }
        case 0x000B: {                                   // get descriptor -> ES:EDI
            uint32_t a = desc_addr(cpu_.r[BX]), d = cpu_.lin(ES, cpu_.gd(DI));
            for (int i = 0; i < 8; ++i) mem_.w8(d + i, mem_.r8(a + i));
            ok(); break;
        }
        case 0x000C: {                                   // set descriptor from ES:EDI
            uint32_t a = desc_addr(cpu_.r[BX]), d = cpu_.lin(ES, cpu_.gd(DI));
            for (int i = 0; i < 8; ++i) mem_.w8(a + i, mem_.r8(d + i));
            ok(); break;
        }
        case 0x0100: {                                   // allocate DOS memory block
            const uint16_t paras = cpu_.r[BX];
            const uint16_t seg = alloc_dos ? alloc_dos(paras) : 0;
            if (!seg) { cpu_.r[BX] = 0; fail(0x0008); break; }   // DOS error 8: out of memory
            const uint16_t sel = alloc_sel();
            set_desc(sel, static_cast<uint32_t>(seg) << 4, paras * 16u - 1, false, false);
            cpu_.r[AX] = seg; cpu_.r[DX] = sel; ok(); break;
        }
        case 0x0101: case 0x0102: ok(); break;           // free/resize DOS block: bump allocator
        // Interrupt and exception handler management. Nothing here raises an exception
        // or delivers a hardware interrupt yet, so these only have to remember what the
        // client installed and give it back. Refusing them is not neutral: DJGPP's crt0
        // reports "DPMI setup failed" and abandons its FPU-emulator setup.
        case 0x0200: {                                   // get real-mode interrupt vector
            const uint32_t v = mem_.r32((cpu_.r[BX] & 0xFF) * 4);
            cpu_.r[CX] = static_cast<uint16_t>(v >> 16); cpu_.r[DX] = static_cast<uint16_t>(v);
            ok(); break;
        }
        case 0x0201:                                     // set real-mode interrupt vector
            mem_.w32((cpu_.r[BX] & 0xFF) * 4,
                     (static_cast<uint32_t>(cpu_.r[CX]) << 16) | cpu_.r[DX]);
            ok(); break;
        case 0x0202: case 0x0204: {                      // get exception / PM interrupt handler
            const int i = (fn == 0x0202) ? (cpu_.r[BX] & 0x1F) : (cpu_.r[BX] & 0xFF);
            const Handler& h = (fn == 0x0202) ? exc_[i] : pmint_[i];
            cpu_.r[CX] = h.sel; cpu_.sd(DX, h.off); ok(); break;
        }
        case 0x0203: case 0x0205: {                      // set exception / PM interrupt handler
            const int i = (fn == 0x0203) ? (cpu_.r[BX] & 0x1F) : (cpu_.r[BX] & 0xFF);
            Handler& h = (fn == 0x0203) ? exc_[i] : pmint_[i];
            h.sel = cpu_.r[CX]; h.off = cpu_.gd(DX); ok(); break;
        }
        case 0x0900: case 0x0901:                        // disable/enable virtual interrupts
            cpu_.sb(AX, cpu_.get_flag(IF) ? 1 : 0);      // AL = previous state
            cpu_.set_flag(IF, fn == 0x0901); ok(); break;
        case 0x0902: cpu_.sb(AX, cpu_.get_flag(IF) ? 1 : 0); ok(); break;   // get state

        case 0x0300: simulate_real_int(); break;         // simulate real-mode interrupt
        case 0x0400:                                     // get DPMI version
            cpu_.r[AX] = 0x005A;                         // 0.90
            cpu_.r[BX] = 0x0005;                         // no V86, no virtual memory
            cpu_.sb(2 /*CL*/, 3);                        // 80386
            cpu_.r[DX] = 0xFFFF;                         // master/slave PIC base
            ok(); break;
        case 0x0500: {                                   // free memory information
            uint32_t d = cpu_.lin(ES, cpu_.gd(DI));
            for (int i = 0; i < 0x30; ++i) mem_.w8(d + i, 0xFF);
            const uint32_t free = Memory::kSize - heap_next_;
            mem_.w32(d + 0x00, free);                    // largest free block
            mem_.w32(d + 0x04, free >> 12);              // in pages
            ok(); break;
        }
        case 0x0501: {                                   // allocate memory block
            const uint32_t len = (static_cast<uint32_t>(cpu_.r[BX]) << 16) | cpu_.r[CX];
            const uint32_t base = heap_next_;
            if (static_cast<uint64_t>(base) + len > Memory::kSize) { fail(0x8013); break; }
            heap_next_ = (base + len + 0xFFF) & ~0xFFFu;
            cpu_.r[BX] = static_cast<uint16_t>(base >> 16); cpu_.r[CX] = static_cast<uint16_t>(base);
            cpu_.r[SI] = static_cast<uint16_t>(base >> 16); cpu_.r[DI] = static_cast<uint16_t>(base);
            ok(); break;                                 // handle == address, we never free
        }
        case 0x0502: ok(); break;                        // free memory block: bump allocator
        case 0x0600: case 0x0601: case 0x0602: case 0x0603:
            ok(); break;                                 // lock/unlock: no paging here
        case 0x0604: cpu_.r[BX] = 0; cpu_.r[CX] = 0x1000; ok(); break;   // page size
        default:
            fail(0x8001);                                // unsupported function
            break;
    }

    if (trace)
        printf("[dpmi] int31 AX=%04X BX=%04X CX=%04X DX=%04X -> %s AX=%04X\n",
               fn, cpu_.r[BX], cpu_.r[CX], cpu_.r[DX],
               cpu_.get_flag(CF) ? "FAIL" : "ok", cpu_.r[AX]);
    return true;
}

}  // namespace dosemu
