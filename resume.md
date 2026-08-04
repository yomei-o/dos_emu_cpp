# Where this is, and what to do next

Working notes for picking the project back up. The README says what the emulator
*is*; this says what works, what's next, and what was learned.

> **All work is on `main`.** Protected mode and the DPMI host are in and regression-green.
> Three toolchains now compile *and link* inside the emulator: LSI C-86, DJGPP gcc/g++, and
> **Watcom C/C++ — the linker works too, as of this session.**
>
> **HANDOFF (next session starts here).** Everything is on `main` and green. There is no
> broken thread to pick up; what follows is a menu, not a queue.
>
>     sh get_fixtures.sh ow && sh build.sh
>     ./dosemu --root ow ow/binw/wcc.exe   hello.c
>     ./dosemu --root ow ow/binw/wlink.exe "system dos file hello.obj name hello.exe"
>     ./dosemu --root ow hello.exe        # hello from Watcom C, sum=55
>     ./dosemu --root ow ow/binw/wcl.exe  hello.c    # ...or the driver, in one command
>
> Regression before committing anything: `./dosemu --root scratch_root
> scratch_root/LSIC86/BIN/LCC.EXE PROG.C` then `PROG.exe` (sum=55), `COMMAND.COM /C "lcc
> prog.c"`, `fp.exe` (H10=2.928968 sqrt2=1.414214 sin1=0.841471), DJGPP `hello.exe` and
> `mini.exe`, the Watcom compilers + `wcl hello.c`, and — after `EMCC=<path> sh
> web/build.sh` — all **five** of `web/test_shell.mjs`, `test_node.mjs`, `test_bundle.mjs`,
> `test_djgpp.mjs`, `test_watcom.mjs`.
>
> **What was done this session** (details in `OPENWATCOM.md`, "Where the linker actually
> stands"):
>
> - **The linker links.** Two DPMI-host bugs: `pm_int_default()` unwound the interrupt
>   frame on top of `rm_call()`'s transfer of control (so DOS/4GW ended up executing the
>   interrupt vector table), and `rm_return()` reported the flags an IRET had just
>   restored from the host's own fabricated frame instead of the ones the real-mode
>   procedure computed — throwing away `CF`, which is the whole result of a DOS `open`.
>   `Cpu::iret_flags` keeps them. The second bug is why wlink claimed
>   `invalid library file attribute` about a library it had never opened.
> - **`wcl` works**, which took three EXEC fixes: a program's own path must be stamped
>   into a caller-supplied environment block (DOS 3.0+ does this, and Watcom's stub reads
>   it back to tell `w32run.exe` what to load); the DTA and the FindFirst/FindNext state
>   belong to the process and must not leak from a child to its parent; and a child's
>   environment belongs immediately below the child, not at the bottom of the arena, where
>   freeing it left one-paragraph holes that DOS/4GW's sizing loop collected as its
>   transfer buffer.
> - **A third browser demo**: `web/watcom.html` + `web/watcom.tar.gz` (1.7 MB) + `node
>   web/test_watcom.mjs`, rebuilt by `sh make_watcom_bundle.sh`. Provenance and the one
>   version mix — the 16-bit libraries are OpenWatcom v2's, and why they have to be — are
>   in `web/WATCOM-LICENSE.md`.
> - The **DPMI trace now goes to stderr** rather than stdout. It used to interleave with
>   the guest's own output (`Error! E2012: ...[dpmi] real-mode call returned`) and vanish
>   whenever a run died before the buffer flushed; correlating it with `DOSEMU_DOS_TRACE`
>   is how both linker bugs were found.
>
> **The four items the last note left are all done, and three were emulator bugs after
> all** (`OPENWATCOM.md` has each in full):
>
> 1. **`LODSB` was clobbering `AH`.** The byte form loads AL and must leave AH alone. This
>    *was* "OpenWatcom v2's binaries do not run": its DOS/4G stub counts a PATH element's
>    length in AH across a `lodsb` copy loop, and with the count destroyed it never appended
>    the `\` — hence `A:\BINWDOS4GW.EXE`. Four toolchains had not caught it.
> 2. **A number that does not name a *live* descriptor is not a selector.** `load_frame()`
>    accepted any value with bit 2 set whose LDT slot was in range, and an empty slot reads
>    back as base 0 — so the paragraph `0x018F` that DOS/4GW correctly puts in the frame
>    became segment 0. **This is what the garbled `DOS/16M error: ...` was**, the thing two
>    sessions chased through DS. The descriptor now has to be present.
> 3. **`INT 21h AH=40h` with `CX=0` truncates the file at the current position** — the only
>    way DOS can shorten a file. Ours returned success and did nothing, so a stale tail
>    survived when Watcom's compilers rewrote an object they had opened read/write. That was
>    the "8.3 case folding" item: not case at all.
> 4. **A 32-bit link works now**, and what it builds runs: `math387r.lib` from the v2
>    installer, plus a real stub for the output — carved out of `wlink.exe`'s own MZ image,
>    since no `wstub.exe` ships in the component zips. `get_fixtures.sh ow` does both.
> 5. **`AH=5Dh` and `AH=66h`** answer honestly (invalid function) instead of falling through
>    to the "unimplemented" message. Every caller has a fallback and takes it.
>
> **And then the last three, which are also done:**
>
> - **OpenWatcom v2's own DOS toolchain runs** — its `wcc` compiles and its `wlink` links,
>   inside the emulator. What was in the way was our own cleverness: `load_frame()` tried to
>   recognise a selector in the real-mode call structure's segment field and substitute the
>   paragraph it covers. **A paragraph and a selector are the same sixteen bits**, so that is
>   not a decidable question: DOS/4GW's correct paragraph `0x018F` was read as a live
>   descriptor with base `0xE1E0`, and it reported `can't find file A:\WCC.EXE to load` about
>   a file that was there. The conversion was a workaround for bugs since fixed (the lost
>   `CF`, the arena holes, `LODSB`); taking the frame literally is simpler, matches the spec,
>   and keeps every Watcom path green.
> - **The emulator no longer dies on a filename.** A guest that has jumped into data passes
>   machine code where a name should be, and building an `fs::path` from it throws under a
>   DBCS code page (cp932). It exited with no message, which reads as a hang. `host_path()`
>   catches it, and the whole INT 21h dispatch is inside a `try` that answers "not found"
>   (`CpuError` excepted — that one is ours and belongs to `main()`). That is what let
>   DOS/4GW print the message that identified the bug above.
> - **The one-byte opcode map is complete for the integer set**: the BCD adjust group
>   (`DAA`/`DAS`/`AAA`/`AAS`/`AAM`/`AAD`), plus `SALC` and `ICEBP`. `scratch_root/bcd.c`,
>   which `get_fixtures.sh djgpp` writes, checks all six against the values the SDM fixes and
>   is compiled by DJGPP inside the emulator. `INS`/`OUTS`/`XLAT`/`BOUND`/`ARPL`/`0x82` were
>   already there; `OPENWATCOM.md` said otherwise and was out of date.
>
> **What to pick up.** Nothing is outstanding. Ideas, in rough order of interest:
>
> 1. **`wmake`, and building something real** — FreeCOM itself is the obvious target, since
>    that is what the Watcom tools were brought up for.
> 2. **A `watcom.tar.gz` with no version mix.** The bundle is 11.0c binaries plus four
>    OpenWatcom v2 libraries (`web/WATCOM-LICENSE.md` says why). Now that v2's own binaries
>    run, an all-v2 bundle would be one provenance instead of two — at the cost of depending
>    on the 128 MB installer rather than the documented component zips.
> 3. **32-bit output in the browser demo.** `system dos4g` works natively; the bundle carries
>    only the 16-bit libraries and would need `clib3r`/`plib3r`/`math387r` and `dos4gw.exe`
>    added, about 1 MB gzipped.
> 4. **The x87 computes correctly** through libm and `printf("%f")` — `scratch_root/fp.c`.
>    Two fixes worth knowing about, in "The x87 was wrong twice" below.

