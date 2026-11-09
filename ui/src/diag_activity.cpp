// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — экран «Отчёт отладки». Подача: вывод сверху, этапы, затем подробности.
// Отчёты — по играм: sdmc:/switch/<APP>/logs/<TID>/diag-NNNN.txt и gfx-NNNN.txt (последние 5;
// список — mk::logs::list_reports). Нет ни одного — запасной старый файл в корне карты.
#include "ui/diag_activity.hpp"
#include "ui/text_sheet.hpp"

#include <sys/stat.h>

#include <ctime>

#include <mk/logs.hpp>

#include "platform/core_fs.hpp"
#include "platform/diag_report.hpp"
#include "platform/file_time.hpp"
#include "platform/paths.hpp"

namespace mk::ui {

namespace pd = mk::plat::diag;

namespace {

std::string fileTime(const std::string& path) {
    const auto t = mk::plat::fileMtime(path);
    if (!t) return "";
    const time_t tt = static_cast<time_t>(*t);
    struct tm tmv;
    gmtime_r(&tt, &tmv);   // метка FAT — «настенное» время консоли, без пересчёта поясов
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv);
    return buf;
}

std::string verdictText(const pd::Verdict& v) {
    switch (v.args.size()) {
        case 0: return tr(v.key);
        case 1: return trf(v.key, v.args[0]);
        default: return trf(v.key, v.args[0], v.args[1]);
    }
}

void addStages(brls::Box* card, const std::vector<pd::Stage>& stages, const char* prefix) {
    for (const auto& s : stages) {
        auto* row = makeRow();
        row->setMarginBottom(6);
        row->setAlignItems(brls::AlignItems::FLEX_START);
        const char* mark = s.state == pd::StageState::Ok ? "●" : s.state == pd::StageState::Fail ? "×" : "?";
        NVGcolor c = s.state == pd::StageState::Ok ? colOk() : s.state == pd::StageState::Fail ? colBad() : colSub();
        auto* m = makeLabel(mark, 22, c, true);
        m->setWidth(40);
        row->addView(m);
        auto* col = makeColumn();
        col->setGrow(1.0f);
        col->addView(makeLabel(tr(std::string(prefix) + s.code), 19, colText()));
        if (!s.detail.empty()) col->addView(makeLabel(s.detail, 15, colSub()));
        row->addView(col);
        card->addView(row);
    }
}

// Сырой текст — кнопкой на экран длинного текста (там он прокручивается сам, без фокуса): на этом экране фокус
// стоит на кнопке «Назад» внизу, и длинный текст выше неё с крестовины было бы не пролистать. Не больше 200 строк,
// чтобы экран не тормозил.
void addRaw(brls::Box* parent, const std::string& name, const std::string& text) {
    std::string cut;
    int lines = 0;
    size_t pos = 0;
    while (pos < text.size() && lines < 200) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        cut.append(text, pos, nl - pos + (nl < text.size() ? 1 : 0));
        pos = nl + 1;
        ++lines;
    }
    if (pos < text.size()) cut += "\n" + tr("diag/raw_cut");
    TextSheet s;
    s.title = tr("diag/title");
    s.headline = trf("diag/raw_title_file", name);
    SheetBlock blk;
    blk.lines = {cut};
    blk.lineSize = 15;
    s.blocks.push_back(std::move(blk));
    auto* btn = makeButton(trf("diag/raw_title_file", name), &brls::BUTTONSTYLE_BORDERED, [s] { openTextSheet(s); });
    btn->setId("diag_raw_btn");
    btn->setHeight(48);
    btn->setMarginBottom(10);
    parent->addView(btn);
}

// Старый отчёт (корень карты) от этой игры: первая строка нового вида с её TitleID или TitleID в тексте.
bool legacyIsFor(const std::string& text, const std::string& tid) {
    const mk::logs::ReportHead head = mk::logs::parse_report_head(text);
    return (head.ok && head.tid.find(tid) != std::string::npos) || mk::logs::legacy_report_mentions(text, tid);
}

std::vector<mk::logs::ReportFile> reportsOf(const std::string& tid, const char* kind) {
    try {
        return mk::logs::list_reports(mk::plat::posixFs(), mk::plat::paths::kLogsDir, tid, kind);
    } catch (const mk::Error&) {
        return {};
    }
}

}  // namespace

