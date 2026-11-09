/*
 * logs_test.cpp — хост-тест ОТЧЁТОВ ОТЛАДОЧНОГО МОДУЛЯ (src/nxr_logs.hpp).
 *
 * Copyright (c) 2026, участники проекта nxredirect. GNU GPL версии 2.
 *
 * Отчёты — по играм, с номером, последние 5:
 *   nxrsd:/switch/ModKitNX/logs/<TID>/diag-NNNN.txt и gfx-NNNN.txt.
 * Что доказывается на ПК, без консоли (на поддельной ФС в памяти, которая, как
 * nn::fs, не создаёт папку без родителя и не открывает несуществующую):
 *   1. путь, звенья папок, имя файла и разбор номера — ровно такие;
 *   2. РОТАЦИЯ ОСТАВЛЯЕТ 5: после 12 запусков в папке ровно 5 отчётов вида,
 *      с наибольшими номерами, номера растут без повторов;
 *   3. виды (diag/gfx) и посторонние файлы друг друга не трогают;
 *   4. «грязная» папка (дыры в номерах, лишние файлы) приводится к 5;
 *   5. ФС без чтения папок — отчёт всё равно есть (номер 1);
 *   6. первая строка, дата из секунд POSIX, время с включения — по формату.
 * Плюс «затвор нуля»: тест, не выполнивший проверок, — красный. Негативные
 * пробы (испорченные копии заголовка) — в run_fsgatetest.sh.
 *
 * Сборка: g++ -std=c++20 -O2 -Wall -o logs_test logs_test.cpp -I. (рядом
 * nxr_logs.hpp и nxr_path.hpp). Код возврата: 0 — зелено, 1 — есть красные.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nxr_logs.hpp"

static int g_Checks = 0;
static int g_Failed = 0;

static void Expect(bool cond, const char* what, const std::string& detail = "") {
    ++g_Checks;
    if (cond) {
        std::printf("  ok   %s\n", what);
    } else {
        ++g_Failed;
        std::printf("  КРАСНО %s: %s\n", what, detail.c_str());
    }
}

/* Поддельная ФС: папки и файлы, как у nn::fs (без родителя папку не создать). */
struct FakeFs {
    std::set<std::string>                        dirs {"nxrsd:"};
    std::map<std::string, std::set<std::string>> files;
    bool listable = true;
    int  removes  = 0;

    static std::string Parent(const std::string& p) {
        const auto k = p.rfind('/');
        return k == std::string::npos ? std::string() : p.substr(0, k);
    }
    void MakeDir(const char* path) {
        const std::string p(path);
        if (dirs.count(Parent(p)) != 0) {
            dirs.insert(p);
        }
    }
    template <typename F>
    bool List(const char* dir, F&& f) {
        if (!listable || dirs.count(dir) == 0) {
            return false;
        }
        const std::set<std::string> snapshot = files[dir];
        for (const auto& name : snapshot) {
            f(name.c_str());
        }
        return true;
    }
    void Remove(const char* path) {
        const std::string p(path);
        files[Parent(p)].erase(p.substr(p.rfind('/') + 1));
        ++removes;
    }
    /* То, что делает модуль после Prepare: создать файл. false — нет папки. */
    bool Create(const std::string& p) {
        if (dirs.count(Parent(p)) == 0) {
            return false;
        }
        files[Parent(p)].insert(p.substr(p.rfind('/') + 1));
        return true;
    }
    std::vector<std::string> Kind(const std::string& dir, const char* prefix) {
        std::vector<std::string> out;
        for (const auto& n : files[dir]) {
            if (n.rfind(prefix, 0) == 0) {
                out.push_back(n);
            }
        }
        return out;
    }
};

static const std::uint64_t Tid = 0x01007A2027548000ull;
static const std::string   Dir = "nxrsd:/switch/ModKitNX/logs/01007A2027548000";

static std::string Join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) {
        s += x + " ";
    }
    return s;
}

