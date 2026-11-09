// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — «Найти sdk в других играх» на консоли. См. platform/sdk_harvest.hpp.
#include "platform/sdk_harvest.hpp"

#include <cstdio>

#include "platform/content.hpp"
#include "platform/core_fs.hpp"
#include "platform/exefs.hpp"
#include "platform/log.hpp"
#include "platform/titles.hpp"

namespace mk::plat {

namespace {

// Признак содержимого игры: content id program NCA (меняется с каждым обновлением и переустановкой) + номер
// версии записи ncm. Совпал с кэшем — sdk игры та же, exefs не открываем.
std::string contentKey(const ProgramNca& nca) {
    char buf[64];
    std::string id;
    for (int i = 0; i < 0x10; ++i) {
        std::snprintf(buf, sizeof(buf), "%02x", nca.contentId.c[i]);
        id += buf;
    }
    std::snprintf(buf, sizeof(buf), ":%s:%u", nca.fromPatch ? "patch" : "base",
                  nca.fromPatch ? nca.patchVersion : nca.baseVersion);
    return id + buf;
}

// Открыть exefs игры (как source/app_model.cpp loadDetails): program NCA → заголовок (program id) → Code FS.
bool openGame(uint64_t appId, const ProgramNca& nca, ExefsMount& mount) {
    uint64_t programId = appId;
    NcaHeaderInfo hdr;
    if (readProgramNcaHeader(nca, hdr) && hdr.programId != 0) programId = hdr.programId;
    ExefsReport rep;
    return mount.open(programId, nca, rep);
}

uint64_t tidValue(const std::string& tid) {
    try {
        return std::stoull(tid, nullptr, 16);
    } catch (const std::exception&) {
        return 0;
    }
}

}  // namespace

HarvestScan scanInstalledGamesSdk(const HarvestProgress& progress, const std::atomic<bool>* cancel) {
    HarvestScan out;
    std::vector<uint64_t> ids;
    if (!listApplicationIds(ids)) {
        MK_LOG("sdk-harvest: no application list");
        return out;
    }
    out.total = ids.size();
    PosixFs& fs = posixFs();
    mk::Env env{fs, nxHash(), nxClock()};
    mk::harvest::Cache cache;
    try {
        cache = mk::harvest::load_cache(env, kSdkScanCache);
    } catch (const mk::Error& e) {
        MK_LOG("sdk-harvest: cache unreadable %s — scanning all", e.code().c_str());
    } catch (const std::exception& e) {
        MK_LOG("sdk-harvest: cache unreadable (%s) — scanning all", e.what());
    }
    bool changed = false;
    for (uint64_t id : ids) {
        if (cancel && cancel->load()) {
            out.cancelled = true;
            break;
        }
        const std::string tid = paths::tidHex(id);
        std::string name = tid;
        ProgramNca nca;
        if (!findProgramNca(id, nca)) {
            out.failed++;
            out.checked++;
            if (progress) progress(out.checked, out.total, name);
            continue;
        }
        const std::string key = contentKey(nca);
        if (const mk::harvest::GameSdk* hit = mk::harvest::cache_hit(cache, tid, key)) {
            out.games.push_back(*hit);
            out.fromCache++;
            out.checked++;
            if (progress) progress(out.checked, out.total, hit->name);
            continue;
        }
        mk::harvest::GameSdk g;
        g.tid = tid;
        g.key = key;
        TitleControl tc;
        if (readTitleControl(id, tc)) {
            g.name = tc.name.empty() ? tid : tc.name;
            g.version = tc.displayVersion;
        } else {
            g.name = tid;
        }
        name = g.name;
        ExefsMount mount;
        if (!openGame(id, nca, mount)) {
            out.failed++;
            out.checked++;
            MK_LOG("sdk-harvest: %s exefs not opened", tid.c_str());
            if (progress) progress(out.checked, out.total, name);
            continue;
        }
        try {
            g = mk::harvest::probe_sdk_file(env, std::string(ExefsMount::root()) + "sdk", g);
            mk::harvest::cache_put(cache, g);
            changed = true;
            out.games.push_back(g);
            MK_LOG("sdk-harvest: %s sdk=%d nnsdk=%d ver=%s", tid.c_str(), g.has_sdk, g.nnsdk,
                   g.sdk_version ? mk::version_text(*g.sdk_version).c_str() : "-");
        } catch (const mk::Error& e) {
            out.failed++;   // ошибку в кэш не кладём — в следующий раз проверим снова
            MK_LOG("sdk-harvest: %s read failed %s", tid.c_str(), e.code().c_str());
        } catch (const std::exception& e) {
            out.failed++;   // чужое исключение (не mk::Error) — так же сбой, в кэш не кладём
            MK_LOG("sdk-harvest: %s read failed (%s)", tid.c_str(), e.what());
        }
        mount.close();
        out.checked++;
        if (progress) progress(out.checked, out.total, name);
    }
    if (changed) {
        try {
            mk::harvest::save_cache(env, kSdkScanCache, cache);
        } catch (const mk::Error& e) {
            MK_LOG("sdk-harvest: cache not saved %s: %s", e.code().c_str(), e.what());   // не беда: проверим заново
        }
    }
    MK_LOG("sdk-harvest: %zu/%zu checked, %zu from cache, %zu failed%s", out.checked, out.total, out.fromCache,
           out.failed, out.cancelled ? " (cancelled)" : "");
    return out;
}

mk::harvest::SaveResult saveHarvested(const mk::harvest::Candidate& c) {
    mk::harvest::SaveResult r;
    r.code = "no_exefs";
    PosixFs& fs = posixFs();
    mk::Env env{fs, nxHash(), nxClock()};
    for (const auto& g : c.games) {
        const uint64_t id = tidValue(g.tid);
        ProgramNca nca;
        if (!id || !findProgramNca(id, nca)) continue;
        ExefsMount mount;
        if (!openGame(id, nca, mount)) continue;
        // Источник — эта игра (первая открывшаяся): по ней имя папки и «из игры …».
        mk::harvest::Candidate cc = c;
        for (size_t i = 0; i < cc.games.size(); ++i)
            if (cc.games[i].tid == g.tid && i) std::swap(cc.games[0], cc.games[i]);
        r = mk::harvest::save_candidate(env, paths::kSdkDirOwn, cc, std::string(ExefsMount::root()) + "sdk", MK_APP_NAME);
        mount.close();
        MK_LOG("sdk-harvest: save %s from %s -> %s ok=%d code=%s", mk::version_text(c.version).c_str(), g.tid.c_str(),
               r.target.dir.c_str(), r.ok, r.code.c_str());
        if (r.ok || r.code != "changed") return r;
    }
    return r;
}

}  // namespace mk::plat
