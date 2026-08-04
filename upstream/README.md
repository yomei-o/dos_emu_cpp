# `upstream/` — the DJGPP archives exactly as downloaded

The unmodified `.zip` files from <http://www.delorie.com/pub/djgpp/current/>, kept the
same way `lsic330c.lzh` is kept at the repo root: the original, before anything was
unpacked, pruned or rearranged.

| file | from | contents |
|---|---|---|
| `djdev205.zip` | `v2/` | DJGPP runtime: libc, headers, `crt0.o`, `stubify` |
| `bnu2351b.zip` | `v2gnu/` | binutils 2.35.1 — `as`, `ld` |
| `gcc474b.zip` | `v2gnu/` | gcc 4.7.4 — `gcc`, `cpp`, `cc1` |
| `gpp474b.zip` | `v2gnu/` | g++ 4.7.4 — `gpp`, `cc1plus`, libstdc++ |

    sha256
    4557dfb6c161d326680ae5fa71f0098ac49425a1b11b90a020b83162eb705dda  djdev205.zip
    caa2160831563ec743eb73cf97f745e3fe871d91284c0170e38091b872cea32f  bnu2351b.zip
    cc91a9cb80b1f781855d1d84f07bb172cebf0ee40303a088f33e36d62a5a4d47  gcc474b.zip
    b4b936ee280a8f762b05d1b129229cb3fd7cb44fffb636945fb118d6b87668ea  gpp474b.zip

## Relationship to `web/djgpp.tar.gz`

`web/djgpp.tar.gz` is what these four unpack into, minus `info/`, `man/`, `gnu/`,
`share/` and the binutils tools the compile path never runs, plus the two adjustments
described in `web/DJGPP-LICENSE.md` (the `bin/` hard links and the `c++config.h`
aliases). `sh make_djgpp_bundle.sh` rebuilds the tarball from these archives, so the
derived file can always be checked against the originals.

Only the 4.7.4 compilers are kept. gcc/g++ **3.4.6** was tried first because it is a
third of the size, and its `cc1` does compile correctly when run directly — but its
*driver* cannot exec `cc1` out of `libexec/` under this emulator, and with `cc1` on
`PATH` instead the compiler aborts. 4.7.4 works end to end, so that is what ships. The
3.4.6 archives are `gcc346b.zip` / `gpp346b.zip` from the same directory if anyone
wants to chase that.

## Licence

gcc, g++ and binutils are GPL, and these are binary distributions. The matching
sources are the `s`-suffixed archives in the same upstream directories —
`gcc474s.zip`, `gpp474s.zip`, `bnu2351s.zip` — and `v2/djlsr205.zip` for DJGPP's own
libc. `web/DJGPP-LICENSE.md` has the full URLs and the rest of the terms.
