# Where this is, and what to do next

Working notes for picking the project back up. The README says what the emulator
*is*; this says what works, what's next, and what was learned.

## State (all verified, on the browser build too)

A 16-bit real-mode 8086 MS-DOS emulator in C++/WebAssembly.

- **8086 core** (`src/cpu.cpp`): integer subset + string ops + shifts + MUL/DIV;
  x87 escapes (D8-DF) are consumed as no-ops (LSI C uses software float). A runaway
  guard (`Cpu::max_insns`) stops rather than hanging the tab.
- **DOS** (`src/dos.cpp`): INT 21h — console + line input, file I/O (handles, DTA,
  FindFirst/Next with DOS 8.3 wildcards), memory (48/49/4A bump alloc), **EXEC
  (4Bh)** run children on the same CPU via a nested loop, mkdir/rmdir, chdir/getcwd
  with a **persistent current directory**, country-info fallback (NLS), INT 16h/2Fh.
- **Loader** (`src/loader.cpp`): .COM and .EXE (MZ) with relocations, PSP, an
  environment block carrying the program's own path + PATH/COMSPEC.
- **LSI C-86** compiles and runs C end to end: `LCC.EXE` spawns CPP→CF→CG86→R86→LLD
  (nested EXECs) into a DOS `.EXE`, which then runs.
- **FreeDOS COMMAND.COM** (FreeCOM 0.86) runs to an interactive prompt: `dir`, `ver`,
  `echo`, `type`, `cd`, `md`/`rd`, and external programs all work.
- **Browser demo** (`web/`, GitHub Pages): a FreeDOS prompt; each line runs as
  `COMMAND.COM /C <cmd>`. Headless tests: `web/test_node.mjs`, `test_bundle.mjs`,
  `test_shell.mjs`.

The compiler and shell are gitignored downloads: `lsic330c.lzh` (in repo) extracts
to `lsic/`; FreeCOM is `fdos/`; the browser bundle is `web/lsic.tar.gz`.

## Next: DJGPP / DOS extender (DPMI, 32-bit protected mode)

The goal: run DJGPP programs (32-bit DOS gcc etc.). This is the big one — roughly a
second CPU (32-bit protected mode) plus a DPMI host on top of the working 16-bit core.

**What a DJGPP `.EXE` is** (measured, `djgpp/bin/djecho.exe`, a 97 KB test program;
`djgpp/CWSDPMI.EXE` is the stock DPMI host, both gitignored):
- a **2 KB real-mode DOS stub** (go32) — MZ, followed by
- an **i386 COFF** image at the stub's end: machine `0x14C`, a.out magic `0x10B`,
  entry `0x18B0`, sections `.text` (v `0x18a8`), `.data` (v `0x13c00`), `.bss`.
  `src/coff_loader.cpp` already detects (`is_djgpp_coff`) and parses this.
- The stub finds/loads a DPMI host, switches to 32-bit protected mode, maps the COFF
  flat and jumps to its entry. `load_program()` now detects a DJGPP exe and returns a
  clear "not implemented yet" message instead of crashing.

**First gap hit today:** running the stub in the 16-bit core faults at ~457
instructions on opcode **`0x66`** — the 32-bit operand-size prefix. That is the door
to everything below.

**Plan (recommended order):**

1. **32-bit CPU.** Give the interpreter 32-bit registers and operands. Safest way
   that won't disturb the working 16-bit paths: keep `uint16_t r[8]` and add a
   parallel `uint16_t rhi[8]` for the upper halves (a 32-bit reg is
   `(rhi[i]<<16)|r[i]`); 16-bit ops stay untouched. Handle the `0x66` (operand-size)
   and `0x67` (address-size) prefixes → an `osize`/`asize` in the decoder, and add
   32-bit forms of the ALU/MOV/PUSH/POP/string/shift group plus 386 additions
   (MOVZX/MOVSX 0F B6/B7/BE/BF, SETcc, BT group, IMUL variants, SIB addressing).
2. **Protected mode.** GDT/LDT/IDT, selectors → descriptor {base,limit,flags};
   `LGDT`/`LIDT`/`LMSW`/`MOV CRn`, protected-mode far jump, the real↔protected switch.
   Segment reads become descriptor-based (base+offset) instead of `seg<<4`.
3. **DPMI host — build our own, don't run CWSDPMI.** Intercept the stub's DPMI
   detection (`INT 2Fh AX=1687h` → return "present" + a mode-switch entry point that
   we service), then provide `INT 31h` services the go32 client uses: LDT descriptor
   alloc/free (`0000/0001`), set base/limit (`0007/0008`), allocate memory (`0501`),
   map, get/set exception & protected-mode interrupt vectors, and **simulate real-mode
   interrupt (`0300`)** so the client's DOS calls (INT 21h etc.) reflect back into the
   existing 16-bit DOS layer. Building the host ourselves avoids emulating CWSDPMI's
   own real→PM code.
4. **Load the COFF flat** (`coff_loader.cpp` → map `.text`/`.data`, zero `.bss`, set
   flat 4 GB CS/DS/SS selectors) and jump to `entry` in 32-bit mode.
5. **Reuse the DOS layer** for file/console I/O via the `0300` real-mode reflection,
   with 32-bit register access.

Test target: `djecho.exe` first (tiny), then a real DJGPP tool, then `gcc` itself.
This is multi-session; do the 32-bit CPU behind regression tests (LSI C + FreeCOM
must keep passing) before touching protected mode / DPMI.

## Practical notes

- Build (MSVC, no vcvars): `cc.sh -std:c++17 -O2 -EHsc -utf-8 -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -Isrc src/*.cpp`.
  WASM: `EMCC=$(which emcc) sh web/build.sh` (rebuild + recommit `web/dosemu.js` after any `src/` change).
- Regression before committing: `node web/test_shell.mjs` (SHELL DEMO OK) and a
  native `LCC.EXE PROG.C` → run.
- The lesson from FreeCOM: a "success but blank" INT 21h reply is worse than an honest
  failure — AH=65h returning CF (not supported) let FreeCOM keep its own sane NLS
  fallback; returning success with an empty buffer had broken every internal command.
