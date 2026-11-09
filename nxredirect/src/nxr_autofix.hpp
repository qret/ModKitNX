/*
 * nxr_autofix.hpp — АВТОИСПРАВЛЕНИЕ: модуль сам решает в рантайме, отдать
 * игре СВОЮ реализацию функции или ПРОКСИРОВАТЬ вызов настоящей функции `sdk`
 * (по каталогу nxr_autofix_catalog.h).
 *
 * Copyright (c) 2026, участники проекта nxredirect. Лицензия — GNU GPL версии 2.
 *
 * ЗАЧЕМ
 * -----
 * Раньше исправления (__hash_memory, заглушка nn::ldn::SetProtocol,
 * NvRmMemQueryHandleParams) включались КЛЮЧАМИ СБОРКИ. Модуль с ключом,
 * поставленный на консоль с РОДНОЙ библиотекой, перекрывал настоящие функции:
 * наш subsdkN грузится раньше `sdk`, и rtld отдаёт импорт первому
 * определению (разбор кода rtld — nxr_symfind.hpp, RtldWinnerOf). Заглушка LDN
 * молча выключала бы локальный мультиплеер, NvRm подменял бы рабочую функцию.
 *
 * КАК ТЕПЕРЬ
 * ----------
 * Экспорт есть ВСЕГДА, но это прокси с запасом. При первом вызове он смотрит,
 * что лежит в загруженной `sdk`:
 *   - настоящая функция есть -> вызов уходит ей (поведение как без мода);
 *   - функции нет (или заглушка, и `sdk` подменена) -> работает своя.
 * Правило — Decide() ниже; каталог исправлений — nxr_autofix_catalog.h (его же
 * читает ПК). Здесь только ЧИСТАЯ логика без svc и без nn::: модуль подставляет
 * запрос памяти через svcQueryMemory, хост-тест — смоделированный процесс.
 *
 * КАК ИЩЕТСЯ `sdk` (однозначно, а не «где угодно»)
 * ------------------------------------------------
 * По ИМЕНИ МОДУЛЯ в начале его rodata — того же поля, по которому Atmosphère
 * подписывает модули в отчёте о падении. Форматов два (проверено на
 * настоящих NSO из пакетов NintendoSDK (папка SDK, в репозиторий не входит) и на дампах игр):
 *   старый: u32 0, s32 длина, имя            (nnSdk 19.3.13: «nnSdk»)
 *   новый:  u32 1, u32 размер заголовка, u32, u32, u32 длина (@0x10), имя (@0x14)
 *           (nnSdk 20.5.17 и 22.2.0: «nnSdk»; opengl, vulkan, multimedia — так же)
 * Берётся последняя часть пути без расширения; `sdk` — модуль с именем ровно
 * «nnSdk», НЕ наш и НЕ другой экземпляр nxredirect (у того есть экспорт-метка
 * nxr_autofix_state_v1). Таких модулей обязан быть ровно ОДИН. Если их ноль или
 * больше одного — «sdk не опознан», и целью считается то определение, которое
 * rtld выбрал бы БЕЗ нашего модуля (первое не-LOCAL при обходе по адресу,
 * исключая все экземпляры nxredirect) — то есть «как было бы без мода».
 * Себя найти так нельзя: наш модуль и любые модули с меткой исключены.
 *
 * ВЕРСИИ
 * ------
 * «sdk старше игры» = версия из строк `SDK MW+Nintendo+NintendoSdk_nnSdk-X_Y_Z`
 * в хвосте rodata `sdk` меньше наибольшей версии таких же строк остальных
 * модулей (OpenGL, Vulkan, movie, libcurl в main — они идут с игрой и собраны
 * той же версией SDK, что её родная `sdk`). Строки лежат в последних
 * 88…1244 байтах rodata у всех проверенных модулей; читается хвост 0x4000.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "nxr_symfind.hpp"

namespace nxr::autofix {

/* ------------------------------------------------------------------ каталог */

enum class Kind : int { MISSING = 1, STUB = 2 };
enum class Scope : int { ANY = 1, NO_VULKAN = 2 };

struct FixInfo {
    const char* id;
    const char* symbol;
    Kind        kind;
    Scope       scope;
};

#define NXR_FIX(id, sym, kind, scope, hist, own) { #id, sym, Kind::kind, Scope::scope },
inline constexpr FixInfo Catalog[] = {
#include "nxr_autofix_catalog.h"
};
#undef NXR_FIX

inline constexpr int FixCount = static_cast<int>(sizeof(Catalog) / sizeof(Catalog[0]));

/* Индексы — в порядке каталога; совпадение имён проверяется при компиляции. */
enum FixIndex : int { FixHashMemory = 0, FixLdnSetProtocol = 1, FixNvrmQuery = 2 };

