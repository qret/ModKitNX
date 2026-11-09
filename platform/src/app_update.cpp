// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — обновление приложения: переносимая часть (без libnx). См. platform/app_update.hpp.
#include "platform/app_update.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include <nlohmann/json.hpp>

#include "mk/core.hpp"
#include "platform/paths.hpp"

namespace mk::plat::update {

namespace {

using json = nlohmann::json;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

// Первая серия ровно из 64 шестнадцатеричных знаков в строке ("" — нет).
std::string findHex64(const std::string& line) {
    size_t i = 0;
    while (i < line.size()) {
        if (!std::isxdigit(static_cast<unsigned char>(line[i]))) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < line.size() && std::isxdigit(static_cast<unsigned char>(line[j]))) ++j;
        if (j - i == 64) return lower(line.substr(i, 64));
        i = j;
    }
    return "";
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\r') continue;
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

// Строка описания — служебная sha256 ассета (её пишет tools/release.ps1): «sha256 (ModKitNX.nro): `…`».
bool isShaLine(const std::string& line) {
    return lower(line).find("sha256") != std::string::npos && !findHex64(line).empty();
}

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t rd64(const uint8_t* p) { return static_cast<uint64_t>(rd32(p)) | (static_cast<uint64_t>(rd32(p + 4)) << 32); }

std::string cstr(const uint8_t* p, size_t max) {
    size_t n = 0;
    while (n < max && p[n]) ++n;
    return std::string(reinterpret_cast<const char*>(p), n);
}

bool fileExists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

bool readAll(const std::string& p, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[64 * 1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    const bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

constexpr size_t kNacpSize = 0x4000;
constexpr size_t kNacpTitle = 0x300;      // запись языка: имя 0x200 + автор 0x100
constexpr size_t kNacpVersion = 0x3060;   // display version, 0x10

}  // namespace

std::string latestUrl() { return std::string("https://api.github.com/repos/") + kRepo + "/releases/latest"; }
std::string assetName() { return std::string(MK_APP_NAME) + ".nro"; }

// ---------------------------------------------------------------- версии

Version parseVersion(const std::string& s) {
    Version v;
    size_t i = (!s.empty() && (s[0] == 'v' || s[0] == 'V')) ? 1 : 0;
    int parts[3] = {0, 0, 0};
    int n = 0;
    while (n < 3 && i < s.size()) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) break;
        long x = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            x = x * 10 + (s[i++] - '0');
            if (x > 1000000) return Version{};   // не номер версии
        }
        parts[n++] = static_cast<int>(x);
        if (i < s.size() && s[i] == '.') {
            ++i;
            continue;
        }
        break;
    }
    // Нужно хотя бы «X.Y»; хвост допустим только после «-» или «+» ("1.2.3-beta").
    if (n < 2) return Version{};
    if (i < s.size() && s[i] != '-' && s[i] != '+') return Version{};
    v.major = parts[0];
    v.minor = parts[1];
    v.patch = parts[2];
    v.valid = true;
    return v;
}

bool isNewer(const std::string& candidate, const std::string& current) {
    const Version a = parseVersion(candidate), b = parseVersion(current);
    if (!a.valid) return false;
    if (!b.valid) return true;   // своя версия не читается (сборка без APP_VERSION) — любая настоящая новее
    if (a.major != b.major) return a.major > b.major;
    if (a.minor != b.minor) return a.minor > b.minor;
    return a.patch > b.patch;
}

bool sameVersion(const std::string& a, const std::string& b) {
    const Version x = parseVersion(a), y = parseVersion(b);
    return x.valid && y.valid && x.major == y.major && x.minor == y.minor && x.patch == y.patch;
}

// ---------------------------------------------------------------- ответ GitHub

