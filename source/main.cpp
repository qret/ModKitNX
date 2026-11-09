// SPDX-License-Identifier: GPL-3.0-or-later
// Based on TorrentShopNX by Langegen (GPL-3.0): source/main.cpp (язык до Application::init, romfs до UI,
// exitToHomeMode, выход через _exit после fsdevCommitDevice)
//
// ModKitNX — точка входа.
//   1. Не режим тайтла (апплет, запуск из Альбома) → экран Borealis «Запустите через игру» без ядра, игр и сети
//      (так делает TorrentShopNX); borealis не поднялся — текстовая заглушка.
//   2. Журнал, службы ns/ncm, язык (config.json; нет выбора — экран выбора языка при первом запуске).
//   3. Borealis: окно, тема (как в системе / светлая / тёмная — config.json), список игр.
//   4. Тихая проверка обновления (ui/update_ui); при выходе — замена .nro скачанной версией (platform/app_update).
// Службы romfs/pl/set/set:sys/psm/nifm/lbl и сокеты поднимает borealis (switch_wrapper.c, userAppInit) —
// свой userAppInit здесь не определяется, чтобы не было двойного определения.
#include <switch.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include <borealis.hpp>

#include "app_model.hpp"
#include "platform/app_update.hpp"
#include "platform/applet_mode.hpp"
#include "platform/log.hpp"
#include "platform/paths.hpp"
#include "ui/applet_activity.hpp"
#include "ui/common.hpp"
#include "ui/games_activity.hpp"
#include "ui/i18n.hpp"
#include "ui/update_ui.hpp"

namespace {

// Шрифт: borealis на Switch рисует весь текст romfs:/font/font.ttf (подмножество Noto Sans, resources/font/);
// чего в нём нет (японские и китайские названия игр), берётся из системных шрифтов. Японского Standard в той
// цепочке нет (switch_font.cpp добавляет его только без своего шрифта) — добавить в конец.
void addStandardFontFallback() {
    NVGcontext* vg = brls::Application::getNVGContext();
    const int def = brls::Application::getDefaultFont();
    const int standard = brls::Application::getFont(brls::FONT_REGULAR);
    if (vg && def != brls::FONT_INVALID && standard != brls::FONT_INVALID && def != standard)
        nvgAddFallbackFontId(vg, def, standard);
}

// 1. Режим апплета: только экран «Запустите через игру» (ui/applet_activity). Ядро, список игр, обновление и сеть
// не трогаются. Память апплета borealis хватает (TorrentShopNX показывает так же свой экран апплета).
int runAppletMode() {
    mk::plat::log::open();
    MK_LOG("main: %s %s start in applet mode, applet type %d", MK_APP_NAME, MK_APP_VERSION,
           mk::plat::appletTypeNumber());
    std::string lang = mk::ui::i18n::savedLanguage();
    brls::Platform::APP_LOCALE_DEFAULT = lang.empty() ? brls::LOCALE_AUTO : lang;
    if (!brls::Application::init()) {
        MK_LOG("main: borealis init failed in applet mode, text screen");
        mk::plat::log::close();
        mk::plat::runAppletModeBlock();
        return EXIT_SUCCESS;
    }
    const brls::ThemeVariant systemTheme = brls::Application::getPlatform()->getThemeVariant();
    brls::Application::createWindow(MK_APP_NAME);
    addStandardFontFallback();
    mk::ui::applyThemeAtStartup(systemTheme);
    if (lang.empty()) lang = mk::ui::i18n::bestMatch(brls::Application::getLocale());
    mk::ui::i18n::setLanguage(lang);
    brls::Application::pushActivity(new mk::ui::AppletActivity());
    while (brls::Application::mainLoop()) {
    }
    MK_LOG("main: exit (applet mode)");
    mk::plat::log::close();
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (!mk::plat::isTitleMode()) return runAppletMode();

    // 2. Журнал и службы.
    mk::plat::log::open();
    MK_LOG("main: %s %s start, applet type %d", MK_APP_NAME, MK_APP_VERSION, mk::plat::appletTypeNumber());
    // Папки приложения создаются сами: пользователю — куда класть sdk и моды.
    for (const std::string* dir : {&mk::plat::paths::kSdkDirOwn, &mk::plat::paths::kModsDir}) {
        const int rc = ::mkdir(dir->c_str(), 0777);
        if (rc != 0 && errno != EEXIST) MK_LOG("main: mkdir %s failed errno=%d", dir->c_str(), errno);
    }
    mk::app::Model::instance().startup();
    mk::plat::update::init(argc, argv);   // путь своего .nro (argv[0]) — для обновления

    // Язык выбирается до Application::init (как в TorrentShopNX): config.json {"language": …}. Нет выбора —
    // первый запуск: borealis — по языку системы, экран выбора языка — поверх списка игр (ui/language_activity).
    const std::string lang = mk::ui::i18n::savedLanguage();
    const bool askLanguage = lang.empty();
    brls::Platform::APP_LOCALE_DEFAULT = askLanguage ? brls::LOCALE_AUTO : lang;
    MK_LOG("main: language %s", askLanguage ? "(first run)" : lang.c_str());

    // 3. Borealis. Весь текст — шрифтом приложения romfs:/font/font.ttf (штатный SwitchFontLoader borealis берёт его
    // как шрифт по умолчанию; в системном Standard кириллицы нет, а китайский рисует её широкими глифами).
    if (!brls::Application::init()) {
        MK_LOG("main: borealis init failed");
        mk::app::Model::instance().shutdown();
        mk::plat::log::close();
        return EXIT_FAILURE;
    }
    // Тема консоли (Basic White / Basic Black): платформа borealis прочла её (setsysGetColorSetId) при создании —
    // взять ДО setThemeVariant, он перезаписывает то же поле.
    const brls::ThemeVariant systemTheme = brls::Application::getPlatform()->getThemeVariant();
    brls::Application::getPlatform()->exitToHomeMode(true);
    brls::Application::createWindow(MK_APP_NAME);
    addStandardFontFallback();
    // Выход посреди сети: Application::exit() шлёт exitEvent ДО Threading::stop (тот join-ит поток brls::async,
    // где идёт загрузка), — прервать сеть здесь, иначе выход ждёт таймаута запроса (до 20/600 с).
    brls::Application::getExitEvent()->subscribe([] { mk::plat::update::abortAll(); });
    mk::ui::applyThemeAtStartup(systemTheme);
    // «+» — выход только со списка игр: на экране операции выход посреди записи недопустим.
    brls::Application::setGlobalQuit(false);

    // Наши строки (ui/i18n) — тем же языком; при первом запуске — запасной en-US до выбора.
    mk::ui::i18n::setLanguage(askLanguage ? mk::ui::i18n::kFallback : lang);
    brls::Application::pushActivity(new mk::ui::GamesActivity(askLanguage));
    // Первый запуск — сначала выбор языка, окно обновления поверх него не показывается.
    if (!askLanguage) mk::ui::update::startupCheck();

    while (brls::Application::mainLoop()) {
    }

    MK_LOG("main: exit");
    // Сеть в фоне не должна пережить main (сокеты закрывает userAppExit): прервать и дождаться.
    mk::plat::update::abortAll();
    if (!mk::plat::update::waitIdle(5000)) MK_LOG("main: update download still running at exit");
    // Скачанная версия — на место своего .nro (romfs отмонтируется; после этого romfs не читать).
    mk::plat::update::applyPendingAtExit();
    mk::app::Model::instance().shutdown();
    mk::plat::log::close();
    fsdevCommitDevice("sdmc");
    return EXIT_SUCCESS;
}
