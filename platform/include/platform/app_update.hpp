// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — обновление приложения из приложения. Код свой; идеи — у Langegen: TorrentShopNX
// (source/ui/MainMenu.cpp: фоновая проверка releases/latest, ассет .nro) и NX-torrent-player (app/update.cpp:
// загрузка в файл рядом, замена при выходе после romfsExit, перезапуск через envSetNextLoad). В отличие от них:
// сертификат сервера проверяется (у TorrentShopNX VERIFYPEER=0), скачанный файл сверяется с NACP и sha256.
//
// Как устроено:
//   1. проверка — GET https://api.github.com/repos/<repo>/releases/latest (один запрос за запуск; без сети — тихо);
//   2. разбор ответа, сравнение версии с APP_VERSION, выбор ассета <APP_NAME>.nro, sha256 ассета (поле digest
//      GitHub или строка «sha256 (<APP_NAME>.nro): `…`» в описании релиза, её пишет tools/release.ps1);
//   3. «Обновить» — скачать ассет в память, проверить (размер, заголовок NRO, NACP: имя и версия, sha256), записать
//      <свой .nro>.new через .part (полностью записан и перечитан);
//   4. замена — при выходе из приложения, когда romfs отмонтирован (работающий .nro открыт загрузчиком и romfs):
//      свой .nro → .old, .new → свой .nro; не вышло — .old обратно. .old убирается при следующем успешном
//      старте (cleanupAfterStart в init); .new не новее работающей версии — в .new.stale, не применяется.
//   5. «Перезапустить» — после замены (п. 4) свой же путь отдаётся загрузчику (envSetNextLoad) и выход идёт в него,
//      а не в HOME: nx-hbloader сразу грузит уже новую версию, без hbmenu. Обычный выход закрывает тайтл, как
//      раньше (exitToHomeMode). Почему так — у planRelaunch ниже.
//
// Части 1–4 без libnx здесь (platform/src/app_update.cpp) и проверяются хост-тестами
// (tests/host/app_update_tests.cpp); сеть и libnx — platform/src/app_update_nx.cpp (только Switch-сборка), в
// предпросмотре на ПК — подставные (preview/mock_update.cpp).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mk {
class IHash;
}

