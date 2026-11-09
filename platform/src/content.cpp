// SPDX-License-Identifier: GPL-3.0-or-later
// Based on TorrentShopNX by Langegen (GPL-3.0): source/ui/LibraryView.cpp (getInstalledTitleMetadata),
// source/installer/ncz_parser.cpp (deriveNcaHeaderKey, decryptNcaHeader), source/utils/switch_utils.cpp
// (queryInstalledGame)
//
// Переделано: ищем ещё и на картридже; запоминаем путь контента и required_system_version; ключ заголовка
// выводится один раз и держится в памяти; из заголовка берём также program id, тип контента, поколение ключа и
// наличие rights id.
#include "platform/content.hpp"

#include <array>
#include <cstdio>
#include <cstring>

#include "platform/log.hpp"
#include "platform/service_lock.hpp"

namespace mk::plat {

namespace {

constexpr uint32_t kNca3Magic = 0x3341434E;  // "NCA3"
constexpr size_t kHeaderSize = 0xC00;

// header_kek_source и header_key_source — общеизвестные источники (как в Awoo, nxdumptool, TorrentShopNX).
// Это не ключ: сам ключ из них выводит spl внутри консоли и наружу не отдаёт.
constexpr uint8_t kHeaderKekSource[0x10] = {
    0x1F, 0x12, 0x91, 0x3A, 0x4A, 0xCB, 0xF0, 0x0D, 0x4C, 0xDE, 0x3A, 0xF6, 0xD5, 0x23, 0x88, 0x2A};
constexpr uint8_t kHeaderKeySource[0x20] = {
    0x5A, 0x3E, 0xD8, 0x4F, 0xDE, 0xC0, 0xD8, 0x26, 0x31, 0xF7, 0xE2, 0x5D, 0x19, 0x7B, 0xF5, 0xD0,
    0x1C, 0x9B, 0x7B, 0xFA, 0xF6, 0x28, 0x18, 0x3D, 0x71, 0xF6, 0x4D, 0x73, 0xF1, 0x50, 0xB9, 0xD2};

struct HeaderKeyCache {
    bool tried = false;
    bool ok = false;
    uint32_t rc = 0;
    std::array<uint8_t, 0x20> key{};
};
HeaderKeyCache g_key;  // под serviceMutex()

bool headerKey(const uint8_t*& outKey, uint32_t& outRc) {
    ServiceLock lock(serviceMutex());
    if (!g_key.tried) {
        g_key.tried = true;
        Result rc = splCryptoInitialize();
        if (R_SUCCEEDED(rc)) {
            uint8_t sealedKek[0x10] = {};
            rc = splCryptoGenerateAesKek(kHeaderKekSource, 0, 0, sealedKek);
            if (R_SUCCEEDED(rc)) rc = splCryptoGenerateAesKey(sealedKek, kHeaderKeySource, g_key.key.data());
            if (R_SUCCEEDED(rc)) {
                rc = splCryptoGenerateAesKey(sealedKek, kHeaderKeySource + 0x10, g_key.key.data() + 0x10);
            }
            std::memset(sealedKek, 0, sizeof(sealedKek));
            splCryptoExit();
        }
        g_key.ok = R_SUCCEEDED(rc);
        g_key.rc = rc;
        if (!g_key.ok) g_key.key.fill(0);
        MK_LOG("content: NCA header key via spl: %s (rc=0x%X)", g_key.ok ? "ok" : "FAILED", rc);  // сам ключ — нет
    }
    outKey = g_key.ok ? g_key.key.data() : nullptr;
    outRc = g_key.rc;
    return g_key.ok;
}

bool latestKey(NcmContentMetaDatabase* db, uint64_t id, NcmContentMetaKey& key) {
    return R_SUCCEEDED(ncmContentMetaDatabaseGetLatestContentMetaKey(db, &key, id));
}

constexpr NcmStorageId kStorages[] = {NcmStorageId_SdCard, NcmStorageId_BuiltInUser, NcmStorageId_GameCard};

}  // namespace

std::string hosVersionText(uint32_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u", (v >> 26) & 0x3F, (v >> 20) & 0x3F, (v >> 16) & 0xF);
    return buf;
}

bool findProgramNca(uint64_t appId, ProgramNca& out) {
    out = ProgramNca{};
    const uint64_t baseId = appId & ~0x1FFFULL;
    const uint64_t patchId = baseId | 0x800ULL;  // [Г] id патча = id приложения + 0x800 (так же у TorrentShopNX)

    ServiceLock lock(serviceMutex());

    struct Hit {
        bool found = false;
        NcmStorageId storage = NcmStorageId_None;
        NcmContentMetaKey key{};
    };
    Hit patch, base;
    for (NcmStorageId sid : kStorages) {
        NcmContentMetaDatabase db;
        if (R_FAILED(ncmOpenContentMetaDatabase(&db, sid))) continue;
        NcmContentMetaKey k{};
        if (latestKey(&db, patchId, k) && (!patch.found || k.version > patch.key.version)) {
            patch = {true, sid, k};
        }
        if (!base.found && latestKey(&db, baseId, k)) base = {true, sid, k};
        ncmContentMetaDatabaseClose(&db);
    }
    out.patchInstalled = patch.found;
    out.patchVersion = patch.found ? patch.key.version : 0;
    out.baseInstalled = base.found;
    out.baseVersion = base.found ? base.key.version : 0;

    // Патч приоритетнее базы: exefs патча полный, читается без базы.
    for (const Hit* h : {&patch, &base}) {
        if (!h->found) continue;
        NcmContentMetaDatabase db;
        if (R_FAILED(ncmOpenContentMetaDatabase(&db, h->storage))) continue;
        NcmContentId cid{};
        Result rc = ncmContentMetaDatabaseGetContentIdByType(&db, &cid, &h->key, NcmContentType_Program);
        u32 rsv = 0;
        const bool rsvOk = R_SUCCEEDED(ncmContentMetaDatabaseGetRequiredSystemVersion(&db, &rsv, &h->key));
        ncmContentMetaDatabaseClose(&db);
        if (R_FAILED(rc)) {
            MK_LOG("content: %016lX no Program content in meta %016lX v%u rc=0x%X", appId, h->key.id, h->key.version,
                   rc);
            continue;
        }

        // Контент обычно в том же хранилище, что и запись; на всякий случай — смотрим и другие.
        bool located = false;
        for (NcmStorageId sid : {h->storage, NcmStorageId_SdCard, NcmStorageId_BuiltInUser, NcmStorageId_GameCard}) {
            NcmContentStorage cs;
            if (R_FAILED(ncmOpenContentStorage(&cs, sid))) continue;
            bool has = false;
            if (R_SUCCEEDED(ncmContentStorageHas(&cs, &has, &cid)) && has) {
                char path[FS_MAX_PATH] = {};
                if (R_SUCCEEDED(ncmContentStorageGetPath(&cs, path, sizeof(path), &cid))) {
                    out.path = path;
                    out.storage = sid;
                    located = true;
                }
            }
            ncmContentStorageClose(&cs);
            if (located) break;
        }
        if (!located) continue;

        out.found = true;
        out.fromPatch = (h == &patch);
        out.key = h->key;
        out.contentId = cid;
        out.rsvKnown = rsvOk;
        out.requiredSystemVersion = rsv;
        MK_LOG("content: %016lX program NCA from %s v%u storage=%d rsv=%s", appId, out.fromPatch ? "patch" : "base",
               h->key.version, static_cast<int>(out.storage), rsvOk ? hosVersionText(rsv).c_str() : "?");
        return true;
    }
    MK_LOG("content: %016lX program NCA not found (base=%d patch=%d)", appId, base.found, patch.found);
    return false;
}

bool readProgramNcaHeaderDecrypted(const ProgramNca& nca, std::array<uint8_t, 0xC00>& dec, uint32_t& rcOut) {
    rcOut = 0;
    if (!nca.found) return false;

    const uint8_t* key = nullptr;
    uint32_t keyRc = 0;
    if (!headerKey(key, keyRc)) {
        rcOut = keyRc;
        return false;
    }

    std::array<uint8_t, kHeaderSize> raw{};
    {
        ServiceLock lock(serviceMutex());
        NcmContentStorage cs;
        Result rc = ncmOpenContentStorage(&cs, nca.storage);
        if (R_SUCCEEDED(rc)) {
            rc = ncmContentStorageReadContentIdFile(&cs, raw.data(), raw.size(), &nca.contentId, 0);
            ncmContentStorageClose(&cs);
        }
        if (R_FAILED(rc)) {
            rcOut = rc;
            MK_LOG("content: read NCA header rc=0x%X", rc);
            return false;
        }
    }

    Aes128XtsContext xts;
    aes128XtsContextCreate(&xts, key, key + 0x10, false);
    for (size_t off = 0; off < kHeaderSize; off += 0x200) {
        aes128XtsContextResetSector(&xts, off / 0x200, true);
        aes128XtsDecrypt(&xts, dec.data() + off, raw.data() + off, 0x200);
    }
    std::memset(&xts, 0, sizeof(xts));
    return true;
}

bool readProgramNcaHeader(const ProgramNca& nca, NcaHeaderInfo& out) {
    out = NcaHeaderInfo{};
    std::array<uint8_t, kHeaderSize> dec{};
    if (!readProgramNcaHeaderDecrypted(nca, dec, out.rc)) return false;

    auto rd32 = [&](size_t o) {
        uint32_t v;
        std::memcpy(&v, dec.data() + o, 4);
        return v;
    };
    auto rd64 = [&](size_t o) {
        uint64_t v;
        std::memcpy(&v, dec.data() + o, 8);
        return v;
    };
    if (rd32(0x200) != kNca3Magic || rd64(0x208) < kHeaderSize) {
        MK_LOG("content: NCA header magic mismatch");
        out.rc = 1;
        return false;
    }
    out.contentType = dec[0x205];
    out.keyGeneration = dec[0x220] > dec[0x206] ? dec[0x220] : dec[0x206];
    out.programId = rd64(0x210);
    out.sdkAddonRaw = rd32(0x21C);
    out.sdkAddon = Version3::fromSdkAddon(out.sdkAddonRaw);
    for (size_t i = 0x230; i < 0x240; ++i) {
        if (dec[i] != 0) {
            out.hasRightsId = true;
            break;
        }
    }
    out.ok = true;
    MK_LOG("content: NCA header ok: program %016lX sdk %s type %u keygen %u rights=%d", out.programId,
           out.sdkAddon.toString().c_str(), out.contentType, out.keyGeneration, out.hasRightsId);
    return true;
}

}  // namespace mk::plat
