// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — экран игры. См. ui/game_activity.hpp.
#include "ui/game_activity.hpp"

#include <cstdio>

#include "app_model.hpp"
#include "platform/paths.hpp"
#include "ui/diag_activity.hpp"
#include "ui/logs_activity.hpp"
#include "ui/no_keys_activity.hpp"
#include "ui/operation_activity.hpp"
#include "ui/sdk_harvest_activity.hpp"

namespace mk::ui {

using mk::app::GameDetails;
using mk::app::GameRow;
using mk::app::OpKind;
using mk::plat::SdkNeed;

namespace {

std::string orUnknown(const std::string& s) { return s.empty() ? tr("common/unknown") : s; }

std::string hex32(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%X", v);
    return b;
}

std::string updateNumber(uint32_t v) { return std::to_string(v >> 16); }

}  // namespace

GameActivity::GameActivity(const GameRow& row, std::function<void(const GameRow&)> onRowChanged)
    : row_(row), onRowChanged_(std::move(onRowChanged)) {}

GameActivity::~GameActivity() { *alive_ = false; }

void GameActivity::onContentAvailable() {
    if (auto* frame = dynamic_cast<brls::AppletFrame*>(getContentView())) frame->setTitle(row_.name);
    anchor_ = new brls::Box();
    anchor_->setFocusable(true);
    anchor_->setHideHighlight(true);
    anchor_->setHideClickAnimation(true);
    anchor_->setWidth(1);
    anchor_->setHeight(1);
    body->addView(anchor_);
    reload();
}

void GameActivity::clearBody() {
    if (anchor_) brls::Application::giveFocus(anchor_);
    std::vector<brls::View*> kids = body->getChildren();
    for (brls::View* v : kids)
        if (v != anchor_) body->removeView(v, true);
}

void GameActivity::reload() {
    loaded_ = false;
    // Наверх: после операции прокрутка оставалась внизу (у «Установить»), и экран с одной строкой «Загрузка…»
    // наверху выглядел чёрным.
    scroll->setContentOffsetY(0, false);
    clearBody();
    body->addView(makeLabel(tr("game/loading"), 24, colSub()));
    AliveToken alive = alive_;
    const GameRow row = row_;
    brls::async([this, alive, row]() {
        auto& m = mk::app::Model::instance();
        GameDetails d = m.loadDetails(row);
        std::vector<mk::app::SdkLine> sdk = m.sdkList(false);
        ReportsInfo reports = findReports(row.tid);   // списки файлов — тоже в фоне
        brls::sync([this, alive, d = std::move(d), sdk = std::move(sdk), reports]() mutable {
            if (!*alive) return;
            d_ = std::move(d);
            sdk_ = std::move(sdk);
            reports_ = reports;
            loaded_ = true;
            // Выбор по умолчанию: рекомендованная sdk, если подмена нужна; мод — тот, что стоит (квитанция), иначе
            // «без мода».
            sdkChoice_ = (d_.sdk.advice.need == SdkNeed::Needed) ? d_.sdk.advice.recommendedIndex : -1;
            modChoice_ = -1;
            for (size_t i = 0; i < d_.mods.size(); ++i)
                if (!d_.mods[i].inContents && !d_.installedModPath.empty() && d_.mods[i].path == d_.installedModPath)
                    modChoice_ = static_cast<int>(i);
            // Флажок «Отладочная сборка» отражает то, что стоит сейчас (квитанция); без нашей установки — выключен.
            debug_ = d_.ourInstalled && d_.debugInstalled;
            build();
            if (onRowChanged_) onRowChanged_(d_.row);
        });
    });
}

void GameActivity::build() {
    clearBody();
    sdkRows_.clear();
    modRows_.clear();
    fixesBox_ = nullptr;
    sdkButtons_ = nullptr;
    actionsBox_ = nullptr;
    firstFocus_ = nullptr;
    buildHeader(body);
    buildState(body);
    buildSdk(body);
    buildFixes(body);
    buildMods(body);
    buildActions(body);
    refreshActions();
    if (firstFocus_) brls::Application::giveFocus(firstFocus_);
}

void GameActivity::buildHeader(brls::Box* parent) {
    auto* row = makeRow();
    row->setMarginBottom(18);
    auto* img = new brls::Image();
    img->setWidth(112);
    img->setHeight(112);
    img->setCornerRadius(12);
    img->setMarginRight(24);
    if (!row_.icon.empty()) img->setImageFromMem(row_.icon.data(), static_cast<int>(row_.icon.size()));
    else img->setImageFromRes("img/cover_blank.png");
    row->addView(img);

    auto* col = makeColumn();
    col->setGrow(1.0f);
    col->addView(makeLabel(row_.name, 30, colText()));
    col->addView(makeLabel(trf("games/version_tid", orUnknown(row_.version), row_.tid), 18, colSub(), true));
    auto* badges = makeRow();
    badges->setMarginTop(8);
    if (d_.ourInstalled) badges->addView(makeBadge(tr("games/badge_ours"), colOk()));
    if (d_.sdk.advice.need == SdkNeed::Needed) badges->addView(makeBadge(tr("games/badge_sdk_needed"), colWarn()));
    if (d_.sdk.advice.need == SdkNeed::SwappedOk) badges->addView(makeBadge(tr("games/badge_sdk_swapped"), colOk()));
    col->addView(badges);
    row->addView(col);
    parent->addView(row);
}

void GameActivity::buildState(brls::Box* parent) {
    auto* card = makeCard();
    card->addView(makeSectionTitle(tr("state/title")));
    card->addView(makeInfoLine(tr("state/version"), orUnknown(row_.version), colText()));
    card->addView(makeInfoLine(tr("state/update"),
                               d_.patchVersion ? trf("state/update_yes", updateNumber(d_.patchVersion))
                                               : tr("state/update_no"),
                               colText()));
    std::string reqFw;
    if (d_.requiredFwBySdkMajor > 0)
        reqFw = trf("state/required_fw_by_sdk", std::to_string(d_.requiredFwBySdkMajor), d_.sdk.ncaSdk);
    else
        reqFw = d_.rsvKnown ? trf("state/required_fw_value", d_.requiredFirmware) : tr("common/unknown");
    card->addView(makeInfoLine(tr("state/required_fw"), reqFw, colText()));

    // Как прочитан код игры.
    std::string via;
    NVGcolor viaColor = colOk();
    switch (d_.exefs.via) {
        case mk::plat::ExefsVia::CodeWithId: via = tr("state/via_code_with_id"); break;
        case mk::plat::ExefsVia::CodeWithPatch: via = tr("state/via_code_with_patch"); break;
        case mk::plat::ExefsVia::RawNcaWithKeys: via = tr("state/via_raw"); break;
        default:
            via = d_.programFound ? trf("state/via_failed", hex32(d_.exefs.rcWithId), hex32(d_.exefs.rcWithPatch))
                                  : tr("state/no_program");
            viaColor = colBad();
            break;
    }
    card->addView(makeInfoLine(tr("state/code_read"), via, viaColor));

    // ZBIC: снимать или нет — решает правило (прошивка + Atmosphère), вопроса пользователю нет.
    {
        const std::string ams = orUnknown(d_.sdk.atmosphere);
        std::string policy;
        switch (d_.zbicNeed) {
            case mk::plat::ZbicNeed::NotNeededHos22: policy = tr("state/zbic_not_needed_hos22"); break;
            case mk::plat::ZbicNeed::NotNeededAms112: policy = trf("state/zbic_not_needed_ams", ams); break;
            case mk::plat::ZbicNeed::Needed: policy = trf("state/zbic_will_unpack", ams); break;
            default: policy = tr("state/zbic_will_unpack_unknown"); break;
        }
        const bool checked = d_.exefsOpened && d_.coreOk;
        std::string text;
        NVGcolor color = colText();
        if (checked && !d_.zbic) {
            text = tr("state/zbic_no");
        } else if (checked && d_.zbicUnpackedOurs) {
            // Несжатые файлы уже положены нами (квитанция, файлы на карте) — снимать больше нечего.
            text = trf("state/zbic_yes", std::to_string(d_.zbicFiles.size())) + " — " + tr("state/zbic_unpacked_ours");
            color = colOk();
        } else {
            text = (checked ? trf("state/zbic_yes", std::to_string(d_.zbicFiles.size())) + " — "
                            : tr("state/zbic_unknown") + " — ") +
                   policy;
            color = mk::plat::zbicUnpackNeeded(d_.zbicNeed) ? colWarn() : colOk();
        }
        card->addView(makeInfoLine(tr("state/zbic"), text, color));
    }
    card->addView(makeInfoLine(tr("state/ours"),
                               d_.ourInstalled ? trf("state/ours_yes", orUnknown(d_.slot), orUnknown(d_.installedAt))
                                               : tr("state/ours_no"),
                               d_.ourInstalled ? colOk() : colText()));
    if (d_.ourInstalled) {
        // Вид сборки по квитанции: обычная / отладочная (поля diag, diag_gfx, module_variant).
        card->addView(makeInfoLine(tr("state/build_kind"),
                                   d_.debugInstalled ? tr("state/build_debug") : tr("state/build_plain"),
                                   d_.debugInstalled ? colWarn() : colOk()));
    }
    for (const auto& n : d_.notes) {
        auto* l = makeLabel(trText(n), 17, colSub());
        l->setMarginTop(4);
        card->addView(l);
    }

    // Что требует внимания — отдельной жёлтой подкарточкой под таблицей (таблица остаётся таблицей).
    brls::Box* issues = nullptr;
    auto issue = [&issues, card](const std::string& text, NVGcolor color, float size) {
        if (!issues) {
            issues = makeSubCard(Tone::Warn, icon::kWarn, tr("state/issues_title"));
            card->addView(issues);
        }
        addSubLine(issues, text, color, size);
    };
    if (d_.receiptStale) issue(tr("state/stale"), colText(), 18);
    if (d_.programIdMismatch) issue(tr("state/program_mismatch"), colText(), 18);
    if (!d_.coreOk && !d_.coreError.empty()) issue(trf("state/core_failed", trText(d_.coreError)), colSub(), 17);

    if (d_.exefs.needKeys) {
        issue(tr("state/need_keys"), colText(), 19);
        auto* b = makeButton(tr("state/why_keys"), &brls::BUTTONSTYLE_BORDERED,
                             [] { brls::Application::pushActivity(new NoKeysActivity()); });
        b->setId("why_keys_btn");
        b->setHeight(48);
        addSubView(issues, b);
        b->setMarginTop(10);
        if (!firstFocus_) firstFocus_ = b;
    } else if (d_.exefs.rawNotImplemented) {
        issue(tr("state/raw_not_impl"), colText(), 18);
    } else if (!d_.exefsOpened && d_.exefs.rawTried) {
        // Путь 3 (сырой NCA) не прошёл: какой шаг и код — для пробы на консоли; подробности — «Собрать логи».
        std::string step = d_.exefs.rawStep.empty() ? std::string("mount") : d_.exefs.rawStep;
        for (auto& c : step)
            if (c == '-') c = '_';
        std::string code = d_.exefs.rawCore;
        if (d_.exefs.rcRaw) code += (code.empty() ? "" : " ") + hex32(d_.exefs.rcRaw);
        if (code.empty()) code = "-";
        issue(trf("state/raw_failed", tr("state/raw_step_" + step), code), colText(), 18);
        if (d_.exefs.rawPersonalized) issue(tr("state/raw_personalized"), colSub(), 17);
        issue(tr("state/raw_send_logs"), colSub(), 17);
    }
    parent->addView(card);
}

void GameActivity::buildSdk(brls::Box* parent) {
    const auto& s = d_.sdk;
    const auto& a = s.advice;
    auto* card = makeCard();
    card->setId("sdk_card");   // по нему предпросмотр прокручивает к блоку
    card->addView(makeSectionTitle(tr("sdk/title")));

    // (1) что требует игра
    card->addView(makeInfoLine(tr("sdk/game_nca"), orUnknown(s.ncaSdk), colText()));
    card->addView(makeInfoLine(tr("sdk/game_exefs"), s.exefsSdk.empty() ? tr("sdk/not_read") : s.exefsSdk, colText()));
    // (2) что у игры сейчас на консоли
    std::string now;
    NVGcolor nowColor = colText();
    if (!s.swapPresent) {
        now = tr("sdk/now_native");
    } else if (!s.swapIsNnSdk) {
        now = tr("sdk/now_unknown_file");
        nowColor = colWarn();
    } else if (s.swapOurs) {
        now = trf("sdk/now_ours", orUnknown(s.swapVersion));
        nowColor = colOk();
    } else {
        now = trf("sdk/now_foreign", orUnknown(s.swapVersion));
        nowColor = colWarn();
    }
    card->addView(makeInfoLine(tr("sdk/now"), now, nowColor));
    // (3) прошивка и Atmosphère
    card->addView(makeInfoLine(tr("sdk/firmware"), orUnknown(s.firmware), colText()));
    card->addView(makeInfoLine(tr("sdk/atmosphere"), orUnknown(s.atmosphere), colText()));

    // (4) вывод простыми словами
    const std::string game = a.gameNeeds.valid ? a.gameNeeds.toString() : tr("common/unknown");
    const std::string fw = orUnknown(s.firmware);
    const bool haveRec = a.recommendedIndex >= 0 && a.recommendedIndex < static_cast<int>(sdk_.size());
    std::string verdict;
    Tone vTone = Tone::Neutral;
    switch (a.need) {
        case SdkNeed::NotNeeded:
            verdict = trf("sdk/v_not_needed", game, fw);
            vTone = Tone::Ok;
            break;
        case SdkNeed::Needed:
            vTone = Tone::Warn;
            if (haveRec) {
                verdict = trf("sdk/v_needed", game, fw, sdk_[a.recommendedIndex].version);
            } else if (a.bestForFirmware.valid) {
                verdict = trf("sdk/v_needed_none", game, fw, a.bestForFirmware.toString(), mk::plat::paths::kSdkDirOwn);
            } else {
                verdict = trf("sdk/v_needed_none_any", game, fw, mk::plat::paths::kSdkDirOwn);
            }
            break;
        case SdkNeed::SwappedOk:
            verdict = trf("sdk/v_swapped_ok", orUnknown(s.swapVersion), fw);
            vTone = Tone::Ok;
            break;
        case SdkNeed::SwappedTooNew:
            verdict = trf("sdk/v_swapped_too_new", orUnknown(s.swapVersion), fw);
            vTone = Tone::Bad;
            break;
        case SdkNeed::SwappedUnknown:
            verdict = tr("sdk/v_swapped_unknown");
            vTone = Tone::Warn;
            break;
        default:
            verdict = tr("sdk/v_unknown");
            vTone = Tone::Neutral;
            break;
    }
    // Вывод — подкарточкой по тону; если подмена нужна, а подходящей sdk нет, в ней же — «Найти sdk в других играх».
    auto* v = makeSubCard(vTone, toneIcon(vTone), verdict, 21);
    v->setMarginTop(12);
    card->addView(v);

    // Подмена нужна, а подходящей sdk в папках нет — взять её из установленных игр (ничего не скачивается).
    if (a.need == SdkNeed::Needed && !haveRec) {
        addSubLine(v, trk("sdkscan/explain"), colText(), 18);
        const std::string need = a.bestForFirmware.valid ? std::to_string(a.bestForFirmware.major) + ".x"
                                                         : (s.firmware.empty() ? tr("common/unknown") : s.firmware);
        auto* find = makeButton(trk("sdkscan/btn"), &brls::BUTTONSTYLE_PRIMARY, [this, need] {
            AliveToken alive = alive_;
            brls::Application::pushActivity(new SdkHarvestActivity(row_.name, need, [this, alive] {
                if (*alive) reload();
            }));
        });
        find->setId("harvest_btn");
        find->setHeight(48);
        addSubView(v, find);
        find->setMarginTop(10);
    }

    // Выбор sdk — прямо под вопросом.
    auto* q = makeLabel(tr("sdk/question"), 22, colText());
    q->setMarginTop(16);
    q->setMarginBottom(8);
    card->addView(q);
    if (sdk_.empty()) {
        card->addView(makeLabel(trf("sdk/none_found", mk::plat::paths::kSdkDirOwn, mk::plat::paths::kSdkDirDbi), 18,
                                colSub()));
    } else {
        auto refresh = [this]() {
            for (size_t i = 0; i < sdkRows_.size(); ++i) sdkRows_[i]->setChecked(static_cast<int>(i) - 1 == sdkChoice_);
            refreshFixes();     // блок «Что исправит модуль» — для выбранной sdk
            refreshActions();   // «Подменить sdk», «Переустановить», «также подменит» зависят от выбора
        };
        auto* none = new ChoiceRow(tr("sdk/choice_none"), "", "");
        none->onPick([this, refresh] {
            sdkChoice_ = -1;
            refresh();
        });
        sdkRows_.push_back(none);
        card->addView(none);
        for (size_t i = 0; i < sdk_.size(); ++i) {
            const auto& l = sdk_[i];
            std::string sub = l.inOwn && l.inDbi ? tr("sdk/choice_sub_both")
                              : l.inDbi           ? tr("sdk/choice_sub_dbi")
                                                  : tr("sdk/choice_sub_own");
            if (static_cast<int>(i) == a.recommendedIndex) sub = tr("sdk/recommended") + " · " + sub;
            if (l.sameVersionOtherBuild) sub += " · " + tr("sdk/other_build");
            if (!l.fromGame.empty()) sub += " · " + trkv("sdkscan/from_game", {l.fromGame});
            auto* r = new ChoiceRow(l.fileName, trf("sdk/choice_right", l.version), sub);
            const int idx = static_cast<int>(i);
            r->onPick([this, refresh, idx] {
                sdkChoice_ = idx;
                refresh();
            });
            sdkRows_.push_back(r);
            card->addView(r);
        }
        refresh();
    }

    // «Подменить sdk» / «Удалить подмену» — в sdkButtons_ (решает decideGameActions, перестраивается при выборе);
    // «Подробности» — всегда.
    auto* buttons = makeRow();
    buttons->setMarginTop(12);
    sdkButtons_ = makeRow();
    buttons->addView(sdkButtons_);
    buttons->addView(makeButton(tr("sdk/details"), &brls::BUTTONSTYLE_BORDERLESS, [this] {
        std::string text = tr("sdk/details_head") + "\n";
        for (const auto& l : sdk_) {
            text += "• " + l.fileName + " — sdk " + l.version + "\n";
            for (const auto& p : l.allPaths) text += "    " + p + "\n";
        }
        text += "\n" + tr("sdk/details_rule");
        auto* dlg = new brls::Dialog(text);
        dlg->addButton(tr("common/ok"), [] {});
        dlg->open();
    }));
    card->addView(buttons);
    parent->addView(card);
}

void GameActivity::buildFixes(brls::Box* parent) {
    auto* card = makeCard();
    card->setId("fixes_card");   // по нему предпросмотр прокручивает к блоку
    // Заголовок с версией sdk — внутри fixesBox_: меняется вместе с выбором sdk.
    fixesBox_ = makeColumn();
    card->addView(fixesBox_);
    parent->addView(card);
    refreshFixes();
}

// Блок «Что исправит модуль» — подкарточками по смыслу: что модуль исправит (зелёная), где не вмешивается,
// чего не восполнит (жёлтая), совет (синяя). Технические строки (символ, импорт, решение) и правило Vulkan —
// в «Технических подробностях».
void GameActivity::refreshFixes() {
    if (!fixesBox_) return;
    fixesBox_->clearViews();
    const auto& b = d_.fixes;
    auto plainTitle = [this] { fixesBox_->addView(makeSectionTitle(tr("fixes/title"))); };
    if (!b.gameOk) {
        plainTitle();
        fixesBox_->addView(makeLabel(b.error.empty() ? tr("fixes/no_game") : trf("fixes/failed", trText(b.error)), 19, colSub()));
        return;
    }
    // Какая sdk будет у игры: выбранная для подмены, иначе текущая (подменённая на карте или родная).
    const mk::app::FixForecastView* f = nullptr;
    std::string src;
    bool forSwap = false;
    if (sdkChoice_ >= 0 && sdkChoice_ < static_cast<int>(b.bySdk.size())) {
        f = &b.bySdk[sdkChoice_];
        src = "fixes/src_swap";
        forSwap = true;
    } else if (b.current.ok || !b.current.error.empty()) {
        f = &b.current;
        src = b.currentIsSwap ? "fixes/src_current_swap" : "fixes/src_current_native";
    }
    if (!f || (!f->ok && f->error.empty())) {
        plainTitle();
        fixesBox_->addView(makeLabel(tr("fixes/pick_sdk"), 19, colSub()));
        return;
    }
    if (!f->ok) {
        plainTitle();
        fixesBox_->addView(makeLabel(trf("fixes/failed", trText(f->error)), 19, colWarn()));
        return;
    }
    const std::string ver = f->sdkVersion.empty() ? tr("common/unknown") : f->sdkVersion;
    auto* title = makeSectionTitle(tr("fixes/title") + " · sdk " + ver);
    title->setMarginBottom(2);
    fixesBox_->addView(title);
    fixesBox_->addView(makeLabel(tr(src), 17, colSub()));

    // Человеческие названия и пояснения — только для записей, известных этой версии; новая запись каталога
    // показывается своим id, пока к ней не добавят строки.
    static const char* kNamed[] = {"hash_memory", "ldn_set_protocol", "nvrm_query"};
    auto nameOf = [](const std::string& id) {
        for (const char* n : kNamed)
            if (id == n) return tr("fixes/name_" + id);
        return id;
    };

    // Разбор по смыслу: исправит своей / не вмешивается (передаст настоящей, Vulkan, родная пустышка…) / игре не нужно.
    std::vector<size_t> own, pass;
    std::string unused;
    int needed = 0;
    bool passWarn = false;
    for (size_t i = 0; i < f->fixes.size(); ++i) {
        const auto& l = f->fixes[i];
        if (l.importers.empty()) {
            unused += (unused.empty() ? "" : ", ") + nameOf(l.id);
            continue;
        }
        ++needed;
        if (l.choice == "own") {
            own.push_back(i);
        } else {
            pass.push_back(i);
            if (l.why == "StubNoVersion" || l.why == "NoSdkDefined") passWarn = true;
        }
    }

    // Зелёная: что модуль исправит.
    {
        auto* sc = makeSubCard(Tone::Ok, icon::kOk,
                               own.empty() ? tr("fixes/summary_none")
                                           : trf("fixes/sc_fixes", std::to_string(own.size()), std::to_string(needed)),
                               own.empty() ? 19 : 21);
        for (size_t i : own) {
            const auto& l = f->fixes[i];
            std::string act = (l.why == "StubOlderSdk") ? tr("fixes/act_stub_own") : tr("fixes/act_add");
            std::string note;
            if (l.id == "nvrm_query" && l.why == "StubOlderSdk") note = tr("fixes/note_nvrm_query");
            if (l.id == "ldn_set_protocol") note = tr("fixes/note_ldn_set_protocol");
            if (!note.empty()) act += " (" + note + ")";
            addSubLine(sc, nameOf(l.id) + " — " + act, colText(), 19);
        }
        fixesBox_->addView(sc);
    }

    // Нужно игре, но модуль не вмешивается: передаст настоящей, Vulkan, пустышка без версии…
    if (!pass.empty()) {
        auto* sc = makeSubCard(passWarn ? Tone::Warn : Tone::Neutral, passWarn ? icon::kWarn : icon::kInfo,
                               trf("fixes/sc_pass", std::to_string(pass.size())), 19);
        for (size_t i : pass) {
            const auto& l = f->fixes[i];
            std::string state = tr("fixes/st_proxy");
            NVGcolor color = colText();
            if (l.why == "StubVulkan") {
                state = tr("fixes/st_vulkan");
            } else if (l.why == "StubNative") {
                state = tr("fixes/st_native_stub");
            } else if (l.why == "StubNoVersion") {
                state = tr("fixes/st_no_version");
                color = colWarn();
            } else if (l.why == "NoSdkDefined") {
                state = tr("fixes/st_unknown_sdk");
                color = colWarn();
            }
            addSubLine(sc, nameOf(l.id) + " — " + state, color, 18);
        }
        fixesBox_->addView(sc);
    }

    // Игре не нужно — одной серой строкой.
    if (!unused.empty()) {
        auto* u = makeLabel(trf("fixes/unused_line", unused), 17, colSub());
        u->setMarginTop(10);
        fixesBox_->addView(u);
    }

    // Чего модуль не восполнит — только для sdk, выбранной для подмены; пробелов нет — ничего не показываем.
    if (forSwap) addGaps(*f);

    // Технические подробности: символ, импорт, вид, решение — и правило Vulkan.
    std::string text = tr("fixes/title") + " · sdk " + ver + "\n";
    for (const auto& l : f->fixes) {
        std::string who;
        for (const auto& m : l.importers) who += (who.empty() ? "" : ", ") + m;
        text += "\n• " + nameOf(l.id) + "\n    " +
                trf("fixes/tech", l.symbol, who.empty() ? "—" : who, l.kind, l.scope, l.choice, l.why) + "\n";
    }
    text += "\n" + (b.vulkan ? trf("fixes/vk_rule_vulkan", b.vulkanModule)
                             : (b.opengl ? tr("fixes/vk_rule_gl") : tr("fixes/vk_rule_other")));
    auto* row = makeRow();
    row->setMarginTop(10);
    auto* tech = makeButton(tr("fixes/tech_btn"), &brls::BUTTONSTYLE_BORDERLESS, [text] {
        auto* dlg = new brls::Dialog(text);
        dlg->addButton(tr("common/ok"), [] {});
        dlg->open();
    });
    tech->setId("fixes_tech_btn");
    row->addView(tech);
    fixesBox_->addView(row);
}

void GameActivity::addGaps(const mk::app::FixForecastView& f) {
    const std::string ver = f.sdkVersion.empty() ? tr("common/unknown") : f.sdkVersion;
    if (!d_.fixes.ownSdkOk) {
        fixesBox_->addView(makeSubCard(Tone::Warn, icon::kWarn, tr("fixes/gaps_unchecked"), 18));
        return;
    }
    if (f.gaps.empty()) return;
    std::vector<std::string> strong, weak;
    for (const auto& g : f.gaps) (g.weak ? weak : strong).push_back(g.symbol);
    constexpr size_t kShow = 5;   // остальные — в «Подробностях»
    auto list = [](const std::vector<std::string>& v) {
        std::string s;
        for (size_t i = 0; i < v.size() && i < kShow; ++i) s += (i ? ", " : "") + v[i];
        if (v.size() > kShow) s += trf("fixes/gaps_more", std::to_string(v.size() - kShow));
        return s;
    };
    // Жёлтая — обязательные функции; только необязательные (слабые ссылки) — серая.
    brls::Box* sc = nullptr;
    if (!strong.empty()) {
        sc = makeSubCard(Tone::Warn, icon::kWarn, trf("fixes/sc_gaps", std::to_string(strong.size())));
        addSubLine(sc, list(strong), colText(), 17);
        addSubLine(sc, tr("fixes/gaps_may_crash"), colText(), 17);
        if (!weak.empty()) addSubLine(sc, trf("fixes/gaps_weak_more", std::to_string(weak.size())), colSub(), 17);
    } else {
        sc = makeSubCard(Tone::Neutral, icon::kInfo, trf("fixes/sc_gaps_weak", std::to_string(weak.size())), 19);
        addSubLine(sc, list(weak), colText(), 17);
        addSubLine(sc, tr("fixes/gaps_weak_note"), colSub(), 17);
    }
    std::string text = trf("fixes/gaps_details_head", ver) + "\n";
    for (const auto& g : f.gaps) {
        std::string who;
        for (const auto& m : g.importers) who += (who.empty() ? "" : ", ") + m;
        text += "• " + g.symbol + " — " + tr(g.kind == "STUB" ? "fixes/gaps_k_stub" : "fixes/gaps_k_missing") +
                (g.weak ? ", " + tr("fixes/gaps_k_weak") : std::string()) + " — " + who + "\n";
    }
    if (!weak.empty()) text += "\n" + tr("fixes/gaps_weak_note");
    text += "\n" + tr("fixes/gaps_details_note");
    auto* details = makeButton(tr("fixes/gaps_details"), &brls::BUTTONSTYLE_BORDERED, [text] {
        auto* dlg = new brls::Dialog(text);
        dlg->addButton(tr("common/ok"), [] {});
        dlg->open();
    });
    details->setId("gaps_details_btn");
    details->setHeight(48);
    addSubView(sc, details);
    details->setMarginTop(10);
    fixesBox_->addView(sc);

    // Совет: отладочная сборка покажет, на какой функции игра остановится, а «Собрать логи» даст отчёт для issue.
    if (!debug_) {
        auto* tip = makeSubCard(Tone::Info, icon::kTip, tr("fixes/tip_title"));
        addSubLine(tip, tr("fixes/gaps_debug_tip"), colText(), 17);
        fixesBox_->addView(tip);
    }
}

void GameActivity::buildMods(brls::Box* parent) {
    auto* card = makeCard();
    card->addView(makeSectionTitle(tr("mods/title")));
    auto* q = makeLabel(tr("mods/question"), 22, colText());
    q->setMarginBottom(8);
    card->addView(q);
    if (d_.mods.empty()) {
        card->addView(makeSubCard(Tone::Neutral, icon::kInfo, trf("mods/none_found", mk::plat::paths::kModsDir + row_.tid + "/"), 18));
        parent->addView(card);
        return;
    }
    auto refresh = [this]() {
        for (size_t i = 0; i < modRows_.size(); ++i) modRows_[i]->setChecked(static_cast<int>(i) - 1 == modChoice_);
        refreshActions();
    };
    auto* none = new ChoiceRow(tr("mods/choice_none"), "", "");
    none->onPick([this, refresh] {
        modChoice_ = -1;
        refresh();
    });
    modRows_.push_back(none);
    card->addView(none);
    for (size_t i = 0; i < d_.mods.size(); ++i) {
        const auto& m = d_.mods[i];
        if (m.inContents) {
            // Уже лежит в atmosphere/contents (поставлено раньше, не нами) — для сведения, не выбирается.
            card->addView(makeLabel(trf("mods/in_contents_info", m.title), 17, colSub()));
            continue;
        }
        std::string flags;
        auto add = [&flags](const std::string& f) { flags += (flags.empty() ? "" : " · ") + f; };
        if (m.hasRomfs) add(tr("mods/f_romfs"));
        if (m.hasOwnModule) add(tr("mods/f_module"));
        if (m.hasIps) add(tr("mods/f_ips"));
        if (m.inContents) add(tr("mods/f_in_contents"));
        auto* r = new ChoiceRow(m.title.empty() ? m.id : m.title, "", flags);
        const int idx = static_cast<int>(i);
        r->onPick([this, refresh, idx] {
            modChoice_ = idx;
            refresh();
        });
        modRows_.push_back(r);
        card->addView(r);
    }
    refresh();
    parent->addView(card);
}

void GameActivity::buildActions(brls::Box* parent) {
    // Флажок «Отладочная сборка» — прямо над кнопками; переключение сразу перестраивает кнопки
    // («Переустановить» и её причина — если вид сборки не совпадает с установленным).
    auto* dbg = new ChoiceRow(tr("debug/toggle"), "", tr("debug/toggle_sub"));
    dbg->setChecked(debug_);
    dbg->setId("debug_toggle");
    dbg->onPick([this, dbg] {
        debug_ = !debug_;
        dbg->setChecked(debug_);
        refreshActions();
    });
    dbg->setMarginTop(4);
    parent->addView(dbg);
    // Кнопки, строки при них, «Отладка» и «Собрать логи» — в actionsBox_ (перестраивается refreshActions).
    actionsBox_ = makeColumn();
    actionsBox_->setMarginBottom(30);
    parent->addView(actionsBox_);
}

mk::app::GameActionsInput GameActivity::actionsInput() const {
    mk::app::GameActionsInput s;
    s.moduleInstalled = d_.ourInstalled && !d_.slot.empty();
    s.receiptStale = d_.receiptStale;
    s.debugInstalled = d_.debugInstalled;
    s.debugChosen = debug_;
    const std::string modPath = selectedModPath();
    s.modChosen = !modPath.empty();
    s.modDiffers = modPath != d_.installedModPath;
    s.swapPresent = d_.sdk.swapPresent;
    s.swapOurs = d_.sdk.swapOurs;
    const bool haveChoice = sdkChoice_ >= 0 && sdkChoice_ < static_cast<int>(sdk_.size());
    s.sdkChosen = haveChoice;
    if (haveChoice) {
        // Та же sdk, что стоит нашей подменой: по sha256 (квитанция и опознание ядра), нет его — по версии.
        const auto& l = sdk_[sdkChoice_];
        const bool oursNow = d_.sdk.swapPresent && d_.sdk.swapOurs;
        const bool same = oursNow && (!l.sha256.empty() && !d_.installedSdkSha.empty() ? l.sha256 == d_.installedSdkSha
                                                                                       : l.version == d_.sdk.swapVersion);
        s.sdkDiffers = !same;
    }
    // Есть ли модулю работа для выбранной sdk: своя функция у нужного игре, пробелы sdk; не посчитано — «да».
    s.moduleHasWork = true;
    const auto& b = d_.fixes;
    if (haveChoice && b.gameOk && b.ownSdkOk && sdkChoice_ < static_cast<int>(b.bySdk.size())) {
        const auto& f = b.bySdk[sdkChoice_];
        if (f.ok) {
            bool work = !f.gaps.empty();
            for (const auto& l : f.fixes)
                if (!l.importers.empty() && l.choice == "own") work = true;
            s.moduleHasWork = work;
        }
    }
    s.zbicUnpack = d_.zbicUnpack && !d_.zbicUnpackedOurs;
    s.diagReport = reports_.diag;
    s.crashReport = reports_.crash;
    return s;
}

void GameActivity::refreshActions() {
    if (!loaded_ || !actionsBox_ || !sdkButtons_) return;
    using mk::app::Reinstall;
    const mk::app::GameActions a = mk::app::decideGameActions(actionsInput());

    // Блок sdk: «Подменить sdk» (одной подмены достаточно) / «Удалить подмену» (наша стоит).
    sdkButtons_->clearViews();
    if (a.sdkSwap) {
        auto* b = makeButton(tr("sdk/btn_swap"), &brls::BUTTONSTYLE_BORDERED, [this] { startOperation(OpKind::SdkSwap); });
        b->setId("sdk_swap_btn");
        sdkButtons_->addView(b);
    }
    if (a.sdkRemove) {
        auto* b = makeButton(tr("sdk/btn_unswap"), dangerStyle(), [this] { startOperation(OpKind::SdkUnswap); });
        b->setId("sdk_unswap_btn");
        sdkButtons_->addView(b);
    }

    actionsBox_->clearViews();
    // Выбор sdk виден и у кнопки установки: подмена входит в «Установить» / «Переустановить».
    if (a.alsoSwaps && sdkChoice_ >= 0 && sdkChoice_ < static_cast<int>(sdk_.size())) {
        auto* n = makeLabel(trf("actions/also_swap", sdk_[sdkChoice_].version), 17, colInfo());
        n->setMarginTop(8);
        actionsBox_->addView(n);
    }
    auto* row = makeRow();
    row->setMarginTop(8);
    brls::View* main = nullptr;
    if (a.install) {
        auto* install = makeButton(tr("actions/install"), &brls::BUTTONSTYLE_PRIMARY, [this] {
            if (!d_.exefsOpened && d_.exefs.needKeys) {
                brls::Application::pushActivity(new NoKeysActivity());
                return;
            }
            startOperation(OpKind::Install);
        });
        install->setId("install_btn");
        row->addView(install);
        main = install;
    }
    if (a.uninstall) {
        auto* un = makeButton(tr("actions/uninstall"), dangerStyle(), [this] { startOperation(OpKind::Uninstall); });
        un->setId("uninstall_btn");
        row->addView(un);
        if (!main) main = un;
    }
    if (a.reinstall != Reinstall::None) {
        auto* re = makeButton(tr("actions/reinstall"), &brls::BUTTONSTYLE_BORDERED, [this] {
            if (!d_.exefsOpened && d_.exefs.needKeys) {
                brls::Application::pushActivity(new NoKeysActivity());
                return;
            }
            startOperation(OpKind::Install);
        });
        re->setId("reinstall_btn");
        row->addView(re);
        main = re;   // есть причина поставить заново — фокус на «Переустановить», а не на удаление
    }
    actionsBox_->addView(row);
    if (a.reinstall != Reinstall::None) {
        const char* key = a.reinstall == Reinstall::Stale     ? "actions/why_stale"
                          : a.reinstall == Reinstall::ToDebug ? "debug/will_debug"
                          : a.reinstall == Reinstall::ToPlain ? "debug/will_plain"
                          : a.reinstall == Reinstall::OtherMod ? "actions/why_other_mod"
                                                               : "actions/why_other_sdk";
        auto* why = makeLabel(tr(key), 17, colWarn());
        why->setMarginTop(6);
        actionsBox_->addView(why);
    }

    // Отладка: есть отчёт этой игры — строка с временем последнего и кнопка «Отчёт отладки».
    if (a.diagReport) {
        auto* sc = makeSubCard(Tone::Info, icon::kInfo, tr("debug/sc_title"), 20);
        sc->setMarginTop(16);
        addSubLine(sc, reports_.lastWhen.empty() ? tr("debug/report_have_plain") : trf("debug/report_have", reports_.lastWhen),
                   colText(), 18);
        const std::string tid = row_.tid;
        auto* rep = makeButton(tr("debug/report_btn"), &brls::BUTTONSTYLE_BORDERED,
                               [tid] { brls::Application::pushActivity(new DiagActivity(tid)); });
        rep->setId("diag_btn");
        rep->setHeight(48);
        addSubView(sc, rep);
        rep->setMarginTop(10);
        actionsBox_->addView(sc);
    }
    // «Собрать логи для отправки» — только если есть что собрать (отчёт модуля или отчёт о падении).
    if (a.collectLogs) {
        auto* logs = makeButton(tr("logs/btn"), &brls::BUTTONSTYLE_BORDERED,
                                [this] { brls::Application::pushActivity(new LogsActivity(d_)); });
        logs->setId("logs_btn");
        logs->setMarginTop(16);
        auto* logsRow = makeRow();
        logsRow->addView(logs);
        actionsBox_->addView(logsRow);
    }
    if (!firstFocus_) firstFocus_ = main;
}

std::string GameActivity::selectedSdkPath() const {
    return (sdkChoice_ >= 0 && sdkChoice_ < static_cast<int>(sdk_.size())) ? sdk_[sdkChoice_].path : "";
}

std::string GameActivity::selectedModPath() const {
    return (modChoice_ >= 0 && modChoice_ < static_cast<int>(d_.mods.size())) ? d_.mods[modChoice_].path : "";
}

void GameActivity::startOperation(OpKind op) {
    AliveToken alive = alive_;
    std::string sdkPath = selectedSdkPath();
    // Чужую подмену sdk при установке не трогаем: план ядра всё равно откажет, но не предлагаем.
    if (d_.sdk.swapPresent && !d_.sdk.swapOurs && op == OpKind::Install) sdkPath.clear();
    const bool debug = (op == OpKind::Install) && debug_;
    const bool removal = (op == OpKind::Uninstall || op == OpKind::SdkUnswap);
    brls::Application::pushActivity(
        new OperationActivity(op, d_, selectedModPath(), sdkPath, debug, [this, alive, removal](bool ok) {
            if (!*alive) return;
            if (ok) returnToList(removal);   // успех — к списку игр
            else reload();                   // ошибка — остаться на странице игры и показать, что случилось
        }));
}

void GameActivity::returnToList(bool removal) {
    // Вызывается из колбэка закрытия экрана операции: он уже снят со стека, сверху — этот экран.
    const GameRow row = row_;
    std::function<void(const GameRow&)> cb = onRowChanged_;
    brls::Application::notify(trf(removal ? "op/removed_toast" : "op/done_toast", row.name));
    // Значки строки («Мод установлен», «sdk подменена») — перечитать игру в фоне; список проверяет, жив ли он.
    brls::async([cb, row]() {
        mk::app::GameDetails d = mk::app::Model::instance().loadDetails(row);
        brls::sync([cb, r = d.row]() {
            if (cb) cb(r);
        });
    });
    brls::Application::popActivity(brls::TransitionAnimation::FADE);
}

}  // namespace mk::ui
