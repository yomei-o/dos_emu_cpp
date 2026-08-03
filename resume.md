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

The goal: run 32-bit DOS-extended programs — **DJGPP** (gcc etc.) *and* **OpenWatcom**
(wcc/wcc386/wlink), which is what compiles FreeCOM. Both are the same shape: a
real-mode stub wrapping a 32-bit image that switches to protected mode via a DPMI
host. They share everything below except the image loader (COFF vs LE).

- **DJGPP** `.EXE` (measured, `djgpp/bin/djecho.exe`): 2 KB real-mode **go32** stub +
  **i386 COFF** (machine `0x14C`, magic `0x10B`, entry `0x18B0`, `.text`/`.data`/`.bss`).
  `src/coff_loader.cpp` detects (`is_djgpp_coff`) and parses it.
- **OpenWatcom** DOS `.EXE` (measured, `ow/binw/wcc.exe` etc., gitignored under `ow/`):
  MZ + **LE** (linear executable) at `e_lfanew`, string `"DOS/4G"`. `wcc`/`wcc386`/
  `wlink` are all DOS/4GW. `wcl.exe` is a plain 16-bit driver that just spawns them.

### DONE — 80386 real-mode core (`src/cpu.cpp`, committed, regression-clean)

The 32-bit CPU is built the safe way: `uint16_t r[8]` unchanged, upper halves in a
parallel `uint16_t rhi[8]` (`gd`/`sd` in cpu.h), so 16-bit writes preserve the upper
halves like a real 386 and **every 16-bit path is byte-for-byte identical**. Added:
`0x66/0x67` prefixes → `o32`/`a32`; 32-bit ModRM + SIB; FS/GS + `0x64/0x65` overrides;
width-generic ALU/MOV/PUSH/POP/INC-DEC/TEST/XCHG/shift/string/mul-div; PUSHA/POPA,
PUSH imm, IMUL r/rm/imm, ENTER/LEAVE, CWDE/CDQ; the `0x0F` map (long Jcc, SETcc,
CMOVcc, MOVZX/MOVSX, IMUL, BT/BTS/BTR/BTC, SHLD/SHRD, BSF/BSR, PUSH/POP FS/GS,
LSS/LFS/LGS, CPUID/RDTSC stubs); and the system instructions that arm the mode
switch (MOV CRn/DRn, LGDT/LIDT/SGDT/SIDT, LMSW/SMSW, LLDT/LTR, CLTS) writing new
`cr[]`/`dr[]`/`gdt_*`/`idt_*`/`ldtr`/`tr` state in cpu.h.

**Validated with real extender code** (probe: temporarily no-op the `is_djgpp_coff`
bail in `loader.cpp`, build, run): the go32 stub and the DOS/4GW stub both now run
all the way through their real-mode setup and stop exactly at the DPMI check —
DJGPP prints `no DPMI - Get csdpmi*b.zip`, DOS/4GW prints `Can't run DOS/4G(W)`.
The old `0x66` fault is gone. So the CPU is ready; what's missing is the mode switch.

### IN PROGRESS — protected mode + DPMI (branch `wip-pmode-dpmi`, has a regression)

The protected-mode CPU groundwork is written but **breaks the LSI C regression** and
is parked on branch `wip-pmode-dpmi` (main stays green at the 386-core commit). Pick
up here. What the branch changes (src/cpu.h, src/cpu.cpp, src/memory.h only):

- **Memory → 16 MiB** (`memory.h`): `kSize=0x1000000`, `kMask`; low 1 MiB is real
  mode, above is extended (protected-mode linear). `phys()` still masks real mode to
  1 MiB. `rd`/`wd` seg:off helpers added.
- **`ip` widened to 32-bit**; `sreg[6]` (FS/GS); descriptor cache `sbase[6]`,
  `slimit[6]`, `cs_d`/`ss_d`; system regs `cr[]`,`dr[]`,`gdt_*`,`idt_*`,`ldt_base`,
  `ldtr`,`tr`. `pe()`, `lin(seg,off)` (real mode wraps at 1 MiB exactly as before;
  PE uses `sbase+off`), `set_seg()` (loads a GDT/LDT descriptor in PE, `sel<<4` in
  real mode), stack-size-aware `sp_get/sp_set`/push/pop, `on_pm_switch`/`pm_switch_addr`
  hook (reaching that linear addr runs the DPMI mode switch instead of an instruction).
- **`cpu.cpp`**: ModRM now yields a segment *index* (`seg_idx`) and `pa()=lin()`;
  all segment loads/far transfers go through `set_seg`; string ops/moffs are
  address-size aware; effective `o32/a32 = prefix ^ cs_d`.

**The regression (must fix before continuing):** native `LCC.EXE PROG.C` fails with
`cg: can't open: 2.$$$`. Root cause traced precisely:
- The pass chain is CPP → CF → CG86 (all EXEC'd, **all load at CS=0x3010**, one at a
  time — so a naive "CS==0x3010" trace catches CPP, which *works*; you must gate the
  trace on CF specifically).
