// Логи по играм и «Собрать логи для отправки». См. include/mk/logs.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mk/logs.hpp"

#include "mk/nacp.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>

namespace mk::logs {

namespace {

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
bool is_hex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool ends_with(const std::string& s, const std::string& e) {
    return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0;
}
std::string strip_bom(const std::string& t) {
    return t.size() >= 3 && static_cast<unsigned char>(t[0]) == 0xEF && static_cast<unsigned char>(t[1]) == 0xBB &&
                   static_cast<unsigned char>(t[2]) == 0xBF
               ? t.substr(3)
               : t;
}
// Первые 16 шестнадцатеричных цифр подряд, начиная с pos (после «0x», если есть).
std::string hex16_at(const std::string& s, size_t pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == ':' || s[pos] == '=')) ++pos;
    if (pos + 1 < s.size() && s[pos] == '0' && (s[pos + 1] == 'x' || s[pos + 1] == 'X')) pos += 2;
    size_t e = pos;
    while (e < s.size() && is_hex(s[e])) ++e;
    return e - pos == 16 ? upper(s.substr(pos, 16)) : std::string();
}
std::string read_text(IFileSystem& fs, const std::string& p, size_t max = 64 * 1024) {
    Bytes b = fs.read_range(p, 0, max);
    return std::string(b.begin(), b.end());
}

}  // namespace

// ------------------------------------------------------------ отчёты модуля
bool parse_report_name(const std::string& name, std::string& kind, uint32_t& number) {
    std::string k;
    if (name.rfind("diag-", 0) == 0) k = "diag";
    else if (name.rfind("gfx-", 0) == 0) k = "gfx";
    else return false;
    if (!ends_with(name, ".txt")) return false;
    const std::string digits = name.substr(k.size() + 1, name.size() - k.size() - 1 - 4);
    if (digits.size() < 4 || digits.size() > 8) return false;
    uint64_t n = 0;
    for (char c : digits) {
        if (!is_digit(c)) return false;
        n = n * 10 + static_cast<uint64_t>(c - '0');
    }
    kind = k;
    number = static_cast<uint32_t>(n);
    return true;
}

std::vector<ReportFile> list_reports(IFileSystem& fs, const std::string& logs_root, const std::string& tid,
                                     const std::string& kind) {
    std::vector<ReportFile> v;
    const std::string dir = path_join(logs_root, upper(tid));
    if (!fs.is_dir(dir)) return v;
    for (auto& e : fs.list_dir(dir)) {
        if (e.is_dir) continue;
        ReportFile r;
        if (!parse_report_name(e.name, r.kind, r.number) || r.kind != kind) continue;
        r.name = e.name;
        r.path = path_join(dir, e.name);
        v.push_back(std::move(r));
    }
    std::sort(v.begin(), v.end(), [](const ReportFile& a, const ReportFile& b) {
        return a.number != b.number ? a.number > b.number : a.name > b.name;
    });
    return v;
}

ReportHead parse_report_head(const std::string& text_in) {
    ReportHead h;
    const std::string t = strip_bom(text_in);
    const std::string line = t.substr(0, t.find_first_of("\r\n"));
    if (line.rfind("TitleID=", 0) != 0) return h;
    auto field = [&](const char* key) {
        const std::string k = std::string(key) + "=";
        size_t p = line.find(k);
        if (p == std::string::npos) return std::string();
        p += k.size();
        size_t e = line.find(' ', p);
        return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
    };
    h.tid = upper(field("TitleID"));
    h.build_id = field("BuildID");
    h.report = field("report");
    h.ok = h.tid.size() == 16 && std::all_of(h.tid.begin(), h.tid.end(), is_hex);
    return h;
}

bool legacy_report_mentions(const std::string& text, const std::string& tid) {
    return upper(text).find(upper(tid)) != std::string::npos;
}

// ------------------------------------------------------------ отчёты о падении
std::optional<uint64_t> leading_number(const std::string& name) {
    size_t e = 0;
    while (e < name.size() && is_digit(name[e])) ++e;
    if (e == 0 || e > 19) return std::nullopt;
    uint64_t n = 0;
    for (size_t i = 0; i < e; ++i) n = n * 10 + static_cast<uint64_t>(name[i] - '0');
    return n;
}

