# OpenWatcom — where it stands

The third toolchain. **The compilers work**: `wcc386` (32-bit C), `wcc` (16-bit C) and
`wpp386` (C++) all run inside the emulator and write a `.obj`. Linking does not, and the
one thing missing is named precisely at the bottom.

    sh get_fixtures.sh ow
    ./dosemu --root ow ow/binw/wcc386.exe hello.c     # -> hello.obj, "Code size: 44"
    ./dosemu --root ow ow/binw/wcc.exe    hello.c     # the 16-bit compiler
    ./dosemu --root ow ow/binw/wpp386.exe hello.cpp   # the C++ one

    Watcom C32 Optimizing Compiler  Version 11.0c
    Copyright by Sybase, Inc., and its subsidiaries, 1984, 2002.
    hello.c: 10 lines, included 477, 0 warnings, 0 errors
    Code size: 44

Note that 11.0c predates the standard library headers: `<stdio.h>`, not `<cstdio>`, and
no `std::`. That is the compiler being old, not the emulator being wrong.

## Getting the toolchain

`ow/` is gitignored; `sh get_fixtures.sh ow` unpacks it from the archives in `upstream/`,
which are the Watcom 11.0c component zips exactly as downloaded (about 8 MB) plus
DOS/4GW. `upstream/README.md` lists every file, its checksum and its licence, and
explains why DOS/4GW had to come from somewhere else.

## The shape of the problem

`binw/wcc386.exe` is **MZ + LX**, and its real-mode stub is not DOS/4GW. The strings in
it are `DPMI error modifying selectors` and `Error allocating protect-mode selectors`,
and it looks for `W32RUN.EXE` on `PATH` — FlashTek's X-32 extender. So the shape is the
same as DJGPP's: a real-mode stub, an extender, a 32-bit image the extender loads. No
LE/LX loader of our own was needed.

`wlink.exe` is stubbed for **DOS/4GW** instead, which is why the linker is a separate
problem from the compilers.

## What it took to run the compilers

Everything below is in the two commits that landed them. They are listed here because
the pattern is the point: with one exception, every defect was an *answer* that was
well-formed and untrue, and the honest fix was smaller than the workaround would have
been.

### Memory had to stop being flat

X-32 does not ask for memory, it takes it: it enters protected mode under its own GDT
and puts its entire 32-bit world at linear **256 MiB**. The 64 MiB flat array folded
every one of those addresses back through the size mask, so the extender wrote its
environment copy at one address and read it from another — and read-back was
self-consistent for its *code*, which is why this hid for so long. `Memory` is now a page
table over the whole 32-bit space: 64 KiB pages allocated zeroed on first write, and an
untouched page reads as zero for nothing. Startup no longer zeroes tens of megabytes
either, which the browser build was paying for on every run.

`DOSEMU_MEMCHK=1` said this outright the first time it was pointed at Watcom
(`w32 past end of RAM: 10000000`). It had been built for exactly this class of bug three
weeks earlier and never aimed at this guest.

### A protected-mode guest that loads its own IDT means it

We serviced every `INT` as DOS regardless of mode, because that is what DPMI clients
want. X-32 is not a DPMI client — **it never issues a single INT 31h** — and reflects DOS
calls through its own handler under its own IDT. The giveaway is in its code:

    0174  B8 09 35     mov ax, 3509h        ; get interrupt vector 9
    0177  8B DC        mov ebx, esp
    0179  ...
    017F  CD 21        int 21h
    018D  8B E3        mov esp, ebx

`mov ebx,esp` / `int 21h` / `mov esp,ebx` only makes sense if what runs in between is
*yours*. Serviced as DOS, `AH=35h` returned the vector's offset in BX — and `mov esp,ebx`
made it the stack pointer. That is how a 32-bit program came to be running on a stack at
`0x24`, and why its environment scan found two NULs and quit.

`INT` now goes through the guest's IDT gate when there is one. A vector the guest left
un-hooked still falls through to DOS, so DPMI clients — which do not own the IDT, the
host reflects for them — are untouched.

### Privilege changes are real

X-32 runs the application at **CPL 3** (selectors `002B`/`0033`, RPL 3) and its handler at
**CPL 0** (`0008`). So the gate switches to the ring-0 stack out of the TSS and stacks the
old SS:ESP below the usual three words, and the handler's closing far return pops them
back. Both halves are implemented; `far_ret()` covers RETF, RETF imm and IRET.

