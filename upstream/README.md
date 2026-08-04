# `upstream/` — the toolchain archives exactly as downloaded

The unmodified `.zip` files, kept the same way `lsic330c.lzh` is kept at the repo root:
the original, before anything was unpacked, pruned or rearranged. Two toolchains live
here — DJGPP first, then OpenWatcom.

## DJGPP

From <http://www.delorie.com/pub/djgpp/current/>.

| file | from | contents |
|---|---|---|
| `djdev205.zip` | `v2/` | DJGPP runtime: libc, headers, `crt0.o`, `stubify` |
| `bnu2351b.zip` | `v2gnu/` | binutils 2.35.1 — `as`, `ld` |
| `gcc474b.zip` | `v2gnu/` | gcc 4.7.4 — `gcc`, `cpp`, `cc1` |
| `gpp474b.zip` | `v2gnu/` | g++ 4.7.4 — `gpp`, `cc1plus`, libstdc++ |
| `gcc346b.zip` | `v2gnu/` | gcc 3.4.6 — a third the size, and much faster |
| `gpp346b.zip` | `v2gnu/` | g++ 3.4.6 |

    sha256
    4557dfb6c161d326680ae5fa71f0098ac49425a1b11b90a020b83162eb705dda  djdev205.zip
    caa2160831563ec743eb73cf97f745e3fe871d91284c0170e38091b872cea32f  bnu2351b.zip
    cc91a9cb80b1f781855d1d84f07bb172cebf0ee40303a088f33e36d62a5a4d47  gcc474b.zip
    b4b936ee280a8f762b05d1b129229cb3fd7cb44fffb636945fb118d6b87668ea  gpp474b.zip
    05a85a3166a5f96cfd3bfe10c3515e28b1382f776727d30b6060dc8e8210bb2b  gcc346b.zip
    68cad40c49cbb2e1f2fef06bc26ffeddf159c67325e736ea604a95c82c832042  gpp346b.zip

## Relationship to `web/djgpp.tar.gz`

`web/djgpp.tar.gz` is what these four unpack into, minus `info/`, `man/`, `gnu/`,
`share/` and the binutils tools the compile path never runs, plus the two adjustments
described in `web/DJGPP-LICENSE.md` (the `bin/` hard links and the `c++config.h`
aliases). `sh make_djgpp_bundle.sh` rebuilds the tarball from these archives, so the
derived file can always be checked against the originals.

**Both versions are here, and both are used, for different things.**

- **4.7.4** is the one whose *driver* works: `gcc hello.c -o hello.exe` runs the whole
  chain by itself. It is also large (cc1 is 12 MB) and slow — 51 s for a C file, 2 min
  41 s for a C++ one, natively.
- **3.4.6** is a third of the size and far quicker, and its `cc1`/`cc1plus` compile
  correctly — but its driver cannot exec `cc1` out of `libexec/` under this emulator,
  and with `cc1` on `PATH` instead the compiler aborts. Driving the four passes
  (`cc1` → `as` → `ld` → `stubify`) by hand works fine, which is what the browser demo
  does; it also makes the stages visible, which is the more interesting thing to show.

## OpenWatcom (Watcom C/C++ 11.0c) and DOS/4GW

The DOS-hosted tools, as the component zips from
<https://github.com/open-watcom/open-watcom-1.9/releases/tag/w11.0c-zips>. Unzipping all
eleven into one tree gives the `ow/` layout (`binw/`, `h/`, `lib286/`, `lib386/`) that
`--root ow` expects; about 8 MB. Far cheaper than the OpenWatcom v2 installers, which
are 128 MB each *and* are themselves DOS-extended programs, so unpacking one would need
the thing we are trying to build.

| file | contents |
|---|---|
| `core_binw.zip` | the DOS-hosted core: `wlink`, `wasm`, `wlib`, `wmake` |
| `core_all.zip` | host-independent parts — including `w32run.exe`, the X-32 loader |
| `c_binw.zip` | `wcc` (16-bit C) and `wcc386` (32-bit C) |
| `cpp_binw.zip` | `wpp` and `wpp386` (C++) |
| `clib_hdr.zip` | C headers (`h/`) |
| `clib_d32.zip` `clib_d16.zip` | C libraries, 32- and 16-bit DOS |
| `clib_o16.zip` | 16-bit startup objects |
| `cpplib_hdr.zip` | C++ headers |
| `cpplib_o32.zip` `cpplib_o16.zip` | C++ libraries |
| `dos4gw.zip` | DOS/4GW 1.95, which `wlink.exe`'s own stub loads |

    sha256
    27d3d51582ac3e2db7bef79d55dc41cccc5c0c7dbc5a028344b540ea2a3c9c2f  core_binw.zip
    919643a6241f6ff8089a92bbd3c33ac1dbfc02b12d550f318737b0b8716185b8  core_all.zip
    2006318b9f8d36e7182a5a5af28cea4322e743aa730544853ff82167ecaabf64  c_binw.zip
    18017777a0a49143ce4b850acd329b7d34f49aa175d889845dc312c0dee48bcf  cpp_binw.zip
    9c92b0350e26e3e120b8900481bb72f8b8e4f337615119ad7b22a8c6c83839c9  clib_hdr.zip
    1055f79e01b0f678eba60c74336d797aeb1a4209f37405cee27d4dba253b8ebd  clib_d32.zip
    34603d74774fe190da943c00fc408564d83fce13ff12496ff885b5a254d83a48  clib_d16.zip
    4f703b12f51896ed4614eef850573c2209e42f5c11ba5cb1dedaa925d959b5e8  clib_o16.zip
    3a60bf3cdf569c371031081bdba49f097f00e6f88660f8eecfcbf4d3a7e10a50  cpplib_hdr.zip
    5995aa9b59dcdca373ab1c01d28052d12d9ac6e6f18a29abd6bbdc2d7761d391  cpplib_o32.zip
    5382793bb7787aed283cbdf37e0f1c7f3e715c839361c470c69f6ba1ec292104  cpplib_o16.zip
    7b1ce37629319b3e228dcb3161d437989e83eb10a5ce61dfe21b5c54c23b7df6  dos4gw.zip

**Why `dos4gw.zip` comes from somewhere else.** The compilers are stubbed for FlashTek
X-32 and load `w32run.exe`, which is in `core_all.zip`. `wlink.exe` is stubbed for
DOS/4GW instead and loads `dos4gw.exe`, which is *not* in any of the component zips —
it is Tenberry Software's, licensed to Watcom rather than owned by it, so the Open
Watcom source release could not include it. This copy is the one redistributed with
uCON64 (<https://sourceforge.net/projects/ucon64/files/ucon64misc/>), DOS/4GW 1.95,
dated 1995-07-11.

Unpack it as `ow/binw/dos4gw.exe` (lower case; that is the name the stub searches
`PATH` for). See `OPENWATCOM.md` for how far the linker gets.

## Licence

gcc, g++ and binutils are GPL, and these are binary distributions. The matching
sources are the `s`-suffixed archives in the same upstream directories —
`gcc474s.zip`, `gpp474s.zip`, `bnu2351s.zip` — and `v2/djlsr205.zip` for DJGPP's own
libc. `web/DJGPP-LICENSE.md` has the full URLs and the rest of the terms.

Watcom C/C++ is under the Sybase Open Watcom Public License; `ow/license.txt` (from
`core_all.zip`) carries the text, and the sources are the Open Watcom 1.9 release in the
same GitHub repository. DOS/4GW is Tenberry Software's and is redistributable with
programs built by the Watcom tools; it is included here for exactly that purpose.
