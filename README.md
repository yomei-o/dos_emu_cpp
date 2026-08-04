# dos_emu_cpp

A small user-mode **MS-DOS** emulator in C++17, in the same spirit as
[x86_emu_cpp](https://github.com/yomei-o/x86_emu_cpp): it interprets 16-bit
real-mode 8086 machine code instruction by instruction and supplies the operating
system itself — DOS services (`INT 21h`) and the BIOS interrupts a program uses —
so a real DOS `.EXE` / `.COM` runs with no DOS underneath.

The goal is to run the **LSI C-86** compiler (a classic 16-bit DOS C compiler)
inside the emulator: feed it a `.c` file, let its passes (`LCC → CPP → CF → CG86 →
R86 → LLD`) compile it to a DOS `.EXE`, and then run that `.EXE` — all in a browser
tab via WebAssembly. `lsic330c.lzh` in this repo is the LSI C-86 3.30 trial
distribution the demo drives.

That goal is met, and the emulator has since grown a 32-bit half: an 80386 core,
protected mode, and a DPMI host of its own, which is enough to run **DJGPP** — so the
same page can hand a `.c` or a `.cpp` to a real `gcc`/`g++` and run the 32-bit
executable that comes out. The DJGPP archives are in `upstream/`, unmodified, with
`web/DJGPP-LICENSE.md` recording their GPL terms and source URLs.

A third toolchain, **Watcom C/C++ 11.0c**, now goes the whole way: its compilers run under
FlashTek's X-32 extender, which wanted rather more of a 386 than DJGPP does (its own GDT
and IDT, the application at ring 3, a TSS stack switch on every DOS call), and `wlink` runs
under DOS/4GW as a client of the built-in DPMI host. So `wcc` + `wlink` — or `wcl` in one
command — builds a DOS `.EXE` that then runs. [`OPENWATCOM.md`](OPENWATCOM.md) is the full
account.

## How it works (same shape as the sibling project)

```
  .EXE / .COM ──► loader ──► real-mode memory ──► interpreter ──► INT dispatch
   MZ / COM       PSP,       1 MiB, seg:off        cpu.cpp          DOS INT 21h
                  relocs                                            BIOS INT 10h/16h
```

- **CPU** (`src/cpu.cpp`): an 8086/80386 interpreter — segmented addressing
  (`physical = segment*16 + offset`) and, in protected mode, descriptor bases; the
  ALU/`MOV`/stack/`Jcc`/string groups, `ModRM` 16- and 32-bit addressing, `INT n`, the
  x87 (`src/fpu.cpp`), and the whole one-byte integer map down to the BCD adjusts.
- **DOS** (`src/dos.cpp`): the `INT 21h` service layer — console and file I/O,
  memory allocation, the PSP and command line, and **program load & execute**
  (`AH=4Bh`), which is what lets the compiler driver spawn its passes.
- **Loader** (`src/loader.cpp`): `.COM` (flat, loaded at `PSP:0x100`) and `.EXE`
  (MZ header, relocation table, initial `CS:IP`/`SS:SP`).

Anything unimplemented stops with a message naming the opcode or `INT` function and
the address, so bringing up a new guest is a matter of following the messages —
the same method used for the sibling emulator.

## 🕹 Live demos

**[16-bit — FreeDOS + LSI C-86](https://yomei-o.github.io/dos_emu_cpp/)**
A FreeDOS `COMMAND.COM` prompt. Edit C, press **Compile & Run**: LSI C-86 compiles it
(spawning its passes as DOS programs) into a DOS `.EXE`, which then runs.

**[32-bit — DJGPP gcc / g++](https://yomei-o.github.io/dos_emu_cpp/web/djgpp.html)**
The same emulator with its built-in **DPMI host**, running DJGPP's real gcc 3.4.6.
Compile C *or* C++ to a 32-bit protected-mode `.EXE` and run it — 2.0 s for C, 3.5 s
for C++ in wasm. The page drives the four passes itself (`cc1` → `as` → `ld` →
`stubify`) and will show you the assembly gcc produced.

**[Watcom C/C++ 11.0c — compile *and* link](https://yomei-o.github.io/dos_emu_cpp/web/watcom.html)**
The real Watcom compiler and linker, in the page. `wcc`/`wpp` compiles C or C++ to an
`.obj`, `wlink` links it into a DOS `.EXE`, and the emulator runs that — about a second for
either language, from a 1.7 MB download. Three DOS programs deep, and each leans on a
different part of the emulator: the compilers run under **FlashTek X-32** (not a DPMI client
at all — its own IDT, ring 3), the linker under **DOS/4GW** (a DPMI client, reflecting its
DOS calls through `INT 31h 0302h`), and what they build is plain 16-bit real mode. The page
will also show you the link map.

All three are static pages; nothing leaves the browser.

## Status

1. ✅ 16-bit CPU core + `.COM`/`.EXE` loader + `INT 21h` (console, exit)
2. ✅ DOS file I/O, memory, PSP/environment → a single LSI C pass (CPP) runs
3. ✅ `INT 21h AH=4Bh` EXEC (child processes) → the full `LCC` pipeline
4. ✅ compile a `.c` to `.EXE` and run it (`hello from LSI C!`)
5. ✅ WebAssembly build + browser demo (edit a `.c`, compile & run in the page)
6. ✅ 80386 core + protected mode + a built-in **DPMI host** → DOS-extended programs run
7. ✅ **DJGPP gcc and g++** compile C and C++ to 32-bit `.EXE`s, in the browser too
8. ✅ **Watcom C/C++ 11.0c** — `wcc`, `wcc386`, `wpp` and `wpp386` compile C and C++
   under FlashTek X-32, which the emulator runs (its own GDT/IDT, CPL 3, a TSS stack
   switch), and **`wlink` links**, running under DOS/4GW as a DPMI client — 16-bit
   (`system dos`) and 32-bit protected-mode (`system dos4g`) output, both of which run.
   `wcl hello.c` does compile and link in one command.
   [`OPENWATCOM.md`](OPENWATCOM.md) has the whole account, including the emulator bugs
   the exercise turned up: an interrupt frame unwound over a transfer of control, a `CF`
   result thrown away by the very `IRET` that returned it, `LODSB` clobbering `AH`, and
   `AH=40h` with `CX=0` not truncating — which is the only way DOS can shorten a file.

## Building

```sh
sh build.sh          # or: g++ -std=c++17 -O2 -Isrc -o dosemu src/*.cpp
./dosemu PROG.EXE [args...]
```

## License

Own code MIT. `lsic330c.lzh` is the LSI C-86 trial package, redistributed under its
own terms (see the archive's READ.ME / LSIC86.MAN).
