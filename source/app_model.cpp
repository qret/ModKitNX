// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — модель приложения. См. app_model.hpp.
#include "app_model.hpp"

#include <sys/stat.h>

#include <mk/cache.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

#include "core_bridge.hpp"
#include "harvest_view.hpp"
#include "sdk_check_text.hpp"
#include "platform/content.hpp"
#include "platform/core_fs.hpp"
#include "platform/exefs.hpp"
#include "platform/file_time.hpp"
#include "platform/forwarder.hpp"
#include "platform/log.hpp"
#include "platform/paths.hpp"
#include "platform/sdk_harvest.hpp"
#include "platform/sdk_scan.hpp"
#include "platform/services.hpp"
#include "platform/timing.hpp"
#include "platform/titles.hpp"

namespace mk::app {

namespace {

namespace C = mk::cache;

bool fileExists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string swapPathFor(uint64_t tid) { return plat::paths::contentsDirFor(tid) + "exefs/sdk"; }
std::string receiptPathFor(uint64_t tid) { return plat::paths::contentsDirFor(tid) + "nxredirect.json"; }
std::string iconPathFor(const std::string& tid) { return plat::paths::kIconsCacheDir + tid + ".jpg"; }

mk::Env env() { return mk::Env{plat::posixFs(), plat::nxHash(), plat::nxClock()}; }
const std::string& appKey() {
    static const std::string k = C::app_key(MK_APP_VERSION);
    return k;
}

// Кэш списка игр в памяти (games.json): один на процесс, под своим мьютексом. Только показ.
std::mutex g_gamesMtx;
bool g_gamesLoaded = false;
C::GamesCache g_games;
// После Y «Обновить» (полный проход мимо кэша) страница каждой игры один раз проверяется живьём и перезаписывает
// game/<TID>.json. Под g_gamesMtx.
std::set<std::string> g_liveCheckOnce;

void loadGamesCacheLocked() {
    if (g_gamesLoaded) return;
    mk::Env e = env();
    g_games = C::parse_games(C::read_cache_file(e, plat::paths::kGamesCache), appKey());
    g_gamesLoaded = true;
}

void saveGamesCacheLocked() {
    mk::Env e = env();
    try {
        C::write_cache_file(e, plat::paths::kGamesCache, C::dump_games(g_games, appKey()));
    } catch (const std::exception& ex) {
        MK_LOG("model: games cache not saved: %s", ex.what());   // кэш — только ускорение
    }
}

// Подмена exefs/sdk игры: лежит ли, nnSdk ли, версия, sha256. По отпечатку файла (размер, mtime, начало) — из кэша
// записи игры; другой файл или bypass — опознание целиком (и запись в кэш). Ошибка чтения в кэш не идёт.
struct SwapState {
    bool present = false, nn = false;
    std::string version, sha;
};
SwapState swapState(uint64_t id, C::GameEntry* entry, bool bypass, bool& dirty) {
    SwapState s;
    const std::string path = swapPathFor(id);
    s.present = fileExists(path);
    if (!s.present) {
        if (entry && entry->swap) {
            entry->swap.reset();
            dirty = true;
        }
        return s;
    }
    mk::Env e = env();
    std::optional<C::FileStamp> stamp;
    try {
        stamp = C::stamp_file(e, path, plat::fileMtime);
    } catch (const std::exception&) {
    }
    if (!bypass && entry && entry->swap && stamp && C::same_stamp(entry->swap->stamp, *stamp)) {
        s.nn = entry->swap->nnsdk;
        s.version = entry->swap->version;
        s.sha = entry->swap->sha256;
        return s;
    }
    bridge::SdkProbe p;
    if (bridge::identifySdk(path, p)) {
        s.nn = p.isNnSdk;
        s.version = p.isNnSdk ? p.mwVersion : "";
        s.sha = p.sha256;
        if (entry && stamp) {
            entry->swap = C::SwapInfo{*stamp, s.nn, s.version, s.sha};
            dirty = true;
        }
    } else if (entry && entry->swap) {
        entry->swap.reset();
        dirty = true;
    }
    return s;
}

// Метки строки — ОДИН способ для списка, строки после операции и страницы игры: квитанция (stat), моды (stat),
// подмена (swapState), значок sdk (adviseSdk по sdk игры из NCA и подмене, без списка найденных).
SwapState fillRowLabels(GameRow& r, const plat::Version3& fw, C::GameEntry* entry, bool bypass, bool& dirty) {
    r.ourInstalled = fileExists(receiptPathFor(r.id));
    r.modsAvailable = fileExists(plat::paths::kModsDir + r.tid);
    const SwapState s = swapState(r.id, entry, bypass, dirty);
    const plat::SdkAdvice a = plat::adviseSdk(plat::Version3::fromString(r.gameSdk), plat::Version3{}, fw, s.present,
                                              s.nn, plat::Version3::fromString(s.version), {});
    r.sdkNeed = a.need;
    return s;
}

bridge::GameCtx ctxFor(const GameRow& row, bool exefsOpen) {
    bridge::GameCtx g;
    g.titleId = row.id;
    g.tid = row.tid;
    g.displayVersion = row.version;
    g.exefsOpen = exefsOpen;
    return g;
}

// Открыть exefs игры заново (для плана и записи): program NCA → заголовок → Code FS.
bool openExefs(const GameDetails& d, plat::ExefsMount& mount) {
    plat::ProgramNca nca;
    if (!plat::findProgramNca(d.row.id, nca)) return false;
    plat::ExefsReport rep;
    return mount.open(d.programId, nca, rep);
}

}  // namespace

Model& Model::instance() {
    static Model m;
    return m;
}

void Model::startup() {
    servicesOk_ = plat::initServices();
    (void)console();
}

void Model::shutdown() { plat::exitServices(); }

plat::ConsoleInfo Model::console() { return plat::queryConsole(); }

std::vector<GameRow> Model::loadGames(bool bypassCache) {
    const uint64_t t0 = plat::nowMs();
    std::vector<GameRow> rows;
    std::vector<uint64_t> ids;
    if (!plat::listApplicationIds(ids)) return rows;
    const plat::Version3 fw = console().firmware;
    mk::Env e = env();
    uint64_t tNs = 0, tNcm = 0, tHdr = 0, tLabels = 0;
    size_t hits = 0, misses = 0;

    std::lock_guard<std::mutex> lock(g_gamesMtx);
    loadGamesCacheLocked();
    bool dirty = bypassCache;   // Y — перезаписать кэш свежим проходом
    std::set<std::string> present;
    for (uint64_t id : ids) {
        GameRow r;
        r.id = id;
        r.tid = plat::paths::tidHex(id);
        present.insert(r.tid);

        uint64_t t = plat::nowMs();
        plat::ProgramNca nca;
        const bool hasNca = plat::findProgramNca(id, nca);
        const std::string key = hasNca ? plat::contentKeyOf(nca) : std::string();
        tNcm += plat::nowMs() - t;

        C::GameEntry* entry = nullptr;
        auto it = g_games.games.find(r.tid);
        if (it != g_games.games.end()) entry = &it->second;
        bool hit = false;
        if (!bypassCache && entry && !key.empty() && entry->key == key) {
            hit = true;
            r.name = entry->name;
            r.version = entry->version;
            r.gameSdk = entry->game_sdk;
            if (entry->icon) {
                try {
                    r.icon = e.fs.read_file(iconPathFor(r.tid));
                } catch (const std::exception&) {
                    hit = false;   // иконки нет на карте — перечитать игру
                }
            }
        }
        if (hit) {
            ++hits;
        } else {
            ++misses;
            t = plat::nowMs();
            plat::TitleControl tc;
            const bool ok = plat::readTitleControl(id, tc);
            tNs += plat::nowMs() - t;
            if (!ok) continue;  // запись без установленного контента
            r.name = tc.name.empty() ? r.tid : tc.name;
            r.version = tc.displayVersion;
            r.icon = std::move(tc.iconJpeg);
            r.gameSdk.clear();
            t = plat::nowMs();
            plat::NcaHeaderInfo hdr;
            if (hasNca && plat::readProgramNcaHeader(nca, hdr)) r.gameSdk = hdr.sdkAddon.toString();
            tHdr += plat::nowMs() - t;
            if (!key.empty()) {
                C::GameEntry& ne = g_games.games[r.tid];   // подмену (по отпечатку) из прежней записи не теряем
                ne.key = key;
                ne.name = r.name;
                ne.version = r.version;
                ne.game_sdk = r.gameSdk;
                ne.icon = false;
                if (!r.icon.empty()) {
                    try {
                        C::write_cache_file(e, iconPathFor(r.tid), Bytes(r.icon.begin(), r.icon.end()));
                        ne.icon = true;
                    } catch (const std::exception& ex) {
                        MK_LOG("model: icon cache %s not saved: %s", r.tid.c_str(), ex.what());
                    }
                }
                entry = &ne;
                dirty = true;
            } else {
                entry = nullptr;   // без program NCA (картридж вынут) — без кэша
            }
        }
        t = plat::nowMs();
        (void)fillRowLabels(r, fw, entry, bypassCache, dirty);
        tLabels += plat::nowMs() - t;
        rows.push_back(std::move(r));
    }
    // Игр, которых больше нет, — из кэша вон (запись, иконка, проверка игры). Это кэш приложения, не данные
    // пользователя.
    for (auto it = g_games.games.begin(); it != g_games.games.end();) {
        if (!present.count(it->first)) {
            it = g_games.games.erase(it);
            dirty = true;
        } else {
            ++it;
        }
    }
    const size_t pruned = C::remove_cache_files(e, plat::paths::kCacheDir, &present, false);
    if (dirty) saveGamesCacheLocked();
    if (bypassCache) g_liveCheckOnce = present;
    std::sort(rows.begin(), rows.end(), [](const GameRow& a, const GameRow& b) { return lower(a.name) < lower(b.name); });
    MK_LOG("model: %zu games listed; cache %s: %zu hit, %zu read, %zu stale files removed", rows.size(),
           bypassCache ? "bypass" : "on", hits, misses, pruned);
    MK_LOG("model: step list ns=%llu ms ncm=%llu ms nca-header=%llu ms labels=%llu ms total=%llu ms",
           (unsigned long long)tNs, (unsigned long long)tNcm, (unsigned long long)tHdr, (unsigned long long)tLabels,
           plat::msSince(t0));
    return rows;
}

GameRow Model::refreshRow(const GameRow& base) {
    GameRow r = base;
    const plat::Version3 fw = console().firmware;
    std::lock_guard<std::mutex> lock(g_gamesMtx);
    loadGamesCacheLocked();
    auto it = g_games.games.find(r.tid);
    bool dirty = false;
    (void)fillRowLabels(r, fw, it != g_games.games.end() ? &it->second : nullptr, false, dirty);
    if (dirty) saveGamesCacheLocked();
    MK_LOG("model: row %s ours=%d mods=%d sdkNeed=%d", r.tid.c_str(), r.ourInstalled, r.modsAvailable,
           static_cast<int>(r.sdkNeed));
    return r;
}

void Model::scanSdkLocked(bool useCache) {
    plat::SdkScanStats stats;
    sdk_ = plat::scanSdk(stats, useCache);  // опознание и склейка повторов — ядро (mk::scan_sdk)
    sdkScanned_ = true;
}

std::vector<SdkLine> Model::sdkList(bool reload, bool bypassCache) {
    std::lock_guard<std::mutex> lock(sdkMtx_);
    if (reload || !sdkScanned_) scanSdkLocked(!bypassCache);
    std::vector<SdkLine> out;
    for (const auto& f : sdk_) {
        SdkLine l;
        l.fileName = f.fileName;
        l.version = f.versionText;
        l.path = f.path;
        l.inOwn = f.inOwn;
        l.inDbi = f.inDbi;
        l.sameVersionOtherBuild = f.sameVersionOtherBuild;
        l.allPaths = f.allPaths;
        l.fromGame = f.fromGame;
        l.fromGameVersion = f.fromGameVersion;
        l.sha256 = f.sha256Hex;
        out.push_back(std::move(l));
    }
    return out;
}

GameDetails Model::loadDetails(const GameRow& row, bool bypassCache) {
    const uint64_t t0 = plat::nowMs();
    GameDetails d;
    d.row = row;
    const plat::ConsoleInfo ci = console();
    d.sdk.firmware = ci.firmware.valid ? ci.firmwareDisplay : "";
    d.sdk.atmosphere = ci.amsKnown ? ci.atmosphere.toString() : "";
    d.zbicNeed = plat::zbicPolicy(ci.firmware, ci.amsKnown, ci.atmosphere);

    plat::ProgramNca nca;
    plat::NcaHeaderInfo hdr;
    d.programFound = plat::findProgramNca(row.id, nca);
    d.fromPatch = nca.fromPatch;
    d.patchVersion = nca.patchVersion;
    d.rsvKnown = nca.rsvKnown;
    if (nca.rsvKnown) d.requiredFirmware = plat::hosVersionText(nca.requiredSystemVersion);
    d.programId = row.id;
    if (d.programFound && plat::readProgramNcaHeader(nca, hdr)) {
        d.sdk.ncaSdk = hdr.sdkAddon.toString();
        d.row.gameSdk = d.sdk.ncaSdk;
        if ((!d.rsvKnown || nca.requiredSystemVersion == 0) && hdr.sdkAddon.major > 0)
            d.requiredFwBySdkMajor = hdr.sdkAddon.major;
        d.hasRightsId = hdr.hasRightsId;
        if (hdr.programId != 0) d.programId = hdr.programId;
        d.programIdMismatch = (hdr.programId != 0 && hdr.programId != row.id);
    }

    // Метки строки (то, что увидит список) и подмена sdk — тем же способом, что в списке.
    SwapState sw;
    {
        std::lock_guard<std::mutex> lock(g_gamesMtx);
        if (g_liveCheckOnce.erase(row.tid)) bypassCache = true;
        loadGamesCacheLocked();
        auto it = g_games.games.find(row.tid);
        bool dirty = false;
        sw = fillRowLabels(d.row, ci.firmware, it != g_games.games.end() ? &it->second : nullptr, bypassCache, dirty);
        if (dirty) saveGamesCacheLocked();
    }
    d.sdk.swapPresent = sw.present;
    d.sdk.swapIsNnSdk = sw.nn;
    d.sdk.swapVersion = sw.version;
    d.ourInstalled = d.row.ourInstalled;   // уточняет checkGame по квитанции

    std::vector<plat::FoundSdk> found;
    {
        std::lock_guard<std::mutex> lock(sdkMtx_);
        if (!sdkScanned_) scanSdkLocked();
        found = sdk_;
    }
    std::vector<std::pair<std::string, std::string>> cand;
    for (const auto& f : found) cand.emplace_back(f.path, f.sha256Hex);

    // Проверка игры: данные ядра по exefs — из кэша game/<TID>.json или из exefs (откроется только при промахе);
    // квитанция — живьём; «Что исправит модуль» — для текущей sdk и каждой найденной.
    bridge::CheckKeys keys;
    keys.key = d.programFound ? plat::contentKeyOf(nca) : std::string();
    keys.fw = ci.firmware.valid ? ci.firmware.toString() : "?";
    keys.ams = ci.amsKnown ? ci.atmosphere.toString() : "?";
    keys.useCache = !bypassCache;
    plat::ExefsMount mount;
    auto opener = [&](plat::ExefsReport& rep) { return d.programFound && mount.open(d.programId, nca, rep); };
    bridge::checkGame(ctxFor(row, false), keys, opener, sw.present ? swapPathFor(row.id) : std::string(), sw.sha, cand, d);
    mount.close();
    d.mods = bridge::findMods(ctxFor(row, false));

    // Блок sdk: вывод простыми словами.
    d.sdk.advice = plat::adviseSdk(hdr.sdkAddon, plat::Version3::fromString(d.sdk.exefsSdk), ci.firmware,
                                   d.sdk.swapPresent, d.sdk.swapIsNnSdk, plat::Version3::fromString(d.sdk.swapVersion),
                                   found);
    // ZBIC снимается только если игра сжата И правило говорит «нужно». Если ядро не ответило — решает правило.
    d.zbicUnpack = plat::zbicUnpackNeeded(d.zbicNeed) && (d.coreOk ? d.zbic : true);
    d.ok = true;
    MK_LOG("model: details %s exefs=%s cache=%d core=%d zbic=%d ours=%d sdkNeed=%d rowSdkNeed=%d %llu ms", row.tid.c_str(),
           plat::exefsViaName(d.exefs.via), d.checkFromCache, d.coreOk, d.zbic, d.ourInstalled,
           static_cast<int>(d.sdk.advice.need), static_cast<int>(d.row.sdkNeed), plat::msSince(t0));
    return d;
}

PlanView Model::plan(OpKind op, const GameDetails& d, const std::string& modPath, const std::string& sdkPath,
                     bool debugBuild) {
    // Установке нужна exefs игры; снятию и подмене sdk — нет, но открыть не вредно. Кэш здесь не участвует:
    // план строится по живым файлам.
    plat::ExefsMount mount;
    const bool open = openExefs(d, mount);
    PlanView v = bridge::makePlan(op, ctxFor(d.row, open), modPath, sdkPath, d.zbicUnpack, debugBuild);
    mount.close();
    return v;
}

ApplyView Model::apply(const GameDetails& d, const std::function<void(size_t, size_t, const PlanItem&)>& progress) {
    plat::ScopedNoSleep noSleep;  // не уснуть посреди записи 80 МБ
    plat::ExefsMount mount;
    const bool open = openExefs(d, mount);
    ApplyView v = bridge::applyPending(ctxFor(d.row, open), progress);
    mount.close();
    // Любой исход (и частичная запись при ошибке): проверка этой игры — заново при следующем показе.
    (void)bridge::dropGameCheck(d.row.tid);
    {
        std::lock_guard<std::mutex> lock(sdkMtx_);
        sdkScanned_ = false;  // подмена sdk меняет картину — пересканировать при следующем показе
    }
    return v;
}

size_t Model::resetCache() {
    mk::Env e = env();
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lock(g_gamesMtx);
        n = C::remove_cache_files(e, plat::paths::kCacheDir, nullptr, true);
        g_games = C::GamesCache{};
        g_gamesLoaded = true;   // файлов больше нет — пустой кэш, следующий список заполнит его заново
        g_liveCheckOnce.clear();
    }
    bridge::clearMemoryCache();
    {
        std::lock_guard<std::mutex> lock(sdkMtx_);
        sdkScanned_ = false;
    }
    MK_LOG("model: cache reset, %zu files removed", n);
    return n;
}