Without the switch the handler's frame stayed on the application's stack, and the routine
that drops to real mode reloads SS from a fixed selector and expects the frame to still be
there. It wasn't, so it returned to an address read out of the wrong segment.

### POP computes its address after the pop

When ESP is the base register, `pop dword [esp+8]` addresses off the **incremented** ESP.
We decoded the ModRM first, so the address came from the old one. X-32's handler rewrites
the interrupt frame it was entered on with exactly that instruction pair:

    096E  67 FF 74 24 08     push word [esp+8]     ; the flags
    0972  9D                 popf
    0975  67 66 FF 74 24 04  push dword [esp+4]    ; CS
    097B  67 66 8F 44 24 08  pop  dword [esp+8]
    0981  67 66 8F 04 24     pop  dword [esp]
    ...
    0F19  66 CB              retf                  ; EIP, CS, ESP, SS

One slot of error put CS where EIP should be, and the far return went to
`CS=<flags>:EIP=<CS>` and ran 20 million instructions of `add [bx+si],al` through zeroed
memory. This is the one defect on the list that is a plain misreading of the manual
rather than a lie told to a guest.

### An environment block belongs below the program

We allocated the child's block just past its arena. It read back correctly and was still
wrong: DOS builds the environment *first*, so it sits outside the memory the child owns.
The moment W32RUN asked `AH=4Ah` to grow into everything it had been told it owned —
0x6F00 paragraphs, immediately — it zeroed its own environment and could no longer find
the `$=` variable naming the program to load. It looked like our environment block was
empty; it was overwritten by its reader.

(The `$` variable is how the target is communicated at all: the stub writes
`$=A:\BINW\WCC386.EXE` into the environment, appending it over the end-of-strings NUL and
taking the count word and trailing program name with it. `make_child_env()` copies the
strings and *regenerates* the name, so both survive — W32RUN needs the name to find
itself and the variable to find its target.)

### Conventional memory had to stop being a bump pointer

A bump pointer serves a program that starts, mallocs and exits. It cannot serve a DOS
extender, because the way one sizes itself is to ask for *everything* and then release
what it does not need. Worse, the rule that kept it from handing out memory a program
owned ("if the block is above the mark, move the mark past it") destroyed the whole arena
the moment a stub relocated itself high and resized there — wlink's does, and every later
allocation failed with "insufficient memory" while 600 KiB sat unused.

It is a block list now: address order, first fit, `AH=49h` frees, `AH=4Ah` shrinks and
grows in place. Two details cost an hour each and are commented in the source. Only
**free** neighbours coalesce — merging two used blocks loses the boundary `AH=49h` needs.
And a grow **extends** the block rather than marking the range used; marking left the new
paragraphs as a second used block butted against the first, so the next grow found its
own tail in the way. LSI C's driver creeps its block up one paragraph at a time and got
`out of memory` after exactly two.

### Smaller things, same shape

- **`AH=4Ah` said yes to everything.** The standard way to ask "how much can I have?" is
  to request 0xFFFF paragraphs and read the real maximum out of BX when it *fails*.
  Granting a 1 MiB block left W32RUN convinced it owned memory that was not there:
  `Fatal error allocating DOS memory`.
- **The interrupt vector table pointed at nothing.** The emulator services `INT` through
  a callback and never read the IVT, so it had been left as zeros — fine until a program
  *reads* a vector and calls it. W32RUN takes INT 21h with `AH=35h` and far-calls what it
  gets; it got `0000:0000`, jumped to address zero and marched through low memory until
  it wandered into the DPMI entry point, which duly "switched to protected mode" on a
  stack frame that was not a return address.
- **...but only up to 0x7F.** Above that the table is where drivers and extenders put
  *themselves*, and they find room by scanning for entries not in use. DOS/4GW walks
  0x2C8-0x2FA looking for a repeated value; with all 256 stubbed every entry was distinct
  and it scanned for ever. Zero is what DOS leaves there.
- **The A20 gate did not exist.** `IN` returned 0 and `OUT` did nothing, so the gate could
  never open: `Cannot enable the A20 line, XMS memory manager required`. Both historical
  routes work now — port 0x92 bit 1, and `OUT 64h,D1h` / `OUT 60h,<bit 1>` — and `lin()`
  applies the 1 MiB real-mode wrap only while the gate is **closed**, which is what the
  gate has always meant.
