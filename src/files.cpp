#include "files.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace dosemu {

// DOS error codes (returned negated).
static constexpr int kFileNotFound = 2;
static constexpr int kAccessDenied = 5;
static constexpr int kInvalidHandle = 6;

int DosFiles::alloc() {
    for (int i = 3; i < 256; ++i) if (h_.find(i) == h_.end() || !h_[i].used) return i;
    return -1;
}

// Translate a DOS path to a host path under root_. Handles a drive letter, mixes of
// '\' and '/', and a leading-slash "absolute" path (relative to the root). The final
// component is resolved case-insensitively against the host, since DOS is.
std::string DosFiles::host_path(const std::string& dos_path) {
    std::string p = dos_path;
    if (p.size() >= 2 && p[1] == ':') p = p.substr(2);       // drop drive letter
    for (char& c : p) if (c == '\\') c = '/';
    std::string rel;
    if (!p.empty() && p[0] == '/') rel = p;                   // absolute under root
    else {                                                    // relative to cwd
        std::string cw = cwd_; for (char& c : cw) if (c == '\\') c = '/';
        rel = cw; if (rel.empty() || rel.back() != '/') rel += '/'; rel += p;
    }
    // Build the host path component by component, matching case-insensitively.
    fs::path host = root_;
    std::string cur;
    auto step = [&](const std::string& name) {
        if (name.empty() || name == ".") return;
        std::error_code ec;
        fs::path want = host / name;
        if (fs::exists(want, ec)) { host = want; return; }
        // case-insensitive scan of the directory
        for (auto& e : fs::directory_iterator(host, ec)) {
            std::string h = e.path().filename().string();
            if (h.size() == name.size() &&
                std::equal(h.begin(), h.end(), name.begin(),
                           [](char a, char b){ return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); })) {
                host = e.path(); return;
            }
        }
        host = want;   // does not exist yet (e.g. a file about to be created)
    };
    for (size_t i = 0; i <= rel.size(); ++i) {
        if (i == rel.size() || rel[i] == '/') { step(cur); cur.clear(); }
        else cur += rel[i];
    }
    return host.string();
}

std::string DosFiles::normalize(const std::string& in) const {
    std::string p = in;
    if (p.size() >= 2 && p[1] == ':') p = p.substr(2);       // drop drive
    for (char& c : p) if (c == '/') c = '\\';
    std::vector<std::string> comp;
    // start from root for an absolute path, else from the current directory
    std::string start = (!p.empty() && p[0] == '\\') ? std::string("\\") : cwd_;
    auto split = [&](const std::string& s) {
        std::string cur;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == '\\') { if (!cur.empty()) comp.push_back(cur); cur.clear(); }
            else cur += s[i];
        }
    };
    split(start); split(p);
    std::vector<std::string> out;
    for (auto& c : comp) {
        if (c == ".") continue;
        if (c == "..") { if (!out.empty()) out.pop_back(); continue; }
        out.push_back(c);
    }
    std::string r = "\\";
    for (size_t i = 0; i < out.size(); ++i) { r += out[i]; if (i + 1 < out.size()) r += '\\'; }
    return r;
}

bool DosFiles::chdir(const std::string& dir) {
    std::string norm = normalize(dir);
    std::string save = cwd_; cwd_ = "\\";       // resolve the normalized (absolute) path from root
    std::string host = host_path(norm);
    cwd_ = save;
    std::error_code ec;
    if (norm != "\\" && !std::filesystem::is_directory(host, ec)) return false;
    cwd_ = norm;
    return true;
}

int DosFiles::open(const std::string& dos_path, int access) {
    const char* mode = access == 0 ? "rb" : access == 1 ? "r+b" : "r+b";
    std::string hp = host_path(dos_path);
    std::FILE* fp = std::fopen(hp.c_str(), mode);
    if (!fp && access != 0) fp = std::fopen(hp.c_str(), "rb");   // fall back to read
    if (!fp) return -kFileNotFound;
    int h = alloc(); if (h < 0) { std::fclose(fp); return -kAccessDenied; }
    h_[h] = {fp, false, 0, true};
    return h;
}

int DosFiles::create(const std::string& dos_path, int) {
    std::string hp = host_path(dos_path);
    std::FILE* fp = std::fopen(hp.c_str(), "w+b");
    if (!fp) return -kAccessDenied;
    int h = alloc(); if (h < 0) { std::fclose(fp); return -kAccessDenied; }
    h_[h] = {fp, false, 0, true};
    return h;
}

int DosFiles::close(int handle) {
    auto it = h_.find(handle);
    if (it == h_.end() || !it->second.used) return -kInvalidHandle;
    if (!it->second.console && it->second.fp) std::fclose(it->second.fp);
    if (handle > 2) h_.erase(it); else it->second.used = true;
    return 0;
}

int DosFiles::read(int handle, uint8_t* dst, int len) {
    auto it = h_.find(handle);
    if (it == h_.end() || !it->second.used) return -kInvalidHandle;
    if (it->second.console) return 0;   // stdin handled by the DOS layer
    return static_cast<int>(std::fread(dst, 1, len, it->second.fp));
}

int DosFiles::write(int handle, const uint8_t* src, int len) {
    auto it = h_.find(handle);
    if (it == h_.end() || !it->second.used) return -kInvalidHandle;
    if (it->second.console) return len;   // routed by the DOS layer
    return static_cast<int>(std::fwrite(src, 1, len, it->second.fp));
}

long DosFiles::seek(int handle, long offset, int whence) {
    auto it = h_.find(handle);
    if (it == h_.end() || !it->second.used || it->second.console) return -kInvalidHandle;
    int w = whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET;
    if (std::fseek(it->second.fp, offset, w) != 0) return -kInvalidHandle;
    return std::ftell(it->second.fp);
}

int DosFiles::remove(const std::string& dos_path) {
    return std::remove(host_path(dos_path).c_str()) == 0 ? 0 : -kFileNotFound;
}

bool DosFiles::exists(const std::string& dos_path) {
    std::error_code ec; return fs::exists(host_path(dos_path), ec);
}

int DosFiles::dup(int handle) {
    auto it = h_.find(handle);
    if (it == h_.end() || !it->second.used) return -kInvalidHandle;
    int h = alloc(); if (h < 0) return -kAccessDenied;
    h_[h] = it->second;   // shares the FILE* (close bookkeeping is loose; fine for the driver)
    return h;
}

int DosFiles::dup2(int from, int to) {
    auto it = h_.find(from);
    if (it == h_.end() || !it->second.used) return -kInvalidHandle;
    h_[to] = it->second;
    return to;
}

bool DosFiles::is_console(int handle, int& con_fd) const {
    auto it = h_.find(handle);
    if (it == h_.end() || !it->second.used || !it->second.console) return false;
    con_fd = it->second.con_fd; return true;
}

}  // namespace dosemu
