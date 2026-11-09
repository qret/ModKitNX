// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — экран «Отчёт отладки». Подача: вывод сверху, этапы, затем подробности.
// Отчёты — по играм: sdmc:/switch/<APP>/logs/<TID>/diag-NNNN.txt и gfx-NNNN.txt (последние 5;
// список — mk::logs::list_reports). Нет ни одного — запасной старый файл в корне карты.
#include "ui/diag_activity.hpp"

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

// Сырой текст: не больше 200 строк, чтобы экран не тормозил.
void addRaw(brls::Box* parent, const std::string& name, const std::string& text) {
    auto* card = makeCard();
    card->addView(makeLabel(trf("diag/raw_title_file", name), 18, colSub()));
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
    card->addView(makeLabel(cut, 14, colText()));
    parent->addView(card);
}

std::vector<mk::logs::ReportFile> reportsOf(const std::string& tid, const char* kind) {
    try {
        return mk::logs::list_reports(mk::plat::posixFs(), mk::plat::paths::kLogsDir, tid, kind);
    } catch (const mk::Error&) {
        return {};
    }
}

}  // namespace

bool diagReportsPresent(const std::string& tid) {
    struct stat st;
    if (!tid.empty() && (!reportsOf(tid, "diag").empty() || !reportsOf(tid, "gfx").empty())) return true;
    return ::stat(pd::kDiagPath, &st) == 0 || ::stat(pd::kGfxPath, &st) == 0;
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
    body->clearViews();
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
    const bool haveDiag = !diagPath.empty() && pd::readText(diagPath.c_str(), diagText);
    const bool haveGfx = !gfxPath.empty() && pd::readText(gfxPath.c_str(), gfxText);

    body->addView(makeLabel(tr("diag/lead"), 18, colSub()));
    const std::string dir = mk::plat::paths::kLogsDir + tid_ + "/";
    body->addView(makeLabel(trf("diag/folder", dir), 16, colSub(), true));
    if (!haveDiag && !haveGfx) {
        auto* none = makeLabel(trf("diag/none_dir", dir), 22, colWarn());
        none->setMarginTop(16);
        body->addView(none);
        return;
    }
    if (legacy) body->addView(makeLabel(tr("diag/legacy"), 17, colWarn()));

    if (haveDiag) {
        const pd::DiagSummary s = pd::parseDiag(diagText);
        auto* card = makeCard();
        card->setMarginTop(12);
        card->addView(makeSectionTitle(trf("diag/diag_title_file", diagName, fileTime(diagPath))));
        const bool works = s.verdict.key == "diag/v_works";
        card->addView(makeLabel(verdictText(s.verdict), 23, works ? colOk() : colWarn()));
        const mk::logs::ReportHead head = mk::logs::parse_report_head(diagText);
        const std::string reportTid = head.ok ? head.tid : s.tid;
        if (s.isReport && !reportTid.empty() && !tid_.empty() && reportTid.find(tid_) == std::string::npos) {
            card->addView(makeLabel(trf("diag/other_game", reportTid), 18, colWarn()));
        }
        if (s.truncated) card->addView(makeLabel(tr("diag/truncated"), 16, colSub()));
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
        card->addView(makeLabel(verdictText(g.verdict), 21, g.suspects.empty() ? colText() : colWarn()));
        addPicker(card, "diag/pick_gfx", gfxs, selGfx_);
        auto* stages = makeColumn();
        stages->setMarginTop(12);
        addStages(stages, g.stages, "diag/g_s_");
        card->addView(stages);
        body->addView(card);
    }
    if (haveDiag) addRaw(body, diagName, diagText);
    if (haveGfx) addRaw(body, gfxName, gfxText);
}

}  // namespace mk::ui
