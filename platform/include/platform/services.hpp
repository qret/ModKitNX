// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — службы, которые нужны приложению сверх того, что поднимает borealis (switch_wrapper.c:
// romfs, pl, set, set:sys, psm, nifm, lbl, сокеты). ns и ncm — на всё время работы; spl — по месту
// (platform/content, platform/console_info). Только в режиме тайтла.
#pragma once

namespace mk::plat {

bool initServices();  // false — ns или ncm не поднялись (журнал скажет какой); приложение покажет ошибку списка
void exitServices();

// Запрет автосна на время записи. Счётчик: вложенные области допустимы.
// Способ — TorrentShopNX (source/utils/switch_utils.cpp, preventSleepBegin/End).
class ScopedNoSleep {
public:
    ScopedNoSleep();
    ~ScopedNoSleep();
    ScopedNoSleep(const ScopedNoSleep&) = delete;
    ScopedNoSleep& operator=(const ScopedNoSleep&) = delete;
};

}  // namespace mk::plat