constexpr bool StrEq(const char* a, const char* b) {
    return *a == *b && (*a == '\0' || StrEq(a + 1, b + 1));
}
static_assert(FixCount == 3, "каталог изменился — обнови FixIndex и экспорты");
static_assert(StrEq(Catalog[FixHashMemory].symbol, "_ZNSt3__113__hash_memoryEPKvm"), "порядок каталога");
static_assert(StrEq(Catalog[FixLdnSetProtocol].symbol, "_ZN2nn3ldn11SetProtocolENS0_8ProtocolE"), "порядок каталога");
static_assert(StrEq(Catalog[FixNvrmQuery].symbol, "NvRmMemQueryHandleParams"), "порядок каталога");

/* Имена, по которым модуль опознаёт окружение. */
inline constexpr char SdkModuleName[]  = "nnSdk";
inline constexpr char MarkerSymbol[]   = "nxr_autofix_state_v1";
inline constexpr char VulkanSymbol[]   = "vkCreateInstance";
inline constexpr std::uint64_t TailWindow = 0x4000;

/* ----------------------------------------------------------------- решение */

enum class Choice : std::uint8_t { Unresolved = 0, Own = 1, Proxy = 2 };

enum class Why : std::uint8_t {
    None           = 0,
    SdkMissing     = 1,   /* sdk опознан, символа в нём нет                -> своя   */
    SdkDefined     = 2,   /* MISSING: в sdk символ есть                    -> прокси */
    SdkReal        = 3,   /* STUB: в sdk настоящая функция                 -> прокси */
    StubOlderSdk   = 4,   /* STUB: заглушка, sdk старше игры (подменена)   -> своя   */
    StubNative     = 5,   /* STUB: заглушка, но sdk не старше игры (родная) -> прокси */
    StubVulkan     = 6,   /* STUB: заглушка, но в процессе Vulkan          -> прокси */
    StubNoVersion  = 7,   /* STUB: заглушка, версии не прочитались         -> прокси */
    NoSdkMissing   = 8,   /* sdk не опознан, определения нет нигде         -> своя   */
    NoSdkDefined   = 9,   /* sdk не опознан, определение у другого модуля  -> прокси */
    TargetTooFar   = 10,  /* адрес цели не упаковывается (не бывает на Switch) -> своя */
};

struct Resolution {
    Choice        choice = Choice::Unresolved;
    Why           why    = Why::None;
    std::uint64_t target = 0;   /* куда проксировать (0 — некуда) */
};

/* Факты о процессе, из которых принимается решение по одному исправлению. */
struct FixFacts {
    bool          sdkKnown  = false;  /* ровно один модуль «nnSdk», не наш         */
    std::uint64_t def       = 0;      /* определение символа (в sdk или «без нас») */
    bool          defIsStub = false;  /* тело def — «вернуть константу»            */
    std::uint32_t sdkVer    = 0;      /* версия sdk (EncodeVersion), 0 — не прочли */
    std::uint32_t gameVer   = 0;      /* наибольшая версия библиотек игры, 0 — нет */
    bool          vulkan    = false;  /* есть модуль, определяющий vkCreateInstance */
};

/*
 * ПРАВИЛО. Чистая функция: одинаковые факты — одинаковый ответ. Безопасная
 * сторона выбрана так: если не уверены — оставляем то, что игра получила бы
 * БЕЗ мода (прокси), а своя включается только там, где без неё заведомо хуже:
 *   - символа нет -> без нас rtld оставил бы ноль и игра упала бы на вызове;
 *   - заглушка в подменённой (старшей игры) sdk, доказанная разбором.
 */
inline Resolution Decide(const FixInfo& fix, const FixFacts& f) {
    Resolution r {};
    if (f.def == 0) {
        r.choice = Choice::Own;
        r.why = f.sdkKnown ? Why::SdkMissing : Why::NoSdkMissing;
        return r;
    }
    r.target = f.def;
    r.choice = Choice::Proxy;
    if (!f.sdkKnown) {
        r.why = Why::NoSdkDefined;
        return r;
    }
    if (fix.kind == Kind::MISSING) {
        r.why = Why::SdkDefined;
        return r;
    }
    /* Kind::STUB */
    if (!f.defIsStub) {
        r.why = Why::SdkReal;
        return r;
    }
    if (fix.scope == Scope::NO_VULKAN && f.vulkan) {
        r.why = Why::StubVulkan;
        return r;
    }
    if (f.sdkVer == 0 || f.gameVer == 0) {
        r.why = Why::StubNoVersion;
        return r;
    }
    if (f.sdkVer >= f.gameVer) {
        r.why = Why::StubNative;
        return r;
    }
    r.choice = Choice::Own;
    r.why = Why::StubOlderSdk;
    r.target = 0;
    return r;
}

