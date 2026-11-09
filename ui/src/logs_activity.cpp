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
    // Пока собирается — выбирать нечего: фокус на теле без рамки (иначе пустая рамка на прокрутке); B — как обычно.
    body->setFocusable(true);
    body->setHideHighlight(true);
    body->setHideClickAnimation(true);
    brls::Application::giveFocus(body);
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
    body->setFocusable(false);   // фокус уйдёт на «Назад» (addBack)
    if (!v.ok) {
        auto* fail = makeSubCard(Tone::Bad, icon::kBad, tr("logs/failed"), 22);
        fail->setMarginTop(0);
        if (!v.error.empty()) addSubLine(fail, trText(v.error), colText(), 18);
        body->addView(fail);
        addBack();
        return;
    }
    auto* card = makeCard();
    card->addView(makeSectionTitle(tr("logs/done")));
    card->addView(makeLabel(tr("logs/zip"), 18, colSub()));
    auto* zip = makeLabel(v.zip, 24, colOk());
    zip->setMarginBottom(10);
    card->addView(zip);
    // Что сделать с архивом — главное на экране: синей подкарточкой.
    card->addView(makeSubCard(Tone::Info, icon::kInfo, tr("logs/send_hint"), 20));
    auto* folder = makeLabel(trf("logs/folder", v.folder), 16, colSub());
    folder->setMarginTop(10);
    card->addView(folder);
    card->addView(makeLabel(tr("logs/no_secrets"), 16, colSub()));
    body->addView(card);

    auto* list = makeCard();
    list->addView(makeSectionTitle(trf("logs/inside", std::to_string(v.files))));
    for (const auto& t : v.items) list->addView(makeLabel(trText(t), 16, colText()));
    // Чего в архиве нет — жёлтой подкарточкой под списком.
    if (!v.warnings.empty()) {
        auto* w = makeSubCard(Tone::Warn, icon::kWarn, tr("common/attention"), 19);
        for (const auto& t : v.warnings) addSubLine(w, trText(t), colText(), 17);
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
