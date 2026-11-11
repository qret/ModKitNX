/*
 * nxr_symfind — обход модулей процесса и поиск символа в .dynsym.
 *
 * Copyright (c) 2026, участники проекта nxredirect
 * Основано на exlaunch (github.com/shadowninja108/exlaunch), лицензия GPL-2.0.
 * Этот файл распространяется на тех же условиях: GNU GPL версии 2.
 *
 * Зачем отдельный файл
 * --------------------
 * Ровно этот код исполняется и в модуле на консоли (src/main.cpp), и в хост-тесте на ПК
 * (hosttest/symfind_test.cpp). Единственное, чем они отличаются, — функция запроса
 * памяти: на консоли это svcQueryMemory, в тесте — заглушка, которая отдаёт
 * смоделированную раскладку процесса, собранную из настоящих модулей игры.
 * Так ошибки вида «MOD0 лежит не в области кода» и «у модуля четыре области, а не
 * три» ловятся сборкой на ПК, а не запуском игры на консоли.
 *
 * Все ссылки на «эту игру» и пробы на консоли ниже относятся к Trails in the Sky
 * the 2nd (TID 01007A2027548000, обновление 1.0.3) — на ней эта логика и добывалась.
 * Ничего специфичного для конкретной игры в коде нет: раскладка модулей nnSdk
 * одинакова у всех игр на этом SDK.
 *
 * Никаких зависимостей от exlaunch, libnx и nnSdk здесь нет — только <cstdint>.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace nxr {

    /*
     * Подмножество MemoryInfo (libnx/exlaunch), которое нам нужно от ядра.
     * Функция запроса имеет вид: bool query(std::uint64_t addr, MemRegion* out).
     * false означает «ядро отказало» — обход на этом заканчивается.
     */
    struct MemRegion {
        std::uint64_t addr;
        std::uint64_t size;
        std::uint32_t type;   /* MemoryState; тип — младший байт */
        std::uint32_t perm;   /* MemoryPermission */
    };

    /* Константы ядра (совпадают с libnx: MemState_Type, MemType_*, Perm_*). */
    constexpr std::uint32_t MemStateTypeMask  = 0xFF;
    constexpr std::uint32_t MemTypeUnmapped   = 0x00;
    constexpr std::uint32_t MemTypeCodeStatic = 0x03;
    constexpr std::uint32_t MemTypeCodeMutable = 0x04;
    constexpr std::uint32_t PermNone = 0;
    constexpr std::uint32_t PermR    = 1;
    constexpr std::uint32_t PermW    = 2;
    constexpr std::uint32_t PermX    = 4;
    constexpr std::uint32_t PermRw   = PermR | PermW;
    constexpr std::uint32_t PermRx   = PermR | PermX;

    /*
     * Заголовок модуля Switch. Раскладка байт в байт совпадает с exl::util::Mod0;
     * держим свою копию, чтобы файл собирался и без exlaunch.
     */
    struct Mod0 {
        char         m_Magic[4];
        std::int32_t m_DynamicStartOffset;
        std::int32_t m_BssStartOffset;
        std::int32_t m_BssEndOffset;
        std::int32_t m_EhFrameHdrStartOffset;
        std::int32_t m_EhFrameHdrEndOffset;
        std::int32_t m_ModuleRuntimeOffset;
    };
    static_assert(sizeof(Mod0) == 0x1C, "Mod0 должен быть 28 байт");

    /* ELF64: Elf64_Dyn и Elf64_Sym. */
    struct Dyn {
        std::int64_t  d_tag;
        std::uint64_t d_val;
    };
    struct Sym {
        std::uint32_t st_name;
        std::uint8_t  st_info;
        std::uint8_t  st_other;
        std::uint16_t st_shndx;
        std::uint64_t st_value;
        std::uint64_t st_size;
    };
    static_assert(sizeof(Dyn) == 16, "Elf64_Dyn должен быть 16 байт");
    static_assert(sizeof(Sym) == 24, "Elf64_Sym должен быть 24 байта");

    constexpr std::int64_t DT_NULL_   = 0;
    constexpr std::int64_t DT_HASH_   = 4;
    constexpr std::int64_t DT_STRTAB_ = 5;
    constexpr std::int64_t DT_SYMTAB_ = 6;
    constexpr std::int64_t DT_STRSZ_  = 10;
    constexpr std::int64_t DT_SYMENT_ = 11;
    constexpr std::int64_t DT_GNU_HASH_ = 0x6FFFFEF5;
    constexpr std::uint16_t SHN_UNDEF_ = 0;

    /*
     * Пределы. Все — «заведомо с запасом»: их задача не отсечь настоящий модуль, а
     * не дать разбору уйти в бесконечность на мусорных данных.
     */
    struct Limits {
        /*
         * Насколько далеко от начала модуля может лежать MOD0.
         * ОГРАНИЧИВАТЬ РАЗМЕРОМ ОБЛАСТИ КОДА НЕЛЬЗЯ: у всех модулей этой игры MOD0
         * лежит в области данных только для чтения, сразу за кодом (проверено на
         * распакованных rtld/main/subsdk0/sdk и пробой на консоли —
         * код 0xDEAD0001, ни один модуль не был распознан).
         * РАЗМЕРОМ МОДУЛЯ ТОЖЕ НЕ ОГРАНИЧИВАТЬ. Был предел 64 МБ («больше любого
         * модуля игры») — у Biped 2 (Unity, весь код в main) main занимает ~121 МБ,
         * MOD0 оказался дальше предела, main не распознавался и модуль падал с
         * 0xDEAD0001 (отчёт с консоли 11.11.2026). Смещение — 32-битное слово, поэтому
         * предел — весь его диапазон: от мусора защищают проверка отображённости
         * (ReadableSize) и магия 'MOD0', а не размер. Хост-тест — bigmod_test.cpp.
         */
        std::uint64_t maxMod0Offset = 0x100000000ull;
        /* Сколько областей памяти пройти, прежде чем сдаться (защита от зацикливания). */
        std::uint32_t maxRegions    = 0x4000;
        /* Сколько записей .dynamic прочитать, прежде чем счесть таблицу мусором. */
        std::uint32_t maxDynEntries = 0x1000;
        /* Верхняя граница числа символов в .dynsym (у sdk игры их ~23 000). */
        std::uint64_t maxSymbols    = 1u << 22;
    };

    /* ------------------------------------------------------------------ */

    /* Сравнение имён символов: strcmp() без libc. */
    inline bool NameEquals(const char* a, const char* b) {
        while (*a != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        return *a == *b;
    }

    /*
     * Сколько байт от adr можно прочитать, не выходя за одну отображённую область.
     * 0 — читать нельзя (не отображено, нет права на чтение или ядро отказало).
     * Считаем через размер области, а не через «начало + размер», чтобы не
     * переполниться на последней области адресного пространства.
     */
    template <typename Query>
    inline std::uint64_t ReadableSize(Query& query, std::uint64_t addr) {
        MemRegion probe {};
        if (!query(addr, &probe)) {
            return 0;
        }
        if ((probe.type & MemStateTypeMask) == MemTypeUnmapped) {
            return 0;
        }
        if ((probe.perm & PermR) != PermR) {
            return 0;
        }
        if (addr < probe.addr) {
            return 0;
        }
        const std::uint64_t used = addr - probe.addr;
        if (used >= probe.size) {
            return 0;
        }
        return probe.size - used;
    }

    /* Что удалось вытащить из .dynamic: где лежат .dynsym и .dynstr. */
    struct Tables {
        std::uint64_t symtab = 0;
        std::uint64_t strtab = 0;
        std::uint64_t strsz  = 0;
        std::uint64_t count  = 0;   /* число символов в .dynsym */
        bool          hasHash = false; /* есть ли DT_HASH (у модулей игры — нет) */
        bool          hasGnuHash = false; /* есть ли DT_GNU_HASH (nxr_rofix.hpp) */
    };

    /*
     * Разбор .dynamic одного модуля: находит .dynsym и .dynstr и убеждается, что
     * обе таблицы целиком читаемы. Возвращает false, если модуль разобрать нельзя.
     * Вынесено отдельно, чтобы диагностика могла отличить «таблицы прочитаны, символа
     * нет» от «таблицы вообще не разобрались».
     */
    template <typename Query>
    inline bool ResolveTables(Query& query, std::uint64_t base, const Dyn* dyn,
                              Tables* out, const Limits& limits = {}) {
        if (dyn == nullptr || out == nullptr) {
            return false;
        }

        /* Таблица .dynamic должна быть читаема; читаем не дальше своей области. */
        std::uint64_t dynLeft = ReadableSize(query, reinterpret_cast<std::uint64_t>(dyn)) / sizeof(Dyn);
        if (dynLeft == 0) {
            return false;
        }
        if (dynLeft > limits.maxDynEntries) {
            dynLeft = limits.maxDynEntries;
        }

        Tables t {};
        std::uint64_t syment = sizeof(Sym);

        for (; dynLeft > 0 && dyn->d_tag != DT_NULL_; ++dyn, --dynLeft) {
            /*
             * Значение бывает и смещением от базы модуля, и готовым адресом:
             * rtld игры адреса в .dynamic не правит (проверено на распакованных
             * rtld/main/subsdk0/sdk: там короткие смещения), но у чужих модулей —
             * например, внедрённых плагинов — там может оказаться уже адрес.
             */
            const std::uint64_t value = dyn->d_val < base ? base + dyn->d_val : dyn->d_val;
            switch (dyn->d_tag) {
                case DT_STRTAB_: t.strtab  = value;      break;
                case DT_SYMTAB_: t.symtab  = value;      break;
                case DT_STRSZ_:  t.strsz   = dyn->d_val; break;
                case DT_SYMENT_: syment    = dyn->d_val; break;
                case DT_HASH_:   t.hasHash = true;       break;
                case DT_GNU_HASH_: t.hasGnuHash = true;  break;
                default: break;
            }
        }

        if (t.strtab == 0 || t.symtab == 0 || t.strsz == 0) {
            return false;
        }
        /* Мы умеем разбирать только стандартный ELF64: 24 байта на символ. */
        if (syment != sizeof(Sym)) {
            return false;
        }
        /* .dynsym обязана лежать перед .dynstr — иначе конец таблицы не определить. */
        if (t.symtab >= t.strtab) {
            return false;
        }

        t.count = (t.strtab - t.symtab) / sizeof(Sym);
        if (t.count == 0 || t.count > limits.maxSymbols) {
            return false;
        }

        /* Обе таблицы должны быть читаемы целиком, иначе разбор уйдёт в неотображённое. */
        if (ReadableSize(query, t.symtab) < t.count * sizeof(Sym)) {
            return false;
        }
        if (ReadableSize(query, t.strtab) < t.strsz) {
            return false;
        }

        *out = t;
        return true;
    }

    /*
     * Поиск символа в .dynsym одного модуля.
     *
     * Готовую ModuleObject::GetSymbolByName из шаблона exlaunch использовать нельзя:
     * она ищет через DT_HASH, а у модулей этой игры есть только DT_GNU_HASH —
     * указатель на корзины нулевой, и поиск падает (проба на консоли,
     * Data Abort по нулю, наш модуль +0x310).
     *
     * Число символов нигде не записано, но .dynsym лежит непосредственно перед
     * .dynstr, поэтому концом таблицы служит начало строк (проверено на
     * распакованных rtld/main/subsdk0/sdk: DT_SYMTAB < DT_STRTAB у всех четырёх).
     *
     * Возвращает адрес символа или 0.
     */
    template <typename Query>
    inline std::uint64_t FindSymbolIn(Query& query, std::uint64_t base, const Dyn* dyn,
                                      const char* name, const Limits& limits = {}) {
        Tables t {};
        /*
         * Пустое имя отвергаем сразу: в .dynsym попадаются определённые символы с
         * st_name == 0, то есть с пустым именем, и поиск "" находил бы их — а это
         * заведомо не то, что искали (поймано хост-тестом на ПК).
         */
        if (name == nullptr || name[0] == '\0' || !ResolveTables(query, base, dyn, &t, limits)) {
            return 0;
        }

        const char* strings = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(t.strtab));
        const Sym*  syms    = reinterpret_cast<const Sym*>(static_cast<std::uintptr_t>(t.symtab));

        for (std::uint64_t i = 0; i < t.count; ++i) {
            const Sym& sym = syms[i];
            /* Нас интересуют только символы, ОПРЕДЕЛЁННЫЕ в этом модуле и именованные. */
            if (sym.st_shndx == SHN_UNDEF_ || sym.st_value == 0 ||
                sym.st_name == 0 || sym.st_name >= t.strsz) {
                continue;
            }
            if (NameEquals(strings + sym.st_name, name)) {
                return base + sym.st_value;
            }
        }
        return 0;
    }

    /*
     * Есть ли у модуля ИМПОРТ с таким именем, то есть собирается ли модуль
     * ЗВАТЬ эту функцию. Импорт — символ с st_shndx == SHN_UNDEF: имя в таблице
     * есть, а определения в этом модуле нет.
     *
     * Зачем это нужно: по импорту nn::fs::SetAllocator в модулях игры видно,
     * задаёт ли игра свой распределитель nn::fs. От ответа зависит, каким путём
     * открывать затвор файловой системы (см. nxr_fsgate.hpp): у Well Dweller
     * импорт есть — ждём возврата из SetAllocator; у SiN Reloaded импорта нет
     * ни в main, ни в subsdk0..3 — значит SetAllocator не будет вызван никогда
     * и ждать его бессмысленно.
     */
    template <typename Query>
    inline bool HasImportIn(Query& query, std::uint64_t base, const Dyn* dyn,
                            const char* name, const Limits& limits = {}) {
        Tables t {};
        if (name == nullptr || name[0] == '\0' || !ResolveTables(query, base, dyn, &t, limits)) {
            return false;
        }

        const char* strings = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(t.strtab));
        const Sym*  syms    = reinterpret_cast<const Sym*>(static_cast<std::uintptr_t>(t.symtab));

        for (std::uint64_t i = 0; i < t.count; ++i) {
            const Sym& sym = syms[i];
            /* Нас интересуют ТОЛЬКО неопределённые (импортируемые) символы. */
            if (sym.st_shndx != SHN_UNDEF_) {
                continue;
            }
            if (sym.st_name == 0 || sym.st_name >= t.strsz) {
                continue;
            }
            if (NameEquals(strings + sym.st_name, name)) {
                return true;
            }
        }
        return false;
    }

    /*
     * Свой обход модулей процесса через запрос памяти у ядра.
     *
     * Готовым списком exlaunch пользоваться нельзя: его сканер ждёт у модуля ровно
     * три области подряд (код Rx, данные только для чтения R, изменяемые данные Rw),
     * а у nnSdk их больше — часть страниц после релокации переводится в режим только
     * для чтения (relro). Из-за этого nnSdk в список не попадает и все функции nn::fs
     * «не находятся» (проба на консоли, код 0xD1040400: модулей 4 из 5).
     *
     * Мы перебираем все области исполняемого кода и проверяем у начала области
     * заголовок модуля: слово по +4 — смещение до структуры MOD0.
     *
     * visit(base, dyn) → true означает «хватит, нашли».
     */
    template <typename Query, typename Visit>
    inline void ForEachModule(Query& query, Visit&& visit, const Limits& limits = {}) {
        MemRegion info {};
        std::uint64_t next = 0;

        for (std::uint32_t guard = 0; guard < limits.maxRegions; ++guard) {
            if (!query(next, &info)) {
                return;
            }
            if (info.size == 0) {
                return;
            }

            const bool isCode = (info.type & MemStateTypeMask) == MemTypeCodeStatic &&
                                info.perm == PermRx;
            if (isCode && info.size >= 0x10) {
                /*
                 * Смещение до MOD0 — слово по +4 от начала модуля. Оно почти всегда
                 * больше размера области кода: сам MOD0 лежит уже в области данных
                 * только для чтения. Чтобы не читать по мусорному адресу, сначала
                 * спрашиваем у ядра, отображён ли он (и целиком ли).
                 */
                const std::uint32_t mod0Offset =
                    reinterpret_cast<const std::uint32_t*>(
                        static_cast<std::uintptr_t>(info.addr))[1];

                /*
                 * Порог 0x8, а не 0x10 (исправлено по разбору Zelda:
                 * Tears of the Kingdom). У её main, sdk и subsdk0 смещение до MOD0
                 * равно РОВНО 0x8: crt0 кладёт заголовок сразу за словом смещения
                 * ("b start; .word _mod0 - .", и MOD0 начинается с +8). Прежний
                 * порог 0x10 отбрасывал такие модули целиком, и обход находил
                 * только rtld (у него 0x2018) и сам наш модуль — то есть ни nnMain,
                 * ни nn::fs::OpenFile не находились и мод молча не работал.
                 * Ослабление безопасно: подлинность всё равно решает магия 'MOD0'
                 * ниже, а 0x8 — наименьшее смещение, при котором заголовок не
                 * перекрывает само слово смещения по +4.
                 */
                if (mod0Offset >= 0x8 && mod0Offset < limits.maxMod0Offset) {
                    const std::uint64_t mod0Addr = info.addr + mod0Offset;
                    if (ReadableSize(query, mod0Addr) >= sizeof(Mod0)) {
                        const auto* mod0 = reinterpret_cast<const Mod0*>(
                            static_cast<std::uintptr_t>(mod0Addr));
                        const bool isMod0 = mod0->m_Magic[0] == 'M' && mod0->m_Magic[1] == 'O' &&
                                            mod0->m_Magic[2] == 'D' && mod0->m_Magic[3] == '0';
                        if (isMod0) {
                            /* Смещение .dynamic знаковое и отсчитывается ОТ MOD0. */
                            const std::uint64_t dynAddr = static_cast<std::uint64_t>(
                                static_cast<std::int64_t>(mod0Addr) + mod0->m_DynamicStartOffset);
                            const auto* dyn = reinterpret_cast<const Dyn*>(
                                static_cast<std::uintptr_t>(dynAddr));
                            if (visit(info.addr, dyn)) {
                                return;
                            }
                        }
                    }
                }
            }

            /* Следующая область. На краю адресного пространства сумма обнуляется. */
            const std::uint64_t end = info.addr + info.size;
            if (end <= next) {
                return;
            }
            next = end;
        }
    }

    /* Поиск символа во ВСЕХ модулях процесса. Возвращает адрес или 0. */
    template <typename Query>
    inline std::uint64_t FindSymbolAnywhere(Query& query, const char* name,
                                            const Limits& limits = {}) {
        std::uint64_t found = 0;
        ForEachModule(query, [&](std::uint64_t base, const Dyn* dyn) {
            found = FindSymbolIn(query, base, dyn, name, limits);
            return found != 0;
        }, limits);
        return found;
    }

    /* Импортирует ли ХОТЬ ОДИН модуль процесса символ с таким именем. */
    template <typename Query>
    inline bool HasImportAnywhere(Query& query, const char* name,
                                  const Limits& limits = {}) {
        bool found = false;
        ForEachModule(query, [&](std::uint64_t base, const Dyn* dyn) {
            found = HasImportIn(query, base, dyn, name, limits);
            return found;
        }, limits);
        return found;
    }

    /* ------------------------------------------------------------------ */
    /* КТО ВЫИГРЫВАЕТ СИМВОЛ У rtld И КУДА РАЗРЕШИЛСЯ ИМПОРТ               */
    /* ------------------------------------------------------------------ */
    /*
     * Зачем. Модуль доэкспортирует NvRmMemQueryHandleParams — символ, который
     * ЕСТЬ и в `sdk` (19.3.13: заглушка `mov w0,#2; ret`). Прежние символы
     * совместимости в `sdk` отсутствовали, поэтому вопрос «чей выиграет» не
     * вставал. Ответ дал разбор кода rtld игры (Well Dweller и SiN Reloaded —
     * .text rtld байт в байт одинаковый, sha256 e9fdb5e4…):
     *
     *   - список модулей (0x5030) заполняется в __rtld_start: сначала сам rtld,
     *     затем каждый найденный svcQueryMemory модуль ДОБАВЛЯЕТСЯ В КОНЕЦ
     *     (0x4fc..0x514), а обход памяти идёт по возрастанию адреса (0x414..0x518);
     *     loader кладёт модули по возрастанию адреса в порядке
     *     rtld, main, subsdk0..9, sdk (на консоли: наш subsdk9 0x1713cf6000 <
     *     sdk 0x1713d21000, отчёт nxredirect-diag (2).txt);
     *   - поиск глобального символа (0x1070, его же зовёт ленивый резолвер
     *     0x11f0 и связывание при загрузке через указатель 0x41a8) идёт от
     *     ГОЛОВЫ списка: для каждого модуля GetSymbolByName (0xe90; неопределённые
     *     и COMMON пропускает), и ПЕРВЫЙ символ с st_info >= 0x10 (не LOCAL)
     *     выигрывает (`cmp w8,#0x10; b.lo` @0x10c0/0x12a0).
     *
     * Значит, «чей символ выиграет» = первый ОПРЕДЕЛЁННЫЙ НЕ-LOCAL символ при
     * обходе модулей по возрастанию адреса. Ровно это и считает функция ниже —
     * тем же нашим обходом ForEachModule (он тоже идёт по возрастанию адреса).
     * Хост-тест symfind_test прогоняет её на настоящих модулях игры; поломка
     * rtld-last-wins (выигрывает последний) обязана покраснеть.
     */
    constexpr std::int64_t  DT_PLTRELSZ_ = 2;
    constexpr std::int64_t  DT_RELA_     = 7;
    constexpr std::int64_t  DT_RELASZ_   = 8;
    constexpr std::int64_t  DT_JMPREL_   = 23;
    constexpr std::uint32_t R_AARCH64_ABS64_     = 257;
    constexpr std::uint32_t R_AARCH64_GLOB_DAT_  = 1025;
    constexpr std::uint32_t R_AARCH64_JUMP_SLOT_ = 1026;
    constexpr std::uint8_t  STB_LOCAL_ = 0;

    struct Rela {
        std::uint64_t r_offset;
        std::uint64_t r_info;
        std::int64_t  r_addend;
    };
    static_assert(sizeof(Rela) == 24, "Elf64_Rela должен быть 24 байта");

    /*
     * Тот же поиск, что FindSymbolIn, но с правилом rtld: LOCAL-символ не
     * считается (rtld требует st_info >= 0x10, то есть GLOBAL или WEAK).
     */
    template <typename Query>
    inline std::uint64_t FindGlobalSymbolIn(Query& query, std::uint64_t base, const Dyn* dyn,
                                            const char* name, const Limits& limits = {}) {
        Tables t {};
        if (name == nullptr || name[0] == '\0' || !ResolveTables(query, base, dyn, &t, limits)) {
            return 0;
        }
        const char* strings = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(t.strtab));
        const Sym*  syms    = reinterpret_cast<const Sym*>(static_cast<std::uintptr_t>(t.symtab));
        for (std::uint64_t i = 0; i < t.count; ++i) {
            const Sym& sym = syms[i];
            if (sym.st_shndx == SHN_UNDEF_ || sym.st_value == 0 ||
                sym.st_name == 0 || sym.st_name >= t.strsz) {
                continue;
            }
            if ((sym.st_info >> 4) == STB_LOCAL_) {
                continue;
            }
            if (NameEquals(strings + sym.st_name, name)) {
                return base + sym.st_value;
            }
        }
        return 0;
    }

    /* Какой адрес отдаст rtld на импорт name: первый определённый не-LOCAL. */
    template <typename Query>
    inline std::uint64_t RtldWinnerOf(Query& query, const char* name, const Limits& limits = {}) {
        std::uint64_t found = 0;
        ForEachModule(query, [&](std::uint64_t base, const Dyn* dyn) {
            const std::uint64_t a = FindGlobalSymbolIn(query, base, dyn, name, limits);
            if (a != 0) {
                found = a;
                return true;   /* rtld: выигрывает ПЕРВЫЙ найденный */
            }
            return false;
        }, limits);
        return found;
    }

    /*
     * Все определения name по модулям (по возрастанию адреса): visit(base, addr).
     * Нужна диагностике — показать и «наш», и «из sdk» адреса рядом.
     */
    template <typename Query, typename Visit>
    inline int ForEachDefinitionOf(Query& query, const char* name, Visit&& visit,
                                   const Limits& limits = {}) {
        int n = 0;
        ForEachModule(query, [&](std::uint64_t base, const Dyn* dyn) {
            const std::uint64_t a = FindGlobalSymbolIn(query, base, dyn, name, limits);
            if (a != 0) {
                ++n;
                visit(base, a);
            }
            return false;
        }, limits);
        return n;
    }

    /*
     * Слот GOT, через который модуль ЗОВЁТ импорт name: адрес ячейки, куда rtld
     * записал разрешённый адрес (релокация JUMP_SLOT, GLOB_DAT или ABS64 на
     * НЕОПРЕДЕЛЁННЫЙ в этом модуле символ с таким именем). 0 — такого импорта нет.
     *
     * Прочитав ячейку, видно, КУДА импорт разрешился на самом деле — это прямой
     * ответ на вопрос «выиграл ли наш экспорт», без догадок. У OpenGL 22.2.0
     * DT_FLAGS = BIND_NOW, то есть ячейка заполнена ещё при загрузке.
     */
    template <typename Query>
    inline std::uint64_t FindImportSlotIn(Query& query, std::uint64_t base, const Dyn* dyn,
                                          const char* name, const Limits& limits = {}) {
        Tables t {};
        if (name == nullptr || name[0] == '\0' || !ResolveTables(query, base, dyn, &t, limits)) {
            return 0;
        }
        std::uint64_t rela = 0, relaSz = 0, jmprel = 0, pltSz = 0;
        std::uint64_t dynLeft = ReadableSize(query, reinterpret_cast<std::uint64_t>(dyn)) / sizeof(Dyn);
        if (dynLeft > limits.maxDynEntries) {
            dynLeft = limits.maxDynEntries;
        }
        for (; dynLeft > 0 && dyn->d_tag != DT_NULL_; ++dyn, --dynLeft) {
            const std::uint64_t value = dyn->d_val < base ? base + dyn->d_val : dyn->d_val;
            switch (dyn->d_tag) {
                case DT_RELA_:     rela   = value;      break;
                case DT_RELASZ_:   relaSz = dyn->d_val; break;
                case DT_JMPREL_:   jmprel = value;      break;
                case DT_PLTRELSZ_: pltSz  = dyn->d_val; break;
                default: break;
            }
        }
        const char* strings = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(t.strtab));
        const Sym*  syms    = reinterpret_cast<const Sym*>(static_cast<std::uintptr_t>(t.symtab));
        const std::uint64_t tables[2][2] = { { jmprel, pltSz }, { rela, relaSz } };
        for (int k = 0; k < 2; ++k) {
            const std::uint64_t at = tables[k][0];
            const std::uint64_t sz = tables[k][1];
            if (at == 0 || sz < sizeof(Rela) || ReadableSize(query, at) < sz) {
                continue;
            }
            const Rela* r = reinterpret_cast<const Rela*>(static_cast<std::uintptr_t>(at));
            const std::uint64_t n = sz / sizeof(Rela);
            for (std::uint64_t i = 0; i < n; ++i) {
                const std::uint32_t type = static_cast<std::uint32_t>(r[i].r_info & 0xFFFFFFFFu);
                const std::uint64_t si   = r[i].r_info >> 32;
                if (type != R_AARCH64_JUMP_SLOT_ && type != R_AARCH64_GLOB_DAT_ &&
                    type != R_AARCH64_ABS64_) {
                    continue;
                }
                if (si == 0 || si >= t.count) {
                    continue;
                }
                const Sym& sym = syms[si];
                if (sym.st_shndx != SHN_UNDEF_ || sym.st_name == 0 || sym.st_name >= t.strsz) {
                    continue;
                }
                if (NameEquals(strings + sym.st_name, name)) {
                    return base + r[i].r_offset;
                }
            }
        }
        return 0;
    }

    /* Первый модуль, импортирующий name: его база и адрес слота. */
    struct ImportSlot {
        std::uint64_t moduleBase = 0;
        std::uint64_t slot       = 0;
    };

    template <typename Query>
    inline ImportSlot FindImportSlotAnywhere(Query& query, const char* name,
                                             const Limits& limits = {}) {
        ImportSlot out {};
        ForEachModule(query, [&](std::uint64_t base, const Dyn* dyn) {
            const std::uint64_t s = FindImportSlotIn(query, base, dyn, name, limits);
            if (s != 0) {
                out.moduleBase = base;
                out.slot = s;
                return true;
            }
            return false;
        }, limits);
        return out;
    }

    /* Сколько модулей видно в процессе — для диагностики. */
    template <typename Query>
    inline std::uint32_t CountModules(Query& query, const Limits& limits = {}) {
        std::uint32_t count = 0;
        ForEachModule(query, [&](std::uint64_t, const Dyn*) {
            ++count;
            return false;
        }, limits);
        return count;
    }

}