inline const char* ChoiceText(Choice c) {
    switch (c) {
        case Choice::Own:        return "СВОЯ";
        case Choice::Proxy:      return "ПРОКСИ";
        case Choice::Unresolved: return "не решено";
    }
    return "?";
}

inline const char* WhyText(Why w) {
    switch (w) {
        case Why::None:          return "-";
        case Why::SdkMissing:    return "в sdk символа нет";
        case Why::SdkDefined:    return "в sdk символ есть — отдаём его";
        case Why::SdkReal:       return "в sdk настоящая функция — отдаём её";
        case Why::StubOlderSdk:  return "в sdk заглушка, sdk старше библиотек игры (подменена)";
        case Why::StubNative:    return "в sdk заглушка, но sdk не старше игры — родное поведение";
        case Why::StubVulkan:    return "в sdk заглушка, но в процессе Vulkan — путь, который работает, не трогаем";
        case Why::StubNoVersion: return "в sdk заглушка, версии не прочитались — не трогаем";
        case Why::NoSdkMissing:  return "sdk не опознан, определения нет ни у кого";
        case Why::NoSdkDefined:  return "sdk не опознан — отдаём то, что rtld выбрал бы без нас";
        case Why::TargetTooFar:  return "адрес цели вне упаковки — своя";
    }
    return "?";
}

/* ------------------------------------------- упаковка и потокобезопасность */
/*
 * Решение по исправлению — ОДНО 64-битное слово: [63:56] why, [55:48] choice,
 * [47:0] адрес цели (адреса процесса на Switch < 2^39). 0 — «ещё не решено»
 * (choice никогда не 0 у принятого решения).
 *
 * ПОТОКОБЕЗОПАСНОСТЬ. Первый вызов может прийти из нескольких потоков сразу.
 * Каждый поток, увидевший 0, считает решение САМ (только чтение памяти и
 * svcQueryMemory — ни замков, ни выделений, ни nn::), затем публикует его
 * compare_exchange 0 -> слово. Победитель один; проигравший берёт ОПУБЛИКОВАННОЕ
 * слово, а не своё. Ожидания (спина) нет НАМЕРЕННО: у Horizon фиксированные
 * приоритеты, и поток с высоким приоритетом, крутящийся в ожидании, на том же
 * ядре мог бы навсегда не дать доработать низкоприоритетному. Расчёт
 * детерминирован, так что в норме оба получат одно и то же; но даже если бы
 * процесс между расчётами изменился (подгрузка NRO), решение одно на всё время
 * работы — то, что опубликовано первым. Проверка: hosttest/autofix_test.cpp
 * (вложенный «второй поток» между расчётом и публикацией + 32 настоящих потока).
 */
inline constexpr std::uint64_t TargetMask = (std::uint64_t {1} << 48) - 1;

inline std::uint64_t Pack(const Resolution& r) {
    return (static_cast<std::uint64_t>(r.why) << 56) |
           (static_cast<std::uint64_t>(r.choice) << 48) |
           (r.target & TargetMask);
}

inline Resolution Unpack(std::uint64_t w) {
    Resolution r {};
    r.why    = static_cast<Why>((w >> 56) & 0xFF);
    r.choice = static_cast<Choice>((w >> 48) & 0xFF);
    r.target = w & TargetMask;
    return r;
}

/* Решение, которое можно упаковать: цель вне 48 бит проксировать нельзя. */
inline Resolution Packable(Resolution r) {
    if (r.choice == Choice::Unresolved) {
        r.choice = Choice::Own;
    }
    if (r.target > TargetMask) {
        r.choice = Choice::Own;
        r.why = Why::TargetTooFar;
        r.target = 0;
    }
    return r;
}

