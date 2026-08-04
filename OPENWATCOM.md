# OpenWatcom — where it stands

The third toolchain. LSI C-86 (16-bit) and DJGPP (32-bit) both compile and run inside
the emulator; Watcom does not yet, and this is the precise state of it.

## Getting the toolchain

`ow/` is gitignored. It is Watcom 11.0c's DOS-hosted tools, assembled from the small
component zips at
<https://github.com/open-watcom/open-watcom-1.9/releases/tag/w11.0c-zips>:

    core_binw  core_all  c_binw  cpp_binw  clib_hdr  clib_d32  cpplib_hdr  cpplib_o32

about 5 MB in total, unzipped into one tree (`binw/`, `h/`, `lib386/`). That is far
cheaper than the OpenWatcom v2 installers, which are 128 MB each *and* are themselves
DOS-extended programs, so unpacking one would need the thing we are trying to build.

## The shape of the problem

`binw/wcc386.exe` is **MZ + LX** — and its real-mode stub is not DOS/4GW. The strings
in it are `DPMI error modifying selectors` and `Error allocating protect-mode
selectors`, and it looks for `W32RUN.EXE` on `PATH` (that one is in `core_all.zip`).
So the shape is the same as DJGPP's: a real-mode stub, a DPMI client, a 32-bit image
the client loads itself. No LE/LX loader of our own was needed to start making
progress.

## How far it gets

    ./dosemu --root <ow-root> <ow-root>/binw/wcc386.exe hello.c

1. the stub finds `W32RUN.EXE` on `PATH` and `EXEC`s it
2. W32RUN resizes its memory block (asking 0xFFFF paragraphs first, to learn the max)
3. it opens the A20 gate
4. it switches to protected mode — under **its own GDT**, after an `LGDT`, not through
   our DPMI host
5. it reads 0xAD0 bytes of the LX header out of `wcc386.exe`
6. it loads **its own** 32-bit part (seek to 0x451, read 0x4410 bytes) and jumps to it
7. that 32-bit code runs about **1,700 instructions**, compares some strings, finds no
   match, returns 0 and exits — without ever opening `wcc386.exe`

No output, no `.OBJ`.

**How the target is communicated — found.** The stub writes an environment variable:

    PATH=... COMSPEC=... DJGPP=... WATCOM=A:\ INCLUDE=A:\H $=A:\BINW\WCC386.EXE

a variable literally named `$`, holding the path of the program to load. Nothing on the
command line or in the PSP carries it — the disassembly at `0110:0686` shows the `EXEC`
parameter block holds only `env=0`, the stub's own PSP command tail and its two FCBs.

**W32RUN loads itself, not the target.** Measured: after opening a file it always seeks
to `0x451` and reads `0x4410` bytes — hard-coded offsets that belong to *its own*
executable. Feeding it the parent's path (an experiment worth doing, since the trailing
program name is the other thing it reads) made it read `wcc386.exe` at W32RUN's offsets
and jump into the middle of data, which is how BOUND and the `0x82` alias turned up as
"unimplemented instructions". So the trailing name must be the **child's own** path, and
the `$` variable is the target.

Both of those now hold at once, which took some care: the stub appends `$=…` over the
end-of-strings NUL, taking the count word and program name with it, so a faithful copy
of the parent's block has `$` but no name, and a rebuilt block has a name but no `$`.
`make_child_env()` copies the strings and *regenerates* the name, keeping both.

**Where it stops now, traced instruction by instruction.** W32RUN loads its own 32-bit
part, switches to protected mode, and the 32-bit code runs about 230 instructions before
exiting 0. The last 30 of them are unambiguous (`DOSEMU_TRACE=10196-10290`):

    002B:0894  FF ...        -> 002B:0890  C3   ret
    002B:089A  FF ...        -> 002B:0890  C3   ret
    002B:08AC  FF ...        -> 002B:0890  C3   ret
    002B:08B2  FF ...        -> 002B:0890  C3   ret
    002B:08BA  E9  -> 01E9   push/pop, mov ah,4Ch, int 21h

Four indirect calls in a row, every one of them through a pointer whose value is
`0x890`, and `0x890` is a bare `ret`. Then it exits. Those are the emulator's own
execution records, so unlike a guess at the file layout they are reliable.

That reads like a table of initialisation or service routines in which **every slot
holds the same do-nothing address** — either because it was never populated, or because
the population depends on something we answered as "nothing available". Finding what
fills that table is the next step, and it is the last unknown: the target path is
delivered (`$`), the extender loads and enters protected mode, and it is executing real
code when it gives up.

Two practical notes for whoever picks this up:

- **Do not disassemble W32RUN at `0x451 + EIP`.** `0x451` is the *file* offset of the
  32-bit part; the image is loaded elsewhere and the EIPs are offsets into the loaded
  image. Decoding at the file offset produces plausible-looking nonsense (it even yields
  an `int 21h` near the right place, which is exactly how such a mistake survives). Get
  the load address from the destination of the `AH=3Fh` read that pulls in those 0x4410
  bytes, then disassemble relative to that.
- The calls arrive from `CS=002B` with `SS=0033`, `d=1` — a 32-bit stack, so the
  descriptors W32RUN built for itself are being read correctly.

## Fixed on the way here

All four were found with `DOSEMU_DOS_TRACE=1`, and all four are the same shape as every
other bug on this project — a well-formed answer that was not true.

- **`AH=4Ah` (resize memory block) said yes to everything.** The standard way to ask
  "how much can I have?" is to request 0xFFFF paragraphs and read the real maximum out
  of BX when it *fails*. Granting a 1 MiB block left W32RUN convinced it owned memory
  that was not there: `Fatal error allocating DOS memory`.
- **The interrupt vector table pointed at nothing.** The emulator services `INT`
  through a callback and never reads the IVT, so it had been left as zeros — fine until
  a program *reads* a vector and calls it. W32RUN takes INT 21h with `AH=35h` and
  far-calls what it gets; it got `0000:0000`, jumped to address zero and marched
  through low memory until it wandered into the DPMI entry point, which duly "switched
  to protected mode" on a stack frame that was not a return address. Every vector now
  points at a four-byte stub at `0050:n*4` holding `INT n; IRET`.
- **The A20 gate did not exist.** `IN` returned 0 and `OUT` did nothing, so the gate
  could never open: `Cannot enable the A20 line, XMS memory manager required`. Both
  historical routes work now — port 0x92 bit 1, and the keyboard controller's
  `OUT 64h,D1h` / `OUT 60h,<bit 1>` — and `lin()` applies the 1 MiB real-mode wrap only
  while the gate is **closed**, which is what the gate has always meant. Guests that
  never touch it see exactly the old behaviour.
- **`AH=63h` (DBCS lead-byte table) was refused.** An empty table is the truthful
  answer on a non-Japanese DOS, and it is an *answer* rather than an error, which is
  what the stub wants before it will start.
- **`INT 21h` left the carry flag wherever the previous call had put it.** The handlers
  only ever *set* CF on failure, so a call that succeeded silently inherited a stale
  flag. Watcom's loader installs its interrupt handlers with `AH=25h` and checks CF
  afterwards; it read the leftover and concluded the install had failed. `int21()` now
  clears CF on entry, which is what DOS does, and failures say so explicitly.
- **There was exactly one environment block**, at a fixed segment, rebuilt for whoever
  loaded last. Fine while one program runs at a time and nobody looks back — but
  `EXEC`ing a helper *overwrote the parent's own program name* with the helper's, which
  is precisely the thing a loader stub would want to read. Every program now gets its
  own block: `make_child_env()` copies the parent's strings and appends the child's
  path, which is what DOS does and where a C runtime finds `argv[0]`. This was a latent
  bug for DJGPP too — `gcc`'s environment was being clobbered the moment it spawned
  `cc1`.
- **The PSP was missing the fields that describe *context* rather than arguments**:
  `02h` (segment past the end of this program's memory, the same ceiling `AH=4Ah` now
  reports), `16h` (**the parent's PSP** — a loader stub that has to find the program
  that launched it walks this, and zero sent it to segment 0), and `50h` (the classic
  `INT 21h; RETF` call gate). `Dos::init_psp()` fills them, because the loader knows
  neither who the parent is nor where the arena ends.

Also: `PATH` gained `A:\BINW`, and the environment gained `WATCOM=A:\` and
`INCLUDE=A:\H`.

## What to try next

- Find out why W32RUN gives up after reading the LX header. It exits 0, so it thinks
  it did the right thing — the likeliest cause is that something it queried came back
  in a shape it read as "nothing to do".
- If W32RUN turns out to want more of the machine than is worth emulating, the fallback
  is the original plan: write the LE/LX loader ourselves (`src/coff_loader.cpp` is the
  model) and run the 32-bit image directly under our own DPMI host, the way go32's COFF
  is loaded — except we would be doing the loading instead of the extender.
- The 16-bit `wcc.exe` is the same LX shape, so there is no easier target among the
  Watcom tools.
- `INS`/`OUTS` (`6Ch`-`6Fh`) are still unimplemented. They only turned up while the
  loader was executing data, so they are not blocking anything — but they are real
  80186 instructions and the CPU should have them.
