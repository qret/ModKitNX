// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/no_keys_activity.hpp"

#include "ui/common.hpp"

namespace mk::ui {

void NoKeysActivity::onContentAvailable() {
    setFrameTitle(this, "nokeys/title");
    translateIds(getContentView(), {"nokeys/headline", "nokeys/lead", "nokeys/step1", "nokeys/step2", "nokeys/step3",
                                    "nokeys/step4", "nokeys/step5", "nokeys/privacy"});
    btnOk->setText(tr("nokeys/ok"));
    btnOk->registerClickAction([](brls::View*) {
        brls::Application::popActivity();
        return true;
    });
    brls::Application::giveFocus(btnOk);
}

}  // namespace mk::ui
