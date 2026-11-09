// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — экран длинного текста: подробности, технические строки, «Что нового», текст отчёта. Всё, что может
// не поместиться в окно, показывается здесь, а не в brls::Dialog: диалог borealis не прокручивается, и когда
// перенесённый текст выше экрана, Label отказывается от переноса и рисует его одной строкой за краями окна.
//
// Вид: шапка с заголовком, прокручиваемая колонка (крупная строка, пояснение, блоки-подкарточки, примечание) и
// закреплённый внизу ряд кнопок (по умолчанию одна «Закрыть»; фокус — на кнопке). Прокрутка: крестовина и левый
// стик вверх/вниз, правый стик, L/R — на страницу, касание. B — назад.
#pragma once

#include <borealis.hpp>

#include <functional>
#include <string>
#include <vector>

#include "ui/common.hpp"

namespace mk::ui {

// Смысловой блок: подкарточка с заголовком и строками. Каждая строка — отдельный абзац, переносится по ширине;
// слово длиннее строки (имя функции, путь) переносится посимвольно.
struct SheetBlock {
    std::string title;                 // пусто — без строки заголовка
    std::vector<std::string> lines;
    Tone tone = Tone::Neutral;
    std::string icon;                  // значок (icon::…); пусто — без значка, строки без отступа
    float lineSize = 18;               // размер строк (сырой текст отчёта — мельче)
};

struct SheetButton {
    std::string label;
    const brls::ButtonStyle* style = &brls::BUTTONSTYLE_BORDERED;
    std::function<void()> onClick;     // вызывается после закрытия экрана; пусто — просто закрыть
    std::string id;                    // id вида (предпросмотр, проверки)
    bool danger = false;               // красная (dangerStyle), вместо style
};

struct TextSheet {
    std::string title;                 // заголовок шапки
    std::string headline;              // крупная строка вверху (необязательно)
    std::string lead;                  // серая строка под ней (необязательно)
    std::vector<SheetBlock> blocks;
    std::string note;                  // серое примечание в конце (правило, пояснение)
    std::vector<SheetButton> buttons;  // пусто — одна «Закрыть»
    size_t focus = 0;                  // какая кнопка в фокусе при открытии
};

// Открыть экран поверх текущего.
void openTextSheet(TextSheet sheet);

// Строки текста по «\n» (пустые строки отбрасываются) — для SheetBlock::lines.
std::vector<std::string> splitLines(const std::string& text);

// Короткий диалог (вопрос «Да/Нет», уведомление в одну-две фразы): текст в колонке на всю ширину окна, по
// центру, с переносом. Длинный или переменный текст — только openTextSheet.
brls::Dialog* makeShortDialog(const std::string& text);

}  // namespace mk::ui
