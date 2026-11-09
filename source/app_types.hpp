// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — данные, которые видят экраны. Экраны не знают ни libnx, ни ядра: только эти структуры.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "platform/exefs.hpp"
#include "platform/sdk_compat.hpp"
#include "platform/zbic_policy.hpp"

namespace mk::app {

// Переводимый текст для экрана: key — полный ключ i18n ("core/plan/slot_free" — ядро,
// resources/i18n/<язык>/core.json; "app/..." — приложение, app.json), args — {0}, {1}…, sub — вложенные тексты
// (идут после args). Пустой key или ключ без перевода — показывается fallback (русский текст ядра).
// Переводит ui::trText (ui/common.hpp).
struct Text {
    std::string key;
    std::vector<std::string> args;
    std::vector<Text> sub;
    std::string fallback;
    bool empty() const { return key.empty() && fallback.empty(); }
};
inline Text textOf(std::string key, std::vector<std::string> args = {}) {
    Text t;
    t.key = std::move(key);
    t.args = std::move(args);
    return t;
}

// Строка списка игр.
struct GameRow {
    uint64_t id = 0;
    std::string tid;             // 16 hex
    std::string name;
    std::string version;         // display_version
    std::vector<uint8_t> icon;   // JPEG
    bool ourInstalled = false;   // есть наша квитанция nxredirect.json
    bool modsAvailable = false;  // есть папка модов приложения для этой игры
    plat::SdkNeed sdkNeed = plat::SdkNeed::Unknown;  // значок «нужна подмена sdk» / «sdk подменена»
    std::string gameSdk;         // sdk игры по заголовку NCA, «22.5.0»
};

// Найденная sdk (строка списка выбора).
struct SdkLine {
    std::string fileName;    // как лежит на карте
    std::string version;     // «19.3.13» — показывается крупно справа
    std::string path;        // что будет положено
    bool inOwn = false, inDbi = false;
    bool sameVersionOtherBuild = false;
    std::vector<std::string> allPaths;  // «Подробности»
    std::string fromGame, fromGameVersion;   // взята из установленной игры («Найти sdk в других играх»)
    std::string sha256;      // содержимое (ядро mk::scan_sdk); пусто — не известно
};

// Блок «sdk» на экране игры.
struct SdkBlock {
    std::string ncaSdk;          // (1) какое sdk требует игра — sdk_addon_version из program NCA
    std::string exefsSdk;        // (1) собственная sdk игры из exefs (SDK MW), через ядро
    bool swapPresent = false;    // (2) лежит atmosphere/contents/<TID>/exefs/sdk
    bool swapIsNnSdk = false;
    std::string swapVersion;
    bool swapOurs = false;       // положено нами (квитанция); иначе — чужое, не трогаем
    std::string firmware;        // (3)
    std::string atmosphere;      // (3)
    plat::SdkAdvice advice;      // (4)
};

// Блок «Что исправит модуль»: прогноз ядра (mk::forecast_fixes) для пары «exefs игры × sdk».
struct FixLine {
    std::string id, symbol, kind, scope;   // запись каталога nxr_autofix_catalog.h
    std::string choice, why;               // own|proxy и причина (SdkMissing, StubOlderSdk, StubVulkan…)
    bool sdkHas = false, sdkStub = false;
    std::vector<std::string> importers;    // модули игры, которые импортируют символ; пусто — не нужен игре
    std::string own;                       // что делает своя реализация (текст каталога, RU)
};
// Нужная игре функция, которой в этой sdk нет или там пустая заглушка (а в своей sdk игры — настоящая), и которую
// модуль восполнять не умеет (её нет в каталоге) — mk::sdk_gaps.
struct GapLine {
    std::string symbol;                    // как в .dynsym
    std::string kind;                      // MISSING | STUB
    bool weak = false;                     // слабый импорт: rtld оставит ноль, игра может проверить сама
    std::vector<std::string> importers;
};
struct FixForecastView {
    bool ok = false;                       // прогноз посчитан
    std::string sdkVersion;                // «19.3.13»
    bool sdkKnown = false;                 // это nnSdk
    bool swapped = false;                  // sdk старше библиотек игры
    std::vector<FixLine> fixes;
    std::vector<GapLine> gaps;             // пусто — пробелов нет (или не проверялось: FixesBlock::ownSdkOk)
    Text error;                            // почему не посчитан (ошибка ядра)
};
struct FixesBlock {
    bool gameOk = false;                   // сторона игры прочитана (импорты main/subsdk)
    std::string gameVersion;               // наибольшая SDK MW библиотек игры
    bool vulkan = false, opengl = false;
    std::string vulkanModule;
    bool ownSdkOk = false;                 // своя sdk игры прочитана — пробелы sdk посчитаны
    std::string ownSdkError;               // код ошибки ядра, если нет
    Text error;
    FixForecastView current;               // sdk, которая у игры сейчас (подменённая на карте, иначе родная)
    bool currentIsSwap = false;
    std::vector<FixForecastView> bySdk;    // по индексу SdkLine (найденные sdk) — если выбрана для подмены
};

struct ModLine {
    std::string id, title, path;
    bool inContents = false;     // уже стоит в atmosphere/contents/<TID>
    bool hasRomfs = false, hasOwnModule = false, hasIps = false;
};

struct GameDetails {
    bool ok = false;
    GameRow row;
    bool programFound = false;
    uint64_t programId = 0;         // из заголовка NCA; если не прочитан — id приложения
    bool programIdMismatch = false; // program id ≠ id приложения (многопрограммная игра)
    bool fromPatch = false;
    uint32_t patchVersion = 0;
    bool rsvKnown = false;
    std::string requiredFirmware;   // required_system_version из ncm, «как записано при установке»
    // Запись пустая (0 — так пишет DBI у патча): прошивка по sdk игры из заголовка NCA —
    // мажор sdk = мажор нужной прошивки (правило мажоров ядра, fw_sdk_table). 0 — неизвестно.
    int requiredFwBySdkMajor = 0;
    bool hasRightsId = false;