Release parseRelease(const std::string& text, const std::string& asset) {
    Release r;
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        r.error = std::string("json: ") + e.what();
        return r;
    }
    if (!j.is_object()) {
        r.error = "json: not an object";
        return r;
    }
    auto str = [](const json& o, const char* k) {
        auto it = o.find(k);
        return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string();
    };
    r.tag = str(j, "tag_name");
    if (r.tag.empty()) {
        // Нет релизов (404 {"message": "Not Found"}) или лимит запросов (403 {"message": "API rate limit…"}).
        const std::string m = str(j, "message");
        r.error = m.empty() ? "no tag_name" : "github: " + m;
        return r;
    }
    r.version = (r.tag[0] == 'v' || r.tag[0] == 'V') ? r.tag.substr(1) : r.tag;
    r.name = str(j, "name");
    r.body = str(j, "body");
    // j.value() бросает type_error, если поле не bool («"draft":"false"», null) — вне try это std::terminate.
    auto flag = [&](const char* k) {
        auto it = j.find(k);
        return it != j.end() && it->is_boolean() && it->get<bool>();
    };
    r.draft = flag("draft");
    r.prerelease = flag("prerelease");
    if (!parseVersion(r.version).valid) {
        r.error = "tag is not a version: " + r.tag;
        return r;
    }

    // Ассет: точное имя (без учёта регистра); иначе — единственный .nro релиза.
    const json* exact = nullptr;
    const json* onlyNro = nullptr;
    int nroCount = 0;
    auto assets = j.find("assets");
    if (assets != j.end() && assets->is_array()) {
        for (const auto& a : *assets) {
            if (!a.is_object()) continue;
            const std::string n = lower(str(a, "name"));
            if (n == lower(asset)) exact = &a;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".nro") == 0) {
                ++nroCount;
                onlyNro = &a;
            }
        }
    }
    const json* pick = exact ? exact : (nroCount == 1 ? onlyNro : nullptr);
    std::string digestSha;
    if (pick) {
        r.assetUrl = str(*pick, "browser_download_url");
        auto sz = pick->find("size");
        if (sz != pick->end() && sz->is_number_unsigned()) r.assetSize = sz->get<uint64_t>();
        else if (sz != pick->end() && sz->is_number_integer() && sz->get<int64_t>() > 0)
            r.assetSize = static_cast<uint64_t>(sz->get<int64_t>());
        const std::string d = lower(str(*pick, "digest"));   // GitHub: "sha256:<hex>"
        if (d.rfind("sha256:", 0) == 0 && isHex64(d.substr(7))) digestSha = d.substr(7);
    }
    // sha256 в описании (tools/release.ps1): строка с «sha256» и 64 hex.
    std::string bodySha;
    for (const auto& line : splitLines(r.body))
        if (isShaLine(line)) {
            bodySha = findHex64(line);
            break;
        }
    if (!digestSha.empty() && !bodySha.empty() && digestSha != bodySha) {
        // Два источника расходятся — ставить такое нельзя: ассет подменён или описание не от этого файла.
        r.error = "sha256 of the asset differs from the one in the release notes";
        r.assetUrl.clear();
        r.shaMismatch = true;
    }
    r.sha256 = !digestSha.empty() ? digestSha : bodySha;
    r.ok = true;
    return r;
}

std::string notesForDisplay(const std::string& body, size_t maxChars, size_t maxLines) {
    std::vector<std::string> keep;
    for (std::string line : splitLines(body)) {
        const std::string t = trim(line);
        if (t == "---" || t == "***" || isShaLine(t)) continue;
        std::string s = t;
        // «## Заголовок» → «Заголовок»; «* пункт» / «- пункт» → «• пункт»; **жирный** → жирный.
        size_t h = 0;
        while (h < s.size() && s[h] == '#') ++h;
        if (h > 0 && h < s.size() && s[h] == ' ') s = s.substr(h + 1);
        if (s.size() > 2 && (s[0] == '*' || s[0] == '-') && s[1] == ' ') s = "• " + s.substr(2);
        for (size_t p; (p = s.find("**")) != std::string::npos;) s.erase(p, 2);
        if (s.empty()) continue;   // в окне место дорого: без пустых строк
        keep.push_back(s);
    }

    std::string out;
    bool cut = false;
    for (size_t i = 0; i < keep.size(); ++i) {
        if (i >= maxLines) {
            cut = true;
            break;
        }
        const std::string add = (i ? "\n" : "") + keep[i];
        if (out.size() + add.size() > maxChars) {
            // Строка не помещается целиком: взять её начало, не разрезая символ UTF-8.
            size_t room = maxChars > out.size() ? maxChars - out.size() : 0;
            if (room > 16) {
                std::string part = add.substr(0, room);
                while (!part.empty() && (static_cast<unsigned char>(part.back()) & 0xC0) == 0x80) part.pop_back();
                if (!part.empty() && (static_cast<unsigned char>(part.back()) & 0xC0) == 0xC0) part.pop_back();
                out += part;
            }
            cut = true;
            break;
        }
        out += add;
    }
    if (cut) out += "…";
    return out;
}

