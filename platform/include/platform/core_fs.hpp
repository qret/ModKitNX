// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — реализации интерфейсов ядра на консоли (core/include/mk/core.hpp): IFileSystem, IHash, IClock.
//
// Одна файловая система на все тома: stdio/POSIX поверх устройств fsdev — sdmc:/, romfs:/ и mkexefs:/
// (exefs игры, смонтированная platform/exefs; запись туда отклонит сама ФС). Ошибки — mk::Error с кодом
// "FS_*". Запись — через <путь>.part и переименование (требование ядра).
// Удаление (remove_file / remove_empty_dir) есть для снятия по квитанции (настоящее удаление);
// ядро зовёт его только после подтверждения в UI и только для файлов со своим sha256.
#pragma once

#include <mk/core.hpp>

#include <string>

namespace mk::plat {

class PosixFs final : public mk::IFileSystem {
public:
    bool is_file(const std::string& path) override;
    bool is_dir(const std::string& path) override;
    uint64_t file_size(const std::string& path) override;
    mk::Bytes read_file(const std::string& path) override;
    mk::Bytes read_range(const std::string& path, uint64_t off, size_t n) override;
    std::vector<mk::DirEntry> list_dir(const std::string& path) override;
    void make_dirs(const std::string& path) override;
    void write_file(const std::string& path, const uint8_t* data, size_t size) override;
    void copy_file(const std::string& src, const std::string& dst) override;
    void write_file_replace(const std::string& path, const uint8_t* data, size_t size) override;
    void copy_file_replace(const std::string& src, const std::string& dst) override;
    void rename(const std::string& from, const std::string& to) override;
    void remove_file(const std::string& path) override;
    void remove_empty_dir(const std::string& path) override;

private:
    void writeImpl(const std::string& path, const uint8_t* data, size_t size, bool replace);
    void copyImpl(const std::string& src, const std::string& dst, bool replace);
};

// Аппаратный SHA-256 libnx.
class NxHash final : public mk::IHash {
public:
    mk::Digest sha256(const uint8_t* data, size_t size) override;
};

// Местное время консоли.
class NxClock final : public mk::IClock {
public:
    std::string iso_seconds() override;  // 2026-11-08T21:05:03
    std::string stamp() override;        // 20261108-210503
};

// Общие экземпляры на процесс.
PosixFs& posixFs();
NxHash& nxHash();
NxClock& nxClock();

}  // namespace mk::plat
