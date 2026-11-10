// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — какие кнопки показать на странице игры. Одна чистая функция от состояния (без borealis и платформы),
// её проверяют хост-тесты (tests/host/game_actions_tests.cpp); экран (ui/src/game_activity.cpp) только рисует ответ.
//
// Правила:
//   - «Подменить sdk» — только когда одной подмены достаточно: выбрана sdk, модулю для неё нечего исправлять и
//     нечего «не восполнит», снимать ZBIC не нужно, мод не выбран, и в папке игры sdk ещё не лежит. Иначе подмена
//     выбранной sdk входит в «Установить» — тогда у кнопки строка «Установка также подменит sdk на …».
//   - Делать, кроме подмены, нечего, но у sdk есть пробелы, которые модуль не восполнит («Не восполнит»): модуль их
//     не закроет, поэтому «Установить» выполняет ТОЛЬКО подмену sdk — тот же план, что «Подменить sdk» (без модуля и
//     без main.npdm), строка «Установка подменит sdk на … — больше ничего не нужно». Исключение — отмечена
//     «Отладочная сборка»: тогда ставится модуль (он и пишет отчёт отладки).
//   - Наша подмена уже стоит — «Подменить sdk» нет, есть «Удалить подмену» (опасная, красная). Чужую не трогаем.
//   - Модуль не установлен — «Установить». Установлен — «Удалить установленное» (опасная), а «Переустановить» —
//     только при причине: установка устарела, вид сборки (обычная / отладочная) не совпадает с флажком, выбран
//     другой мод или другая sdk, чем в установке.
//   - «Отчёт отладки» — только если у игры есть отчёт; «Собрать логи для отправки» — если есть отчёт модуля или
//     отчёт Atmosphère о падении этой игры.
#pragma once

namespace mk::app {

struct GameActionsInput {
    bool moduleInstalled = false;   // наш модуль стоит (квитанция со слотом модуля)
    bool receiptStale = false;      // модуль на карте не совпадает с квитанцией
    bool debugInstalled = false;    // стоит отладочная сборка
    bool debugChosen = false;       // флажок «Отладочная сборка»
    bool modChosen = false;         // выбран мод
    bool modDiffers = false;        // выбранный мод (или «без мода») не тот, что в установке
    bool sdkChosen = false;         // выбрана sdk для подмены
    bool sdkDiffers = false;        // выбранная sdk не та, что стоит нашей подменой (или подмены нет)
    bool swapPresent = false;       // в папке игры лежит exefs/sdk
    bool swapOurs = false;          // её положили мы (квитанция)
    bool moduleHasWork = true;      // для выбранной sdk модулю есть что исправлять из каталога (неизвестно — да)
    bool sdkGaps = false;           // у выбранной sdk есть пробелы, которые модуль не восполнит
    bool zbicUnpack = false;        // установка снимет ZBIC
    bool diagReport = false;        // у игры есть отчёт отладки
    bool crashReport = false;       // есть отчёт Atmosphère о падении этой игры
};

enum class Reinstall { None, Stale, ToDebug, ToPlain, OtherMod, OtherSdk };

struct GameActions {
    bool install = false;           // «Установить»
    bool uninstall = false;         // «Удалить установленное» (опасная)
    Reinstall reinstall = Reinstall::None;   // «Переустановить» и почему; None — кнопки нет
    bool sdkSwap = false;           // «Подменить sdk»
    bool sdkRemove = false;         // «Удалить подмену» (опасная)
    bool alsoSwaps = false;         // строка «Установка также подменит sdk на …»
    bool installSwapOnly = false;   // «Установить» = только подмена sdk (план «Подменить sdk»), строка «больше ничего»
    bool diagReport = false;        // «Отчёт отладки»
    bool collectLogs = false;       // «Собрать логи для отправки»
};

// Кроме подмены sdk, делать нечего: модулю нечего исправлять из каталога, ZBIC не снимается, мод не выбран.
inline bool onlySwapToDo(const GameActionsInput& s) {
    return s.sdkChosen && !s.moduleHasWork && !s.zbicUnpack && !s.modChosen;
}

// Одной подмены sdk достаточно — модулю делать нечего и пробелов, которые он не восполнит, нет.
inline bool swapIsEnough(const GameActionsInput& s) { return onlySwapToDo(s) && !s.sdkGaps; }

inline GameActions decideGameActions(const GameActionsInput& s) {
    GameActions a;
    const bool foreignSwap = s.swapPresent && !s.swapOurs;
    a.sdkRemove = s.swapPresent && s.swapOurs;
    a.sdkSwap = !s.swapPresent && swapIsEnough(s);
    if (!s.moduleInstalled) {
        a.install = true;
    } else {
        a.uninstall = true;
        if (s.receiptStale) a.reinstall = Reinstall::Stale;
        else if (s.debugChosen && !s.debugInstalled) a.reinstall = Reinstall::ToDebug;
        else if (!s.debugChosen && s.debugInstalled) a.reinstall = Reinstall::ToPlain;
        else if (s.modDiffers) a.reinstall = Reinstall::OtherMod;
        else if (s.sdkChosen && s.sdkDiffers && !foreignSwap) a.reinstall = Reinstall::OtherSdk;
    }
    a.installSwapOnly = a.install && !s.swapPresent && s.sdkDiffers && onlySwapToDo(s) && s.sdkGaps && !s.debugChosen;
    a.alsoSwaps = (a.install || a.reinstall != Reinstall::None) && s.sdkChosen && s.sdkDiffers && !foreignSwap &&
                  !a.sdkSwap && !a.installSwapOnly;
    a.diagReport = s.diagReport;
    a.collectLogs = s.diagReport || s.crashReport;
    return a;
}

}  // namespace mk::app
