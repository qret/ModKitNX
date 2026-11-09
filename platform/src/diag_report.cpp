// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — разбор отчётов отладочной сборки модуля: построчно, без регулярных выражений. См. platform/diag_report.hpp.
#include "platform/diag_report.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace mk::plat::diag {

namespace {

std::vector<std::string> splitLines(const std::string& t) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= t.size()) {
        size_t nl = t.find('\n', pos);
        if (nl == std::string::npos) nl = t.size();
        std::string l = t.substr(pos, nl - pos);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(std::move(l));
        pos = nl + 1;
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }
bool contains(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }

// Значение после префикса «ключ<sep>».
std::string after(const std::string& line, const char* prefix) { return trim(line.substr(std::strlen(prefix))); }

bool symOk(const std::string& v) {
    const std::string t = trim(v);
    return !t.empty() && !contains(t, "НЕ НАЙДЕН") && t != "0" && t != "0x0";
}

void stage(std::vector<Stage>& st, const char* code, int ok, const std::string& detail) {
    Stage s;
    s.code = code;
    s.state = ok > 0 ? StageState::Ok : ok == 0 ? StageState::Fail : StageState::Unknown;
    s.detail = detail;
    st.push_back(std::move(s));
}

StageState stateOf(const std::vector<Stage>& st, const char* code) {
    for (const auto& s : st)
        if (s.code == code) return s.state;
    return StageState::Unknown;
}

}  // namespace

bool readText(const char* path, std::string& out, size_t maxBytes) {
    out.clear();
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<char> buf(64 * 1024);
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0 && out.size() < maxBytes) out.append(buf.data(), n);
    std::fclose(f);
    if (out.size() > maxBytes) out.resize(maxBytes);
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF && static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

DiagSummary parseDiag(const std::string& text) {
    DiagSummary r;
    if (text.empty() || !contains(text, "nxredirect")) {
        r.verdict = {"diag/v_not_report", {}};
        return r;
    }
    r.isReport = true;
    std::string slot, mount, probePath, probeResult;
    int modules = -1;
    std::map<std::string, std::string> syms;
    std::map<std::string, int> reasons;
    bool anyRedirected = false;

    for (const auto& raw : splitLines(text)) {
        const std::string& l = raw;
        if (startsWith(l, "слот модуля:")) slot = after(l, "слот модуля:");
        else if (startsWith(l, "TitleID (svcGetInfo ProgramId) =")) r.tid = after(l, "TitleID (svcGetInfo ProgramId) =");
        else if (startsWith(l, "путь мода на SD:")) r.modPath = after(l, "путь мода на SD:");
        else if (startsWith(l, "всего модулей найдено:")) modules = std::atoi(after(l, "всего модулей найдено:").c_str());
        else if (startsWith(l, "MountSdCard(\"sd\") =")) mount = after(l, "MountSdCard(\"sd\") =");
        else if (startsWith(l, "проба открытия папки мода ")) {
            const std::string rest = l.substr(std::strlen("проба открытия папки мода "));
            const size_t eq = rest.rfind(" =");
            if (eq != std::string::npos) {
                probePath = trim(rest.substr(0, eq));
                probeResult = trim(rest.substr(eq + 2));
            }
        } else if (startsWith(l, "nnMain") || startsWith(l, "nn::fs::")) {
            const size_t eq = l.find('=');
            if (eq != std::string::npos) syms[trim(l.substr(0, eq))] = trim(l.substr(eq + 1));
        } else if (startsWith(l, "#") && contains(l, " подменено ")) {
            ++r.opens;
            const bool redirected = contains(l, " подменено ДА");
            if (redirected) {
                ++r.attempts;
                anyRedirected = true;
            }
            // «подменено ДА» — лишь попытка; настоящая подмена — «успех, файл взят с SD».
            if (contains(l, "успех, файл взят с SD")) ++r.taken;
            const size_t rp = l.find("причина:");
            if (rp != std::string::npos) ++reasons[trim(l.substr(rp + std::strlen("причина:")))];
        }
        if (contains(l, "дальше не пишем")) r.truncated = true;
    }
    (void)anyRedirected;
    for (const auto& kv : reasons) r.reasons.emplace_back(kv.first, kv.second);

    stage(r.stages, "loaded", 1, slot);
    const bool tidOk = !r.tid.empty() && !contains(r.tid, "НЕ ПОЛУЧЕН");
    stage(r.stages, "tid", tidOk ? 1 : 0, r.tid);
    const bool pathOk = !r.modPath.empty() && !contains(r.modPath, "не собран");
    stage(r.stages, "modpath", pathOk ? 1 : 0, r.modPath);
    stage(r.stages, "modules", modules > 0 ? 1 : 0, modules >= 0 ? std::to_string(modules) : "");
    const char* need[] = {"nnMain", "nn::fs::OpenFile", "nn::fs::MountSdCard"};
    bool symAll = !syms.empty();
    std::string symDetail;
    for (const char* k : need) {
        auto it = syms.find(k);
        symAll = symAll && it != syms.end() && symOk(it->second);
        symDetail += std::string(symDetail.empty() ? "" : ", ") + k + " = " + (it != syms.end() ? it->second : "—");
    }
    stage(r.stages, "symbols", symAll ? 1 : 0, symDetail);
    stage(r.stages, "mount", mount.empty() ? -1 : (startsWith(mount, "успех") ? 1 : 0), mount);
    if (probeResult.empty()) {
        stage(r.stages, "modfolder", -1, "");
    } else {
        std::string lower = probeResult;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        stage(r.stages, "modfolder", contains(lower, "0x7d402") ? 0 : 1, probeResult + "  [" + probePath + "]");
    }
    stage(r.stages, "redirect", r.opens ? (r.taken > 0 ? 1 : 0) : -1,
          std::to_string(r.opens) + " / " + std::to_string(r.attempts) + " / " + std::to_string(r.taken));

    // Вывод — по первому проваленному этапу в порядке хода модуля, затем итог подмены.
    if (stateOf(r.stages, "symbols") == StageState::Fail) r.verdict = {"diag/v_symbols", {}};
    else if (stateOf(r.stages, "tid") == StageState::Fail) r.verdict = {"diag/v_tid", {}};
    else if (stateOf(r.stages, "mount") == StageState::Fail) r.verdict = {"diag/v_mount", {}};
    else if (stateOf(r.stages, "modfolder") == StageState::Fail) r.verdict = {"diag/v_modfolder", {r.modPath}};
    else if (r.opens && r.taken == 0) {
        bool onlyMissing = !reasons.empty();
        std::string list;
        for (const auto& kv : reasons) {
            if (kv.first != "файла нет на SD") onlyMissing = false;
            list += (list.empty() ? "" : "; ") + kv.first + " ×" + std::to_string(kv.second);
        }
        if (onlyMissing) r.verdict = {"diag/v_files_missing", {}};
        else if (!reasons.empty()) r.verdict = {"diag/v_no_redirect_reasons", {list}};
        else r.verdict = {"diag/v_no_redirect", {}};
    } else if (r.taken > 0) {
        r.verdict = {"diag/v_works", {}};
    } else {
        r.verdict = {"diag/v_no_opens", {}};
    }
    return r;
}

