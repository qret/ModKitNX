// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — «Собрать логи для отправки»: отчёты отладки игры, последний отчёт Atmosphère о падении, паспорт
// консоли и квитанция -> папка sdmc:/switch/<APP>/logs/<Игра> v<версия>/ и .zip рядом (mk::logs, без ключей и
// файлов игры). Итог: путь к архиву и подсказка «отправьте этот файл (например, в issue на GitHub)».
#pragma once

#include <borealis.hpp>

#include "app_types.hpp"
#include "ui/common.hpp"

namespace mk::ui {

class LogsActivity : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("diag_activity.xml");   // та же разметка: прокрутка + body
    explicit LogsActivity(mk::app::GameDetails d);
    ~LogsActivity() override;
    void onContentAvailable() override;

private:
    void show(const mk::app::LogsView& v);
    void addBack();   // кнопка «Назад» в конце экрана, в фокусе

    BRLS_BIND(brls::Box, body, "body");
    mk::app::GameDetails d_;
    AliveToken alive_ = makeAlive();
};

}  // namespace mk::ui
