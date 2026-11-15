// SPDX-License-Identifier: GPL-3.0-or-later
// Based on sphaira by ITotalJustice (GPL-3.0): sphaira/source/owo.cpp (install_forwader_internal), sphaira/source/yati/nx/ns.cpp
//
// ModKitNX — ярлык на домашнем экране (форвардер, Р-43): установка на SD через ncm, запись приложения в ns,
// удаление штатно (nsDeleteApplicationCompletely). Сборку байтов делает ядро (core/include/mk/forwarder.hpp);
// загрузчик hbl (main + main.npdm) лежит в romfs:/forwarder/ — собирается из hbl/ при сборке .nro.
// TitleID — формула sphaira от paths::kAppNroForwarder: тот же, что у ярлыка ModKitNX, созданного в sphaira.
// Трогается только этот TitleID. Блокирующие (IPC, запись на карту) — звать из brls::async.
#pragma once

#include <cstdint>
#include <string>

namespace mk::plat::forwarder {

struct Status {
    uint64_t tid = 0;
    bool installed = false;      // есть запись приложения с этим TitleID (наш ярлык или ярлык sphaira на тот же .nro)
    bool runningFromIt = false;  // приложение запущено с этого ярлыка
    bool nroPresent = false;     // sdmc:/switch/<APP>/<APP>.nro на месте
};

// Итог операции: code — машинный код для текста ("running", "no_nro", "no_loader", "nro_nacp", "key", "build",
// "write", "database", "record", "remove"); rc — код системы (0 — нет).
struct Outcome {
    bool ok = false;
    std::string code;
    uint32_t rc = 0;
};

uint64_t titleId();
Status status();
// Собрать и установить (прежний ярлык с этим TitleID снимается перед записью — не дублируется).
Outcome install();
// Удалить ярлык (запись и содержимое) — только этот TitleID.
Outcome remove();

}  // namespace mk::plat::forwarder
