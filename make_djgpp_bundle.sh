#!/bin/sh
# Rebuild web/djgpp.tar.gz from the archives in upstream/.
#
#   sh make_djgpp_bundle.sh
#
# The tarball is a derived file, so it should be reproducible from the originals
# rather than being something that only exists because someone once assembled it by
# hand. Everything this does beyond unpacking is listed in web/DJGPP-LICENSE.md.
set -e
cd "$(dirname "$0")"

for f in djdev205 bnu2351b gcc474b gpp474b; do
    [ -f "upstream/$f.zip" ] || { echo "missing upstream/$f.zip"; exit 1; }
done

TMP=build_djgpp
rm -rf "$TMP"
mkdir -p "$TMP/DJGPP"
for f in djdev205 bnu2351b gcc474b gpp474b; do
    echo "unpacking $f"
    python -c "import zipfile,sys;zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" \
        "upstream/$f.zip" "$TMP/DJGPP"
done

cd "$TMP/DJGPP"

# Documentation and the binutils tools the compile path never runs. Nothing that
# gcc, g++, as, ld or stubify reaches for is removed.
rm -rf info man gnu share tmp
for f in addr2line ar cxxfilt elfedit gprof nm objcopy objdump ranlib readelf size \
         strings strip gcov gcov-tool gcov-dump protoize unprotoize djtar djtart \
         djtarx djasm djmerge djsplit dxe3gen dxe3res dxegen edebug32 fsdb getconf \
         redir symify texi2ps update bin2h coff2exe dtou utod; do
    rm -f "bin/$f.exe"
done

# The driver computes its library search paths from where cc1 lives under libexec/,
# but execs the compiler proper through PATH. It needs both locations. Hard links, so
# the archive stores each binary once.
for f in cc1 cc1plus collect2; do
    ln "libexec/gcc/djgpp/4.74/$f.exe" "bin/$f.exe" 2>/dev/null || \
        cp "libexec/gcc/djgpp/4.74/$f.exe" "bin/$f.exe"
done

# DJGPP ships these under 8.3-legal names (`+` is not a valid DOS filename character)
# and relies on long filename support to bridge the two. This emulator reports no LFN,
# so <cstdio>'s own `#include <bits/c++config.h>` would not resolve. Copy rather than
# rename, so both spellings work.
B=include/cxx/4.74/djgpp/bits
for f in cxxconfig cxxallocator cxxio cxxlocale; do
    t=$(echo "$f" | sed 's/^cxx/c++/')
    [ -f "$B/$f.h" ] && cp "$B/$f.h" "$B/$t.h"
done

cd ..
tar czf ../web/djgpp.tar.gz DJGPP
cd ..
rm -rf "$TMP"
ls -l web/djgpp.tar.gz
echo "done. Check it with: sh get_fixtures.sh djgpp"
