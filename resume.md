# Where this is, and what to do next

Working notes for picking the project back up. The README says what the emulator
*is*; this says what works, what's next, and what was learned.

> **All work is on `main` again.** `wip-pmode-dpmi` was merged once its LSI C
> regression was fixed (see "The regression, and what it actually was" below);
> the protected-mode groundwork is now in `main` and regression-green. Next up is
> the DPMI host — the numbered TODO at the end.

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

### DONE — protected-mode groundwork (merged to `main`, regression-clean)

The protected-mode CPU groundwork is in and the LSI C regression that parked it is
**fixed** (see below). All three headless tests and the native compile-and-run pass.
It touches src/cpu.h, src/cpu.cpp and src/memory.h only:

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

**The regression, and what it actually was.** Symptom: native `LCC.EXE PROG.C` failed
with `cg: can't open: 2.$$$` because CF crashed after ~640 instructions — an `E8`
near-CALL at `3010:0321` landing in zeroed memory.

Cause: **widening `ip` from `uint16_t` to `uint32_t` deleted an invariant nobody had
written down.** While `ip` was 16 bits, every `ip += rel` wrapped inside the 64 KiB
segment for free — which is exactly what a 16-bit code segment does. As a `uint32_t`
it no longer wrapped, so a backward branch from a low offset produced `0xFFFF9708`
instead of `0x9708`, and `lin(CS, ip)` — which adds `CS<<4` and masks to 20 bits —
turned that into a linear address **exactly 64 KiB below** the intended one. Reading
zeros there was a symptom two steps removed from the cause, which is why the earlier
diagnosis (a stray write zeroing code) pointed the wrong way. Note the trap: the
address *looked* right when printed as `CS:IP` with IP truncated for display.

Fix: `Cpu::ip_mask` (`0xFFFF` in a 16-bit code segment, `0xFFFFFFFF` when the CS
descriptor has D=1), maintained by `set_seg`, applied by `Cpu::jump()` and by the
`fetch*` advance. Every near transfer — Jcc short and near, `E8`/`E9`/`EB`, LOOP*/
JCXZ, `C2`/`C3`, `FF /2`, `FF /4` — goes through `jump()`; the far ones (`EA`, `9A`,
`CA`, `CB`, `CF`, `FF /3`, `FF /5`, and `interrupt()`) now load CS **before** jumping,
so the new segment's width is already in `ip_mask`. Drive-by: `FF /2` (CALL near rm)
now reads its target before pushing, which matters once an operand can be ESP-relative.

**The general lesson** (this is the second time on this project): a refactor that
widens a type silently deletes whatever the narrow type was enforcing. `uint16_t ip`
was not a storage choice, it was the 16-bit-segment wrap rule. Same shape as the
FreeCOM NLS lesson below — the code was *more correct* in the narrow case by accident,
and making it general removed the accident without replacing it.

Verified: `LCC.EXE PROG.C` → `PROG.exe` → `sum=55`; `node web/test_shell.mjs`
(SHELL DEMO OK), `web/test_bundle.mjs` (BUNDLE OK), `web/test_node.mjs` (DOS + LSI C
IN WASM OK) after rebuilding `web/dosemu.js`.

**Fixtures on a fresh clone: `sh get_fixtures.sh [djgpp]`.** `lsic/`, `scratch_root/`
and `djgpp/` are all gitignored, and `lsic330c.lzh` in the repo root needs an LZH
extractor most machines lack. But `web/lsic.tar.gz` **is committed** and holds the whole
LSI C tree plus `COMMAND.COM`, so the script reconstitutes everything from it with no
network and no extractor; the optional `djgpp` argument additionally downloads
`djdev205.zip`. It also writes `scratch_root/PROG.C` rather than committing it — the file
needs CRLF endings *and* a literal `\n` in the source, and getting that wrong yields
`missing "` from the LSI C front end instead of anything informative.

**`DOSEMU_RUNSTUB=1` runs a DJGPP/DOS-4GW stub instead of refusing it** (`loader.cpp`).
This is the only 32-bit-heavy code available to test the CPU against before the DPMI host
exists, so it is now a documented switch rather than a temporary source edit. Expected
output today — anything *earlier* than these is a CPU bug:

    DOSEMU_RUNSTUB=1 ./dosemu --root scratch_root scratch_root/DJECHO.EXE
    -> no DPMI - Get csdpmi*b.zip

**The DJGPP fixture** (`djgpp/bin/djecho.exe` from djdev205, 97792 bytes) parses with
`src/coff_loader.cpp` as: entry `0x18B0`, text base `0x18A8`, and

    .text  vaddr 0x000018A8  size 0x012358  raw 0x0010A8
    .data  vaddr 0x00013C00  size 0x004200  raw 0x013400
    .bss   vaddr 0x00017E00  size 0x003600  raw 0

so step 4 below (map .text/.data, zero .bss) has concrete numbers to check against.

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

- **Fixtures first on a fresh clone:** `sh get_fixtures.sh djgpp` (see above). Nothing
  runs without it.

- Build (MSVC, no vcvars): `cc.sh -std:c++17 -O2 -EHsc -utf-8 -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -Isrc src/*.cpp`.
  WASM: `EMCC=$(which emcc) sh web/build.sh` (rebuild + recommit `web/dosemu.js` after any `src/` change).
- Regression before committing: `node web/test_shell.mjs` (SHELL DEMO OK) and a
  native `LCC.EXE PROG.C` → run.
- The lesson from FreeCOM: a "success but blank" INT 21h reply is worse than an honest
  failure — AH=65h returning CF (not supported) let FreeCOM keep its own sane NLS
  fallback; returning success with an empty buffer had broken every internal command.
