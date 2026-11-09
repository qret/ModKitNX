// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — «Отчёт отладки»: после запуска игры с отладочной сборкой читает отчёты модуля этой игры
// sdmc:/switch/<APP>/logs/<TID>/diag-NNNN.txt и gfx-NNNN.txt (последний по номеру; список последних 5 — выбрать),
// запасной путь — старые sdmc:/nxredirect-diag.txt и nxredirect-gfx.txt. Сверху — короткий вывод простыми словами
// (подмена работает / нет и почему), ниже — этапы со светофором, ещё ниже — сам текст отчёта. Разбор —
// platform/diag_report (разбор отчёта модуля), список и первая строка — mk::logs.
#pragma once

#include <borealis.hpp>

#include <string>
#include <vector>

#include <mk/logs.hpp>

#include "ui/common.hpp"

namespace mk::ui {

class DiagActivity : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("diag_activity.xml");
    explicit DiagActivity(std::string gameTid);
    ~DiagActivity() override;
    void onContentAvailable() override;

private:
    void load();
    void addPicker(brls::Box* parent, const char* titleKey, const std::vector<mk::logs::ReportFile>& files, int& sel);

    BRLS_BIND(brls::Box, body, "body");
    std::string tid_;
    int selDiag_ = 0, selGfx_ = 0;   // 0 — последний по номеру
    AliveToken alive_ = makeAlive();
};

// Есть ли на карте отчёт отладки этой игры (или старый в корне карты) — для показа кнопки.
bool diagReportsPresent(const std::string& tid);

}  // namespace mk::ui
