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
