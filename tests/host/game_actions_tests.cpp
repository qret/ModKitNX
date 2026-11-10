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
    {   // Gurei: каталогу делать нечего, ZBIC нет, мод не выбран, но у sdk есть пробелы, которые модуль не восполнит
        // (nn::friends). «Подменить sdk» не показывается, «Установить» = только подмена sdk (план «Подменить sdk»).
        GameActionsInput s;
        s.sdkChosen = s.sdkDiffers = true;
        s.moduleHasWork = false;
        s.sdkGaps = true;
        auto a = decideGameActions(s);
        CHECK(!a.sdkSwap, "неустранимые пробелы — без «Подменить sdk»");
        CHECK(a.install && a.installSwapOnly, "неустранимые пробелы — «Установить» = только подмена");
        CHECK(!a.alsoSwaps, "только подмена — строки «также подменит» нет");
        s.debugChosen = true;
        a = decideGameActions(s);
        CHECK(a.install && !a.installSwapOnly && a.alsoSwaps, "отладочная сборка — ставится модуль, подмена в нём");
        s.debugChosen = false;
        s.moduleHasWork = true;
        a = decideGameActions(s);
        CHECK(!a.installSwapOnly && a.alsoSwaps, "модулю есть работа — полная установка");
        s.moduleHasWork = false;
        s.zbicUnpack = true;
        CHECK(!decideGameActions(s).installSwapOnly, "ZBIC — полная установка");
        s.zbicUnpack = false;
        s.modChosen = true;
        CHECK(!decideGameActions(s).installSwapOnly, "выбран мод — полная установка");
        s.modChosen = false;
        s.swapPresent = s.swapOurs = true;
        a = decideGameActions(s);
        CHECK(!a.installSwapOnly && a.sdkRemove, "наша подмена уже стоит — «Установить» полная, есть «Удалить подмену»");
        s.swapPresent = s.swapOurs = false;
        s.sdkGaps = false;
        a = decideGameActions(s);
        CHECK(a.sdkSwap && !a.installSwapOnly, "без пробелов — «Подменить sdk», «Установить» как прежде");
        s.sdkGaps = true;
        s.moduleInstalled = true;
        a = decideGameActions(s);
        CHECK(!a.install && !a.installSwapOnly, "модуль стоит — «Установить» нет");
    }
    {   // Pokémon Legends: Z-A (sdk игры 19.3.11, прошивка 19.0.1): подмена не нужна, ZBIC нет, модулю нечего
        // исправлять, пробелов нет — игре ничего не нужно.
        GameActionsInput s;
        s.swapNotNeeded = true;
        s.moduleHasWork = false;
        auto a = decideGameActions(s);
        CHECK(a.nothingNeeded && !a.install && !a.installModOnly, "модов нет — «ничего не нужно», без «Установить»");
        CHECK(!a.sdkChoice && !a.debugToggle && !a.sdkSwap && !a.alsoSwaps, "модов нет — без выбора sdk и флажка");
        CHECK(!a.uninstall && a.reinstall == Reinstall::None && !a.sdkRemove, "модов нет — других кнопок нет");
        // Мод в списке есть, но выбрано «Без мода» (modChosen = false) — как без модов: см. проверки выше.
        s.modChosen = s.modDiffers = true;
        a = decideGameActions(s);
        CHECK(a.install && a.installModOnly && !a.nothingNeeded, "мод выбран — «Установить мод»");
        CHECK(!a.sdkChoice && a.debugToggle && !a.alsoSwaps && !a.sdkSwap, "мод — флажок есть, выбора sdk нет");
        s.debugChosen = true;
        a = decideGameActions(s);
        CHECK(a.installModOnly && a.debugToggle, "отладочная с модом — та же кнопка");
        s.debugChosen = false;
        s.moduleInstalled = true;
        a = decideGameActions(s);
        CHECK(!a.install && !a.installModOnly && !a.nothingNeeded && a.uninstall, "модуль стоит — «Удалить установленное»");
        CHECK(a.reinstall == Reinstall::OtherMod && a.sdkChoice && a.debugToggle, "модуль стоит — прежние правила");
        s.modChosen = s.modDiffers = false;
        a = decideGameActions(s);
        CHECK(a.uninstall && a.reinstall == Reinstall::None && !a.nothingNeeded, "модуль стоит без модов — только удалить");
        s.moduleInstalled = false;
        // Любая работа — прежние правила.
        s.moduleHasWork = true;
        CHECK(!decideGameActions(s).nothingNeeded && decideGameActions(s).install, "модулю есть работа — «Установить»");
        s.moduleHasWork = false;
        s.sdkGaps = true;
        CHECK(!decideGameActions(s).nothingNeeded, "пробелы sdk — не «ничего не нужно»");
        s.sdkGaps = false;
        s.zbicUnpack = true;
        CHECK(!decideGameActions(s).nothingNeeded, "ZBIC — не «ничего не нужно»");
        s.zbicUnpack = false;
        s.swapNotNeeded = false;
        CHECK(!decideGameActions(s).nothingNeeded, "подмена нужна — не «ничего не нужно»");
        s.swapNotNeeded = true;
        s.swapPresent = s.swapOurs = true;
        a = decideGameActions(s);
        CHECK(!a.nothingNeeded && a.sdkRemove && a.install, "наша подмена стоит — «Удалить подмену», прежние правила");
        s.swapPresent = s.swapOurs = false;
        s.diagReport = true;
        a = decideGameActions(s);
        CHECK(a.nothingNeeded && a.diagReport && a.collectLogs, "ничего не нужно — отчёты по-прежнему видны");
    }
    {   // Прежние случаи не задевает: по умолчанию подмена считается нужной.
        GameActionsInput s;
        const auto a = decideGameActions(s);
        CHECK(a.install && !a.nothingNeeded && !a.installModOnly && a.sdkChoice && a.debugToggle, "по умолчанию — «Установить»");
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
