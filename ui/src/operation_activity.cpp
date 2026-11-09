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

// Тело экрана — невидимая опора фокуса, пока на экране нечего выбирать (план составляется, идёт запись):
// иначе фокус падает на рамку прокрутки — пустая рамка «кнопки без текста», по которой ходит курсор. B ловит
// обработчик тела. Перед очисткой фокус переводится на тело — удаляемая кнопка не остаётся в фокусе.
void OperationActivity::holdFocusOnBody() {
    body->setFocusable(true);
    body->setHideHighlight(true);
    body->setHideClickAnimation(true);
    brls::Application::giveFocus(body);
}

void OperationActivity::showPlanning() {
    holdFocusOnBody();
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
    holdFocusOnBody();
    body->clearViews();
    body->setFocusable(false);
    if (!plan_.ok) {
        auto* fail = makeSubCard(Tone::Bad, icon::kBad, tr("op/plan_failed"), 24);
        fail->setMarginTop(0);
        addSubLine(fail, errorText(), colText(), 20);
        body->addView(fail);
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

    // Предупреждения, пояснение к удалению и отладочная сборка — подкарточками по тону.
    if (!plan_.warnings.empty()) {
        auto* w = makeSubCard(Tone::Warn, icon::kWarn, tr("common/attention"));
        for (const auto& it : plan_.warnings) addSubLine(w, trText(it), colText(), 19);
        body->addView(w);
    }
    if (isRemoval(op_)) body->addView(makeSubCard(Tone::Neutral, icon::kInfo, tr("op/removal_note"), 18));
    if (debug_) body->addView(makeSubCard(Tone::Info, icon::kInfo, tr("debug/plan_note"), 19));

    auto* buttons = makeRow();
    buttons->setMarginTop(22);
    buttons->setMarginBottom(30);
    auto* go = makeButton(tr("op/" + std::string(opKey(op_)) + "/confirm_btn"),
                          isRemoval(op_) ? dangerStyle() : &brls::BUTTONSTYLE_PRIMARY,
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
    holdFocusOnBody();
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
    holdFocusOnBody();
    body->clearViews();
    body->setFocusable(false);
    stepLabel_ = nullptr;
    countLabel_ = nullptr;
    bar_ = nullptr;
    const bool ok = result_.ok;
    body->addView(makeLabel(tr(ok ? "op/result_ok" : "op/result_fail"), 34, ok ? colOk() : colBad()));
    // Итог по смыслу: ошибка — красная подкарточка, сделанное — зелёная (пропущенное — серым в ней же),
    // проблемы — жёлтая, что дальше — синяя; пути квитанции и журнала — мелко внизу.
    if (!ok) body->addView(makeSubCard(Tone::Bad, icon::kBad,
                                       result_.error.empty() ? tr("error/unknown") : trText(result_.error), 20));
    if (!result_.done.empty() || !result_.skipped.empty()) {
        auto* done = makeSubCard(result_.done.empty() ? Tone::Neutral : Tone::Ok,
                                 result_.done.empty() ? icon::kInfo : icon::kOk, tr("op/done_list"), 20);
        for (const auto& s : result_.done) addSubLine(done, "• " + trText(s), colText(), 18);
        for (const auto& s : result_.skipped) addSubLine(done, "— " + trText(s), colSub(), 18);
        body->addView(done);
    }
    if (!result_.problems.empty()) {
        auto* pr = makeSubCard(Tone::Warn, icon::kWarn, tr("op/problems_title"), 20);
        for (const auto& s : result_.problems) addSubLine(pr, trText(s), colText(), 18);
        body->addView(pr);
    }
    if (ok && (!isRemoval(op_) || debug_)) {
        brls::Box* next = nullptr;
        if (!isRemoval(op_)) next = makeSubCard(Tone::Info, icon::kInfo, tr("op/next_launch"), 19);
        if (debug_) {
            if (next) addSubLine(next, tr("debug/after_install"), colText(), 18);
            else next = makeSubCard(Tone::Info, icon::kInfo, tr("debug/after_install"), 19);
        }
        body->addView(next);
    }
    auto addPath = [this](const std::string& text) {
        auto* l = makeLabel(text, 16, colSub());
        l->setMarginTop(6);
        body->addView(l);
    };
    if (!result_.receiptPath.empty()) addPath(trf("op/receipt", result_.receiptPath));
    if (!result_.logPath.empty()) addPath(trf("op/log", result_.logPath));

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