- **`INT 21h` left the carry flag wherever the previous call had put it.** The handlers
  only ever *set* CF on failure, so a call that succeeded silently inherited a stale flag.
  Watcom's loader installs its handlers with `AH=25h` and checks CF afterwards.
- **The PSP was missing the fields that describe context rather than arguments**: `02h`
  (segment past the end of this program's memory), `16h` (**the parent's PSP** — a loader
  stub walks this, and zero sent it to segment 0), and `50h` (the `INT 21h; RETF` gate).
- **`AH=63h`** (DBCS lead-byte table) was refused; an empty table is the truthful answer
  on a non-Japanese DOS, and it is an *answer*. **`AH=0Dh`** (reset drive) likewise.
- **`SMSW`/`LMSW` computed a memory address for a register operand**, indexing `sbase[]`
  out of bounds. A segfault waiting for the first guest to use the register form, which
  DOS/4GW is.

## The linker: exactly one thing missing

`wlink.exe` is stubbed for DOS/4GW, so `dos4gw.exe` has to be on `PATH`
(`get_fixtures.sh ow` puts it there). With it present the stub now gets a long way:

1. it finds `dos4gw.exe` and loads it with **`AH=4Bh AL=03`, load overlay** — the image
   goes where the caller says, relocated by the factor it supplies, with no PSP and no
   transfer of control, because the caller has already sized its own block and will
   far-call in itself. "Load and execute" is not a substitute: a second PSP is precisely
   what the stub does not want. This is implemented.
2. DOS/4GW starts, finds our DPMI host via `INT 2Fh AX=1687h`
3. it performs the standard memory dance — allocate one paragraph, grow it to 0xFFFF and
   read the real maximum out of BX, shrink back, repeat — which the block allocator now
   answers correctly, step for step
4. it calls **`AH=52h`** for the DOS list-of-lists
5. and then asks to grow its own program block past a block it allocated itself, which
   cannot work with the blocks where we put them, and gives up: `Not enough memory on
   exec`

**The missing thing is a walkable MCB chain.** Step 4 is the tell: having got the
list-of-lists, DOS/4GW walks the memory-control-block chain out of it and plans its
layout from what it sees. Ours has no chain — `AH=52h` returns a well-formed structure
with an empty SFT list and nothing else — so it plans against a memory map that is not
the one it will get.

That is a well-shaped next piece rather than a mystery, and the block list is most of it
already: one MCB per block, `'M'`/`'Z'` signature, owner PSP and size, written at
`seg-1` in guest memory, with the first-MCB pointer at `LoL-2`. Two things to get right:
`mem_alloc` must reserve the extra paragraph each MCB occupies (a block of N paragraphs
costs N+1), and the top-level program's MCB at `00FF` collides with the end of the
environment block at `00F0`, so that moves down. Note also that a *lone* fake MCB was
tried early on, for the environment block alone, and broke FreeCOM completely — a
partial chain is worse than none, so this is all-or-nothing.

The alternative, if the chain turns out to be a bad trade, is to stop using `wlink` and
write the OMF linker ourselves; `src/coff_loader.cpp` is the model for the file parsing,
and the Watcom `.obj` we already produce is the input. That is more work but it is *our*
work, with no third-party extender in the way.

## Also worth knowing

- **`w32run.exe` and `x32run.exe` are the same program** — the copyright string is
  "DOS extender Copyright 1991-1994 by Doug Huffman".
- **W32RUN loads itself, not the target.** After opening a file it always seeks to
  `0x451` and reads `0x4410` bytes: hard-coded offsets belonging to *its own*
  executable. Feeding it the parent's path made it read `wcc386.exe` at W32RUN's offsets
  and jump into the middle of data, which is how `BOUND` and the `0x82` alias turned up
  as "unimplemented instructions".
- **`INS`/`OUTS` (`6Ch`-`6Fh`) are still unimplemented.** They only ever turned up while
  a loader was executing data, so they block nothing — but they are real 80186
  instructions and the CPU should have them.
- The diagnostics that did the work here, in the order they earned their keep:
  `DOSEMU_DOS_TRACE=1` (every INT 21h with its path, command tail and ES),
  `DOSEMU_TRACE=lo-hi` (instruction bytes *and* segment bases — disassembling a file at
  `header + EIP` is a reliable way to produce plausible nonsense), `DOSEMU_MEMCHK=1`,
  `DOSEMU_WATCH=lo-hi`, `DOSEMU_SAMPLE=N` (which distinguishes "slow" from "looping" in
  one run), `DOSEMU_DPMI_TRACE=1`.
