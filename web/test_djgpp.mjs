// Verifies what web/djgpp.html does, without a browser: unpack the DJGPP bundle into
// MEMFS, drive cc1 -> as -> ld -> stubify by hand, then run the .exe that comes out.
//
//   node web/test_djgpp.mjs
//
// The gcc *driver* is not used. Its 3.4.6 build cannot exec cc1 out of libexec/ under
// this emulator, and 4.7.4 -- whose driver does work -- is three times the size and
// takes about a minute per file. Running the four passes from here is both faster and
// more interesting to watch, since each stage's output is a real artefact.
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import zlib from 'node:zlib';
import fs from 'node:fs';
import path from 'node:path';

const require = createRequire(import.meta.url);
const here = path.dirname(fileURLToPath(import.meta.url));
const createDosemu = require(path.join(here, 'dosemu.js'));

function untar(b) {
  const o = [];
  for (let p = 0; p + 512 <= b.length;) {
    let n = ''; for (let i = 0; i < 100 && b[p + i]; i++) n += String.fromCharCode(b[p + i]);
    if (!n) break;
    let pre = ''; for (let i = 0; i < 155 && b[p + 345 + i]; i++) pre += String.fromCharCode(b[p + 345 + i]);
    if (pre) n = pre + '/' + n;
    let s = 0; for (let i = 0; i < 11 && b[p + 124 + i]; i++) s = s * 8 + (b[p + 124 + i] - 48);
    const type = b[p + 156];
    let link = ''; for (let i = 0; i < 100 && b[p + 157 + i]; i++) link += String.fromCharCode(b[p + 157 + i]);
    p += 512;
    // type 0/'0' = file, '1' = hard link (cc1 is stored once and linked into bin/),
    // '5' = directory.
    if (type === 0x30 || type === 0) o.push({ name: n.replace(/^\.\//, ''), bytes: b.subarray(p, p + s) });
    else if (type === 0x31) o.push({ name: n.replace(/^\.\//, ''), link: link.replace(/^\.\//, '') });
    p += Math.ceil(s / 512) * 512;
  }
  return o;
}

let out = '';
const dec = new TextDecoder();
globalThis.dosemuOutput = (fd, by) => { out += dec.decode(by, { stream: true }); };
globalThis.dosemuLog = m => { out += '[emu] ' + m + '\n'; };

const M = await createDosemu();
const mkdirp = d => { let c = ''; for (const part of d.split('/')) { if (!part) continue; c += '/' + part; try { M.FS.mkdir(c); } catch (e) {} } };

const t0 = Date.now();
const files = untar(zlib.gunzipSync(fs.readFileSync(path.join(here, 'djgpp346.tar.gz'))));
const byName = new Map();
for (const f of files) {
  const p = '/dos/' + f.name;
  mkdirp(path.posix.dirname(p));
  const bytes = f.link ? byName.get(f.link) : f.bytes;
  if (!bytes) continue;
  byName.set(f.name, bytes);
  M.FS.writeFile(p, bytes);
}
console.log(`unpacked ${files.length} entries in ${Date.now() - t0} ms`);

const SRC_C = `#include <stdio.h>\r\n\r\nint main(void)\r\n{\r\n\tint i, s = 0;\r\n\tfor (i = 1; i <= 10; i++)\r\n\t\ts += i;\r\n\tprintf("hello from DJGPP gcc, sum=%d\\n", s);\r\n\treturn 0;\r\n}\r\n`;

const SRC_CPP = [
  '#include <cstdio>', '',
  'template <typename T> static T triangle(T n)',
  '{', '\tT s = T();', '\tfor (T i = 1; i <= n; ++i) s += i;', '\treturn s;', '}', '',
  'struct Greeter {',
  '\tconst char* who;',
  '\texplicit Greeter(const char* w) : who(w) {}',
  '\tvirtual ~Greeter() {}',
  '\tvirtual void hello(int n) const { std::printf("hello from %s, sum=%d\\n", who, n); }',
  '};', '',
  'struct Loud : Greeter {',
  '\texplicit Loud(const char* w) : Greeter(w) {}',
  '\tvoid hello(int n) const { std::printf("HELLO FROM %s, SUM=%d\\n", who, n); }',
  '};', '',
  'int main()',
  '{', '\tGreeter a("DJGPP g++");', '\tLoud b("DJGPP g++");',
  '\tconst Greeter* v[2] = { &a, &b };',
  '\tfor (int i = 0; i < 2; i++) v[i]->hello(triangle(10));',
  '\treturn 0;', '}', ''
].join('\r\n');

M.FS.writeFile('/dos/hello.c', SRC_C);

const B = '/dos/DJGPP/bin';
const LIB = ['-LA:/DJGPP/lib', '-LA:/DJGPP/lib/gcc/djgpp/3.46'];
const run = (prog, args) => {
  const t = Date.now();
  out = '';
  const rc = M.ccall('dosemu_run', 'number', ['string', 'string', 'string'],
                     [prog, '/dos', ' ' + args.join(' ')]);
  return { rc, ms: Date.now() - t, out };
};

let fails = 0;
const step = (label, prog, args) => {
  const r = run(prog, args);
  const noise = r.out.split('\n').filter(l => l && !/Coprocessor|floating operations/.test(l));
  console.log(`  ${label}: rc=${r.rc} ${r.ms} ms${noise.length ? '\n     ' + noise.join('\n     ') : ''}`);
  if (r.rc !== 0) fails++;
  return r;
};

function build(kind) {
  const cpp = kind === 'cpp';
  const src = cpp ? 'mini.cpp' : 'hello.c';
  const base = cpp ? 'mini' : 'hello';
  const want = cpp ? 'HELLO FROM DJGPP g++, SUM=55' : 'hello from DJGPP gcc, sum=55';
  console.log(`\ncompiling ${src} the way the demo page does:`);
  const t0 = Date.now();
  step('cc1    ', `${B}/${cpp ? 'cc1plus.exe' : 'cc1.exe'}`, [src, '-quiet', '-O2', '-o', base + '.s']);
  step('as     ', `${B}/as.exe`, [base + '.s', '-o', base + '.o']);
  // -lgcc on both sides of -lc: the first copy satisfies __register_frame_info, so
  // libc's rfinfo.o is never pulled in (it would clash with libgcc's unwinder); the
  // second provides the 64-bit division helpers libc's printf needs.
  const libs = cpp ? ['-lstdcxx', '-lsupcxx', '-lgcc', '-lc', '-lgcc'] : ['-lc', '-lgcc'];
  step('ld     ', `${B}/ld.exe`, ['-o', base + '.cof', 'A:/DJGPP/lib/crt0.o', base + '.o', ...LIB, ...libs]);
  step('stubify', `${B}/stubify.exe`, [base + '.cof']);
  const prog = step('run    ', `/dos/${base}.exe`, []);
  const ok = prog.out.includes(want);
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${src} -> "${want}"   [${((Date.now() - t0) / 1000).toFixed(1)} s]`);
  if (!ok) fails++;
}

M.FS.writeFile('/dos/mini.cpp', SRC_CPP);
build('c');
build('cpp');

console.log(fails ? `\nFAILED (${fails})` : '\nDJGPP IN WASM OK');
process.exit(fails ? 1 : 0);
