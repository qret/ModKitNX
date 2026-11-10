// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/settings_activity.hpp"

#include "app_model.hpp"
#include "platform/app_config.hpp"
#include "platform/log.hpp"
#include "platform/paths.hpp"
#include "ui/update_ui.hpp"

namespace mk::ui {

SettingsActivity::~SettingsActivity() { *alive_ = false; }

void SettingsActivity::onContentAvailable() {
    setFrameTitle(this, "settings/title");
    auto& model = mk::app::Model::instance();
    const auto ci = model.console();

    // Язык — первой карточкой: единственная настройка здесь, на ней и фокус.
    auto* lang = makeCard();
    lang->addView(makeSectionTitle(tr("settings/lang_title")));
    // Тот же список и тот же конфиг, что у экрана первого запуска (resources/i18n/languages.json, config.json).
    // Язык borealis задаётся до init, поэтому новый применяется после перезапуска приложения.
    std::string cur = i18n::savedLanguage();
    if (cur.empty()) cur = i18n::language();
    const auto& langs = i18n::languages();
    // Строки — как на экране первого запуска (LanguageRow): самоназвание, имя по-английски, отметка справа.
    auto* list = makeColumn();
    list->setLineColor(colLine());
    list->setLineTop(1);
    list->setMarginBottom(12);
    for (size_t i = 0; i < langs.size(); ++i) {
        auto* r = new LanguageRow(langs[i].name, langs[i].english, tr("first_run/select"));
        r->setHeight(60);   // в карточке — плотнее, чем на экране первого запуска
        r->setPadding(0, 14, 0, 14);
        r->setChecked(cur == langs[i].code);
        const std::string code = langs[i].code;
        r->onPick([this, code, i] {
            for (size_t k = 0; k < langRows_.size(); ++k) langRows_[k]->setChecked(k == i);
            if (!i18n::saveLanguage(code)) {
                brls::Application::notify(tr("settings/save_failed"));
                return;
            }
            brls::Application::notify(code == i18n::language() ? tr("settings/lang_same") : tr("settings/lang_saved"));
        });
        langRows_.push_back(r);
        list->addView(r);
    }
    lang->addView(list);
    lang->addView(makeLabel(tr("settings/lang_note"), 16, colSub()));
    body->addView(lang);

    // Тема — сразу под языком, тем же списком: «Как в системе» / «Светлая» / «Тёмная».
    // Тема borealis ставится при запуске (виды копируют цвета), поэтому новая — после перезапуска, как язык.
    auto* theme = makeCard();
    theme->setId("theme_card");
    theme->addView(makeSectionTitle(trk("theme/title")));
    auto* tlist = makeColumn();
    tlist->setLineColor(colLine());
    tlist->setLineTop(1);
    tlist->setMarginBottom(12);
    const ThemeChoice curTheme = savedTheme();
    const struct {
        ThemeChoice c;
        std::string name, sub;
    } themes[] = {
        {ThemeChoice::System, trk("theme/system"), systemThemeIsLight() ? trk("theme/now_light") : trk("theme/now_dark")},
        {ThemeChoice::Light, trk("theme/light"), ""},
        {ThemeChoice::Dark, trk("theme/dark"), ""},
    };
    for (size_t i = 0; i < 3; ++i) {
        auto* r = new LanguageRow(themes[i].name, themes[i].sub, tr("first_run/select"));
        r->setHeight(60);
        r->setPadding(0, 14, 0, 14);
        r->setChecked(curTheme == themes[i].c);
        const ThemeChoice c = themes[i].c;
        r->onPick([this, c, i, curTheme] {
            const ThemeChoice before = savedTheme();   // что записано сейчас — к нему вернуть отметку при сбое
            for (size_t k = 0; k < themeRows_.size(); ++k) themeRows_[k]->setChecked(k == i);
            if (!saveTheme(c)) {
                // Не записалось — отметка снова на прежней (записанной) теме, а не на той, что не сохранилась.
                const ThemeChoice order[] = {ThemeChoice::System, ThemeChoice::Light, ThemeChoice::Dark};
                for (size_t k = 0; k < themeRows_.size() && k < 3; ++k) themeRows_[k]->setChecked(order[k] == before);
                brls::Application::notify(tr("settings/save_failed"));
                return;
            }
            brls::Application::notify(c == curTheme ? trk("theme/same") : trk("theme/saved"));
        });
        themeRows_.push_back(r);
        tlist->addView(r);
    }
    theme->addView(tlist);
    theme->addView(makeLabel(trk("theme/note"), 16, colSub()));
    body->addView(theme);

    // Обновления: версия, «Проверить обновления», «Проверять при запуске».
    auto* upd = makeCard();
    upd->setId("update_card");
    upd->addView(makeSectionTitle(trk("update/settings_title")));
    upd->addView(makeInfoLine(trk("update/settings_version"), MK_APP_VERSION, colText()));
    auto* checkBtn = makeButton(trk("update/check_btn"), &brls::BUTTONSTYLE_BORDERED, [] { update::checkNow(); });
    checkBtn->setMarginTop(10);
    checkBtn->setMarginBottom(10);
    checkBtn->setId("update_check_btn");
    upd->addView(checkBtn);
    auto* onStart = new ChoiceRow(trk("update/check_on_start"), "", trk("update/check_on_start_sub"));
    onStart->setChecked(mk::plat::config::getBool(mk::plat::config::kUpdateCheck, true));
    onStart->onPick([onStart] {
        const bool v = !onStart->isChecked();
        if (!mk::plat::config::setBool(mk::plat::config::kUpdateCheck, v)) {
            brls::Application::notify(tr("settings/save_failed"));
            return;
        }
        onStart->setChecked(v);
    });
    upd->addView(onStart);
    body->addView(upd);

    // Журнал приложения: включить/выключить, действует сразу.
    auto* logCard = makeCard();
    logCard->addView(makeSectionTitle(tr("settings/log_title")));
    auto* logRow = new ChoiceRow(tr("settings/log_toggle"), "", trf("settings/log_sub", std::to_string(mk::plat::log::kKeepFiles)));
    logRow->setChecked(mk::plat::log::enabled());
    logRow->onPick([logRow] {
        const bool v = !logRow->isChecked();
        if (!mk::plat::log::setEnabled(v)) {
            brls::Application::notify(tr("settings/save_failed"));
            return;
        }
        logRow->setChecked(v);
        brls::Application::notify(v ? tr("settings/log_on") : tr("settings/log_off"));
    });
    logCard->addView(logRow);
    body->addView(logCard);

    // О программе
    auto* about = makeCard();
    about->addView(makeLabel(std::string(MK_APP_NAME) + " " + MK_APP_VERSION, 30, colText()));
    about->addView(makeLabel(tr("about/what"), 19, colText()));
    about->addView(makeLabel(tr("about/license"), 16, colSub()));
    body->addView(about);

    // Благодарности
    auto* thanks = makeCard();
    thanks->setId("thanks_card");
    thanks->addView(makeSectionTitle(tr("about/thanks_title")));
    thanks->addView(makeLabel(tr("about/thanks_lunya"), 20, colText()));
    thanks->addView(makeLabel("https://github.com/Langegen/TorrentShopNX", 16, colInfo(), true));
    auto* duckbill = makeLabel(tr("about/thanks_duckbill"), 20, colText());
    duckbill->setMarginTop(10);
    thanks->addView(duckbill);
    auto* chifir = makeLabel(tr("about/thanks_4ifir"), 20, colText());
    chifir->setMarginTop(10);
    thanks->addView(chifir);
    auto* others = makeLabel(tr("about/thanks_others"), 17, colSub());
    others->setMarginTop(10);
    thanks->addView(others);
    body->addView(thanks);


    // Паспорт консоли
    auto* pass = makeCard();
    pass->addView(makeSectionTitle(tr("console/title")));
    auto unknown = tr("common/unknown");
    pass->addView(makeInfoLine(tr("console/firmware"), ci.firmware.valid ? ci.firmwareDisplay : unknown, colText()));
    pass->addView(makeInfoLine(tr("console/atmosphere"),
                               ci.amsKnown ? trf("console/ams_value", ci.atmosphere.toString(),
                                                 ci.amsTargetFirmware.toString())
                                           : unknown,
                               colText()));
    pass->addView(makeInfoLine(tr("console/emummc"),
                               ci.emummcKnown ? (ci.emummcType ? tr("console/emummc_yes") : tr("console/emummc_no"))
                                              : unknown,
                               colText()));
    pass->addView(makeInfoLine(tr("console/mode"), ci.titleMode ? tr("console/mode_title") : tr("console/mode_applet"),
                               ci.titleMode ? colOk() : colWarn()));
    pass->addView(makeInfoLine(tr("console/keys"), ci.prodKeysPresent ? tr("console/keys_yes") : tr("console/keys_no"),
                               colText()));
    pass->addView(makeInfoLine(tr("console/sdk_count"), std::to_string(model.sdkList(false).size()), colText()));
    AliveToken alive = alive_;
    auto* save = makeButton(tr("console/save_btn"), &brls::BUTTONSTYLE_BORDERED, [alive] {
        const bool ok = mk::app::Model::instance().writeConsolePassport(0);
        brls::Application::notify(ok ? trf("console/saved", mk::plat::paths::kConsolePassport)
                                     : tr("settings/save_failed"));
    });
    save->setMarginTop(12);
    pass->addView(save);
    body->addView(pass);

    // Папки
    auto* dirs = makeCard();
    dirs->addView(makeSectionTitle(tr("folders/title")));
    dirs->addView(makeInfoLine(tr("folders/mods"), mk::plat::paths::kModsDir + "<TID>/", colText()));
    dirs->addView(makeInfoLine(tr("folders/sdk"), mk::plat::paths::kSdkDirOwn, colText()));
    dirs->addView(makeInfoLine(tr("folders/sdk_dbi"), mk::plat::paths::kSdkDirDbi, colText()));
    {
        const std::string lp = mk::plat::log::currentPath();
        dirs->addView(makeInfoLine(tr("folders/log"), lp.empty() ? tr("settings/log_off_short") : lp, colText()));
    }
    body->addView(dirs);

    // Фокус — на выбранном языке (иначе на первом).
    brls::View* focus = langRows_.empty() ? nullptr : langRows_[0];
    for (auto* r : langRows_)
        if (r->isChecked()) focus = r;
    if (focus) brls::Application::giveFocus(focus);
}

}  // namespace mk::ui
