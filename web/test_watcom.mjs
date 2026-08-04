// Verifies what web/watcom.html does, without a browser: unpack the Watcom bundle into
// MEMFS, compile hello.c with wcc and hello.cpp with wpp, link each with wlink, and run
// what comes out.
//
//   node web/test_watcom.mjs
//
// Three DOS programs deep, and each one exercises a different part of the emulator. The
// compilers are stubbed for FlashTek X-32 and hand themselves to w32run.exe, which is not
// a DPMI client at all -- it loads its own IDT and reflects DOS calls itself, at CPL 3.
// The linker is DOS/4GW, which *is* a client of the built-in DPMI host and reflects its
// DOS calls back through INT 31h 0302h. The program they build is plain 16-bit real mode.
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
    p += 512;
    if (type === 0x30 || type === 0) o.push({ name: n.replace(/^\.\//, ''), bytes: b.subarray(p, p + s) });
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
const files = untar(zlib.gunzipSync(fs.readFileSync(path.join(here, 'watcom.tar.gz'))));
for (const f of files) {
  const p = '/dos/' + f.name;
  mkdirp(path.posix.dirname(p));
  M.FS.writeFile(p, f.bytes);
}
console.log(`unpacked ${files.length} entries in ${Date.now() - t0} ms`);

const run = (prog, args) => {
  const t = Date.now();
  out = '';
  const rc = M.ccall('dosemu_run', 'number', ['string', 'string', 'string'],
                     [prog, '/dos', args ? ' ' + args : '']);
  return { rc, ms: Date.now() - t, out };
};

let fails = 0;
const step = (label, prog, args) => {
  const r = run(prog, args);
  const noise = r.out.split('\n').filter(l => l.trim() && !/Copyright|All rights reserved|^Watcom |^WATCOM /.test(l));
  console.log(`  ${label}: rc=${r.rc} ${r.ms} ms${noise.length ? '\n     ' + noise.join('\n     ') : ''}`);
  if (r.rc !== 0) fails++;
  return r;
};

function build(kind) {
  const cpp = kind === 'cpp';
  const src = cpp ? 'hello.cpp' : 'hello.c';
  const exe = cpp ? 'hellocpp.exe' : 'hello.exe';
  const want = cpp ? 'HELLO FROM Watcom C++, SUM=55' : 'hello from Watcom C, sum=55';
  console.log(`\ncompiling ${src} the way the demo page does:`);
  const t0 = Date.now();
  step(cpp ? 'wpp  ' : 'wcc  ', `/dos/binw/${cpp ? 'wpp.exe' : 'wcc.exe'}`, src);
  step('wlink', '/dos/binw/wlink.exe', `system dos file hello.obj name ${exe}`);
  const prog = step('run  ', `/dos/${exe}`, '');
  const ok = prog.out.includes(want);
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${src} -> "${want}"   [${((Date.now() - t0) / 1000).toFixed(1)} s]`);
  if (!ok) fails++;
}

build('c');
build('cpp');

console.log(fails ? `\nFAILED (${fails})` : '\nWATCOM C/C++ IN WASM OK');
process.exit(fails ? 1 : 0);
