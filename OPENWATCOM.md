# OpenWatcom — where it stands

The third toolchain. **The compilers work**: `wcc386` (32-bit C), `wcc` (16-bit C) and
`wpp386` (C++) all run inside the emulator and write a `.obj`. Linking does not; the
linker section below says how far DOS/4GW gets and why the last step is a bigger piece of
work than everything before it.

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

## The linker

`wlink.exe` is stubbed for DOS/4GW, so `dos4gw.exe` has to be on `PATH`
(`get_fixtures.sh ow` puts it there). With it present the stub gets a long way:

1. it finds `dos4gw.exe` and loads it with **`AH=4Bh AL=03`, load overlay** — the image
   goes where the caller says, relocated by the factor it supplies, with no PSP and no
   transfer of control, because the caller has already sized its own block and will
   far-call in itself. "Load and execute" is not a substitute: a second PSP is precisely
   what the stub does not want. This is implemented.
2. DOS/4GW starts, finds our DPMI host via `INT 2Fh AX=1687h`
3. it performs the standard memory dance — allocate one paragraph, grow it to 0xFFFF and
   read the real maximum out of BX, shrink back, repeat — which the block allocator now
   answers correctly, step for step
4. it calls **`AH=52h`** for the DOS list-of-lists and walks the MCB chain out of it to
   plan its layout — which is what the chain below was needed for

**That chain is in.** One MCB per block in the paragraph below the segment the guest is
given (so a block of N paragraphs costs N+1), `'M'`/`'Z'` signature, owner PSP and size,
republished after every change, with the first MCB's segment at `LoL-2`.

It cost one thing on the way: FreeCOM stopped finding its own NLS strings and started
asking where `COMMAND.COM` was. The environment block was built by the loader at a fixed
low segment, *outside* the arena, so it had no MCB — and a shell that trusts MCBs
validates the block it is handed against one. That is the third time FreeCOM has caught a
half-truth about memory (the first was a lone fake MCB for the environment block alone,
which left it answering "Bad command or filename" to everything), and the lesson was the
same each time: a partial chain is worse than none. Fixed at the root — the environment
is allocated out of the arena like every other block, so `load_program()` no longer
chooses where it goes.

### What the chain unlocked, and where DOS/4GW stands now

With memory described honestly, DOS/4GW stops failing the memory dance and starts
behaving like the DPMI client it is. Following it from there cost four DPMI functions,
one 8086 instruction and one internal inconsistency:

- **`0303h`/`0304h`, real-mode callbacks.** A real-mode address that runs a
  protected-mode procedure of the client's with the real-mode machine state laid out in
  a structure it nominated. **This was also why every DJGPP program opened with
  `Coprocessor not present and DPMI setup failed!`** — 0303h is how DJGPP hooks the FPU
  emulator. That warning is gone, which is the best thing to come out of the exercise.
- **`0305h`, save-state addresses.** Size zero, meaning "you need not call these", which
  is true here rather than a shortcut: a mode switch preserves the whole CPU.
- **`0306h`, raw mode-switch addresses.** Entered by a far JMP with the whole new machine
  state in registers.
- **`XLAT`.** An 8086 instruction no guest here had used, and DOS/4GW needs it *before*
  it can print its own error message — so the first symptom was an unimplemented opcode
  standing exactly where the explanation should have been.
- **Descriptor reads and writes disagreed about which table.** `desc_addr()` was LDT-only
  while `read_desc()` honoured bit 2. DOS/4GW does selector arithmetic on the PSP
  selector and comes back with `0018` instead of `001C`; with no GDT loaded it then read
  "descriptors" out of the interrupt vector table and ran with whatever base the vector
  for INT 6 spelled. With no GDT there is nothing a GDT selector can mean, so it now
  means what the only table we have says.

### Protected-mode interrupts have to reach the handler that hooked them

The apparent wall after that — DOS calls arriving with "selector base 0, offset 0" and a
file read landing on the interrupt vector table — was not a limit of what a host can do.
It was us. We stored what a client registered with `0205h` and never dispatched to it.
DOS/4GW hooks **both INT 21h and INT 31h** precisely so its own handler can translate a
call before passing it down; skipping that handed the DOS layer a flat 32-bit pointer and
let it read 21 KiB of a file over low memory. Same shape as the guest-owns-the-IDT case
above: a client that hooks a vector means it.

Three details had to be right before it worked at all, and each was worth an hour:

- **The frame width comes from the client, not from the handler's code segment.** DOS/4GW
  registers handlers in the 16-bit alias selector the mode switch handed it and unwinds
  them with `IRETD`. Reading the width off the selector pushed three words and had six
  popped, straight into a not-present selector.
- **`0204h` has to name a real default handler.** A client reads all 256 vectors at
  startup and saves them to chain to; `0:0` for an un-hooked one is a jump to nowhere.
  There are 256 one-byte stubs in the host's entry paragraph now, and reaching one
  services the interrupt and unwinds the frame itself.
- **The default handler must not dispatch back to the client.** Chaining is a request for
  the *host's* behaviour; handing the call back to the handler that just chained is a loop
  with no exit, and it presented as the emulator running at 2,500 instructions a second.

And `INT 16h AH=01h` stopped answering "yes, Enter is waiting" unconditionally. A caller
that believes that issues the blocking read; DOS/4GW polls the keyboard during startup and
stopped dead with no output at all, which is the hardest kind of hang to find because a
process asleep in a read looks exactly like one wedged in a loop.

### Where the linker actually stands

DOS/4GW gets through its own startup, walks the MCB chain and releases what is not its,
grows its block to all of memory, hooks INT 21h and INT 31h, forwards INT 31h to us
correctly, switches to 32-bit protected mode, runs about 80,000 instructions of 32-bit
code — and then calls `AH=4Ch` with status 8, having printed nothing at all. **No DPMI
function fails any more** except `0A00h`, which is a probe every client makes.

So it is no longer a list of missing pieces; it is one silent decision inside a 259 KiB
extender, and finding it means following that 80,000 instructions. That is a real
possibility now in a way it was not before, but it is open-ended.

The alternative has not changed and is looking better in comparison: stop using `wlink` and
write the OMF linker ourselves. `src/coff_loader.cpp` is the model for the file parsing,
and the `.obj` the compilers already produce is the input. More work, but *our* work, with
no third-party extender in the way and nothing left to reverse-engineer.

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
- **A DPMI client that issues `INT 21h` directly** gets its DS/ES selectors aliased to the
  paragraph they cover, for the duration of the call, when the base is in conventional
  memory. That is what a real host does and the only case one can do anything for; it is
  also where DOS/4GW runs out of road, because it passes 32-bit offsets.
- The diagnostics that did the work here, in the order they earned their keep:
  `DOSEMU_DOS_TRACE=1` (every INT 21h with its path, command tail and ES),
  `DOSEMU_TRACE=lo-hi` (instruction bytes *and* segment bases — disassembling a file at
  `header + EIP` is a reliable way to produce plausible nonsense), `DOSEMU_MEMCHK=1`,
  `DOSEMU_WATCH=lo-hi`, `DOSEMU_SAMPLE=N` (which distinguishes "slow" from "looping" in
  one run), `DOSEMU_DPMI_TRACE=1`.