Decision decide(const Release& r, const std::string& current, const std::string& skippedVersion) {
    Decision d;
    if (!r.ok || r.draft || r.prerelease) return d;
    d.newer = isNewer(r.version, current);
    d.skipped = d.newer && !skippedVersion.empty() && sameVersion(r.version, skippedVersion);
    // Ставить только сверяемое: без sha256 (ни digest ассета, ни строки в описании) скачанное не с чем сверить.
    d.installable = d.newer && !r.assetUrl.empty() && !r.sha256.empty();
    // Ассет есть, но сверить не с чем или два источника sha256 расходятся — «файл не сходится», а не «нет ассета».
    d.fileMismatch = d.newer && !d.installable && (r.shaMismatch || !r.assetUrl.empty());
    return d;
}

// ---------------------------------------------------------------- .nro

NroInfo readNro(const uint8_t* data, size_t size) {
    NroInfo n;
    if (!data || size < 0x80 || std::memcmp(data + 0x10, "NRO0", 4) != 0) {
        n.error = "not_nro";
        return n;
    }
    const uint64_t image = rd32(data + 0x18);
    if (image < 0x80 || image + 0x38 > size) {
        n.error = "truncated";
        return n;
    }
    const uint8_t* aset = data + image;
    if (std::memcmp(aset, "ASET", 4) != 0) {
        n.error = "no_assets";
        return n;
    }
    const uint64_t nacpOff = rd64(aset + 0x18), nacpSize = rd64(aset + 0x20);
    if (nacpSize < kNacpSize || nacpOff > size || image + nacpOff + kNacpSize > size) {
        n.error = "no_nacp";
        return n;
    }
    const uint8_t* nacp = aset + nacpOff;
    for (size_t i = 0; i < 16 && n.name.empty(); ++i) n.name = cstr(nacp + i * kNacpTitle, 0x200);
    n.version = cstr(nacp + kNacpVersion, 0x10);
    n.ok = true;
    return n;
}

NroInfo verifyNro(const uint8_t* data, size_t size, const Release& r, const std::string& expectName, mk::IHash& hash) {
    if (r.assetSize > 0 && size != r.assetSize) {
        NroInfo n;
        n.error = "size";
        return n;
    }
    NroInfo n = readNro(data, size);
    if (!n.ok) return n;
    const uint8_t* nacp = data + rd32(data + 0x18) + rd64(data + rd32(data + 0x18) + 0x18);
    bool nameOk = false;
    for (size_t i = 0; i < 16 && !nameOk; ++i) nameOk = cstr(nacp + i * kNacpTitle, 0x200) == expectName;
    if (!nameOk) {
        n.ok = false;
        n.error = "name";
        return n;
    }
    if (!sameVersion(n.version, r.version)) {
        n.ok = false;
        n.error = "version";
        return n;
    }
    if (!r.sha256.empty()) {
        const mk::Digest d = hash.sha256(data, size);
        if (mk::hex(d) != lower(r.sha256)) {
            n.ok = false;
            n.error = "sha256";
            return n;
        }
    }
    return n;
}

// ---------------------------------------------------------------- файлы

std::string selfNroPath(const char* argv0) {
    if (!argv0 || !*argv0) return "";
    std::string p = argv0;
    if (p.size() < 5 || lower(p.substr(p.size() - 4)) != ".nro") return "";
    if (p[0] == '/') p = "sdmc:" + p;
    if (p.find(":/") == std::string::npos) return "";
    return p;
}

std::string restartArgv(const std::string& self) {
    if (self.find_first_of(" \t") == std::string::npos) return self;
    return "\"" + self + "\"";
}

