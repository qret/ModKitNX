// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — PosixFs / NxHash / NxClock. См. platform/core_fs.hpp.
#include "platform/core_fs.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <switch.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "platform/log.hpp"

namespace mk::plat {

namespace {

// Путь без завершающего '/', но корень тома ("sdmc:/") не трогаем.
std::string norm(std::string p) {
    while (p.size() > 1 && p.back() == '/' && p[p.size() - 2] != ':') p.pop_back();
    return p;
}

std::string parentOf(const std::string& path) {
    const std::string p = norm(path);
    const size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) return "";
    if (slash > 0 && p[slash - 1] == ':') return p.substr(0, slash + 1);  // "sdmc:/"
    return p.substr(0, slash);
}

[[noreturn]] void fail(const char* code, const std::string& what, const std::string& path) {
    const std::string msg = what + ": " + path + " (errno " + std::to_string(errno) + ")";
    MK_LOG("fs: %s %s", code, msg.c_str());
    throw mk::Error(code, msg, {path});
}

struct File {
    FILE* f = nullptr;
    ~File() {
        if (f) std::fclose(f);
    }
};

bool statOf(const std::string& p, struct stat& st) { return ::stat(norm(p).c_str(), &st) == 0; }

}  // namespace

bool PosixFs::is_file(const std::string& path) {
    struct stat st;
    return statOf(path, st) && S_ISREG(st.st_mode);
}

bool PosixFs::is_dir(const std::string& path) {
    struct stat st;
    return statOf(path, st) && S_ISDIR(st.st_mode);
}

uint64_t PosixFs::file_size(const std::string& path) {
    struct stat st;
    if (!statOf(path, st) || !S_ISREG(st.st_mode)) fail("FS_NOT_FOUND", "нет файла", path);
    return static_cast<uint64_t>(st.st_size);
}

mk::Bytes PosixFs::read_file(const std::string& path) {
    const uint64_t size = file_size(path);
    return read_range(path, 0, static_cast<size_t>(size));
}

mk::Bytes PosixFs::read_range(const std::string& path, uint64_t off, size_t n) {
    File h;
    h.f = std::fopen(norm(path).c_str(), "rb");
    if (!h.f) fail("FS_READ", "не открывается на чтение", path);
    mk::Bytes out;
    if (fseeko(h.f, static_cast<off_t>(off), SEEK_SET) != 0) fail("FS_READ", "не перемещается позиция", path);
    out.resize(n);
    size_t got = 0;
    while (got < n) {
        const size_t r = std::fread(out.data() + got, 1, n - got, h.f);
        if (r == 0) break;
        got += r;
    }
    if (std::ferror(h.f)) fail("FS_READ", "ошибка чтения", path);
    out.resize(got);  // у конца файла — короче (контракт ядра)
    return out;
}

std::vector<mk::DirEntry> PosixFs::list_dir(const std::string& path) {
    std::vector<mk::DirEntry> out;
    const std::string p = norm(path);
    DIR* d = ::opendir(p.c_str());
    if (!d) fail("FS_LIST", "папка не открывается", path);
    const std::string base = (p.back() == '/') ? p : p + "/";
    while (struct dirent* e = ::readdir(d)) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
        mk::DirEntry de;
        de.name = e->d_name;
        struct stat st;
        if (::stat((base + de.name).c_str(), &st) == 0) {
            de.is_dir = S_ISDIR(st.st_mode);
            de.size = de.is_dir ? 0 : static_cast<uint64_t>(st.st_size);
        } else {
            de.is_dir = (e->d_type == DT_DIR);
        }
        out.push_back(std::move(de));
    }
    ::closedir(d);
    // Побайтная сортировка по имени — как sorted() в Python (контракт ядра).
    std::sort(out.begin(), out.end(), [](const mk::DirEntry& a, const mk::DirEntry& b) { return a.name < b.name; });
    return out;
}

