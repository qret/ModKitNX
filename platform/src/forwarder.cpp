// SPDX-License-Identifier: GPL-3.0-or-later
// Based on sphaira by ITotalJustice (GPL-3.0): sphaira/source/owo.cpp (install_forwader_internal), sphaira/source/yati/nx/ns.cpp
//
// ModKitNX — ярлык на домашнем экране. См. platform/forwarder.hpp.
// Порядок записи — как в forwarder.c (порт sphaira, проверен на консоли): сначала снять прежнее с этим TitleID
// (у sphaira снятие идёт после записи и при той же сборке убирало бы только что записанное — содержимое называется
// по хешу и совпадает), затем placeholder → register трёх NCA, ncmContentMetaDatabaseSet + Commit, запись
// приложения в ns (ns:am2 команда 16 PushApplicationRecord) и сброс кэша control (команда 404).
#include "platform/forwarder.hpp"

#include <switch.h>
#include <sys/stat.h>

#include <cstddef>
#include <cstring>
#include <vector>

#include <mk/forwarder.hpp>

#include "platform/console_info.hpp"
#include "platform/content.hpp"
#include "platform/core_fs.hpp"
#include "platform/log.hpp"
#include "platform/paths.hpp"
#include "platform/service_lock.hpp"
#include "platform/services.hpp"

