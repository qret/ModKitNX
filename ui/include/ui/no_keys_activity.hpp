// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — экран «Нет ключей консоли»: крупные нумерованные шаги Lockpick_RCM (текст — спецификация).
// Показывается только у функций, которым ключи нужны; остальное приложение работает без них.
#pragma once

#include <borealis.hpp>

namespace mk::ui {

class NoKeysActivity : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("no_keys_activity.xml");
    void onContentAvailable() override;

private:
    BRLS_BIND(brls::Button, btnOk, "btnOk");
};

}  // namespace mk::ui
