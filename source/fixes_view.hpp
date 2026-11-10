// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — перевод прогноза ядра (mk::forecast_fixes, mk::sdk_gaps) в вид для экрана «Что исправит модуль».
// Одно место и для моста (source/core_bridge.cpp), и для предпросмотра (preview/mock_platform.cpp).
#pragma once

#include <mk/sdk.hpp>

#include "app_types.hpp"
#include "symbol_names.hpp"

namespace mk::app {

inline FixForecastView fixesView(const mk::ForecastGame& game, const mk::ForecastSdk& sdk) {
    FixForecastView v;
    const mk::ModuleForecast m = mk::forecast_fixes(game, sdk);
    v.ok = true;
    v.sdkKnown = sdk.known;
    if (sdk.version) v.sdkVersion = mk::version_text(*sdk.version);
    v.swapped = m.swapped;
    for (const auto& f : m.fixes) {
        FixLine l;
        l.id = f.fix.id;
        l.symbol = f.fix.symbol;
        l.kind = f.fix.kind;
        l.scope = f.fix.scope;
        l.choice = f.choice;
        l.why = f.why;
        l.sdkHas = f.sdk_has;
        l.sdkStub = f.sdk_stub;
        l.importers = f.importers;
        l.own = f.fix.own;
        v.fixes.push_back(std::move(l));
    }
    // Пробелы sdk, которые модуль не восполнит: только то, чего нет в каталоге.
    if (game.own_sdk_ok)
        for (const auto& g : mk::sdk_gaps(game.all_imports, game.module_exports, sdk.exports, game.own_sdk)) {
            if (g.fix) continue;
            v.gaps.push_back(GapLine{g.symbol, g.kind, g.weak, g.importers, readableSymbol(g.symbol),
                                     onlineServiceOfSymbol(g.symbol)});
        }
    return v;
}

inline void fillFixesGame(FixesBlock& b, const mk::ForecastGame& game) {
    b.gameOk = true;
    if (game.version) b.gameVersion = mk::version_text(*game.version);
    b.vulkan = game.vulkan;
    b.vulkanModule = game.vulkan_module;
    b.opengl = game.opengl;
    b.ownSdkOk = game.own_sdk_ok;
    b.ownSdkError = game.own_sdk_error;
}

}  // namespace mk::app
