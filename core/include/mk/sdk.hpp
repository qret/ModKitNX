// sdk: опознание nnSdk по содержимому, поиск в двух местах без повторов, профиль игры,
// прогноз автоисправления и детектор заглушек (порт nxredirect/scripts/stubdiff.py), оценка статуса sdk игры,
// план подмены sdk игры (exefs/sdk) и её снятия.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mk/core.hpp"
#include "mk/nso.hpp"
#include "mk/plan.hpp"

#include <nlohmann/json.hpp>
#include <optional>

namespace mk {

using Ver3 = std::array<int, 3>;

// ------------------------------------------------------------ каталог автоисправления
// Тот же X-макрос NXR_FIX(...), что компилируется в модуль: core/src/nxr_autofix_catalog.h
// (копия nxredirect/src/nxr_autofix_catalog.h, сверяется с оригиналом хост-тестом).
struct AutofixFix {
    std::string id, symbol, kind /*MISSING|STUB*/, scope /*ANY|NO_VULKAN*/, history, own;
};
const std::vector<AutofixFix>& autofix_catalog();

// ------------------------------------------------------------ опознание
struct SdkFile {
    std::string path, label;            // label — "modkit" | "dbi" | "game" | любая подпись места
    bool nnsdk = false;                 // NSO0 + module_name == "nnSdk"
    std::string module_name;
    std::optional<Ver3> version;        // SDK MW
    std::string module_id, sha256;      // hex
    uint64_t size = 0;
};
// nullopt — не NSO или не разбирается.
std::optional<SdkFile> identify_sdk(Env& env, const std::string& path, const std::string& label);

struct SdkEntry {
    SdkFile file;                                    // первое найденное (порядок мест сохраняется)
    std::vector<std::pair<std::string, std::string>> sources;   // (label, path) — все места
    bool same_version_other_id = false;              // такая же версия MW при другом ModuleId
};
// Обход мест по порядку (подпапки до max_depth уровней), только nnSdk, повторы — по ModuleId и sha256.
// rejected — отбракованные NSO (не nnSdk: OpenGL, movie…; module_name заполнено) для «Подробностей».
// Место поиска — именованные поля, не std::pair: перепутанный порядок (метка, папка) в платформе молча давал
// «sdk не найдены» на консоли.
struct SdkRoot {
    std::string dir;     // папка, например sdmc:/switch/ModKitNX/sdk/
    std::string label;   // метка места (modkit, dbi) — попадает в SdkEntry::sources
};
std::vector<SdkEntry> scan_sdk(Env& env, const std::vector<SdkRoot>& roots, int max_depth = 2,
                               std::vector<SdkFile>* rejected = nullptr);

// ------------------------------------------------------------ stubdiff
bool is_return_const_stub(uint32_t i0, uint32_t i1);
// Профиль: модули, графика, версия родной sdk, ZBIC, cnmt.
nlohmann::ordered_json sdk_profile(Env& env, const std::string& exefs, const std::string* cnmt);
// Прогноз: что выберет модуль на консоли с этой sdk (own/proxy и почему).
nlohmann::ordered_json sdk_forecast(Env& env, const std::string& console_sdk, const std::string& exefs);
// Прогноз по частям — для экрана «Что исправит модуль» (то же правило decide, что у sdk_forecast).
// Сторона игры читается один раз, сторона sdk — для каждой sdk-кандидата; сам прогноз — без ввода-вывода,
// поэтому его можно считать и по готовым спискам символов (предпросмотр на ПК без файлов игр).
struct ForecastGame {
    std::optional<Ver3> version;          // наибольшая SDK MW среди main/subsdk*
    bool vulkan = false;                  // какой-то модуль игры определяет vkCreateInstance
    std::string vulkan_module;            // первый такой (main/subsdkN)
    bool opengl = false;                  // какой-то модуль определяет eglCreateWindowSurface
    // символ каталога -> модули игры, которые его импортируют (только символы каталога)
    std::map<std::string, std::vector<std::string>> imports;
};
struct ForecastSdk {
    std::optional<std::string> name;      // module_name
    bool known = false;                   // name == "nnSdk"
    std::optional<Ver3> version;
    std::map<std::string, bool> exports;  // символ каталога, определённый в sdk -> тело — заглушка «вернуть константу»
};
struct FixForecast {
    AutofixFix fix;
    std::string choice, why;              // как sdk_forecast: own|proxy и причина (SdkMissing, StubOlderSdk…)
    bool sdk_has = false, sdk_stub = false;
    std::vector<std::string> importers;   // пусто — игра символ не импортирует, модулю тут делать нечего
};
struct ModuleForecast {
    bool swapped = false;                 // sdk старше библиотек игры
    std::vector<FixForecast> fixes;       // порядок каталога
};
ForecastGame forecast_game(Env& env, const std::string& exefs);
ForecastSdk forecast_sdk(Env& env, const std::string& sdk_path);
ModuleForecast forecast_fixes(const ForecastGame& game, const ForecastSdk& sdk);
// Детектор: импорты игры, которых нет / заглушки в старой sdk.
nlohmann::ordered_json sdk_detect(Env& env, const std::string& old_sdk, const std::string& new_sdk,
                                  const std::string& exefs);
std::string hos_version_text(uint32_t v);           // ((v>>26)&0x3F).((v>>20)&0x3F).((v>>16)&0xF)
nlohmann::ordered_json read_cnmt(const Bytes& data, bool xml_hint);

// ------------------------------------------------------------ статус sdk игры
// Таблица «прошивка -> подходящая sdk» (одно место; источник у каждой строки).
struct FwSdkRow { int hos_major; Ver3 sdk; const char* source; };
const std::vector<FwSdkRow>& fw_sdk_table();

struct SdkStatusInput {
    std::optional<Ver3> game_sdk_nca;       // sdk_addon_version из NCA (major.minor.micro)
    std::optional<Ver3> game_sdk_exefs;     // SDK MW библиотеки sdk игры
    std::optional<Ver3> firmware;           // HOS консоли
    bool swapped_present = false;           // есть atmosphere/contents/<TID>/exefs/sdk
    std::optional<Ver3> swapped_version;    // его SDK MW
    std::vector<Ver3> available;            // найденные sdk (scan_sdk)
};
struct SdkStatus {
    std::string verdict;    // "not_needed" | "needed" | "swapped_ok" | "swapped_mismatch" | "unknown"
    std::string text_key;   // i18n-ключ "core/sdk/<...>"
    std::optional<Ver3> game_sdk, recommended, fw_max_sdk;
    std::string recommended_source;   // источник строки таблицы
};
SdkStatus evaluate_sdk_status(const SdkStatusInput& in);

// План подмены: выбранный файл -> <dest>/atmosphere/contents/<TID>/exefs/sdk + квитанция (sdk_swap).
Plan plan_swap_sdk(Env& env, const std::string& dest, const std::string& tid, const SdkFile& sdk,
                   const std::string& installed_by = "console");
// Снять подмену: exefs/sdk -> .removed-<stamp>, только если в квитанции записан sdk_swap
// и sha256 файла совпадает; квитанция дописывается.
Plan plan_unswap_sdk(Env& env, const std::string& dest, const std::string& tid);

}  // namespace mk
