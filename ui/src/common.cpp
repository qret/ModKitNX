// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common.hpp"

#include "platform/app_config.hpp"
#include "platform/log.hpp"

namespace mk::ui {

std::string tr(const std::string& key) { return trv(key, {}); }

std::string trv(const std::string& key, const std::vector<std::string>& args) {
    bool found = false;
    const std::string s = i18n::raw("app/" + key, &found);
    return found ? i18n::format(s, args) : "app/" + key;
}

std::string trkv(const std::string& fullKey, const std::vector<std::string>& args) {
    bool found = false;
    const std::string s = i18n::raw(fullKey, &found);
    return found ? i18n::format(s, args) : fullKey;
}

std::string trText(const mk::app::Text& t) {
    if (!t.key.empty()) {
        bool found = false;
        const std::string s = i18n::raw(t.key, &found);
        if (found) {
            std::vector<std::string> args = t.args;
            for (const auto& sub : t.sub) args.push_back(trText(sub));
            return i18n::format(s, args);
        }
    }
    if (!t.fallback.empty()) return t.fallback;
    return t.key;
}

void translateIds(brls::View* root, std::initializer_list<const char*> keys) {
    if (!root) return;
    for (const char* k : keys) {
        brls::View* v = root->getView(std::string("tr/") + k);
        if (auto* l = dynamic_cast<brls::Label*>(v)) l->setText(tr(k));
        else if (auto* b = dynamic_cast<brls::Button*>(v)) b->setText(tr(k));
    }
}

void setFrameTitle(brls::Activity* a, const std::string& key) {
    if (auto* frame = dynamic_cast<brls::AppletFrame*>(a ? a->getContentView() : nullptr)) frame->setTitle(tr(key));
}

NVGcolor colText() { return brls::Application::getTheme()["brls/text"]; }
NVGcolor colSub() { return brls::Application::getTheme()["brls/header/subtitle"]; }
NVGcolor colOk() { return brls::Application::getTheme()["mk/ok"]; }
NVGcolor colWarn() { return brls::Application::getTheme()["mk/warn"]; }
NVGcolor colBad() { return brls::Application::getTheme()["mk/bad"]; }
NVGcolor colInfo() { return brls::Application::getTheme()["mk/info"]; }
NVGcolor colPanel() { return brls::Application::getTheme()["mk/panel"]; }
NVGcolor colLine() { return brls::Application::getTheme()["mk/line"]; }
NVGcolor colTrack() { return brls::Application::getTheme()["mk/track"]; }

namespace {
bool g_systemLight = false;
}

ThemeChoice savedTheme() {
    const std::string v = mk::plat::config::getString(mk::plat::config::kTheme, "system");
    if (v == "light") return ThemeChoice::Light;
    if (v == "dark") return ThemeChoice::Dark;
    return ThemeChoice::System;
}

bool saveTheme(ThemeChoice c) {
    const char* v = c == ThemeChoice::Light ? "light" : c == ThemeChoice::Dark ? "dark" : "system";
    return mk::plat::config::setString(mk::plat::config::kTheme, v);
}

bool systemThemeIsLight() { return g_systemLight; }

void applyThemeAtStartup(brls::ThemeVariant system) {
    g_systemLight = system == brls::ThemeVariant::LIGHT;
    const ThemeChoice c = savedTheme();
    const bool light = c == ThemeChoice::Light || (c == ThemeChoice::System && g_systemLight);
    brls::Application::getPlatform()->setThemeVariant(light ? brls::ThemeVariant::LIGHT : brls::ThemeVariant::DARK);
    MK_LOG("theme: system %s, setting %s -> %s", g_systemLight ? "light" : "dark",
           c == ThemeChoice::System ? "system" : (c == ThemeChoice::Light ? "light" : "dark"), light ? "light" : "dark");

    // Наши цвета. Тёмная — прежние значения; светлая — темнее и насыщеннее, чтобы читалось на светлом фоне
    // (оранжевый #FF9800 и голубой #29B6F6 на белом почти не видны), карточки — белые на сером фоне borealis.
    brls::Theme& d = brls::Theme::getDarkTheme();
    d.addColor("mk/ok", nvgRGB(76, 175, 80));
    d.addColor("mk/warn", nvgRGB(255, 152, 0));
    d.addColor("mk/bad", nvgRGB(229, 57, 53));
    d.addColor("mk/info", nvgRGB(41, 182, 246));
    d.addColor("mk/panel", nvgRGBA(255, 255, 255, 18));
    d.addColor("mk/line", nvgRGBA(255, 255, 255, 40));
    d.addColor("mk/track", nvgRGBA(255, 255, 255, 30));
    brls::Theme& l = brls::Theme::getLightTheme();
    l.addColor("mk/ok", nvgRGB(46, 125, 50));
    l.addColor("mk/warn", nvgRGB(216, 67, 21));
    l.addColor("mk/bad", nvgRGB(198, 40, 40));
    l.addColor("mk/info", nvgRGB(2, 119, 189));
    l.addColor("mk/panel", nvgRGB(255, 255, 255));
    l.addColor("mk/line", nvgRGBA(0, 0, 0, 38));
    l.addColor("mk/track", nvgRGBA(0, 0, 0, 28));
}

brls::Label* makeLabel(const std::string& text, float size, NVGcolor color, bool singleLine) {
    auto* l = new brls::Label();
    l->setText(text);
    l->setFontSize(size);
    l->setTextColor(color);
    l->setSingleLine(singleLine);
    return l;
}

brls::Box* makeColumn() {
    auto* b = new brls::Box(brls::Axis::COLUMN);
    return b;
}

brls::Box* makeRow() {
    auto* b = new brls::Box(brls::Axis::ROW);
    b->setAlignItems(brls::AlignItems::CENTER);
    return b;
}

brls::Box* makeCard() {
    auto* b = makeColumn();
    b->setBackgroundColor(colPanel());
    b->setCornerRadius(10);
    b->setPadding(18, 22, 18, 22);
    b->setMarginBottom(18);
    return b;
}

brls::Box* makeSectionTitle(const std::string& text) {
    auto* b = makeRow();
    b->setMarginBottom(10);
    b->addView(makeLabel(text, 24, colText(), true));
    return b;
}

brls::Box* makeInfoLine(const std::string& key, const std::string& value, NVGcolor valueColor) {
    auto* row = makeRow();
    row->setMarginBottom(6);
    auto* k = makeLabel(key, 19, colSub(), true);
    k->setWidth(330);
    row->addView(k);
    auto* v = makeLabel(value, 19, valueColor);
    v->setGrow(1.0f);
    row->addView(v);
    return row;
}

brls::Box* makeBadge(const std::string& text, NVGcolor bg) {
    auto* b = makeRow();
    b->setBackgroundColor(bg);
    b->setCornerRadius(5);
    b->setPadding(3, 9, 3, 9);
    b->setMarginRight(8);
    b->addView(makeLabel(text, 15, nvgRGB(255, 255, 255), true));
    return b;
}

brls::Button* makeButton(const std::string& text, const brls::ButtonStyle* style, std::function<void()> onClick) {
    auto* b = new brls::Button();
    b->setText(text);
    b->setStyle(style);
    b->setHeight(56);
    b->setMarginRight(16);
    b->setPadding(0, 28, 0, 28);
    // По ширине текста и в колонке: иначе колонка (карточка, экран итога) растягивает кнопку на всю ширину.
    b->setAlignSelf(brls::AlignSelf::FLEX_START);
    b->registerClickAction([onClick](brls::View*) {
        if (onClick) onClick();
        return true;
    });
    return b;
}

ChoiceRow::ChoiceRow(const std::string& title, const std::string& right, const std::string& sub)
    : brls::Box(brls::Axis::ROW) {
    setFocusable(true);
    setAlignItems(brls::AlignItems::CENTER);
    setPadding(10, 16, 10, 16);
    setMarginBottom(4);
    setCornerRadius(8);
    setMinHeight(56);

    mark_ = makeLabel("○", 26, colSub(), true);
    mark_->setWidth(44);
    addView(mark_);

    auto* col = makeColumn();
    col->setGrow(1.0f);
    col->addView(makeLabel(title, 20, colText()));
    if (!sub.empty()) col->addView(makeLabel(sub, 15, colSub()));
    addView(col);

    if (!right.empty()) {
        auto* r = makeLabel(right, 24, colInfo(), true);
        r->setMarginLeft(16);
        addView(r);
    }
    registerClickAction([this](brls::View*) {
        if (pick_) pick_();
        return true;
    });
}

void ChoiceRow::setChecked(bool checked) {
    checked_ = checked;
    mark_->setText(checked ? "●" : "○");
    mark_->setTextColor(checked ? colOk() : colSub());
}

LanguageRow::LanguageRow(const std::string& name, const std::string& english, const std::string& hint)
    : brls::Box(brls::Axis::ROW) {
    setFocusable(true);
    setAlignItems(brls::AlignItems::CENTER);
    setHeight(70);
    setPadding(0, 22, 0, 22);
    setLineColor(colLine());
    setLineBottom(1);

    name_ = makeLabel(name, 24, colText(), true);
    addView(name_);
    if (!english.empty() && english != name) {
        auto* en = makeLabel(english, 18, colSub(), true);
        en->setMarginLeft(14);
        addView(en);
    }
    auto* spacer = new brls::Box();
    spacer->setGrow(1.0f);
    addView(spacer);
    mark_ = new brls::CheckBox();
    mark_->setVisibility(brls::Visibility::INVISIBLE);
    addView(mark_);

    registerAction(hint, brls::ControllerButton::BUTTON_A, [this](brls::View*) {
        if (pick_) pick_();
        return true;
    });
    addGestureRecognizer(new brls::TapGestureRecognizer(this));
}

void LanguageRow::setChecked(bool checked) {
    checked_ = checked;
    mark_->setVisibility(checked ? brls::Visibility::VISIBLE : brls::Visibility::INVISIBLE);
    // Как у RadioCell borealis: выбранный пункт — цветом значения списка.
    name_->setTextColor(checked ? brls::Application::getTheme()["brls/list/listItem_value_color"] : colText());
}

void LanguageRow::onFocusGained() {
    brls::Box::onFocusGained();
    if (focus_) focus_();
}

ProgressBar::ProgressBar() : brls::Box(brls::Axis::ROW) {
    setHeight(18);
    setCornerRadius(9);
    setBackgroundColor(colTrack());
    fill_ = new brls::Box();
    fill_->setHeight(18);
    fill_->setCornerRadius(9);
    fill_->setBackgroundColor(colOk());
    fill_->setWidthPercentage(0);
    addView(fill_);
}

void ProgressBar::setProgress(float v) {
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    fill_->setWidthPercentage(v * 100.0f);
}

}  // namespace mk::ui
