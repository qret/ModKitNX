/*
 * rofix_test — поиск символов nn::ro в старой sdk (src/nxr_rofix.hpp).
 * Файлы игры и sdk не нужны.
 *
 * [1] Где внутренняя функция поиска: первый `bl` в nn::ro::LookupModuleSymbol
 *     после `ldr x0, [x1]` и `mov x1, x2`. Слова — по разбору nnSdk 19.3.13
 *     (@0x14b180 -> 0x14b1e0) и 20.5.14 (@0x14eb90 -> 0x14ebf0); это обычный
 *     пролог и три инструкции, записанные кодировками.
 * [2] Старая ли она: 19.3.13 — `ldr x8, [x0, #80]` сразу после пролога (только
 *     DT_HASH); 20.5.14 — сначала `ldrb w9, [x0, #195]` (флаг DT_GNU_HASH).
 * [3] Свой поиск в модуле без DT_HASH: отсевы как у оригинала, имя, DT_HASH-флаг.
 *
 *   g++ -std=c++17 -O1 -Wall -Wextra -o rofix_test rofix_test.cpp && ./rofix_test
 * (в run_hosttest.sh — рядом с bigmod_test).
 */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#include "nxr_rofix.hpp"

namespace {

int g_Fail = 0;
int g_Ran = 0;

void Check(bool cond, const char* what) {
    ++g_Ran;
    std::printf("   %s %s\n", cond ? "OK  " : "ПЛОХО", what);
    if (!cond) ++g_Fail;
}

/* Кодировки инструкций AArch64, из которых состоят образцы. */
constexpr std::uint32_t StpX29X30Pre32 = 0xA9BE7BFD;   /* stp x29, x30, [sp, #-32]! */
constexpr std::uint32_t StpX29X30Pre80 = 0xA9BB7BFD;   /* stp x29, x30, [sp, #-80]! */
constexpr std::uint32_t StpX20X19At16  = 0xA9014FF4;   /* stp x20, x19, [sp, #16]   */
constexpr std::uint32_t StpX26X25At16  = 0xA90167FA;   /* stp x26, x25, [sp, #16]   */
constexpr std::uint32_t StpX24X23At32  = 0xA9025FF8;
constexpr std::uint32_t StpX22X21At48  = 0xA90357F6;
constexpr std::uint32_t StpX20X19At64  = 0xA9044FF4;
constexpr std::uint32_t MovX29Sp       = 0x910003FD;   /* mov x29, sp   */
constexpr std::uint32_t MovX19X0       = 0xAA0003F3;   /* mov x19, x0   */
constexpr std::uint32_t MovX20X1       = 0xAA0103F4;   /* mov x20, x1   */
constexpr std::uint32_t MovX19X1       = 0xAA0103F3;   /* mov x19, x1   */
constexpr std::uint32_t LdrX8X0At80    = 0xF9402808;   /* ldr x8, [x0, #80]   */
constexpr std::uint32_t LdrbW9X0At195  = 0x39430C09;   /* ldrb w9, [x0, #195] */

std::uint32_t Bl(std::uint64_t from, std::uint64_t to) {
    const std::int64_t d = (static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from)) / 4;
    return 0x94000000u | (static_cast<std::uint32_t>(d) & 0x03FFFFFFu);
}

/* ---- синтетический модуль: MOD0, .dynamic, .dynsym, .dynstr ---- */
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
        return false;
    }
};

constexpr std::uint64_t kCode = 0x1000, kTotal = 0x2000, kMod0 = 0x1000;

/* Модуль с символами: [1] FILE «foo», [2] неопределённый «foo», [3] COMMON «foo»,
   [4] определённый «foo» (FUNC GLOBAL, 0x40), [5] «bar» (0x80). withHash — DT_HASH есть. */
void Build(std::uint8_t* buf, bool withHash) {
    std::memset(buf, 0, kTotal);
    reinterpret_cast<std::uint32_t*>(buf)[0] = 0x14000000;
    reinterpret_cast<std::uint32_t*>(buf)[1] = static_cast<std::uint32_t>(kMod0);
    auto* mod0 = reinterpret_cast<nxr::Mod0*>(buf + kMod0);
    std::memcpy(mod0->m_Magic, "MOD0", 4);
    mod0->m_DynamicStartOffset = 0x40;
    auto* dyn = reinterpret_cast<nxr::Dyn*>(buf + kMod0 + 0x40);
    int i = 0;
    dyn[i++] = {nxr::DT_SYMTAB_, kMod0 + 0x200};
    dyn[i++] = {nxr::DT_STRTAB_, kMod0 + 0x300};
    dyn[i++] = {nxr::DT_STRSZ_, 9};
    if (withHash) dyn[i++] = {nxr::DT_HASH_, kMod0 + 0x400};
    dyn[i++] = {nxr::DT_NULL_, 0};
    auto* s = reinterpret_cast<nxr::Sym*>(buf + kMod0 + 0x200);
    s[1] = {1, 0x14, 0, 1, 0x10, 0};        /* STT_FILE «foo» */
    s[2] = {1, 0x12, 0, 0, 0, 0};           /* неопределённый «foo» */
    s[3] = {1, 0x11, 0, 0xFFF2, 0x20, 8};   /* SHN_COMMON «foo» */
    s[4] = {1, 0x12, 0, 1, 0x40, 4};        /* определённый «foo» */
    s[5] = {5, 0x12, 0, 1, 0x80, 4};        /* «bar» */
    std::memcpy(buf + kMod0 + 0x300, "\0foo\0bar\0", 9);
}

