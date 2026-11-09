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
    void addBack();
    void addPicker(brls::Box* parent, const char* titleKey, const std::vector<mk::logs::ReportFile>& files, int& sel);

    BRLS_BIND(brls::Box, body, "body");
    std::string tid_;
    int selDiag_ = 0, selGfx_ = 0;   // 0 — последний по номеру
    brls::View* pickFocus_ = nullptr;   // строка выбранного отчёта — на неё фокус после перестройки
    AliveToken alive_ = makeAlive();
};

// Что есть на карте для этой игры — для кнопок страницы игры. Дёшево: списки файлов; старый отчёт из корня карты
// читается (он маленький) и засчитывается, только если он от этой игры. Звать в фоне (brls::async).
struct ReportsInfo {
    bool diag = false;        // отчёт отладки модуля (diag-*/gfx-* в папке игры или старый корневой этой игры)
    std::string lastWhen;     // время последнего отчёта «ГГГГ-ММ-ДД ЧЧ:ММ»; "" — не известно
    bool crash = false;       // отчёт Atmosphère о падении этой игры (по имени файла, как mk::logs)
};
ReportsInfo findReports(const std::string& tid);

}  // namespace mk::ui
