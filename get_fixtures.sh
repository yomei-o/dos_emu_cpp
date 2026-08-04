#!/bin/sh
# Reconstitute the gitignored fixtures a fresh clone needs to run the tests.
#
#   sh get_fixtures.sh          # lsic/ + scratch_root/ from the committed bundle
#   sh get_fixtures.sh djgpp    # ...and the DJGPP C/C++ toolchain too
#   sh get_fixtures.sh ow       # ...or the OpenWatcom one, unpacked into ow/
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

# A source file for the guest has to have CRLF endings and a literal backslash-n in
# it. Writing that with printf escapes is how PROG.C got mangled twice; a heredoc plus
# one sed is unambiguous.
crlf() { sed 's/$/\r/' > "$1"; }

if [ "$1" = "djgpp" ]; then
    echo "== DJGPP toolchain from the committed bundle (no network needed)"
    tar xzf web/djgpp.tar.gz -C scratch_root
    echo "   scratch_root/DJGPP: gcc + g++ 4.7.4, binutils 2.35.1 ($(du -sh scratch_root/DJGPP | cut -f1))"
    echo "   licence and provenance: web/DJGPP-LICENSE.md"

    crlf scratch_root/hello.c <<'CEOF'
#include <stdio.h>

int main(void)
{
	int i, s = 0;
	for (i = 1; i <= 10; i++)
		s += i;
	printf("hello from DJGPP gcc, sum=%d\n", s);
	return 0;
}
CEOF

    crlf scratch_root/mini.cpp <<'CXXEOF'
#include <cstdio>

template <typename T>
static T triangle(T n)
{
	T s = T();
	for (T i = 1; i <= n; ++i)
		s += i;
	return s;
}

struct Greeter {
	const char* who;
	explicit Greeter(const char* w) : who(w) {}
	virtual ~Greeter() {}
	virtual void hello(int n) const { std::printf("hello from %s, sum=%d\n", who, n); }
};

struct Loud : Greeter {
	explicit Loud(const char* w) : Greeter(w) {}
	void hello(int n) const { std::printf("HELLO FROM %s, SUM=%d\n", who, n); }
};

int main()
{
	Greeter a("DJGPP g++");
	Loud b("DJGPP g++");
	const Greeter* v[2] = { &a, &b };
	for (int i = 0; i < 2; i++)
		v[i]->hello(triangle(10));
	return 0;
}
CXXEOF
    # Floating point, which is its own thing: it goes through the x87 register stack, the
    # control word's rounding mode, libm and printf's %f conversion, and every one of
    # those was wrong at some point in a way that still printed a plausible number.
    crlf scratch_root/fp.c <<'FPEOF'
#include <stdio.h>
#include <math.h>

int main(void)
{
	double s = 0.0;
	int i;
	for (i = 1; i <= 10; i++)
		s += 1.0 / i;
	printf("H10=%.6f sqrt2=%.6f sin1=%.6f\n", s, sqrt(2.0), sin(1.0));
	return 0;
}
FPEOF
    echo "   wrote scratch_root/hello.c, scratch_root/mini.cpp and scratch_root/fp.c"
fi

if [ "$1" = "ow" ]; then
    echo "== OpenWatcom 11.0c into ow/ from upstream/*.zip"
    mkdir -p ow
    for z in core_binw core_all c_binw cpp_binw clib_hdr clib_d32 clib_d16 clib_o16              cpplib_hdr cpplib_o32 cpplib_o16; do
        unzip -oq "upstream/$z.zip" -d ow
    done
    # dos4gw.exe is the one piece not in the component zips; the stub looks for it on
    # PATH by that exact (lower-case) name.
    unzip -oq upstream/dos4gw.zip -d ow/binw
    [ -f ow/binw/DOS4GW.EXE ] && mv ow/binw/DOS4GW.EXE ow/binw/dos4gw.exe
    echo "   ow/: $(du -sh ow | cut -f1) -- provenance and licences in upstream/README.md"

    crlf ow/hello.c <<'WCEOF'
#include <stdio.h>

int main(void)
{
	int i, s = 0;
	for (i = 1; i <= 10; i++)
		s += i;
	printf("hello from Watcom C, sum=%d\n", s);
	return 0;
}
WCEOF

    crlf ow/hello.cpp <<'WXEOF'
#include <stdio.h>

struct Adder {
	int total;
	Adder() : total(0) {}
	void add(int v) { total += v; }
};

int main()
{
	Adder a;
	for (int i = 1; i <= 10; ++i) a.add(i);
	printf("hello from Watcom C++, sum=%d\n", a.total);
	return 0;
}
WXEOF
    echo "   wrote ow/hello.c and ow/hello.cpp"
    cat <<'WEOF'

done. Now:
  sh build.sh
  ./dosemu --root ow ow/binw/wcc386.exe hello.c     # -> hello.obj, "Code size: 44"
  ./dosemu --root ow ow/binw/wcc.exe    hello.c     # the 16-bit compiler
  ./dosemu --root ow ow/binw/wpp386.exe hello.cpp   # the C++ one
Linking does not work yet -- see OPENWATCOM.md.
WEOF
    exit 0
fi

cat <<'EOF'

done. Now:
  sh build.sh
  ./dosemu --root scratch_root scratch_root/LSIC86/BIN/LCC.EXE PROG.C   # then run PROG.exe -> sum=55
  node web/test_shell.mjs && node web/test_bundle.mjs && node web/test_node.mjs
  ./dosemu --root scratch_root scratch_root/DJGPP/bin/gcc.exe -O2 hello.c  -o hello.exe
  ./dosemu --root scratch_root scratch_root/DJGPP/bin/gpp.exe -O2 mini.cpp -o mini.exe
  ./dosemu --root scratch_root scratch_root/hello.exe    # hello from DJGPP gcc, sum=55
  ./dosemu --root scratch_root scratch_root/mini.exe     # ...and the C++ one
  ./dosemu --root scratch_root scratch_root/DJGPP/bin/gcc.exe -O2 fp.c -lm -o fp.exe
  ./dosemu --root scratch_root scratch_root/fp.exe       # H10=2.928968 sqrt2=1.414214 sin1=0.841471
EOF
