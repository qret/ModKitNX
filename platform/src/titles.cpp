// SPDX-License-Identifier: GPL-3.0-or-later
// Based on TorrentShopNX by Langegen (GPL-3.0): source/ui/LibraryView.cpp (сканирование игр: nsListApplicationRecord,
// nsGetApplicationControlData, unpackNacpTitles, saveInstalledIcon)
//
// Переделано: иконка не кэшируется на карту, а возвращается в памяти (UI грузит её через setImageFromMem).
// Название, издатель, версия и распаковка сжатой таблицы SDK 20+ — ядром (mk::parse_nacp, одно место; правило
// языка — как в эталонном parse_nacp). Свой разбор platform/nacp_text не используется (см. его шапку).
#include "platform/titles.hpp"

#include <switch.h>

#include <memory>

#include "platform/log.hpp"
#include <mk/nacp.hpp>

#include "platform/service_lock.hpp"

namespace mk::plat {

bool listApplicationIds(std::vector<uint64_t>& out) {
    out.clear();
    ServiceLock lock(serviceMutex());
    s32 offset = 0;
    s32 count = 0;
    std::vector<NsApplicationRecord> batch(50);
    do {
        count = 0;
        const Result rc = nsListApplicationRecord(batch.data(), static_cast<s32>(batch.size()), offset, &count);
        if (R_FAILED(rc)) {
            MK_LOG("titles: nsListApplicationRecord rc=0x%X offset=%d", rc, offset);
            return offset > 0;
        }
        for (s32 i = 0; i < count; ++i) {
            // Ярлык Homebrew Menu на главном экране (форвардер hbmenu) — не игра: в список не берём
            // (иначе в списке строка «Homebrew Menu, версия FWD, sdk 0.12.17»).
            if (batch[i].application_id == 0x0104444444444000ULL) continue;
            out.push_back(batch[i].application_id);
        }
        offset += count;
    } while (count == static_cast<s32>(batch.size()));
    MK_LOG("titles: %zu application records", out.size());
    return true;
}

namespace {

// JPEG по маркерам FF D8 … FF D9 внутри поля icon (как saveInstalledIcon в TorrentShopNX).
void extractJpeg(const u8* icon, size_t avail, std::vector<uint8_t>& out) {
    out.clear();
    size_t start = 0;
    bool found = false;
    for (size_t i = 0; i + 1 < avail; ++i) {
        if (icon[i] == 0xFF && icon[i + 1] == 0xD8) {
            start = i;
            found = true;
            break;
        }
    }
    if (!found) return;
    size_t end = start;
    for (size_t i = start; i + 1 < avail; ++i) {
        if (icon[i] == 0xFF && icon[i + 1] == 0xD9) end = i + 2;
    }
    if (end <= start + 4) return;
    out.assign(icon + start, icon + end);
}

}  // namespace

bool readTitleControl(uint64_t appId, TitleControl& out) {
    out = TitleControl{};
    out.appId = appId;
    // 0x24000 байт — в кучу, не на стек.
    auto ctrl = std::make_unique<NsApplicationControlData>();
    u64 actual = 0;
    Result rc;
    {
        ServiceLock lock(serviceMutex());
        rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, appId, ctrl.get(),
                                         sizeof(NsApplicationControlData), &actual);
    }
    if (R_FAILED(rc) || actual < sizeof(ctrl->nacp)) {
        MK_LOG("titles: %016lX control data rc=0x%X size=%lu", appId, rc, static_cast<unsigned long>(actual));
        return false;
    }

    // Игры SDK 20+ хранят таблицу названий сжатой — распаковывает ядро (в копии).
    try {
        const mk::NacpInfo n = mk::parse_nacp(reinterpret_cast<const uint8_t*>(&ctrl->nacp), sizeof(ctrl->nacp));
        out.name = n.title.value_or("");
        out.publisher = n.publisher.value_or("");
        out.displayVersion = n.display_version.value_or("");
        out.titlesWereCompressed = n.table_was_compressed;
    } catch (const mk::Error& e) {
        MK_LOG("titles: %016lX NACP: %s %s", appId, e.code().c_str(), e.what());
    }
    if (out.titlesWereCompressed) MK_LOG("titles: %016lX compressed NACP titles unpacked", appId);

    const size_t nacpSize = sizeof(NacpStruct);
    if (actual > nacpSize + 4) {
        size_t avail = static_cast<size_t>(actual) - nacpSize;
        if (avail > sizeof(ctrl->icon)) avail = sizeof(ctrl->icon);
        extractJpeg(ctrl->icon, avail, out.iconJpeg);
    }
    return true;
}

}  // namespace mk::plat