## State (all verified, on the browser build too)

An 8086/80386 MS-DOS emulator in C++/WebAssembly — 16-bit real mode, plus 32-bit
protected mode with a built-in DPMI host for DOS-extended programs.

- **8086 core** (`src/cpu.cpp`): integer subset + string ops + shifts + MUL/DIV;
  the x87 stack (`src/fpu.cpp`); LAR/LSL, BOUND, the A20 gate, and protected-mode
  interrupt dispatch through the guest's own IDT with a TSS stack switch. A runaway
  guard (`Cpu::max_insns`) stops rather than hanging the tab.
- **DOS** (`src/dos.cpp`): INT 21h — console + line input, file I/O (handles, DTA,
  FindFirst/Next with DOS 8.3 wildcards), memory (48/49/4Ah over a real block list
  with a free that frees), **EXEC (4Bh)** run children on the same CPU via a nested
  loop, **load overlay (4Bh AL=03)**, mkdir/rmdir, chdir/getcwd
  with a **persistent current directory**, country-info fallback (NLS), INT 16h/2Fh.
- **Loader** (`src/loader.cpp`): .COM and .EXE (MZ) with relocations, PSP, an
  environment block carrying the program's own path + PATH/COMSPEC.
- **LSI C-86** compiles and runs C end to end: `LCC.EXE` spawns CPP→CF→CG86→R86→LLD
  (nested EXECs) into a DOS `.EXE`, which then runs.
