/*
 * nxr_compat_exports.hpp — ЭКСПОРТЫ АВТОИСПРАВЛЕНИЯ модуля.
 *
 * Copyright (c) 2026, участники проекта nxredirect. Лицензия — GNU GPL версии 2.
 *
 * Только для модуля (включается из main.cpp): здесь svcQueryMemory и символы
 * с asm-именами. Чистая логика — nxr_autofix.hpp (её гоняет хост-тест), свои
 * реализации — nxr_compat.hpp (сверены с оригиналом в эмуляторе).
 *
 * ПО УМОЛЧАНИЮ ВКЛЮЧЕНО. Каждая сборка модуля экспортирует три символа из
 * каталога (nxr_autofix_catalog.h), но каждый экспорт — ПРОКСИ С ЗАПАСОМ: при
 * первом вызове решает (nxr::autofix::Decide), отдать вызов настоящей функции
 * загруженной `sdk` или работать своей реализацией. Поэтому один и тот же
 * модуль безопасен и на консоли с родной библиотекой (там все три уходят в
 * sdk), и на консоли с подменённой (там включается своё).
 *
 * Выключить целиком — ключ сборки -DNXR_NO_AUTOFIX (конвейер: --no-autofix):
 * тогда модуль такой же, как прежняя сборка без ключей, — ни одного экспорта.
 *
 * Прежние ключи -DNXR_COMPAT_HASH_MEMORY / -DNXR_COMPAT_LDN_STUB больше не
 * управляют модулем: ими пользуется только хост-тест hosttest/hash_test.cpp
 * (чистая своя реализация без прокси). -DNXR_COMPAT_NVRM_QUERY определяется
 * здесь сам — для кода, который спрашивает «есть ли в модуле исправление NvRm»
 * (main.cpp: ApplyNvrmFix; nxr_gfx.hpp: раздел отчёта о графике).
 *
 * СОГЛАШЕНИЕ О ВЫЗОВЕ ПРОКСИ (AAPCS64). Наша функция объявлена с той же
 * сигнатурой, что настоящая, и передаёт ВСЕ аргументы дальше без изменений:
 *   __hash_memory(const void*, size_t) -> size_t           x0, x1      -> x0
 *   nn::ldn::SetProtocol(Protocol)     -> nn::Result (u32) w0          -> w0
 *   NvRmMemQueryHandleParams(u32, void*, void*, u32) -> u32 w0, x1, x2, w3 -> w0
 * Аргументов меньше восьми, плавающих нет, результат — скаляр в x0/w0 (не
 * через x8), значит ни стек, ни x8 в передаче не участвуют; вызываемая функция
 * сохраняет x19..x28 сама. Сверка на ПК: hosttest/run_autofix.py вызывает наш
 * экспорт в эмуляторе и смотрит, с какими регистрами пришёл вызов в sdk.
 */

#pragma once

#if defined(NXR_COMPAT_HASH_MEMORY) || defined(NXR_COMPAT_LDN_STUB)
#  error "NXR_COMPAT_HASH_MEMORY / NXR_COMPAT_LDN_STUB — только для hosttest/hash_test.cpp; модуль экспортирует через автоисправление"
#endif

#ifndef NXR_NO_AUTOFIX

#define NXR_AUTOFIX 1
#ifndef NXR_COMPAT_NVRM_QUERY
#  define NXR_COMPAT_NVRM_QUERY 1
#endif

#include "nxr_compat.hpp"
#include "nxr_autofix.hpp"

/*
 * Состояние решений — оно же МЕТКА модуля nxredirect: другой экземпляр
 * nxredirect в том же процессе узнаётся по этому экспорту и исключается из
 * поиска цели (иначе два экземпляра проксировали бы друг в друга).
 * Слово на исправление, 0 — ещё не решено (упаковка — nxr::autofix::Pack).
 * Экспорт нужен и хост-проверке run_autofix.py: она читает решение отсюда.
 */
