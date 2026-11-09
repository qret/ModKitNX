// sdk из установленных игр («Найти sdk в других играх»): запись о sdk одной игры, кэш проверенных игр,
// выбор кандидатов для прошивки (рекомендованная первой, затем ближайшие; дубли — одной строкой), имя папки,
// сохранение в папку sdk приложения с source.json рядом и чтение подписи «из игры …».
// Ничего не скачивается и не раздаётся: файл берётся из exefs игры, которая уже стоит на консоли.
// Ввод-вывод — только через IFileSystem (платформа монтирует exefs каждой игры и даёт путь к её sdk).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mk/core.hpp"
#include "mk/sdk.hpp"

#include <optional>

namespace mk::harvest {

// ------------------------------------------------------------ одна игра (запись кэша)
struct GameSdk {
    std::string tid;            // 16 hex заглавными
    std::string name;           // название игры (NACP)
    std::string version;        // display_version, «1.4.0»
    std::string key;            // признак содержимого игры (платформа: content id program NCA + номер обновления);
                                // изменился — игра обновлена или переустановлена, пересканировать
    bool has_sdk = false;       // в exefs есть файл sdk и это NSO
    bool nnsdk = false;         // модуль nnSdk
    std::string module_name;    // имя модуля (для «не nnSdk»)
    std::optional<Ver3> sdk_version;   // SDK MW
    std::string module_id, sha256;     // hex
    uint64_t size = 0;
    bool zbic = false;          // файл сжат ZBIC (бывает только у sdk 22+)
    std::string checked;        // когда проверено (iso)
};

// Прочитать sdk игры по пути (обычно <exefs>/sdk). Нет файла — has_sdk=false. Файл есть, но не читается как
// NSO (в т.ч. временная ошибка чтения) — mk::Error("SDK_UNREADABLE"); ошибки чтения — mk::Error (запись с
// ошибкой в кэш не кладётся: в следующий раз игра проверится снова).
GameSdk probe_sdk_file(Env& env, const std::string& sdk_path, GameSdk base);

// ------------------------------------------------------------ кэш sdmc:/switch/<APP>/cache/sdk-scan.json
struct Cache {
    std::vector<GameSdk> games;
};
constexpr int kCacheSchema = 1;
// Битый файл, чужая схема, неверные типы полей — пустой кэш (просто проверим всё заново); не бросает.
Cache parse_cache(const Bytes& data);
Bytes dump_cache(const Cache& c);   // JSON, отступ 2, UTF-8, \n в конце
Cache load_cache(Env& env, const std::string& path);   // нет файла — пустой
void save_cache(Env& env, const std::string& path, const Cache& c);   // папки создаются; свой файл — перезапись
// Запись той же игры с тем же признаком содержимого — пересканировать не нужно.
const GameSdk* cache_hit(const Cache& c, const std::string& tid, const std::string& key);
void cache_put(Cache& c, const GameSdk& g);   // заменить запись игры (по tid) или добавить

// ------------------------------------------------------------ выбор кандидатов
struct Candidate {
    Ver3 version{};
    std::string sha256, module_id;   // одинаковые sha256 или ModuleId — одна строка (как mk::scan_sdk)
    uint64_t size = 0;
    bool zbic = false;
    std::vector<GameSdk> games;      // игры, в которых лежит этот файл; первая — источник (по ней имя папки)
    bool recommended = false;        // версия из таблицы ядра fw_sdk_table для этой прошивки
    bool other_build = false;        // та же версия, другой файл (другая сборка)
};
struct Selection {
    std::optional<Ver3> firmware;
    std::optional<Ver3> table_sdk;   // что таблица ядра советует для прошивки (nullopt — строки нет)
    std::string table_source;
    std::vector<Candidate> candidates;   // порядок показа: рекомендованная, затем ближайшие (по убыванию версии)
    int games = 0;          // сколько записей пришло
    int with_nnsdk = 0;     // у скольких игр прочитана nnSdk с версией
    int too_new = 0;        // игр, чья sdk новее прошивки
    int already = 0;        // игр, чья sdk уже лежит в папках sdk
    int not_nnsdk = 0;      // файл sdk есть, но не nnSdk / без версии
    int older_hidden = 0;   // игр, чья sdk старее мажора прошивки и скрыта: есть sdk с мажором прошивки
};
// present — sdk, которые уже лежат в папках (sha256, ModuleId): такие не предлагаются.
// Без прошивки кандидатов нет (не понять, что подойдёт). Правило «подходит» — как evaluate_sdk_status:
// мажор sdk не новее мажора прошивки. Нижняя граница: если есть кандидат с мажором прошивки, кандидаты
// с мажором ниже не показываются (older_hidden); если такого нет — показываются все подходящие (последний шанс).
Selection select_candidates(const std::vector<GameSdk>& games, const std::optional<Ver3>& firmware,
                            const std::vector<std::pair<std::string, std::string>>& present);

// ------------------------------------------------------------ имя папки и сохранение
// «nnSdk 19.3.13 (Celeste)» — безопасно для FAT32/exFAT (mk::safe_folder_name), название игры укорачивается.
std::string folder_name(const Candidate& c);

struct SaveTarget {
    std::string dir, sdk_path, source_path;
    bool already = false;   // в этой папке уже лежит ровно этот файл (sha256) — сохранять нечего
};
// Куда сохранится (ничего не пишет): <root>/<folder_name>/sdk; занято другим файлом — « 2», « 3»…
SaveTarget save_target(Env& env, const std::string& root, const Candidate& c);

// source.json рядом с sdk: откуда взята (игра, версия, TitleID), версия sdk, sha256, дата, «из установленной игры».
nlohmann::ordered_json source_json(const Candidate& c, const std::string& saved_iso, const std::string& app);

struct SaveResult {
    bool ok = false;
    std::string code;       // "" | "changed" (файл в игре не тот, что найден) | "not_nnsdk" | "no_room" | код mk::Error
    std::string detail;     // текст ошибки (журнал)
    SaveTarget target;
};
// Прочитать src_path (sdk игры-источника), сверить sha256 с найденным, записать <dir>/sdk и <dir>/source.json
// (через .part; поверх чужого не пишет). Файл копируется как есть, побайтно.
SaveResult save_candidate(Env& env, const std::string& root, const Candidate& c, const std::string& src_path,
                          const std::string& app);

// Подпись для списка sdk: source.json рядом с файлом, если его sha256 совпал с файлом.
struct SourceInfo {
    std::string game, game_version, title_id, sdk_version;
};
std::optional<SourceInfo> read_source(Env& env, const std::string& sdk_path, const std::string& sha256);

}  // namespace mk::harvest
