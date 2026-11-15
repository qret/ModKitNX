// SPDX-License-Identifier: GPL-3.0-or-later
// Based on TorrentShopNX by Langegen (GPL-3.0): source/ui/LibraryView.cpp (RecyclerDataSource списка игр,
// фоновая загрузка brls::async → brls::sync с токеном отмены)
#include "ui/games_activity.hpp"

#include "app_model.hpp"
#include "ui/forwarder_ui.hpp"
#include "ui/game_activity.hpp"
#include "ui/language_activity.hpp"
#include "ui/settings_activity.hpp"
#include "ui/text_sheet.hpp"
#include "ui/update_ui.hpp"

namespace mk::ui {

using mk::app::GameRow;
using mk::plat::SdkNeed;

// ---------------- ячейка ----------------

GameCell::GameCell() {
    this->inflateFromXMLRes("xml/game_cell.xml");
    translateIds(this, {"games/badge_ours", "games/badge_mods"});
}

GameCell* GameCell::create() { return new GameCell(); }

void GameCell::fill(const GameRow& r) {
    name->setText(r.name);
    std::string s = r.version.empty() ? r.tid : trf("games/version_tid", r.version, r.tid);
    sub->setText(s);
    if (!r.icon.empty()) {
        icon->setImageFromMem(r.icon.data(), static_cast<int>(r.icon.size()));
    } else {
        icon->setImageFromRes("img/cover_blank.png");  // из TorrentShopNX, см. resources/CREDITS.md
    }
    badgeOurs->setVisibility(r.ourInstalled ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    badgeMods->setVisibility(r.modsAvailable ? brls::Visibility::VISIBLE : brls::Visibility::GONE);

    switch (r.sdkNeed) {
        case SdkNeed::Needed:
            badgeSdk->setVisibility(brls::Visibility::VISIBLE);
            badgeSdk->setBackgroundColor(colWarn());
            badgeSdkText->setText(tr("games/badge_sdk_needed"));
            break;
        case SdkNeed::SwappedOk:
            badgeSdk->setVisibility(brls::Visibility::VISIBLE);
            badgeSdk->setBackgroundColor(colOk());
            badgeSdkText->setText(tr("games/badge_sdk_swapped"));
            break;
        case SdkNeed::SwappedTooNew:
        case SdkNeed::SwappedUnknown:
            badgeSdk->setVisibility(brls::Visibility::VISIBLE);
            badgeSdk->setBackgroundColor(colBad());
            badgeSdkText->setText(tr("games/badge_sdk_check"));
            break;
        default:
            badgeSdk->setVisibility(brls::Visibility::GONE);
            break;
    }
    sdkRight->setText(r.gameSdk.empty() ? "" : trf("games/sdk_right", r.gameSdk));
}

// ---------------- источник данных ----------------

class GamesDataSource : public brls::RecyclerDataSource {
public:
    explicit GamesDataSource(GamesActivity* a) : a_(a) {}
    int numberOfRows(brls::RecyclerFrame*, int) override { return static_cast<int>(a_->rows_.size()); }
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override {
        auto* cell = dynamic_cast<GameCell*>(recycler->dequeueReusableCell("Game"));
        if (cell && index.row >= 0 && static_cast<size_t>(index.row) < a_->rows_.size()) cell->fill(a_->rows_[index.row]);
        return cell;
    }
    float heightForRow(brls::RecyclerFrame*, brls::IndexPath) override { return 92; }
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index) override {
        if (index.row < 0 || static_cast<size_t>(index.row) >= a_->rows_.size()) return;
        GamesActivity* owner = a_;
        AliveToken alive = owner->alive_;
        brls::Application::pushActivity(new GameActivity(a_->rows_[index.row], [owner, alive](const GameRow& r, bool focus) {
            if (*alive) owner->updateRow(r, focus);
        }));
    }

private:
    GamesActivity* a_;
};

// ---------------- экран ----------------

GamesActivity::GamesActivity(bool askLanguage, bool startupPrompts)
    : askLanguage_(askLanguage), startupPrompts_(startupPrompts) {}

GamesActivity::~GamesActivity() { *alive_ = false; }

void GamesActivity::onContentAvailable() {
    list->registerCell("Game", []() { return GameCell::create(); });
    list->setDataSource(new GamesDataSource(this));
    // Фокус должен всегда стоять на живом виде этого экрана. Пока список грузится, видимых строк нет, и без опоры
    // фокус оставался на строке экрана поверх (выбор языка, диалог). Borealis при его закрытии не переводит фокус,
    // если стек фокуса пуст, — указатель висит на удалённом виде, и следующий giveFocus(list) зовёт onFocusLost
    // у мёртвого объекта: вылет при первом запуске после выбора языка.
    statusBox->setFocusable(true);
    statusBox->setHideHighlight(true);
    statusBox->setHideClickAnimation(true);
    if (askLanguage_) {
        // Первый запуск: выбор языка поверх списка; список строится уже на выбранном языке.
        showStatus("");
        AliveToken alive = alive_;
        brls::sync([this, alive] {
            brls::Application::pushActivity(new LanguageActivity([this, alive] {
                if (!*alive) return;
                askLanguage_ = false;
                applyTexts();
                reload();
                // Первый запуск: после языка — вопрос о ярлыке; обновление при первом запуске не проверяется.
                if (startupPrompts_) forwarder::offerOnce(nullptr);
            }));
        });
        return;
    }
    applyTexts();
    reload();
    // Уже пользуются (язык выбран, ответа о ярлыке нет) — вопрос один раз; иначе тихая проверка обновления.
    if (startupPrompts_) forwarder::offerOnce([] { update::startupCheck(); });
}

void GamesActivity::applyTexts() {
    setFrameTitle(this, "games/title");
    // B на главном экране — выход с вопросом (как у borealis), но тексты — нашим переводом.
    registerAction(tr("hint/back"), brls::ControllerButton::BUTTON_B, [](brls::View*) {
        auto* dlg = makeShortDialog(tr("common/exit_confirm"));
        dlg->addButton(tr("common/cancel"), [] {});
        dlg->addButton(tr("common/ok_short"), [] { brls::Application::quit(); });
        dlg->open();
        return true;
    });
    registerAction(tr("hint/settings"), brls::ControllerButton::BUTTON_X, [](brls::View*) {
        brls::Application::pushActivity(new SettingsActivity());
        return true;
    });
    registerAction(tr("hint/exit"), brls::ControllerButton::BUTTON_START, [](brls::View*) {
        brls::Application::quit();
        return true;
    });
    registerAction(tr("hint/refresh"), brls::ControllerButton::BUTTON_Y, [this](brls::View*) {
        reload(true);   // полный проход мимо кэша
        return true;
    });

    const auto ci = mk::app::Model::instance().console();
    consoleLine->setText(trf("games/console_line", ci.firmware.valid ? ci.firmwareDisplay : tr("common/unknown"),
                             ci.amsKnown ? ci.atmosphere.toString() : tr("common/unknown")));
}

void GamesActivity::showStatus(const std::string& text) {
    statusText->setText(text);
    statusBox->setVisibility(brls::Visibility::VISIBLE);
    list->setVisibility(brls::Visibility::GONE);
    brls::Application::giveFocus(statusBox);
}

void GamesActivity::reload(bool bypassCache) {
    if (loading_) return;
    // Y «Обновить»: фокус остаётся на той же игре (раньше прыгал на первую).
    const uint64_t keep = bypassCache ? focusedId() : 0;
    loading_ = true;
    pendingApply_ = false;
    showStatus(tr("games/loading"));
    countLine->setText("");
    AliveToken alive = alive_;
    brls::async([this, alive, bypassCache, keep]() {
        auto& m = mk::app::Model::instance();
        std::vector<GameRow> rows;
        const bool ok = m.servicesOk();
        if (ok) {
            rows = m.loadGames(bypassCache);
            (void)m.sdkList(true, bypassCache);  // обновить и список sdk — он нужен на экране игры
        }
        brls::sync([this, alive, ok, keep, rows = std::move(rows)]() mutable {
            if (!*alive) return;
            loading_ = false;
            rows_ = std::move(rows);
            if (!ok) {
                showStatus(tr("games/services_failed"));
                return;
            }
            if (rows_.empty()) {
                showStatus(tr("games/empty"));
                return;
            }
            countLine->setText(trf("games/count", std::to_string(rows_.size())));
            statusBox->setVisibility(brls::Visibility::GONE);
            list->setVisibility(brls::Visibility::VISIBLE);
            applyRows(keep);
        });
    });
}

uint64_t GamesActivity::focusedId() const {
    auto* cell = dynamic_cast<brls::RecyclerCell*>(brls::Application::getCurrentFocus());
    if (!cell || cell->getParentActivity() != this) return 0;
    const int i = cell->getIndexPath().row;
    return (i >= 0 && static_cast<size_t>(i) < rows_.size()) ? rows_[i].id : 0;
}

void GamesActivity::applyRows(uint64_t focusId) {
    pendingApply_ = false;
    if (rows_.empty()) return;
    int idx = 0;
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].id == focusId) idx = static_cast<int>(i);
    // reloadData в конце сам выбирает defaultCellFocus (центрирует строку и делает её запомненной), giveFocus(list)
    // уводит фокус со снятой ячейки на эту строку.
    list->setDefaultCellFocus(brls::IndexPath(0, idx));
    list->reloadData();
    brls::Application::giveFocus(list);
}