bool Model::writeConsolePassport(size_t gameCount) {
    const size_t sdkCount = sdkList(false).size();
    return plat::writePassport(plat::passportText(console(), sdkCount, gameCount));
}

LogsView Model::collectLogs(const GameDetails& d) {
    // Паспорт приложения — запасной: модуль кладёт свой (atmosphere/contents/<TID>/nxredirect-passport.txt).
    const std::string passport = plat::passportText(console(), sdkList(false).size(), 0);
    // Журнал чтения кода игры (шаги exefs с кодами) — отдельным файлом; ключей в нём нет.
    std::string journal = plat::exefsJournalFor(d.programId ? d.programId : d.row.id);
    // Страница показана из кэша проверки игры — exefs в этот раз не открывалась, шагов чтения нет.
    if (journal.empty() && d.checkFromCache)
        journal = "exefs was not opened this time: the game check was shown from the cache (" +
                  plat::paths::kGameCheckCacheDir + d.row.tid + ".json); via=" + plat::exefsViaName(d.exefs.via) + "\n";
    std::vector<std::pair<std::string, std::string>> texts;
    texts.emplace_back("exefs-read.txt", journal);
    // Проверка sdk со страницы игры, со списком «не восполнит» (раньше — только в выключенный журнал приложения).
    texts.emplace_back("sdk-check.txt", sdkCheckText(d.fixes));
    LogsView v = bridge::collectLogs(ctxFor(d.row, false), d.row.name, passport, true, texts);
    MK_LOG("model: logs %s ok=%d files=%zu zip=%s", d.row.tid.c_str(), v.ok, v.files, v.zip.c_str());
    return v;
}

