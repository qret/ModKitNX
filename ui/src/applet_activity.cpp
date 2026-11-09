// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — экран режима апплета. См. ui/applet_activity.hpp.
#include "ui/applet_activity.hpp"

#include "platform/paths.hpp"
#include "ui/common.hpp"

namespace mk::ui {

void AppletActivity::onContentAvailable() {
    icon->setImageFromRes("icon.jpg");   // своя иконка ModKitNX (resources/CREDITS.md)
    translateIds(getContentView(), {"applet/title", "applet/lead", "applet/step1", "applet/step2"});
    step3->setText(trf("applet/step3", MK_APP_NAME));
    btnExit->setText(tr("applet/exit"));
    btnExit->registerClickAction([](brls::View*) {
        brls::Application::quit();
        return true;
    });
    // B — тоже выход: другого экрана здесь нет.
    getContentView()->registerAction(tr("applet/exit"), brls::ControllerButton::BUTTON_B, [](brls::View*) {
        brls::Application::quit();
        return true;
    });
    brls::Application::giveFocus(btnExit);
}

}  // namespace mk::ui