RelaunchPlan planRelaunch(bool requested, bool loaderHasNextLoad, const std::string& self) {
    RelaunchPlan p;
    if (!requested) {
        p.reason = "not_requested";
        return p;
    }
    if (!loaderHasNextLoad) {
        p.reason = "no_loader";
        return p;
    }
    if (self.empty()) {
        p.reason = "no_self";
        return p;
    }
    const std::string argv = restartArgv(self);
    if (self.size() >= kLoaderPathMax || argv.size() >= kLoaderArgvMax) {
        p.reason = "too_long";
        return p;
    }
    // Загрузчик читает NroStart (0x10) и заголовок с "NRO0" и падает, если файла нет или он не NRO.
    FILE* f = std::fopen(self.c_str(), "rb");
    if (!f) {
        p.reason = "self_missing";
        return p;
    }
    uint8_t head[0x14] = {0};
    const bool got = std::fread(head, 1, sizeof(head), f) == sizeof(head);
    std::fclose(f);
    if (!got || std::memcmp(head + 0x10, "NRO0", 4) != 0) {
        p.reason = "not_nro";
        return p;
    }
    p.relaunch = true;
    p.path = self;
    p.argv = argv;
    p.reason = "ok";
    return p;
}

bool stagePending(const std::string& self, const std::vector<uint8_t>& data, std::string& error) {
    const std::string part = partPath(self), dest = pendingPath(self);
    if (fileExists(part)) std::remove(part.c_str());   // наш недописанный файл прошлой попытки
    FILE* f = std::fopen(part.c_str(), "wb");
    if (!f) {
        error = "write";
        return false;
    }
    const bool wrote = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    const bool closed = std::fclose(f) == 0;
    std::vector<uint8_t> back;
    if (!wrote || !closed) {
        std::remove(part.c_str());
        error = "write";
        return false;
    }
    if (!readAll(part, back) || back != data) {
        std::remove(part.c_str());
        error = "readback";
        return false;
    }
    if (fileExists(dest)) std::remove(dest.c_str());   // прежняя скачанная, ещё не применённая версия
    if (std::rename(part.c_str(), dest.c_str()) != 0) {
        std::remove(part.c_str());
        error = "rename";
        return false;
    }
    return true;
}

bool hasPending(const std::string& self, const std::string& current) {
    if (self.empty()) return false;
    const std::string src = pendingPath(self);
    std::vector<uint8_t> b;
    if (!readAll(src, b)) return false;
    const NroInfo n = readNro(b.data(), b.size());
    if (!n.ok) return false;   // битый .new не применяется
    if (isNewer(n.version, current)) return true;
    // Залежавшийся .new не новее работающей версии (её поставили иначе): не откатывать — отодвинуть в .new.stale.
    const std::string stale = stalePath(self);
    if (fileExists(stale)) std::remove(stale.c_str());   // наш же прошлый .new.stale
    std::rename(src.c_str(), stale.c_str());
    return false;
}

bool swapInPending(const std::string& self, const std::string& current, std::string& error) {
    if (!hasPending(self, current)) {
        error = "no_pending";
        return false;
    }
    const std::string src = pendingPath(self), old = oldPath(self);
    if (fileExists(old)) std::remove(old.c_str());   // .old прошлого обновления (новая версия уже стартовала)
    const bool moved = std::rename(self.c_str(), old.c_str()) == 0;
    if (!moved && fileExists(self)) {
        error = "move_old";   // свой .nro не отодвинулся — ничего не трогаем, .new применится в следующий раз
        return false;
    }
    if (std::rename(src.c_str(), self.c_str()) == 0) return true;   // .old остаётся до успешного старта новой
    error = "move_new";
    if (moved) std::rename(old.c_str(), self.c_str());   // вернуть старый
    return false;
}

std::string cleanupAfterStart(const std::string& self) {
    if (self.empty()) return "";
    // Своего .nro на месте нет (замена оборвалась снаружи) — мы и так запущены: ни .old, ни .new не трогать.
    if (!fileExists(self)) return "self_missing";
    const std::string old = oldPath(self);
    if (!fileExists(old)) return "";
    return std::remove(old.c_str()) == 0 ? "old_removed" : "old_kept";
}

}  // namespace mk::plat::update
