// Логи по играм и «Собрать логи для отправки»: переносимая часть без libnx.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Отладочный модуль nxredirect пишет отчёты в <logs>/<TitleID>/diag-NNNN.txt и gfx-NNNN.txt (последние 5 каждого
// вида; первая строка «TitleID=<16 HEX> BuildID=<40 HEX|?> report=<вид>-<NNNN>»; nxredirect/src/nxr_logs.hpp). Старые
// сборки модуля писали один файл в корень карты (sdmc:/nxredirect-diag.txt, -gfx.txt) — понимается как запасной.
// Сборка для отправки: папка <logs>/<Игра> v<версия>/ и архив <logs>/<Игра> v<версия> <ГГГГММДД-ЧЧММСС>.zip
// (без сжатия, store) — отчёты diag/gfx игры, ПОСЛЕДНИЙ отчёт Atmosphère о падении этой игры, паспорт консоли,
// квитанция. Ключей и файлов игры в сборке нет. Формат архива — LOGS-ZIP-FORMAT.
#pragma once

#include "mk/core.hpp"

#include <functional>
#include <memory>
#include <optional>

namespace mk::logs {

constexpr int kKeep = 5;   // сколько отчётов каждого вида хранит модуль на игру

// ---------------- отчёты отладочного модуля
struct ReportFile {
    std::string kind;        // "diag" | "gfx"
    uint32_t number = 0;     // NNNN
    std::string name, path;  // "diag-0005.txt", полный путь; для старого формата number = 0
    bool legacy = false;     // sdmc:/nxredirect-diag.txt (старые сборки модуля)
};
// «diag-0005.txt» -> ("diag", 5). Не меньше 4 цифр, вид diag|gfx, расширение .txt. false — не отчёт.
bool parse_report_name(const std::string& name, std::string& kind, uint32_t& number);
// Отчёты вида kind игры tid в <logs_root>/<TID>/ — от нового к старому (по номеру). Нет папки — пусто.
std::vector<ReportFile> list_reports(IFileSystem& fs, const std::string& logs_root, const std::string& tid,
                                     const std::string& kind);

// Первая строка отчёта (BOM UTF-8 пропускается).
struct ReportHead {
    bool ok = false;
    std::string tid, build_id, report;   // report — «diag-0005»
};
ReportHead parse_report_head(const std::string& text);
// Старый отчёт (без первой строки нового формата) относится к игре tid: в тексте есть её TitleID.
bool legacy_report_mentions(const std::string& text, const std::string& tid);

// ---------------- отчёты Atmosphère о падении (atmosphere/crash_reports/<время>_<TID>.log)
struct CrashFile {
    std::string name, path;
    std::optional<int64_t> mtime;   // дата изменения файла (секунды); нет — платформа не смогла узнать
};
using MtimeFn = std::function<std::optional<int64_t>(const std::string& path)>;
using ReadHeadFn = std::function<std::string(const std::string& path)>;   // начало файла (несколько КБ)

std::optional<uint64_t> leading_number(const std::string& name);   // «01699999999_…» -> 1699999999
std::string crash_tid_from_name(const std::string& name);           // 16 HEX заглавными или ""
std::string crash_tid_from_text(const std::string& text);           // строка «Program ID: …» или ""
// Файлы *.log папки (без подпапок) с датами от mtime.
std::vector<CrashFile> list_crash_files(IFileSystem& fs, const std::string& dir, const MtimeFn& mtime);
// Последний отчёт о падении игры tid. TitleID — из имени, нет в имени — из текста (read_head).
// Порядок: если дата есть у ВСЕХ кандидатов — по дате изменения, при равенстве — по числу в начале имени;
// если хоть у одного даты нет — по числу в начале имени. Последний довод — имя (побайтно).
std::optional<CrashFile> pick_last_crash(const std::vector<CrashFile>& files, const std::string& tid,
                                         const ReadHeadFn& read_head);

// ---------------- сборка для отправки
struct BundleInput {
    std::string tid;                    // 16 hex
    std::string game_name, game_version;
    std::string logs_root;              // "sdmc:/switch/ModKitNX/logs/"
    std::string crash_dir;              // "sdmc:/atmosphere/crash_reports/"
    std::string contents_dir;           // "sdmc:/atmosphere/contents/"
    std::string legacy_diag, legacy_gfx;   // "sdmc:/nxredirect-diag.txt", "sdmc:/nxredirect-gfx.txt"
    std::string passport_text;          // паспорт приложения — если модуль не положил свой nxredirect-passport.txt
    std::string stamp;                  // ГГГГММДД-ЧЧММСС (IClock::stamp)
    // Тексты приложения в сборку: {имя файла, текст} — например «exefs-read.txt» (журнал чтения кода игры).
    std::vector<std::pair<std::string, std::string>> app_texts;
};
struct BundleItem {
    std::string name;                        // имя внутри папки игры: diag-0005.txt, crash-<имя>.log, passport.txt …
    std::string src;                         // файл на карте; пусто — data
    std::shared_ptr<const Bytes> data;       // текст, собранный приложением (паспорт)
    std::optional<int64_t> mtime;            // дата для записи в zip (нет — время сборки)
    std::string what;                        // diag | gfx | crash | passport | receipt | app
};
struct Bundle {
    std::string inner;    // «<Игра> v<версия>» — папка внутри архива и на карте
    std::string folder;   // <logs_root><inner>/
    std::string zip;      // <logs_root><inner> <stamp>.zip
    std::vector<BundleItem> items;
    Texts lines, warnings;
};
// Что войдёт в сборку. Ничего не пишет.
Bundle plan_logs_bundle(Env& env, const BundleInput& in, const MtimeFn& mtime);
struct BundleResult {
    std::string folder, zip;
    size_t files = 0;
    uint64_t zip_size = 0;
};
// Копии в папку (свои файлы перезаписываются) и архив. Ничего не удаляет.
BundleResult write_logs_bundle(Env& env, const Bundle& b);

// ---------------- zip без сжатия (store; имена UTF-8, флаг 0x0800)
struct ZipEntry {
    std::string name;
    Bytes data;
    uint16_t dos_time = 0, dos_date = (1 << 5) | 1;   // 1980-01-01
};
Bytes zip_store(const std::vector<ZipEntry>& entries);
uint32_t crc32(const uint8_t* data, size_t size);
// Секунды (как в mtime, «настенное» время) -> поля DOS; до 1980 — 1980-01-01.
void dos_datetime(int64_t secs, uint16_t& dos_time, uint16_t& dos_date);
// «20261109-153000» -> поля DOS (не разобралось — 1980-01-01).
void dos_from_stamp(const std::string& stamp, uint16_t& dos_time, uint16_t& dos_date);

}  // namespace mk::logs