ReportsInfo findReports(const std::string& tid) {
    ReportsInfo r;
    if (tid.empty()) return r;
    std::optional<int64_t> last;
    for (const char* kind : {"diag", "gfx"}) {
        const auto files = reportsOf(tid, kind);
        if (files.empty()) continue;
        r.diag = true;
        const auto t = mk::plat::fileMtime(files.front().path);   // первый — последний по номеру
        if (t && (!last || *t > *last)) last = t;
    }
    if (!r.diag) {
        // Старый отчёт в корне карты — только если он от этой игры.
        for (const char* p : {pd::kDiagPath, pd::kGfxPath}) {
            std::string text;
            if (!pd::readText(p, text, 64 * 1024)) continue;
            if (legacyIsFor(text, tid)) {
                r.diag = true;
                const auto t = mk::plat::fileMtime(p);
                if (t && (!last || *t > *last)) last = t;
            }
        }
    }
    if (last) {
        const time_t tt = static_cast<time_t>(*last);
        struct tm tmv;
        gmtime_r(&tt, &tmv);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv);
        r.lastWhen = buf;
    }
    // Отчёт Atmosphère о падении: только список имён (<время>_<TID>.log), файлы не читаются.
    try {
        for (const auto& e : mk::plat::posixFs().list_dir(mk::plat::paths::kCrashReportsDir))
            if (!e.is_dir && mk::logs::crash_tid_from_name(e.name) == tid) {
                r.crash = true;
                break;
            }
    } catch (const mk::Error&) {
    }
    return r;
}

DiagActivity::DiagActivity(std::string gameTid) : tid_(std::move(gameTid)) {}

DiagActivity::~DiagActivity() { *alive_ = false; }

void DiagActivity::onContentAvailable() {
    setFrameTitle(this, "diag/title");
    registerAction(tr("hint/refresh"), brls::ControllerButton::BUTTON_Y, [this](brls::View*) {
        load();
        return true;
    });
    load();
}

// Выбор отчёта одного вида: последние (до 5) прямо под заголовком, отмечен показанный.
void DiagActivity::addPicker(brls::Box* parent, const char* titleKey, const std::vector<mk::logs::ReportFile>& files,
                             int& sel) {
    if (files.size() < 2) return;
    auto* q = makeLabel(tr(titleKey), 19, colText());
    q->setMarginTop(10);
    q->setMarginBottom(4);
    parent->addView(q);
    for (size_t i = 0; i < files.size(); ++i) {
        const std::string when = fileTime(files[i].path);
        auto* r = new ChoiceRow(files[i].name, when, i == 0 ? tr("diag/latest") : "");
        r->setChecked(static_cast<int>(i) == sel);
        if (static_cast<int>(i) == sel && !pickFocus_) pickFocus_ = r;
        const int idx = static_cast<int>(i);
        int* target = &sel;
        r->onPick([this, target, idx] {
            *target = idx;
            load();
        });
        parent->addView(r);
    }
}