- **FreeDOS COMMAND.COM** (FreeCOM 0.86) runs to an interactive prompt: `dir`, `ver`,
  `echo`, `type`, `cd`, `md`/`rd`, and external programs all work.
- **Watcom C/C++ 11.0c** compiles *and links*, 16- and 32-bit: `wcc`/`wpp` and
  `wcc386`/`wpp386` (all under FlashTek X-32), then `wlink` (under DOS/4GW, a DPMI client)
  into a DOS `.EXE` that runs — `system dos` for a real-mode one, `system dos4g` for a
  32-bit protected-mode one that loads DOS/4GW itself. `wcl hello.c` does it in one command.
- **Browser demos** (`web/`, GitHub Pages): a FreeDOS prompt where each line runs as
  `COMMAND.COM /C <cmd>`, plus the DJGPP and Watcom compiler pages. Headless tests:
  `web/test_node.mjs`, `test_bundle.mjs`, `test_shell.mjs`, `test_djgpp.mjs`,
  `test_watcom.mjs`.

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

**DJGPP compiles C, and the result runs.** `sh get_fixtures.sh djgpp` installs
djdev205 + gcc 3.4.6 + binutils 2.35.1 under `scratch_root/DJGPP`, and the loader puts
`DJGPP=A:\DJGPP\DJGPP.ENV` and its `bin` directory in the environment. Driving the
passes by hand:

    cc1.exe   hello.c -quiet -O2 -o hello.s
    as.exe    hello.s -o hello.o
    ld.exe    -o hello.cof A:/DJGPP/lib/crt0.o hello.o               -LA:/DJGPP/lib -LA:/DJGPP/lib/gcc/djgpp/3.46 -lc -lgcc
    stubify.exe hello.cof
    ./dosemu --root scratch_root scratch_root/hello.exe
    -> hello from DJGPP gcc, sum=55

That is a 32-bit protected-mode program, compiled from C by gcc inside the emulator,
running under the built-in DPMI host. `-lgcc` is not optional: printf pulls in
`__udivdi3`/`__umoddi3`.

**The driver works too, and the bug was a register leak across EXEC.** `gcc -O2
hello.c -o hello.exe` runs the whole chain — cc1, as, ld, and the final rename — and
the program it builds runs. So does `gpp -O2 mini.cpp -o mini.exe`, virtual functions
and templates included.

