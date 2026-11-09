// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — экран «Собрать логи для отправки». См. ui/logs_activity.hpp.
#include "ui/logs_activity.hpp"

#include "app_model.hpp"

namespace mk::ui {

LogsActivity::LogsActivity(mk::app::GameDetails d) : d_(std::move(d)) {}

LogsActivity::~LogsActivity() { *alive_ = false; }

void LogsActivity::onContentAvailable() {
    setFrameTitle(this, "logs/title");
    body->clearViews();
    body->addView(makeLabel(tr("logs/working"), 22, colSub()));
    AliveToken alive = alive_;
    const mk::app::GameDetails d = d_;
    brls::async([this, alive, d]() {
        mk::app::LogsView v = mk::app::Model::instance().collectLogs(d);
        brls::sync([this, alive, v = std::move(v)]() {
            if (*alive) show(v);
        });
    });
}

void LogsActivity::show(const mk::app::LogsView& v) {
    body->clearViews();
    if (!v.ok) {
        body->addView(makeLabel(tr("logs/failed"), 24, colBad()));
        if (!v.error.empty()) body->addView(makeLabel(trText(v.error), 18, colSub()));
        addBack();
        return;
    }
    auto* card = makeCard();
    card->addView(makeSectionTitle(tr("logs/done")));
    card->addView(makeLabel(tr("logs/zip"), 18, colSub()));
    auto* zip = makeLabel(v.zip, 24, colOk());
    zip->setMarginBottom(10);
    card->addView(zip);
    card->addView(makeLabel(tr("logs/send_hint"), 21, colText()));
    auto* folder = makeLabel(trf("logs/folder", v.folder), 16, colSub());
    folder->setMarginTop(10);
    card->addView(folder);
    card->addView(makeLabel(tr("logs/no_secrets"), 16, colSub()));
    body->addView(card);

    auto* list = makeCard();
    list->addView(makeSectionTitle(trf("logs/inside", std::to_string(v.files))));
    for (const auto& t : v.items) list->addView(makeLabel(trText(t), 16, colText()));
    for (const auto& t : v.warnings) {
        auto* w = makeLabel(trText(t), 17, colWarn());
        w->setMarginTop(6);
        list->addView(w);
    }
    body->addView(list);
    addBack();
}

// На экране нет ничего выбираемого: без кнопки фокус некуда отдать — ни подсказки «Назад» внизу, ни выхода по B.
void LogsActivity::addBack() {
    auto* back = makeButton(tr("common/back"), &brls::BUTTONSTYLE_BORDERED,
                            [] { brls::Application::popActivity(brls::TransitionAnimation::FADE); });
    back->setId("logs_back");
    back->setMarginTop(8);
    back->setMarginBottom(30);
    body->addView(back);
    brls::Application::giveFocus(back);
}

}  // namespace mk::ui