void DiagActivity::load() {
    // Выбор отчёта перестраивает экран, а фокус стоит на строке выбора: сначала фокус — на само тело (без рамки),
    // потом очистка; иначе указатель фокуса остался бы на удалённой строке.
    body->setFocusable(true);
    body->setHideHighlight(true);
    brls::Application::giveFocus(body);
    body->clearViews();
    body->setFocusable(false);
    pickFocus_ = nullptr;
    const auto diags = reportsOf(tid_, "diag");
    const auto gfxs = reportsOf(tid_, "gfx");
    if (selDiag_ >= static_cast<int>(diags.size())) selDiag_ = 0;
    if (selGfx_ >= static_cast<int>(gfxs.size())) selGfx_ = 0;

    // Какие файлы показать: выбранные из папки игры; нет ни одного — старые файлы в корне карты.
    std::string diagPath, gfxPath, diagName, gfxName;
    bool legacy = false;
    if (!diags.empty() || !gfxs.empty()) {
        if (!diags.empty()) diagPath = diags[selDiag_].path, diagName = diags[selDiag_].name;
        if (!gfxs.empty()) gfxPath = gfxs[selGfx_].path, gfxName = gfxs[selGfx_].name;
    } else {
        legacy = true;
        diagPath = pd::kDiagPath;
        gfxPath = pd::kGfxPath;
        diagName = "nxredirect-diag.txt";
        gfxName = "nxredirect-gfx.txt";
    }
    std::string diagText, gfxText;
    bool haveDiag = !diagPath.empty() && pd::readText(diagPath.c_str(), diagText);
    bool haveGfx = !gfxPath.empty() && pd::readText(gfxPath.c_str(), gfxText);
    // Старый отчёт из корня карты — только если он от этой игры (как кнопка на странице игры, findReports).
    if (legacy && !tid_.empty()) {
        if (haveDiag && !legacyIsFor(diagText, tid_)) haveDiag = false;
        if (haveGfx && !legacyIsFor(gfxText, tid_)) haveGfx = false;
    }

    body->addView(makeLabel(tr("diag/lead"), 18, colSub()));
    const std::string dir = mk::plat::paths::kLogsDir + tid_ + "/";
    body->addView(makeLabel(trf("diag/folder", dir), 16, colSub(), true));
    if (!haveDiag && !haveGfx) {
        auto* none = makeSubCard(Tone::Warn, icon::kWarn, trf("diag/none_dir", dir), 21);
        none->setMarginTop(16);
        body->addView(none);
        addBack();
        return;
    }
    if (legacy) body->addView(makeSubCard(Tone::Warn, icon::kWarn, tr("diag/legacy"), 18));

    if (haveDiag) {
        const pd::DiagSummary s = pd::parseDiag(diagText);
        auto* card = makeCard();
        card->setMarginTop(12);
        card->addView(makeSectionTitle(trf("diag/diag_title_file", diagName, fileTime(diagPath))));
        // Вывод — подкарточкой: работает — зелёная, иначе жёлтая; оговорки к отчёту — в ней же.
        const Tone tone = s.verdict.key == "diag/v_works" ? Tone::Ok : Tone::Warn;
        auto* verdict = makeSubCard(tone, toneIcon(tone), verdictText(s.verdict), 21);
        verdict->setMarginTop(4);
        const mk::logs::ReportHead head = mk::logs::parse_report_head(diagText);
        const std::string reportTid = head.ok ? head.tid : s.tid;
        if (s.isReport && !reportTid.empty() && !tid_.empty() && reportTid.find(tid_) == std::string::npos) {
            addSubLine(verdict, trf("diag/other_game", reportTid), colWarn(), 18);
        }
        if (s.truncated) addSubLine(verdict, tr("diag/truncated"), colSub(), 16);
        card->addView(verdict);
        addPicker(card, "diag/pick_diag", diags, selDiag_);
        auto* stages = makeColumn();
        stages->setMarginTop(12);
        addStages(stages, s.stages, "diag/s_");
        card->addView(stages);
        body->addView(card);
    }
    if (haveGfx) {
        const pd::GfxSummary g = pd::parseGfx(gfxText);
        auto* card = makeCard();
        card->addView(makeSectionTitle(trf("diag/gfx_title_file", gfxName, fileTime(gfxPath))));
        const Tone tone = g.suspects.empty() ? Tone::Neutral : Tone::Warn;
        auto* verdict = makeSubCard(tone, toneIcon(tone), verdictText(g.verdict), 20);
        verdict->setMarginTop(4);
        card->addView(verdict);
        addPicker(card, "diag/pick_gfx", gfxs, selGfx_);
        auto* stages = makeColumn();
        stages->setMarginTop(12);
        addStages(stages, g.stages, "diag/g_s_");
        card->addView(stages);
        body->addView(card);
    }
    if (haveDiag) addRaw(body, diagName, diagText);
    if (haveGfx) addRaw(body, gfxName, gfxText);
    // Выбирать нечего (по одному отчёту каждого вида) — кнопка «Назад», чтобы фокус не падал на пустую рамку.
    if (pickFocus_) brls::Application::giveFocus(pickFocus_);
    else addBack();
}

// Настоящая кнопка «Назад» с фокусом: без неё фокус уходит на рамку прокрутки (пустая рамка без текста).
void DiagActivity::addBack() {
    auto* back = makeButton(tr("common/back"), &brls::BUTTONSTYLE_BORDERED,
                            [] { brls::Application::popActivity(brls::TransitionAnimation::FADE); });
    back->setId("diag_back");
    back->setMarginTop(8);
    back->setMarginBottom(30);
    body->addView(back);
    brls::Application::giveFocus(back);
}

}  // namespace mk::ui