extern "C" {
    __attribute__((visibility("default"), used))
    std::uint64_t nxr_autofix_state[nxr::autofix::FixCount] asm("nxr_autofix_state_v1") = {};
}

namespace nxr::autofix {

    /* Запрос памяти у ядра — тот же, что у обхода модулей в main.cpp. */
    struct SvcQuery {
        bool operator()(std::uint64_t addr, MemRegion* out) const {
            MemoryInfo info {};
            u32 pageInfo = 0;
            if (R_FAILED(svcQueryMemory(&info, &pageInfo, addr))) {
                return false;
            }
            out->addr = info.addr;
            out->size = info.size;
            out->type = info.type;
            out->perm = info.perm;
            return true;
        }
    };

    /* Адрес внутри нашего модуля: по нему находится «свой» модуль. */
    inline std::uint64_t SelfAddr() {
        return reinterpret_cast<std::uint64_t>(&nxr_autofix_state[0]);
    }

    inline Resolution Resolve(int fix) {
        return ResolveOnce(&nxr_autofix_state[fix], [fix]() {
            SvcQuery q;
            return ComputeFix(q, fix, SelfAddr());
        });
    }

    /*
     * Счётчик вызовов для паспорта. Насыщается на CallsCap: дальше только
     * чтение, без записи — __hash_memory зовут часто и из многих потоков, и
     * постоянная запись в общую ячейку гоняла бы строку кэша между ядрами.
     */
    inline constexpr std::uint32_t CallsCap = 1000;
    inline std::uint32_t g_Calls[FixCount] = {};

    inline void NoteCall(int fix) {
        if (__atomic_load_n(&g_Calls[fix], __ATOMIC_RELAXED) < CallsCap) {
            __atomic_fetch_add(&g_Calls[fix], 1u, __ATOMIC_RELAXED);
        }
    }

}  // namespace nxr::autofix

/* ------------------------------------------------------------ __hash_memory */

extern "C" __attribute__((visibility("default"), used, noinline))
std::size_t nxr_compat_hash_memory(const void* key, std::size_t len)
    asm("_ZNSt3__113__hash_memoryEPKvm");

std::size_t nxr_compat_hash_memory(const void* key, std::size_t len) {
    using namespace nxr::autofix;
    const Resolution r = Resolve(FixHashMemory);
    NoteCall(FixHashMemory);
    if (r.choice == Choice::Proxy && r.target != 0) {
        using Fn = std::size_t (*)(const void*, std::size_t);
        return reinterpret_cast<Fn>(static_cast<std::uintptr_t>(r.target))(key, len);
    }
    return static_cast<std::size_t>(nxr::compat::HashMemory(key, static_cast<std::uint64_t>(len)));
}

/* ------------------------------------------------------- nn::ldn::SetProtocol */
/*
 * Своя — ЗАГЛУШКА: успех (nn::Result 0) и ничего. Включается, только когда в
 * sdk символа НЕТ (без нас rtld оставил бы ноль и игра упала бы при заходе в
 * меню локальной игры). Если в sdk он есть (20.5.17+), вызов уходит в него, и
 * локальный мультиплеер работает как без мода.
 */
extern "C" __attribute__((visibility("default"), used, noinline))
std::uint32_t nxr_compat_ldn_set_protocol(std::int32_t protocol)
    asm("_ZN2nn3ldn11SetProtocolENS0_8ProtocolE");

std::uint32_t nxr_compat_ldn_set_protocol(std::int32_t protocol) {
    using namespace nxr::autofix;
    const Resolution r = Resolve(FixLdnSetProtocol);
    NoteCall(FixLdnSetProtocol);
    if (r.choice == Choice::Proxy && r.target != 0) {
        using Fn = std::uint32_t (*)(std::int32_t);
        return reinterpret_cast<Fn>(static_cast<std::uintptr_t>(r.target))(protocol);
    }
    return 0;  /* nn::ResultSuccess */
}