template <typename Compute>
inline Resolution ResolveOnce(std::uint64_t* word, Compute&& compute) {
    std::uint64_t w = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    if (w == 0) {
        const std::uint64_t mine = Pack(Packable(compute()));
        std::uint64_t expected = 0;
        if (__atomic_compare_exchange_n(word, &expected, mine, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            w = mine;
        } else {
            w = expected;   /* опубликовано другим потоком раньше — берём ЕГО */
        }
    }
    return Unpack(w);
}

/* ---------------------------------------------------- разбор байтов (чистый) */

/* Версия SDK «X_Y_Z» -> одно число для сравнения; 0 — не версия. */
inline std::uint32_t EncodeVersion(std::uint32_t maj, std::uint32_t min, std::uint32_t pat) {
    if (maj > 255) maj = 255;
    if (min > 255) min = 255;
    if (pat > 255) pat = 255;
    return (maj << 16) | (min << 8) | pat;
}

/*
 * Имя модуля из начала rodata (оба формата, см. шапку). Возвращает длину
 * базового имени (без пути и расширения) и указатель на него, 0 — не разобрать.
 */
inline std::uint32_t ModuleNameView(const unsigned char* ro, std::uint64_t avail,
                                    const char** outName) {
    if (ro == nullptr || avail < 8) {
        return 0;
    }
    std::uint32_t w0, w1;
    __builtin_memcpy(&w0, ro, 4);
    __builtin_memcpy(&w1, ro + 4, 4);
    std::uint64_t at = 0;
    std::uint32_t len = 0;
    if (w0 == 0) {
        at = 8;
        len = w1;
    } else if (w0 == 1) {
        if (avail < 0x14 || w1 < 0x14 || w1 > 0x400) {
            return 0;
        }
        __builtin_memcpy(&len, ro + 0x10, 4);
        at = 0x14;
        if (at + len > w1) {
            return 0;
        }
    } else {
        return 0;
    }
    if (len == 0 || len > 0x200 || at + len > avail) {
        return 0;
    }
    const char* s = reinterpret_cast<const char*>(ro + at);
    /* Длина может включать завершающие нули — отрезаем. */
    while (len > 0 && s[len - 1] == '\0') {
        --len;
    }
    /* Последняя часть пути. */
    std::uint32_t start = 0;
    for (std::uint32_t i = 0; i < len; ++i) {
        if (s[i] == '/' || s[i] == '\\') {
            start = i + 1;
        }
    }
    std::uint32_t end = len;
    for (std::uint32_t i = start; i < len; ++i) {
        if (s[i] == '.') {
            end = i;   /* до ПЕРВОЙ точки базового имени: «nnSdk.nss» -> «nnSdk» */
            break;
        }
    }
    if (end <= start) {
        return 0;
    }
    for (std::uint32_t i = start; i < end; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c >= 0x7F) {
            return 0;   /* мусор, а не имя */
        }
    }
    *outName = s + start;
    return end - start;
}

inline bool NameIs(const char* name, std::uint32_t len, const char* want) {
    std::uint32_t i = 0;
    for (; i < len; ++i) {
        if (want[i] == '\0' || want[i] != name[i]) {
            return false;
        }
    }
    return want[i] == '\0';
}

/*
 * Наибольшая версия среди строк «SDK MW+Nintendo+NintendoSDK_<lib>-X_Y_Z…» и
 * «…NintendoSdk_<lib>-X_Y_Z…» в буфере. Чужие поставщики (YoYoGames, Microsoft)
 * и NintendoWare_* — не версии SDK, не считаются. 0 — ни одной.
 */
inline std::uint32_t ScanMwVersion(const unsigned char* p, std::uint64_t n) {
    static constexpr char Head[] = "SDK MW+Nintendo+Nintendo";
    constexpr std::uint64_t HeadLen = sizeof(Head) - 1;
    std::uint32_t best = 0;
    if (p == nullptr || n < HeadLen + 8) {
        return 0;
    }
    for (std::uint64_t i = 0; i + HeadLen + 8 <= n; ++i) {
        if (p[i] != 'S') {
            continue;
        }
        std::uint64_t k = 0;
        while (k < HeadLen && p[i + k] == static_cast<unsigned char>(Head[k])) {
            ++k;
        }
        if (k != HeadLen) {
            continue;
        }
        std::uint64_t j = i + HeadLen;
        /* «SDK_» или «Sdk_» */
        if (j + 4 > n || p[j] != 'S' || (p[j + 1] != 'D' && p[j + 1] != 'd') ||
            (p[j + 2] != 'K' && p[j + 2] != 'k') || p[j + 3] != '_') {
            continue;
        }
        j += 4;
        /* имя библиотеки до '-' (в пределах 64 байт) */
        std::uint64_t lim = j + 64;
        while (j < n && j < lim && p[j] != '-' && p[j] != '\0') {
            ++j;
        }
        if (j >= n || p[j] != '-') {
            continue;
        }
        ++j;
        std::uint32_t part[3] = {0, 0, 0};
        int idx = 0;
        bool digits = false;
        bool ok = true;
        while (j < n && idx < 3) {
            const unsigned char c = p[j];
            if (c >= '0' && c <= '9') {
                part[idx] = part[idx] * 10 + (c - '0');
                if (part[idx] > 100000) { ok = false; break; }
                digits = true;
                ++j;
            } else if (c == '_' && digits && idx < 2) {
                ++idx;
                digits = false;
                ++j;
            } else {
                break;
            }
        }
        if (!ok || idx != 2 || !digits) {
            continue;
        }
        const std::uint32_t v = EncodeVersion(part[0], part[1], part[2]);
        if (v > best) {
            best = v;
        }
        i = j;
    }
    return best;
}

