#pragma once

#include "common.hpp"

#define EXL_MODULE_NAME "nxredirect"

/*
 * Релизная сборка: без ассертов и отладочного лога.
 *
 * ВАЖНО. Logging здесь — SvcLogger (loggers.hpp), то есть КАЖДАЯ строка лога
 * превращается в системный вызов svcOutputDebugString. Поэтому все вызовы
 * Logging.Log обёрнуты в `#if defined(EXL_DEBUG)` (и в нашем коде, и в библиотеке
 * exlaunch) — в релизе их в модуле нет вовсе.
 *
 * Обёрнуто ТОЛЬКО логирование. Сравните с romfsex (проект автора DBI): там под
 * `#if defined(EXL_DEBUG)` в source/lib/init/init.cpp вместе с логом попал ещё и
 * вызов exl::util::impl::InitSocType(), то есть в релизной сборке отключалась часть
 * инициализации. У нас этот вызов оставлен вне условия (см. init.cpp).
 */
/* #define EXL_DEBUG */
#define EXL_USE_FAKEHEAP

/*
#define EXL_SUPPORTS_REBOOTPAYLOAD
*/

namespace exl::setting {
    /* How large the fake .bss heap will be. */
    constexpr size_t HeapSize = 0x5000;

    /*
     * How large the JIT area will be for hooks.
     *
     * nxredirect: было 0x1000 — ровно 20 трамплинов
     * (JitSize / (MaxInstructions * 10 * sizeof(u32)) = 0x1000 / 200). Проба
     * ШАГ-7 ставила 22 хука, пул кончился, и exlaunch аварийно завершил игру
     * (Data Abort по 0x6969696900000000 — подпись exl::diag::AbortWithCtx).
     * Удвоено до 0x2000 = 40 трамплинов; это всего одна лишняя страница JIT.
     * Число продублировано в src/nxr_gfx.hpp (JitSizeBytes) и сверяется
     * гейтом hosttest/gfx_targets.py, чтобы бюджет не разъехался.
     */
    constexpr size_t JitSize = 0x2000;

    /* How large the area will be inline hook pool. */
    constexpr size_t InlinePoolSize = 0x1000;

    /* How large the formatting buffer should be for logging. The buffer will be on the stack. */
    constexpr size_t LogBufferSize = 512;

    /* Sanity checks. */
    static_assert(ALIGN_UP(JitSize, PAGE_SIZE) == JitSize, "");
    static_assert(ALIGN_UP(InlinePoolSize, PAGE_SIZE) == InlinePoolSize, "");
}
