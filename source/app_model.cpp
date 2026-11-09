// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — модель приложения. См. app_model.hpp.
#include "app_model.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>

#include "core_bridge.hpp"
#include "harvest_view.hpp"
#include "platform/content.hpp"
#include "platform/core_fs.hpp"
#include "platform/exefs.hpp"
#include "platform/log.hpp"
#include "platform/paths.hpp"
#include "platform/sdk_harvest.hpp"
#include "platform/sdk_scan.hpp"
#include "platform/services.hpp"
#include "platform/titles.hpp"

namespace mk::app {

namespace {

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

// Версия файла exefs/sdk на карте (через ядро). invalid — нет файла или не nnSdk.
plat::Version3 swappedVersion(uint64_t tid, bool& present, bool& isNnSdk) {
    present = fileExists(swapPathFor(tid));
    isNnSdk = false;
    if (!present) return {};
    bridge::SdkProbe p;
    if (!bridge::identifySdk(swapPathFor(tid), p) || !p.isNnSdk) return {};
    isNnSdk = true;
    return plat::Version3::fromString(p.mwVersion);
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

std::vector<GameRow> Model::loadGames() {
    std::vector<GameRow> rows;
    std::vector<uint64_t> ids;
    if (!plat::listApplicationIds(ids)) return rows;
    const plat::Version3 fw = console().firmware;

    for (uint64_t id : ids) {
        plat::TitleControl tc;
        if (!plat::readTitleControl(id, tc)) continue;  // запись без установленного контента
        GameRow r;
        r.id = id;
        r.tid = plat::paths::tidHex(id);
        r.name = tc.name.empty() ? r.tid : tc.name;
        r.version = tc.displayVersion;
        r.icon = std::move(tc.iconJpeg);
        r.ourInstalled = fileExists(receiptPathFor(id));
        r.modsAvailable = fileExists(plat::paths::kModsDir + r.tid);

        plat::ProgramNca nca;
        plat::NcaHeaderInfo hdr;
        if (plat::findProgramNca(id, nca) && plat::readProgramNcaHeader(nca, hdr)) r.gameSdk = hdr.sdkAddon.toString();

        bool present = false, nn = false;
        const plat::Version3 sv = swappedVersion(id, present, nn);
        const plat::SdkAdvice a = plat::adviseSdk(hdr.sdkAddon, plat::Version3{}, fw, present, nn, sv, {});
        r.sdkNeed = a.need;
        rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end(), [](const GameRow& a, const GameRow& b) { return lower(a.name) < lower(b.name); });
    MK_LOG("model: %zu games listed", rows.size());
    return rows;
}

void Model::scanSdkLocked() {
    plat::SdkScanStats stats;
    sdk_ = plat::scanSdk(stats);  // опознание и склейка повторов — ядро (mk::scan_sdk)
    sdkScanned_ = true;
}

std::vector<SdkLine> Model::sdkList(bool reload) {
    std::lock_guard<std::mutex> lock(sdkMtx_);
    if (reload || !sdkScanned_) scanSdkLocked();
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
        out.push_back(std::move(l));
    }
    return out;
}

GameDetails Model::loadDetails(const GameRow& row) {
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
        if ((!d.rsvKnown || nca.requiredSystemVersion == 0) && hdr.sdkAddon.major > 0)
            d.requiredFwBySdkMajor = hdr.sdkAddon.major;
        d.hasRightsId = hdr.hasRightsId;
        if (hdr.programId != 0) d.programId = hdr.programId;
        d.programIdMismatch = (hdr.programId != 0 && hdr.programId != row.id);
    }

    // exefs — ядро смотрит ZBIC, свою установку, sdk игры.
    plat::ExefsMount mount;
    d.exefsOpened = d.programFound && mount.open(d.programId, nca, d.exefs);

    const bridge::GameCtx g = ctxFor(row, d.exefsOpened);

    // То, что видно без ядра: лежит ли exefs/sdk и квитанция.
    bool nn = false;
    const plat::Version3 sv = swappedVersion(row.id, d.sdk.swapPresent, nn);
    d.sdk.swapIsNnSdk = nn;
    d.sdk.swapVersion = sv.toString();
    d.ourInstalled = fileExists(receiptPathFor(row.id));

    bridge::inspectGame(g, d);  // при успехе уточняет поля (в т. ч. swapOurs, exefsSdk)
    d.mods = bridge::findMods(g);

    std::vector<plat::FoundSdk> found;
    {
        std::lock_guard<std::mutex> lock(sdkMtx_);
        if (!sdkScanned_) scanSdkLocked();
        found = sdk_;
    }
    // «Что исправит модуль»: импорты игры × sdk, которая будет у игры (подменённая на карте, иначе родная;
    // для каждой найденной — если её выбрать для подмены). Пока exefs смонтирована.
    {
        std::vector<std::pair<std::string, std::string>> cand;
        for (const auto& f : found) cand.emplace_back(f.path, f.sha256Hex);
        const std::string cur = d.sdk.swapPresent ? swapPathFor(row.id) : std::string(plat::ExefsMount::root()) + "sdk";
        bridge::forecastFixes(g, cur, d.sdk.swapPresent, cand, d);
    }
    mount.close();

    // Блок sdk: вывод простыми словами.
    d.sdk.advice = plat::adviseSdk(hdr.sdkAddon, plat::Version3::fromString(d.sdk.exefsSdk), ci.firmware,
                                   d.sdk.swapPresent, d.sdk.swapIsNnSdk, plat::Version3::fromString(d.sdk.swapVersion),
                                   found);
    // ZBIC снимается только если игра сжата И правило говорит «нужно». Если ядро не ответило — решает правило.
    d.zbicUnpack = plat::zbicUnpackNeeded(d.zbicNeed) && (d.coreOk ? d.zbic : true);
    d.row.sdkNeed = d.sdk.advice.need;
    d.row.ourInstalled = d.ourInstalled;
    d.ok = true;
    MK_LOG("model: details %s exefs=%s core=%d zbic=%d ours=%d sdkNeed=%d", row.tid.c_str(),
           plat::exefsViaName(d.exefs.via), d.coreOk, d.zbic, d.ourInstalled, static_cast<int>(d.sdk.advice.need));
    return d;
}

PlanView Model::plan(OpKind op, const GameDetails& d, const std::string& modPath, const std::string& sdkPath,
                     bool debugBuild) {
    // Установке нужна exefs игры; снятию и подмене sdk — нет, но открыть не вредно.
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
    {
        std::lock_guard<std::mutex> lock(sdkMtx_);
        sdkScanned_ = false;  // подмена sdk меняет картину — пересканировать при следующем показе
    }
    return v;
}

bool Model::writeConsolePassport(size_t gameCount) {
    const size_t sdkCount = sdkList(false).size();
    return plat::writePassport(plat::passportText(console(), sdkCount, gameCount));
}

LogsView Model::collectLogs(const GameDetails& d) {
    // Паспорт приложения — запасной: модуль кладёт свой (atmosphere/contents/<TID>/nxredirect-passport.txt).
    const std::string passport = plat::passportText(console(), sdkList(false).size(), 0);
    // Журнал чтения кода игры (шаги exefs с кодами) — отдельным файлом; ключей в нём нет.
    const std::string journal = plat::exefsJournalFor(d.programId ? d.programId : d.row.id);
    std::vector<std::pair<std::string, std::string>> texts;
    texts.emplace_back("exefs-read.txt", journal);
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

}  // namespace mk::app
