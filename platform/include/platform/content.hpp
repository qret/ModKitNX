// SPDX-License-Identifier: GPL-3.0-or-later
// Based on TorrentShopNX by Langegen (GPL-3.0): source/ui/LibraryView.cpp (getInstalledTitleMetadata),
// source/installer/ncz_parser.cpp (deriveNcaHeaderKey, decryptNcaHeader), source/installer/ncz_parser.h (NcaHeader)
//
// Program NCA установленной игры через ncm (патч приоритетнее базы), требуемая прошивка (ncm), заголовок NCA
// и sdk_addon_version @0x21C. Ключ заголовка выводится службой spl на самой консоли — prod.keys не нужен.
// Ключ живёт только в памяти процесса; в журнал/файлы/на экран не попадает.
#pragma once

#include <switch.h>

#include <array>
#include <cstdint>
#include <string>

#include "platform/version.hpp"

namespace mk::plat {

struct ProgramNca {
    bool found = false;
    bool fromPatch = false;              // true — program NCA патча (обновления), иначе базы
    NcmStorageId storage = NcmStorageId_None;
    NcmContentMetaKey key{};             // ключ записи, из которой взят NCA
    NcmContentId contentId{};
    std::string path;                    // путь контента для fsOpenFileSystemWithId (@SdCardContent:/…)

    bool baseInstalled = false;
    uint32_t baseVersion = 0;
    bool patchInstalled = false;
    uint32_t patchVersion = 0;           // номер обновления (N << 16)

    bool rsvKnown = false;               // required_system_version из ncm, «как записано при установке»
    uint32_t requiredSystemVersion = 0;
};

// Ищет program NCA: сначала патч (appId | 0x800 [Г]), потом база; хранилища SdCard → BuiltInUser → GameCard.
bool findProgramNca(uint64_t appId, ProgramNca& out);

struct NcaHeaderInfo {
    bool ok = false;
    uint32_t rc = 0;              // код отказа чтения/ключа (0 — ок)
    uint64_t programId = 0;       // @0x210
    uint32_t sdkAddonRaw = 0;     // @0x21C
    Version3 sdkAddon;            // разобранная sdk_addon_version
    uint8_t contentType = 0;      // @0x205 (0 = Program)
    uint8_t keyGeneration = 0;    // max(@0x206, @0x220)
    bool hasRightsId = false;     // @0x230 не нули — титульный ключ из тикета
};

// Читает 0xC00 байт заголовка через ncm и расшифровывает (AES-128-XTS, «Nintendo-tweak», сектор 0x200).
bool readProgramNcaHeader(const ProgramNca& nca, NcaHeaderInfo& out);

// То же, но отдаёт весь расшифрованный заголовок (0xC00) — для чтения exefs из сырого NCA (platform/exefs_raw).
// rc — код отказа (spl или ncm), 0 при успехе. Заголовок не содержит ключей в открытом виде (key area зашифрована).
bool readProgramNcaHeaderDecrypted(const ProgramNca& nca, std::array<uint8_t, 0xC00>& dec, uint32_t& rc);

// Текст «22.5.0» для required_system_version (major<<26 | minor<<20 | micro<<16).
std::string hosVersionText(uint32_t v);

}  // namespace mk::plat
