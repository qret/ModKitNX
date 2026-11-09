// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — найденные sdk из двух мест: sdmc:/switch/<APP>/sdk/ и запасное sdmc:/switch/DBI/dumps/SDK/.
// Имена файлов любые — опознание по содержимому (NSO0 + модуль nnSdk + строка SDK MW) и склейку повторов по
// ModuleId/sha256 делает ядро: mk::scan_sdk (core/include/mk/sdk.hpp). Здесь — только места и вид для экрана.
#pragma once

#include <string>
#include <vector>

#include "platform/version.hpp"

namespace mk::plat {

struct FoundSdk {
    std::string path;                    // путь, который будет использован (первое место: своя папка раньше DBI)
    std::string fileName;                // имя файла как есть — показывается в списке
    Version3 version;                    // из строки SDK MW
    std::string versionText;             // «19.3.13»
    std::string moduleIdHex, sha256Hex;
    bool inOwn = false, inDbi = false;   // где лежит (в обеих — повтор, показан один раз)
    std::vector<std::string> allPaths;   // все места одинакового файла — для «Подробностей»
    bool sameVersionOtherBuild = false;  // та же версия MW, другой ModuleId
    std::string fromGame, fromGameVersion;   // сохранена «Найти sdk в других играх»: source.json рядом (sha256 совпал)
};

struct SdkScanStats {
    bool ownDirExists = false;
    bool dbiDirExists = false;
    std::string error;                   // текст mk::Error, если обход упал
};

// Блокирующий обход карты — из фонового потока. Порядок — как вернуло ядро (по местам).
std::vector<FoundSdk> scanSdk(SdkScanStats& stats);

}  // namespace mk::plat
