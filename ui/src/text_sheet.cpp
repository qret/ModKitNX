// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — экран длинного текста. См. ui/text_sheet.hpp.
#include "ui/text_sheet.hpp"

#include <algorithm>
#include <cmath>

namespace mk::ui {

namespace {

constexpr float kStep = 80.0f;        // шаг крестовины и левого стика, пикселей
constexpr float kStickSpeed = 900.0f; // правый стик до упора, пикселей в секунду

// Прокрутка без фокуса внутри: текст не фокусируется, фокус стоит на кнопке внизу, а смещение меняют действия
// экрана (крестовина, L/R) и правый стик. ScrollingFrame borealis сам прокручивает только к виду в фокусе.
class TextScroller : public brls::ScrollingFrame {
public:
    explicit TextScroller(brls::Activity* owner) : owner_(owner) {
        setFocusable(false);
        setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    }

    void scrollBy(float dy) {
        const float limit = std::max(0.0f, getContentHeight() - getScrollingAreaHeight());
        const float to = std::clamp(getContentOffsetY() + dy, 0.0f, limit);
        if (std::fabs(to - getContentOffsetY()) > 0.5f) setContentOffsetY(to, true);
    }

    float page() { return std::max(kStep, getScrollingAreaHeight() * 0.85f); }

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
              brls::FrameContext* ctx) override {
        scrollByStick();
        brls::ScrollingFrame::draw(vg, x, y, width, height, style, ctx);
    }

private:
    void scrollByStick() {
        const auto stack = brls::Application::getActivitiesStack();
        if (stack.empty() || stack.back() != owner_) return;   // поверх открыт другой экран
        const float axis = brls::Application::getControllerState().axes[brls::RIGHT_Y];
        if (std::fabs(axis) < 0.2f) return;
        const float fps = std::max(1.0f, static_cast<float>(brls::Application::getFPS()));
        const float limit = std::max(0.0f, getContentHeight() - getScrollingAreaHeight());
        const float to = std::clamp(getContentOffsetY() + axis * kStickSpeed / fps, 0.0f, limit);
        setContentOffsetY(to, false);
    }

    brls::Activity* owner_;
};

brls::View* makeBlock(const SheetBlock& b) {
    if (!b.icon.empty()) {
        auto* sub = makeSubCard(b.tone, b.icon, b.title, 20);
        for (const auto& l : b.lines) addSubLine(sub, l, colText(), b.lineSize);
        return sub;
    }
    auto* box = makeColumn();
    box->setBackgroundColor(toneBg(b.tone));
    box->setCornerRadius(8);
    box->setPadding(12, 16, 14, 16);
    box->setMarginTop(10);
    if (!b.title.empty()) box->addView(makeLabel(b.title, 20, b.tone == Tone::Neutral ? colText() : toneText(b.tone)));
    for (const auto& l : b.lines) {
        auto* label = makeLabel(l, b.lineSize, colText());
        label->setMarginTop(b.title.empty() && l == b.lines.front() ? 0 : 4);
        box->addView(label);
    }
    return box;
}

class TextSheetActivity : public brls::Activity {
public:
    explicit TextSheetActivity(TextSheet s) : sheet_(std::move(s)) {
        if (sheet_.buttons.empty()) sheet_.buttons.push_back({tr("common/close"), &brls::BUTTONSTYLE_BORDERED, nullptr, "sheet_close"});
        if (sheet_.focus >= sheet_.buttons.size()) sheet_.focus = 0;
    }

    brls::View* createContentView() override {
        auto* root = makeColumn();

        auto* scroller = new TextScroller(this);
        scroller->setGrow(1.0f);
        auto* body = makeColumn();
        body->setId("body");
        body->setPadding(16, 50, 24, 50);
        if (!sheet_.headline.empty()) body->addView(makeLabel(sheet_.headline, 26, colText()));
        if (!sheet_.lead.empty()) {
            auto* lead = makeLabel(sheet_.lead, 18, colSub());
            lead->setMarginTop(4);
            body->addView(lead);
        }
        for (const auto& b : sheet_.blocks) body->addView(makeBlock(b));
        if (!sheet_.note.empty()) {
            auto* note = makeLabel(sheet_.note, 16, colSub());
            note->setMarginTop(14);
            body->addView(note);
        }
        scroller->setContentView(body);
        root->addView(scroller);

        // Кнопки закреплены под текстом: видны всегда, фокус не уходит в прокрутку.
        auto* bar = makeRow();
        bar->setPadding(12, 50, 14, 50);
        bar->setLineColor(colLine());
        bar->setLineTop(1);
        for (const auto& sb : sheet_.buttons) {
            auto cb = sb.onClick;
            auto* btn = makeButton(sb.label, sb.danger ? dangerStyle() : sb.style, [cb] {
                brls::Application::popActivity(brls::TransitionAnimation::FADE, [cb] {
                    if (cb) cb();
                });
            });
            if (!sb.id.empty()) btn->setId(sb.id);
            bar->addView(btn);
        }
        bar->setDefaultFocusedIndex(static_cast<int>(sheet_.focus));
        root->addView(bar);

        // Вверх/вниз крестовиной и левым стиком — прокрутка (кнопки стоят в ряд, им нужны только влево/вправо).
        root->registerAction("", brls::ControllerButton::BUTTON_NAV_UP, [scroller](brls::View*) {
            scroller->scrollBy(-kStep);
            return true;
        }, true, true);
        root->registerAction("", brls::ControllerButton::BUTTON_NAV_DOWN, [scroller](brls::View*) {
            scroller->scrollBy(kStep);
            return true;
        }, true, true);
        root->registerAction("", brls::ControllerButton::BUTTON_LB, [scroller](brls::View*) {
            scroller->scrollBy(-scroller->page());
            return true;
        }, true, true);
        root->registerAction("", brls::ControllerButton::BUTTON_RB, [scroller](brls::View*) {
            scroller->scrollBy(scroller->page());
            return true;
        }, true, true);

        auto* frame = new brls::AppletFrame(root);
        frame->setTitle(sheet_.title);
        return frame;
    }

private:
    TextSheet sheet_;
};

}  // namespace

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") != std::string::npos) out.push_back(line);
        pos = nl + 1;
    }
    return out;
}

void openTextSheet(TextSheet sheet) { brls::Application::pushActivity(new TextSheetActivity(std::move(sheet))); }

brls::Dialog* makeShortDialog(const std::string& text) {
    brls::Style style = brls::Application::getStyle();
    auto* box = makeColumn();
    box->setPadding(style["brls/dialog/paddingTopBottom"], style["brls/dialog/paddingLeftRight"],
                    style["brls/dialog/paddingTopBottom"], style["brls/dialog/paddingLeftRight"]);
    box->setJustifyContent(brls::JustifyContent::CENTER);
    auto* label = makeLabel(text, style["brls/dialog/fontSize"], colText());
    label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    box->addView(label);
    return new brls::Dialog(box);
}

}  // namespace mk::ui
