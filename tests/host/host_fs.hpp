// ModKitNX — реализация интерфейсов ядра на ПК (std::filesystem) для хост-тестов и core_cli.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mk/core.hpp"

namespace mkhost {

class StdFs : public mk::IFileSystem {
public:
    bool is_file(const std::string& p) override;
    bool is_dir(const std::string& p) override;
    uint64_t file_size(const std::string& p) override;
    mk::Bytes read_file(const std::string& p) override;
    mk::Bytes read_range(const std::string& p, uint64_t off, size_t n) override;
    std::vector<mk::DirEntry> list_dir(const std::string& p) override;
    void make_dirs(const std::string& p) override;
    void write_file(const std::string& p, const uint8_t* d, size_t n) override;
    void copy_file(const std::string& s, const std::string& d) override;
    void write_file_replace(const std::string& p, const uint8_t* d, size_t n) override;
    void copy_file_replace(const std::string& s, const std::string& d) override;
    void rename(const std::string& a, const std::string& b) override;
    void remove_file(const std::string& p) override;
    void remove_empty_dir(const std::string& p) override;
};

// Часы: системное время или зафиксированное (для повторяемых тестов).
class Clock : public mk::IClock {
public:
    std::string fixed_iso, fixed_stamp;
    std::string iso_seconds() override;
    std::string stamp() override;
};

}  // namespace mkhost
