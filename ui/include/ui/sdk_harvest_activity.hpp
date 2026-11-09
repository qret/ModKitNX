// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — «Найти sdk в других играх»: один экран на всё —
//   поиск («Проверено N из M игр», B — остановить) → выбор прямо под вопросом (рекомендованная для прошивки первой,
//   затем ближайшие; дубли одной строкой; куда сохранится) → «Сохранить» → итог («Готово» — назад, экран игры
//   перечитывает список sdk). Ничего не скачивается: читается только файл sdk установленных игр.
// Строки — resources/i18n/<язык>/sdkscan.json (ключи «sdkscan/…», ui::trk).
#pragma once

#include <borealis.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "harvest_types.hpp"
#include "ui/common.hpp"

namespace mk::ui {

class SdkHarvestActivity : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("operation_activity.xml");   // та же разметка: прокрутка и тело «body»
    // needText — какая sdk нужна («19.x»), для честного «ничего не нашлось».
    SdkHarvestActivity(std::string gameName, std::string needText, std::function<void()> onDone);
    ~SdkHarvestActivity() override;
    void onContentAvailable() override;

private:
    void startScan();
    void showChoice();
    void showNothing();
    void save();
    void showSaved();
    void finish();
    std::string lineTitle(const mk::app::HarvestLine& l) const;
    std::string lineSub(const mk::app::HarvestLine& l, size_t index) const;

    BRLS_BIND(brls::Box, body, "body");

    std::string gameName_, needText_;
    std::function<void()> onDone_;
    mk::app::HarvestView view_;
    mk::app::HarvestSaveView saved_;
    int choice_ = 0;
    bool busy_ = false;        // идёт поиск или запись
    bool saving_ = false;
    bool savedOk_ = false;
    std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
    std::vector<ChoiceRow*> rows_;
    brls::Label* countLabel_ = nullptr;
    brls::Label* gameLabel_ = nullptr;
    brls::Label* targetLabel_ = nullptr;
    ProgressBar* bar_ = nullptr;
    AliveToken alive_ = makeAlive();
};

}  // namespace mk::ui