/* GNU build-id (note: namesz 4, descsz 0x14, type 3, "GNU\0"). */
inline bool FindBuildId(const unsigned char* p, std::uint64_t n, unsigned char out[20]) {
    static constexpr unsigned char Hdr[16] = {4, 0, 0, 0, 0x14, 0, 0, 0, 3, 0, 0, 0,
                                              'G', 'N', 'U', 0};
    if (p == nullptr || n < 36) {
        return false;
    }
    for (std::uint64_t i = 0; i + 36 <= n; ++i) {
        std::uint64_t k = 0;
        while (k < 16 && p[i + k] == Hdr[k]) {
            ++k;
        }
        if (k == 16) {
            for (int b = 0; b < 20; ++b) {
                out[b] = p[i + 16 + b];
            }
            return true;
        }
    }
    return false;
}

/*
 * «Вернуть константу»: первая инструкция кладёт число в w0/x0, вторая — ret.
 *   movz w0,#imm{,lsl} 0x52800000 | movz x0 0xD2800000   (маска 0xFF80001F)
 *   movn w0            0x12800000 | movn x0 0x92800000
 *   mov  w0,wzr        0x2A1F03E0 | mov  x0,xzr 0xAA1F03E0
 *   ret                0xD65F03C0
 * Заглушка NvRmMemQueryHandleParams в 19.3.13 и 20.5.17 — `52800040 d65f03c0`.
 */
inline bool IsReturnConstStub(std::uint32_t i0, std::uint32_t i1) {
    if (i1 != 0xD65F03C0u) {
        return false;
    }
    const std::uint32_t op = i0 & 0xFF80001Fu;
    return op == 0x52800000u || op == 0xD2800000u || op == 0x12800000u ||
           op == 0x92800000u || i0 == 0x2A1F03E0u || i0 == 0xAA1F03E0u;
}

/* ------------------------------------------------------- обход процесса */

/* Где у модуля rodata: область сразу за областью кода. false — не нашли. */
template <typename Query>
inline bool RodataOf(Query& q, std::uint64_t base, std::uint64_t* roAddr, std::uint64_t* roSize) {
    MemRegion text {};
    if (!q(base, &text) || text.addr != base || text.size == 0) {
        return false;
    }
    const std::uint64_t ro = text.addr + text.size;
    MemRegion r {};
    if (!q(ro, &r) || (r.perm & PermR) != PermR || (r.perm & PermX) != 0 ||
        (r.type & MemStateTypeMask) == MemTypeUnmapped || ro < r.addr) {
        return false;
    }
    const std::uint64_t used = ro - r.addr;
    if (used >= r.size) {
        return false;
    }
    *roAddr = ro;
    *roSize = r.size - used;
    return true;
}

template <typename Query>
inline std::uint32_t ModuleNameAt(Query& q, std::uint64_t base, const char** name) {
    std::uint64_t ro = 0, roSize = 0;
    if (!RodataOf(q, base, &ro, &roSize)) {
        return 0;
    }
    const std::uint64_t avail = roSize < 0x400 ? roSize : 0x400;
    return ModuleNameView(reinterpret_cast<const unsigned char*>(static_cast<std::uintptr_t>(ro)),
                          avail, name);
}

/* Хвост rodata (последние TailWindow байт) — там строки SDK MW и build-id. */
template <typename Query>
inline bool RodataTail(Query& q, std::uint64_t base, const unsigned char** p, std::uint64_t* n) {
    std::uint64_t ro = 0, roSize = 0;
    if (!RodataOf(q, base, &ro, &roSize)) {
        return false;
    }
    const std::uint64_t take = roSize < TailWindow ? roSize : TailWindow;
    *p = reinterpret_cast<const unsigned char*>(static_cast<std::uintptr_t>(ro + roSize - take));
    *n = take;
    return true;
}

/* Свёртка процесса: всё, что нужно и решению, и паспорту. */
struct View {
    std::uint64_t selfBase     = 0;
    std::uint32_t modules      = 0;
    std::uint32_t nxrModules   = 0;   /* модулей с меткой (включая наш)   */
    std::uint32_t sdkNamed     = 0;   /* модулей «nnSdk» (кроме nxredirect) */
    std::uint64_t sdkBase      = 0;
    const Dyn*    sdkDyn       = nullptr;
    std::uint32_t sdkVer       = 0;
    std::uint32_t gameVer      = 0;
    bool          vulkan       = false;
    std::uint64_t vulkanBase   = 0;
    bool          haveBuildId  = false;
    unsigned char sdkBuildId[20] = {};

