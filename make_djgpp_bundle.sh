#!/bin/sh
# Rebuild web/djgpp.tar.gz from the archives in upstream/.
#
#   sh make_djgpp_bundle.sh
#
# The tarball is a derived file, so it should be reproducible from the originals
# rather than being something that only exists because someone once assembled it by
# hand. Everything this does beyond unpacking is listed in web/DJGPP-LICENSE.md.
#
#   sh make_djgpp_bundle.sh          # both: web/djgpp.tar.gz and web/djgpp346.tar.gz
#   sh make_djgpp_bundle.sh 474      # only the big one
#   sh make_djgpp_bundle.sh 346      # only the small one
#
# Two bundles because the two versions are good at different things. 4.7.4 has the
# working driver; 3.4.6 is a third of the size and much faster, and the browser demo
# drives its passes itself. See upstream/README.md.
set -e
cd "$(dirname "$0")"

build() {                       # $1 = 474 | 346
    V=$1
    case "$V" in
        474) GCC=gcc474b; GPP=gpp474b; D=4.74; OUT=web/djgpp.tar.gz ;;
        346) GCC=gcc346b; GPP=gpp346b; D=3.46; OUT=web/djgpp346.tar.gz ;;
        *)   echo "unknown version $V"; exit 1 ;;
    esac
    for f in djdev205 bnu2351b "$GCC" "$GPP"; do
        [ -f "upstream/$f.zip" ] || { echo "missing upstream/$f.zip"; exit 1; }
    done

    TMP=build_djgpp
    rm -rf "$TMP"
    mkdir -p "$TMP/DJGPP"
    for f in djdev205 bnu2351b "$GCC" "$GPP"; do
        echo "unpacking $f"
        python -c "import zipfile,sys;zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" \
            "upstream/$f.zip" "$TMP/DJGPP"
    done

    cd "$TMP/DJGPP"

    # Documentation and the binutils tools the compile path never runs. Nothing that
    # gcc, g++, as, ld or stubify reaches for is removed.
    rm -rf info man gnu share tmp
    for f in addr2line ar cxxfilt elfedit gprof nm objcopy objdump ranlib readelf size              strings strip gcov gcov-tool gcov-dump protoize unprotoize djtar djtart              djtarx djasm djmerge djsplit dxe3gen dxe3res dxegen edebug32 fsdb getconf              redir symify texi2ps update bin2h coff2exe dtou utod; do
        rm -f "bin/$f.exe"
    done

    # The driver computes its library search paths from where cc1 lives under libexec/,
    # but execs the compiler proper through PATH. It needs both locations. Hard links,
    # so the archive stores each binary once.
    for f in cc1 cc1plus collect2; do
        ln "libexec/gcc/djgpp/$D/$f.exe" "bin/$f.exe" 2>/dev/null ||             cp "libexec/gcc/djgpp/$D/$f.exe" "bin/$f.exe"
    done

    # DJGPP ships these under 8.3-legal names (`+` is not a valid DOS filename
    # character) and relies on long filename support to bridge the two. This emulator
    # reports no LFN, so <cstdio>'s own `#include <bits/c++config.h>` would not
    # resolve. Copy rather than rename, so both spellings work.
    B="include/cxx/$D/djgpp/bits"
    for f in cxxconfig cxxallocator cxxio cxxlocale; do
        t=$(echo "$f" | sed 's/^cxx/c++/')
        [ -f "$B/$f.h" ] && cp "$B/$f.h" "$B/$t.h"
    done

    cd ..
    tar czf "../$OUT" DJGPP
    cd ..
    rm -rf "$TMP"
    ls -l "$OUT"
}

case "${1:-both}" in
    474)  build 474 ;;
    346)  build 346 ;;
    both) build 474; build 346 ;;
    *)    echo "usage: $0 [474|346|both]"; exit 1 ;;
esac
echo "done. Check with: sh get_fixtures.sh djgpp"