namespace mk::plat::update {

// Репозиторий релизов (qret/ModKitNX) и адрес проверки.
inline const char* const kRepo = "qret/ModKitNX";
std::string latestUrl();   // https://api.github.com/repos/qret/ModKitNX/releases/latest
std::string assetName();   // <APP_NAME>.nro

// ---------------------------------------------------------------- версии
struct Version {
    int major = 0, minor = 0, patch = 0;
    bool valid = false;
};
// "v1.2.3", "1.2.3", "1.2" (patch 0); хвост "-beta" отрезается. Не число — valid=false.
Version parseVersion(const std::string& s);
// Кандидат строго новее текущей. Нечитаемая версия кандидата — НЕ новее (кривой тег не должен навязать
// обновление всем).
bool isNewer(const std::string& candidate, const std::string& current);
bool sameVersion(const std::string& a, const std::string& b);

// ---------------------------------------------------------------- ответ GitHub
struct Release {
    bool ok = false;          // ответ разобран и это релиз (а не {"message": "Not Found"})
    std::string error;        // !ok: что не так (в журнал)
    std::string tag;          // "v0.2.0"
    std::string version;      // "0.2.0"
    std::string name;         // заголовок релиза
    std::string body;         // описание (markdown)
    bool draft = false, prerelease = false;
    std::string assetUrl;     // browser_download_url ассета <APP_NAME>.nro; "" — ассета нет
    uint64_t assetSize = 0;   // размер ассета по API; 0 — не сказано
    std::string sha256;       // ожидаемый sha256 ассета (hex, строчные); "" — неизвестен
    bool shaMismatch = false; // digest ассета и строка sha256 в описании расходятся (assetUrl тогда пуст)
};
// json — тело ответа releases/latest; asset — имя файла ассета (обычно assetName()).
Release parseRelease(const std::string& json, const std::string& asset);

// «Что нового» для окна: описание без служебной строки sha256, разделителя «---» и пустых строк, без разметки
// заголовков и **жирного**, не длиннее maxChars байт и maxLines строк (обрезка по границе строки или символа UTF-8, в конце «…»).
std::string notesForDisplay(const std::string& body, size_t maxChars = 700, size_t maxLines = 12);

// Что показать пользователю.
struct Decision {
    bool newer = false;        // версия релиза новее текущей
    bool skipped = false;      // пользователь пропустил именно эту версию («Пропустить эту версию»)
    bool installable = false;  // версия новее, есть ассет и известен его sha256: можно предложить «Обновить»
    bool fileMismatch = false; // версия новее, но файл не сверить: sha256 не дан или digest ≠ описание
};
Decision decide(const Release& r, const std::string& current, const std::string& skippedVersion);

// ---------------------------------------------------------------- проверка скачанного .nro
struct NroInfo {
    bool ok = false;
    std::string error;     // код причины: size, not_nro, truncated, no_assets, no_nacp, name, version, sha256
    std::string name;      // имя из NACP (первая непустая запись)
    std::string version;   // display version из NACP
};
// Разобрать NRO в памяти: "NRO0" @0x10, размер образа @0x18, за ним ASET, в нём NACP (0x4000).
NroInfo readNro(const uint8_t* data, size_t size);
// readNro + сверка с релизом: размер ассета (если API его дал), имя expectName в одной из записей NACP, версия
// NACP = версия релиза, sha256 (если известен). hash — программная mk::soft_hash() или аппаратная консоли.
NroInfo verifyNro(const uint8_t* data, size_t size, const Release& r, const std::string& expectName, mk::IHash& hash);

// ---------------------------------------------------------------- файлы рядом со своим .nro
// Путь своего .nro из argv[0]: "sdmc:/switch/X.nro" — как есть, "/switch/X.nro" — с "sdmc:"; не .nro — "".
std::string selfNroPath(const char* argv0);
inline std::string pendingPath(const std::string& self) { return self + ".new"; }
inline std::string partPath(const std::string& self) { return self + ".new.part"; }
inline std::string oldPath(const std::string& self) { return self + ".old"; }
inline std::string stalePath(const std::string& self) { return self + ".new.stale"; }
// argv для перезапуска (envSetNextLoad): путь в кавычках, если в нём пробел — argvSetup libnx режет по пробелам,
// а "…" в начале аргумента держит пробелы и снимает кавычки.
std::string restartArgv(const std::string& self);

// Перезапуск при выходе. Обычный выход закрывает тайтл: main включает exitToHomeMode borealis (это
// __nx_applet_exit_mode = 1 libnx), и тогда _appletCleanup (libnx nx/source/services/applet.c) подменяет адрес
// возврата в загрузчик на собственный выход процесса — envSetNextLoad при этом ни на что не влияет. Для перезапуска
// режим возвращается в 0: libnx отдаёт управление nx-hbloader, тот (loadNro, source/main.c) открывает g_nextNroPath
// (char[512]) с аргументами g_nextArgv (char[2048]). Нет файла или это не NRO — загрузчик падает
// (diagAbortWithResult), поэтому путь проверяется после замены, перед самым выходом.
inline constexpr size_t kLoaderPathMax = 512;    // nx-hbloader: g_nextNroPath, вместе с завершающим нулём
inline constexpr size_t kLoaderArgvMax = 2048;   // nx-hbloader: g_nextArgv, вместе с завершающим нулём
struct RelaunchPlan {
    bool relaunch = false;   // true — отдать path/argv загрузчику и выйти в него; false — закрыть тайтл
    std::string path, argv;
    // Причина для журнала: ok; not_requested — не просили; no_loader — загрузчик без next load; no_self — путь
    // своего .nro неизвестен; too_long — путь или argv не помещаются в буфер загрузчика; self_missing — файла нет;
    // not_nro — на месте не NRO (загрузчик бы упал).
    std::string reason;
};
// requested — нажато «Перезапустить»; loaderHasNextLoad — envHasNextLoad(); self — путь своего .nro.
// Читает заголовок self (первые 0x14 байт: "NRO0" @0x10).
RelaunchPlan planRelaunch(bool requested, bool loaderHasNextLoad, const std::string& self);

// Записать проверенный образ как <self>.new: сначала .new.part, перечитать и сверить, затем переименовать.
// false — error: код причины (write, readback, rename).
bool stagePending(const std::string& self, const std::vector<uint8_t>& data, std::string& error);
// Лежит ли целый .new (заголовок NRO и NACP) с версией строго новее current (обычно MK_APP_VERSION). Битый .new
// не применяется; целый, но не новее (залежался, а версию поставили иначе) — переименовывается в .new.stale
// (наш файл; прежний .new.stale заменяется), чтобы не откатить приложение.
bool hasPending(const std::string& self, const std::string& current);
// Заменить свой .nro на .new: self → .old, .new → self; не вышло — .old обратно. .old остаётся: его убирает
// cleanupAfterStart при следующем успешном старте. Зовётся при выходе, после romfsExit. true — заменено.
bool swapInPending(const std::string& self, const std::string& current, std::string& error);
// При старте (update::init): свой .nro на месте — убрать .old прошлой замены ("old_removed"; не удалось —
// "old_kept"). Своего .nro нет — ничего не трогать ("self_missing"). Нечего делать — "".
std::string cleanupAfterStart(const std::string& self);

// ---------------------------------------------------------------- сеть и консоль (app_update_nx.cpp / mock)
// Запомнить путь своего .nro (argv[0]); без него обновление выключено, а не угадывается.
void init(int argc, char** argv);
const std::string& selfPath();
// Есть ли подключение к интернету (nifm). Нет — проверка не начинается (без ожидания таймаутов).
bool networkUp();
// GET releases/latest и разбор. false — сеть/HTTP/разбор; error — для журнала.
bool fetchLatest(Release& out, std::string& error);
// Скачать ассет в память. progress(сделано, всего; всего 0 — неизвестно) с потока загрузки; вернуть false — стоп.
bool download(const Release& r, std::vector<uint8_t>& out, const std::function<bool(uint64_t, uint64_t)>& progress,
              std::string& error);
// Проверка образа аппаратным/программным sha256 платформы.
NroInfo verifyDownloaded(const std::vector<uint8_t>& data, const Release& r);
// При выходе: если лежит .new — отмонтировать romfs и заменить (swapInPending). Следом — relaunchAtExit.
void applyPendingAtExit();
// «Перезапустить»: запомнить просьбу (сам перезапуск — relaunchAtExit при выходе). false — загрузчик не умеет
// запускать следующий .nro или путь своего .nro неизвестен: закрыть и открыть вручную.
bool restartNow();
// При выходе, после applyPendingAtExit: по planRelaunch — envSetNextLoad и __nx_applet_exit_mode = 0 (выход в
// загрузчик, он сразу запустит свой .nro) или ничего (тайтл закрывается, как обычно). Всё — в журнал.
void relaunchAtExit();
// Выход приложения: прервать загрузку/проверку на следующем шаге и дождаться (не дольше ms) конца фоновой работы.
void abortAll();
bool waitIdle(int ms);
// Счётчик фоновой работы (сеть идёт): busyBegin/busyEnd вокруг каждого фонового запроса.
void busyBegin();
void busyEnd();
bool aborting();

}  // namespace mk::plat::update
