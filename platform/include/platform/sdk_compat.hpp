// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — вывод блока «sdk» простыми словами.
// ПРАВИЛО «прошивка → какая sdk подходит» — ОДНО МЕСТО, в ядре: mk::fw_sdk_table() / mk::evaluate_sdk_status()
// (core/include/mk/sdk.hpp, источник у каждой строки; проверено консолью: HOS 19.0.1 + nnSdk 19.3.13).
// Здесь только перевод ответа ядра в вид для экрана и выбор рекомендуемой строки из найденных.
#pragma once

#include <string>
#include <vector>

#include "platform/sdk_scan.hpp"
#include "platform/version.hpp"

namespace mk::plat {

enum class SdkNeed {
    Unknown,        // не знаем версию игры или прошивки
    NotNeeded,      // игра собрана на sdk не новее прошивки
    Needed,         // нужна подмена, ещё не сделана
    SwappedOk,      // подменено, подменённая sdk подходит прошивке
    SwappedTooNew,  // подменено, но не подходит (ядро: swapped_mismatch)
    SwappedUnknown  // лежит файл exefs/sdk, но это не nnSdk
};

struct SdkAdvice {
    SdkNeed need = SdkNeed::Unknown;
    Version3 gameNeeds;          // sdk игры (по NCA, иначе по своей sdk из exefs)
    Version3 bestForFirmware;    // что ядро рекомендует для прошивки
    std::string ruleSource;      // источник строки таблицы ядра
    int recommendedIndex = -1;   // индекс в списке найденных sdk, -1 — подходящей нет
};

// swappedIsNnSdk=false при swappedPresent → SwappedUnknown без вопроса к ядру.
SdkAdvice adviseSdk(const Version3& gameSdkNca, const Version3& gameSdkMw, const Version3& firmware,
                    bool swappedPresent, bool swappedIsNnSdk, const Version3& swappedVersion,
                    const std::vector<FoundSdk>& found);

}  // namespace mk::plat
