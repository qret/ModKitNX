// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — ярлык на домашнем экране (Р-43): вопрос при запуске и карточка в «Настройках».
// Работа — Model::createForwarder / removeForwarder (platform/forwarder); строки — app.json, раздел forwarder.
#pragma once

#include <borealis.hpp>

#include <functional>

namespace mk::ui::forwarder {

// Один раз (config.json "forwarder_prompt" нет): «Создать ярлык на домашнем экране?». Ярлык уже есть или приложение
// запущено с него — вопрос не задаётся, записывается "exists". Ответ пишется в config.json и после обновлений не
// спрашивается. Вопроса не было — зовётся otherwise (UI-поток), например тихая проверка обновления.
void offerOnce(std::function<void()> otherwise);

// Карточка «Ярлык на домашнем экране» (id forwarder_card): состояние, «Создать ярлык» или «Удалить ярлык».
brls::Box* makeSettingsCard();

}  // namespace mk::ui::forwarder