HarvestView Model::findSdkInGames(const std::function<void(size_t, size_t, const std::string&)>& progress,
                                  const std::atomic<bool>* cancel) {
    const plat::HarvestScan scan = plat::scanInstalledGamesSdk(progress, cancel);
    std::vector<std::pair<std::string, std::string>> present;
    {
        std::lock_guard<std::mutex> lock(sdkMtx_);
        if (!sdkScanned_) scanSdkLocked();
        for (const auto& f : sdk_) present.emplace_back(f.sha256Hex, f.moduleIdHex);
    }
    const plat::ConsoleInfo ci = console();
    std::optional<mk::Ver3> fw;
    if (ci.firmware.valid) fw = mk::Ver3{ci.firmware.major, ci.firmware.minor, ci.firmware.micro};
    mk::Env env{plat::posixFs(), plat::nxHash(), plat::nxClock()};
    HarvestView v = harvestView(mk::harvest::select_candidates(scan.games, fw, present),
                                [&env](const mk::harvest::Candidate& c) {
                                    try {
                                        return mk::harvest::save_target(env, plat::paths::kSdkDirOwn, c);
                                    } catch (const mk::Error&) {
                                        return mk::harvest::SaveTarget{};
                                    }
                                });
    if (ci.firmware.valid) v.firmware = ci.firmwareDisplay;
    v.total = scan.total;
    v.checked = scan.checked;
    v.fromCache = scan.fromCache;
    v.failed = scan.failed;
    v.cancelled = scan.cancelled;
    if (scan.total == 0) {
        v.ok = false;
        v.error = textOf("sdkscan/err/no_games");
    }
    {
        std::lock_guard<std::mutex> lock(harvestMtx_);
        harvest_ = v.lines;
    }
    MK_LOG("model: sdk from games: %zu lines, checked %zu/%zu", v.lines.size(), v.checked, v.total);
    return v;
}

