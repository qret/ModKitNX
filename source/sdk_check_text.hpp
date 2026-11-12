// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — текст «sdk-check.txt» для «Собрать логи для отправки»: что страница игры показывает про sdk —
// версия библиотек игры, sdk, что исправит модуль и полный список «не восполнит» (с техническими именами).
// Раньше этот список писался только в журнал приложения, а он по умолчанию выключен — в логах его не было.
// Шаблон по типу блока (FixesBlock из app_types.hpp): заголовок не тянет платформу, хост-тест — свои структуры.
#pragma once

#include <string>

namespace mk::app {

template <typename Forecast>
inline void appendForecast(std::string& out, const Forecast& v, const char* title) {
    out += title;
    if (!v.ok) {
        out += ": не посчитано";
        if (!v.error.empty()) out += " (" + (v.error.key.empty() ? v.error.fallback : v.error.key) + ")";
        out += "\r\n";
        return;
    }
    out += ": sdk " + (v.sdkVersion.empty() ? std::string("?") : v.sdkVersion);
    out += v.sdkKnown ? "" : " (не nnSdk)";
    out += v.swapped ? ", старше библиотек игры (подмена)" : "";
    out += "\r\n";
    // Поиск символов nn::ro (то же, что строка в «Технических подробностях»).
    if (!v.ro.verdict.empty()) {
        const std::string sv = "sdk " + (v.sdkVersion.empty() ? std::string("?") : v.sdkVersion);
        std::string gnu;
        for (const auto& m : v.ro.gnuModules) gnu += (gnu.empty() ? "" : ", ") + m;
        out += "  поиск символов nn::ro: ";
        if (v.ro.verdict == "replace_gnu")
            out += sv + " знает только старую таблицу символов (DT_HASH); у модулей игры — DT_GNU_HASH (" + gnu +
                   "); модуль подменит поиск";
        else if (v.ro.verdict == "replace")
            out += sv + " знает только старую таблицу символов (DT_HASH); у модулей игры DT_GNU_HASH нет; модуль всё "
                        "равно подменит поиск (для модулей, подгруженных позже)";
        else if (v.ro.verdict == "not_needed")
            out += sv + " знает DT_GNU_HASH — подменять поиск не нужно";
        else if (v.ro.verdict == "not_old")
            out += sv + ": поиск nn::ro не старого вида (не только DT_HASH) — модуль его не трогает";
        else if (v.ro.verdict == "no_ro")
            out += "в " + sv + " нет nn::ro::LookupModuleSymbol — подменять нечего";
        else
            out += sv + ": код nn::ro::LookupModuleSymbol не узнан — модуль поиск не трогает";
        out += " [" + v.ro.verdict + ", модулей игры " + std::to_string(v.ro.modules) + "]\r\n";
    }
    for (const auto& f : v.fixes) {
        if (f.importers.empty()) continue;   // игре не нужно
        out += "  исправит: " + f.id + " (" + f.symbol + ") — " + f.choice + ", " + f.why + "\r\n";
    }
    if (v.gaps.empty()) {
        out += "  не восполнит: нет\r\n";
        return;
    }
    out += "  не восполнит — " + std::to_string(v.gaps.size()) + ":\r\n";
    for (const auto& g : v.gaps) {
        out += "    " + g.kind + (g.weak ? " (слабый импорт)" : "") + "  " +
               (g.readable.empty() ? g.symbol : g.readable);
        if (!g.online.empty()) out += "  [онлайн: " + g.online + "]";
        out += "\r\n      " + g.symbol + "\r\n      зовут: ";
        std::string who;
        for (const auto& m : g.importers) who += (who.empty() ? "" : ", ") + m;
        out += (who.empty() ? std::string("?") : who) + "\r\n";
    }
}

template <typename Fixes>
inline std::string sdkCheckText(const Fixes& f) {
    std::string out = "\xEF\xBB\xBF";   // BOM: блокнот Windows покажет русский текст
    out += "ModKitNX — проверка sdk для этой игры (то, что показывает страница игры)\r\n";
    out += "библиотеки SDK игры: " + (f.gameOk ? (f.gameVersion.empty() ? std::string("не прочитаны") : f.gameVersion)
                                               : std::string("сторона игры не прочитана")) + "\r\n";
    out += std::string("графика: ") + (f.vulkan ? "Vulkan" : (f.opengl ? "OpenGL" : "не определена")) + "\r\n";
    if (!f.ownSdkOk) {
        out += "своя sdk игры не прочитана — пробелы не считались" +
               (f.ownSdkError.empty() ? std::string() : " (" + f.ownSdkError + ")") + "\r\n";
    }
    appendForecast(out, f.current, f.currentIsSwap ? "sdk сейчас (подменённая на карте)" : "sdk сейчас (родная)");
    return out;
}

}  // namespace mk::app