The child cc1 had been dying because `load_program` set `r[SP]` and left **`rhi[SP]`
holding the parent's ESP high half**. Sixteen-bit code cannot observe the upper halves
of EAX..EDI, so nothing had ever noticed; a DOS extender can. The child started with
SP=0x0760 but ESP=0x00200760, and because its stack segment was 16-bit, `PUSH` used SP
while `mov ecx,[esp+4]` addressed through the full ESP — a read 2 MiB from the write.
sbrk() read its own argument as garbage, asked DPMI for 2.1 GB, was refused, and the
whole child tore itself down. The same cc1 run directly was fine: nothing had run
before it to leave anything in `rhi[]`.

Found by narrowing, not by guessing: `DOSEMU_SAMPLE` showed the child spinning, the
DPMI trace showed the absurd 0501h size, `DOSEMU_TRACE=lo-hi` printed the 55
instructions that computed it, and `objdump` on cc1.exe at that address showed
`mov 0x4(%esp),%ecx` — at which point printing ESP alongside SP made it obvious.

**Four bugs, all the same shape.** Every one of these let the guest keep running on an
answer that was well-formed and wrong, so the damage surfaced far away:

- **PSP had no file-handle table.** Offsets 0x18 (the 20-byte JFT), 0x32 (count) and
  0x34 (far pointer) were never filled in. No 16-bit guest looks — a .COM gets
  DS=ES=PSP and keeps it — but DJGPP's `fstat` starts with
  `if (fhandle >= _farpeekw(_dos_ds, psp_addr + 0x32)) return -1;`, and against a zero
  every handle is out of range. gcc 4.7's cc1 says `hello.c: Bad file descriptor` about
  a file it just opened successfully.
- **`AH=52h` returned a zeroed scratch area.** DJGPP's fstat deliberately does not check
  CF here and walks the SFT chain from `LoL+4` until a next-pointer of `0xFFFF`. Zeros
  never terminate: cc1 spun there **forever**, at 20 M instructions/sec. It now returns a
  well-formed *empty* structure (SFT list = `0xFFFF:0xFFFF`), so `get_sft_entry` exits at
  once and returns -2, which fstat reads as "no SFT" and falls back to ordinary DOS
  calls. Inventing SFT contents would have been the same mistake in the other direction.
- **`AH=57h` (get file date) was missing** — it is fstat's *trusted* fallback once the
  SFT is unavailable. Now returns the same fixed timestamp FindFirst reports, so the two
  agree about a file.
- **`AH=5Bh` (create new file) was missing** — how a program claims a temporary name
  without a race. Without it the driver stops at `Cannot create temporary file`.

- **The descriptor cache did not survive clearing cr0.PE.** `lin()` recomputed
  `selector<<4` the instant PE dropped, but a 386 keeps the cached base until the next
  far jump — which is the whole mechanism behind unreal mode, and exactly what a DOS
  extender relies on when it switches back:

        mov cr0, eax         ; PE off, but CS still addresses through its cache
        jmp far real_cs:ip   ; only now does CS become a paragraph again

  `lin()` now always uses `sbase[]`, and applies the 1 MiB wrap only to a segment whose
  base is still `selector<<4` — that wrap is the A20 line, not segmentation: with the
  gate off address bit 20 does not exist and FFFF:0010 comes out at 0. A base that came
  from a descriptor is not a paragraph and must never be wrapped.

- **`INT 21h AH=56h` (rename) was missing** — the last step of every compile. The
  driver builds `hello.000` and renames it into place, so gcc did all the work and
  then threw it away with `rename ... failed`.
- **DPMI 0502h (free memory) was a no-op.** A bump allocator was fine while clients
  allocated once, but DJGPP's sbrk grows the heap by allocating a bigger block,
  copying, and freeing the old one — so every growth step leaked the previous heap and
  consumption went quadratic. There is a real first-fit allocator now, with adjacent
  free blocks merged. (Its first version took a `Block&` and then `push_back`ed into
  the same vector, which invalidated the reference and broke the C compile that had
  just started working. Index, not reference.)

