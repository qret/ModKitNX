// Кэш показа (sdmc:/switch/<APP>/cache/): отпечатки файлов sdk, список игр, проверка игры.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Правило, которое держит всё: кэш влияет ТОЛЬКО на показ (список игр, страница игры, кнопки). План и запись
// (plan_install, plan_swap_sdk, plan_uninstall, apply_plan) кэш не читают — открывают exefs и файлы живьём.
// Квитанция, наши файлы в atmosphere/contents, моды, отчёты — не кэшируются (читаются живьём, это дёшево).
//
// Файлы (все — через write_file_replace: на FAT/exFAT rename поверх файла отказывает, N-12):
//   sdk-files.json      — отпечаток файла sdk (размер + mtime + sha256 первых 0x100 байт) -> результат identify_sdk;
//   games.json          — строка списка (название, версия, sdk игры по NCA) по признаку содержимого игры; подмена
//                         exefs/sdk — по отпечатку файла; иконки — icons/<TID>.jpg;
//   game/<TID>.json     — проверка игры (данные ядра по exefs: ZBIC, своя sdk, импорты — урезанные, стороны sdk по
//                         sha256), ключ — признак содержимого + прошивка + Atmosphère.
// Каждый файл несёт schema и app (версия приложения + отпечаток каталога автоисправлений): не совпало — пустой кэш.
// Битый JSON, чужие типы полей — пустой кэш (промах), без исключения наружу. Ошибки и временные сбои не кэшируются.
#pragma once

#include "mk/core.hpp"
#include "mk/sdk.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mk::cache {

constexpr int kSchema = 1;
constexpr size_t kHeadSize = 0x100;                 // сколько байт начала файла входит в отпечаток
constexpr uint64_t kMaxFileSize = 16ull << 20;      // больше — не читаем (не наш файл)
// Поднимает тот, кто меняет логику forecast_game / forecast_sdk / export_stubs / ro_lookup_kind / sdk_gaps:
// старые записи game/<TID>.json тогда не годятся (тестовые сборки одной APP_VERSION).
constexpr int kForecastRev = 1;

// ------------------------------------------------------------ ключи
// Отпечаток каталога автоисправлений и kForecastRev — 16 hex.
std::string forecast_fingerprint();
// Ключ app файла кэша: «<версия приложения>/<forecast_fingerprint>».
std::string app_key(const std::string& app_version);

// Отпечаток файла на карте. mtime может не быть (часы, ФС) — тогда решают размер и начало файла.
struct FileStamp {
    uint64_t size = 0;
    std::optional<int64_t> mtime;
    std::string head;   // sha256 первых kHeadSize байт (hex)
};
using MtimeFn = std::function<std::optional<int64_t>(const std::string&)>;
// Бросает Error ФС (нет файла, не читается).
FileStamp stamp_file(Env& env, const std::string& path, const MtimeFn& mtime);
bool same_stamp(const FileStamp& a, const FileStamp& b);

// ------------------------------------------------------------ sdk-files.json
struct SdkFilesCache {
    struct Item {
        FileStamp stamp;
        SdkFile file;   // результат identify_sdk (label и path подставляются при выдаче)
    };
    std::map<std::string, Item> files;   // путь -> запись
};
SdkFilesCache parse_sdk_files(const Bytes& data, const std::string& app);
Bytes dump_sdk_files(const SdkFilesCache& c, const std::string& app);
// Как scan_sdk, но файл с тем же отпечатком не читается целиком (SdkFile из кэша). Кэш обновляется: новые и
// изменённые файлы — заново, исчезнувшие пути выбрасываются. changed — кэш изменился (сохранить); hits/misses — для
// журнала. Не прочитанный файл (ошибка) в кэш не попадает.
struct ScanStats {
    size_t hits = 0, misses = 0;
    bool changed = false;
};
std::vector<SdkEntry> scan_sdk_cached(Env& env, const std::vector<SdkRoot>& roots, SdkFilesCache& cache,
                                      const MtimeFn& mtime, ScanStats* stats = nullptr, int max_depth = 2,
                                      std::vector<SdkFile>* rejected = nullptr);

// ------------------------------------------------------------ games.json
struct SwapInfo {          // подмена atmosphere/contents/<TID>/exefs/sdk — по отпечатку файла
    FileStamp stamp;
    bool nnsdk = false;
    std::string version;   // SDK MW, «17.5.0»; "" — нет строки версии
    std::string sha256;
};
struct GameEntry {
    std::string key;       // признак содержимого игры (content id program NCA + base/patch + версия)
    std::string name, version, game_sdk;
    bool icon = false;     // иконка лежит в icons/<TID>.jpg
    std::optional<SwapInfo> swap;
};
struct GamesCache {
    std::map<std::string, GameEntry> games;   // TID (16 hex) -> запись
};
GamesCache parse_games(const Bytes& data, const std::string& app);
Bytes dump_games(const GamesCache& c, const std::string& app);

// ------------------------------------------------------------ game/<TID>.json
// Урезание для кэша (результат прогноза тот же — хост-тест): из импортов игры остаются только те, что даёт своя sdk
// и не закрывают модули игры (только их смотрит sdk_gaps); экспорты модулей игры тогда не нужны.
ForecastGame compact_game(const ForecastGame& g);
// Сторона sdk — только символы, которые смотрит прогноз этой игры (импорты compact_game + символы каталога).
ForecastSdk restrict_sdk(const ForecastSdk& s, const ForecastGame& compacted);

nlohmann::json game_to_json(const ForecastGame& g);
bool game_from_json(const nlohmann::json& j, ForecastGame& out);   // false — не тот вид
nlohmann::json sdk_to_json(const ForecastSdk& s);
bool sdk_from_json(const nlohmann::json& j, ForecastSdk& out);

struct GameCheck {
    std::string key, fw, ams;                          // признак содержимого, прошивка, Atmosphère
    nlohmann::json exefs = nlohmann::json::object();   // «чем открыта exefs» (платформа; для подробностей)
    bool zbic = false;
    std::vector<std::string> zbic_files;
    std::map<std::string, std::string> module_ids;     // имя файла exefs -> ModuleId (installed_matches_game)
    ForecastGame game;                                 // compact_game
    std::optional<ForecastSdk> own_side;               // своя sdk игры (restrict_sdk); нет — не nnSdk/не прочитана
    std::map<std::string, ForecastSdk> by_sdk;         // sha256 sdk -> сторона (restrict_sdk)
};
// nullopt — промах: нет данных, чужая схема/app, другой ключ, прошивка или Atmosphère.
std::optional<GameCheck> parse_game_check(const Bytes& data, const std::string& app, const std::string& key,
                                          const std::string& fw, const std::string& ams);
Bytes dump_game_check(const GameCheck& c, const std::string& app);

// ------------------------------------------------------------ файлы
// Прочитать файл кэша: нет, слишком большой, не читается — пусто (не исключение).
Bytes read_cache_file(Env& env, const std::string& path);
// Записать (папки создаются; свой файл — перезапись write_file_replace).
void write_cache_file(Env& env, const std::string& path, const Bytes& data);
// Убрать файлы кэша в cache_dir: icons/<TID>.jpg и game/<TID>.json, чей TID не из keep (keep == nullptr — все), и
// при top_files — games.json, sdk-files.json, sdk-scan.json. Трогает только эти имена (16 hex + расширение);
// ничего другого в папке не удаляет. Возвращает число убранных; отказ одного файла — пропуск.
size_t remove_cache_files(Env& env, const std::string& cache_dir, const std::set<std::string>* keep, bool top_files);

}  // namespace mk::cache
