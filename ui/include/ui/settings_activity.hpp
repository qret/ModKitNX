// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — «Настройки и о программе»: версия, язык, тема, обновления, паспорт консоли, папки, благодарности (Луня — Langegen).
#pragma once

#include <borealis.hpp>

#include <string>
#include <vector>

#include "ui/common.hpp"

namespace mk::ui {

class SettingsActivity : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("settings_activity.xml");
    ~SettingsActivity() override;
    void onContentAvailable() override;

private:
    BRLS_BIND(brls::Box, body, "body");
    std::vector<LanguageRow*> langRows_;
    std::vector<LanguageRow*> themeRows_;
    AliveToken alive_ = makeAlive();
};

}  // namespace mk::ui
