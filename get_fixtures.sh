#!/bin/sh
# Reconstitute the gitignored fixtures a fresh clone needs to run the tests.
#
#   sh get_fixtures.sh          # lsic/ + scratch_root/ from the committed bundle
#   sh get_fixtures.sh djgpp    # ...and the DJGPP C/C++ toolchain too
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
    echo "   wrote scratch_root/hello.c and scratch_root/mini.cpp"
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
EOF
