// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — один экран на всю операцию, чтобы не прыгать между экранами:
//   план «Будет сделано» / «Будет удалено» → кнопка → (для удаления — подтверждение) → прогресс → итог.
// Во время записи кнопка B не работает (нельзя уйти с половиной файлов). «Готово» возвращает на экран игры,
// и он перечитывает состояние.
#pragma once

#include <borealis.hpp>

#include <functional>

#include "app_types.hpp"
#include "ui/common.hpp"

namespace mk::ui {

class OperationActivity : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("operation_activity.xml");
    OperationActivity(mk::app::OpKind op, const mk::app::GameDetails& d, std::string modPath, std::string sdkPath,
                      bool debugBuild, std::function<void(bool ok)> onDone);
    ~OperationActivity() override;
    void onContentAvailable() override;

private:
    void showPlanning();
    void showPlan();
    void confirmThenRun();
    void run();
    void showResult();
    void finish();
    std::string stepText(const mk::app::PlanItem& it) const;
    std::string errorText() const;

    BRLS_BIND(brls::Box, body, "body");

    mk::app::OpKind op_;
    mk::app::GameDetails d_;
    std::string modPath_, sdkPath_;
    bool debug_ = false;
    std::function<void(bool ok)> onDone_;   // вызывается после закрытия экрана, если операция выполнялась
    mk::app::PlanView plan_;
    mk::app::ApplyView result_;
    bool running_ = false;
    bool finished_ = false;
    brls::Label* stepLabel_ = nullptr;
    brls::Label* countLabel_ = nullptr;
    ProgressBar* bar_ = nullptr;
    AliveToken alive_ = makeAlive();
};

}  // namespace mk::ui