/* Один запуск модуля: Prepare + создание файла. Возвращает номер (0 — отказ). */
static std::uint32_t Run(FakeFs& fs, const char* tmpl, std::string* pathOut = nullptr,
                         nxr::logs::Prepared* prep = nullptr) {
    char path[nxr::logs::PathSize] = {};
    const nxr::logs::Prepared r = nxr::logs::Prepare(fs, tmpl, Tid, path, sizeof(path));
    if (prep != nullptr) {
        *prep = r;
    }
    if (r.number == 0 || !fs.Create(path)) {
        return 0;
    }
    if (pathOut != nullptr) {
        *pathOut = path;
    }
    return r.number;
}

static void TestLayout() {
    std::printf("\n[1] путь, звенья папок, имя, разбор номера\n");
    nxr::logs::Layout L {};
    Expect(nxr::logs::Expand(&L, nxr::logs::DiagTemplate, Tid), "шаблон diag раскладывается");
    Expect(std::string(L.dir, L.dirLen) == Dir, "папка = nxrsd:/switch/ModKitNX/logs/<TID ЗАГЛАВНЫМИ>",
           std::string(L.dir, L.dirLen));
    Expect(std::string(L.prefix, L.prefixLen) == "diag-", "приставка diag-");
    std::vector<std::string> links;
    nxr::logs::ForEachDirLink(L, [&](const char* p) { links.emplace_back(p); });
    Expect(links.size() == 4 && links[0] == "nxrsd:/switch" && links[1] == "nxrsd:/switch/ModKitNX" &&
           links[2] == "nxrsd:/switch/ModKitNX/logs" && links[3] == Dir,
           "звенья папок по порядку от корня", Join(links));
    char p[nxr::logs::PathSize];
    nxr::logs::FilePath(p, sizeof(p), L, 1);
    Expect(std::string(p) == Dir + "/diag-0001.txt", "имя: 4 цифры с нулями", p);
    nxr::logs::FilePath(p, sizeof(p), L, 12345);
    Expect(std::string(p) == Dir + "/diag-12345.txt", "имя: больше 4 цифр — без обрезки", p);
    nxr::logs::Layout G {};
    Expect(nxr::logs::Expand(&G, nxr::logs::GfxTemplate, Tid) &&
           std::string(G.prefix, G.prefixLen) == "gfx-" && std::string(G.dir, G.dirLen) == Dir,
           "шаблон gfx: та же папка, приставка gfx-");
    Expect(std::strcmp(nxr::logs::KindPrefix(nxr::logs::GfxTemplate), "gfx-") == 0, "KindPrefix(gfx)");

    std::uint32_t n = 0;
    Expect(nxr::logs::ParseNumber("diag-0007.txt", "diag-", 5, &n) && n == 7, "разбор diag-0007.txt");
    Expect(nxr::logs::ParseNumber("diag-123456.txt", "diag-", 5, &n) && n == 123456, "разбор diag-123456.txt");
    const char* bad[] = {"gfx-0001.txt", "diag-0003 (1).txt", "diag-.txt", "diag-0001.txt.bak",
                         "diag-0001.TXT", "diag-0001", "xdiag-0001.txt", "diag-1234567890.txt", "readme.txt"};
    bool none = true;
    std::string which;
    for (const char* b : bad) {
        if (nxr::logs::ParseNumber(b, "diag-", 5, &n)) {
            none = false;
            which += b;
        }
    }
    Expect(none, "чужие и посторонние имена не разбираются как свои", which);
}

