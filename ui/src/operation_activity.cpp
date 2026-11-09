// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — план → подтверждение → прогресс → итог. См. ui/operation_activity.hpp.
#include "ui/operation_activity.hpp"

#include <algorithm>

#include "app_model.hpp"

namespace mk::ui {

using mk::app::OpKind;

namespace {

const char* opKey(OpKind op) {
    switch (op) {
        case OpKind::Install: return "install";
        case OpKind::Uninstall: return "uninstall";
        case OpKind::SdkSwap: return "sdk_swap";
        case OpKind::SdkUnswap: return "sdk_unswap";
    }
    return "install";
}

bool isRemoval(OpKind op) { return op == OpKind::Uninstall || op == OpKind::SdkUnswap; }

}  // namespace

OperationActivity::OperationActivity(OpKind op, const mk::app::GameDetails& d, std::string modPath,
                                     std::string sdkPath, bool debugBuild, std::function<void(bool ok)> onDone)
    : op_(op),
      d_(d),
      modPath_(std::move(modPath)),
      sdkPath_(std::move(sdkPath)),
      debug_(debugBuild),
      onDone_(std::move(onDone)) {}

OperationActivity::~OperationActivity() { *alive_ = false; }

void OperationActivity::onContentAvailable() {
    if (auto* frame = dynamic_cast<brls::AppletFrame*>(getContentView())) {
        frame->setTitle(trf("op/" + std::string(opKey(op_)) + "/screen_title", d_.row.name));
    }
    // B перехватывается на теле экрана раньше, чем у AppletFrame: во время записи — ничего не делает.
    body->registerAction(tr("hint/back"), brls::ControllerButton::BUTTON_B, [this](brls::View*) {
        if (running_) {
            brls::Application::notify(tr("op/cant_leave"));
            return true;
        }
        finish();
        return true;
    });
    showPlanning();
}

void OperationActivity::showPlanning() {
    body->clearViews();
    body->addView(makeLabel(tr("op/planning"), 24, colSub()));
    AliveToken alive = alive_;
    brls::async([this, alive]() {
        mk::app::PlanView p = mk::app::Model::instance().plan(op_, d_, modPath_, sdkPath_, debug_);
        brls::sync([this, alive, p = std::move(p)]() mutable {
            if (!*alive) return;
            plan_ = std::move(p);
            showPlan();
        });
    });
}

std::string OperationActivity::stepText(const mk::app::PlanItem& it) const { return trText(it.text); }

std::string OperationActivity::errorText() const {
    return plan_.error.empty() ? tr("error/unknown") : trText(plan_.error);
}

void OperationActivity::showPlan() {
    body->clearViews();
    if (!plan_.ok) {
        body->addView(makeLabel(tr("op/plan_failed"), 26, colBad()));
        auto* e = makeLabel(errorText(), 20, colText());
        e->setMarginTop(10);
        body->addView(e);
        auto* back = makeButton(tr("common/back"), &brls::BUTTONSTYLE_BORDERED, [this] { finish(); });
        back->setMarginTop(24);
        body->addView(back);
        brls::Application::giveFocus(back);
        return;
    }

    body->addView(makeLabel(tr(isRemoval(op_) ? "op/will_remove" : "op/will_do"), 28, colText()));
    auto* card = makeCard();
    card->setMarginTop(12);
    int n = 1;
    for (const auto& it : plan_.items) {
        auto* row = makeRow();
        row->setMarginBottom(8);
        row->setAlignItems(brls::AlignItems::FLEX_START);
        auto* num = makeLabel(std::to_string(n++) + ".", 20, colInfo(), true);
        num->setWidth(44);
        row->addView(num);
        auto* t = makeLabel(stepText(it), 20, colText());
        t->setGrow(1.0f);
        row->addView(t);
        card->addView(row);
    }
    if (plan_.items.empty()) card->addView(makeLabel(tr("op/nothing"), 20, colSub()));
    body->addView(card);

    for (const auto& w : plan_.warnings) body->addView(makeLabel("! " + trText(w), 19, colWarn()));
    if (isRemoval(op_)) body->addView(makeLabel(tr("op/removal_note"), 18, colSub()));
    if (debug_) body->addView(makeLabel(tr("debug/plan_note"), 19, colInfo()));

    auto* buttons = makeRow();
    buttons->setMarginTop(22);
    buttons->setMarginBottom(30);
    auto* go = makeButton(tr("op/" + std::string(opKey(op_)) + "/confirm_btn"),
                          isRemoval(op_) ? &brls::BUTTONSTYLE_HIGHLIGHT : &brls::BUTTONSTYLE_PRIMARY,
                          [this] { confirmThenRun(); });
    buttons->addView(go);
    buttons->addView(makeButton(tr("common/back"), &brls::BUTTONSTYLE_BORDERED, [this] { finish(); }));
    body->addView(buttons);
    brls::Application::giveFocus(go);
}

void OperationActivity::confirmThenRun() {
    if (!isRemoval(op_)) {
        run();
        return;
    }
    // Удаление — с отдельным подтверждением (спецификация: показать список и спросить).
    auto* dlg = new brls::Dialog(tr("op/confirm_remove"));
    dlg->addButton(tr("common/cancel"), [] {});
    AliveToken alive = alive_;
    dlg->addButton(tr("op/" + std::string(opKey(op_)) + "/confirm_btn"), [this, alive] {
        if (*alive) run();
    });
    dlg->open();
}

void OperationActivity::run() {
    running_ = true;
    body->clearViews();
    body->addView(makeLabel(tr("op/" + std::string(opKey(op_)) + "/running"), 28, colText()));
    stepLabel_ = makeLabel(tr("op/starting"), 21, colText());
    stepLabel_->setMarginTop(18);
    body->addView(stepLabel_);
    bar_ = new ProgressBar();
    bar_->setMarginTop(16);
    body->addView(bar_);
    countLabel_ = makeLabel("", 18, colSub(), true);
    countLabel_->setMarginTop(8);
    body->addView(countLabel_);
    auto* warn = makeLabel(tr("op/do_not_power_off"), 19, colWarn());
    warn->setMarginTop(24);
    body->addView(warn);
    // Фокус — на тело, чтобы B доходил до нашего обработчика. Рамку выделения не рисуем: тело — не кнопка,
    // иначе весь экран прогресса обведён рамкой «выбрано» от края до края.
    body->setFocusable(true);
    body->setHideHighlight(true);
    brls::Application::giveFocus(body);

    AliveToken alive = alive_;
    brls::async([this, alive]() {
        auto progress = [this, alive](size_t i, size_t total, const mk::app::PlanItem& item) {
            const std::string text = stepText(item);
            brls::sync([this, alive, i, total, text]() {
                if (!*alive || !bar_) return;
                // Метка шага пересоздаётся: длинная строка плана после setText у уже разложенной метки
                // обрезалась в одну строку (предпросмотр WD, шаг подмены sdk); новая метка переносится.
                auto* fresh = makeLabel(text, 21, colText());
                fresh->setMarginTop(18);
                const auto& kids = body->getChildren();
                const auto it = std::find(kids.begin(), kids.end(), stepLabel_);
                if (it != kids.end()) {
                    const size_t pos = static_cast<size_t>(it - kids.begin());
                    body->addView(fresh, pos);
                    body->removeView(stepLabel_);
                    stepLabel_ = fresh;
                } else {
                    stepLabel_->setText(text);
                }
                bar_->setProgress(total ? static_cast<float>(i) / static_cast<float>(total) : 0.f);
                countLabel_->setText(trf("op/step_of", std::to_string(i + 1), std::to_string(total)));
            });
        };
        mk::app::ApplyView r = mk::app::Model::instance().apply(d_, progress);
        brls::sync([this, alive, r = std::move(r)]() mutable {
            if (!*alive) return;
            result_ = std::move(r);
            running_ = false;
            finished_ = true;
            showResult();
        });
    });
}

void OperationActivity::showResult() {
    body->clearViews();
    body->setFocusable(false);
    stepLabel_ = nullptr;
    countLabel_ = nullptr;
    bar_ = nullptr;
    const bool ok = result_.ok;
    body->addView(makeLabel(tr(ok ? "op/result_ok" : "op/result_fail"), 34, ok ? colOk() : colBad()));
    if (!ok) {
        auto* e = makeLabel(result_.error.empty() ? tr("error/unknown") : trText(result_.error), 20, colText());
        e->setMarginTop(10);
        body->addView(e);
    }
    if (!result_.done.empty() || !result_.skipped.empty() || !result_.problems.empty()) {
        auto* card = makeCard();
        card->setMarginTop(14);
        card->addView(makeLabel(tr("op/done_list"), 20, colSub()));
        for (const auto& s : result_.done) card->addView(makeLabel("• " + trText(s), 18, colText()));
        for (const auto& s : result_.skipped) card->addView(makeLabel("— " + trText(s), 18, colSub()));
        for (const auto& s : result_.problems) card->addView(makeLabel("! " + trText(s), 18, colWarn()));
        body->addView(card);
    }
    if (ok && !isRemoval(op_)) body->addView(makeLabel(tr("op/next_launch"), 19, colInfo()));
    if (ok && debug_) body->addView(makeLabel(tr("debug/after_install"), 19, colInfo()));
    if (!result_.receiptPath.empty()) body->addView(makeLabel(trf("op/receipt", result_.receiptPath), 16, colSub()));
    if (!result_.logPath.empty()) body->addView(makeLabel(trf("op/log", result_.logPath), 16, colSub()));

    auto* done = makeButton(tr("op/done_btn"), &brls::BUTTONSTYLE_PRIMARY, [this] { finish(); });
    done->setMarginTop(24);
    done->setMarginBottom(30);
    body->addView(done);
    brls::Application::giveFocus(done);
}

void OperationActivity::finish() {
    if (running_) return;
    std::function<void(bool)> cb = finished_ ? onDone_ : std::function<void(bool)>{};
    const bool ok = finished_ && result_.ok;
    brls::Application::popActivity(brls::TransitionAnimation::FADE, [cb, ok] {
        if (cb) cb(ok);
    });
}

}  // namespace mk::ui