    plat::ExefsReport exefs;
    bool exefsOpened = false;

    bool coreOk = false;            // ядро ответило на inspect
    Text coreError;
    bool zbic = false;
    std::vector<std::string> zbicFiles;
    plat::ZbicNeed zbicNeed = plat::ZbicNeed::NeededAmsUnknown;  // правило platform/zbic_policy
    bool zbicUnpack = false;        // снимать ли ZBIC при установке (по правилу, без вопроса пользователю)
    bool zbicUnpackedOurs = false;  // ZBIC уже снят нами: несжатые файлы из квитанции (zbic_files) лежат в exefs
    bool ourInstalled = false;
    std::string slot, installedAt, installedBy;
    bool receiptStale = false;
    // Вид сборки стоящего модуля — по квитанции (diag, diag_gfx, module_variant; формат 1).
    bool debugInstalled = false;    // стоит отладочная сборка
    std::string moduleVariant;      // «autofix», «autofix-diag»…; "" — прежняя квитанция вида не называет
    // Что стоит по квитанции — чтобы понять, отличается ли выбор на экране (причина «Переустановить»).
    std::string installedModPath;   // mod.source — папка мода; "" — без мода
    std::string installedSdkSha;    // sdk_swap.sha256 нашей подмены; "" — нет или не записано

    SdkBlock sdk;
    FixesBlock fixes;
    std::vector<ModLine> mods;
    std::vector<Text> notes;
};

enum class OpKind { Install, Uninstall, SdkSwap, SdkUnswap };

// «Собрать логи для отправки» (mk::logs): папка и архив на карте.
struct LogsView {
    bool ok = false;
    Text error;
    std::string folder, zip;       // sdmc:/switch/<APP>/logs/<Игра> v<версия>/ и …/<Игра> v<версия> <дата>.zip
    std::vector<Text> items;       // что вошло
    std::vector<Text> warnings;    // чего нет (отчётов отладки, отчёта о падении, квитанции)
    size_t files = 0;
};

// Шаг плана — строка ядра (Plan::lines) с ключом перевода.
struct PlanItem {
    Text text;
};

struct PlanView {
    OpKind op = OpKind::Install;
    bool ok = false;
    std::string errorCode;
    Text error;                       // ошибка ядра (core/err/<код>) или приложения (app/error/<код>)
    std::vector<PlanItem> items;
    std::vector<Text> warnings;
};

struct ApplyView {
    bool ok = false;
    Text error;
    std::vector<Text> done, skipped, problems;
    std::string receiptPath;
    std::string logPath;
};

}  // namespace mk::app