**The x87 is implemented** (`src/fpu.cpp`): eight doubles as the register stack, exact
m32/m64/m80 and integer memory formats, compares through C0/C2/C3, FLDCW/FNSTSW/FNINIT.
Swallowing D8-DF as no-ops had been fine for LSI C, which does floating point in
software, but cc1 executes 35 x87 instructions during startup — and a no-op FPU is the
worst kind of stub, because the program computes with whatever was in the register and
carries on. `0x9B` (FWAIT) was also missing. Note that the FPU turned out *not* to be
what made cc1 abort; it was necessary but not sufficient.

**Diagnostics that earned their keep**, all in the shipped build:
`DOSEMU_SAMPLE=N` (print CS:EIP every N instructions — found the SFT loop in seconds),
`DOSEMU_OPHIST=1` (per-opcode counts; diffing a working program against a failing one
narrowed the suspects to seven instructions, five of them x87),
`DOSEMU_WATCH=lo-hi` (data accesses in a linear range, in `lin()` so it catches string
ops), `DOSEMU_MEMCHK=1` (accesses past the end of RAM instead of wrapping at `kMask`),
`DOSEMU_STATS=1`, `DOSEMU_FPU_TRACE=1`, and file paths in the DPMI trace.

For the record, the interpreter is **21.7 M instructions/sec** (the whole LSI C pass
chain, 5.26 M instructions, in 0.24 s). When something takes minutes it is looping, not
slow — measure before optimising.

**The browser demos.** Three pages, all static, all keeping everything client-side:

- `web/index.html` — the 16-bit one: FreeDOS COMMAND.COM, LSI C-86, `lsic.tar.gz`.
- `web/djgpp.html` — the 32-bit one: DJGPP **gcc 3.4.6** compiles C or C++ from a text
  area to a protected-mode `.EXE`, which the emulator then runs. `djgpp346.tar.gz` is
  7.9 MB and is fetched only when you press the button; it is gunzipped with
  `DecompressionStream` so no library is needed.
- `web/watcom.html` — **Watcom C/C++ 11.0c**, compiler *and* linker: `wcc`/`wpp` writes a
  `.obj`, `wlink` links it into a DOS `.EXE`, the emulator runs it, and the page will show
  you the link map. `watcom.tar.gz` is 1.7 MB and the whole round trip is about a second
  for either language. Three DOS programs deep, and each one uses a different part of the
  emulator: the compilers run under FlashTek X-32 (not a DPMI client — its own IDT, ring
  3), the linker under DOS/4GW (a DPMI client reflecting through `INT 31h 0302h`), and
  what they build is plain 16-bit real mode. `sh make_watcom_bundle.sh` rebuilds the
  bundle; `web/WATCOM-LICENSE.md` records what is in it.

The page drives the four passes itself — `cc1` → `as` → `ld` → `stubify` — rather than
using the `gcc` driver, for two reasons. 3.4.6's driver cannot exec `cc1` out of
`libexec/` here, and 4.7.4 (whose driver does work, and is what
`web/djgpp.tar.gz`/`get_fixtures.sh` install for native use) is three times the size
and roughly a minute per file. Driving the passes is 2.0 s for C and 3.5 s for C++ in
wasm, and each stage leaves an artefact worth showing — the page has a button for the
generated assembly.