std::string crash_tid_from_name(const std::string& name) {
    // <время>_<16 hex>.log (Atmosphère creport: "%011lu_%016lx.log")
    size_t us = name.find('_');
    if (us == std::string::npos) return "";
    std::string t = name.substr(us + 1);
    size_t dot = t.find('.');
    if (dot != std::string::npos) t = t.substr(0, dot);
    if (t.size() != 16 || !std::all_of(t.begin(), t.end(), is_hex)) return "";
    return upper(t);
}

std::string crash_tid_from_text(const std::string& text) {
    for (const char* key : {"Program ID", "Title ID", "ProgramId", "TitleID"}) {
        size_t p = text.find(key);
        while (p != std::string::npos) {
            std::string h = hex16_at(text, p + std::char_traits<char>::length(key));
            if (!h.empty()) return h;
            p = text.find(key, p + 1);
        }
    }
    return "";
}

std::vector<CrashFile> list_crash_files(IFileSystem& fs, const std::string& dir, const MtimeFn& mtime) {
    std::vector<CrashFile> v;
    if (!fs.is_dir(dir)) return v;
    for (auto& e : fs.list_dir(dir)) {
        if (e.is_dir) continue;
        std::string low = e.name;
        for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!ends_with(low, ".log")) continue;
        CrashFile f;
        f.name = e.name;
        f.path = path_join(dir, e.name);
        if (mtime) f.mtime = mtime(f.path);
        v.push_back(std::move(f));
    }
    return v;
}

std::optional<CrashFile> pick_last_crash(const std::vector<CrashFile>& files, const std::string& tid,
                                         const ReadHeadFn& read_head) {
    const std::string want = upper(tid);
    std::vector<CrashFile> mine;
    for (const auto& f : files) {
        std::string t = crash_tid_from_name(f.name);
        if (t.empty() && read_head) t = crash_tid_from_text(read_head(f.path));
        if (t == want) mine.push_back(f);
    }
    if (mine.empty()) return std::nullopt;
    const bool all_dated = std::all_of(mine.begin(), mine.end(), [](const CrashFile& f) { return f.mtime.has_value(); });
    auto num = [](const CrashFile& f) { return leading_number(f.name).value_or(0); };
    auto older = [&](const CrashFile& a, const CrashFile& b) {   // a старше b
        if (all_dated && *a.mtime != *b.mtime) return *a.mtime < *b.mtime;
        if (num(a) != num(b)) return num(a) < num(b);
        return a.name < b.name;
    };
    return *std::max_element(mine.begin(), mine.end(), older);
}