GfxSummary parseGfx(const std::string& text) {
    GfxSummary r;
    if (text.empty() || !(contains(text, "отчёт о ГРАФИКЕ") || contains(text, "nxredirect-gfx.txt"))) {
        r.verdict = {"diag/g_not_report", {}};
        return r;
    }
    r.isReport = true;
    bool inCalls = false;
    std::string surfaceResult;
    bool surfaceSeen = false, surfaceSuspect = false;
    for (const auto& l : splitLines(text)) {
        if (startsWith(l, "--- перехваченные вызовы графики")) {
            inCalls = true;
            continue;
        }
        if (!inCalls) {
            if (startsWith(l, "поставлено перехватчиков:")) {
                int a = -1, b = -1;
                if (std::sscanf(after(l, "поставлено перехватчиков:").c_str(), "%d из %d", &a, &b) == 2) {
                    r.hooksSet = a;
                    r.hooksTotal = b;
                }
            }
            continue;
        }
        const std::string t = trim(l);
        if (t.empty() || startsWith(t, "---")) continue;
        const bool suspect = startsWith(l, "!!");
        const bool callLine = suspect || startsWith(l, "  ");
        if (!callLine || t.find(" = ") == std::string::npos) continue;
        ++r.calls;
        const std::string body = suspect ? trim(l.substr(2)) : t;
        if (contains(body, "eglCreateWindowSurface")) {
            surfaceSeen = true;
            surfaceSuspect = suspect;
            surfaceResult = body;
        }
        if (suspect) {
            r.suspects.push_back(body);
            const size_t e = body.find("eglGetError=");
            if (r.firstEglError.empty() && e != std::string::npos) {
                size_t end = e + std::strlen("eglGetError=");
                size_t stop = end;
                while (stop < body.size() && std::isxdigit(static_cast<unsigned char>(body[stop])) ) ++stop;
                if (stop < body.size() && (body[stop] == 'x' || body[stop] == 'X')) {
                    ++stop;
                    while (stop < body.size() && std::isxdigit(static_cast<unsigned char>(body[stop]))) ++stop;
                }
                r.firstEglError = body.substr(end, stop - end);
            }
        }
    }
    stage(r.stages, "hooks", r.hooksSet < 0 ? -1 : (r.hooksSet == r.hooksTotal ? 1 : 0),
          r.hooksSet < 0 ? "" : std::to_string(r.hooksSet) + " / " + std::to_string(r.hooksTotal));
    stage(r.stages, "surface", surfaceSeen ? (surfaceSuspect ? 0 : 1) : -1, surfaceResult);
    stage(r.stages, "calls", r.calls > 0 ? 1 : 0,
          std::to_string(r.calls) + " / !! " + std::to_string(r.suspects.size()));

    if (r.calls == 0) r.verdict = {"diag/g_no_calls", {}};
    else if (r.suspects.empty()) r.verdict = {"diag/g_no_suspects", {}};
    else r.verdict = {"diag/g_suspect", {r.suspects.front(), r.firstEglError.empty() ? "—" : r.firstEglError}};
    return r;
}

}  // namespace mk::plat::diag
