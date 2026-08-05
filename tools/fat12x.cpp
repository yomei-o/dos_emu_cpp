// fat12x — extract every file from a FAT12 floppy image into a directory.
//
//   g++ -std=c++17 -O2 -o fat12x tools/fat12x.cpp
//   ./fat12x DISK01.IMG outdir            # repeat per disk; later disks add files
//
// This is how the Borland and Microsoft compiler disks in the demos were unpacked:
// each 360K/720K image is a plain FAT12 volume, so the layout is entirely described
// by the BPB in its boot sector — FAT right after the reserved sectors, the fixed
// root directory after the FATs, data clusters after that. Nothing here is specific
// to floppies beyond assuming FAT12 (single-byte-and-a-half entries); hard-disk
// FAT16 images would need a different next-cluster read and nothing else.
//
// A sibling tools/fat12x.py does the same job for a machine with Python but no C++
// compiler; keep the two in step.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static uint16_t r16(const std::vector<uint8_t>& d, size_t o) {
    return static_cast<uint16_t>(d[o] | (d[o + 1] << 8));
}
static uint32_t r32(const std::vector<uint8_t>& d, size_t o) {
    return static_cast<uint32_t>(r16(d, o)) | (static_cast<uint32_t>(r16(d, o + 2)) << 16);
}

struct Volume {
    std::vector<uint8_t> img;
    size_t fat_off = 0, root_off = 0, data_off = 0;
    size_t cluster_size = 0, nroot = 0, nclusters = 0;

    // FAT12: entry n lives in the byte-and-a-half at n*3/2; odd entries take the
    // high 12 bits, even the low 12. 0xFF8.. marks end of chain.
    uint16_t next(uint16_t n) const {
        const size_t o = fat_off + n * 3 / 2;
        const uint16_t v = static_cast<uint16_t>(img[o] | (img[o + 1] << 8));
        return (n & 1) ? (v >> 4) : (v & 0xFFF);
    }

    std::vector<uint8_t> chain(uint16_t c, size_t size, bool cap) const {
        std::vector<uint8_t> out;
        size_t hops = 0;
        while (c >= 2 && c < 0xFF8 && hops++ <= nclusters) {   // hop cap: corrupt FATs loop
            const size_t o = data_off + static_cast<size_t>(c - 2) * cluster_size;
            if (o + cluster_size > img.size()) break;
            out.insert(out.end(), img.begin() + o, img.begin() + o + cluster_size);
            c = next(c);
        }
        if (cap && out.size() > size) out.resize(size);        // directories have size 0
        return out;
    }
};

// One 32-byte directory entry -> host name, or "" for the slots to skip.
static std::string entry_name(const uint8_t* e) {
    if (e[0] == 0x00 || e[0] == 0xE5) return "";               // free / deleted
    if (e[11] == 0x0F) return "";                              // VFAT long-name slot
    if (e[11] & 0x08) return "";                               // volume label
    std::string base, ext;
    for (int i = 0; i < 8; ++i) base += static_cast<char>(e[i]);
    for (int i = 8; i < 11; ++i) ext += static_cast<char>(e[i]);
    while (!base.empty() && base.back() == ' ') base.pop_back();
    while (!ext.empty() && ext.back() == ' ') ext.pop_back();
    if (base.empty()) return "";
    if (base[0] == 0x05) base[0] = '\xE5';                     // KANJI lead byte escape
    return ext.empty() ? base : base + "." + ext;
}

static void walk(const Volume& v, const std::vector<uint8_t>& dir, const fs::path& out) {
    for (size_t i = 0; i + 32 <= dir.size(); i += 32) {
        const uint8_t* e = dir.data() + i;
        if (e[0] == 0x00) break;                               // end-of-directory marker
        const std::string name = entry_name(e);
        if (name.empty() || name == "." || name == "..") continue;
        const uint16_t clus = static_cast<uint16_t>(e[26] | (e[27] << 8));
        const uint32_t size = static_cast<uint32_t>(e[28]) | (static_cast<uint32_t>(e[29]) << 8) |
                              (static_cast<uint32_t>(e[30]) << 16) | (static_cast<uint32_t>(e[31]) << 24);
        if (e[11] & 0x10) {                                    // subdirectory: recurse
            fs::create_directories(out / name);
            walk(v, v.chain(clus, 0, false), out / name);
        } else {
            fs::create_directories(out);
            const std::vector<uint8_t> body = v.chain(clus, size, true);
            std::ofstream f(out / name, std::ios::binary);
            f.write(reinterpret_cast<const char*>(body.data()),
                    static_cast<std::streamsize>(body.size()));
            std::printf("%s %u\n", (out / name).string().c_str(), size);
        }
    }
}

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: fat12x IMAGE.IMG OUTDIR\n"); return 2; }
    Volume v;
    {
        std::ifstream f(argv[1], std::ios::binary);
        if (!f) { std::fprintf(stderr, "fat12x: cannot read %s\n", argv[1]); return 1; }
        v.img.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (v.img.size() < 512) { std::fprintf(stderr, "fat12x: not a disk image\n"); return 1; }
    const uint16_t bps = r16(v.img, 11);
    const uint8_t spc = v.img[13];
    const uint16_t rsvd = r16(v.img, 14);
    const uint8_t nfats = v.img[16];
    const uint16_t nroot = r16(v.img, 17);
    uint32_t totsec = r16(v.img, 19);
    if (totsec == 0) totsec = r32(v.img, 32);                  // large-volume field
    const uint16_t spf = r16(v.img, 22);
    if (!bps || !spc || !nfats || !spf) { std::fprintf(stderr, "fat12x: no FAT BPB\n"); return 1; }
    v.fat_off = static_cast<size_t>(rsvd) * bps;
    v.root_off = v.fat_off + static_cast<size_t>(nfats) * spf * bps;
    v.data_off = v.root_off + static_cast<size_t>(nroot) * 32;
    v.cluster_size = static_cast<size_t>(spc) * bps;
    v.nclusters = (static_cast<size_t>(totsec) * bps - v.data_off) / v.cluster_size;
    walk(v, {v.img.begin() + v.root_off, v.img.begin() + v.data_off}, argv[2]);
    return 0;
}