void GamesActivity::onResume() {
    if (!pendingApply_) return;
    // popActivity зовёт onResume ДО того, как вернёт фокус на запомненный вид (указатель на ячейку, которую
    // reloadData уже отдала другой игре), — поэтому применяем следующим кадром, после него.
    AliveToken alive = alive_;
    brls::sync([this, alive] {
        if (!*alive || !pendingApply_ || loading_) return;
        applyRows(pendingFocus_);
    });
}

void GamesActivity::updateRow(const GameRow& row, bool focus) {
    bool changed = false;
    for (auto& r : rows_) {
        if (r.id != row.id) continue;
        const std::string sdk = row.gameSdk.empty() ? r.gameSdk : row.gameSdk;
        changed = r.ourInstalled != row.ourInstalled || r.sdkNeed != row.sdkNeed ||
                  r.modsAvailable != row.modsAvailable || r.gameSdk != sdk;
        r.ourInstalled = row.ourInstalled;
        r.sdkNeed = row.sdkNeed;
        r.modsAvailable = row.modsAvailable;
        r.gameSdk = sdk;
    }
    if (rows_.empty() || loading_ || (!changed && !focus && !pendingApply_)) return;
    const auto stack = brls::Application::getActivitiesStack();
    if (!stack.empty() && stack.back() == this) {
        // Список уже сверху (ответ пришёл позже, пользователь мог уйти на другую строку) — фокус там, где он есть;
        // после операции — на эту игру.
        const uint64_t here = focusedId();
        applyRows(focus || here == 0 ? row.id : here);
        return;
    }
    if (!changed && !focus) return;   // уже ждёт применения — с прежней игрой фокуса
    pendingApply_ = true;
    pendingFocus_ = row.id;
}

}  // namespace mk::ui