/* --------------------------------------------------- NvRmMemQueryHandleParams */

namespace nxr::compat {
    using NvrmGetFdFn = std::int32_t (*)();
    using NvrmIoctlFn = std::uint32_t (*)(std::int32_t, std::uint32_t, void*, std::uint32_t);
    /* Крючок диагностики графики: после КАЖДОГО обращения (только память). */
    using NvrmNoteFn  = void (*)(std::uint32_t handle, const void* out, std::uint32_t rc,
                                 std::uint64_t size, std::uint64_t retAddr);

    inline constexpr char SymNvrmQuery[]    = "NvRmMemQueryHandleParams";
    inline constexpr char SymNvrmGetFd[]    = "NvRm_MemmgrGetIoctlFile";
    inline constexpr char SymNvOsDrvIoctl[] = "NvOsDrvIoctl";

    inline NvrmNoteFn g_NvrmNote = nullptr;

    /*
     * Адреса двух функций sdk для своей реализации. 0 — ещё не искали,
     * 1 — искали и не нашли (не искать на каждом вызове), иначе — адрес.
     * Гонка двух потоков безвредна: оба найдут одно и то же.
     */
    inline std::uint64_t g_NvrmHelper[2] = {0, 0};

    inline std::uint64_t NvrmHelper(int i, const char* sym) {
        std::uint64_t a = __atomic_load_n(&g_NvrmHelper[i], __ATOMIC_ACQUIRE);
        if (a == 0) {
            nxr::autofix::SvcQuery q;
            a = nxr::autofix::HelperFor(q, nxr::autofix::SelfAddr(), sym);
            if (a == 0) {
                a = 1;
            }
            __atomic_store_n(&g_NvrmHelper[i], a, __ATOMIC_RELEASE);
        }
        return a == 1 ? 0 : a;
    }
}

extern "C" __attribute__((visibility("default"), used, noinline))
std::uint32_t nxr_compat_nvrm_query(std::uint32_t handle, void* p1, void* out, std::uint32_t p3)
    asm("NvRmMemQueryHandleParams");

std::uint32_t nxr_compat_nvrm_query(std::uint32_t handle, void* p1, void* out, std::uint32_t p3) {
    using namespace nxr::autofix;
    using namespace nxr::compat;
    const Resolution r = Resolve(FixNvrmQuery);
    NoteCall(FixNvrmQuery);
    std::uint32_t rc = 2;   /* как заглушка 19.3.13 — если звать нечего */
    if (r.choice == Choice::Proxy && r.target != 0) {
        using Fn = std::uint32_t (*)(std::uint32_t, void*, void*, std::uint32_t);
        rc = reinterpret_cast<Fn>(static_cast<std::uintptr_t>(r.target))(handle, p1, out, p3);
    } else {
        const auto getFd = reinterpret_cast<NvrmGetFdFn>(
            static_cast<std::uintptr_t>(NvrmHelper(0, SymNvrmGetFd)));
        const auto doIoctl = reinterpret_cast<NvrmIoctlFn>(
            static_cast<std::uintptr_t>(NvrmHelper(1, SymNvOsDrvIoctl)));
        if (getFd != nullptr && doIoctl != nullptr) {
            rc = QueryHandleParams(handle, out, [getFd]() { return getFd(); }, doIoctl);
        }
    }
    const NvrmNoteFn note = __atomic_load_n(&g_NvrmNote, __ATOMIC_ACQUIRE);
    if (note != nullptr) {
        std::uint64_t size = 0;
        if (rc == 0 && out != nullptr) {
            __builtin_memcpy(&size, static_cast<const unsigned char*>(out) + QueryOutSizeOffset,
                             sizeof(size));
        }
        note(handle, out, rc, size,
             reinterpret_cast<std::uint64_t>(__builtin_return_address(0)));
    }
    return rc;
}

#endif  /* NXR_NO_AUTOFIX */
