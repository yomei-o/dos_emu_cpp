// Verifies the committed page bundle: gunzip + untar web/lsic.tar.gz into MEMFS the
// way index.html does, compile a .c with LSI C, run the produced .EXE.
//   node web/test_bundle.mjs
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
    const t = b[p + 156]; p += 512;
    if (t === 0x30 || t === 0) o.push({ name: n.replace(/^\.\//, ''), bytes: b.subarray(p, p + s) });
    p += Math.ceil(s / 512) * 512;
  }
  return o;
}

let out = '';
const d = new TextDecoder();
globalThis.dosemuOutput = (fd, by) => { out += d.decode(by, { stream: true }); };
globalThis.dosemuLog = (m) => { console.error('[emu]', m); };

const M = await createDosemu();
const FS = M.FS;
const mk = (p) => { let c = ''; for (const s of p.split('/').filter(Boolean)) { c += '/' + s; try { FS.mkdir(c); } catch {} } };

const lsic = untar(zlib.gunzipSync(fs.readFileSync(path.join(here, 'lsic.tar.gz'))));
mk('/dos');
for (const f of lsic) { const full = '/dos/' + f.name; const sl = full.lastIndexOf('/'); if (sl > 0) mk(full.slice(0, sl)); FS.writeFile(full, new Uint8Array(f.bytes)); }
console.log('untarred', lsic.length, 'files');

const src = [
  '#include <stdio.h>',
  'int main(void) {',
  '  int i, sum = 0;',
  '  for (i = 1; i <= 10; i++) sum += i;',
  '  printf("sum = %d\\n", sum);',
  '  return 0;',
  '}',
  '',
].join('\n');
FS.writeFile('/dos/PROG.C', src);

const run = (p, t) => M.ccall('dosemu_run', 'number', ['string', 'string', 'string'], [p, '/dos', t]);

out = '';
const crc = run('/dos/LSIC86/BIN/LCC.EXE', 'PROG.C');
if (crc !== 0) { console.log('compile output:\n' + out.replace(/\r/g, '')); console.log('compile FAILED, exit', crc); process.exit(1); }
const exe = FS.analyzePath('/dos/PROG.exe').exists ? '/dos/PROG.exe' : '/dos/PROG.EXE';

out = '';
const rrc = run(exe, '');
const text = out.replace(/\r/g, '');
process.stdout.write(text);
console.log('--- compile', crc, 'run', rrc, '---');
const ok = rrc === 0 && text.includes('sum = 55');
console.log(ok ? 'BUNDLE OK' : 'FAIL');
process.exit(ok ? 0 : 1);