- CPP runs fine (writes `1.$$$`). **CF runs ~640 instructions then crashes**: a
  `E8` near-CALL at `3010:0321` jumps to `3010:9708`, which is **all zeros** in this
  build but holds code in the working (386-core) build. CF then marches through zero
  memory and dies. So somewhere in CF's first 640 instructions the refactor either
  (a) mis-addresses a memory *write* that zeroes code at CS:0x9708, or (b) takes a
  wrong branch (bad flags) reaching a corrupt `E8`. CPP's first 300 instructions are
  byte-identical old-vs-new, so it's subtle and CF-specific.

**Fastest way to find it (harness recipe):** add `bool dbg` to `Cpu`, print
`CS:IP op regs` at the top of `step()` when `dbg`, and in `Dos::exec` set
`cpu_.dbg=true` while `name.find("cf.exe")!=npos` (gate on `getenv("TE")`). Build the
**working** tree too (`git stash` / checkout the 386-core `cpu.*`+`memory.h`, keep the
same dbg hook) as `dosemu_old.exe`. Run both `TE=1 ... LCC.EXE PROG.C 2>trace`, grep
`^3010:`, and `diff` the two CF traces — the **first differing line** is the buggy
opcode. (Watch the shell: cp932/heredoc mangles C string escapes; use the Edit tool,
not python heredocs, to patch the trace into committed files.)

**Prime suspects** (real-mode reductions of the refactor that could still be wrong):
`set_seg` side effects on far RET/CALL; `sp_get/sp_set` vs the old `r[SP]-=2`; a
`seg_idx` defaulting to the wrong segment on some ModRM form (writes going to CS/ES
instead of DS/SS → would zero code); `lin()` masking; or an `aluv`/flags edge that
flips a `Jcc`. Test target after any fix: `LCC.EXE PROG.C` → run, and
`node web/test_shell.mjs`.

Scratch fixtures (gitignored): `scratch_root/` = a drive with `LSIC86/`, `PROG.C`,
`COMMAND.COM`; build with `cc.sh ... -Fe:dosemu.exe`; run
`./dosemu.exe --root scratch_root scratch_root/LSIC86/BIN/LCC.EXE PROG.C`.

### TODO — after the regression is fixed: the DPMI host

1. **Bigger address space.** `Memory` is 1 MiB flat. Protected-mode flat clients
   address many MB. Add extended RAM (e.g. 16 MB) reachable by *physical* 32-bit
   address; real mode keeps using the low 1 MiB. (This is the first concrete step.)
2. **Protected mode.** Selector→descriptor {base,limit,flags} from GDT/LDT; when
   `cr[0]&PE`, segment access is `base+offset` (32-bit) not `seg<<4`. Instruction
   fetch and stack switch to the descriptor model. The system instrs above already
   load the tables.
3. **DPMI host — build our own, don't run CWSDPMI.** Intercept `INT 2Fh AX=1687h` →
   return present (CF=0), 32-bit capable (BX bit0), CPU=3, ver 0.90, SI=0 paras
   needed, `ES:DI` = a real-mode entry we recognise. When the stub far-calls it,
   perform the real→protected switch ourselves. Then service `INT 31h`: descriptor
   alloc/free (`0000/0001`), set base/limit (`0007/0008`), alloc LDT selectors,
   allocate memory (`0501`), get/set exception & PM interrupt vectors, real-mode
   callbacks, and **simulate real-mode interrupt (`0300`)** so the client's DOS/BIOS
   calls reflect back into the existing 16-bit DOS layer (with 32-bit regs).
4. **Image load.** DJGPP: map COFF `.text`/`.data`, zero `.bss` (`coff_loader.cpp`),
   flat 4 GB CS/DS/SS/ES/FS/GS selectors, jump to `entry`. OpenWatcom: write an LE
   loader (objects → pages, relocations/fixups) and hand DOS/4GW's own extender
   code protected mode via our DPMI host (it becomes a DPMI *client*, same path).
5. **Reuse the DOS layer** for I/O via `0300` reflection. Also add INT 21h **AH=31h**
   (TSR) — the go32 stub calls it during DPMI setup (seen in the probe).

Test order: `djecho.exe` (tiny COFF) → prove protected mode + DPMI + COFF; then the
LE loader → `wcl hello.c` (OpenWatcom parity with the LSI C demo); then scale toward
FreeCOM's full `wmake` build. Keep LSI C + FreeCOM green throughout (`node
web/test_shell.mjs`, native `LCC.EXE PROG.C`).

## Practical notes

- Build (MSVC, no vcvars): `cc.sh -std:c++17 -O2 -EHsc -utf-8 -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -Isrc src/*.cpp`.
  WASM: `EMCC=$(which emcc) sh web/build.sh` (rebuild + recommit `web/dosemu.js` after any `src/` change).
- Regression before committing: `node web/test_shell.mjs` (SHELL DEMO OK) and a
  native `LCC.EXE PROG.C` → run.
- The lesson from FreeCOM: a "success but blank" INT 21h reply is worse than an honest
  failure — AH=65h returning CF (not supported) let FreeCOM keep its own sane NLS
  fallback; returning success with an empty buffer had broken every internal command.