    bool SdkKnown() const { return sdkNamed == 1 && sdkBase != 0; }
};

/* База модуля, в котором лежит адрес (наибольшая база <= адреса). */
template <typename Query>
inline std::uint64_t ModuleBaseOf(Query& q, std::uint64_t addr) {
    std::uint64_t best = 0;
    ForEachModule(q, [&](std::uint64_t base, const Dyn*) {
        if (base <= addr && base > best) {
            best = base;
        }
        return false;
    });
    return best;
}

template <typename Query>
inline bool IsNxrModule(Query& q, std::uint64_t base, const Dyn* dyn, std::uint64_t selfBase) {
    return base == selfBase || FindGlobalSymbolIn(q, base, dyn, MarkerSymbol) != 0;
}

template <typename Query>
inline View ComputeView(Query& q, std::uint64_t selfAddr) {
    View v {};
    v.selfBase = ModuleBaseOf(q, selfAddr);
    ForEachModule(q, [&](std::uint64_t base, const Dyn* dyn) {
        ++v.modules;
        if (IsNxrModule(q, base, dyn, v.selfBase)) {
            ++v.nxrModules;
            return false;
        }
        const char* name = nullptr;
        const std::uint32_t len = ModuleNameAt(q, base, &name);
        const bool isSdk = len != 0 && NameIs(name, len, SdkModuleName);
        const unsigned char* tail = nullptr;
        std::uint64_t tailLen = 0;
        const bool haveTail = RodataTail(q, base, &tail, &tailLen);
        if (isSdk) {
            ++v.sdkNamed;
            if (v.sdkBase == 0) {
                v.sdkBase = base;
                v.sdkDyn = dyn;
                if (haveTail) {
                    v.sdkVer = ScanMwVersion(tail, tailLen);
                    v.haveBuildId = FindBuildId(tail, tailLen, v.sdkBuildId);
                }
            }
            return false;
        }
        if (haveTail) {
            const std::uint32_t ver = ScanMwVersion(tail, tailLen);
            if (ver > v.gameVer) {
                v.gameVer = ver;
            }
        }
        if (!v.vulkan && FindGlobalSymbolIn(q, base, dyn, VulkanSymbol) != 0) {
            v.vulkan = true;
            v.vulkanBase = base;
        }
        return false;
    });
    if (!v.SdkKnown()) {
        /* Неоднозначно — версия sdk не считается известной. */
        v.sdkVer = 0;
    }
    return v;
}

/*
 * Определение символа: в опознанной sdk; если sdk не опознан — то, что rtld
 * выбрал бы без нас (первое не-LOCAL по адресу, все nxredirect исключены).
 */
template <typename Query>
inline std::uint64_t DefinitionFor(Query& q, const View& v, const char* symbol) {
    if (v.SdkKnown()) {
        return FindGlobalSymbolIn(q, v.sdkBase, v.sdkDyn, symbol);
    }
    std::uint64_t found = 0;
    ForEachModule(q, [&](std::uint64_t base, const Dyn* dyn) {
        if (IsNxrModule(q, base, dyn, v.selfBase)) {
            return false;
        }
        found = FindGlobalSymbolIn(q, base, dyn, symbol);
        return found != 0;
    });
    return found;
}

template <typename Query>
inline bool CodeIsStub(Query& q, std::uint64_t addr) {
    if (addr == 0 || (addr & 3) != 0 || ReadableSize(q, addr) < 8) {
        return false;   /* не прочли — считаем НАСТОЯЩЕЙ: прокси, родное поведение */
    }
    const auto* code = reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(addr));
    return IsReturnConstStub(code[0], code[1]);
}

template <typename Query>
inline FixFacts FactsFor(Query& q, const View& v, int fix) {
    FixFacts f {};
    f.sdkKnown  = v.SdkKnown();
    f.def       = DefinitionFor(q, v, Catalog[fix].symbol);
    f.defIsStub = CodeIsStub(q, f.def);
    f.sdkVer    = v.sdkVer;
    f.gameVer   = v.gameVer;
    f.vulkan    = v.vulkan;
    return f;
}

template <typename Query>
inline Resolution ComputeFix(Query& q, int fix, std::uint64_t selfAddr) {
    const View v = ComputeView(q, selfAddr);
    Resolution r = Decide(Catalog[fix], FactsFor(q, v, fix));
    /* Последняя страховка от рекурсии: цель в нашем модуле — не цель. */
    if (r.choice == Choice::Proxy && v.selfBase != 0 &&
        ModuleBaseOf(q, r.target) == v.selfBase) {
        r.choice = Choice::Own;
        r.target = 0;
    }
    return r;
}

