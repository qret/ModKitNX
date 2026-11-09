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
        brls::sync([this, alive, d = std::move(d), sdk = std::move(sdk)]() mutable {
            if (!*alive) return;
            d_ = std::move(d);
            sdk_ = std::move(sdk);
            loaded_ = true;
            // Выбор по умолчанию: рекомендованная sdk, если подмена нужна; мод — «без мода».
            sdkChoice_ = (d_.sdk.advice.need == SdkNeed::Needed) ? d_.sdk.advice.recommendedIndex : -1;
            modChoice_ = -1;
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
    firstFocus_ = nullptr;
    buildHeader(body);
    buildState(body);
    buildSdk(body);
    buildFixes(body);
    buildMods(body);
    buildActions(body);
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
    if (d_.receiptStale) card->addView(makeLabel(tr("state/stale"), 19, colWarn()));
    if (d_.programIdMismatch) card->addView(makeLabel(tr("state/program_mismatch"), 19, colWarn()));
    if (!d_.coreOk && !d_.coreError.empty()) {
        card->addView(makeLabel(trf("state/core_failed", trText(d_.coreError)), 17, colSub()));
    }
    for (const auto& n : d_.notes) card->addView(makeLabel(trText(n), 17, colSub()));

    if (d_.exefs.needKeys) {
        card->addView(makeLabel(tr("state/need_keys"), 20, colWarn()));
        auto* b = makeButton(tr("state/why_keys"), &brls::BUTTONSTYLE_BORDERED,
                             [] { brls::Application::pushActivity(new NoKeysActivity()); });
        b->setMarginTop(10);
        card->addView(b);
        if (!firstFocus_) firstFocus_ = b;
    } else if (d_.exefs.rawNotImplemented) {
        card->addView(makeLabel(tr("state/raw_not_impl"), 19, colWarn()));
    } else if (!d_.exefsOpened && d_.exefs.rawTried) {
        // Путь 3 (сырой NCA) не прошёл: какой шаг и код — для пробы на консоли; подробности — «Собрать логи».
        std::string step = d_.exefs.rawStep.empty() ? std::string("mount") : d_.exefs.rawStep;
        for (auto& c : step)
            if (c == '-') c = '_';
        std::string code = d_.exefs.rawCore;
        if (d_.exefs.rcRaw) code += (code.empty() ? "" : " ") + hex32(d_.exefs.rcRaw);
        if (code.empty()) code = "-";
        card->addView(makeLabel(trf("state/raw_failed", tr("state/raw_step_" + step), code), 19, colWarn()));
        if (d_.exefs.rawPersonalized) card->addView(makeLabel(tr("state/raw_personalized"), 17, colSub()));
        card->addView(makeLabel(tr("state/raw_send_logs"), 17, colSub()));
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
    NVGcolor vColor = colText();
    switch (a.need) {
        case SdkNeed::NotNeeded:
            verdict = trf("sdk/v_not_needed", game, fw);
            vColor = colOk();
            break;
        case SdkNeed::Needed:
            vColor = colWarn();
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
            vColor = colOk();
            break;
        case SdkNeed::SwappedTooNew:
            verdict = trf("sdk/v_swapped_too_new", orUnknown(s.swapVersion), fw);
            vColor = colBad();
            break;
        case SdkNeed::SwappedUnknown:
            verdict = tr("sdk/v_swapped_unknown");
            vColor = colWarn();
            break;
        default:
            verdict = tr("sdk/v_unknown");
            vColor = colSub();
            break;
    }
    auto* v = makeLabel(verdict, 23, vColor);
    v->setMarginTop(10);
    v->setMarginBottom(6);
    card->addView(v);

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
            refreshFixes();   // блок «Что исправит модуль» — для выбранной sdk
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

    // Подмена нужна, а подходящей sdk в папках нет — взять её из установленных игр (ничего не скачивается).
    if (a.need == SdkNeed::Needed && !haveRec) {
        auto* why = makeLabel(trk("sdkscan/explain"), 18, colSub());
        why->setMarginTop(12);
        card->addView(why);
        const std::string need = a.bestForFirmware.valid ? std::to_string(a.bestForFirmware.major) + ".x"
                                                         : (s.firmware.empty() ? tr("common/unknown") : s.firmware);
        auto* find = makeButton(trk("sdkscan/btn"), &brls::BUTTONSTYLE_PRIMARY, [this, need] {
            AliveToken alive = alive_;
            brls::Application::pushActivity(new SdkHarvestActivity(row_.name, need, [this, alive] {
                if (*alive) reload();
            }));
        });
        find->setId("harvest_btn");
        find->setMarginTop(8);
        auto* findRow = makeRow();
        findRow->addView(find);
        card->addView(findRow);
    }

    // Кнопки «Подменить sdk» / «Убрать подмену» и «Подробности».
    auto* buttons = makeRow();
    buttons->setMarginTop(12);
    if (!sdk_.empty() && !(s.swapPresent && !s.swapOurs)) {
        buttons->addView(makeButton(tr("sdk/btn_swap"), &brls::BUTTONSTYLE_BORDERED, [this] {
            if (sdkChoice_ < 0) {
                brls::Application::notify(tr("sdk/pick_first"));
                return;
            }
            startOperation(OpKind::SdkSwap);
        }));
    }
    if (s.swapPresent && s.swapOurs) {
        buttons->addView(
            makeButton(tr("sdk/btn_unswap"), &brls::BUTTONSTYLE_BORDERED, [this] { startOperation(OpKind::SdkUnswap); }));
    }
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
    card->addView(makeSectionTitle(tr("fixes/title")));
    fixesBox_ = makeColumn();
    card->addView(fixesBox_);
    parent->addView(card);
    refreshFixes();
}

void GameActivity::refreshFixes() {
    if (!fixesBox_) return;
    fixesBox_->clearViews();
    const auto& b = d_.fixes;
    if (!b.gameOk) {
        fixesBox_->addView(makeLabel(b.error.empty() ? tr("fixes/no_game") : trf("fixes/failed", trText(b.error)), 19, colSub()));
        return;
    }
    // Какая sdk будет у игры: выбранная для подмены, иначе текущая (подменённая на карте или родная).
    const mk::app::FixForecastView* f = nullptr;
    std::string head;
    if (sdkChoice_ >= 0 && sdkChoice_ < static_cast<int>(b.bySdk.size())) {
        f = &b.bySdk[sdkChoice_];
        head = "fixes/for_swap";
    } else if (b.current.ok || !b.current.error.empty()) {
        f = &b.current;
        head = b.currentIsSwap ? "fixes/for_current_swap" : "fixes/for_current_native";
    }
    if (!f || (!f->ok && f->error.empty())) {
        fixesBox_->addView(makeLabel(tr("fixes/pick_sdk"), 19, colSub()));
        return;
    }
    if (!f->ok) {
        fixesBox_->addView(makeLabel(trf("fixes/failed", trText(f->error)), 19, colWarn()));
        return;
    }
    fixesBox_->addView(makeLabel(trf(head, f->sdkVersion.empty() ? tr("common/unknown") : f->sdkVersion), 21, colText()));

    // Итог: сколько из нужных игре функций модуль заменит своими.
    int needed = 0, own = 0;
    for (const auto& l : f->fixes) {
        if (l.importers.empty()) continue;
        ++needed;
        if (l.choice == "own") ++own;
    }
    auto* sum = makeLabel(own ? trf("fixes/summary", std::to_string(own), std::to_string(needed)) : tr("fixes/summary_none"),
                          19, own ? colOk() : colSub());
    sum->setMarginTop(4);
    fixesBox_->addView(sum);

    // Человеческие названия и пояснения — только для записей, известных этой версии; новая запись каталога
    // показывается своим id, пока к ней не добавят строки.
    static const char* kNamed[] = {"hash_memory", "ldn_set_protocol", "nvrm_query"};
    auto named = [](const std::string& id) {
        for (const char* n : kNamed)
            if (id == n) return true;
        return false;
    };
    for (const auto& l : f->fixes) {
        std::string state;
        NVGcolor color = colText();
        std::string note;
        if (l.importers.empty()) {
            state = tr("fixes/st_unused");
            color = colSub();
        } else if (l.choice == "own") {
            color = colOk();
            state = (l.why == "StubOlderSdk") ? tr("fixes/st_stub_own") : tr("fixes/st_add");
            if (l.id == "nvrm_query" && l.why == "StubOlderSdk") note = tr("fixes/note_nvrm_query");
            if (l.id == "ldn_set_protocol") note = tr("fixes/note_ldn_set_protocol");
        } else if (l.why == "StubVulkan") {
            state = tr("fixes/st_vulkan");
            color = colInfo();
        } else if (l.why == "StubNative") {
            state = tr("fixes/st_native_stub");
            color = colSub();
        } else if (l.why == "StubNoVersion") {
            state = tr("fixes/st_no_version");
            color = colWarn();
        } else if (l.why == "NoSdkDefined") {
            state = tr("fixes/st_unknown_sdk");
            color = colWarn();
        } else {
            state = tr("fixes/st_proxy");
        }
        if (!note.empty()) state += " (" + note + ")";

        auto* t = makeLabel(named(l.id) ? tr("fixes/name_" + l.id) : l.id, 21, l.importers.empty() ? colSub() : colText());
        t->setMarginTop(12);
        fixesBox_->addView(t);
        auto* st = makeLabel(state, 19, color);
        st->setMarginTop(3);
        fixesBox_->addView(st);
        std::string who;
        for (const auto& m : l.importers) who += (who.empty() ? "" : ", ") + m;
        auto* tech = makeLabel(trf("fixes/tech", l.symbol, who.empty() ? "—" : who, l.kind, l.scope, l.choice, l.why),
                               14, colSub());
        tech->setMarginTop(3);
        fixesBox_->addView(tech);
    }

    // Правило Vulkan — отдельной строкой.
    std::string rule = b.vulkan ? trf("fixes/vk_rule_vulkan", b.vulkanModule)
                                : (b.opengl ? tr("fixes/vk_rule_gl") : tr("fixes/vk_rule_other"));
    auto* r = makeLabel(rule, 17, b.vulkan ? colInfo() : colSub());
    r->setMarginTop(14);
    fixesBox_->addView(r);
}

void GameActivity::buildMods(brls::Box* parent) {
    auto* card = makeCard();
    card->addView(makeSectionTitle(tr("mods/title")));
    auto* q = makeLabel(tr("mods/question"), 22, colText());
    q->setMarginBottom(8);
    card->addView(q);
    if (d_.mods.empty()) {
        card->addView(makeLabel(trf("mods/none_found", mk::plat::paths::kModsDir + row_.tid + "/"), 18, colSub()));
        parent->addView(card);
        return;
    }
    auto refresh = [this]() {
        for (size_t i = 0; i < modRows_.size(); ++i) modRows_[i]->setChecked(static_cast<int>(i) - 1 == modChoice_);
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
    // Флажок «Отладочная сборка» — прямо над кнопкой «Установить».
    auto* dbg = new ChoiceRow(tr("debug/toggle"), "", tr("debug/toggle_sub"));
    dbg->setChecked(debug_);
    dbg->setId("debug_toggle");
    // Подсказка под флажком: что сделает «Установить», если вид сборки меняется (модуль перезапишется начисто).
    auto* kindNote = makeLabel("", 17, colWarn());
    auto refreshNote = [this, kindNote] {
        std::string t;
        if (d_.ourInstalled && d_.debugInstalled && !debug_) t = tr("debug/will_plain");
        else if (d_.ourInstalled && !d_.debugInstalled && debug_) t = tr("debug/will_debug");
        kindNote->setText(t);
        kindNote->setVisibility(t.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    };
    dbg->onPick([this, dbg, refreshNote] {
        debug_ = !debug_;
        dbg->setChecked(debug_);
        refreshNote();
    });
    dbg->setMarginTop(4);
    parent->addView(dbg);
    parent->addView(kindNote);
    refreshNote();

    auto* row = makeRow();
    row->setMarginTop(6);
    row->setMarginBottom(30);
    auto* install = makeButton(tr("actions/install"), &brls::BUTTONSTYLE_PRIMARY, [this] {
        if (!d_.exefsOpened && d_.exefs.needKeys) {
            brls::Application::pushActivity(new NoKeysActivity());
            return;
        }
        startOperation(OpKind::Install);
    });
    install->setId("install_btn");
    row->addView(install);
    if (!firstFocus_) firstFocus_ = install;
    if (d_.ourInstalled) {
        row->addView(makeButton(tr("actions/uninstall"), &brls::BUTTONSTYLE_BORDERED,
                                [this] { startOperation(OpKind::Uninstall); }));
    }
    if (debug_ || d_.ourInstalled || diagReportsPresent(row_.tid)) {
        const std::string tid = row_.tid;
        row->addView(makeButton(tr("debug/report_btn"), &brls::BUTTONSTYLE_BORDERLESS,
                                [tid] { brls::Application::pushActivity(new DiagActivity(tid)); }));
    }
    parent->addView(row);
    // «Собрать логи для отправки» — отдельной строкой под действиями (отчёты, падение, паспорт, квитанция -> .zip).
    auto* logs = makeButton(tr("logs/btn"), &brls::BUTTONSTYLE_BORDERED,
                            [this] { brls::Application::pushActivity(new LogsActivity(d_)); });
    logs->setId("logs_btn");
    logs->setMarginTop(-18);
    logs->setMarginBottom(30);
    auto* logsRow = makeRow();
    logsRow->addView(logs);
    parent->addView(logsRow);
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
