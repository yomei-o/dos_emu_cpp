#!/bin/sh
# Rebuild web/watcom.tar.gz -- the Watcom C/C++ toolchain the browser demo runs.
#
#   sh make_watcom_bundle.sh
#
# Like web/djgpp.tar.gz, this is a derived file, and it should be reproducible from the
# originals rather than being something someone once assembled by hand. Everything it
# contains is listed in web/WATCOM-LICENSE.md.
#
# The bundle is the smallest tree that compiles *and links* both languages: two
# compilers, the linker, the two loaders the compilers and the linker are stubbed for,
# the headers, and the four 16-bit DOS libraries. 8 MB of ow/ down to about 4 MB, which
# gzips to under 2.
#
# Sixteen-bit, not 32-bit, and that is a deliberate choice rather than a limitation of
# the emulator: the 32-bit libraries are all here in ow/ and wcc386 works, but a 32-bit
# link needs math387r.lib, which the w11.0c component zips do not ship. The 16-bit path
# is also the smaller download and the faster compile.
set -e
cd "$(dirname "$0")"

for f in core_binw core_all c_binw cpp_binw clib_hdr cpplib_hdr clib_d16 dos4gw; do
    [ -f "upstream/$f.zip" ] || { echo "missing upstream/$f.zip -- see upstream/README.md"; exit 1; }
done

# The C++ runtime is the one thing the w11.0c component zips cannot supply:
# `cpplib_o16.zip` turns out to hold only the OS/2 build of it, and there is no
# math87s.lib anywhere in the eleven either. Both are in the OpenWatcom v2 DOS installer,
# which is an ordinary zip file, so take them from there when it is around.
#
# And when they come from v2, clibs.lib has to as well. Mixing v2's plibs.lib with
# 11.0c's C library gets as far as `plibs.lib(fatalerr.cpp): undefined symbol
# __clib_fatal_` -- an internal name that moved between the two releases. The libraries
# are one set; the *compilers* and the linker stay at 11.0c, because v2's own DOS
# binaries do not run here yet (their DOS/4G stub builds its PATH search wrong).
#
# Without the installer the bundle still builds and C still works end to end; C++
# compiles to an object and stops at the link. See web/WATCOM-LICENSE.md.
V2=""
for c in upstream/ow-dos.exe ow/ow-dos.exe; do [ -f "$c" ] && V2="$c" && break; done

TMP=build_watcom
rm -rf "$TMP"
mkdir -p "$TMP/OW"

unpack() {                                  # unpack() zip path...
    z=$1; shift
    unzip -oq "upstream/$z.zip" "$@" -d "$TMP/OW"
}

echo "unpacking the compilers, the linker and the two loaders"
unpack core_binw  'binw/wlink.exe' 'binw/wlink.lnk'
unpack core_all   'binw/w32run.exe' 'binw/wlsystem.lnk' 'license.txt'
unpack c_binw     'binw/wcc.exe'
unpack cpp_binw   'binw/wpp.exe'
echo "unpacking the headers"
unpack clib_hdr   'h/*'
unpack cpplib_hdr 'h/*'
if [ -z "$V2" ]; then
    echo "unpacking the 16-bit DOS C library"
    unpack clib_d16 'lib286/dos/clibs.lib' 'lib286/dos/emu87.lib'
fi

# DOS/4GW is not in the component zips (Tenberry's, not Watcom's) and the stub searches
# PATH for it in lower case.
unzip -oq upstream/dos4gw.zip -d "$TMP/OW/binw"
[ -f "$TMP/OW/binw/DOS4GW.EXE" ] && mv "$TMP/OW/binw/DOS4GW.EXE" "$TMP/OW/binw/dos4gw.exe"
rm -f "$TMP/OW/binw/DOS4GW.DOC" "$TMP/OW/binw/dos4gw.doc"

if [ -n "$V2" ]; then
    echo "taking the 16-bit DOS libraries from $V2 (OpenWatcom v2)"
    mkdir -p "$TMP/OW/lib286/dos"
    unzip -oqj "$V2" 'lib286/dos/clibs.lib' 'lib286/plibs.lib' 'lib286/math87s.lib' \
                     'lib286/emu87.lib' -d "$TMP/OW/lib286/dos"
else
    echo "no OpenWatcom v2 installer found: C++ will compile but not link"
fi

# wlink reads `system` definitions from wlink.lnk, which it looks for by bare name in the
# current directory and nowhere else -- one open call, measured. The guest starts at the
# root of the drive, so that is where both files go. wlink.lnk `@`-includes wlsystem.lnk
# through %WATCOM%, which the loader sets to A:\.
cp "$TMP/OW/binw/wlink.lnk" "$TMP/OW/binw/wlsystem.lnk" "$TMP/OW/"

# The samples the demo page starts with. CRLF, because a DOS compiler reads DOS text.
crlf() { sed 's/$/\r/' > "$1"; }

crlf "$TMP/OW/hello.c" <<'CEOF'
#include <stdio.h>

int main(void)
{
	int i, s = 0;
	for (i = 1; i <= 10; i++)
		s += i;
	printf("hello from Watcom C, sum=%d\n", s);
	return 0;
}
CEOF

crlf "$TMP/OW/hello.cpp" <<'XEOF'
#include <stdio.h>

struct Greeter {
	const char* who;
	Greeter(const char* w) : who(w) {}
	virtual ~Greeter() {}
	virtual void hello(int n) const { printf("hello from %s, sum=%d\n", who, n); }
};

struct Loud : Greeter {
	Loud(const char* w) : Greeter(w) {}
	void hello(int n) const { printf("HELLO FROM %s, SUM=%d\n", who, n); }
};

static int triangle(int n)
{
	int s = 0;
	for (int i = 1; i <= n; ++i) s += i;
	return s;
}

int main()
{
	Greeter a("Watcom C++");
	Loud b("Watcom C++");
	const Greeter* v[2] = { &a, &b };
	for (int i = 0; i < 2; i++) v[i]->hello(triangle(10));
	return 0;
}
XEOF

cd "$TMP/OW"
tar czf ../../web/watcom.tar.gz .
cd ../..
rm -rf "$TMP"
ls -l web/watcom.tar.gz
echo "done. Check with: node web/test_watcom.mjs"
