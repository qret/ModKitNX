// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — «Найти sdk в других играх». См. ui/sdk_harvest_activity.hpp.
#include "ui/sdk_harvest_activity.hpp"
#include "ui/text_sheet.hpp"

#include "app_model.hpp"
#include "platform/paths.hpp"

namespace mk::ui {

namespace {

std::string shortSha(const std::string& s) { return s.size() > 12 ? s.substr(0, 12) : s; }

std::string gameWithVersion(const mk::app::HarvestGame& g) {
    return g.version.empty() ? g.name : g.name + " " + g.version;
}

}  // namespace

SdkHarvestActivity::SdkHarvestActivity(std::string gameName, std::string needText, std::function<void()> onDone)
    : gameName_(std::move(gameName)), needText_(std::move(needText)), onDone_(std::move(onDone)) {}

SdkHarvestActivity::~SdkHarvestActivity() {
    *alive_ = false;
    cancel_->store(true);   // ушли с экрана посреди поиска — фоновый обход остановится после текущей игры
}

void SdkHarvestActivity::onContentAvailable() {
    if (auto* frame = dynamic_cast<brls::AppletFrame*>(getContentView())) frame->setTitle(trk("sdkscan/title"));
    // B: во время поиска — остановить (проверенное сохраняется в кэш), во время записи — нельзя.
    body->registerAction(tr("hint/back"), brls::ControllerButton::BUTTON_B, [this](brls::View*) {
        if (saving_) {
            brls::Application::notify(trk("sdkscan/wait_saving"));
            return true;
        }
        if (busy_) {
            cancel_->store(true);
            brls::Application::notify(trk("sdkscan/stopping"));
            return true;
        }
        finish();
        return true;
    });
    startScan();
}

void SdkHarvestActivity::startScan() {
    busy_ = true;
    body->clearViews();
    body->addView(makeLabel(trk("sdkscan/scanning"), 28, colText()));
    auto* note = makeLabel(trk("sdkscan/scanning_note"), 19, colSub());
    note->setMarginTop(8);
    body->addView(note);
    bar_ = new ProgressBar();
    bar_->setMarginTop(22);
    body->addView(bar_);
    countLabel_ = makeLabel(trkv("sdkscan/progress", {"0", "…"}), 22, colText(), true);
    countLabel_->setMarginTop(12);
    body->addView(countLabel_);
    gameLabel_ = makeLabel("", 18, colSub(), true);
    gameLabel_->setMarginTop(6);
    body->addView(gameLabel_);
    auto* hint = makeLabel(trk("sdkscan/stop_hint"), 17, colSub());
    hint->setMarginTop(24);
    body->addView(hint);
    body->setFocusable(true);
    body->setHideHighlight(true);
    brls::Application::giveFocus(body);

    AliveToken alive = alive_;
    auto cancel = cancel_;
    brls::async([this, alive, cancel]() {
        auto progress = [this, alive](size_t done, size_t total, const std::string& name) {
            brls::sync([this, alive, done, total, name]() {
                if (!*alive || !bar_) return;
                bar_->setProgress(total ? static_cast<float>(done) / static_cast<float>(total) : 0.f);
                countLabel_->setText(trkv("sdkscan/progress", {std::to_string(done), std::to_string(total)}));
                gameLabel_->setText(name);
            });
        };
        mk::app::HarvestView v = mk::app::Model::instance().findSdkInGames(progress, cancel.get());
        brls::sync([this, alive, v = std::move(v)]() mutable {
            if (!*alive) return;
            busy_ = false;
            view_ = std::move(v);
            bar_ = nullptr;
            countLabel_ = gameLabel_ = nullptr;
            body->setFocusable(false);
            if (view_.ok && !view_.lines.empty()) showChoice();
            else showNothing();
        });
    });
}

std::string SdkHarvestActivity::lineTitle(const mk::app::HarvestLine& l) const {
    const mk::app::HarvestGame g = l.games.empty() ? mk::app::HarvestGame{} : l.games.front();
    return trkv("sdkscan/line", {l.version, gameWithVersion(g)});
}

std::string SdkHarvestActivity::lineSub(const mk::app::HarvestLine& l, size_t index) const {
    std::vector<std::string> parts;
    if (l.recommended) parts.push_back(trkv("sdkscan/badge_recommended", {view_.firmware}));
    else if (index == 0 && !view_.tableSdk.empty()) parts.push_back(trkv("sdkscan/badge_closest", {view_.tableSdk}));
    if (l.games.size() > 1) parts.push_back(trkv("sdkscan/also_in", {std::to_string(l.games.size() - 1)}));
    if (l.otherBuild) parts.push_back(trk("sdkscan/other_build"));
    if (l.targetAlready) parts.push_back(trk("sdkscan/already_there"));
    std::string s;
    for (const auto& p : parts) s += (s.empty() ? "" : " · ") + p;
    return s;
}

void SdkHarvestActivity::showChoice() {
    body->clearViews();
    rows_.clear();
    body->addView(makeLabel(trk("sdkscan/found_title"), 28, colText()));
    auto* stats = makeLabel(view_.cancelled ? trkv("sdkscan/stats_cancelled", {std::to_string(view_.checked),
                                                                             std::to_string(view_.total)})
                                            : trkv("sdkscan/stats", {std::to_string(view_.checked),
                                                                   std::to_string(view_.fromCache)}),
                            18, colSub());
    stats->setMarginTop(6);
    body->addView(stats);

    auto* card = makeCard();
    card->setMarginTop(14);
    // Вопрос и выбор — прямо под ним.
    auto* q = makeLabel(trk("sdkscan/question"), 22, colText());
    q->setMarginBottom(8);
    card->addView(q);
    targetLabel_ = makeLabel("", 18, colInfo());
    auto refresh = [this]() {
        for (size_t i = 0; i < rows_.size(); ++i) rows_[i]->setChecked(static_cast<int>(i) == choice_);
        if (choice_ >= 0 && choice_ < static_cast<int>(view_.lines.size()) && targetLabel_) {
            const auto& l = view_.lines[choice_];
            targetLabel_->setText(l.targetAlready ? trkv("sdkscan/target_already", {l.targetDir})
                                                  : trkv("sdkscan/target", {l.targetDir + "/sdk"}));
        }
    };
    choice_ = 0;
    for (size_t i = 0; i < view_.lines.size(); ++i) {
        const auto& l = view_.lines[i];
        auto* r = new ChoiceRow(lineTitle(l), l.version, lineSub(l, i));
        const int idx = static_cast<int>(i);
        r->onPick([this, refresh, idx] {
            choice_ = idx;
            refresh();
        });
        rows_.push_back(r);
        card->addView(r);
    }
    targetLabel_->setMarginTop(12);
    card->addView(targetLabel_);
    auto* srcNote = makeLabel(trk("sdkscan/source_note"), 16, colSub());
    srcNote->setMarginTop(4);
    card->addView(srcNote);
    body->addView(card);
    refresh();

    auto* buttons = makeRow();
    buttons->setMarginTop(18);
    buttons->setMarginBottom(30);
    auto* go = makeButton(trk("sdkscan/btn_save"), &brls::BUTTONSTYLE_PRIMARY, [this] { save(); });
    go->setId("harvest_save");
    buttons->addView(go);
    buttons->addView(makeButton(trk("sdkscan/btn_details"), &brls::BUTTONSTYLE_BORDERLESS, [this] {
        // По блоку на найденную sdk: версия и начало sha256, ниже — игры, в которых она лежит.
        TextSheet s;
        s.title = trk("sdkscan/btn_details");
        s.headline = trk("sdkscan/details_head");
        for (const auto& l : view_.lines) {
            SheetBlock blk;
            blk.title = trkv("sdkscan/details_line", {l.version, shortSha(l.sha256)});
            for (const auto& g : l.games) blk.lines.push_back(trkv("sdkscan/details_game", {gameWithVersion(g), g.tid}));
            s.blocks.push_back(std::move(blk));
        }
        if (view_.tooNew > 0) s.note = trkv("sdkscan/details_too_new", {std::to_string(view_.tooNew), view_.firmware});
        openTextSheet(std::move(s));
    }));
    buttons->addView(makeButton(tr("common/back"), &brls::BUTTONSTYLE_BORDERED, [this] { finish(); }));
    body->addView(buttons);
    if (!rows_.empty()) brls::Application::giveFocus(rows_.front());
}

void SdkHarvestActivity::showNothing() {
    body->clearViews();
    if (!view_.ok) {
        body->addView(makeLabel(trk("sdkscan/failed"), 28, colBad()));
        auto* e = makeLabel(view_.error.empty() ? tr("error/unknown") : trText(view_.error), 20, colText());
        e->setMarginTop(10);
        body->addView(e);
    } else {
        body->addView(makeLabel(trk("sdkscan/none_title"), 28, colWarn()));
        const std::string text = view_.firmware.empty()
                                     ? trk("sdkscan/no_firmware")
                                     : trkv("sdkscan/none_text", {needText_, mk::plat::paths::kSdkDirOwn});
        auto* t = makeLabel(text, 22, colText());
        t->setMarginTop(12);
        body->addView(t);
        auto* stats = makeLabel(trkv("sdkscan/none_stats", {std::to_string(view_.checked), std::to_string(view_.tooNew),
                                                         std::to_string(view_.failed)}),
                                18, colSub());
        stats->setMarginTop(12);
        body->addView(stats);
        if (view_.already > 0) body->addView(makeLabel(trk("sdkscan/none_already"), 18, colSub()));
        if (view_.cancelled)
            body->addView(makeLabel(trkv("sdkscan/stats_cancelled", {std::to_string(view_.checked), std::to_string(view_.total)}),
                                    18, colWarn()));
    }
    auto* back = makeButton(tr("common/back"), &brls::BUTTONSTYLE_BORDERED, [this] { finish(); });
    back->setId("harvest_back");
    back->setMarginTop(24);
    back->setMarginBottom(30);
    body->addView(back);
    brls::Application::giveFocus(back);
}

void SdkHarvestActivity::save() {
    if (choice_ < 0 || choice_ >= static_cast<int>(view_.lines.size())) return;
    saving_ = busy_ = true;
    const std::string version = view_.lines[choice_].version;
    body->clearViews();
    body->addView(makeLabel(trkv("sdkscan/saving", {version}), 28, colText()));
    body->setFocusable(true);
    body->setHideHighlight(true);
    brls::Application::giveFocus(body);
    AliveToken alive = alive_;
    const size_t index = static_cast<size_t>(choice_);
    brls::async([this, alive, index]() {
        mk::app::HarvestSaveView r = mk::app::Model::instance().saveSdkFromGames(index);
        brls::sync([this, alive, r = std::move(r)]() mutable {
            if (!*alive) return;
            saving_ = busy_ = false;
            saved_ = std::move(r);
            savedOk_ = saved_.ok;
            body->setFocusable(false);
            showSaved();
        });
    });
}

void SdkHarvestActivity::showSaved() {
    body->clearViews();
    const bool ok = saved_.ok;
    body->addView(makeLabel(ok ? trk("sdkscan/saved_title") : trk("sdkscan/save_failed"), 34, ok ? colOk() : colBad()));
    if (ok) {
        const std::string game = saved_.gameVersion.empty() ? saved_.game : saved_.game + " " + saved_.gameVersion;
        auto* t = makeLabel(saved_.already ? trkv("sdkscan/saved_already", {saved_.version, saved_.dir})
                                           : trkv("sdkscan/saved_text", {saved_.version, game, saved_.game}),
                            22, colText());
        t->setMarginTop(12);
        body->addView(t);
        auto* card = makeCard();
        card->setMarginTop(14);
        card->addView(makeInfoLine(trk("sdkscan/saved_file"), saved_.sdkPath, colText()));
        card->addView(makeInfoLine(trk("sdkscan/saved_source"), saved_.dir + "/source.json", colText()));
        body->addView(card);
        auto* next = makeLabel(trkv("sdkscan/saved_next", {gameName_}), 19, colInfo());
        next->setMarginTop(12);
        body->addView(next);
    } else {
        auto* e = makeLabel(saved_.error.empty() ? tr("error/unknown") : trText(saved_.error), 20, colText());
        e->setMarginTop(10);
        body->addView(e);
    }
    auto* done = makeButton(trk("sdkscan/btn_done"), &brls::BUTTONSTYLE_PRIMARY, [this] { finish(); });
    done->setId("harvest_done");
    done->setMarginTop(24);
    done->setMarginBottom(30);
    body->addView(done);
    brls::Application::giveFocus(done);
}

void SdkHarvestActivity::finish() {
    if (busy_) return;
    std::function<void()> cb = savedOk_ ? onDone_ : std::function<void()>{};
    brls::Application::popActivity(brls::TransitionAnimation::FADE, [cb] {
        if (cb) cb();
    });
}

}  // namespace mk::ui
