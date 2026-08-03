// Verifies the shell flow the page uses: COMMAND.COM /C <cmd>. Untar the bundle,
// write a .c, have FreeCOM compile it with LSI C, then have FreeCOM run the result.
//   node web/test_shell.mjs
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import zlib from 'node:zlib';
import fs from 'node:fs';
import path from 'node:path';

const require = createRequire(import.meta.url);
const here = path.dirname(fileURLToPath(import.meta.url));
const createDosemu = require(path.join(here, 'dosemu.js'));

function untar(b) { const o = []; for (let p = 0; p + 512 <= b.length;) {
  let n = ''; for (let i = 0; i < 100 && b[p + i]; i++) n += String.fromCharCode(b[p + i]); if (!n) break;
  let pre = ''; for (let i = 0; i < 155 && b[p + 345 + i]; i++) pre += String.fromCharCode(b[p + 345 + i]); if (pre) n = pre + '/' + n;
  let s = 0; for (let i = 0; i < 11 && b[p + 124 + i]; i++) s = s * 8 + (b[p + 124 + i] - 48); const t = b[p + 156]; p += 512;
  if (t === 0x30 || t === 0) o.push({ name: n.replace(/^\.\//, ''), bytes: b.subarray(p, p + s) }); p += Math.ceil(s / 512) * 512; } return o; }

let out = '';
const d = new TextDecoder();
globalThis.dosemuOutput = (fd, by) => { out += d.decode(by, { stream: true }); };
globalThis.dosemuLog = () => {};

const M = await createDosemu();
const FS = M.FS;
const mk = (p) => { let c = ''; for (const s of p.split('/').filter(Boolean)) { c += '/' + s; try { FS.mkdir(c); } catch {} } };
const lsic = untar(zlib.gunzipSync(fs.readFileSync(path.join(here, 'lsic.tar.gz'))));
mk('/dos');
for (const f of lsic) { const full = '/dos/' + f.name; const sl = full.lastIndexOf('/'); if (sl > 0) mk(full.slice(0, sl)); FS.writeFile(full, new Uint8Array(f.bytes)); }

FS.writeFile('/dos/PROG.C', '#include <stdio.h>\nint main(void){ printf("shell -> LSI C -> exe!\\n"); return 0; }\n');

const cmd = (line) => { out = ''; const rc = M.ccall('dosemu_run', 'number', ['string', 'string', 'string'],
  ['/dos/COMMAND.COM', '/dos', '/C ' + line]); return { rc, out: out.replace(/\r/g, '') }; };

console.log('A:\\> lcc prog.c');
let r = cmd('lcc prog.c'); process.stdout.write(r.out);
const built = FS.analyzePath('/dos/PROG.exe').exists || FS.analyzePath('/dos/PROG.EXE').exists;
console.log('  (PROG.EXE built:', built, ')');
console.log('A:\\> prog');
r = cmd('prog'); process.stdout.write(r.out);
const ok = r.out.includes('shell -> LSI C -> exe!');
console.log(ok ? 'SHELL DEMO OK' : 'FAIL');
process.exit(ok ? 0 : 1);