Two link-order details the page bakes in, both learned the hard way:
`-lgcc` goes on **both** sides of `-lc` (the first copy satisfies `__register_frame_info`
so libc's `rfinfo.o` is never pulled in and cannot clash with libgcc's unwinder; the
second provides the 64-bit division helpers libc's `printf` needs), and C++ additionally
needs `-lstdcxx -lsupcxx` ahead of them.

`node web/test_djgpp.mjs` is the same sequence without a browser, and is the way to
check the page after touching the emulator: it unpacks the bundle into MEMFS, builds
both languages and asserts on what the programs print. The tar reader has to follow
hard links — `cc1` is stored once and linked into `bin/`.

### The x87 was wrong twice, and both were invisible

The FPU stopped being a no-op some time ago, but nothing had run a program that
*computed* with it until now. Two bugs, and the interesting thing is that neither
looked like an FPU bug from the outside.

**Integer stores ignored the control word's rounding mode.** `FIST`/`FISTP` always
rounded to nearest. A C cast to int has to truncate, and the way a compiler gets that is
to set RC=11 around the store — so `(long)0.666` came out as 1. It is also how
`printf("%f")` splits a value, which is why `0.841471` printed as `1.000000`: nothing
about that output says "rounding mode", it says "arithmetic is broken".

**`FUCOMPP` (DA E9) was missing.** The unordered compare-and-pop-both, which a C library
uses because it must not fault on a NaN. Without it the compare flags held the *previous*
comparison, so printf's digit loop branched wrongly and emitted a fraction of all zeros.
The symptom was `(long)(q*1e6)` giving exactly `666666` while `printf("%f", q)` gave
`0.000000` — provably correct arithmetic and wrong formatting, which is a strange place to
end up and is what a missing compare looks like.

The lesson is the one at the top of `src/fpu.cpp`, now with evidence: an FPU instruction
that silently does nothing is worse than one that faults, because the program carries on
computing with whatever was in the register.

**Fixed.** Every DJGPP program used to open with `Warning: Coprocessor not present and
DPMI setup failed!`. It was `0303h` (allocate real-mode callback), which is how DJGPP
hooks the FPU emulator; that is implemented and the warning is gone. `0E01h` (set
coprocessor emulation) and `0507h` are still unimplemented and still do not matter —
the client only probes them.

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

## ⏳ OpenWatcom — compilers work, DOS/4GW starts, the linker does not finish

**OPENWATCOM.md is the full account** and is current. Short version: `wcc386`, `wcc` and
`wpp386` compile C and C++ to `.obj` inside the emulator (`sh get_fixtures.sh ow`, then
`./dosemu --root ow ow/binw/wcc386.exe hello.c`). `wlink` is stubbed for DOS/4GW; DOS/4GW
now starts, prints its own banner, loads the 32-bit image and runs it for a quarter of a
million instructions before going off the rails.

### PICK UP HERE — one bug, diagnosed, with the fix sketched and a warning

The failure is exact and reproducible:

    ./dosemu --root ow ow/binw/wlink.exe "system dos file hello.obj name hello.exe"

    DOS/4GW Protected Mode Run-time  Version 1.97
    Copyright (c) Rational Systems, Inc. 1990-1994
    [cpu] exception INT 03h at 0000:000000CD (unhandled)
    dosemu: unimplemented instruction (opcode 0x27) at 0237:0083  [252970 instructions]

At instruction 251,479 the application issues `INT 31h AX=0302h`. DOS/4GW's own
protected-mode handler takes it (we dispatch there — `[dpmi] pm int 31 ax=0302 -> client
0157:000000C4`), does its translation, and **chains to the host's default handler**, which
is one of the 256 one-byte stubs in the entry paragraph. That lands in
`Dpmi::pm_int_default()`, which does:

    in_default_ = true;  real_int(n);  in_default_ = false;      // service the interrupt
    pop EIP / CS / EFLAGS from the client's stack; set CS:IP     // then emulate the IRET

and `real_int(n)` for 0302h reaches `Dpmi::rm_call()`, which **transfers control**: it
drops the machine to real mode, points CS:IP at the real-mode procedure and returns,
expecting the guest to run next. The unwind afterwards then pops three dwords off the
*real-mode* stack and jumps to what it finds — `CS = 0, EIP = 0x51`, and 0x51 is
`kOffRmRet`, the return offset `rm_call` had just pushed. Everything after that is a guest
executing the interrupt vector table. You can watch the moment at 251,694:

    DOSEMU_TRACE=251660-251705 ./dosemu --root ow ow/binw/wlink.exe "system dos file hello.obj name hello.exe"
    [t]251694 0157:0000094E 1F ...        pop ds
    [t]251695 0157:00000950 CF ...        iretd   -> 0000:00000051

**This is the third instance of one mistake**, so it is worth naming as a rule: *a host
routine that transfers control must not have its caller unwind afterwards.* The first was
`SegAlias` restoring a segment register after 0302h had loaded it for real mode (fixed by
not aliasing INT 31h at all). The second is this one. Anything else that calls into
`int31()` and then adjusts machine state is suspect.

**The obvious fix does not work as written.** Reordering `pm_int_default` to unwind first
and service second is more truthful — a service acts on the client's context and returns to
it — and it compiles and keeps every regression green, but the linker then *hangs* instead
of derailing (fewer than 20 M instructions in 45 s, so it is stuck in host code or in a
tight loop involving it). It is reverted; `git log` has it in the message of the ARPL
commit only, not in the tree. Diagnose the hang before adopting the reorder: the likely
issue is that `rm_call`'s snapshot then captures the client's context, so `rm_return`
restores the client rather than the stub, and something re-enters.

The alternative shape, probably better: give `rm_call()` a way to say "I transferred
control" (a flag on `Dpmi`), have `pm_int_default()` skip its unwind when it is set, and
have `rm_return()` perform the unwind that `pm_int_default()` would have done. That keeps
each routine responsible for the state it created.

### What was fixed getting here (all on `main`, all regression-green)

In the order they were found, because each one uncovered the next:

1. **Memory is paged** — X-32 works at linear 256 MiB and a 64 MiB flat array folded every
   address back through the size mask.
2. **A guest that loads its own IDT gets its own handler** — X-32 is not a DPMI client and
   reflects DOS calls itself.
3. **Privilege-changing interrupt gates** — X-32 runs the application at CPL 3, so the gate
   switches to the ring-0 stack out of the TSS and the far return pops it back.
4. **`POP` computes its address after the pop** — X-32's handler rewrites its own interrupt
   frame with `pop dword [esp+8]`.
5. **An environment block belongs below the program**, and **conventional memory is a block
   list with a real MCB chain** — an extender sizes itself by asking for everything and
   releasing what it does not need, and it walks the chain off `AH=52h` to plan.
6. **DPMI 0303h/0304h (real-mode callbacks), 0305h, 0306h, 0301h/0302h** — 0303h is also
   why every DJGPP program used to apologise about the FPU; that warning is gone.
7. **Protected-mode interrupts reach the handler that hooked them**, with the frame width
   taken from the *client's* declared bitness and a real default handler for 0204h to name.
8. **`LAR` does not report the present bit** — an empty descriptor slot is accessible with
   access rights zero, which is how a program finds a free one.
9. **`INT 16h AH=01h` stopped claiming a key was waiting**, which had sent DOS/4GW into a
   blocking read with no output at all.
10. **`XLAT`, `INS`/`OUTS`, `ARPL`** implemented; **DPMI selectors are RPL 3** because the
    descriptors always said DPL 3 and a client reads its own CPL off CS.

### Tooling notes worth keeping

- **`DOSEMU_WATCH` now covers `Memory` writes** (tagged `[write]`), not just accesses
  through a segment register. It used to be blind to the entire DOS layer and to the host's
  own writes, and its *silence* read as evidence — two hours went into a conclusion drawn
  from exactly that.
- **A REP that copies more than 16 MiB reports itself.** A whole REP runs inside one
  `step()`, so a runaway count is invisible: the emulator looks wedged while the sample
  trace insists it is executing normally.
- The trace that settled the hardest question was one line at the entry to
  `Cpu::interrupt`, printing `ds=%04X(%08X)` — selector *and* cached base. A register
  disagreeing with its own cache narrows the suspects to code that writes one without the
  other.

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
