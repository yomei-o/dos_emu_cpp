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

## How it works (same shape as the sibling project)

```
  .EXE / .COM ──► loader ──► real-mode memory ──► interpreter ──► INT dispatch
   MZ / COM       PSP,       1 MiB, seg:off        cpu.cpp          DOS INT 21h
                  relocs                                            BIOS INT 10h/16h
```

- **CPU** (`src/cpu.cpp`): a 16-bit 8086 real-mode interpreter — segmented
  addressing (`physical = segment*16 + offset`), the ALU/`MOV`/stack/`Jcc`/string
  groups, `ModRM` 16-bit addressing modes, and `INT n`.
- **DOS** (`src/dos.cpp`): the `INT 21h` service layer — console and file I/O,
  memory allocation, the PSP and command line, and **program load & execute**
  (`AH=4Bh`), which is what lets the compiler driver spawn its passes.
- **Loader** (`src/loader.cpp`): `.COM` (flat, loaded at `PSP:0x100`) and `.EXE`
  (MZ header, relocation table, initial `CS:IP`/`SS:SP`).

Anything unimplemented stops with a message naming the opcode or `INT` function and
the address, so bringing up a new guest is a matter of following the messages —
the same method used for the sibling emulator.

## Status (staged, bring-up in progress)

1. 🚧 16-bit CPU core + `.COM`/`.EXE` loader + minimal `INT 21h` (console, exit)
2. ⏭ DOS file I/O, memory, PSP/argv → run a single LSI C pass on its own
3. ⏭ `INT 21h AH=4Bh` EXEC (child processes) → the full `LCC` pipeline
4. ⏭ compile a `.c` to `.EXE` and run it
5. ⏭ WebAssembly build + browser demo (drop/edit a `.c`, compile & run in the page)

## Building

```sh
sh build.sh          # or: g++ -std=c++17 -O2 -Isrc -o dosemu src/*.cpp
./dosemu PROG.EXE [args...]
```

## License

Own code MIT. `lsic330c.lzh` is the LSI C-86 trial package, redistributed under its
own terms (see the archive's READ.ME / LSIC86.MAN).
