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
bool load_program(const std::vector<uint8_t>&, Cpu&, uint16_t, const std::string&, std::string&, const std::string&, uint16_t);
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

// The DOS current directory persists across dosemu_run() calls, so a shell prompt
// driven one command per /C invocation still remembers where 'cd' moved to.
static std::string g_cwd = "\\";

extern "C" {

const char* dosemu_cwd() { return g_cwd.c_str(); }

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
    dos.files().set_cwd(g_cwd);
    dos.output = [](int fd, const char* data, size_t len) { js_out(fd, data, static_cast<int>(len)); };

    std::string err;
    std::string tail = cmdtail ? cmdtail : "";
    // argv[0] as the guest will see it: A:\ plus the program's location under `root`,
    // not just its base name. A program in a subdirectory finds its own installation
    // by opening argv[0] — DJGPP's gcc does — and with the base name alone it reports
    // `A:\GCC.EXE: can't open`. Same derivation as main.cpp.
    std::string dn;
    {
        auto slashes = [](std::string s) { for (char& c : s) if (c == '/') c = '\\'; return s; };
        std::string p = slashes(prog_host), r = slashes(root ? root : "");
        while (!r.empty() && r.back() == '\\') r.pop_back();
        if (!r.empty() && r != "." && p.size() > r.size() + 1 &&
            p.compare(0, r.size(), r) == 0 && p[r.size()] == '\\') {
            dn = p.substr(r.size() + 1);
        } else {
            auto s = p.find_last_of('\\');
            dn = (s == std::string::npos) ? p : p.substr(s + 1);
        }
        for (char& c : dn) c = static_cast<char>(toupper((unsigned char)c));
        dn = "A:\\" + dn;
    }
    if (!load_program(file, cpu, 0x0100, tail, err, dn, 0)) { js_log(err.c_str()); return -1; }
    cpu.max_insns = 2000000000ULL;   // ~ generous for a compile; guards against a runaway freezing the page
    dos.psp_seg = 0x0100;

    int code;
    try {
        cpu.run();
        code = cpu.exit_code;
    } catch (const CpuError& e) {
        js_log(e.what.c_str());
        code = -1;
    }
    g_cwd = dos.files().cwd();   // remember any 'cd' for the next command
    return code;
}

}  // extern "C"
