# `web/watcom.tar.gz` — what is in it, and under what terms

A ready-to-run Watcom C/C++ toolchain for the emulated drive, committed the same way
`web/lsic.tar.gz` and `web/djgpp.tar.gz` are, so a clone can compile *and link* C and C++
with no downloads:

    mkdir -p ow_demo && tar xzf web/watcom.tar.gz -C ow_demo
    ./dosemu --root ow_demo ow_demo/binw/wcc.exe   hello.c
    ./dosemu --root ow_demo ow_demo/binw/wlink.exe "system dos file hello.obj name hello.exe"
    ./dosemu --root ow_demo ow_demo/hello.exe          # hello from Watcom C, sum=55

    ./dosemu --root ow_demo ow_demo/binw/wpp.exe   hello.cpp
    ./dosemu --root ow_demo ow_demo/binw/wlink.exe "system dos file hello.obj name hellocpp.exe"
    ./dosemu --root ow_demo ow_demo/hellocpp.exe       # hello from Watcom C++, sum=55

`sh make_watcom_bundle.sh` rebuilds the tarball from the archives in `upstream/`, so the
derived file can always be checked against the originals. `node web/test_watcom.mjs` is
the same sequence in wasm, which is what `web/watcom.html` does in a browser.

## Contents

The 16-bit DOS path only — two compilers, the linker, the two loaders they are stubbed
for, the headers, and four libraries. That is 4 MB before compression, out of the 8 MB
`ow/` tree; the 32-bit compilers work too but a 32-bit link needs `math387r.lib`, which
none of the component zips ship.

Unmodified binaries from the Open Watcom 1.9 `w11.0c-zips` release,
<https://github.com/open-watcom/open-watcom-1.9/releases/tag/w11.0c-zips>:

| package | what this bundle takes from it |
|---|---|
| `c_binw.zip` | `binw/wcc.exe` — the 16-bit C compiler |
| `cpp_binw.zip` | `binw/wpp.exe` — the 16-bit C++ compiler |
| `core_binw.zip` | `binw/wlink.exe`, `binw/wlink.lnk` |
| `core_all.zip` | `binw/w32run.exe` (the FlashTek X-32 loader), `binw/wlsystem.lnk`, `license.txt` |
| `clib_hdr.zip` | `h/` — the C headers |
| `cpplib_hdr.zip` | `h/` — the C++ headers |

and DOS/4GW 1.95, which is not in any of them because it is Tenberry Software's rather
than Watcom's, from the copy redistributed with uCON64
(<https://sourceforge.net/projects/ucon64/files/ucon64misc/>), dated 1995-07-11.

## The four libraries come from OpenWatcom v2, and why

`lib286/dos/clibs.lib`, `plibs.lib`, `math87s.lib` and `emu87.lib` are from the
**OpenWatcom v2 DOS installer** (`ow-dos.exe`, itself an ordinary zip), not from the
w11.0c component zips. This is worth stating plainly because it is the one version mix in
the bundle:

- There is no DOS build of the C++ runtime in the w11.0c zips. `cpplib_o16.zip` and
  `cpplib_o32.zip` contain only `lib286/os2/` and `lib386/os2/` — the OS/2 builds. Without
  `plibs.lib` a C++ program compiles to an object and then stops at
  `___wcpp_4_data_init_fs_root_ is an undefined reference`.
- `math87s.lib` is not in the eleven either, so even a C link reports
  `W1008: cannot open math87s.lib`.
- And once `plibs.lib` comes from v2, `clibs.lib` has to as well. Mixing v2's C++ runtime
  with 11.0c's C library gets as far as `plibs.lib(fatalerr.cpp): undefined symbol
  __clib_fatal_` — an internal name that moved between the releases. The libraries are one
  set.

The **compilers and the linker stay at 11.0c**, because OpenWatcom v2's own DOS binaries
do not run under this emulator yet: their DOS/4G stub builds its `PATH` search without a
separator (`A:\BINWDOS4GW.EXE`) and gives up. That is an open question, noted in
`resume.md`.

Both releases are under the same licence, so this changes nothing about the terms — only
about provenance, which is why it is written down here rather than left to be discovered.

## Two additions, neither of them to a binary

- **`wlink.lnk` and `wlsystem.lnk` are copied to the root of the drive** as well as
  `binw/`. They are where `system dos` is defined, and `wlink` looks for `wlink.lnk` by
  bare name in the current directory and nowhere else — one `open` call, measured. Without
  it the linker has no system definitions at all and stops at
  `E3002: format not decided`.
- **`hello.c` and `hello.cpp`** are added as the samples the demo page starts with, with
  CRLF line endings, because a DOS compiler reads DOS text.

## Licence

Watcom C/C++ is under the **Sybase Open Watcom Public License**; the full text is
`license.txt` inside this bundle (from `core_all.zip`). The sources are the Open Watcom
1.9 release in the GitHub repository linked above, and the OpenWatcom v2 sources are at
<https://github.com/open-watcom/open-watcom-v2>.

**DOS/4GW** is Tenberry Software's. It is redistributable with programs built by the
Watcom tools, which is exactly what it is here for: `wlink.exe`'s own stub loads it.
