// INT 21h / BIOS service dispatch.
#include "dos.h"
#include <cstdio>
#include <vector>
#include <string>
#include <cctype>
#include <filesystem>

namespace dosemu {

bool load_program(const std::vector<uint8_t>&, Cpu&, uint16_t, const std::string&, std::string&, const std::string&);

void Dos::terminate(int code) {
    if (exec_depth_ > 0) { child_exited_ = true; child_code_ = code; return; }  // end the child only
    cpu_.exit_code = code;
    cpu_.halted = true;
}

// INT 21h AH=4Bh AL=0: load and run a child program to completion, then return to
// the parent. The child runs on the same CPU through a nested step loop; the
// parent's registers, PSP and heap mark are saved and restored around it.
bool Dos::exec(const std::string& name, uint16_t pb_seg, uint16_t pb_off) {
    std::string host = files_.host_path(name);
    std::FILE* fp = std::fopen(host.c_str(), "rb");
    if (!fp) { cpu_.flags |= CF; cpu_.r[AX] = 2; return true; }   // file not found
    std::fseek(fp, 0, SEEK_END); long n = std::ftell(fp); std::fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> file(n > 0 ? n : 0);
    if (n > 0) { if (std::fread(file.data(), 1, n, fp) != (size_t)n) file.clear(); }
    std::fclose(fp);
    if (file.empty()) { cpu_.flags |= CF; cpu_.r[AX] = 2; return true; }

    // Parameter block: [0]=env seg, [2..5]=cmdline far ptr (offset,segment).
    uint16_t cmd_off = mem_.rw(pb_seg, pb_off + 2);
    uint16_t cmd_seg = mem_.rw(pb_seg, pb_off + 4);
    uint8_t taillen = mem_.rb(cmd_seg, cmd_off);
    std::string tail;
    for (uint8_t i = 0; i < taillen; ++i) { char c = static_cast<char>(mem_.rb(cmd_seg, cmd_off + 1 + i)); if (c == '\r') break; tail += c; }

    // Save the parent's whole context.
    uint16_t sr[8], ss[4], sip = cpu_.ip, sfl = cpu_.flags, spsp = psp_seg, sheap = heap_next_;
    for (int i = 0; i < 8; ++i) sr[i] = cpu_.r[i];
    for (int i = 0; i < 4; ++i) ss[i] = cpu_.sreg[i];

    uint16_t child_psp = heap_next_;
    heap_next_ += 0x1800;   // ~96 KiB for the child (LSI C passes are small)

    std::string err;
    if (!load_program(file, cpu_, child_psp, tail, err, name)) {
        for (int i = 0; i < 8; ++i) cpu_.r[i] = sr[i];
        for (int i = 0; i < 4; ++i) cpu_.sreg[i] = ss[i];
        cpu_.ip = sip; cpu_.flags = sfl; psp_seg = spsp; heap_next_ = sheap;
        cpu_.flags |= CF; cpu_.r[AX] = 2; return true;
    }
    psp_seg = child_psp;

    // Run the child to completion.
    bool saved_exited = child_exited_; child_exited_ = false;
    ++exec_depth_;
    while (!child_exited_ && !cpu_.halted) cpu_.step();
    --exec_depth_;
    int code = child_code_;
    child_exited_ = saved_exited;

    // Restore the parent and hand it the child's exit code (via AH=4Dh).
    for (int i = 0; i < 8; ++i) cpu_.r[i] = sr[i];
    for (int i = 0; i < 4; ++i) cpu_.sreg[i] = ss[i];
    cpu_.ip = sip; cpu_.flags = sfl; psp_seg = spsp; heap_next_ = sheap;
    last_child_code_ = code;
    cpu_.flags &= ~CF; cpu_.r[AX] = 0;
    return true;
}

// DOS 8.3 wildcard match (case-insensitive) over "NAME.EXT".
static bool wildmatch(const std::string& pat, const std::string& name) {
    size_t p = 0, n = 0, star = std::string::npos, mark = 0;
    auto up = [](char c){ return (char)std::toupper((unsigned char)c); };
    while (n < name.size()) {
        if (p < pat.size() && (pat[p] == '?' || up(pat[p]) == up(name[n]))) { ++p; ++n; }
        else if (p < pat.size() && pat[p] == '*') { star = p++; mark = n; }
        else if (star != std::string::npos) { p = star + 1; n = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

// DOS 8.3 match: the name and extension are matched separately, so "*.*" (and a
// bare "*") match names with no extension too, which a plain glob would not.
static bool dos83match(const std::string& pat, const std::string& name) {
    auto split = [](const std::string& s, std::string& nm, std::string& ext) {
        size_t d = s.find('.');
        if (d == std::string::npos) { nm = s; ext.clear(); }
        else { nm = s.substr(0, d); ext = s.substr(d + 1); }
    };
    std::string pn, pe, nn, ne;
    split(pat, pn, pe); split(name, nn, ne);
    if (pat.find('.') == std::string::npos) pe = "*";   // "FOO" / "*" also match any extension
    return wildmatch(pn, nn) && wildmatch(pe, ne);
}

bool Dos::find_first(const std::string& spec, uint16_t attr) {
    find_.clear(); find_pos_ = 0;
    std::string dir = spec, pat = "*";
    size_t s = spec.find_last_of("/\\:");
    if (s != std::string::npos) { pat = spec.substr(s + 1); dir = spec.substr(0, s + 1); }
    else { pat = spec; dir = ""; }
    if (pat.empty()) pat = "*";
    const uint16_t date = ((1993 - 1980) << 9) | (8 << 5) | 19, time = (12 << 11);
    bool wantDirs = (attr & 0x10) != 0;
    // "." and ".." in a real (non-root) subdirectory when directories are requested
    if (wantDirs && files_.normalize(dir.empty() ? "." : dir) != "\\") {
        find_.push_back({".", 0, true, date, time});
        find_.push_back({"..", 0, true, date, time});
    }
    std::string host = files_.host_path(dir.empty() ? "." : dir);
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(host, ec)) {
        bool is_dir = e.is_directory(ec);
        if (is_dir && !wantDirs) continue;             // dirs only when asked for (attr & 0x10)
        std::string dosname = e.path().filename().string();
        for (char& c : dosname) c = (char)std::toupper((unsigned char)c);
        if (!dos83match(pat, dosname)) continue;
        Found f; f.name = dosname; f.is_dir = is_dir;
        f.size = is_dir ? 0 : (uint32_t)e.file_size(ec);
        f.date = date; f.time = time;
        find_.push_back(f);
    }
    return !find_.empty();
}

void Dos::write_dta_entry() {
    const Found& f = find_[find_pos_++];
    uint16_t s = dta_seg_, o = dta_off_;
    for (int i = 0; i < 21; ++i) mem_.wb(s, o + i, 0);           // reserved (search state)
    mem_.wb(s, o + 21, f.is_dir ? 0x10 : 0x20);                 // attribute
    mem_.ww(s, o + 22, f.time);
    mem_.ww(s, o + 24, f.date);
    mem_.w16(Memory::phys(s, o + 26), f.size & 0xFFFF);
    mem_.w16(Memory::phys(s, o + 28), f.size >> 16);
    std::string nm = f.name; if (nm.size() > 12) nm.resize(12);
    for (size_t i = 0; i < nm.size(); ++i) mem_.wb(s, o + 30 + i, nm[i]);
    mem_.wb(s, o + 30 + nm.size(), 0);
}

bool Dos::handle(uint8_t n) {
    switch (n) {
        case 0x20: terminate(0); return true;           // terminate program
        case 0x21: return int21();
        case 0x16: {                                     // BIOS keyboard services
            uint8_t ah = cpu_.r[AX] >> 8;
            if (ah == 0x00 || ah == 0x10) {              // read a key -> AL=ascii, AH=scancode
                int c = getch(); if (c < 0) c = 0x1A;
                cpu_.r[AX] = (static_cast<uint16_t>(c ? 0x1C : 0) << 8) | (c & 0xFF);
            } else if (ah == 0x01 || ah == 0x11) {       // key available? -> ZF=0 and peek in AX
                cpu_.flags &= ~ZF; cpu_.r[AX] = 0x1C0D;  // pretend Enter is waiting
            } else if (ah == 0x02) {                     // shift status
                cpu_.sb(AX, 0);
            }
            return true;
        }
        case 0x2F:                                       // DOS multiplex
            if (cpu_.r[AX] == 0xAE00) cpu_.sb(AX, 0);     // no installable-command extension
            return true;
        case 0x10:                                       // BIOS video — accept and ignore
        case 0x1A:                                       // BIOS time
            return true;
        default:
            return int21_default(n);
    }
}

bool Dos::int21_default(uint8_t) { return true; }   // unknown INTs: no-op for now

bool Dos::int21() {
    uint8_t ah = cpu_.r[AX] >> 8;
    switch (ah) {
        case 0x00: terminate(0); return true;                       // terminate
        case 0x01: {                                                // read char from stdin, with echo -> AL
            int c = getch(); if (c < 0) c = 0x1A;                    // Ctrl-Z at EOF
            out(1, static_cast<char>(c)); cpu_.sb(AX, static_cast<uint8_t>(c)); return true;
        }
        case 0x02: out(1, cpu_.r[DX] & 0xFF); return true;          // output char in DL
        case 0x07:                                                  // read char, no echo, no Ctrl-C check
        case 0x08: {                                                // read char, no echo
            int c = getch(); cpu_.sb(AX, static_cast<uint8_t>(c < 0 ? 0x1A : c)); return true;
        }
        case 0x0A: {                                                // buffered line input at DS:DX
            uint16_t seg = cpu_.sreg[DS], off = cpu_.r[DX];
            uint8_t maxlen = mem_.rb(seg, off);
            uint8_t len = 0;
            for (;;) {
                int c = getch();
                if (c < 0 || c == '\r') { out(1, '\r'); out(1, '\n'); break; }
                if (c == 0x08) { if (len) { --len; out(1, '\b'); out(1, ' '); out(1, '\b'); } continue; }  // backspace
                if (len + 1 >= maxlen) continue;
                mem_.wb(seg, off + 2 + len, static_cast<uint8_t>(c)); ++len; out(1, static_cast<char>(c));
            }
            mem_.wb(seg, off + 2 + len, 0x0D);
            mem_.wb(seg, off + 1, len);
            return true;
        }
        case 0x04: return true;                                     // output to aux — ignore
        case 0x05: return true;                                     // output to printer — ignore
        case 0x06:                                                  // direct console I/O
            if ((cpu_.r[DX] & 0xFF) != 0xFF) { out(1, cpu_.r[DX] & 0xFF); }
            else { int c = getch(); if (c < 0) { cpu_.sb(AX, 0); cpu_.flags |= ZF; } else { cpu_.sb(AX, (uint8_t)c); cpu_.flags &= ~ZF; } }
            return true;
        case 0x09: {                                                // output '$'-terminated string at DS:DX
            uint16_t seg = cpu_.sreg[DS], off = cpu_.r[DX];
            for (int i = 0; i < 65535; ++i) {
                char c = static_cast<char>(mem_.rb(seg, off + i));
                if (c == '$') break;
                out(1, c);
            }
            cpu_.sb(AX, '$');
            return true;
        }
        case 0x0B: cpu_.sb(AX, 0xFF); return true;                  // check input status: char available
        case 0x0C: return true;                                     // flush + input — ignore
        case 0x0E: cpu_.sb(AX, 3); return true;                     // select drive -> report 3 drives
        case 0x19: cpu_.sb(AX, 0); return true;                     // get current drive -> A:
        case 0x1A: dta_seg_ = cpu_.sreg[DS]; dta_off_ = cpu_.r[DX]; return true;   // set DTA
        case 0x2F: cpu_.sreg[ES] = dta_seg_; cpu_.r[BX] = dta_off_; return true;   // get DTA -> ES:BX
        case 0x4E:                                                  // find first (DS:DX spec, CX attr)
            if (cpu_.r[CX] == 0x08) { cpu_.flags |= CF; cpu_.r[AX] = 18; return true; }  // no volume label
            if (find_first(read_asciiz(cpu_.sreg[DS], cpu_.r[DX]), cpu_.r[CX])) { write_dta_entry(); cpu_.flags &= ~CF; }
            else { cpu_.flags |= CF; cpu_.r[AX] = 18; }             // no more files
            return true;
        case 0x4F:                                                  // find next
            if (find_pos_ < find_.size()) { write_dta_entry(); cpu_.flags &= ~CF; }
            else { cpu_.flags |= CF; cpu_.r[AX] = 18; }
            return true;
        case 0x36:                                                  // get free disk space (DL drive)
            cpu_.r[AX] = 8;        // sectors per cluster
            cpu_.r[CX] = 512;      // bytes per sector
            cpu_.r[BX] = 0xFFFF;   // free clusters
            cpu_.r[DX] = 0xFFFF;   // total clusters
            return true;
        case 0x69: cpu_.flags &= ~CF; return true;                  // get/set disk serial: accept
        case 0x73: cpu_.flags |= CF; return true;                   // FAT32 free space: unsupported -> use 36h
        case 0x47: {                                                // get current directory (DL drive) -> DS:SI
            std::string c = files_.cwd();                            // DOS wants it without the leading backslash
            if (!c.empty() && c[0] == '\\') c = c.substr(1);
            for (size_t i = 0; i < c.size(); ++i) mem_.wb(cpu_.sreg[DS], cpu_.r[SI] + i, c[i]);
            mem_.wb(cpu_.sreg[DS], cpu_.r[SI] + c.size(), 0);
            cpu_.r[AX] = 0x0100; cpu_.flags &= ~CF; return true;
        }
        case 0x38: {                                                // get country info (DS:DX 34-byte buffer)
            uint16_t seg = cpu_.sreg[DS], off = cpu_.r[DX];
            for (int i = 0; i < 34; ++i) mem_.wb(seg, off + i, 0);
            mem_.ww(seg, off + 0, 0);        // date format: 0 = USA (m d y)
            mem_.wb(seg, off + 2, '$');       // currency symbol
            mem_.wb(seg, off + 7, ',');       // thousands separator
            mem_.wb(seg, off + 9, '.');       // decimal separator
            mem_.wb(seg, off + 11, '-');      // date separator
            mem_.wb(seg, off + 13, ':');      // time separator
            mem_.wb(seg, off + 17, 0);        // time format: 12-hour
            // case-map: a far routine that uppercases AL. Plant it in scratch and
            // point the country block at it (offset 18 = far pointer).
            const uint16_t cseg = 0x0090, coff = 0x0100;
            const uint8_t cm[] = { 0x3C,0x61, 0x72,0x06, 0x3C,0x7A, 0x77,0x02, 0x2C,0x20, 0xCB };  // cmp/jb/cmp/ja/sub/retf
            for (size_t i = 0; i < sizeof cm; ++i) mem_.wb(cseg, coff + i, cm[i]);
            mem_.ww(seg, off + 18, coff); mem_.ww(seg, off + 20, cseg);
            mem_.wb(seg, off + 22, ',');      // data-list separator
            cpu_.r[BX] = 1;                   // country code = USA
            cpu_.flags &= ~CF; return true;
        }
        case 0x58:                                                  // get/set allocation strategy / UMB link
            switch (cpu_.r[AX] & 0xFF) {
                case 0: cpu_.r[AX] = 0; break;        // get strategy -> first fit
                case 2: cpu_.r[AX] = 0; break;        // get UMB link -> not linked
                default: break;                       // set strategy / set UMB link: accept
            }
            cpu_.flags &= ~CF; return true;
        case 0x65:                                                  // get extended country info: report "not
            cpu_.r[AX] = 1; cpu_.flags |= CF; return true;          // supported" so callers (FreeCOM) keep
                                                                    // their own sane NLS fallback (valid
                                                                    // filename chars etc.) instead of our blank.
        case 0x71:                                                  // Windows long-filename API: not supported
            cpu_.r[AX] = 0x7100; cpu_.flags |= CF; return true;     // the standard "no LFN" answer
        case 0x33: cpu_.sb(DX, 0); cpu_.flags &= ~CF; return true;  // get/set Ctrl-Break flag -> off
        case 0x3B:                                                  // chdir (DS:DX)
            if (files_.chdir(read_asciiz(cpu_.sreg[DS], cpu_.r[DX]))) cpu_.flags &= ~CF;
            else { cpu_.flags |= CF; cpu_.r[AX] = 3; }
            return true;
        case 0x25: {                                                // set interrupt vector (AL=n) DS:DX
            uint8_t v = cpu_.r[AX] & 0xFF;
            mem_.w16(v * 4, cpu_.r[DX]);
            mem_.w16(v * 4 + 2, cpu_.sreg[DS]);
            return true;
        }
        case 0x30:                                                  // get DOS version: AL=major, AH=minor
            cpu_.r[AX] = (30 << 8) | 3;   // 3.30
            cpu_.r[BX] = 0; cpu_.r[CX] = 0;
            return true;
        case 0x35: {                                                // get interrupt vector (AL=n) -> ES:BX
            uint8_t v = cpu_.r[AX] & 0xFF;
            cpu_.r[BX] = mem_.r16(v * 4);
            cpu_.sreg[ES] = mem_.r16(v * 4 + 2);
            return true;
        }
        case 0x37:                                                  // get/set switch character
            if ((cpu_.r[AX] & 0xFF) == 0) { cpu_.sb(AX, 0); cpu_.r[DX] = (cpu_.r[DX] & 0xFF00) | '/'; }
            else cpu_.sb(AX, 0);
            return true;
        case 0x52:                                                  // get list of lists (SysVars) -> ES:BX
            cpu_.sreg[ES] = 0x0090; cpu_.r[BX] = 0x0000;            // a zeroed scratch area
            return true;
        case 0x29: {                                                // parse filename (DS:SI) into an FCB (ES:DI)
            uint16_t sseg = cpu_.sreg[DS], si = cpu_.r[SI];
            uint16_t eseg = cpu_.sreg[ES], di = cpu_.r[DI];
            while (mem_.rb(sseg, si) == ' ') ++si;                   // skip leading blanks
            uint8_t drive = 0;
            if (mem_.rb(sseg, si + 1) == ':') { drive = (std::toupper(mem_.rb(sseg, si)) - 'A') + 1; si += 2; }
            mem_.wb(eseg, di, drive);
            auto fill = [&](int at, int width, bool ext) {
                for (int i = 0; i < width; ++i) mem_.wb(eseg, di + at + i, ' ');
                int i = 0;
                while (i < width) { char c = static_cast<char>(mem_.rb(sseg, si)); if (!c || c == ' ' || c == '\r' || (!ext && c == '.') || c == '/' ) break; if (c=='.') break; mem_.wb(eseg, di + at + i, std::toupper(c)); ++si; ++i; }
                while (mem_.rb(sseg, si) && mem_.rb(sseg, si) != ' ' && mem_.rb(sseg, si) != '.' && mem_.rb(sseg, si) != '\r' && !ext) ++si;
            };
            fill(1, 8, false);
            if (mem_.rb(sseg, si) == '.') { ++si; fill(9, 3, true); }
            else { for (int i = 0; i < 3; ++i) mem_.wb(eseg, di + 9 + i, ' '); }
            cpu_.sb(AX, 0); cpu_.r[SI] = si;
            return true;
        }
        case 0x2A:                                                  // get date -> CX=year DH=month DL=day AL=dow
            cpu_.r[CX] = 1993; cpu_.r[DX] = (8 << 8) | 19; cpu_.sb(AX, 4); return true;
        case 0x2B: cpu_.sb(AX, 0); cpu_.flags &= ~CF; return true;      // set date -> accept
        case 0x2C:                                                  // get time -> CH=hr CL=min DH=sec DL=1/100
            cpu_.r[CX] = (12 << 8) | 0; cpu_.r[DX] = 0; return true;
        case 0x43: {                                                // get/set file attributes (AL=0 get, 1 set)
            if ((cpu_.r[AX] & 0xFF) == 0) {                          // get: also how a program tests existence
                if (files_.exists(read_asciiz(cpu_.sreg[DS], cpu_.r[DX]))) { cpu_.r[CX] = 0x20; cpu_.flags &= ~CF; }
                else { cpu_.flags |= CF; cpu_.r[AX] = 2; }
            } else cpu_.flags &= ~CF;                                // set: accept
            return true;
        }
        case 0x39: {                                                // create directory (DS:DX name)
            std::error_code ec;
            bool ok = std::filesystem::create_directory(files_.host_path(read_asciiz(cpu_.sreg[DS], cpu_.r[DX])), ec);
            if (ok && !ec) cpu_.flags &= ~CF; else { cpu_.flags |= CF; cpu_.r[AX] = 3; }
            return true;
        }
        case 0x3A: {                                                // remove directory (DS:DX name)
            std::error_code ec;
            bool ok = std::filesystem::remove(files_.host_path(read_asciiz(cpu_.sreg[DS], cpu_.r[DX])), ec);
            if (ok && !ec) cpu_.flags &= ~CF; else { cpu_.flags |= CF; cpu_.r[AX] = 5; }
            return true;
        }
        case 0x3C: {                                                // create file (CX attr, DS:DX name) -> handle
            int h = files_.create(read_asciiz(cpu_.sreg[DS], cpu_.r[DX]), cpu_.r[CX]);
            if (h < 0) { cpu_.flags |= CF; cpu_.r[AX] = -h; } else { cpu_.flags &= ~CF; cpu_.r[AX] = h; }
            return true;
        }
        case 0x3D: {                                                // open file (AL access, DS:DX name) -> handle
            int h = files_.open(read_asciiz(cpu_.sreg[DS], cpu_.r[DX]), cpu_.r[AX] & 0x03);
            if (h < 0) { cpu_.flags |= CF; cpu_.r[AX] = -h; } else { cpu_.flags &= ~CF; cpu_.r[AX] = h; }
            return true;
        }
        case 0x3E: {                                                // close handle
            int r = files_.close(cpu_.r[BX]);
            if (r < 0) { cpu_.flags |= CF; cpu_.r[AX] = -r; } else cpu_.flags &= ~CF;
            return true;
        }
        case 0x3F: {                                                // read (BX handle, CX bytes, DS:DX buf)
            uint16_t h = cpu_.r[BX], cnt = cpu_.r[CX], seg = cpu_.sreg[DS], off = cpu_.r[DX];
            int cfd;
            if (files_.is_console(h, cfd)) {                        // console read: a line, CR/LF terminated
                uint16_t k = 0;
                while (k < cnt) {
                    int c = getch();
                    if (c < 0) break;                               // EOF
                    if (c == '\r') { out(1,'\r'); mem_.wb(seg, off + k++, '\r'); if (k < cnt) { out(1,'\n'); mem_.wb(seg, off + k++, '\n'); } break; }
                    out(1, static_cast<char>(c)); mem_.wb(seg, off + k++, static_cast<uint8_t>(c));
                }
                cpu_.r[AX] = k; cpu_.flags &= ~CF; return true;
            }
            std::vector<uint8_t> buf(cnt);
            int n = files_.read(h, buf.data(), cnt);
            if (n < 0) { cpu_.flags |= CF; cpu_.r[AX] = -n; return true; }
            for (int i = 0; i < n; ++i) mem_.wb(seg, off + i, buf[i]);
            cpu_.r[AX] = n; cpu_.flags &= ~CF; return true;
        }
        case 0x40: {                                                // write (BX handle, CX bytes, DS:DX buf)
            uint16_t h = cpu_.r[BX], cnt = cpu_.r[CX], seg = cpu_.sreg[DS], off = cpu_.r[DX];
            int cfd;
            if (files_.is_console(h, cfd)) {
                for (uint16_t i = 0; i < cnt; ++i) out(cfd == 2 ? 2 : 1, static_cast<char>(mem_.rb(seg, off + i)));
                cpu_.r[AX] = cnt; cpu_.flags &= ~CF; return true;
            }
            std::vector<uint8_t> buf(cnt);
            for (uint16_t i = 0; i < cnt; ++i) buf[i] = mem_.rb(seg, off + i);
            int n = files_.write(h, buf.data(), cnt);
            if (n < 0) { cpu_.flags |= CF; cpu_.r[AX] = -n; return true; }
            cpu_.r[AX] = n; cpu_.flags &= ~CF; return true;
        }
        case 0x41: {                                                // delete file (DS:DX name)
            int r = files_.remove(read_asciiz(cpu_.sreg[DS], cpu_.r[DX]));
            if (r < 0) { cpu_.flags |= CF; cpu_.r[AX] = -r; } else cpu_.flags &= ~CF;
            return true;
        }
        case 0x42: {                                                // lseek (AL method, BX handle, CX:DX offset) -> DX:AX
            long off = (static_cast<long>(cpu_.r[CX]) << 16) | cpu_.r[DX];
            long pos = files_.seek(cpu_.r[BX], off, cpu_.r[AX] & 0xFF);
            if (pos < 0) { cpu_.flags |= CF; cpu_.r[AX] = 6; return true; }
            cpu_.r[AX] = pos & 0xFFFF; cpu_.r[DX] = (pos >> 16) & 0xFFFF; cpu_.flags &= ~CF;
            return true;
        }
        case 0x45: {                                                // dup handle
            int h = files_.dup(cpu_.r[BX]);
            if (h < 0) { cpu_.flags |= CF; cpu_.r[AX] = -h; } else { cpu_.flags &= ~CF; cpu_.r[AX] = h; }
            return true;
        }
        case 0x46: {                                                // dup2 (force BX onto CX)
            int r = files_.dup2(cpu_.r[BX], cpu_.r[CX]);
            if (r < 0) { cpu_.flags |= CF; cpu_.r[AX] = -r; } else cpu_.flags &= ~CF;
            return true;
        }
        case 0x44:                                                  // IOCTL: report 0/1/2 as console devices
            if ((cpu_.r[AX] & 0xFF) == 0 && cpu_.r[BX] <= 2) {
                cpu_.r[DX] = 0x0080 | (cpu_.r[BX] == 0 ? 0x01 : 0x02) | 0x0010;  // char device, stdin/stdout, is-con
                cpu_.flags &= ~CF; return true;
            }
            cpu_.flags &= ~CF; cpu_.r[DX] = 0; return true;
        case 0x48: {                                                // allocate memory (BX paragraphs)
            uint16_t paras = cpu_.r[BX];
            if (heap_next_ + paras > heap_end_) {                   // not enough: report largest free
                cpu_.flags |= CF; cpu_.r[AX] = 8;                   // INSUFFICIENT MEMORY
                cpu_.r[BX] = heap_end_ - heap_next_;
                return true;
            }
            cpu_.r[AX] = heap_next_; heap_next_ += paras;
            cpu_.flags &= ~CF; return true;
        }
        case 0x49: cpu_.flags &= ~CF; return true;                  // free memory: accept (bump never frees)
        case 0x4A: {                                                // resize block (ES:block, BX paragraphs)
            // A startup shrink to free memory for children always succeeds; a grow
            // is granted up to the arena, which is all the guest needs here.
            cpu_.flags &= ~CF; return true;
        }
        case 0x4B:                                                  // load & execute (AL=0) / load overlay
            if ((cpu_.r[AX] & 0xFF) == 0)
                return exec(read_asciiz(cpu_.sreg[DS], cpu_.r[DX]), cpu_.sreg[ES], cpu_.r[BX]);
            cpu_.flags |= CF; cpu_.r[AX] = 1; return true;          // other subfunctions unsupported
        case 0x4C: terminate(cpu_.r[AX] & 0xFF); return true;       // terminate with return code
        case 0x4D: cpu_.r[AX] = last_child_code_ & 0xFF; return true;  // get child return code
        default:
            std::fprintf(stderr, "[dos] unimplemented INT 21h AH=%02Xh at %04X:%04X\n",
                         ah, cpu_.sreg[CS], cpu_.ip);
            cpu_.flags |= CF;   // signal error to the guest
            return true;
    }
}

}  // namespace dosemu
