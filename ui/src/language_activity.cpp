// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — выбор языка при первом запуске. См. ui/language_activity.hpp.
#include "ui/language_activity.hpp"

#include "platform/log.hpp"
#include "platform/paths.hpp"

namespace mk::ui {

void LanguageActivity::onContentAvailable() {
    const auto& langs = i18n::languages();
    // Язык системы консоли: borealis на первом запуске берёт его у HOS (APP_LOCALE_DEFAULT = auto, source/main.cpp).
    const std::string sys = i18n::bestMatch(brls::Application::getLocale());
    size_t start = 0;
    for (size_t i = 0; i < langs.size(); ++i)
        if (langs[i].code == sys) start = i;

    auto* icon = new brls::Image();
    icon->setImageFromRes("icon.jpg");   // своя иконка ModKitNX (resources/CREDITS.md)
    icon->setWidth(112);
    icon->setHeight(112);
    icon->setCornerRadius(24);
    icon->setScalingType(brls::ImageScalingType::FIT);
    body->addView(icon);

    welcome_ = makeLabel("", 40, colText(), true);
    welcome_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    welcome_->setMarginTop(26);
    body->addView(welcome_);
    subtitle_ = makeLabel("", 21, colSub(), true);
    subtitle_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    subtitle_->setMarginTop(8);
    subtitle_->setMarginBottom(30);
    body->addView(subtitle_);

    auto* list = makeColumn();
    list->setWidth(620);
    list->setLineColor(colLine());
    list->setLineTop(1);
    for (size_t i = 0; i < langs.size(); ++i) {
        auto* r = new LanguageRow(langs[i].name, langs[i].english, i18n::rawIn(langs[i].code, "app/first_run/select"));
        r->setId("lang/" + langs[i].code);   // по нему предпросмотр ставит фокус
        r->setChecked(i == start);
        r->onFocus([this, i] { showTexts(i); });
        r->onPick([this, i] { choose(i); });
        rows_.push_back(r);
        list->addView(r);
    }
    body->addView(list);
    later_ = makeLabel("", 17, colSub(), true);
    later_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    later_->setMarginTop(22);
    body->addView(later_);

    // Без выбора дальше не идём: B ничего не делает.
    getContentView()->registerAction("", brls::ControllerButton::BUTTON_B, [](brls::View*) { return true; }, true);
    showTexts(start);
    if (!rows_.empty()) brls::Application::giveFocus(rows_[start]);
}

void LanguageActivity::showTexts(size_t i) {
    const auto& langs = i18n::languages();
    if (i >= langs.size() || chosen_) return;
    welcome_->setText(i18n::rawIn(langs[i].code, "app/first_run/welcome"));
    subtitle_->setText(langs[i].choose);
    later_->setText(i18n::rawIn(langs[i].code, "app/first_run/later"));
}

void LanguageActivity::choose(size_t i) {
    const auto& langs = i18n::languages();
    if (chosen_ || i >= langs.size()) return;
    showTexts(i);
    chosen_ = true;
    for (size_t k = 0; k < rows_.size(); ++k) rows_[k]->setChecked(k == i);
    const std::string code = langs[i].code;
    i18n::setLanguage(code);
    const bool saved = i18n::saveLanguage(code);
    MK_LOG("language: chosen %s, saved=%d", code.c_str(), saved);
    // Короткая пауза: отметка и текст на выбранном языке успевают показаться, потом — плавно к списку игр.
    std::function<void()> cb = onChosen_;
    brls::delay(350, [cb, saved] {
        brls::Application::popActivity(brls::TransitionAnimation::FADE, [cb, saved] {
            if (cb) cb();
            if (!saved) brls::Application::notify(tr("settings/save_failed"));
        });
    });
}

}  // namespace mk::ui