// ------------------------------------------------------------ сборка
Bundle plan_logs_bundle(Env& env, const BundleInput& in, const MtimeFn& mtime) {
    Bundle b;
    const std::string tid = upper(in.tid);
    const std::string title = in.game_name.empty() ? tid : in.game_name;
    b.inner = safe_folder_name(in.game_version.empty() ? title : title + " v" + in.game_version, tid);
    b.folder = path_join(in.logs_root, b.inner) + "/";
    b.zip = path_join(in.logs_root, b.inner + " " + (in.stamp.empty() ? env.clock.stamp() : in.stamp) + ".zip");
    auto add_file = [&](const std::string& name, const std::string& src, const std::string& what) {
        BundleItem it;
        it.name = name;
        it.src = src;
        it.what = what;
        if (mtime) it.mtime = mtime(src);
        b.items.push_back(std::move(it));
        b.lines.add(name + "   <- " + src, "logs/put", {name, src});
    };
    // отчёты модуля: все лежащие (модуль хранит последние kKeep), новый формат; нет — старый файл в корне карты
    for (const std::string kind : {"diag", "gfx"}) {
        auto rep = list_reports(env.fs, in.logs_root, tid, kind);
        for (auto& r : rep) add_file(r.name, r.path, kind);
        bool legacy_added = false;
        const std::string& legacy = kind == "diag" ? in.legacy_diag : in.legacy_gfx;
        if (rep.empty() && !legacy.empty() && env.fs.is_file(legacy) &&
            legacy_report_mentions(read_text(env.fs, legacy), tid)) {
            add_file(path_name(legacy), legacy, kind);
            legacy_added = true;
        }
        if (kind == "diag" && rep.empty() && !legacy_added)
            b.warnings.add("отчётов отладки этой игры нет — поставьте «Отладочную сборку» и запустите игру",
                           "logs/no_diag");
    }
    // последний отчёт Atmosphère о падении этой игры
    auto crashes = list_crash_files(env.fs, in.crash_dir, mtime);
    auto last = pick_last_crash(crashes, tid, [&](const std::string& p) { return read_text(env.fs, p, 16 * 1024); });
    if (last) add_file("crash-" + last->name, last->path, "crash");
    else b.warnings.add("отчёта Atmosphère о падении этой игры нет", "logs/no_crash");
    // паспорта: приложения (прошивка, Atmosphère, emuMMC, ключи — из процесса игры их не прочесть) и модуля
    // (что загружено в процессе игры: sdk, исправления). Нужны ОБА: раньше паспорт модуля заменял паспорт
    // приложения под тем же именем, и прошивка с Atmosphère пропадали из логов (Biped 2, 11.11.2026).
    const std::string td = path_join(in.contents_dir, tid);
    const std::string mod_passport = path_join(td, "nxredirect-passport.txt");
    if (env.fs.is_file(mod_passport)) add_file("passport-module.txt", mod_passport, "passport");
    if (!in.passport_text.empty()) {
        BundleItem it;
        it.name = "passport.txt";
        it.data = std::make_shared<const Bytes>(in.passport_text.begin(), in.passport_text.end());
        it.what = "passport";
        b.items.push_back(std::move(it));
        b.lines.add("passport.txt   <- паспорт консоли (приложение)", "logs/put_passport_app", {"passport.txt"});
    }
    // тексты приложения (журнал чтения exefs и т. п.) — собраны в памяти, ключей в них нет
    for (const auto& t : in.app_texts) {
        if (t.first.empty() || t.second.empty()) continue;
        BundleItem it;
        it.name = t.first;
        it.data = std::make_shared<const Bytes>(t.second.begin(), t.second.end());
        it.what = "app";
        b.items.push_back(std::move(it));
        b.lines.add(t.first + "   <- журнал приложения", "logs/put_app_text", {t.first});
    }
    // квитанция игры
    const std::string receipt = path_join(td, kReceiptName);
    if (env.fs.is_file(receipt)) add_file(kReceiptName, receipt, "receipt");
    else b.warnings.add("квитанции нет — наш модуль у этой игры не стоит", "logs/no_receipt");
    // страховка: ни ключей, ни файлов игры (список собран явно, но проверяем ещё раз)
    for (auto& it : b.items) {
        std::string low = it.src;
        for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ends_with(low, ".keys") || ends_with(low, ".nca") || ends_with(low, ".nsp") || ends_with(low, ".xci") ||
            low.find("/exefs/") != std::string::npos || low.find("/romfs/") != std::string::npos)
            throw Error("FAIL_LOGS_FORBIDDEN", "в сборку логов попал запрещённый файл: " + it.src, {it.src});
    }
    b.lines.add(b.zip + "   <- архив (без сжатия)", "logs/put_zip", {b.zip});
    return b;
}

BundleResult write_logs_bundle(Env& env, const Bundle& b) {
    BundleResult r;
    r.folder = b.folder;
    r.zip = b.zip;
    uint16_t now_t = 0, now_d = 0;
    dos_from_stamp(env.clock.stamp(), now_t, now_d);
    std::vector<ZipEntry> z;
    env.fs.make_dirs(b.folder);
    for (const auto& it : b.items) {
        ZipEntry e;
        e.name = b.inner + "/" + it.name;
        e.data = it.data ? *it.data : env.fs.read_file(it.src);
        if (it.mtime) dos_datetime(*it.mtime, e.dos_time, e.dos_date);
        else {
            e.dos_time = now_t;
            e.dos_date = now_d;
        }
        env.fs.write_file_replace(path_join(b.folder, it.name), e.data.data(), e.data.size());
        z.push_back(std::move(e));
        r.files++;
    }
    Bytes zip = zip_store(z);
    env.fs.write_file_replace(b.zip, zip.data(), zip.size());
    r.zip_size = zip.size();
    return r;
}

