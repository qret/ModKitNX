// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — хост-тесты правила кнопок страницы игры (source/game_actions.hpp).
#include <cstdio>

#include "game_actions.hpp"

using mk::app::decideGameActions;
using mk::app::GameActionsInput;
using mk::app::Reinstall;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, what)                                                       \
    do {                                                                        \
        if (cond) {                                                             \
            ++g_pass;                                                           \
        } else {                                                                \
            ++g_fail;                                                           \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, what); \
        }                                                                       \
    } while (0)

int main() {
    {   // Не установлено, хватает подмены (пример: «Исправлять нечего», ZBIC нет, мод не выбран).
        GameActionsInput s;
        s.sdkChosen = s.sdkDiffers = true;
        s.moduleHasWork = false;
        const auto a = decideGameActions(s);
        CHECK(a.install && !a.uninstall, "не установлено — Установить");
        CHECK(a.sdkSwap, "хватает подмены — Подменить sdk");
        CHECK(!a.alsoSwaps, "есть «Подменить sdk» — строки «также подменит» нет");
        CHECK(!a.sdkRemove && a.reinstall == Reinstall::None, "нет подмены и переустановки");
    }
    {   // Модулю есть что исправлять (2 из 3) — «Подменить sdk» нет, подмена — в «Установить».
        GameActionsInput s;
        s.sdkChosen = s.sdkDiffers = true;
        s.moduleHasWork = true;
        const auto a = decideGameActions(s);
        CHECK(!a.sdkSwap, "модулю есть работа — без «Подменить sdk»");
        CHECK(a.alsoSwaps, "«Установка также подменит sdk»");
    }
    {   // Нечего исправлять, но ZBIC снимается / выбран мод — подмены мало.
        GameActionsInput s;
        s.sdkChosen = s.sdkDiffers = true;
        s.moduleHasWork = false;
        s.zbicUnpack = true;
        CHECK(!decideGameActions(s).sdkSwap, "ZBIC — без «Подменить sdk»");
        s.zbicUnpack = false;
        s.modChosen = true;
        CHECK(!decideGameActions(s).sdkSwap, "выбран мод — без «Подменить sdk»");
        s.modChosen = false;
        s.sdkChosen = false;
        const auto a = decideGameActions(s);
        CHECK(!a.sdkSwap && !a.alsoSwaps, "sdk не выбрана — ни кнопки, ни строки");
    }
    {   // Наша подмена стоит — только «Удалить подмену»; чужая — ничего.
        GameActionsInput s;
        s.swapPresent = s.swapOurs = true;
        s.sdkChosen = s.sdkDiffers = true;
        s.moduleHasWork = false;
        auto a = decideGameActions(s);
        CHECK(a.sdkRemove && !a.sdkSwap, "наша подмена — «Удалить подмену», без «Подменить»");
        s.swapOurs = false;
        a = decideGameActions(s);
        CHECK(!a.sdkRemove && !a.sdkSwap && !a.alsoSwaps, "чужая подмена — не трогаем");
    }
    {   // Установлено без причины — «Удалить установленное», без «Установить» и «Переустановить».
        GameActionsInput s;
        s.moduleInstalled = true;
        const auto a = decideGameActions(s);
        CHECK(!a.install && a.uninstall && a.reinstall == Reinstall::None, "установлено — только удалить");
        CHECK(!a.alsoSwaps, "без переустановки — строки «также подменит» нет");
    }
    {   // Причины переустановки и их порядок.
        GameActionsInput s;
        s.moduleInstalled = true;
        s.receiptStale = true;
        s.debugChosen = true;
        CHECK(decideGameActions(s).reinstall == Reinstall::Stale, "устарело — первая причина");
        s.receiptStale = false;
        CHECK(decideGameActions(s).reinstall == Reinstall::ToDebug, "флажок отладки включён — ToDebug");
        s.debugChosen = false;
        s.debugInstalled = true;
        CHECK(decideGameActions(s).reinstall == Reinstall::ToPlain, "флажок снят у отладочной — ToPlain");
        s.debugInstalled = false;
        s.modDiffers = true;
        CHECK(decideGameActions(s).reinstall == Reinstall::OtherMod, "другой мод");
        s.modDiffers = false;
        s.sdkChosen = s.sdkDiffers = true;
        auto a = decideGameActions(s);
        CHECK(a.reinstall == Reinstall::OtherSdk && a.alsoSwaps, "другая sdk — переустановить, подменит");
        s.sdkDiffers = false;
        CHECK(decideGameActions(s).reinstall == Reinstall::None, "та же sdk — без причины");
    }
    {   // Отчёты: кнопка отчёта — по отчёту отладки, логи — по любому отчёту.
        GameActionsInput s;
        auto a = decideGameActions(s);
        CHECK(!a.diagReport && !a.collectLogs, "отчётов нет — кнопок нет");
        s.crashReport = true;
        a = decideGameActions(s);
        CHECK(!a.diagReport && a.collectLogs, "только падение — логи есть, отчёта нет");
        s.crashReport = false;
        s.diagReport = true;
        a = decideGameActions(s);
        CHECK(a.diagReport && a.collectLogs, "отчёт отладки — обе кнопки");
    }
    std::printf("game_actions: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
