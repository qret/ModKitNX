// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — выбор языка при первом запуске. Композиция по центру: иконка приложения, крупное приветствие и
// подзаголовок «Выберите язык», под ними список языков из resources/i18n/languages.json (самоназвание + имя
// по-английски, без флагов). Курсор и отметка сразу стоят на языке системы консоли; приветствие и подсказка A
// меняются на язык строки под курсором. Выбор пишется в sdmc:/switch/<APP>/config.json и применяется сразу; при
// следующих запусках экран не показывается. Сменить язык потом — «Настройки».
#pragma once

#include <borealis.hpp>

#include <functional>
#include <vector>

#include "ui/common.hpp"

namespace mk::ui {

class LanguageActivity : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("language_activity.xml");
    explicit LanguageActivity(std::function<void()> onChosen) : onChosen_(std::move(onChosen)) {}
    void onContentAvailable() override;

private:
    void showTexts(size_t i);   // приветствие и подзаголовок на языке i
    void choose(size_t i);

    BRLS_BIND(brls::Box, body, "body");
    brls::Label* welcome_ = nullptr;
    brls::Label* subtitle_ = nullptr;
    brls::Label* later_ = nullptr;
    std::vector<LanguageRow*> rows_;
    std::function<void()> onChosen_;
    bool chosen_ = false;
};

}  // namespace mk::ui
