// ModKitNX — IFileSystem/IClock на std::filesystem (ПК).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "host_fs.hpp"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace mkhost {

static fs::path P(const std::string& s) { return fs::u8path(s); }

bool StdFs::is_file(const std::string& p) { std::error_code e; return fs::is_regular_file(P(p), e); }
bool StdFs::is_dir(const std::string& p) { std::error_code e; return fs::is_directory(P(p), e); }

uint64_t StdFs::file_size(const std::string& p) {
    std::error_code e;
    auto n = fs::file_size(P(p), e);
    if (e) throw mk::Error("IO_STAT", "нет файла: " + p);
    return n;
}

mk::Bytes StdFs::read_file(const std::string& p) {
    std::ifstream f(P(p), std::ios::binary);
    if (!f) throw mk::Error("IO_READ", "не читается: " + p);
    f.seekg(0, std::ios::end);
    auto n = f.tellg();
    f.seekg(0);
    mk::Bytes b(static_cast<size_t>(n));
    if (n > 0) f.read(reinterpret_cast<char*>(b.data()), n);
    return b;
}

mk::Bytes StdFs::read_range(const std::string& p, uint64_t off, size_t n) {
    std::ifstream f(P(p), std::ios::binary);
    if (!f) throw mk::Error("IO_READ", "не читается: " + p);
    f.seekg(0, std::ios::end);
    uint64_t sz = uint64_t(f.tellg());
    if (off >= sz) return {};
    size_t take = size_t(std::min<uint64_t>(n, sz - off));
    mk::Bytes b(take);
    f.seekg(std::streamoff(off));
    f.read(reinterpret_cast<char*>(b.data()), std::streamsize(take));
    return b;
}

std::vector<mk::DirEntry> StdFs::list_dir(const std::string& p) {
    std::vector<mk::DirEntry> out;
    std::error_code e;
    for (auto& it : fs::directory_iterator(P(p), e)) {
        mk::DirEntry d;
        d.name = it.path().filename().u8string();
        d.is_dir = it.is_directory();
        d.size = d.is_dir ? 0 : it.file_size();
        out.push_back(d);
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.name < b.name; });
    return out;
}

void StdFs::make_dirs(const std::string& p) {
    std::error_code e;
    fs::create_directories(P(p), e);
    if (e) throw mk::Error("IO_MKDIR", "не создаётся папка: " + p);
}

// Как консоль (platform/src/core_fs.cpp): без replace существующая цель — ошибка. Так хост-тесты ловят
// перезапись, которую на FAT/exFAT консоли сделать нельзя.
static void host_write(const std::string& p, const uint8_t* d, size_t n, bool replace);

void StdFs::write_file(const std::string& p, const uint8_t* d, size_t n) { host_write(p, d, n, false); }
void StdFs::write_file_replace(const std::string& p, const uint8_t* d, size_t n) { host_write(p, d, n, true); }

static void host_write(const std::string& p, const uint8_t* d, size_t n, bool replace) {
    {
        std::error_code e0;
        if (!replace && fs::exists(P(p), e0)) throw mk::Error("IO_EXISTS", "цель уже есть, перезапись не делается: " + p);
    }
    std::string part = p + ".part";
    {
        std::ofstream f(P(part), std::ios::binary | std::ios::trunc);
        if (!f) throw mk::Error("IO_WRITE", "не пишется: " + part);
        f.write(reinterpret_cast<const char*>(d), std::streamsize(n));
        if (!f) throw mk::Error("IO_WRITE", "запись оборвалась: " + part);
    }
    std::error_code e;
    fs::rename(P(part), P(p), e);
    if (e) throw mk::Error("IO_RENAME", "не переименовывается: " + part);
}

void StdFs::copy_file(const std::string& s, const std::string& d) {
    mk::Bytes b = read_file(s);
    write_file(d, b.data(), b.size());
}

void StdFs::copy_file_replace(const std::string& s, const std::string& d) {
    mk::Bytes b = read_file(s);
    write_file_replace(d, b.data(), b.size());
}

void StdFs::rename(const std::string& a, const std::string& b) {
    std::error_code e;
    fs::rename(P(a), P(b), e);
    if (e) throw mk::Error("IO_RENAME", "не переименовывается: " + a);
}

void StdFs::remove_file(const std::string& p) {
    std::error_code e;
    if (!fs::is_regular_file(P(p), e)) throw mk::Error("IO_REMOVE", "не файл: " + p);
    fs::remove(P(p), e);
    if (e) throw mk::Error("IO_REMOVE", "не удаляется: " + p);
}

void StdFs::remove_empty_dir(const std::string& p) {
    std::error_code e;
    if (!fs::is_directory(P(p), e) || !fs::is_empty(P(p), e)) throw mk::Error("IO_RMDIR", "не пустая папка: " + p);
    fs::remove(P(p), e);
    if (e) throw mk::Error("IO_RMDIR", "не удаляется папка: " + p);
}

static std::string fmt(const char* f) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char b[32];
    std::strftime(b, sizeof b, f, &tm);
    return b;
}
std::string Clock::iso_seconds() { return fixed_iso.empty() ? fmt("%Y-%m-%dT%H:%M:%S") : fixed_iso; }
std::string Clock::stamp() { return fixed_stamp.empty() ? fmt("%Y%m%d-%H%M%S") : fixed_stamp; }

}  // namespace mkhost
