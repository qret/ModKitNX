// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — экран режима апплета (запуск из Альбома). Как запустить
// приложение в режиме тайтла: три пронумерованных шага и «Выйти». Ни ядра, ни игр, ни сети — только этот экран
// (source/main.cpp). Язык — сохранённый или системный; тема — как везде. Если borealis не поднялся — текстовая
// заглушка platform/applet_mode.hpp (runAppletModeBlock).
#pragma once

#include <borealis.hpp>

namespace mk::ui {

class AppletActivity : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("applet_activity.xml");
    void onContentAvailable() override;

private:
    BRLS_BIND(brls::Image, icon, "icon");
    BRLS_BIND(brls::Label, step3, "step3");
    BRLS_BIND(brls::Button, btnExit, "btnExit");
};

}  // namespace mk::ui
