// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — путь 3 чтения exefs: сырой program NCA через службы консоли, без prod.keys.
//
// Зачем: на HOS 19.0.1 fsp-srv отказывает в Code FS (проверено на консоли: 0xB06EE202 и 0xB0AEE202 —
// модуль 2 (fs), описание 6001, InvalidArgument; Code открывает только загрузчик через fsp-ldr OpenCodeFileSystem).
// Здесь — тонкий слой платформы; разбор NCA/PFS0/счётчика CTR — ядро (core/include/mk/nca.hpp).
//
// Шаги (каждый — строка журнала с кодом, см. exefsJournalFor):
//   header  — заголовок NCA: ключ заголовка выводит spl (как в content.cpp), XTS — libnx;
//   parse   — разбор заголовка ядром, секция 0 = exefs (PFS0, AES-CTR);
//   без rights id: key-area — spl GenerateAesKek(источник KAEK, поколение NCA) + GenerateAesKey(запись key area [2])
//                  → ключ секции в памяти процесса, CTR — libnx (aes128Ctr);
//   с rights id:   ticket  — es GetCommonTicketSize/Data (общий билет; персональный — отказ «не поддержано»);
//                  titlekey — spl:es AllocateAesKeySlot + PrepareCommonEsTitleKey + LoadPreparedAesKey,
//                  CTR — spl:es ComputeCtr в аппаратном слоте (открытого титульного ключа процесс не видит);
//   pfs0    — первые 0x10 байт секции = «PFS0» (проверка ключа), разбор списка файлов;
//   mount   — устройство devoptab «mkexefs:» только для чтения (open/read/seek/stat/dir).
// Источники: nxdumptool rewrite (nca.c, tik.c — формат; там ключи из prod.keys), Atmosphère (spl: команды и
// раскладка аргументов, secmon: поколение = значение − 1), hactool (счётчик CTR). Код свой.
// Ключи и титульные ключи в журнал, на экран и в файлы не попадают никогда.
#pragma once

#include <cstdint>
#include <string>

#include "platform/content.hpp"

namespace mk::plat {

struct RawExefsOutcome {
    bool ok = false;
    std::string step;        // упавший шаг: header | parse | key-area | ticket | titlekey | pfs0 | mount
    uint32_t rc = 0;         // код службы (0 — отказ ядра, см. coreCode)
    std::string coreCode;    // код ошибки ядра (NCA_PFS0_MAGIC …) или ""
    bool rightsId = false;   // у NCA есть rights id (титульный ключ из билета)
    bool personalized = false;   // общего билета нет (вероятно, персональный) — не поддержано
    uint8_t keyGeneration = 0;
};

// Монтирует exefs из сырого NCA устройством deviceName (без «:»). Журнал — через jlog.
using JournalFn = void (*)(const char* line);
bool rawExefsMount(const ProgramNca& nca, const char* deviceName, RawExefsOutcome& out, JournalFn jlog);
void rawExefsUnmount();

}  // namespace mk::plat
