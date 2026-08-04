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
6. …and then **exits 0 after five INT 21h calls, without loading the image**

No output, no `.OBJ`. `CS=002B` at the end, which is a GDT selector of its own making.
`DOSEMU_DOS_TRACE=1` shows the whole conversation; that is where to pick this up.

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
