// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — перевод ответа ядра «sdk из установленных игр» (mk::harvest) в вид для экрана и обратно.
// Одно место и для модели (source/app_model.cpp), и для предпросмотра (preview/mock_platform.cpp).
#pragma once

#include <mk/sdk_harvest.hpp>

#include <cstdio>
#include <functional>

#include "core_text.hpp"
#include "harvest_types.hpp"

namespace mk::app {

inline HarvestView harvestView(const mk::harvest::Selection& s,
                               const std::function<mk::harvest::SaveTarget(const mk::harvest::Candidate&)>& target) {
    HarvestView v;
    v.ok = true;
    if (s.firmware) v.firmware = mk::version_text(*s.firmware);
    if (s.table_sdk) v.tableSdk = mk::version_text(*s.table_sdk);
    v.withNnSdk = s.with_nnsdk;
    v.tooNew = s.too_new;
    v.already = s.already;
    v.notNnSdk = s.not_nnsdk;
    for (const auto& c : s.candidates) {
        HarvestLine l;
        l.version = mk::version_text(c.version);
        l.recommended = c.recommended;
        l.otherBuild = c.other_build;
        l.zbic = c.zbic;
        l.sha256 = c.sha256;
        l.moduleId = c.module_id;
        l.size = c.size;
        for (const auto& g : c.games) l.games.push_back({g.tid, g.name, g.version});
        if (target) {
            const mk::harvest::SaveTarget t = target(c);
            l.targetDir = t.dir;
            l.targetAlready = t.already;
        }
        v.lines.push_back(std::move(l));
    }
    return v;
}

inline mk::harvest::Candidate candidateOf(const HarvestLine& l) {
    mk::harvest::Candidate c;
    std::array<int, 3> v{};
    std::sscanf(l.version.c_str(), "%d.%d.%d", &v[0], &v[1], &v[2]);
    c.version = v;
    c.sha256 = l.sha256;
    c.module_id = l.moduleId;
    c.size = l.size;
    c.zbic = l.zbic;
    c.recommended = l.recommended;
    c.other_build = l.otherBuild;
    for (const auto& g : l.games) {
        mk::harvest::GameSdk s;
        s.tid = g.tid;
        s.name = g.name;
        s.version = g.version;
        s.has_sdk = s.nnsdk = true;
        s.sdk_version = v;
        s.sha256 = l.sha256;
        s.module_id = l.moduleId;
        s.size = l.size;
        c.games.push_back(std::move(s));
    }
    return c;
}

inline HarvestSaveView harvestSaveView(const mk::harvest::SaveResult& r, const mk::harvest::Candidate& c) {
    HarvestSaveView v;
    v.ok = r.ok;
    v.already = r.target.already;
    v.dir = r.target.dir;
    v.sdkPath = r.target.sdk_path;
    v.version = mk::version_text(c.version);
    if (!c.games.empty()) {
        v.game = c.games.front().name;
        v.gameVersion = c.games.front().version;
    }
    if (!r.ok) {
        if (r.code == "changed" || r.code == "not_nnsdk" || r.code == "no_room" || r.code == "no_exefs") {
            v.error = textOf("sdkscan/err/" + r.code);
        } else {
            v.error = textOf("sdkscan/err/io", {r.code + (r.detail.empty() ? "" : ": " + r.detail)});
        }
    }
    return v;
}

}  // namespace mk::app
