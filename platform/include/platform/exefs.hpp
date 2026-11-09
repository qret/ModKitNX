// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — открытие exefs установленной игры, с запасными путями.
// Идея чтения файлов NCA через fsOpenFileSystemWithId — TorrentShopNX (source/installer/ncm_installer.cpp,
// readCnmtFromContentMetaFs, тип ContentMeta); здесь тип Code и свой код.
//
// Порядок:
//   1. fsOpenFileSystemWithId(program id, тип Code = 0 приведением, путь NCA);
//   2. fsOpenFileSystemWithPatch(program id, Code);
//      На HOS 19.0.1 оба отказывают кодом 2002-6001 (InvalidArgument, проверено на консоли): fsp-srv не открывает
//      Code — это делает только загрузчик через fsp-ldr. Шаги 1–2 оставлены: дёшевы и пишут код в журнал.
//   3. сырой NCA через ncm + spl + es, без prod.keys (platform/exefs_raw.hpp) — основной рабочий путь.
// Открытая ФС — устройство «mkexefs:» (fsdev для шагов 1–2, свой devoptab для шага 3), читать обычными путями
// mkexefs:/main и т. п. Журнал каждого шага — exefsJournalFor (в «Собрать логи» как exefs-read.txt).
#pragma once

#include <cstdint>
#include <string>

#include "platform/content.hpp"

namespace mk::plat {

enum class ExefsVia { None, CodeWithId, CodeWithPatch, RawNcaWithKeys };

struct ExefsReport {
    ExefsVia via = ExefsVia::None;
    uint32_t rcWithId = 0;      // код отказа шага 1 (0 — не пробовали или успех)
    uint32_t rcWithPatch = 0;   // код отказа шага 2
    bool keysPresent = false;   // есть ли sdmc:/switch/prod.keys (только наличие; путь 3 их не читает)
    bool needKeys = false;      // больше не ставится: путь 3 работает без prod.keys (экран «Нет ключей» не нужен)
    bool rawNotImplemented = false;  // больше не ставится (путь 3 реализован)
    bool hasMainNpdm = false;   // после монтирования виден main.npdm
    // шаг 3 (сырой NCA): что упало
    bool rawTried = false;
    std::string rawStep;        // header | parse | key-area | ticket | titlekey | pfs0 | mount (пусто — успех)
    uint32_t rcRaw = 0;         // код службы на упавшем шаге (0 — отказ разбора, см. rawCore)
    std::string rawCore;        // код ядра (NCA_PFS0_MAGIC, TICKET_PERSONALIZED …)
    bool rawRightsId = false;   // титульный ключ из билета
    bool rawPersonalized = false;   // билет персональный (или общего нет) — не поддержано
};

const char* exefsViaName(ExefsVia v);

// Журнал последней попытки открыть exefs программы (строки шагов с кодами; ключей нет). Нет записи по
// programId — журнал последней попытки вообще; не было попыток — "".
std::string exefsJournalFor(uint64_t programId);

// Одна открытая exefs на процесс (устройство «mkexefs:»). Закрывается в деструкторе.
class ExefsMount {
public:
    ExefsMount() = default;
    ~ExefsMount();
    ExefsMount(const ExefsMount&) = delete;
    ExefsMount& operator=(const ExefsMount&) = delete;

    // programId — из заголовка NCA (если прочитан), иначе id приложения.
    bool open(uint64_t programId, const ProgramNca& nca, ExefsReport& report);
    void close();
    bool isOpen() const { return mounted_; }
    static const char* root() { return "mkexefs:/"; }

private:
    bool mounted_ = false;
    bool raw_ = false;   // смонтировано путём 3 (свой devoptab), иначе fsdev
};

}  // namespace mk::plat
