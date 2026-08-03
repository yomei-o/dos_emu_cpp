// Headless proof of the browser flow: populate MEMFS with the LSI C-86 install and
// a .c, compile it with LCC (which spawns its passes), then run the produced .EXE.
//   node web/test_node.mjs
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import fs from 'node:fs';
import path from 'node:path';

const require = createRequire(import.meta.url);
const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.join(here, '..');
const createDosemu = require(path.join(here, 'dosemu.js'));

let out = '';
const dec = new TextDecoder();
globalThis.dosemuOutput = (fd, bytes) => { out += dec.decode(bytes, { stream: true }); };
globalThis.dosemuLog = (m) => { /* diagnostics */ };

const mod = await createDosemu();
const FS = mod.FS;
const mkdirp = (p) => { let c=''; for (const s of p.split('/').filter(Boolean)) { c+='/'+s; try { FS.mkdir(c); } catch {} } };

// Copy the host lsic/ tree into MEMFS as the DOS drive at /dos (A:).
const srcRoot = path.join(root, 'lsic');
function put(hostDir, memDir) {
  mkdirp(memDir);
  for (const e of fs.readdirSync(hostDir, { withFileTypes: true })) {
    const s = path.join(hostDir, e.name), d = memDir + '/' + e.name;
    if (e.isDirectory()) put(s, d);
    else FS.writeFile(d, new Uint8Array(fs.readFileSync(s)));
  }
}
mkdirp('/dos/LSIC86');
put(path.join(srcRoot, 'BIN'), '/dos/LSIC86/BIN');
put(path.join(srcRoot, 'LIB'), '/dos/LSIC86/LIB');
put(path.join(srcRoot, 'INCLUDE'), '/dos/LSIC86/INCLUDE');
FS.writeFile('/dos/_LCC', new Uint8Array(fs.readFileSync(path.join(srcRoot, 'BIN/_LCC'))));
FS.writeFile('/dos/HELLO.C', 'int puts(const char*);\nint main(void){ puts("hello from LSI C in wasm!"); return 0; }\n');

const run = (prog, tail) => mod.ccall('dosemu_run', 'number', ['string','string','string'], [prog, '/dos', tail]);

out = '';
console.log('compiling HELLO.C ...');
const crc = run('/dos/LSIC86/BIN/LCC.EXE', 'HELLO.C');
console.log('  LCC exit', crc, '| produced HELLO.EXE:', FS.analyzePath('/dos/HELLO.exe').exists || FS.analyzePath('/dos/HELLO.EXE').exists);

out = '';
const exe = FS.analyzePath('/dos/HELLO.exe').exists ? '/dos/HELLO.exe' : '/dos/HELLO.EXE';
const rrc = run(exe, '');
const text = out.replace(/\r/g, '');
console.log('--- program output ---'); process.stdout.write(text);
console.log('--- exit', rrc, '---');
const ok = rrc === 0 && text.includes('hello from LSI C in wasm!');
console.log(ok ? 'DOS + LSI C IN WASM OK' : 'not yet');
process.exit(ok ? 0 : 1);
