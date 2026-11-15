// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — время шагов для журнала приложения (замер «долгого запуска» на консоли; журнал выключен по
// умолчанию, Р-33). На консоли — armGetSystemTick (тики ARM, без IPC); на ПК — steady_clock.
#pragma once

#include <cstdint>

#if defined(__SWITCH__)
#include <switch.h>
#else
#include <chrono>
#endif

namespace mk::plat {

// Монотонное время в миллисекундах (для разностей).
inline uint64_t nowMs() {
#if defined(__SWITCH__)
    return armTicksToNs(armGetSystemTick()) / 1000000ull;
#else
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
#endif
}

// Сколько прошло с t (мс) — для строк журнала «шаг … N ms».
inline unsigned long long msSince(uint64_t t) { return static_cast<unsigned long long>(nowMs() - t); }

}  // namespace mk::plat
