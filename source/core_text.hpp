// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — тексты ядра (mk::Msg, mk::Texts, mk::Error) -> переводимые тексты экрана (mk::app::Text).
// Ключ ядра получает префикс «core/» (resources/i18n/<язык>/core.json); русская строка ядра — запасной текст.
// Одно место и для моста (source/core_bridge.cpp), и для предпросмотра (preview/mock_platform.cpp).
#pragma once

#include <mk/core.hpp>

#include <string>
#include <vector>

#include "app_types.hpp"

namespace mk::app {

inline Text coreText(const mk::Msg& m, const std::string& fallback) {
    Text t;
    t.fallback = fallback;
    if (m.key.empty()) return t;
    t.key = "core/" + m.key;
    t.args = m.args;
    for (const auto& s : m.sub) t.sub.push_back(coreText(s, ""));
    return t;
}

inline Text coreError(const mk::Error& ex) { return coreText(ex.msg(), ex.what()); }

inline std::vector<Text> coreTexts(const mk::Texts& v) {
    std::vector<Text> out;
    for (size_t i = 0; i < v.size(); ++i) out.push_back(coreText(v.msg(i), v[i]));
    return out;
}

}  // namespace mk::app
