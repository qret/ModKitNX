// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — перевод интерфейса и выбор языка.
//
// Переводы — resources/i18n/<код>/*.json (app.json — экраны, core.json — план и ошибки ядра, hints.json —
// подсказки кнопок borealis). Список языков — ОДНО место: resources/i18n/languages.json (код и самоназвание);
// новый язык добавляется папкой и строкой в этом файле, без правки кода. Запасной язык — en-US: ключ, которого
// нет в выбранном языке, берётся оттуда. Как переводить — TRANSLATING.md в корне.
//
// Строки ищутся здесь, а не через brls::getStr: язык можно сменить на ходу (экран первого запуска), а у borealis
// язык задаётся один раз до Application::init. Язык из конфига ставится и тут, и в borealis до init — тогда и
// подсказки кнопок borealis на том же языке.
#pragma once

#include <string>
#include <vector>

namespace mk::ui::i18n {

struct Language {
    std::string code;   // имя папки в resources/i18n/ ("en-US", "ru", "uk")
    std::string name;   // самоназвание ("English", "Русский", "Українська")
    std::string english; // имя по-английски ("Russian"); у английского — то же, что name
    std::string choose; // «Выберите язык» на этом языке — экран первого запуска
};

const char* const kFallback = "en-US";   // запасной язык (папка, где есть все ключи)

// Языки из resources/i18n/languages.json (порядок файла). Файла нет или он битый — один en-US.
const std::vector<Language>& languages();
bool known(const std::string& code);
// Язык из списка, ближайший к локали системы ("ru", "en-GB", "zh-Hans"): точное совпадение, затем по языку без
// региона ("en-GB" → "en-US"); не нашлось — en-US.
std::string bestMatch(const std::string& locale);

// Загрузить строки языка (и запасного en-US). Неизвестный код — en-US.
void setLanguage(const std::string& code);
const std::string& language();

// Строка по полному ключу ("app/games/title", "core/plan/slot_free"); нет нигде — пустая строка и found=false.
std::string raw(const std::string& key, bool* found = nullptr);
// То же, но на языке code, не меняя текущий (экран первого запуска показывает текст языка под курсором).
// Строки языка читаются один раз и кешируются; нет ключа — из en-US.
std::string rawIn(const std::string& code, const std::string& key);

// Подставить {0}, {1}… (как fmt у borealis, но число параметров — во время работы).
std::string format(const std::string& tmpl, const std::vector<std::string>& args);

// Язык, выбранный пользователем: sdmc:/switch/<APP>/config.json {"language": "uk"}. Нет файла — прежний
// settings.ini (lang=ru|en) как выбор; нет и его — "" (первый запуск: спросить).
std::string savedLanguage();
// Записать выбор в config.json (прочие поля файла сохраняются). false — карта не записалась.
bool saveLanguage(const std::string& code);

}  // namespace mk::ui::i18n
