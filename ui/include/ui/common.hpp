// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — общие кирпичи экранов: перевод, цвета, простые виды. Один стиль на всё приложение:
// крупный текст, один главный вопрос на экран, выбор — прямо под вопросом.
#pragma once

#include <borealis.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app_types.hpp"
#include "ui/i18n.hpp"

namespace mk::ui {

// ---- перевод (ui/i18n.hpp): ключи — resources/i18n/<язык>/app.json, здесь — без префикса «app/» ----
// Нет строки ни в языке, ни в en-US — показывается сам ключ (видно, что перевода не хватает).
std::string tr(const std::string& key);
std::string trv(const std::string& key, const std::vector<std::string>& args);
template <typename... Args>
std::string trf(const std::string& key, Args&&... args) {
    return trv(key, std::vector<std::string>{std::string(std::forward<Args>(args))...});
}
// Строка из другого файла переводов — полный ключ «<файл>/<ключ>» ("update/btn_update" — update.json,
// "theme/title" — theme.json). Нет строки — сам ключ.
std::string trkv(const std::string& fullKey, const std::vector<std::string>& args);
inline std::string trk(const std::string& fullKey) { return trkv(fullKey, {}); }
template <typename... Args>
std::string trkf(const std::string& fullKey, Args&&... args) {
    return trkv(fullKey, std::vector<std::string>{std::string(std::forward<Args>(args))...});
}
// Текст ядра или приложения с полным ключом (mk::app::Text); нет перевода — запасной русский текст ядра.
std::string trText(const mk::app::Text& t);
// Тексты из XML: у вида id="tr/<ключ>" (Label или Button) — текст ставится здесь, через наш перевод (язык можно
// сменить на ходу, а «@i18n/…» borealis разбирает языком, заданным до init). keys — ключи без «app/».
void translateIds(brls::View* root, std::initializer_list<const char*> keys);
// Заголовок AppletFrame экрана — переводом.
void setFrameTitle(brls::Activity* a, const std::string& key);

// ---- тема: светлая / тёмная, по умолчанию — как в системе ----
enum class ThemeChoice { System, Light, Dark };
ThemeChoice savedTheme();                   // config.json "theme": "system" | "light" | "dark"; нет — System
bool saveTheme(ThemeChoice choice);         // false — карта не записалась
// Сразу после brls::Application::init, до первого экрана. system — тема консоли (Basic White / Basic Black):
// её читает платформа borealis при создании (setsysGetColorSetId) — взять getPlatform()->getThemeVariant() ДО
// любого setThemeVariant, он перезаписывает то же поле. Ставит тему borealis по настройке и регистрирует наши цвета
// mk/* в обеих темах (их же берут XML: textColor="@theme/mk/warn"). Тема меняется только при запуске: виды
// копируют цвета при создании.
void applyThemeAtStartup(brls::ThemeVariant system);
bool systemThemeIsLight();   // тема консоли, запомненная при запуске

// ---- цвета (одни и те же везде; у каждой темы свои значения — тема borealis, ключи mk/*) ----
NVGcolor colText();
NVGcolor colSub();      // второстепенный текст
NVGcolor colOk();       // зелёный — «всё хорошо / установлено»
NVGcolor colWarn();     // оранжевый — «нужно действие»
NVGcolor colBad();      // красный — «ошибка / нельзя»
NVGcolor colInfo();     // синий — справка
NVGcolor colPanel();    // фон карточки
NVGcolor colLine();     // линия-разделитель списка
NVGcolor colTrack();    // дорожка полосы прогресса

// ---- виды ----
brls::Label* makeLabel(const std::string& text, float size, NVGcolor color, bool singleLine = false);
brls::Box* makeColumn();
brls::Box* makeRow();
brls::Box* makeCard();                                    // скруглённая панель с отступами
brls::Box* makeSectionTitle(const std::string& text);     // заголовок блока
brls::Box* makeInfoLine(const std::string& key, const std::string& value, NVGcolor valueColor);
brls::Box* makeBadge(const std::string& text, NVGcolor bg);
brls::Button* makeButton(const std::string& text, const brls::ButtonStyle* style, std::function<void()> onClick);

// Строка выбора «● вариант   справа-крупно» (радиокнопка). Фокусируемая, A — выбрать.
class ChoiceRow : public brls::Box {
public:
    ChoiceRow(const std::string& title, const std::string& right, const std::string& sub);
    void setChecked(bool checked);
    bool isChecked() const { return checked_; }
    void onPick(std::function<void()> cb) { pick_ = std::move(cb); }

private:
    brls::Label* mark_;
    bool checked_ = false;
    std::function<void()> pick_;
};

// Строка списка языков (экран первого запуска и «Настройки»), как системные списки Switch: самоназвание крупно,
// рядом мелким серым имя по-английски, справа круглая отметка выбранного (brls::CheckBox), снизу линия-разделитель.
// Фокус — рамкой borealis; A и касание — pick. hint — подпись кнопки A в подвале (на языке строки).
class LanguageRow : public brls::Box {
public:
    LanguageRow(const std::string& name, const std::string& english, const std::string& hint);
    void setChecked(bool checked);
    bool isChecked() const { return checked_; }
    void onPick(std::function<void()> cb) { pick_ = std::move(cb); }
    void onFocus(std::function<void()> cb) { focus_ = std::move(cb); }
    void onFocusGained() override;

private:
    brls::Label* name_;
    brls::CheckBox* mark_;
    bool checked_ = false;
    std::function<void()> pick_, focus_;
};

// Полоса прогресса (без анимации, 0..1).
class ProgressBar : public brls::Box {
public:
    ProgressBar();
    void setProgress(float v);

private:
    brls::Box* fill_;
};

// Токен «экран ещё жив» для brls::async → brls::sync.
using AliveToken = std::shared_ptr<bool>;
inline AliveToken makeAlive() { return std::make_shared<bool>(true); }

}  // namespace mk::ui