namespace mk::plat::forwarder {

namespace fw = mk::forwarder;
namespace off = mk::forwarder::nacp_off;

// Смещения NACP ядра = NacpStruct libnx.
static_assert(sizeof(NacpStruct) == fw::kNacpSize, "NACP 0x4000");
static_assert(offsetof(NacpStruct, startup_user_account) == off::kStartupUserAccount, "nacp");
static_assert(offsetof(NacpStruct, user_account_switch_lock) == off::kUserAccountSwitchLock, "nacp");
static_assert(offsetof(NacpStruct, add_on_content_registration_type) == off::kAocRegistrationType, "nacp");
static_assert(offsetof(NacpStruct, supported_language_flag) == off::kSupportedLanguageFlag, "nacp");
static_assert(offsetof(NacpStruct, screenshot) == off::kScreenshot, "nacp");
static_assert(offsetof(NacpStruct, video_capture) == off::kVideoCapture, "nacp");
static_assert(offsetof(NacpStruct, data_loss_confirmation) == off::kDataLossConfirmation, "nacp");
static_assert(offsetof(NacpStruct, play_log_policy) == off::kPlayLogPolicy, "nacp");
static_assert(offsetof(NacpStruct, presence_group_id) == off::kPresenceGroupId, "nacp");
static_assert(offsetof(NacpStruct, rating_age) == off::kRatingAge, "nacp");
static_assert(offsetof(NacpStruct, display_version) == off::kDisplayVersion, "nacp");
static_assert(offsetof(NacpStruct, add_on_content_base_id) == off::kAocBaseId, "nacp");
static_assert(offsetof(NacpStruct, save_data_owner_id) == off::kSaveDataOwnerId, "nacp");
static_assert(offsetof(NacpStruct, user_account_save_data_size) == off::kUserAccountSaveDataSize, "nacp");
static_assert(offsetof(NacpStruct, user_account_save_data_journal_size) == off::kUserAccountSaveDataJournalSize, "nacp");
static_assert(offsetof(NacpStruct, device_save_data_size) == off::kDeviceSaveDataSize, "nacp");
static_assert(offsetof(NacpStruct, device_save_data_journal_size) == off::kDeviceSaveDataJournalSize, "nacp");
static_assert(offsetof(NacpStruct, application_error_code_category) == off::kErrorCodeCategory, "nacp");
static_assert(offsetof(NacpStruct, local_communication_id) == off::kLocalCommunicationId, "nacp");
static_assert(offsetof(NacpStruct, logo_type) == off::kLogoType, "nacp");
static_assert(offsetof(NacpStruct, logo_handling) == off::kLogoHandling, "nacp");
static_assert(offsetof(NacpStruct, pseudo_device_id_seed) == off::kPseudoDeviceIdSeed, "nacp");
static_assert(offsetof(NacpStruct, user_account_save_data_size_max) == off::kUserAccountSaveDataSizeMax, "nacp");
static_assert(offsetof(NacpStruct, user_account_save_data_journal_size_max) == off::kUserAccountSaveDataJournalSizeMax, "nacp");
static_assert(offsetof(NacpStruct, device_save_data_size_max) == off::kDeviceSaveDataSizeMax, "nacp");
static_assert(offsetof(NacpStruct, device_save_data_journal_size_max) == off::kDeviceSaveDataJournalSizeMax, "nacp");
static_assert(offsetof(NacpStruct, play_log_query_capability) == off::kPlayLogQueryCapability, "nacp");
static_assert(offsetof(NacpStruct, required_network_service_license_on_launch) == off::kRequiredNetworkLicense, "nacp");
static_assert(offsetof(NacpStruct, titles_data_format) == off::kTitlesDataFormat, "nacp");
// Запись базы ncm ядра = NcmContentMetaHeader + NcmApplicationMetaExtendedHeader + NcmContentInfo[3].
static_assert(sizeof(NcmContentMetaHeader) == 8 && sizeof(NcmApplicationMetaExtendedHeader) == 0x10 &&
                  sizeof(NcmContentInfo) == 0x18 && sizeof(NcmContentId) == 0x10,
              "ncm");

namespace {

const char* const kLoaderMain = "romfs:/forwarder/main";
const char* const kLoaderNpdm = "romfs:/forwarder/main.npdm";
const char* const kIcon = "romfs:/icon.jpg";
constexpr Result kNsNotFound = 0x410;   // нет такого приложения (nsDeleteApplicationCompletely)

// ns:am2 IApplicationManagerInterface: 16 PushApplicationRecord, 404 InvalidateApplicationControlCache.
struct StorageRecord {
    NcmContentMetaKey key;
    u8 storage_id;
    u8 padding[7];
};
static_assert(sizeof(StorageRecord) == 0x18, "ContentStorageRecord");

Result pushRecord(Service* s, u64 tid, const StorageRecord* rec) {
    const struct {
        u8 last_modified_event;
        u8 padding[7];
        u64 tid;
    } in = {3 /* installed */, {0}, tid};
    return serviceDispatchIn(s, 16, in, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
                             .buffers = {{rec, sizeof(*rec)}});
}

Result invalidateControl(Service* s, u64 tid) { return serviceDispatchIn(s, 404, tid); }

bool recordExists(uint64_t tid) {
    ServiceLock lock(serviceMutex());
    std::vector<NsApplicationRecord> batch(50);
    s32 offset = 0, count = 0;
    do {
        count = 0;
        if (R_FAILED(nsListApplicationRecord(batch.data(), static_cast<s32>(batch.size()), offset, &count))) return false;
        for (s32 i = 0; i < count; ++i)
            if (batch[i].application_id == tid) return true;
        offset += count;
    } while (count == static_cast<s32>(batch.size()));
    return false;
}

bool runningFrom(uint64_t tid) {
    u64 pid = 0;
    return R_SUCCEEDED(svcGetInfo(&pid, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0)) && pid == tid;
}

bool fileExists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

Outcome fail(const char* code, uint32_t rc = 0) {
    MK_LOG("forwarder: failed (%s) rc=0x%X", code, rc);
    Outcome o;
    o.code = code;
    o.rc = rc;
    return o;
}

NcmContentId idOf(const mk::Digest& d) {
    NcmContentId id{};
    std::memcpy(id.c, d.data(), sizeof(id.c));
    return id;
}

}  // namespace

uint64_t titleId() { return fw::title_id(paths::kAppNroForwarder, "", nxHash()); }

Status status() {
    Status s;
    s.tid = titleId();
    s.installed = recordExists(s.tid);
    s.runningFromIt = runningFrom(s.tid);
    s.nroPresent = fileExists(paths::kAppNro);
    return s;
}

Outcome install() {
    const uint64_t tid = titleId();
    MK_LOG("forwarder: install %016lX for %s", tid, paths::kAppNroForwarder.c_str());
    if (runningFrom(tid)) return fail("running");
    if (!fileExists(paths::kAppNro)) return fail("no_nro");

    PosixFs& fs = posixFs();
    fw::Input in;
    in.tid = tid;
    in.nro_path = paths::kAppNroForwarder;
    in.argv = fw::next_argv(in.nro_path, "");
    try {
        in.hbl_main = fs.read_file(kLoaderMain);
        in.npdm = fs.read_file(kLoaderNpdm);
        in.icon = fs.read_file(kIcon);
    } catch (const mk::Error& e) {
        MK_LOG("forwarder: loader/icon in romfs: %s %s", e.code().c_str(), e.what());
        return fail("no_loader");
    }
    // NACP — из своего .nro (все поля nacptool), иначе пустой; затем правки форвардера.
    in.nacp = fw::nacp_from_nro(fs, paths::kAppNro).value_or(fw::default_nacp());
    try {
        fw::NacpPatch p;
        p.name = MK_APP_NAME;
        p.author = APP_AUTHOR;
        p.version = MK_APP_VERSION;
        p.error_code_category = MK_APP_NAME;
        p.tid = tid;
        fw::patch_nacp(in.nacp, p);
        // Atmosphère 1.8.0+ (новая раскладка флагов отладки NPDM для HOS 19) — бит ForceDebug, как у sphaira.
        const ConsoleInfo ci = queryConsole();
        Version3 v180;
        v180.major = 1;
        v180.minor = 8;
        v180.valid = true;
        const bool forceDebug = ci.amsKnown && !(ci.atmosphere < v180);
        fw::patch_npdm(in.npdm, tid, forceDebug);
        MK_LOG("forwarder: npdm patched, force_debug=%d", forceDebug ? 1 : 0);
    } catch (const mk::Error& e) {
        MK_LOG("forwarder: %s %s", e.code().c_str(), e.what());
        return fail("build");
    }

    fw::Output out;
    uint32_t keyRc = 0;
    try {
        out = fw::build(in, nxHash(), [&keyRc](uint8_t* h, size_t n) {
            if (!encryptNcaHeader(h, n, keyRc)) throw mk::Error("FWD_KEY", "ключ заголовка NCA недоступен");
        });
    } catch (const mk::Error& e) {
        MK_LOG("forwarder: build %s %s", e.code().c_str(), e.what());
        return fail(e.code() == "FWD_KEY" ? "key" : "build", keyRc);
    }
    MK_LOG("forwarder: built program %zu, control %zu, meta %zu bytes", out.program.data.size(),
           out.control.data.size(), out.meta.data.size());

    ScopedNoSleep noSleep;
    ServiceLock lock(serviceMutex());
    const NcmStorageId storage = NcmStorageId_SdCard;
    // Прежнее с этим TitleID (наш ярлык или ярлык sphaira на тот же .nro) — снять до записи. Чужие TitleID — нет.
    {
        const Result rc = nsDeleteApplicationCompletely(tid);
        if (R_FAILED(rc) && rc != kNsNotFound) MK_LOG("forwarder: delete previous rc=0x%X", rc);
        nsDeleteApplicationEntity(tid);
    }

    const fw::Nca* ncas[3] = {&out.program, &out.control, &out.meta};
    std::vector<NcmContentId> registered;
    NcmContentStorage cs;
    Result rc = ncmOpenContentStorage(&cs, storage);
    if (R_FAILED(rc)) return fail("write", rc);
    for (const fw::Nca* n : ncas) {
        const NcmContentId id = idOf(n->hash);
        NcmPlaceHolderId ph;
        if (R_FAILED(rc = ncmContentStorageGeneratePlaceHolderId(&cs, &ph))) break;
        ncmContentStorageDeletePlaceHolder(&cs, &ph);
        if (R_FAILED(rc = ncmContentStorageCreatePlaceHolder(&cs, &id, &ph, n->data.size()))) break;
        if (R_FAILED(rc = ncmContentStorageWritePlaceHolder(&cs, &ph, 0, n->data.data(), n->data.size()))) {
            ncmContentStorageDeletePlaceHolder(&cs, &ph);
            break;
        }
        ncmContentStorageDelete(&cs, &id);
        if (R_FAILED(rc = ncmContentStorageRegister(&cs, &id, &ph))) {
            ncmContentStorageDeletePlaceHolder(&cs, &ph);
            break;
        }
        registered.push_back(id);
    }
    auto rollback = [&cs, &registered] {
        for (auto& id : registered) ncmContentStorageDelete(&cs, &id);
        registered.clear();
    };
    if (R_FAILED(rc)) {
        rollback();
        ncmContentStorageClose(&cs);
        return fail("write", rc);
    }

    NcmContentMetaKey key{};
    key.id = tid;
    key.version = 0;
    key.type = NcmContentMetaType_Application;
    key.install_type = NcmContentInstallType_Full;
    {
        NcmContentMetaDatabase db;
        rc = ncmOpenContentMetaDatabase(&db, storage);
        if (R_SUCCEEDED(rc)) {
            rc = ncmContentMetaDatabaseSet(&db, &key, out.meta_db_record.data(), out.meta_db_record.size());
            if (R_SUCCEEDED(rc)) rc = ncmContentMetaDatabaseCommit(&db);
            ncmContentMetaDatabaseClose(&db);
        }
    }
    if (R_FAILED(rc)) {
        rollback();
        ncmContentStorageClose(&cs);
        return fail("database", rc);
    }
    ncmContentStorageClose(&cs);

    StorageRecord rec{};
    rec.key = key;
    rec.storage_id = storage;
    Service am;
    rc = nsGetApplicationManagerInterface(&am);
    if (R_SUCCEEDED(rc)) {
        rc = pushRecord(&am, tid, &rec);
        if (R_SUCCEEDED(rc)) {
            const Result inv = invalidateControl(&am, tid);
            MK_LOG("forwarder: record pushed, control cache invalidated rc=0x%X", inv);
        }
        serviceClose(&am);
    }
    if (R_FAILED(rc)) {
        // Запись в ns не встала — снять всё с этим TitleID, чтобы не осталось полузаписанного приложения.
        nsDeleteApplicationCompletely(tid);
        return fail("record", rc);
    }
    MK_LOG("forwarder: installed %016lX on SD", tid);
    Outcome o;
    o.ok = true;
    return o;
}

Outcome remove() {
    const uint64_t tid = titleId();
    if (runningFrom(tid)) return fail("running");
    ServiceLock lock(serviceMutex());
    const Result rc = nsDeleteApplicationCompletely(tid);
    if (R_FAILED(rc) && rc != kNsNotFound) return fail("remove", rc);
    MK_LOG("forwarder: removed %016lX (rc=0x%X)", tid, rc);
    Outcome o;
    o.ok = true;
    return o;
}

}  // namespace mk::plat::forwarder
