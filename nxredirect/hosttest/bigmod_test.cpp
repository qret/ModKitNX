/*
 * bigmod_test — обход модулей на БОЛЬШОМ модуле (файлы игры не нужны).
 *
 * Зачем. До 11.11.2026 смещение до MOD0 ограничивалось 64 МБ («больше любого
 * модуля игры»). У Biped 2 (Unity, весь код в main) main занимает ~121 МБ: MOD0
 * лежал дальше предела, main не распознавался, nnMain не находился, и модуль
 * обрывал игру с 0xDEAD0001 (отчёт Atmosphère с консоли). Здесь в памяти ПК
 * строится модуль, у которого MOD0 лежит в 80 МБ от начала, и проверяется:
 *   1. штатный обход находит в нём nnMain;
 *   2. прежний предел 64 МБ его НЕ находит (тест различает исправление);
 *   3. мусорное смещение — в неотображённую память или на байты без магии
 *      'MOD0' — модулем не считается и не роняет обход.
 *
 * Сборка и запуск — hosttest/run_hosttest.sh (блок до symfind_test), или:
 *   g++ -std=c++17 -O1 -Wall -Wextra -I../src -o bigmod_test bigmod_test.cpp && ./bigmod_test
 */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#include "nxr_symfind.hpp"

namespace {

int g_Fail = 0;
int g_Ran = 0;

void Check(bool cond, const char* what) {
    ++g_Ran;
    std::printf("   %s %s\n", cond ? "OK  " : "ПЛОХО", what);
    if (!cond) ++g_Fail;
}

/* Карта памяти процесса: отсортированные области; промежутки — «не отображено». */
struct Sim {
    std::vector<nxr::MemRegion> regions;

    bool operator()(std::uint64_t addr, nxr::MemRegion* out) const {
        std::uint64_t gapStart = 0;
        for (const auto& r : regions) {
            if (addr < r.addr) {
                *out = {gapStart, r.addr - gapStart, nxr::MemTypeUnmapped, nxr::PermNone};
                return true;
            }
            if (addr - r.addr < r.size) {
                *out = r;
                return true;
            }
            gapStart = r.addr + r.size;
        }
        return false;   /* за последней областью — ядро «отказало», обход закончен */
    }
};

constexpr std::uint64_t kCode     = 0x1000;      /* область кода (Rx) */
constexpr std::uint64_t kMod0At   = 0x5000000;   /* 80 МБ: дальше прежнего предела 64 МБ */
constexpr std::uint64_t kTotal    = kMod0At + 0x1000;
constexpr std::uint64_t kNnMainAt = 0x40;

/*
 * Модуль в buf: [0, kCode) — код, [kCode, kTotal) — данные только для чтения.
 * mod0Word — слово по +4 (смещение до MOD0); withMagic — писать ли настоящий MOD0.
 */
Sim BuildModule(std::uint8_t* buf, std::uint32_t mod0Word, bool withMagic) {
    std::memset(buf, 0, kTotal);
    std::uint32_t* w = reinterpret_cast<std::uint32_t*>(buf);
    w[0] = 0x14000000;          /* b . */
    w[1] = mod0Word;

    if (withMagic) {
        auto* mod0 = reinterpret_cast<nxr::Mod0*>(buf + kMod0At);
        std::memcpy(mod0->m_Magic, "MOD0", 4);
        mod0->m_DynamicStartOffset = 0x100;                         /* .dynamic = MOD0 + 0x100 */

        auto* dyn = reinterpret_cast<nxr::Dyn*>(buf + kMod0At + 0x100);
        dyn[0] = {nxr::DT_SYMTAB_, kMod0At + 0x200};
        dyn[1] = {nxr::DT_STRTAB_, kMod0At + 0x300};
        dyn[2] = {nxr::DT_STRSZ_, 8};
        dyn[3] = {nxr::DT_SYMENT_, sizeof(nxr::Sym)};
        dyn[4] = {nxr::DT_NULL_, 0};

        auto* syms = reinterpret_cast<nxr::Sym*>(buf + kMod0At + 0x200);
        syms[1] = {1, 0x12, 0, 1, kNnMainAt, 4};                    /* GLOBAL FUNC nnMain, st_shndx = 1 */
        std::memcpy(buf + kMod0At + 0x300, "\0nnMain\0", 8);
    }

    const auto base = reinterpret_cast<std::uint64_t>(buf);
    Sim sim;
    sim.regions.push_back({base, kCode, nxr::MemTypeCodeStatic, nxr::PermRx});
    sim.regions.push_back({base + kCode, kTotal - kCode, nxr::MemTypeCodeStatic, nxr::PermR});
    return sim;
}

}  // namespace

int main() {
    std::vector<std::uint8_t> mem(kTotal + 0x1000);
    /* Выравнивание на 0x1000, как у модулей на консоли. */
    auto* buf = reinterpret_cast<std::uint8_t*>(
        (reinterpret_cast<std::uintptr_t>(mem.data()) + 0xFFF) & ~static_cast<std::uintptr_t>(0xFFF));
    const auto base = reinterpret_cast<std::uint64_t>(buf);

    std::printf("[1] MOD0 в 80 МБ от начала модуля (main Biped 2 ~121 МБ)\n");
    {
        Sim sim = BuildModule(buf, static_cast<std::uint32_t>(kMod0At), true);
        const std::uint64_t got = nxr::FindSymbolAnywhere(sim, "nnMain");
        Check(got == base + kNnMainAt, "штатный обход находит nnMain");
        Check(nxr::CountModules(sim) == 1, "модуль распознан ровно один");

        nxr::Limits old {};
        old.maxMod0Offset = 0x4000000;
        Check(nxr::FindSymbolAnywhere(sim, "nnMain", old) == 0,
              "прежний предел 64 МБ модуль не находит (тест различает исправление)");
    }

    std::printf("[2] мусорное смещение до MOD0\n");
    {
        Sim sim = BuildModule(buf, 0xFFFFFFF0u, true);   /* за концом модуля — не отображено */
        Check(nxr::FindSymbolAnywhere(sim, "nnMain") == 0, "смещение в неотображённое — не модуль, без падения");
        Check(nxr::CountModules(sim) == 0, "модулей ноль");
    }
    {
        Sim sim = BuildModule(buf, static_cast<std::uint32_t>(kMod0At), false);  /* читаемо, но без 'MOD0' */
        Check(nxr::FindSymbolAnywhere(sim, "nnMain") == 0, "без магии MOD0 — не модуль");
        Check(nxr::CountModules(sim) == 0, "модулей ноль");
    }

    const int kMinChecks = 7;
    if (g_Ran < kMinChecks) {
        std::printf("КРАСНО: проверок выполнено %d из %d\n", g_Ran, kMinChecks);
        return 2;
    }
    std::printf(g_Fail == 0 ? "bigmod_test: ВСЁ ЗЕЛЁНОЕ (%d проверок)\n" : "bigmod_test: ПЛОХО %d\n",
                g_Fail == 0 ? g_Ran : g_Fail);
    return g_Fail == 0 ? 0 : 1;
}