Sim Map(std::uint8_t* buf) {
    const auto base = reinterpret_cast<std::uint64_t>(buf);
    Sim sim;
    sim.regions.push_back({base, kCode, nxr::MemTypeCodeStatic, nxr::PermRx});
    sim.regions.push_back({base + kCode, kTotal - kCode, nxr::MemTypeCodeStatic, nxr::PermR});
    return sim;
}

}  // namespace

int main() {
    using namespace nxr::rofix;

    std::printf("[1] внутренняя функция поиска по коду LookupModuleSymbol\n");
    {
        const std::uint64_t at19 = 0x14b180, f19 = 0x14b1e0;
        const std::uint32_t lms19[] = {StpX29X30Pre32, StpX20X19At16, MovX29Sp, MovX19X0,
                                       InsnLdrX0X1, MovX20X1, InsnMovX1X2, Bl(at19 + 7 * 4, f19)};
        Check(FindInternalLookup(lms19, at19, 8) == f19, "19.3.13: 0x14b180 -> 0x14b1e0");
        const std::uint64_t at20 = 0x14eb90, f20 = 0x14ebf0;
        const std::uint32_t lms20[] = {StpX29X30Pre32, StpX20X19At16, MovX29Sp, MovX19X0,
                                       InsnLdrX0X1, MovX20X1, InsnMovX1X2, Bl(at20 + 7 * 4, f20)};
        Check(FindInternalLookup(lms20, at20, 8) == f20, "20.5.14: 0x14eb90 -> 0x14ebf0");
        std::uint32_t noMov[8];
        std::memcpy(noMov, lms19, sizeof(noMov));
        noMov[6] = MovX20X1;
        Check(FindInternalLookup(noMov, at19, 8) == 0, "без `mov x1, x2` перед bl — образец не узнан");
        std::uint32_t noObj[8];
        std::memcpy(noObj, lms19, sizeof(noObj));
        noObj[4] = MovX19X0;
        Check(FindInternalLookup(noObj, at19, 8) == 0, "без `ldr x0, [x1]` — образец не узнан");
        Check(FindInternalLookup(nullptr, at19) == 0, "нет кода — 0");
    }

    std::printf("[2] старая ли функция (только DT_HASH)\n");
    {
        const std::uint32_t f19[] = {StpX29X30Pre80, StpX26X25At16, StpX24X23At32, StpX22X21At48,
                                     StpX20X19At64, MovX29Sp, LdrX8X0At80, MovX19X1};
        Check(IsSysvOnly(f19, 8), "19.3.13: ldr x8, [x0, #80] после пролога — старая");
        const std::uint32_t f20[] = {StpX29X30Pre80, StpX26X25At16, StpX24X23At32, StpX22X21At48,
                                     StpX20X19At64, MovX29Sp, LdrbW9X0At195, MovX19X1, LdrX8X0At80};
        Check(!IsSysvOnly(f20, 9), "20.5.14: сначала флаг obj+195 — новая, не трогаем");
        const std::uint32_t other[] = {StpX29X30Pre80, MovX29Sp, MovX19X1, MovX20X1};
        Check(!IsSysvOnly(other, 4), "непохожий код — не трогаем");
    }

    std::printf("[3] свой поиск в модуле\n");
    std::vector<std::uint8_t> mem(kTotal + 0x1000);
    auto* buf = reinterpret_cast<std::uint8_t*>(
        (reinterpret_cast<std::uintptr_t>(mem.data()) + 0xFFF) & ~static_cast<std::uintptr_t>(0xFFF));
    const auto base = reinterpret_cast<std::uint64_t>(buf);
    {
        Build(buf, false);
        Sim sim = Map(buf);
        const nxr::Sym* foo = FindDefinedSym(sim, base, "foo");
        Check(foo != nullptr && foo->st_value == 0x40,
              "«foo»: пропущены FILE, неопределённый и COMMON — найден определённый");
        const nxr::Sym* bar = FindDefinedSym(sim, base, "bar");
        Check(bar != nullptr && bar->st_value == 0x80, "«bar» найден");
        Check(FindDefinedSym(sim, base, "baz") == nullptr, "нет такого — nullptr");
        Check(FindDefinedSym(sim, base, "") == nullptr, "пустое имя — nullptr");
        Check(!HasSysvHash(sim, base), "DT_HASH нет — HasSysvHash = false");
        Check(AnyModuleWithoutSysvHash(sim), "в процессе есть модуль без DT_HASH");
        Check(FindDefinedSym(sim, base + kCode, "foo") == nullptr, "не база модуля — nullptr, без падения");
    }
    {
        Build(buf, true);
        Sim sim = Map(buf);
        Check(HasSysvHash(sim, base), "DT_HASH есть — HasSysvHash = true (идём в оригинал)");
        Check(!AnyModuleWithoutSysvHash(sim), "у всех модулей DT_HASH — перехват не нужен");
    }

    const int kMinChecks = 17;
    if (g_Ran < kMinChecks) {
        std::printf("КРАСНО: проверок выполнено %d из %d\n", g_Ran, kMinChecks);
        return 2;
    }
    std::printf(g_Fail == 0 ? "rofix_test: ВСЁ ЗЕЛЁНОЕ (%d проверок)\n" : "rofix_test: ПЛОХО %d\n",
                g_Fail == 0 ? g_Ran : g_Fail);
    return g_Fail == 0 ? 0 : 1;
}
