/*
 * nxr_rofix.hpp — поиск символов nn::ro в старой sdk, которая не знает
 * DT_GNU_HASH.
 *
 * Copyright (c) 2026, участники проекта nxredirect. Лицензия — GNU GPL версии 2.
 *
 * ЗАЧЕМ
 * -----
 * Biped 2 (Unity, библиотеки SDK 20.5.15) на подменённой sdk 19.3.13 падал в
 * nn::ro::LookupSymbol (отчёт Atmosphère: PC nnSdk+0x14b27c). Разбор кода sdk:
 *   - у всех функций nn::ro (LookupSymbol, LookupModuleSymbol, LoadModule,
 *     detail::Bind, LookupGlobalManual …) поиск символа в ОДНОМ модуле — одна
 *     внутренняя функция Sym* F(ModuleObject* obj, const char* name);
 *   - в 19.3.13 F знает только SysV-таблицу DT_HASH: ведро — obj+80, число
 *     вёдер — obj+168, цепочки — obj+88;
 *   - в 20.x F сначала смотрит флаг obj+195 (бит 4 — у модуля DT_GNU_HASH) и
 *     ищет по GNU-таблице; obj+96 — .dynstr, obj+104 — .dynsym в обеих версиях;
 *   - rtld игры (SDK 20) у модуля без DT_HASH кладёт по obj+80 данные
 *     GNU-таблицы, и F из 19.3.13 читает их как SysV-вёдра: мусорный индекс
 *     символа, Data Abort.
 *
 * ЧТО ДЕЛАЕМ
 * ----------
 * Подменяем F своей всегда, когда F старая (сразу после пролога
 * `ldr x?, [x0, #80]`, нет проверки флага obj+195): DT_GNU_HASH может принести и
 * модуль, подгруженный позже (NRO), а для модулей с одной DT_HASH свой поиск даёт
 * тот же ответ (сверено на Biped 2). Своя F для ЛЮБОГО модуля из снимка (ниже), а
 * модуль не из снимка разбирается по своей базе (TablesAtBase) —
 * перебор .dynsym с теми же отсевами, что у оригинала (неопределённые,
 * SHN_COMMON, STT_FILE), по ИМЕНИ; поля хеш-таблиц в ModuleObject не читаем.
 *
 * НЕ «есть DT_HASH — значит оригинал»: так было в 1.0.7–1.0.8, и main Biped 2
 * упал в оригинале. Код ModuleObject::Initialize sdk 20.5.14 (@0x14e2f4): есть
 * DT_GNU_HASH — по obj+80 кладётся GNU-таблица и ставится флаг, DT_HASH при этом
 * НЕ используется; DT_HASH берётся, только если GNU нет. rtld игры — того же SDK.
 * Таблицы берём не из ModuleObject (его раскладка — дело rtld), а из MOD0 и
 * .dynamic самого модуля; из ModuleObject — только база модуля (obj+40: её же
 * читают LookupSymbol обеих версий, чтобы получить адрес символа).
 *
 * Где F: первый `bl` в экспортированной nn::ro::LookupModuleSymbol сразу после
 * `ldr x0, [x1]` (ModuleObject модуля) и `mov x1, x2` (имя) — так в 19.3.13 и
 * 20.5.x. Не совпало — ничего не трогаем.
 *
 * Здесь только чистая логика (как nxr_symfind.hpp): хост-тест — rofix_test.cpp.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "nxr_symfind.hpp"

namespace nxr::rofix {

    inline constexpr char SymLookupModuleSymbol[] =
        "_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc";

    /* Смещение базы модуля в ModuleObject rtld (19.3.13 и 20.x: `ldr x, [obj, #40]`). */
    inline constexpr std::uint64_t ModuleObjectBase = 40;

    constexpr std::uint32_t InsnLdrX0X1    = 0xF9400020u;   /* ldr x0, [x1]  */
    constexpr std::uint32_t InsnMovX1X2    = 0xAA0203E1u;   /* mov x1, x2    */
    constexpr std::uint32_t InsnBlMask     = 0xFC000000u;
    constexpr std::uint32_t InsnBl         = 0x94000000u;
    /* ldr x<t>, [x0, #80]: LDR (unsigned offset, 64 бит), imm12 = 80/8 = 10, Rn = x0 */
    constexpr std::uint32_t InsnLdrX0At80Mask = 0xFFFFFFE0u;
    constexpr std::uint32_t InsnLdrX0At80     = 0xF9402800u;
    /* ldrb w<t>, [x0, #195]: LDRB (unsigned offset), imm12 = 195, Rn = x0 */
    constexpr std::uint32_t InsnLdrbX0At195Mask = 0xFFFFFFE0u;
    constexpr std::uint32_t InsnLdrbX0At195     = 0x39430C00u;

    constexpr std::uint16_t SHN_COMMON_ = 0xFFF2;
    constexpr std::uint8_t  STT_FILE_   = 4;

    /*
     * Адрес F по коду LookupModuleSymbol (code — её первые слова, addr — её адрес).
     * 0 — образец не совпал.
     */
    inline std::uint64_t FindInternalLookup(const std::uint32_t* code, std::uint64_t addr,
                                            int words = 16) {
        if (code == nullptr) {
            return 0;
        }
        bool sawObj = false;
        for (int i = 0; i < words; ++i) {
            const std::uint32_t w = code[i];
            if (w == InsnLdrX0X1) {
                sawObj = true;
            }
            if ((w & InsnBlMask) == InsnBl) {
                if (!sawObj || i == 0 || code[i - 1] != InsnMovX1X2) {
                    return 0;
                }
                std::int64_t imm = static_cast<std::int64_t>(w & 0x03FFFFFFu);
                if (imm & 0x02000000) {
                    imm -= 0x04000000;
                }
                return addr + static_cast<std::uint64_t>(i) * 4 + static_cast<std::uint64_t>(imm * 4);
            }
        }
        return 0;
    }

    /*
     * Старая ли F (только DT_HASH): в первых словах есть `ldr x?, [x0, #80]`
     * и нет `ldrb w?, [x0, #195]`. Смотрим до первого обращения к obj+80.
     */
    inline bool IsSysvOnly(const std::uint32_t* code, int words = 12) {
        if (code == nullptr) {
            return false;
        }
        for (int i = 0; i < words; ++i) {
            const std::uint32_t w = code[i];
            if ((w & InsnLdrbX0At195Mask) == InsnLdrbX0At195) {
                return false;
            }
            if ((w & InsnLdrX0At80Mask) == InsnLdrX0At80) {
                return true;
            }
        }
        return false;
    }

    /* .dynamic модуля по его базе (MOD0 по слову +4). nullptr — не модуль. */
    template <typename Query>
    inline const Dyn* DynamicOf(Query& query, std::uint64_t base) {
        if (base == 0 || ReadableSize(query, base) < 8) {
            return nullptr;
        }
        const std::uint32_t mod0Offset =
            reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(base))[1];
        if (mod0Offset < 0x8) {
            return nullptr;
        }
        const std::uint64_t mod0Addr = base + mod0Offset;
        if (ReadableSize(query, mod0Addr) < sizeof(Mod0)) {
            return nullptr;
        }
        const auto* mod0 = reinterpret_cast<const Mod0*>(static_cast<std::uintptr_t>(mod0Addr));
        if (mod0->m_Magic[0] != 'M' || mod0->m_Magic[1] != 'O' || mod0->m_Magic[2] != 'D' ||
            mod0->m_Magic[3] != '0') {
            return nullptr;
        }
        return reinterpret_cast<const Dyn*>(static_cast<std::uintptr_t>(
            static_cast<std::uint64_t>(static_cast<std::int64_t>(mod0Addr) + mod0->m_DynamicStartOffset)));
    }

    /*
     * СНИМОК МОДУЛЕЙ. Внутри перехвата НЕЛЬЗЯ звать ядро: на консоли (Biped 2,
     * поиск пришёл через nn::ro::LoadModule -> detail::LookupGlobalManual)
     * svcQueryMemory из перехвата дал «Undefined System Call 0x06», хотя при старте
     * игры тот же вызов работает. Поэтому всё, что нужно перехвату, снимается
     * заранее (ApplyRoFix, из хука nnMain): база, .dynsym, .dynstr, число символов
     * и есть ли DT_HASH. Перехват только читает снимок; модуля нет в снимке
     * (подгружен позже) — разбор по его базе (TablesAtBase ниже), не разобрался —
     * оригинал sdk, как без исправления.
     */
    struct ModuleTables {
        std::uint64_t base   = 0;
        std::uint64_t symtab = 0;
        std::uint64_t strtab = 0;
        std::uint64_t strsz  = 0;
        std::uint64_t count  = 0;
        bool          hasHash = false;
        bool          hasGnuHash = false;
    };

    struct Snapshot {
        static constexpr int Max = 32;   /* rtld, main, subsdk0..9, sdk, внедрённые — с запасом */
        ModuleTables mods[Max] {};
        int          n = 0;
        bool         anyGnuHash = false;   /* у какого-то модуля есть DT_GNU_HASH — нужен перехват */

        const ModuleTables* Find(std::uint64_t base) const {
            for (int i = 0; i < n; ++i) {
                if (mods[i].base == base) {
                    return &mods[i];
                }
            }
            return nullptr;
        }
    };

    template <typename Query>
    inline void TakeSnapshot(Query& query, Snapshot* s) {
        s->n = 0;
        s->anyGnuHash = false;
        ForEachModule(query, [&](std::uint64_t base, const Dyn* dyn) {
            Tables t {};
            if (s->n < Snapshot::Max && ResolveTables(query, base, dyn, &t)) {
                ModuleTables& m = s->mods[s->n++];
                m.base = base;
                m.symtab = t.symtab;
                m.strtab = t.strtab;
                m.strsz = t.strsz;
                m.count = t.count;
                m.hasHash = t.hasHash;
                m.hasGnuHash = t.hasGnuHash;
                if (t.hasGnuHash) {
                    s->anyGnuHash = true;
                }
            }
            return false;
        });
    }

    /* Поиск по снимку: перебор .dynsym с отсевами оригинала. Ядро не зовётся. */
    inline const Sym* FindDefinedSym(const ModuleTables& m, const char* name) {
        if (name == nullptr || name[0] == '\0' || m.symtab == 0 || m.strtab == 0) {
            return nullptr;
        }
        const char* strings = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(m.strtab));
        const Sym*  syms    = reinterpret_cast<const Sym*>(static_cast<std::uintptr_t>(m.symtab));
        /* Не-LOCAL — сразу; LOCAL с тем же именем — запасной ответ: вызывающие
           (LookupSymbol, LookupGlobalManual) отбрасывают st_info < 0x10, и первым
           LOCAL нельзя заслонить GLOBAL, стоящий дальше в таблице. */
        const Sym* local = nullptr;
        for (std::uint64_t i = 0; i < m.count; ++i) {
            const Sym& s = syms[i];
            if (s.st_shndx == SHN_UNDEF_ || s.st_shndx == SHN_COMMON_ ||
                (s.st_info & 0xF) == STT_FILE_ || s.st_name == 0 || s.st_name >= m.strsz) {
                continue;
            }
            if (NameEquals(strings + s.st_name, name)) {
                if ((s.st_info >> 4) != STB_LOCAL_) {
                    return &s;
                }
                if (local == nullptr) {
                    local = &s;
                }
            }
        }
        return local;
    }

    /*
     * МОДУЛЬ НЕ ИЗ СНИМКА (подгружен позже: NRO через nn::ro::LoadModule — Shrek 2
     * Remastered, 12.11.2026). Его ModuleObject строит сама старая sdk, только по
     * DT_HASH: у NRO без DT_HASH ведро — ноль, и оригинальная F падает. Разбираем
     * таблицы прямо по базе из ModuleObject (obj+40), БЕЗ ядра (внутри перехвата
     * оно отказывает): слово +4 — смещение MOD0, магия 'MOD0', .dynamic, DT_SYMTAB /
     * DT_STRTAB / DT_STRSZ / DT_SYMENT. Модуль по этой базе загружен (sdk сама ищет
     * в нём), читаем его начало и те же таблицы, что читает оригинал. Не сошлось
     * хоть что-то — false, и вызов уходит в оригинал, как без исправления.
     * Пределы — те же «заведомо с запасом», что у Limits (nxr_symfind.hpp).
     */
    inline bool TablesAtBase(std::uint64_t base, ModuleTables* out) {
        if (base == 0 || out == nullptr) {
            return false;
        }
        const Limits lim {};
        const std::uint32_t mod0Offset =
            reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(base))[1];
        if (mod0Offset < 0x8 || mod0Offset >= lim.maxMod0Offset) {
            return false;
        }
        const std::uint64_t mod0Addr = base + mod0Offset;
        const auto* mod0 = reinterpret_cast<const Mod0*>(static_cast<std::uintptr_t>(mod0Addr));
        if (mod0->m_Magic[0] != 'M' || mod0->m_Magic[1] != 'O' || mod0->m_Magic[2] != 'D' ||
            mod0->m_Magic[3] != '0') {
            return false;
        }
        const auto* dyn = reinterpret_cast<const Dyn*>(static_cast<std::uintptr_t>(
            static_cast<std::uint64_t>(static_cast<std::int64_t>(mod0Addr) + mod0->m_DynamicStartOffset)));
        ModuleTables t {};
        t.base = base;
        std::uint64_t syment = sizeof(Sym);
        for (std::uint32_t i = 0; i < lim.maxDynEntries && dyn[i].d_tag != DT_NULL_; ++i) {
            const std::uint64_t value = dyn[i].d_val < base ? base + dyn[i].d_val : dyn[i].d_val;
            switch (dyn[i].d_tag) {
                case DT_STRTAB_:   t.strtab = value;         break;
                case DT_SYMTAB_:   t.symtab = value;         break;
                case DT_STRSZ_:    t.strsz = dyn[i].d_val;   break;
                case DT_SYMENT_:   syment = dyn[i].d_val;    break;
                case DT_HASH_:     t.hasHash = true;         break;
                case DT_GNU_HASH_: t.hasGnuHash = true;      break;
                default: break;
            }
        }
        if (t.strtab == 0 || t.symtab == 0 || t.strsz == 0 || syment != sizeof(Sym) || t.symtab >= t.strtab) {
            return false;
        }
        t.count = (t.strtab - t.symtab) / sizeof(Sym);
        if (t.count == 0 || t.count > lim.maxSymbols) {
            return false;
        }
        *out = t;
        return true;
    }

    /*
     * Тот же поиск по базе модуля через query (зовёт ядро) — только для
     * проверок вне перехвата (хост-тест). Возвращает Elf64_Sym* или nullptr.
     */
    template <typename Query>
    inline const Sym* FindDefinedSym(Query& query, std::uint64_t base, const char* name) {
        if (name == nullptr || name[0] == '\0') {
            return nullptr;
        }
        Tables t {};
        const Dyn* dyn = DynamicOf(query, base);
        if (dyn == nullptr || !ResolveTables(query, base, dyn, &t)) {
            return nullptr;
        }
        const char* strings = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(t.strtab));
        const Sym*  syms    = reinterpret_cast<const Sym*>(static_cast<std::uintptr_t>(t.symtab));
        for (std::uint64_t i = 0; i < t.count; ++i) {
            const Sym& s = syms[i];
            if (s.st_shndx == SHN_UNDEF_ || s.st_shndx == SHN_COMMON_ ||
                (s.st_info & 0xF) == STT_FILE_ || s.st_name == 0 || s.st_name >= t.strsz) {
                continue;
            }
            if (NameEquals(strings + s.st_name, name)) {
                return &s;
            }
        }
        return nullptr;
    }

}  // namespace nxr::rofix