static void TestRotationKeepsFive() {
    std::printf("\n[2] ротация оставляет 5 (12 запусков с пустой карты)\n");
    FakeFs fs;
    bool grow = true, count = true, latest = true;
    std::string why;
    for (std::uint32_t run = 1; run <= 12; ++run) {
        const std::uint32_t n = Run(fs, nxr::logs::DiagTemplate);
        grow = grow && n == run;
        const auto have = fs.Kind(Dir, "diag-");
        const std::size_t want = run < 5 ? run : 5;
        if (have.size() != want) {
            count = false;
            why += "запуск " + std::to_string(run) + ": " + Join(have) + "; ";
        }
        for (std::uint32_t k = (run > 5 ? run - 4 : 1); k <= run; ++k) {
            char name[32];
            std::snprintf(name, sizeof(name), "diag-%04u.txt", k);
            latest = latest && fs.files[Dir].count(name) != 0;
        }
    }
    Expect(fs.dirs.count(Dir) != 0, "папки созданы с нуля по звеньям");
    Expect(grow, "номер растёт 1..12 без повторов");
    Expect(count, "в папке ровно min(запусков, 5) отчётов", why);
    Expect(latest, "остаются именно последние 5 (наибольшие номера)", Join(fs.Kind(Dir, "diag-")));
    Expect(fs.Kind(Dir, "diag-").size() == 5 && fs.Kind(Dir, "diag-").front() == "diag-0008.txt",
           "после 12 запусков: diag-0008..0012", Join(fs.Kind(Dir, "diag-")));
}

static void TestKindsAndForeign() {
    std::printf("\n[3] виды diag/gfx и посторонние файлы друг друга не трогают\n");
    FakeFs fs;
    Run(fs, nxr::logs::DiagTemplate);   /* создаёт папки */
    fs.Create(Dir + "/readme.txt");
    fs.Create(Dir + "/diag-0001 (1).txt");
    for (int i = 0; i < 7; ++i) {
        Run(fs, nxr::logs::DiagTemplate);
        Run(fs, nxr::logs::GfxTemplate);
    }
    Expect(fs.Kind(Dir, "diag-").size() == 5 + 1, "diag: 5 своих + посторонний «diag-0001 (1).txt»",
           Join(fs.Kind(Dir, "diag-")));
    Expect(fs.Kind(Dir, "gfx-").size() == 5, "gfx: ровно 5", Join(fs.Kind(Dir, "gfx-")));
    Expect(fs.files[Dir].count("readme.txt") == 1 && fs.files[Dir].count("diag-0001 (1).txt") == 1,
           "посторонние файлы целы");
    Expect(fs.files[Dir].count("gfx-0007.txt") == 1 && fs.files[Dir].count("diag-0008.txt") == 1,
           "номера видов независимы (diag до 8, gfx до 7)", Join(fs.Kind(Dir, "")));
    /* Другая игра — своя папка, чужие отчёты не трогаются. */
    char path[nxr::logs::PathSize];
    nxr::logs::Prepare(fs, nxr::logs::DiagTemplate, 0x0100F2C0115B6000ull, path, sizeof(path));
    Expect(std::string(path) == "nxrsd:/switch/ModKitNX/logs/0100F2C0115B6000/diag-0001.txt",
           "другая игра — своя папка, номер с 1", path);
    Expect(fs.Kind(Dir, "diag-").size() == 6, "отчёты первой игры не тронуты");
}

static void TestMessyFolder() {
    std::printf("\n[4] папка с дырами в номерах и лишними файлами\n");
    FakeFs fs;
    Run(fs, nxr::logs::DiagTemplate);
    for (const char* n : {"diag-0002.txt", "diag-0003.txt", "diag-0007.txt", "diag-0050.txt",
                          "diag-0051.txt", "diag-0052.txt", "diag-0100.txt"}) {
        fs.Create(Dir + "/" + n);
    }
    nxr::logs::Prepared r {};
    std::string path;
    const std::uint32_t n = Run(fs, nxr::logs::DiagTemplate, &path, &r);
    Expect(n == 101, "новый номер = наибольший + 1 (101)", std::to_string(n));
    Expect(r.found == 8 && r.deleted == 4, "было 8, удалено 4",
           std::to_string(r.found) + "/" + std::to_string(r.deleted));
    const auto have = fs.Kind(Dir, "diag-");
    Expect(have.size() == 5 && have[0] == "diag-0050.txt" && have[4] == "diag-0101.txt",
           "остались ровно 5 наибольших: 50, 51, 52, 100, 101", Join(have));
}

