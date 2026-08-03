// WebAssembly entry point. The emulator core has no OS dependencies beyond the
// C++ filesystem/stdio (which emscripten backs with MEMFS), so it compiles to wasm
// as is. JS populates MEMFS with the LSI C-86 install + a .c file, calls
// dosemu_run() to compile it, then again to run the produced .EXE.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "cpu.h"
#include "dos.h"
#include "memory.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
EM_JS(void, js_out, (int fd, const char* p, int n), { globalThis.dosemuOutput(fd, HEAPU8.slice(p, p + n)); });
EM_JS(void, js_log, (const char* p), { globalThis.dosemuLog(UTF8ToString(p)); });
#else
static void js_out(int, const char*, int) {}
static void js_log(const char*) {}
#endif

namespace dosemu {
bool load_program(const std::vector<uint8_t>&, Cpu&, uint16_t, const std::string&, std::string&);
}

static std::vector<uint8_t> read_host(const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return {};
    std::fseek(fp, 0, SEEK_END); long n = std::ftell(fp); std::fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> v(n > 0 ? n : 0);
    if (n > 0 && std::fread(v.data(), 1, n, fp) != static_cast<size_t>(n)) v.clear();
    std::fclose(fp);
    return v;
}

extern "C" {

// Run a DOS program that lives at prog_host (a MEMFS path), with the DOS drive
// rooted at `root` and a command tail `cmdtail`. Output is streamed to JS. Returns
// the guest exit code, or -1 if it could not run.
int dosemu_run(const char* prog_host, const char* root, const char* cmdtail) {
    using namespace dosemu;
    std::vector<uint8_t> file = read_host(prog_host);
    if (file.empty()) { js_log("cannot read program"); return -1; }

    Memory mem;
    Cpu cpu(mem);
    Dos dos(cpu, mem, root);
    dos.output = [](int fd, const char* data, size_t len) { js_out(fd, data, static_cast<int>(len)); };

    std::string err;
    std::string tail = cmdtail ? cmdtail : "";
    if (!load_program(file, cpu, 0x0100, tail, err)) { js_log(err.c_str()); return -1; }
    dos.psp_seg = 0x0100;

    try {
        cpu.run();
    } catch (const CpuError& e) {
        js_log(e.what.c_str());
        return -1;
    }
    return cpu.exit_code;
}

}  // extern "C"
