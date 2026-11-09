// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — окна обновления приложения. См. ui/update_ui.hpp.
#include "ui/update_ui.hpp"

#include <borealis.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "platform/app_config.hpp"
#include "platform/log.hpp"
#include "platform/paths.hpp"
#include "ui/common.hpp"

namespace mk::ui::update {

namespace up = mk::plat::update;
namespace cfg = mk::plat::config;

namespace {

std::atomic<bool> g_checking{false};
std::atomic<bool> g_downloading{false};

void note(const std::string& text) {
    auto* d = new brls::Dialog(text);
    d->addButton(tr("common/ok_short"), [] {});
    d->open();
}

std::string releasesUrl() { return std::string("github.com/") + up::kRepo + "/releases"; }

// Колонка окна: заголовок, подзаголовок, «что нового».
brls::Box* offerContent(const up::Release& r) {
    auto* box = makeColumn();
    box->setPadding(30, 40, 24, 40);
    box->addView(makeLabel(trkf("update/available_title", r.version), 28, colText()));
    auto* cur = makeLabel(trkf("update/current", MK_APP_VERSION), 18, colSub());
    cur->setMarginTop(6);
    box->addView(cur);
    // «Что нового» — нейтральной подкарточкой; ставить некуда — жёлтой.
    auto* news = makeSubCard(Tone::Neutral, icon::kInfo, trk("update/whats_new"), 20);
    news->setMarginTop(16);
    const std::string notes = up::notesForDisplay(r.body, 520, 9);
    addSubLine(news, notes.empty() ? trk("update/no_notes") : notes, colText(), 17);
    box->addView(news);
    if (up::selfPath().empty()) {
        box->addView(makeSubCard(Tone::Warn, icon::kWarn, trkf("update/no_self_path", up::assetName(), releasesUrl()), 17));
    }
    return box;
}

std::string reasonText(const std::string& code) {
    if (code == "net") return trk("update/reason_net");
    if (code == "write") return trk("update/reason_write");
    return trk("update/reason_verify");
}

void failed(brls::Dialog* prog, const std::string& code) {
    prog->close([code] { note(trkf("update/failed", reasonText(code))); });
}

void askRestart(const std::string& version) {
    auto* d = new brls::Dialog(trkf("update/done_title", version));
    d->setCancelable(false);
    d->addButton(trk("update/btn_restart_later"), [] {});
    d->addButton(trk("update/btn_restart"), [] {
        if (up::restartNow()) brls::Application::quit();
        else note(trk("update/restart_manual"));
    });
    if (brls::View* b = d->getView("brls/dialog/button2")) d->setLastFocusedView(b);
    d->open();
}

void startDownload(const up::Release& r) {
    if (g_downloading.exchange(true)) return;
    // Окно загрузки без кнопок и без B: колбэки пишут в его подпись и полосу до конца загрузки.
    auto* box = makeColumn();
    box->setPadding(34, 44, 34, 44);
    box->setAlignItems(brls::AlignItems::CENTER);
    box->setFocusable(true);
    box->setHideHighlight(true);
    auto* label = makeLabel(trkf("update/downloading", r.version), 22, colText());
    label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    box->addView(label);
    auto* bar = new ProgressBar();
    bar->setWidth(560);
    bar->setShrink(0.0f);
    bar->setMarginTop(22);
    box->addView(bar);
    auto* prog = new brls::Dialog(box);
    prog->setCancelable(false);
    prog->open();

    brls::async([r, prog, label, bar] {
        auto last = std::make_shared<int>(-1);
        std::vector<uint8_t> data;
        std::string err;
        const bool got = up::download(
            r, data,
            [r, last, label, bar](uint64_t now, uint64_t total) {
                if (total == 0) total = r.assetSize;
                const int pct = total ? static_cast<int>(now * 100 / total) : -1;
                if (pct != *last) {
                    *last = pct;
                    const std::string ver = r.version;
                    brls::sync([label, bar, pct, ver] {
                        if (pct < 0) return;
                        label->setText(trkf("update/downloading_pct", ver, std::to_string(pct)));
                        bar->setProgress(pct / 100.0f);
                    });
                }
                return !up::aborting();
            },
            err);
        if (!got) {
            MK_LOG("update: download failed: %s", err.c_str());
            g_downloading = false;
            brls::sync([prog] { failed(prog, "net"); });
            return;
        }
        brls::sync([label, bar] {
            label->setText(trk("update/verifying"));
            bar->setProgress(1.0f);
        });
        const up::NroInfo info = up::verifyDownloaded(data, r);
        if (!info.ok) {
            MK_LOG("update: downloaded file rejected: %s (size %zu, nacp '%s' %s)", info.error.c_str(), data.size(),
                   info.name.c_str(), info.version.c_str());
            g_downloading = false;
            brls::sync([prog] { failed(prog, "verify"); });
            return;
        }
        if (!up::stagePending(up::selfPath(), data, err)) {
            MK_LOG("update: cannot stage %s: %s", up::pendingPath(up::selfPath()).c_str(), err.c_str());
            g_downloading = false;
            brls::sync([prog] { failed(prog, "write"); });
            return;
        }
        MK_LOG("update: %s %s staged as %s (%zu bytes, sha256 %s)", info.name.c_str(), info.version.c_str(),
               up::pendingPath(up::selfPath()).c_str(), data.size(), r.sha256.empty() ? "not given" : "matched");
        g_downloading = false;
        const std::string ver = r.version;
        brls::sync([prog, ver] { prog->close([ver] { askRestart(ver); }); });
    });
}

// Фоновая проверка. manual — ответ всегда; иначе только «есть новая версия».
void check(bool manual) {
    if (g_checking.exchange(true)) return;
    brls::async([manual] {
        up::Release r;
        std::string err;
        bool ok = false;
        if (!up::networkUp()) err = "no internet connection";
        else ok = up::fetchLatest(r, err);
        g_checking = false;
        if (!ok) {
            MK_LOG("update: check failed: %s", err.c_str());
            if (manual) brls::sync([] { note(trk("update/check_failed")); });
            return;
        }
        const std::string skipped = cfg::getString(cfg::kUpdateSkip);
        const up::Decision d = up::decide(r, MK_APP_VERSION, manual ? std::string() : skipped);
        MK_LOG("update: latest %s (running %s): newer=%d installable=%d mismatch=%d skipped=%d asset=%s sha256=%s%s",
               r.version.c_str(), MK_APP_VERSION, d.newer, d.installable, d.fileMismatch, d.skipped,
               r.assetUrl.empty() ? "(none)" : r.assetUrl.c_str(), r.sha256.empty() ? "(none)" : "given",
               r.shaMismatch ? " (digest != release notes)" : "");
        if (d.installable && !d.skipped) {
            brls::sync([r] { showOffer(r); });
        } else if (manual) {
            const std::string v = r.version;
            if (d.fileMismatch)
                brls::sync([v] { note(trkf("update/file_mismatch", v, up::assetName(), releasesUrl())); });
            else if (d.newer) brls::sync([v] { note(trkf("update/no_asset", v, up::assetName(), releasesUrl())); });
            else brls::sync([] { note(trkf("update/latest", MK_APP_VERSION)); });
        }
    });
}

}  // namespace

void startupCheck() {
    if (!cfg::getBool(cfg::kUpdateCheck, true)) {
        MK_LOG("update: startup check is off in settings");
        return;
    }
    if (up::selfPath().empty()) return;   // ставить некуда — не беспокоить (журнал — в up::init)
    if (up::hasPending(up::selfPath(), MK_APP_VERSION)) {
        MK_LOG("update: a downloaded version is waiting to be installed at exit");
        return;
    }
    check(false);
}

void checkNow() {
    if (up::hasPending(up::selfPath(), MK_APP_VERSION)) {
        note(trk("update/pending"));
        return;
    }
    brls::Application::notify(trk("update/checking"));
    check(true);
}

void showOffer(const up::Release& r) {
    auto* d = new brls::Dialog(offerContent(r));
    if (up::selfPath().empty()) {
        d->addButton(tr("common/ok_short"), [] {});
        d->open();
        return;
    }
    d->addButton(trk("update/btn_later"), [] {});
    const std::string ver = r.version;
    d->addButton(trk("update/btn_skip"), [ver] {
        cfg::setString(cfg::kUpdateSkip, ver);
        MK_LOG("update: version %s skipped by the user", ver.c_str());
    });
    d->addButton(trk("update/btn_update"), [r] { startDownload(r); });
    if (brls::View* b = d->getView("brls/dialog/button3")) d->setLastFocusedView(b);
    d->open();
}

}  // namespace mk::ui::update
