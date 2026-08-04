#!/bin/sh
# Reconstitute the gitignored fixtures a fresh clone needs to run the tests.
#
#   sh get_fixtures.sh          # lsic/ + scratch_root/ from the committed bundle
#   sh get_fixtures.sh djgpp    # ...and download DJGPP (needs network)
#
# Nothing here is committed: lsic/ is the LSI C-86 tree, scratch_root/ is the drive the
# native emulator runs against, djgpp/ is a third-party download. But web/lsic.tar.gz IS
# committed and contains the whole LSI C tree plus COMMAND.COM, so the first two need no
# network and no LZH extractor — which is otherwise the blocker, since lsic330c.lzh in
# the repo root needs a tool most machines do not have.
set -e
cd "$(dirname "$0")"

echo "== lsic/ and scratch_root/ from web/lsic.tar.gz"
mkdir -p scratch_root
tar xzf web/lsic.tar.gz -C scratch_root
rm -rf lsic
cp -r scratch_root/LSIC86 lsic

# A C program for the compile-and-run regression. Written from the shell rather than
# committed because it must have DOS line endings and a literal backslash-n in the
# source; getting that wrong produces `missing "` from the LSI C front end, not a
# useful error.
printf '#include <stdio.h>\r\n\r\nint main(void)\r\n{\r\n\tint i, s = 0;\r\n\tfor (i = 1; i <= 10; i++) s += i;\r\n\tprintf("sum=%%d\\n", s);\r\n\treturn 0;\r\n}\r\n' > scratch_root/PROG.C
echo "   wrote scratch_root/PROG.C"

if [ "$1" = "djgpp" ]; then
    echo "== djgpp/ (djdev205.zip from delorie.com)"
    mkdir -p djgpp
    if [ ! -f djgpp/djdev205.zip ]; then
        curl -fL --max-time 300 -o djgpp/djdev205.zip \
            http://www.delorie.com/pub/djgpp/current/v2/djdev205.zip
    fi
    # unzip is not always present on Windows shells; python is.
    if command -v unzip >/dev/null 2>&1; then
        (cd djgpp && unzip -oq djdev205.zip)
    else
        python -c "import zipfile;zipfile.ZipFile('djgpp/djdev205.zip').extractall('djgpp')"
    fi
    for p in djecho stubify dtou getconf; do cp djgpp/bin/$p.exe scratch_root/; done
    echo "   djecho/stubify/dtou/getconf -> scratch_root/ (i386 COFF; djecho entry 0x18B0)"
fi

cat <<'EOF'

done. Now:
  sh build.sh
  ./dosemu --root scratch_root scratch_root/LSIC86/BIN/LCC.EXE PROG.C   # then run PROG.exe -> sum=55
  node web/test_shell.mjs && node web/test_bundle.mjs && node web/test_node.mjs
  ./dosemu --root scratch_root scratch_root/stubify.exe                 # a 32-bit DJGPP program
EOF
