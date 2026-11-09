// ModKitNX core — базовые типы и интерфейсы платформы (ввод-вывод, хеш, часы).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ядро не знает libnx: всё, что касается файлов, идёт через IFileSystem.
// Пути — UTF-8 строки с разделителем '/'. Префикс тома задаёт платформа
// (на консоли "sdmc:/...", "code:/..." для exefs игры; на ПК — обычный путь).
// Ошибки — исключение mk::Error с постоянным кодом (как коды событий эталонной реализации,
// например "FAIL_NO_FREE_SLOT") и человеческим текстом.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace mk {

using Bytes = std::vector<uint8_t>;
using Digest = std::array<uint8_t, 32>;

// Переводимое сообщение ядра: ключ i18n (без префикса "core/": "plan/slot_free") и параметры.
// Шаблон перевода — resources/i18n/<язык>/core.json; {0}, {1}… — args по порядку, за ними — sub
// (вложенные сообщения, переведённые тем же способом). Русский текст ядра остаётся рядом как запасной
// (журнал, хост-тесты, сверка с эталоном).
struct Msg {
    std::string key;
    std::vector<std::string> args;
    std::vector<Msg> sub;
};

// Строки «человеческим языком» (план, предупреждения, итог записи): сам вектор — русские строки,
// как раньше (тесты, сверка с эталоном); add() кладёт рядом ключ для перевода. msg(i) — ключ строки i
// (пустой ключ — строка без перевода, показывается как есть).
class Texts : public std::vector<std::string> {
public:
    using std::vector<std::string>::vector;
    void add(std::string ru, std::string key, std::vector<std::string> args = {}, std::vector<Msg> sub = {}) {
        msgs_.resize(size());
        push_back(std::move(ru));
        msgs_.push_back(Msg{std::move(key), std::move(args), std::move(sub)});
    }
    void append(const Texts& o) {
        for (size_t i = 0; i < o.size(); ++i) {
            Msg m = o.msg(i);
            add(o[i], std::move(m.key), std::move(m.args), std::move(m.sub));
        }
    }
    Msg msg(size_t i) const {
        if (i < msgs_.size() && !msgs_[i].key.empty()) return msgs_[i];
        return Msg{};
    }
private:
    std::vector<Msg> msgs_;
};

// Ошибка ядра: code — постоянный машинный код; UI переводит его по ключу "core/err/<code>" с
// параметрами args() (и вложенным sub(), если есть); what() — русский текст для журнала.
class Error : public std::runtime_error {
public:
    Error(std::string code, const std::string& msg, std::vector<std::string> args = {}, std::vector<Msg> sub = {})
        : std::runtime_error(msg), code_(std::move(code)), args_(std::move(args)), sub_(std::move(sub)) {}
    const std::string& code() const noexcept { return code_; }
    const std::vector<std::string>& args() const noexcept { return args_; }
    const std::vector<Msg>& sub() const noexcept { return sub_; }
    Msg msg() const { return Msg{"err/" + code_, args_, sub_}; }
private:
    std::string code_;
    std::vector<std::string> args_;
    std::vector<Msg> sub_;
};

struct DirEntry {
    std::string name;   // только имя, без пути
    bool is_dir = false;
    uint64_t size = 0;  // для файлов
};

// Файловая система. Реализации: host/StdFs (std::filesystem) и platform (fsdev/fsFs*).
class IFileSystem {
public:
    virtual ~IFileSystem() = default;
    virtual bool is_file(const std::string& path) = 0;
    virtual bool is_dir(const std::string& path) = 0;
    bool exists(const std::string& path) { return is_file(path) || is_dir(path); }
    virtual uint64_t file_size(const std::string& path) = 0;                       // бросает Error
    virtual Bytes read_file(const std::string& path) = 0;                          // целиком
    virtual Bytes read_range(const std::string& path, uint64_t off, size_t n) = 0; // короче — у конца
    // Содержимое папки, отсортированное по имени побайтно (как sorted() в Python).
    virtual std::vector<DirEntry> list_dir(const std::string& path) = 0;
    virtual void make_dirs(const std::string& path) = 0;
    // Запись целиком. Реализация обязана писать атомарно: <path>.part -> rename. Цель уже есть — Error
    // (на FAT/exFAT rename поверх файла не работает; молча стирать нельзя).
    virtual void write_file(const std::string& path, const uint8_t* data, size_t size) = 0;
    virtual void copy_file(const std::string& src, const std::string& dst) = 0;
    // Перезапись: <path>.part пишется целиком, затем прежний файл убирается и .part встаёт на его место;
    // прежнего нет — как write_file/copy_file. Ядро зовёт их только для СВОЕГО файла (свои —
    // начисто, без .bak) и для чужого ПОСЛЕ его копии .bak-<время>.
    virtual void write_file_replace(const std::string& path, const uint8_t* data, size_t size) = 0;
    virtual void copy_file_replace(const std::string& src, const std::string& dst) = 0;
    virtual void rename(const std::string& from, const std::string& to) = 0;   // файл или папка
    // Удаление (снятие удаляет по-настоящему, без отложенных копий). Ядро зовёт
    // их только из плана снятия по квитанции, после подтверждения человеком в UI,
    // и только для файлов, чей sha256 совпал с записанным в квитанции.
    virtual void remove_file(const std::string& path) = 0;
    virtual void remove_empty_dir(const std::string& path) = 0;   // только пустую; иначе Error
};

// SHA-256. По умолчанию — программная (mk::soft_hash()); консоль может дать аппаратную.
class IHash {
public:
    virtual ~IHash() = default;
    virtual Digest sha256(const uint8_t* data, size_t size) = 0;
};

// Часы: время для квитанции и суффиксов .bak-/.removed-.
class IClock {
public:
    virtual ~IClock() = default;
    virtual std::string iso_seconds() = 0;  // "2026-11-08T21:05:03" (как isoformat(timespec="seconds"))
    virtual std::string stamp() = 0;        // "20261108-210503" (как STAMP эталона)
};

// Окружение, которое передаётся в функции ядра.
struct Env {
    IFileSystem& fs;
    IHash& hash;
    IClock& clock;
};

IHash& soft_hash();                                   // программная SHA-256 ядра
std::string hex(const uint8_t* p, size_t n, bool upper = false);
inline std::string hex(const Digest& d) { return hex(d.data(), d.size()); }
std::string sha256_hex(IHash& h, const Bytes& b);
std::string file_sha256(Env& env, const std::string& path);

// Пути: склейка через '/', имя, родитель.
std::string path_join(const std::string& a, const std::string& b);
std::string path_name(const std::string& p);
std::string path_parent(const std::string& p);

// Чтение little-endian.
inline uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint64_t rd64(const uint8_t* p) { return uint64_t(rd32(p)) | uint64_t(rd32(p + 4)) << 32; }
inline void wr32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
inline void wr64(uint8_t* p, uint64_t v) { wr32(p, uint32_t(v)); wr32(p + 4, uint32_t(v >> 32)); }

// Версия формата установки, которую ядро повторяет (пишется в квитанцию "version").
// На консоли берётся pipeline_version из romfs:/modules/manifest.json; это — значение по умолчанию.
constexpr const char* kPipelineVersion = "1.1";
constexpr const char* kReceiptName = "nxredirect.json";

}  // namespace mk
