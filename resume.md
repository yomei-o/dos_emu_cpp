# Where this is, and what to do next

Working notes for picking the project back up. The README says what the emulator
*is*; this says what works, what's next, and what was learned.

> **All work is on `main`.** Protected mode and the DPMI host are in and
> regression-green: 32-bit DJGPP programs load and run. The one thing missing is
> **argv** — see "The gap: argc == 0". That is where to pick up.

## State (all verified, on the browser build too)

An 8086/80386 MS-DOS emulator in C++/WebAssembly — 16-bit real mode, plus 32-bit
protected mode with a built-in DPMI host for DOS-extended programs.

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

**Validated with real extender code**: with no DPMI host present, the go32 and DOS/4GW
stubs ran their whole real-mode setup and stopped exactly at the DPMI check — DJGPP
printing `no DPMI - Get csdpmi*b.zip`, DOS/4GW `Can't run DOS/4G(W)`. That was the
milestone at the time; the host below now answers that check. If a future CPU change
breaks 32-bit real-mode code, reaching *those* messages is still the right first test.

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

**DJGPP programs need no special flag** — the loader's refusal is gone, since they run.
`DOSEMU_DPMI_TRACE=1` logs the whole DPMI conversation.

**The DJGPP fixture** (`djgpp/bin/djecho.exe` from djdev205, 97792 bytes) parses with
`src/coff_loader.cpp` as: entry `0x18B0`, text base `0x18A8`, and

    .text  vaddr 0x000018A8  size 0x012358  raw 0x0010A8
    .data  vaddr 0x00013C00  size 0x004200  raw 0x013400
    .bss   vaddr 0x00017E00  size 0x003600  raw 0

so step 4 below (map .text/.data, zero .bss) has concrete numbers to check against.

Scratch fixtures (gitignored): `scratch_root/` = a drive with `LSIC86/`, `PROG.C`,
`COMMAND.COM`; build with `cc.sh ... -Fe:dosemu.exe`; run
`./dosemu.exe --root scratch_root scratch_root/LSIC86/BIN/LCC.EXE PROG.C`.

### DONE — the DPMI host: 32-bit DJGPP programs run

`src/dpmi.{h,cpp}`. A DJGPP `.EXE` now goes down the ordinary MZ path: the go32 stub is
plain 16-bit code, it finds our host, switches to protected mode, loads its own COFF and
jumps into it. No special-casing left in the loader.

    ./dosemu --root scratch_root scratch_root/stubify.exe
    ./dosemu --root scratch_root scratch_root/dtou.exe
    -> the 32-bit program's own usage text

How it fits together:

- **INT 2Fh AX=1687h** answers "present, 32-bit capable, 386, v0.90, 0 paragraphs
  needed" and returns a real-mode far address at segment `0x00E0` — a paragraph nothing
  else uses (IVT/BIOS end at 0x0500, the environment sits at 0x00F0, programs load from
  0x0100). `Cpu::pm_switch_addr` traps that linear address instead of executing it.
- **The switch** pulls the far-call return address off the real-mode stack, builds four
  16-bit descriptors over the client's current CS/DS/ES/SS, sets `cr0.PE`, and resumes
  at the return address with CS reloaded as a selector.
- **Descriptors** live in an LDT at linear `0x00110000`; `0501h` hands out extended
  memory from 2 MiB up with a bump allocator.
- **`0300h` (simulate real-mode interrupt) is the hinge.** It drops the CPU to real mode
  for the duration of the call, loads the client's 50-byte frame into the registers, and
  calls the *existing* DOS layer. Because `dos.cpp` reads its arguments as
  segment:offset, every INT 21h handler works unmodified — there is no second,
  protected-mode-aware copy of the DOS layer, and there does not need to be.
  `Cpu::save()`/`restore()` (new, in cpu.h) puts the client's world back afterwards:
  saving the registers alone is not enough once cr0/cs_d/ip_mask/sbase decide how those
  registers are read.
- **`LAR`/`LSL` (`0F 02`/`0F 03`)** were added to the CPU. DJGPP's startup runs `LSL`
  within a few instructions of its entry to size the segment it was given.
- **`INT 21h AH=50h/51h/62h`** (set/get PSP) were missing. Nothing 16-bit ever asked —
  a .COM or .EXE gets DS=ES=PSP at entry and keeps it — but an extender reloads the
  segment registers and has to ask. The no-op default returned success with BX
  untouched, which is the kind of "success but blank" answer the FreeCOM lesson at the
  bottom of this file warns about.

Watch one run with `DOSEMU_DPMI_TRACE=1`: it logs every INT 31h call and, for each
reflected interrupt, the DOS function and the bytes of any write.

**argv works** — and the bug was worth writing down. Programs ran and produced their own
output but got `argc == 0`, with *both* the command tail and the environment missing.
That pointed at the client's view of the PSP rather than at the tail, and the cause was
one line of the DPMI mode-switch contract: **on exit ES must be a selector for the
client's PSP**, not an alias of whatever ES it happened to be holding when it called.
That is how an extender finds the tail at PSP:0x80 and the environment segment at
PSP:0x2C. We were aliasing the caller's ES, so go32 recorded the wrong PSP and read an
empty tail out of it — with nothing else visibly broken.

Ruled out along the way, so nobody re-checks them: `AH=62h` (the stub never calls it;
implemented anyway, and it *was* genuinely missing), and `_dos_ds` (the client builds it
correctly as selector `0x4C`, base 0, limit `0x110FFF`).

**Also unimplemented, and harmless so far** — every DJGPP program opens with
`Warning: Coprocessor not present and DPMI setup failed!`. That is exactly two missing
functions: `0303h` (allocate real-mode callback), used to hook the FPU emulator, and
`0E01h` (set coprocessor emulation). The x87 escapes are still no-ops in the CPU, so
this only matters for a program that does floating point. `0507h` also fails; it is a
DPMI 1.0 function the client merely probes.

### TODO — the rest of the extender work

1. ✅ Bigger address space (16 MiB), ✅ protected mode, ✅ DPMI host, ✅ COFF load —
   all above. Note the COFF loading is done by **go32 itself**, not by us:
   `src/coff_loader.cpp` parses an image but nothing calls it on the run path. It stays
   useful for inspecting a binary, and for the OpenWatcom LE work below.
2. **Fix `argc == 0`** (see above). Until this is done, a DJGPP program can only do
   what it does with no arguments, so nothing built on top of it is worth trying.
3. **The FPU.** `0303h` + `0E01h` to silence the startup warning, and eventually real
   x87 instead of no-op D8-DF escapes, for anything that computes.
4. **OpenWatcom / DOS/4GW.** An MZ + **LE** image (`ow/binw/wcc.exe`, string `"DOS/4G"`
   at `e_lfanew`). Unlike DJGPP, the extender is not a thin stub — DOS/4GW is a full
   extender that becomes a *client* of our DPMI host. Needs an LE loader (objects →
   pages, fixups). `wcl.exe` is a plain 16-bit driver that just spawns the others.
5. **INT 21h AH=31h** (TSR) — the go32 stub calls it during DPMI setup.

Test order: ✅ `djecho.exe` proved protected mode + DPMI + COFF; then argv; then the LE
loader → `wcl hello.c` (OpenWatcom parity with the LSI C demo); then scale toward
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
