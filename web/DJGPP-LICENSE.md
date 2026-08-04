# `web/djgpp.tar.gz` — what is in it, and under what terms

A ready-to-run DJGPP toolchain for the emulated drive, committed the same way
`web/lsic.tar.gz` is, so a clone can compile C and C++ with no downloads:

    mkdir -p scratch_root && tar xzf web/djgpp.tar.gz -C scratch_root
    ./dosemu --root scratch_root scratch_root/DJGPP/bin/gcc.exe -O2 hello.c   -o hello.exe
    ./dosemu --root scratch_root scratch_root/DJGPP/bin/gpp.exe -O2 mini.cpp  -o mini.exe

## Contents

Unmodified binaries from the official DJGPP distribution at
<http://www.delorie.com/pub/djgpp/current/>:

| package | what it provides |
|---|---|
| `v2/djdev205.zip` | libc, headers, `crt0.o`, `stubify` |
| `v2gnu/bnu2351b.zip` | binutils 2.35.1 — `as`, `ld` |
| `v2gnu/gcc474b.zip` | gcc 4.7.4 — `gcc`, `cpp`, `cc1` |
| `v2gnu/gpp474b.zip` | g++ 4.7.4 — `gpp`, `cc1plus`, libstdc++ |

Two changes, neither of them to any binary:

- **Pruned**: `info/`, `man/`, `gnu/`, `share/` and the binutils tools the compile
  path never runs (objdump, readelf, …). Nothing on the compile path is missing.
- **`cc1`, `cc1plus` and `collect2` are hard-linked into `bin/`** as well as
  `libexec/`. The driver computes its library search paths from the `libexec`
  location but execs the compiler proper through `PATH`, so it needs both. They are
  hard links, so the archive stores each only once.

Also, four headers are duplicated under their upstream names:
`bits/c++config.h`, `c++allocator.h`, `c++io.h`, `c++locale.h`. DJGPP ships them as
`cxxconfig.h` etc. because `+` is not a legal DOS 8.3 character, and relies on long
filename support to bridge the two. This emulator reports no LFN (INT 21h AH=71h),
so `<cstdio>`'s `#include <bits/c++config.h>` would not resolve. Copying rather than
renaming keeps both names working.

## Licence

**gcc, g++ and binutils are GPL.** They are redistributed here under the GPL, which
obliges whoever distributes the binaries to make the corresponding source available.
The exact matching sources are the `s` (source) packages next to the binaries in the
same distribution:

- <http://www.delorie.com/pub/djgpp/current/v2gnu/gcc474s.zip>
- <http://www.delorie.com/pub/djgpp/current/v2gnu/gpp474s.zip>
- <http://www.delorie.com/pub/djgpp/current/v2gnu/bnu2351s.zip>
- <http://www.delorie.com/pub/djgpp/current/v2/djlsr205.zip> (DJGPP's own libc)

DJGPP's own runtime (djdev) is under DJ Delorie's terms; see `copying.dj` inside the
archive. `stubify` states it plainly: *"It is redistributable but only as part of a
complete package. If you have a copy of this program, the place that you got it from
is responsible for making sure you are able to get its sources as well."* — that is
what this file is for.

If you would rather not carry 20 MB of someone else's GPL binaries in your history,
`sh get_fixtures.sh djgpp` downloads and assembles exactly the same tree from
delorie.com instead.