/* Вспомогательный символ «из sdk» (или «как без нас») — для своей реализации. */
template <typename Query>
inline std::uint64_t HelperFor(Query& q, std::uint64_t selfAddr, const char* symbol) {
    const View v = ComputeView(q, selfAddr);
    return DefinitionFor(q, v, symbol);
}

/* --------------------------------------------------------- паспорт консоли */

/*
 * Строка фиксированного размера: куча не используется. Все записи — под
 * проверкой m_Len + k < N. GCC 15 (devkitA64) на экземпляре в .bss выдаёт
 * ложное -Wstringop-overflow («region of size 0»): границу он не выводит из
 * условия; предупреждение для этой структуры выключено точечно.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
template <std::size_t N>
struct TextBuf {
    char        m_Buf[N];
    std::size_t m_Len = 0;

    void Add(const char* s) {
        while (s != nullptr && *s != '\0' && m_Len + 1 < N) {
            m_Buf[m_Len++] = *s++;
        }
    }
    void AddN(const char* s, std::size_t n) {
        for (std::size_t i = 0; i < n && m_Len + 1 < N; ++i) {
            m_Buf[m_Len++] = s[i];
        }
    }
    void AddHex(std::uint64_t value) {
        Add("0x");
        char tmp[16];
        int n = 0;
        do {
            const unsigned d = static_cast<unsigned>(value & 0xF);
            tmp[n++] = static_cast<char>(d < 10 ? ('0' + d) : ('a' + d - 10));
            value >>= 4;
        } while (value != 0 && n < 16);
        while (n > 0 && m_Len + 1 < N) {
            m_Buf[m_Len++] = tmp[--n];
        }
    }
    void AddHex16(std::uint64_t value) {   /* ровно 16 цифр, ЗАГЛАВНЫЕ: TitleID */
        for (int i = 15; i >= 0 && m_Len + 1 < N; --i) {
            const unsigned d = static_cast<unsigned>((value >> (i * 4)) & 0xF);
            m_Buf[m_Len++] = static_cast<char>(d < 10 ? ('0' + d) : ('A' + d - 10));
        }
    }
    void AddDec(std::uint64_t value) {
        char tmp[20];
        int n = 0;
        do {
            tmp[n++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        } while (value != 0 && n < 20);
        while (n > 0 && m_Len + 1 < N) {
            m_Buf[m_Len++] = tmp[--n];
        }
    }
    void AddVersion(std::uint32_t v) {
        if (v == 0) {
            Add("не прочитана");
            return;
        }
        AddDec((v >> 16) & 0xFF);
        Add(".");
        AddDec((v >> 8) & 0xFF);
        Add(".");
        AddDec(v & 0xFF);
    }
    void Line() {   /* CRLF: файл читают в блокноте Windows */
        if (m_Len + 2 < N) {
            m_Buf[m_Len++] = '\r';
            m_Buf[m_Len++] = '\n';
        }
    }
    const char* Data() const { return m_Buf; }
    std::size_t Size() const { return m_Len; }
};
#pragma GCC diagnostic pop

/* Всё, что попадает в паспорт. Заполняет модуль, форматирует функция ниже. */
struct PassportFacts {
    std::uint64_t titleId       = 0;
    const char*   slot          = "";
    bool          kernelMetaOk  = false;   /* svcGetInfo(65000, 0, 0) ответил успехом */
    std::uint64_t kernelMeta    = 0;
    std::uint32_t kernelMetaRc  = 0;
    View          view {};
    Resolution    res[FixCount] {};
    std::uint32_t calls[FixCount] = {};
    int           nvrmPath      = 0;       /* nxr::compat::NvrmPath, 0 — не считался */
    const char*   nvrmPathText  = nullptr;
};

inline constexpr char PassportName[] = "nxredirect-passport.txt";

