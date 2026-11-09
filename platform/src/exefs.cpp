// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — exefs игры: Code FS (шаги 1–2), сырой NCA (шаг 3). См. platform/exefs.hpp.
#include "platform/exefs.hpp"

#include <switch.h>
#include <sys/stat.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>

#include "platform/exefs_raw.hpp"
#include "platform/keys.hpp"
#include "platform/log.hpp"
#include "platform/service_lock.hpp"

namespace mk::plat {

namespace {

constexpr const char* kDevice = "mkexefs";
// В перечислении FsFileSystemType libnx значения Code нет (начинается с Logo = 2); в fsp-srv Code = 0 [Г].
constexpr FsFileSystemType kFsTypeCode = static_cast<FsFileSystemType>(0);

std::atomic<bool> g_deviceBusy{false};

// Журнал попыток: programId -> текст последней попытки (ограничен по размеру; ключей в нём нет).
std::mutex g_jmtx;
std::map<uint64_t, std::string> g_journal;
uint64_t g_lastProgram = 0;
std::string g_cur;
constexpr size_t kJournalMax = 16 * 1024;

void jAppend(const char* line) {
    std::lock_guard<std::mutex> lk(g_jmtx);
    if (g_cur.size() < kJournalMax) {
        g_cur += line;
        g_cur += '\n';
    }
}

void jf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void jf(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    MK_LOG("exefs: %s", buf);
    jAppend(buf);
}

void jBegin(uint64_t programId, const ProgramNca& nca) {
    {
        std::lock_guard<std::mutex> lk(g_jmtx);
        g_cur.clear();
    }
    char hosv[32];
    const u32 v = hosversionGet();
    std::snprintf(hosv, sizeof(hosv), "%u.%u.%u", HOSVER_MAJOR(v), HOSVER_MINOR(v), HOSVER_MICRO(v));
    jf("open program=%016lX HOS=%s found=%d from=%s v%u storage=%d path=%s", programId, hosv, nca.found,
       nca.fromPatch ? "patch" : "base", nca.key.version, static_cast<int>(nca.storage),
       nca.path.empty() ? "-" : nca.path.c_str());
}

void jEnd(uint64_t programId) {
    std::lock_guard<std::mutex> lk(g_jmtx);
    g_journal[programId] = g_cur;
    g_lastProgram = programId;
}

bool mountAndCheck(FsFileSystem& fs, ExefsReport& report) {
    if (fsdevMountDevice(kDevice, fs) < 0) {  // fsdev сам закроет fs и при отказе
        MK_LOG("exefs: fsdevMountDevice failed");
        return false;
    }
    struct stat st;
    report.hasMainNpdm = (::stat("mkexefs:/main.npdm", &st) == 0 && S_ISREG(st.st_mode));
    if (!report.hasMainNpdm) {
        MK_LOG("exefs: mounted, but main.npdm is not visible — not an exefs");
        fsdevUnmountDevice(kDevice);
        return false;
    }
    return true;
}

}  // namespace

std::string exefsJournalFor(uint64_t programId) {
    std::lock_guard<std::mutex> lk(g_jmtx);
    auto it = g_journal.find(programId);
    if (it != g_journal.end()) return it->second;
    it = g_journal.find(g_lastProgram);
    return it != g_journal.end() ? it->second : std::string();
}

const char* exefsViaName(ExefsVia v) {
    switch (v) {
        case ExefsVia::CodeWithId: return "code-with-id";
        case ExefsVia::CodeWithPatch: return "code-with-patch";
        case ExefsVia::RawNcaWithKeys: return "raw-nca-keys";
        default: return "none";
    }
}

ExefsMount::~ExefsMount() { close(); }

void ExefsMount::close() {
    if (!mounted_) return;
    if (raw_) rawExefsUnmount();
    else fsdevUnmountDevice(kDevice);
    mounted_ = false;
    raw_ = false;
    g_deviceBusy.store(false);
}

bool ExefsMount::open(uint64_t programId, const ProgramNca& nca, ExefsReport& report) {
    report = ExefsReport{};
    report.keysPresent = prodKeysPresent();
    if (mounted_) close();
    bool expected = false;
    if (!g_deviceBusy.compare_exchange_strong(expected, true)) {
        MK_LOG("exefs: device busy (another exefs is open)");  // журнал попытки не трогаем
        return false;
    }

    jBegin(programId, nca);

    // 1. Code FS по пути program NCA.
    if (nca.found && !nca.path.empty()) {
        FsFileSystem fs;
        Result rc;
        {
            ServiceLock lock(serviceMutex());
            rc = fsOpenFileSystemWithId(&fs, programId, kFsTypeCode, nca.path.c_str(), FsContentAttributes_All);
        }
        report.rcWithId = R_FAILED(rc) ? rc : 0;
        jf("step1 fsOpenFileSystemWithId(Code) rc=0x%X", rc);
        if (R_SUCCEEDED(rc) && mountAndCheck(fs, report)) {
            report.via = ExefsVia::CodeWithId;
            mounted_ = true;
            jEnd(programId);
            return true;
        }
    }

    // 2. Запасной путь: Code FS «с патчем» без пути контента.
    if (hosversionAtLeast(2, 0, 0)) {
        FsFileSystem fs;
        Result rc;
        {
            ServiceLock lock(serviceMutex());
            rc = fsOpenFileSystemWithPatch(&fs, programId, kFsTypeCode);
        }
        report.rcWithPatch = R_FAILED(rc) ? rc : 0;
        jf("step2 fsOpenFileSystemWithPatch(Code) rc=0x%X", rc);
        if (R_SUCCEEDED(rc) && mountAndCheck(fs, report)) {
            report.via = ExefsVia::CodeWithPatch;
            mounted_ = true;
            jEnd(programId);
            return true;
        }
    }

    // 3. Сырой NCA через ncm + spl + es (без prod.keys).
    if (nca.found) {
        report.rawTried = true;
        RawExefsOutcome o;
        const bool ok = rawExefsMount(nca, kDevice, o, jAppend);
        report.rawRightsId = o.rightsId;
        report.rawPersonalized = o.personalized;
        if (ok) {
            struct stat st;
            report.hasMainNpdm = (::stat("mkexefs:/main.npdm", &st) == 0 && S_ISREG(st.st_mode));
            if (report.hasMainNpdm) {
                report.via = ExefsVia::RawNcaWithKeys;
                mounted_ = true;
                raw_ = true;
                jf("step3 ok: main.npdm visible");
                jEnd(programId);
                return true;
            }
            jf("step3: mounted, but main.npdm is not visible — not an exefs");
            rawExefsUnmount();
            report.rawStep = "mount";
            report.rawCore = "NO_MAIN_NPDM";
        } else {
            report.rawStep = o.step;
            report.rcRaw = o.rc;
            report.rawCore = o.coreCode;
        }
        jf("step3 failed: step=%s rc=0x%X core=%s", report.rawStep.c_str(), report.rcRaw,
           report.rawCore.empty() ? "-" : report.rawCore.c_str());
    } else {
        jf("step3 skipped: program NCA not found");
    }
    jEnd(programId);
    g_deviceBusy.store(false);
    return false;
}

}  // namespace mk::plat