/* ФС, которая не умеет читать папки (нет символов каталогов). */
struct NoListFs : FakeFs {
    template <typename F>
    bool List(const char*, F&&) { return false; }
};

static void TestNoListing() {
    std::printf("\n[5] без чтения папок отчёт всё равно есть\n");
    NoListFs fs;
    char path[nxr::logs::PathSize];
    const auto r = nxr::logs::Prepare(fs, nxr::logs::DiagTemplate, Tid, path, sizeof(path));
    Expect(r.number == 1 && !r.listed && std::string(path) == Dir + "/diag-0001.txt",
           "номер 1, папка не прочитана", path);
    Expect(fs.Create(path), "папки созданы и без чтения — файл заводится");
}

static void TestFormats() {
    std::printf("\n[6] первая строка, дата, время с включения\n");
    nxr::logs::Head h {};
    h.titleId = Tid;
    h.haveBuildId = true;
    const unsigned char bid[20] = {0x5E, 0xA3, 0x94, 0xE0, 0x96, 0x75, 0x2E, 0xE3, 0xD0, 0x4D,
                                   0xEB, 0xFC, 0xEA, 0x6B, 0xF9, 0x11, 0x42, 0x77, 0xD0, 0xB4};
    std::memcpy(h.buildId, bid, 20);
    char buf[256];
    nxr::logs::FormatHead(buf, sizeof(buf), h, "diag-", 3);
    Expect(std::string(buf) ==
           "TitleID=01007A2027548000 BuildID=5EA394E096752EE3D04DEBFCEA6BF9114277D0B4 report=diag-0003",
           "первая строка: TitleID, BuildID, номер", buf);
    h.haveBuildId = false;
    nxr::logs::FormatHead(buf, sizeof(buf), h, "gfx-", 12);
    Expect(std::string(buf) == "TitleID=01007A2027548000 BuildID=? report=gfx-0012",
           "первая строка без BuildID — «?»", buf);

    struct { std::int64_t t; const char* want; } cases[] = {
        {0, "1970-01-01 00:00:00"},
        {951782400, "2000-02-29 00:00:00"},
        {1700000000, "2023-11-14 22:13:20"},
        {1794182400, "2026-11-09 00:00:00"},
    };
    for (const auto& c : cases) {
        int y, mo, d, hh, mi, s;
        nxr::logs::CivilFromPosix(c.t, &y, &mo, &d, &hh, &mi, &s);
        nxr::logs::Out o(buf, sizeof(buf));
        nxr::logs::AddDateTime(o, y, mo, d, hh, mi, s);
        Expect(std::string(buf) == c.want, "дата из секунд POSIX", std::string(buf) + " != " + c.want);
    }
    nxr::logs::Out o(buf, sizeof(buf));
    nxr::logs::AddUptime(o, nxr::logs::TickHz * 3725);
    Expect(std::string(buf) == "1:02:05", "время с включения из тиков 19,2 МГц", buf);
}

int main() {
    std::printf("=== хост-тест отчётов отладочного модуля (src/nxr_logs.hpp) ===\n");
    TestLayout();
    TestRotationKeepsFive();
    TestKindsAndForeign();
    TestMessyFolder();
    TestNoListing();
    TestFormats();
    std::printf("\nвыполнено проверок: %d, красных: %d\n", g_Checks, g_Failed);
    /* ЗАТВОР НУЛЯ: тест, который ничего не проверил, зелёным быть не может. */
    if (g_Checks < 30) {
        std::printf("КРАСНО: проверок выполнено слишком мало (%d)\n", g_Checks);
        return 1;
    }
    if (g_Failed != 0) {
        std::printf("ИТОГ: КРАСНО\n");
        return 1;
    }
    std::printf("ИТОГ: зелено\n");
    return 0;
}
