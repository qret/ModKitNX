// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — ярлык на домашнем экране: вопрос при запуске и карточка настроек. См. ui/forwarder_ui.hpp.
#include "ui/forwarder_ui.hpp"

#include <atomic>
#include <memory>
#include <string>

#include "app_model.hpp"
#include "platform/app_config.hpp"
#include "platform/log.hpp"
#include "platform/paths.hpp"
#include "ui/common.hpp"
#include "ui/text_sheet.hpp"

namespace mk::ui::forwarder {

namespace cfg = mk::plat::config;
using mk::app::ForwarderResult;
using mk::app::ForwarderView;

namespace {

std::atomic<bool> g_busy{false};

void note(const std::string& text) {
    auto* d = makeShortDialog(text);
    d->addButton(tr("common/ok_short"), [] {});
    d->open();
}

std::string errorText(const ForwarderResult& r) {
    static const char* const known[] = {"running", "no_nro", "no_loader", "key",   "build",
                                        "write",   "database", "record",  "remove"};
    std::string code = "unknown";
    for (const char* k : known)
        if (r.code == k) code = k;
    const std::string rc = r.rc.empty() ? "—" : r.rc;
    if (code == "no_nro") return trf("forwarder/err_no_nro", mk::plat::paths::kAppNro);
    return trf("forwarder/err_" + code, rc);
}

// Создать в фоне; done(ok) — в UI-потоке после окна итога.
void createAsync(std::function<void(bool)> done) {
    if (g_busy.exchange(true)) return;
    brls::Application::notify(tr("forwarder/creating"));
    brls::async([done] {
        const ForwarderResult r = mk::app::Model::instance().createForwarder();
        brls::sync([r, done] {
            g_busy = false;
            note(r.ok ? tr("forwarder/created") : errorText(r));
            if (done) done(r.ok);
        });
    });
}

void removeAsync(std::function<void(bool)> done) {
    if (g_busy.exchange(true)) return;
    brls::async([done] {
        const ForwarderResult r = mk::app::Model::instance().removeForwarder();
        brls::sync([r, done] {
            g_busy = false;
            note(r.ok ? tr("forwarder/removed") : errorText(r));
            if (done) done(r.ok);
        });
    });
}

// Карточка: строка состояния и две кнопки (видна одна). Вид живёт, пока жив экран настроек; ответы фоновых
// операций проверяют alive_.
class ForwarderCard : public brls::Box {
public:
    ForwarderCard() {
        // Как makeCard (ui/common.cpp): колонка-панель со скруглением и отступами.
        setAxis(brls::Axis::COLUMN);
        setBackgroundColor(colPanel());
        setCornerRadius(10);
        setPadding(18, 22, 18, 22);
        setMarginBottom(18);
        setId("forwarder_card");
        addView(makeSectionTitle(tr("forwarder/title")));
        addView(makeLabel(tr("forwarder/note"), 16, colSub()));
        state_ = makeLabel("", 19, colText());
        state_->setMarginTop(10);
        addView(state_);
        create_ = makeButton(tr("forwarder/create_btn"), &brls::BUTTONSTYLE_BORDERED, [this] { askCreate(); });
        create_->setId("forwarder_create_btn");
        create_->setMarginTop(12);
        addView(create_);
        remove_ = makeButton(tr("forwarder/remove_btn"), dangerStyle(), [this] { askRemove(); });
        remove_->setId("forwarder_remove_btn");
        remove_->setMarginTop(12);
        addView(remove_);
        apply(mk::app::Model::instance().forwarderStatus());
    }
    ~ForwarderCard() override { *alive_ = false; }

private:
    void apply(const ForwarderView& v) {
        if (v.runningFromIt) state_->setText(tr("forwarder/state_running"));
        else state_->setText(v.installed ? tr("forwarder/state_yes") : tr("forwarder/state_no"));
        state_->setTextColor(v.installed ? colOk() : colSub());
        // Сначала фокус на видимую кнопку, потом прятать прежнюю (фокус на скрытом виде висит).
        brls::Button* show = v.installed ? remove_ : create_;
        brls::Button* hide = v.installed ? create_ : remove_;
        show->setVisibility(brls::Visibility::VISIBLE);
        if (brls::Application::getCurrentFocus() == hide) brls::Application::giveFocus(show);
        hide->setVisibility(brls::Visibility::GONE);
    }
    void refresh() {
        AliveToken alive = alive_;
        brls::async([this, alive] {
            const ForwarderView v = mk::app::Model::instance().forwarderStatus();
            brls::sync([this, alive, v] {
                if (*alive) apply(v);
            });
        });
    }
    void askCreate() {
        auto* d = makeShortDialog(tr("forwarder/confirm_create"));
        d->addButton(tr("common/cancel"), [] {});
        AliveToken alive = alive_;
        d->addButton(tr("forwarder/create_short"), [this, alive] {
            createAsync([this, alive](bool ok) {
                if (ok) cfg::setString(cfg::kForwarderPrompt, "created");
                if (*alive) refresh();
            });
        });
        d->open();
    }
    void askRemove() {
        auto* d = makeShortDialog(tr("forwarder/confirm_remove"));
        d->addButton(tr("common/cancel"), [] {});
        AliveToken alive = alive_;
        d->addButton(tr("forwarder/remove_short"), [this, alive] {
            removeAsync([this, alive](bool) {
                if (*alive) refresh();
            });
        });
        d->open();
    }

    brls::Label* state_ = nullptr;
    brls::Button* create_ = nullptr;
    brls::Button* remove_ = nullptr;
    AliveToken alive_ = makeAlive();
};

}  // namespace

void offerOnce(std::function<void()> otherwise) {
    if (!cfg::getString(cfg::kForwarderPrompt).empty()) {
        if (otherwise) otherwise();
        return;
    }
    brls::async([otherwise] {
        const ForwarderView v = mk::app::Model::instance().forwarderStatus();
        brls::sync([v, otherwise] {
            if (!v.known) {   // службы не поднялись — спросим в другой раз
                if (otherwise) otherwise();
                return;
            }
            if (v.installed || v.runningFromIt) {
                cfg::setString(cfg::kForwarderPrompt, "exists");
                MK_LOG("forwarder: shortcut already there, not asking");
                if (otherwise) otherwise();
                return;
            }
            auto* d = makeShortDialog(tr("forwarder/offer"));
            d->setCancelable(false);
            d->addButton(tr("forwarder/offer_later"), [] {
                if (!cfg::setString(cfg::kForwarderPrompt, "declined")) brls::Application::notify(tr("settings/save_failed"));
            });
            d->addButton(tr("forwarder/create_short"), [] {
                cfg::setString(cfg::kForwarderPrompt, "created");
                createAsync(nullptr);
            });
            if (brls::View* b = d->getView("brls/dialog/button2")) d->setLastFocusedView(b);
            d->open();
        });
    });
}

brls::Box* makeSettingsCard() { return new ForwarderCard(); }

}  // namespace mk::ui::forwarder
