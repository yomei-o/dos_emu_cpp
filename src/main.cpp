// Command-line front end: load a DOS .EXE/.COM and run it.
//   dosemu PROGRAM.EXE [guest args...]
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "cpu.h"
#include "dos.h"
#include "memory.h"

namespace dosemu {
bool load_program(const std::vector<uint8_t>&, Cpu&, uint16_t, const std::string&, std::string&);
}

static std::vector<uint8_t> read_file(const char* path) {
    std::FILE* fp = std::fopen(path, "rb");
    if (!fp) return {};
    std::fseek(fp, 0, SEEK_END); long n = std::ftell(fp); std::fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> v(n > 0 ? n : 0);
    if (n > 0 && std::fread(v.data(), 1, n, fp) != static_cast<size_t>(n)) v.clear();
    std::fclose(fp);
    return v;
}

int main(int argc, char** argv) {
    // dosemu [--root DIR] PROGRAM.EXE [guest args...]
    std::string root = ".";
    int a = 1;
    if (a < argc && std::string(argv[a]) == "--root" && a + 1 < argc) { root = argv[a + 1]; a += 2; }
    if (a >= argc) { std::fprintf(stderr, "usage: dosemu [--root DIR] PROGRAM.EXE [args...]\n"); return 2; }

    std::vector<uint8_t> file = read_file(argv[a]);
    if (file.empty()) { std::fprintf(stderr, "dosemu: cannot read %s\n", argv[a]); return 1; }

    std::string cmdline;
    for (int i = a + 1; i < argc; ++i) { cmdline += ' '; cmdline += argv[i]; }

    using namespace dosemu;
    Memory mem;
    Cpu cpu(mem);
    Dos dos(cpu, mem, root);
    dos.output = [](int fd, const char* data, size_t len) {
        std::fwrite(data, 1, len, fd == 2 ? stderr : stdout);
    };

    std::string err;
    if (!load_program(file, cpu, 0x0100, cmdline, err)) {
        std::fprintf(stderr, "dosemu: %s\n", err.c_str());
        return 1;
    }
    dos.psp_seg = 0x0100;

    try {
        cpu.run();
    } catch (const CpuError& e) {
        std::fflush(stdout);
        std::fprintf(stderr, "\ndosemu: %s  [%llu instructions]\n", e.what.c_str(),
                     static_cast<unsigned long long>(cpu.insns));
        return 1;
    }
    std::fflush(stdout);
    return cpu.exit_code;
}