HarvestSaveView Model::saveSdkFromGames(size_t index) {
    HarvestLine line;
    {
        std::lock_guard<std::mutex> lock(harvestMtx_);
        if (index >= harvest_.size()) {
            HarvestSaveView v;
            v.error = textOf("sdkscan/err/no_exefs");
            return v;
        }
        line = harvest_[index];
    }
    const mk::harvest::Candidate c = candidateOf(line);
    const mk::harvest::SaveResult r = plat::saveHarvested(c);
    // Источником могла стать не первая игра строки (первая не открылась) — подпись по фактической папке.
    mk::harvest::Candidate shown = c;
    if (r.ok) {
        mk::Env env{plat::posixFs(), plat::nxHash(), plat::nxClock()};
        if (const auto src = mk::harvest::read_source(env, r.target.sdk_path, c.sha256)) {
            shown.games.front().name = src->game;
            shown.games.front().version = src->game_version;
        }
    }
    {
        std::lock_guard<std::mutex> lock(sdkMtx_);
        sdkScanned_ = false;   // новая sdk в папке — пересканировать при следующем показе
    }
    return harvestSaveView(r, shown);
}

// ---------------- ярлык на домашнем экране (Р-43) ----------------

namespace {
ForwarderResult forwarderResultOf(const plat::forwarder::Outcome& o) {
    ForwarderResult r;
    r.ok = o.ok;
    r.code = o.code;
    if (o.rc) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%X", o.rc);
        r.rc = buf;
    }
    return r;
}
}  // namespace

ForwarderView Model::forwarderStatus() {
    const plat::forwarder::Status s = plat::forwarder::status();
    ForwarderView v;
    v.known = servicesOk_;
    v.installed = s.installed;
    v.runningFromIt = s.runningFromIt;
    v.nroPresent = s.nroPresent;
    v.tid = plat::paths::tidHex(s.tid);
    return v;
}

ForwarderResult Model::createForwarder() { return forwarderResultOf(plat::forwarder::install()); }

ForwarderResult Model::removeForwarder() { return forwarderResultOf(plat::forwarder::remove()); }

}  // namespace mk::app