template <typename Buf>
inline void FormatPassport(Buf& b, const PassportFacts& f) {
    b.Add("\xEF\xBB\xBF");   /* BOM: блокнот покажет русский текст */
    b.Add("nxredirect — паспорт консоли (пишется при каждом запуске игры, после затвора ФС)");
    b.Line();
    b.Add("TitleID: ");
    b.AddHex16(f.titleId);
    b.Add("   модуль: ");
    b.Add(f.slot);
    b.Add(", автоисправление v1");
    b.Line();
    b.Add("ядро Atmosphère (mesosphère, svcGetInfo 65000/0): ");
    if (f.kernelMetaOk) {
        b.Add("да, версия ядра ");
        b.AddHex(f.kernelMeta);
    } else {
        b.Add("нет ответа, код ");
        b.AddHex(f.kernelMetaRc);
    }
    b.Line();
    b.Add("версия HOS: из процесса игры без новых прав не читается (см. docs/архитектура.md §10ж)");
    b.Line();
    const View& v = f.view;
    b.Add("загружена sdk: ");
    if (v.SdkKnown()) {
        b.Add("модуль nnSdk, база ");
        b.AddHex(v.sdkBase);
        b.Add(", версия ");
        b.AddVersion(v.sdkVer);
        b.Add(", BuildId ");
        if (v.haveBuildId) {
            static constexpr char Hex[] = "0123456789ABCDEF";
            for (int i = 0; i < 20; ++i) {
                const char two[2] = {Hex[v.sdkBuildId[i] >> 4], Hex[v.sdkBuildId[i] & 0xF]};
                b.AddN(two, 2);
            }
        } else {
            b.Add("не найден");
        }
    } else {
        b.Add("НЕ ОПОЗНАНА однозначно (модулей «nnSdk»: ");
        b.AddDec(v.sdkNamed);
        b.Add(")");
    }
    b.Line();
    b.Add("библиотеки SDK игры (строки SDK MW): ");
    b.AddVersion(v.gameVer);
    b.Line();
    b.Add("подмена sdk: ");
    if (!v.SdkKnown() || v.sdkVer == 0 || v.gameVer == 0) {
        b.Add("НЕ ОПРЕДЕЛЕНА");
    } else if (v.sdkVer < v.gameVer) {
        b.Add("ДА — sdk старше библиотек игры");
    } else {
        b.Add("нет — sdk не старше библиотек игры");
    }
    b.Line();
    b.Add("Vulkan в процессе: ");
    b.Add(v.vulkan ? "да" : "нет");
    b.Add("   модулей: ");
    b.AddDec(v.modules);
    b.Add(", из них nxredirect: ");
    b.AddDec(v.nxrModules);
    b.Line();
    b.Add("исправления (СВОЯ — работает наша реализация, ПРОКСИ — вызов уходит в sdk):");
    b.Line();
    for (int i = 0; i < FixCount; ++i) {
        b.Add("  ");
        b.Add(Catalog[i].id);
        b.Add(": ");
        b.Add(ChoiceText(f.res[i].choice));
        b.Add(" — ");
        b.Add(WhyText(f.res[i].why));
        if (f.res[i].choice == Choice::Proxy) {
            b.Add(", цель ");
            b.AddHex(f.res[i].target);
        }
        b.Add("; вызовов к записи: ");
        b.AddDec(f.calls[i]);
        b.Line();
    }
    if (f.nvrmPathText != nullptr) {
        b.Add("  путь nvrm_query к игре: ");
        b.Add(f.nvrmPathText);
        b.Line();
    }
}

/*
 * Запись паспорта. ЗАТВОР — здесь же, а не только у вызывающего: пока затвор
 * ФС закрыт, ни одного обращения к файловой системе (иначе игра падает на своём
 * nn::fs::SetAllocator, Result 0x327602 — см. nxr_fsgate.hpp). Fs — что угодно
 * с методом bool Write(const char* path, const char* data, std::size_t len).
 * Проверка: hosttest/autofix_test.cpp (закрытый затвор — ноль обращений).
 */
template <typename Gate, typename Fs>
inline bool WritePassport(const Gate& gate, Fs& fs, const char* path,
                          const char* data, std::size_t len) {
    if (!gate.IsOpen()) {
        return false;
    }
    if (path == nullptr || data == nullptr || len == 0) {
        return false;
    }
    return fs.Write(path, data, len);
}

/*
 * Путь паспорта из префикса пути мода «<mount>:/atmosphere/contents/<TID>/files/»:
 * рядом с exefs/ и files/ — «<mount>:/atmosphere/contents/<TID>/nxredirect-passport.txt».
 * 0 — префикс не того вида или не влез.
 */
inline std::size_t PassportPath(char* out, std::size_t outSize, const char* modPrefix,
                                std::size_t prefixLen) {
    static constexpr char Files[] = "files/";
    constexpr std::size_t FilesLen = sizeof(Files) - 1;
    if (out == nullptr || modPrefix == nullptr || prefixLen < FilesLen) {
        return 0;
    }
    for (std::size_t i = 0; i < FilesLen; ++i) {
        if (modPrefix[prefixLen - FilesLen + i] != Files[i]) {
            return 0;
        }
    }
    const std::size_t keep = prefixLen - FilesLen;
    const std::size_t nameLen = sizeof(PassportName) - 1;
    if (keep + nameLen + 1 > outSize) {
        return 0;
    }
    for (std::size_t i = 0; i < keep; ++i) {
        out[i] = modPrefix[i];
    }
    for (std::size_t i = 0; i < nameLen; ++i) {
        out[keep + i] = PassportName[i];
    }
    out[keep + nameLen] = '\0';
    return keep + nameLen;
}

}  // namespace nxr::autofix