// ------------------------------------------------------------ zip (store)
uint32_t crc32(const uint8_t* d, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void dos_datetime(int64_t secs, uint16_t& t, uint16_t& d) {
    std::time_t tt = static_cast<std::time_t>(secs);
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &tt);
#else
    gmtime_r(&tt, &tmv);
#endif
    int year = tmv.tm_year + 1900;
    if (year < 1980) {
        t = 0;
        d = (1 << 5) | 1;
        return;
    }
    if (year > 2107) year = 2107;
    t = static_cast<uint16_t>((tmv.tm_hour << 11) | (tmv.tm_min << 5) | (tmv.tm_sec / 2));
    d = static_cast<uint16_t>(((year - 1980) << 9) | ((tmv.tm_mon + 1) << 5) | tmv.tm_mday);
}

void dos_from_stamp(const std::string& s, uint16_t& t, uint16_t& d) {
    t = 0;
    d = (1 << 5) | 1;
    if (s.size() != 15 || s[8] != '-') return;
    for (size_t i = 0; i < s.size(); ++i)
        if (i != 8 && !is_digit(s[i])) return;
    auto n = [&](size_t a, size_t len) { return std::stoi(s.substr(a, len)); };
    int year = n(0, 4), mon = n(4, 2), day = n(6, 2), hh = n(9, 2), mm = n(11, 2), ss = n(13, 2);
    if (year < 1980 || year > 2107 || mon < 1 || mon > 12 || day < 1 || day > 31) return;
    t = static_cast<uint16_t>((hh << 11) | (mm << 5) | (ss / 2));
    d = static_cast<uint16_t>(((year - 1980) << 9) | (mon << 5) | day);
}

namespace {
void le16(Bytes& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void le32(Bytes& b, uint32_t v) {
    le16(b, v & 0xFFFF);
    le16(b, v >> 16);
}
}  // namespace

Bytes zip_store(const std::vector<ZipEntry>& entries) {
    Bytes out, cd;
    for (const auto& e : entries) {
        if (e.data.size() > 0xFFFFFFFEull || out.size() > 0xFFFFFFFEull)
            throw Error("FAIL_ZIP_TOO_BIG", "файл для архива больше 4 ГБ: " + e.name, {e.name});
        const uint32_t crc = crc32(e.data.data(), e.data.size());
        const uint32_t off = static_cast<uint32_t>(out.size());
        const uint32_t sz = static_cast<uint32_t>(e.data.size());
        const uint16_t flags = 0x0800;   // имена в UTF-8
        // локальный заголовок
        le32(out, 0x04034b50);
        le16(out, 10);   // нужна версия 1.0 (store)
        le16(out, flags);
        le16(out, 0);    // метод: store
        le16(out, e.dos_time);
        le16(out, e.dos_date);
        le32(out, crc);
        le32(out, sz);
        le32(out, sz);
        le16(out, static_cast<uint32_t>(e.name.size()));
        le16(out, 0);
        out.insert(out.end(), e.name.begin(), e.name.end());
        out.insert(out.end(), e.data.begin(), e.data.end());
        // центральный каталог
        le32(cd, 0x02014b50);
        le16(cd, 0x031E);   // сделано: UNIX, 3.0
        le16(cd, 10);
        le16(cd, flags);
        le16(cd, 0);
        le16(cd, e.dos_time);
        le16(cd, e.dos_date);
        le32(cd, crc);
        le32(cd, sz);
        le32(cd, sz);
        le16(cd, static_cast<uint32_t>(e.name.size()));
        le16(cd, 0);   // extra
        le16(cd, 0);   // comment
        le16(cd, 0);   // disk
        le16(cd, 0);   // внутренние атрибуты
        le32(cd, 0100644u << 16);   // внешние: обычный файл rw-r--r--
        le32(cd, off);
        cd.insert(cd.end(), e.name.begin(), e.name.end());
    }
    if (entries.size() > 0xFFFF) throw Error("FAIL_ZIP_TOO_MANY", "слишком много файлов для архива");
    const uint32_t cd_off = static_cast<uint32_t>(out.size());
    out.insert(out.end(), cd.begin(), cd.end());
    le32(out, 0x06054b50);
    le16(out, 0);
    le16(out, 0);
    le16(out, static_cast<uint32_t>(entries.size()));
    le16(out, static_cast<uint32_t>(entries.size()));
    le32(out, static_cast<uint32_t>(cd.size()));
    le32(out, cd_off);
    le16(out, 0);
    return out;
}

}  // namespace mk::logs