void PosixFs::make_dirs(const std::string& path) {
    const std::string p = norm(path);
    const size_t dev = p.find(":/");
    const size_t start = (dev == std::string::npos) ? 0 : dev + 2;
    size_t pos = start;
    while (pos <= p.size()) {
        const size_t next = p.find('/', pos);
        const std::string sub = p.substr(0, next == std::string::npos ? p.size() : next);
        if (sub.size() > start) {
            struct stat st;
            if (::stat(sub.c_str(), &st) != 0) {
                if (::mkdir(sub.c_str(), 0777) != 0 && errno != EEXIST) fail("FS_MKDIR", "папка не создаётся", sub);
            } else if (!S_ISDIR(st.st_mode)) {
                fail("FS_MKDIR", "на месте папки лежит файл", sub);
            }
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
}

namespace {
// .part целиком готов: прежний файл (если есть и replace) убирается, .part встаёт на место. Без replace
// существующая цель — ошибка (на FAT/exFAT rename поверх файла не работает; молча стирать нельзя).
void placePart(PosixFs& fs, const std::string& part, const std::string& p, bool replace) {
    if (fs.is_file(p)) {
        if (!replace) fail("FS_EXISTS", "цель уже есть, перезапись не делается", p);
        if (::unlink(p.c_str()) != 0) fail("FS_REMOVE", "прежний файл не убирается перед перезаписью", p);
        MK_LOG("fs: replaced %s", p.c_str());
    }
    if (std::rename(part.c_str(), p.c_str()) != 0) fail("FS_RENAME", "не переименовывается", part);
}
}  // namespace

void PosixFs::write_file(const std::string& path, const uint8_t* data, size_t size) {
    writeImpl(path, data, size, false);
}
void PosixFs::write_file_replace(const std::string& path, const uint8_t* data, size_t size) {
    writeImpl(path, data, size, true);
}
void PosixFs::copy_file(const std::string& src, const std::string& dst) { copyImpl(src, dst, false); }
void PosixFs::copy_file_replace(const std::string& src, const std::string& dst) { copyImpl(src, dst, true); }

void PosixFs::writeImpl(const std::string& path, const uint8_t* data, size_t size, bool replace) {
    const std::string p = norm(path);
    const std::string parent = parentOf(p);
    if (!parent.empty()) make_dirs(parent);
    const std::string part = p + ".part";
    {
        File h;
        h.f = std::fopen(part.c_str(), "wb");
        if (!h.f) fail("FS_WRITE", "не создаётся", part);
        if (size && std::fwrite(data, 1, size, h.f) != size) fail("FS_WRITE", "не записалось целиком", part);
        if (std::fflush(h.f) != 0) fail("FS_WRITE", "не сбросилось на карту", part);
    }
    if (!replace && is_file(p)) fail("FS_EXISTS", "цель уже есть, перезапись не делается", p);
    placePart(*this, part, p, replace);
}

void PosixFs::copyImpl(const std::string& src, const std::string& dst, bool replace) {
    const uint64_t size = file_size(src);
    const std::string d = norm(dst);
    const std::string parent = parentOf(d);
    if (!parent.empty()) make_dirs(parent);
    if (!replace && is_file(d)) fail("FS_EXISTS", "цель уже есть, перезапись не делается", d);
    const std::string part = d + ".part";
    {
        File in, out;
        in.f = std::fopen(norm(src).c_str(), "rb");
        if (!in.f) fail("FS_READ", "не открывается на чтение", src);
        out.f = std::fopen(part.c_str(), "wb");
        if (!out.f) fail("FS_WRITE", "не создаётся", part);
        std::vector<uint8_t> buf(1 << 20);
        uint64_t done = 0;
        while (done < size) {
            const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), size - done));
            const size_t r = std::fread(buf.data(), 1, want, in.f);
            if (r != want) fail("FS_READ", "файл короче, чем был", src);
            if (std::fwrite(buf.data(), 1, r, out.f) != r) fail("FS_WRITE", "не записалось целиком", part);
            done += r;
        }
        if (std::fflush(out.f) != 0) fail("FS_WRITE", "не сбросилось на карту", part);
    }
    placePart(*this, part, d, replace);
}

void PosixFs::rename(const std::string& from, const std::string& to) {
    const std::string t = norm(to);
    struct stat st;
    if (::stat(t.c_str(), &st) == 0) fail("FS_EXISTS", "цель переименования уже есть", t);
    const std::string parent = parentOf(t);
    if (!parent.empty()) make_dirs(parent);
    if (std::rename(norm(from).c_str(), t.c_str()) != 0) fail("FS_RENAME", "не переименовывается", from);
}

void PosixFs::remove_file(const std::string& path) {
    if (!is_file(path)) fail("FS_NOT_FOUND", "нет файла для удаления", path);
    if (::unlink(norm(path).c_str()) != 0) fail("FS_REMOVE", "файл не удаляется", path);
    MK_LOG("fs: removed file %s", path.c_str());
}

void PosixFs::remove_empty_dir(const std::string& path) {
    if (!is_dir(path)) fail("FS_NOT_FOUND", "нет папки", path);
    if (!list_dir(path).empty()) fail("FS_NOT_EMPTY", "папка не пуста — не удаляется", path);
    if (::rmdir(norm(path).c_str()) != 0) fail("FS_REMOVE", "папка не удаляется", path);
    MK_LOG("fs: removed empty dir %s", path.c_str());
}

mk::Digest NxHash::sha256(const uint8_t* data, size_t size) {
    mk::Digest d{};
    sha256CalculateHash(d.data(), data, size);
    return d;
}

std::string NxClock::iso_seconds() {
    const time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
    return buf;
}

std::string NxClock::stamp() {
    const time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tmv);
    return buf;
}

PosixFs& posixFs() {
    static PosixFs fs;
    return fs;
}
NxHash& nxHash() {
    static NxHash h;
    return h;
}
NxClock& nxClock() {
    static NxClock c;
    return c;
}

}  // namespace mk::plat
