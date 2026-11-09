/*
 * symfind_test — проверка на ПК той же логики, что исполняется в модуле на консоли.
 *
 * Copyright (c) 2026, участники проекта nxredirect
 * Лицензия GNU GPL версии 2 (как и весь модуль, основанный на exlaunch).
 *
 * Что делает
 * ----------
 * 1. Читает НАСТОЯЩИЕ модули игры (rtld, main, subsdk0..9, sdk из распакованного
 *    exefs) и наш собранный subsdkN, распаковывает сегменты NSO (LZ4) в образ.
 *
 * ГДЕ БЕРУТСЯ МОДУЛИ ИГРЫ. Папка задаётся первым аргументом или переменной среды
 * NXR_GAME_EXEFS. Если её нет или в ней нет модулей — тест НЕ «пропускается»
 * молча, а КРАСНЕЕТ (код возврата 2). Иначе гейт, который ничего не проверил,
 * выглядел бы как пройденный, а это худший вид проверки: ровно на такую тихо
 * пропущенную проверку и было наше открытое замечание к прежнему проекту.
 * Модули игры не распространяются: их берут из своего дампа.
 * 2. Раскладывает образы в адресном пространстве этого процесса через mmap по тем же
 *    правилам, по каким их раскладывает загрузчик Switch: text (Rx, CodeStatic),
 *    rodata (R, CodeStatic), data (Rw, CodeMutable) — с настоящими дырами между
 *    модулями. У nnSdk дополнительно моделируется relro: ЧЕТЫРЕ области вместо трёх.
 * 3. Подменяет svcQueryMemory заглушкой, которая отдаёт эту раскладку, и прогоняет
 *    ровно тот код, что и модуль: nxr::ForEachModule / FindSymbolIn /
 *    FindSymbolAnywhere из ../exlaunch/source/program/nxr_symfind.hpp.
 *
 * Так ошибки вида «MOD0 лежит за пределами области кода» (проба на консоли,
 * код 0xDEAD0001) и «у модуля четыре области, а не три» (код 0xD1040400) ловятся
 * сборкой на ПК, а не запуском игры.
 *
 * Запуск:  symfind_test <папка exefs игры> <путь к нашему subsdkN>
 *          NXR_GAME_EXEFS=<папка> symfind_test - <путь к нашему subsdkN>
 * Код возврата 0 — все пробы как надо, 1 — проба провалена, 2 — нечем проверять.
 */

#include "nxr_symfind.hpp"

#include <sys/mman.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

namespace {

/* ------------------------------------------------------------------ NSO */

/* LZ4 (block format) — тот же разбор, что в scratchpad/tools/nsosym.py. */
std::vector<std::uint8_t> Lz4(const std::uint8_t* src, std::size_t srcLen, std::size_t outLen) {
    std::vector<std::uint8_t> dst;
    dst.reserve(outLen);
    std::size_t i = 0;
    while (i < srcLen) {
        const std::uint8_t token = src[i++];
        std::size_t lit = token >> 4;
        if (lit == 15) {
            std::uint8_t b;
            do { b = src[i++]; lit += b; } while (b == 255 && i < srcLen);
        }
        for (std::size_t k = 0; k < lit && i < srcLen; ++k) dst.push_back(src[i++]);
        if (i + 1 >= srcLen) break;
        const std::size_t off = static_cast<std::size_t>(src[i]) |
                                (static_cast<std::size_t>(src[i + 1]) << 8);
        i += 2;
        std::size_t m = token & 15;
        if (m == 15) {
            std::uint8_t b;
            do { b = src[i++]; m += b; } while (b == 255 && i < srcLen);
        }
        m += 4;
        if (off == 0 || off > dst.size()) break;
        const std::size_t s = dst.size() - off;
        for (std::size_t k = 0; k < m; ++k) dst.push_back(dst[s + k]);
    }
    return dst;
}

struct Nso {
    std::string               name;
    std::vector<std::uint8_t> image;   /* образ модуля, как он лежит в памяти */
    std::uint64_t             textEnd = 0;   /* = смещение rodata */
    std::uint64_t             roEnd   = 0;   /* = смещение data */
    std::uint64_t             total   = 0;   /* с учётом bss, до границы страницы */
};

std::vector<std::uint8_t> ReadFile(const std::string& path, bool* ok) {
    std::vector<std::uint8_t> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) { *ok = false; return out; }
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(len > 0 ? static_cast<std::size_t>(len) : 0);
    *ok = out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return out;
}

std::uint32_t U32(const std::vector<std::uint8_t>& d, std::size_t off) {
    std::uint32_t v = 0;
    std::memcpy(&v, d.data() + off, 4);
    return v;
}

constexpr std::uint64_t PageSize = 0x1000;
std::uint64_t PageUp(std::uint64_t v) { return (v + PageSize - 1) & ~(PageSize - 1); }

bool LoadNso(const std::string& path, const std::string& name, Nso* out) {
    bool ok = false;
    const std::vector<std::uint8_t> d = ReadFile(path, &ok);
    if (!ok || d.size() < 0x100 || std::memcmp(d.data(), "NSO0", 4) != 0) {
        std::printf("   не NSO или не прочитался: %s\n", path.c_str());
        return false;
    }
    const std::uint32_t flags = U32(d, 0x0C);
    std::vector<std::uint8_t> image;
    std::uint32_t moff[3] = {0, 0, 0};
    std::uint32_t msize[3] = {0, 0, 0};
    for (int n = 0; n < 3; ++n) {
        const std::uint32_t foff = U32(d, 0x10 + n * 0x10 + 0);
        moff[n]  = U32(d, 0x10 + n * 0x10 + 4);
        msize[n] = U32(d, 0x10 + n * 0x10 + 8);
        const std::uint32_t csize = U32(d, 0x60 + n * 4);
        if (foff + csize > d.size()) return false;
        std::vector<std::uint8_t> seg;
        if (flags & (1u << n)) {
            seg = Lz4(d.data() + foff, csize, msize[n]);
        } else {
            seg.assign(d.begin() + foff, d.begin() + foff + csize);
        }
        if (image.size() < moff[n]) image.resize(moff[n], 0);
        if (image.size() < static_cast<std::size_t>(moff[n]) + msize[n])
            image.resize(static_cast<std::size_t>(moff[n]) + msize[n], 0);
        std::memcpy(image.data() + moff[n], seg.data(), std::min<std::size_t>(seg.size(), msize[n]));
    }
    const std::uint32_t bss = U32(d, 0x3C);
    out->name    = name;
    out->textEnd = moff[1];
    out->roEnd   = moff[2];
    out->total   = PageUp(static_cast<std::uint64_t>(moff[2]) + msize[2] + bss);
    image.resize(out->total, 0);
    out->image = std::move(image);
    return true;
}

/* --------------------------------------------------- адресное пространство */

struct Sim {
    std::vector<nxr::MemRegion> parts;   /* полное разбиение адресного пространства */
    std::vector<std::uint64_t>    bases;   /* база каждого модуля, по порядку */
    std::vector<void*>            maps;    /* что освобождать */
    std::vector<std::uint64_t>    mapLens;
    int                           hashStripped = 0;   /* у скольких модулей снят DT_HASH */

    bool operator()(std::uint64_t addr, nxr::MemRegion* out) const {
        std::size_t lo = 0, hi = parts.size();
        while (lo + 1 < hi) {
            const std::size_t mid = (lo + hi) / 2;
            if (parts[mid].addr <= addr) lo = mid; else hi = mid;
        }
        *out = parts[lo];
        return true;
    }

    void Free() {
        for (std::size_t i = 0; i < maps.size(); ++i) munmap(maps[i], mapLens[i]);
        maps.clear();
        mapLens.clear();
    }
};

struct Mapped {
    std::uint64_t addr, size;
    std::uint32_t type, perm;
};

/* Как ломать раскладку — для негативных проб. */
struct Damage {
    int  badMod0OffsetModule = -1;   /* слово по +4 указывает в неотображённое */
    int  badMod0MagicModule  = -1;   /* магия MOD0 испорчена */
    bool foreignCodeRegion   = false; /* чужая Rx-область (как у внедрённого saltysd_core) */
    bool sdkRelro            = true;  /* у nnSdk четыре области вместо трёх */
    /*
     * Убрать DT_HASH из .dynamic КАЖДОГО модуля (запись DT_HASH
     * становится DT_DEBUG — тегом, который поиск символов не читает). Так из одной
     * и той же игры получаются обе раскладки: «таблица есть» (как у TotK) и
     * «таблицы нет» (как у Well Dweller и SiN) — поиск обязан дать одно и то же.
     */
    bool stripHash           = false;
};

/* Базы модулей: с большими дырами, как в настоящем процессе после ASLR. */
constexpr std::uint64_t SimBase   = 0x0000600000000000ull;
constexpr std::uint64_t SimStride = 0x0000000100000000ull;   /* 4 ГБ между модулями */

bool BuildSim(const std::vector<Nso>& mods, const Damage& dmg, Sim* sim) {
    std::vector<Mapped> mapped;

    /*
     * Чужая область исполняемого кода перед всеми модулями. На консоли такая
     * появляется от SaltyNX: saltysd_core внедряется в процесс игры (например,
     * у сборок CFW с SaltyNX). У неё нет MOD0, и наш обход обязан её пропустить,
     * а не принять за модуль и не упасть.
     */
    if (dmg.foreignCodeRegion) {
        const std::uint64_t addr = SimBase - 0x10000000ull;
        void* p = mmap(reinterpret_cast<void*>(addr), PageSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) { std::printf("   mmap (чужая область) не удался\n"); return false; }
        sim->maps.push_back(p);
        sim->mapLens.push_back(PageSize);
        /* Слово по +4 — мусор, указывающий далеко в неотображённое. */
        reinterpret_cast<std::uint32_t*>(p)[0] = 0x14000000;
        reinterpret_cast<std::uint32_t*>(p)[1] = 0x00300000;
        mprotect(p, PageSize, PROT_READ);
        mapped.push_back({addr, PageSize, nxr::MemTypeCodeStatic, nxr::PermRx});
    }

    for (std::size_t i = 0; i < mods.size(); ++i) {
        const Nso& m = mods[i];
        const std::uint64_t base = SimBase + i * SimStride;
        void* p = mmap(reinterpret_cast<void*>(base), m.total, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) {
            std::printf("   mmap по адресу %#llx не удался (модуль %s)\n",
                        (unsigned long long)base, m.name.c_str());
            return false;
        }
        sim->maps.push_back(p);
        sim->mapLens.push_back(m.total);
        std::memcpy(p, m.image.data(), m.image.size());

        auto* bytes = static_cast<std::uint8_t*>(p);

        if (static_cast<int>(i) == dmg.badMod0OffsetModule) {
            /* Смещение до MOD0 указывает далеко за пределы модуля, в пустоту. */
            std::uint32_t bad = 0x00F00000;
            std::memcpy(bytes + 4, &bad, 4);
        }
        if (static_cast<int>(i) == dmg.badMod0MagicModule) {
            const std::uint32_t off = U32(m.image, 4);
            if (off + 4 <= m.total) std::memcpy(bytes + off, "XXXX", 4);
        }
        if (dmg.stripHash) {
            /* .dynamic по MOD0 образа; DT_HASH (4) -> DT_DEBUG (21). */
            const std::uint32_t modOff = U32(m.image, 4);
            if (modOff + 8 <= m.image.size() &&
                std::memcmp(m.image.data() + modOff, "MOD0", 4) == 0) {
                std::int32_t dynRel = 0;
                std::memcpy(&dynRel, m.image.data() + modOff + 4, 4);
                bool stripped = false;
                for (std::size_t o = static_cast<std::size_t>(modOff + dynRel);
                     o + 16 <= m.image.size(); o += 16) {
                    std::int64_t tag = 0;
                    std::memcpy(&tag, bytes + o, 8);
                    if (tag == 0) break;
                    if (tag == 4) {
                        const std::int64_t debugTag = 21;
                        std::memcpy(bytes + o, &debugTag, 8);
                        stripped = true;
                    }
                }
                if (stripped) ++sim->hashStripped;
            }
        }

        /* Три области, как их делает загрузчик Switch. */
        mprotect(bytes, m.textEnd, PROT_READ);
        mapped.push_back({base, m.textEnd, nxr::MemTypeCodeStatic, nxr::PermRx});
        mprotect(bytes + m.textEnd, m.roEnd - m.textEnd, PROT_READ);
        mapped.push_back({base + m.textEnd, m.roEnd - m.textEnd,
                          nxr::MemTypeCodeStatic, nxr::PermR});

        /*
         * У nnSdk (`sdk`) начало сегмента данных после релокации переводится в режим
         * только для чтения (relro) и становится ОТДЕЛЬНОЙ, ЧЕТВЁРТОЙ областью.
         * Именно из-за неё сканер exlaunch не находил nnSdk (проба на консоли:
         * код 0xD1040400: 4 модуля из 5).
         */
        const bool relro = dmg.sdkRelro && m.name == "sdk";
        if (relro) {
            const std::uint64_t relroSize = 0x10000;
            mapped.push_back({base + m.roEnd, relroSize,
                              nxr::MemTypeCodeMutable, nxr::PermR});
            mapped.push_back({base + m.roEnd + relroSize, m.total - m.roEnd - relroSize,
                              nxr::MemTypeCodeMutable, nxr::PermRw});
        } else {
            mapped.push_back({base + m.roEnd, m.total - m.roEnd,
                              nxr::MemTypeCodeMutable, nxr::PermRw});
        }
        sim->bases.push_back(base);
    }

    /* Полное разбиение адресного пространства: дыры между областями — Unmapped. */
    std::sort(mapped.begin(), mapped.end(),
              [](const Mapped& a, const Mapped& b) { return a.addr < b.addr; });
    std::uint64_t cursor = 0;
    for (const Mapped& m : mapped) {
        if (m.addr > cursor) sim->parts.push_back({cursor, m.addr - cursor, nxr::MemTypeUnmapped, 0});
        sim->parts.push_back({m.addr, m.size, m.type, m.perm});
        cursor = m.addr + m.size;
    }
    /* Хвост до конца адресного пространства: addr + size обнуляется — как у ядра. */
    sim->parts.push_back({cursor, 0ull - cursor, nxr::MemTypeUnmapped, 0});
    return true;
}

/* ------------------------------------------- независимый разбор по образу */

/*
 * Второй, НЕЗАВИСИМЫЙ способ найти символ: прямо по образу файла, индексами, без
 * адресов и без кода из nxr_symfind.hpp. Нужен, чтобы ожидаемое значение не
 * бралось из той же логики, которую мы и проверяем.
 */
std::uint64_t LookupInImage(const Nso& m, const char* name, bool* hasHash) {
    const std::vector<std::uint8_t>& img = m.image;
    const std::uint32_t modOff = U32(img, 4);
    if (modOff + 8 > img.size() || std::memcmp(img.data() + modOff, "MOD0", 4) != 0) return 0;
    std::int32_t dynRel = 0;
    std::memcpy(&dynRel, img.data() + modOff + 4, 4);
    std::size_t o = static_cast<std::size_t>(modOff + dynRel);
    std::uint64_t strtab = 0, symtab = 0, strsz = 0;
    if (hasHash) *hasHash = false;
    while (o + 16 <= img.size()) {
        std::int64_t tag = 0;
        std::uint64_t val = 0;
        std::memcpy(&tag, img.data() + o, 8);
        std::memcpy(&val, img.data() + o + 8, 8);
        o += 16;
        if (tag == 0) break;
        if (tag == 4 && hasHash) *hasHash = true;
        if (tag == 5) strtab = val;
        if (tag == 6) symtab = val;
        if (tag == 10) strsz = val;
    }
    if (strtab == 0 || symtab == 0 || strsz == 0 || symtab >= strtab) return 0;
    const std::size_t len = std::strlen(name);
    for (std::uint64_t p = symtab; p + 24 <= strtab; p += 24) {
        std::uint32_t stName = 0;
        std::uint16_t shndx = 0;
        std::uint64_t value = 0;
        std::memcpy(&stName, img.data() + p, 4);
        std::memcpy(&shndx, img.data() + p + 6, 2);
        std::memcpy(&value, img.data() + p + 8, 8);
        if (shndx == 0 || value == 0 || stName >= strsz) continue;
        const char* s = reinterpret_cast<const char*>(img.data() + strtab + stName);
        if (std::strncmp(s, name, len) == 0 && s[len] == '\0') return value;
    }
    return 0;
}

/*
 * НЕЗАВИСИМЫЙ поиск слота импорта по образу (а не по смоделированной памяти):
 * смещение r_offset релокации JUMP_SLOT/GLOB_DAT/ABS64 на НЕОПРЕДЕЛЁННЫЙ символ
 * с таким именем. Своя реализация, чтобы сверять с nxr::FindImportSlotIn.
 */
std::uint64_t ImportSlotInImage(const Nso& m, const char* name) {
    const std::vector<std::uint8_t>& img = m.image;
    if (img.size() < 8) return 0;
    const std::uint32_t modOff = U32(img, 4);
    if (modOff + 8 > img.size() || std::memcmp(img.data() + modOff, "MOD0", 4) != 0) return 0;
    std::int32_t dynRel = 0;
    std::memcpy(&dynRel, img.data() + modOff + 4, 4);
    std::size_t o = static_cast<std::size_t>(modOff + dynRel);
    std::uint64_t strtab = 0, symtab = 0, strsz = 0, rela = 0, relasz = 0, jmprel = 0, pltsz = 0;
    while (o + 16 <= img.size()) {
        std::int64_t tag = 0;
        std::uint64_t val = 0;
        std::memcpy(&tag, img.data() + o, 8);
        std::memcpy(&val, img.data() + o + 8, 8);
        o += 16;
        if (tag == 0) break;
        if (tag == 5) strtab = val;
        if (tag == 6) symtab = val;
        if (tag == 10) strsz = val;
        if (tag == 7) rela = val;
        if (tag == 8) relasz = val;
        if (tag == 23) jmprel = val;
        if (tag == 2) pltsz = val;
    }
    if (strtab == 0 || symtab == 0) return 0;
    const std::size_t len = std::strlen(name);
    const std::uint64_t tabs[2][2] = {{jmprel, pltsz}, {rela, relasz}};
    for (const auto& tb : tabs) {
        for (std::uint64_t p = tb[0]; tb[0] != 0 && p + 24 <= tb[0] + tb[1] && p + 24 <= img.size(); p += 24) {
            std::uint64_t off = 0, info = 0;
            std::memcpy(&off, img.data() + p, 8);
            std::memcpy(&info, img.data() + p + 8, 8);
            const std::uint32_t type = static_cast<std::uint32_t>(info);
            const std::uint64_t si = info >> 32;
            if (type != 1026 && type != 1025 && type != 257) continue;
            const std::uint64_t sp = symtab + si * 24;
            if (si == 0 || sp + 24 > strtab) continue;
            std::uint32_t stName = 0;
            std::uint16_t shndx = 0;
            std::memcpy(&stName, img.data() + sp, 4);
            std::memcpy(&shndx, img.data() + sp + 6, 2);
            if (shndx != 0 || stName == 0 || stName >= strsz) continue;
            const char* s = reinterpret_cast<const char*>(img.data() + strtab + stName);
            if (std::strncmp(s, name, len) == 0 && s[len] == '\0') return off;
        }
    }
    return 0;
}

/* ------------------------------------- эталонные «сломанные» реализации */

/*
 * Как обходил модули exlaunch: ровно три области подряд (Rx CodeStatic,
 * R CodeStatic, Rw CodeMutable). Воспроизводим, чтобы проба показывала: на этой
 * раскладке он действительно теряет nnSdk, а наш обход — нет.
 */
template <typename Query>
int CountModulesExlaunchStyle(Query& q, std::vector<std::uint64_t>* found) {
    nxr::MemRegion info {};
    std::uint64_t next = 0;
    int state = 0;
    std::uint64_t textStart = 0;
    int count = 0;
    for (int guard = 0; guard < 0x4000; ++guard) {
        if (!q(next, &info)) break;
        if (info.size == 0) break;
        const std::uint32_t type = info.type & nxr::MemStateTypeMask;
        if (state == 0) {
            if (type == nxr::MemTypeCodeStatic && info.perm == nxr::PermRx) {
                textStart = info.addr;
                state = 1;
            }
        } else if (state == 1) {
            if (type == nxr::MemTypeCodeStatic && info.perm == nxr::PermR) state = 2;
            else state = 0;
        } else {
            if (type == nxr::MemTypeCodeMutable && info.perm == nxr::PermRw) {
                ++count;
                if (found) found->push_back(textStart);
            }
            state = 0;
        }
        const std::uint64_t end = info.addr + info.size;
        if (end <= next) break;
        next = end;
    }
    return count;
}

/*
 * Как было до исправления №5: смещение до MOD0 ограничивалось размером области
 * кода. У всех модулей игры MOD0 лежит дальше, в rodata, поэтому не находится
 * ничего (проба на консоли, код 0xDEAD0001).
 */
template <typename Query>
int CountModulesCodeLimited(Query& q) {
    nxr::MemRegion info {};
    std::uint64_t next = 0;
    int count = 0;
    for (int guard = 0; guard < 0x4000; ++guard) {
        if (!q(next, &info)) break;
        if (info.size == 0) break;
        if ((info.type & nxr::MemStateTypeMask) == nxr::MemTypeCodeStatic &&
            info.perm == nxr::PermRx && info.size >= 0x10) {
            const std::uint32_t off = reinterpret_cast<const std::uint32_t*>(
                static_cast<std::uintptr_t>(info.addr))[1];
            if (off >= 0x10 && off < info.size) {   /* <- та самая ошибка */
                const auto* mod0 = reinterpret_cast<const nxr::Mod0*>(
                    static_cast<std::uintptr_t>(info.addr + off));
                if (std::memcmp(mod0->m_Magic, "MOD0", 4) == 0) ++count;
            }
        }
        const std::uint64_t end = info.addr + info.size;
        if (end <= next) break;
        next = end;
    }
    return count;
}

/* ------------------------------------------------------------------ пробы */

int g_Fail = 0;
int g_Ran = 0;

/* «Затвор нуля»: см. path_test.cpp. Тест, не выполнивший ни одной проверки, не
   имеет права печатать «ВСЁ ЗЕЛЁНОЕ». Порог — явное число, а не производная от
   самого прогона. */
const int kMinChecks = 20;

void Check(bool cond, const char* what) {
    ++g_Ran;
    std::printf("   %s %s\n", cond ? "OK  " : "ПЛОХО", what);
    if (!cond) ++g_Fail;
}

struct Expect { const char* sym; const char* human; int module; };

}  // namespace

namespace {

bool FileExists(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::fclose(f);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    /*
     * Папка exefs игры: первым аргументом либо через NXR_GAME_EXEFS ("-" означает
     * «взять из переменной среды»). Нет папки — тест краснеет, а не пропускается.
     */
    std::string exefs;
    if (argc >= 2 && std::strcmp(argv[1], "-") != 0) {
        exefs = argv[1];
    } else if (const char* env = std::getenv("NXR_GAME_EXEFS"); env != nullptr) {
        exefs = env;
    }
    const std::string ours = argc >= 3 ? argv[2] : std::string();

    if (exefs.empty() || ours.empty()) {
        std::printf("НЕЧЕМ ПРОВЕРЯТЬ: нужны модули игры и наш модуль.\n"
                    "  использование: symfind_test <папка exefs игры> <путь к subsdkN>\n"
                    "  или:           NXR_GAME_EXEFS=<папка> symfind_test - <путь к subsdkN>\n"
                    "Папку берут из своего дампа игры (распакованный exefs: rtld, main,\n"
                    "subsdk0..9, sdk). Тест намеренно КРАСНЫЙ, а не пропущенный.\n");
        return 2;
    }

    /*
     * Порядок тот же, в каком loader грузит модули: rtld, main, subsdk0..9, наш
     * модуль, sdk. Обязательны rtld, main и sdk (в sdk живут функции nn::fs);
     * субмодули игры берём те, что есть в папке, — их число у игр разное.
     */
    struct Src { std::string path, name; };
    std::vector<Src> src;
    for (const char* req : {"rtld", "main"}) {
        const std::string p = exefs + "/" + req;
        if (!FileExists(p)) {
            std::printf("НЕЧЕМ ПРОВЕРЯТЬ: в %s нет обязательного модуля %s\n",
                        exefs.c_str(), req);
            return 2;
        }
        src.push_back({p, req});
    }
    for (int n = 0; n <= 9; ++n) {
        const std::string name = "subsdk" + std::to_string(n);
        const std::string p = exefs + "/" + name;
        if (FileExists(p)) src.push_back({p, name});
    }
    src.push_back({ours, "self"});
    {
        const std::string p = exefs + "/sdk";
        if (!FileExists(p)) {
            std::printf("НЕЧЕМ ПРОВЕРЯТЬ: в %s нет модуля sdk (в нём функции nn::fs)\n",
                        exefs.c_str());
            return 2;
        }
        src.push_back({p, "sdk"});
    }

    const int total = static_cast<int>(src.size());
    std::vector<Nso> mods(src.size());
    std::printf("[хост-тест] загрузка модулей из %s\n", exefs.c_str());
    for (int i = 0; i < total; ++i) {
        if (!LoadNso(src[i].path, src[i].name, &mods[i])) {
            std::printf("   не удалось загрузить %s\n", src[i].path.c_str());
            return 2;
        }
        std::printf("   %-8s образ %8llu Б, text 0..%#llx, rodata ..%#llx, всего %#llx\n",
                    mods[i].name.c_str(), (unsigned long long)mods[i].image.size(),
                    (unsigned long long)mods[i].textEnd, (unsigned long long)mods[i].roEnd,
                    (unsigned long long)mods[i].total);
    }

    /* Индексы нужных модулей в собранном списке. */
    auto indexOf = [&](const char* name) {
        for (int i = 0; i < total; ++i) if (mods[i].name == name) return i;
        return -1;
    };
    const int Rtld = indexOf("rtld"), Main = indexOf("main"), Sdk = indexOf("sdk");
    const int Self = indexOf("self");

    const Expect expected[] = {
        {"nnMain",                                                     "nnMain (main)",              Main},
        {"_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",                  "nn::fs::OpenFile (sdk)",     Sdk},
        {"_ZN2nn2fs11MountSdCardEPKc",                                 "nn::fs::MountSdCard (sdk)",  Sdk},
        {"_ZN2nn2fs10CreateFileEPKcl",                                 "nn::fs::CreateFile (sdk)",   Sdk},
        {"_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", "nn::fs::WriteFile (sdk)", Sdk},
        {"_ZN2nn2fs9FlushFileENS0_10FileHandleE",                      "nn::fs::FlushFile (sdk)",    Sdk},
        {"_ZN2nn2fs9CloseFileENS0_10FileHandleE",                      "nn::fs::CloseFile (sdk)",    Sdk},
    };

    /* ---------------------------------------------- основной прогон */
    std::printf("\n[1] штатная раскладка (у sdk четыре области: relro)\n");
    {
        Sim sim;
        Damage d {};
        if (!BuildSim(mods, d, &sim)) return 2;

        char head[160];
        std::snprintf(head, sizeof(head), "найдены все %d модулей (нашли %u)",
                      total, nxr::CountModules(sim));
        Check(nxr::CountModules(sim) == static_cast<std::uint32_t>(total), head);

        for (const Expect& e : expected) {
            bool hasHash = false;
            const std::uint64_t want = LookupInImage(mods[e.module], e.sym, &hasHash);
            const std::uint64_t got  = nxr::FindSymbolAnywhere(sim, e.sym);
            char msg[256];
            std::snprintf(msg, sizeof(msg), "%s: %#llx (ожидалось %#llx)", e.human,
                          (unsigned long long)got,
                          (unsigned long long)(want ? sim.bases[e.module] + want : 0));
            Check(want != 0 && got == sim.bases[e.module] + want, msg);
        }

        /* Символа, которого нет, находиться не должно. */
        Check(nxr::FindSymbolAnywhere(sim, "_ZN2nn2fs17NoSuchFunctionXyzEv") == 0,
              "НЕГАТИВ: выдуманный символ не найден");
        Check(nxr::FindSymbolAnywhere(sim, "") == 0,
              "НЕГАТИВ: пустое имя не находится");

        /*
         * ИМПОРТЫ модулей игры. По ним модуль во время работы узнаёт, собирается
         * ли игра звать nn::fs::SetAllocator, и выбирает, каким путём открывать
         * затвор файловой системы (src/nxr_fsgate.hpp).
         * Проверка не привязана к конкретной игре: nn::fs::OpenFile у любой игры
         * на nnSdk ИМПОРТИРУЕТ модуль main и ОПРЕДЕЛЯЕТ модуль sdk.
         */
        {
            bool mainImports = false;
            bool sdkImports  = false;
            nxr::ForEachModule(sim, [&](std::uint64_t base, const nxr::Dyn* dyn) {
                if (base == sim.bases[Main]) {
                    mainImports = nxr::HasImportIn(sim, base, dyn, expected[1].sym);
                }
                if (base == sim.bases[Sdk]) {
                    sdkImports = nxr::HasImportIn(sim, base, dyn, expected[1].sym);
                }
                return false;
            });
            Check(mainImports, "импорт nn::fs::OpenFile у модуля main виден");
            Check(!sdkImports,
                  "НЕГАТИВ: у sdk тот же символ ОПРЕДЕЛЁН, а не импортирован");
            Check(!nxr::HasImportAnywhere(sim, "_ZN2nn2fs17NoSuchFunctionXyzEv"),
                  "НЕГАТИВ: выдуманное имя импортом не считается");
            /*
             * СЛОТ ИМПОРТА: ячейка GOT, по которой видно, куда
             * разрешился импорт. Сверяется с независимым разбором образа.
             */
            std::uint64_t mainSlot = 0;
            nxr::ForEachModule(sim, [&](std::uint64_t base, const nxr::Dyn* dyn) {
                if (base == sim.bases[Main]) {
                    mainSlot = nxr::FindImportSlotIn(sim, base, dyn, expected[1].sym);
                }
                return false;
            });
            const std::uint64_t wantSlot = ImportSlotInImage(mods[Main], expected[1].sym);
            {
                char m2[200];
                std::snprintf(m2, sizeof(m2),
                              "слот импорта OpenFile у main = база+%#llx (независимо: %#llx)",
                              (unsigned long long)(mainSlot ? mainSlot - sim.bases[Main] : 0),
                              (unsigned long long)wantSlot);
                Check(wantSlot != 0 && mainSlot == sim.bases[Main] + wantSlot, m2);
            }
            {
                const nxr::ImportSlot any = nxr::FindImportSlotAnywhere(sim, expected[1].sym);
                Check(any.moduleBase != 0 && any.slot >= any.moduleBase,
                      "FindImportSlotAnywhere находит импортёра OpenFile");
                Check(nxr::FindImportSlotAnywhere(sim, "_ZN2nn2fs17NoSuchFunctionXyzEv").slot == 0,
                      "НЕГАТИВ: у выдуманного имени слота импорта нет");
                /* Другой импорт main должен дать ДРУГОЙ слот: поиск сравнивает имя. */
                const std::uint64_t mountSlot = ImportSlotInImage(mods[Main], expected[2].sym);
                std::uint64_t gotMount = 0;
                nxr::ForEachModule(sim, [&](std::uint64_t base, const nxr::Dyn* dyn) {
                    if (base == sim.bases[Main]) {
                        gotMount = nxr::FindImportSlotIn(sim, base, dyn, expected[2].sym);
                    }
                    return false;
                });
                if (mountSlot != 0) {
                    Check(gotMount == sim.bases[Main] + mountSlot && gotMount != mainSlot,
                          "у MountSdCard свой слот, не слот OpenFile (имя сравнивается)");
                } else {
                    Check(gotMount == 0, "main не импортирует MountSdCard — слота нет");
                }
            }

            /*
             * ПРАВИЛО rtld: выигрывает ПЕРВЫЙ определённый не-LOCAL символ при
             * обходе по возрастанию адреса (разбор кода rtld, nxr_symfind.hpp).
             * OpenFile определяет только sdk — он и победитель.
             */
            {
                /*
                 * Символ, определённый в НЕСКОЛЬКИХ модулях игры (служебные
                 * символы nnSdk есть и в main, и в sdk): победитель обязан быть
                 * ПЕРВЫМ по порядку загрузки. Именно эта проверка ловит поломку
                 * rtld-last-wins на любой сборке, с исправлением или без.
                 */
                const char* cands[] = {"_init", "__nnDetailNintendoSdkRuntimeObjectFileRefer",
                                       "nndetailRoGetRoDataStart", "_fini"};
                const char* pick = nullptr;
                int firstIdx = -1, cnt = 0;
                for (const char* c : cands) {
                    int n = 0, f = -1;
                    for (int i = 0; i < total; ++i) {
                        if (mods[i].name == "self") continue;
                        if (LookupInImage(mods[i], c, nullptr) != 0) {
                            if (f < 0) f = i;
                            ++n;
                        }
                    }
                    if (n >= 2) { pick = c; firstIdx = f; cnt = n; break; }
                }
                Check(pick != nullptr, "нашёлся символ, определённый в двух и более модулях игры");
                if (pick != nullptr) {
                    char m3[200];
                    std::snprintf(m3, sizeof(m3),
                                  "правило rtld: %s (определён в %d модулях) — выигрывает первый (%s)",
                                  pick, cnt, mods[firstIdx].name.c_str());
                    const std::uint64_t selfDef = LookupInImage(mods[Self], pick, nullptr);
                    const int winIdx = (selfDef != 0 && Self < firstIdx) ? Self : firstIdx;
                    Check(nxr::RtldWinnerOf(sim, pick) ==
                              sim.bases[winIdx] + LookupInImage(mods[winIdx], pick, nullptr), m3);
                }
            }
            Check(nxr::RtldWinnerOf(sim, expected[1].sym) ==
                      sim.bases[Sdk] + LookupInImage(mods[Sdk], expected[1].sym, nullptr),
                  "правило rtld: OpenFile разрешается в sdk");
            {
                /*
                 * NvRmMemQueryHandleParams: определён в sdk (у 19.3.13 — заглушка).
                 * Если наш модуль собран с исправлением, он определяет его тоже,
                 * и по правилу rtld ОБЯЗАН выиграть он (лежит раньше sdk).
                 */
                const char* nv = "NvRmMemQueryHandleParams";
                const std::uint64_t inSelf = LookupInImage(mods[Self], nv, nullptr);
                const std::uint64_t inSdk  = LookupInImage(mods[Sdk], nv, nullptr);
                const std::uint64_t win = nxr::RtldWinnerOf(sim, nv);
                int defs = nxr::ForEachDefinitionOf(sim, nv, [](std::uint64_t, std::uint64_t) {});
                /* Импортёр (у Well Dweller — subsdk0, OpenGL 22.2.0): слот сверяется
                   с независимым разбором образа. Нет импортёра — слота нет. */
                int imod = -1;
                for (int i = 0; i < total && imod < 0; ++i) {
                    if (ImportSlotInImage(mods[i], nv) != 0) imod = i;
                }
                const nxr::ImportSlot is = nxr::FindImportSlotAnywhere(sim, nv);
                if (imod >= 0) {
                    char m4[200];
                    std::snprintf(m4, sizeof(m4), "слот импорта %s у %s = база+%#llx", nv,
                                  mods[imod].name.c_str(),
                                  (unsigned long long)ImportSlotInImage(mods[imod], nv));
                    Check(is.moduleBase == sim.bases[imod] &&
                              is.slot == sim.bases[imod] + ImportSlotInImage(mods[imod], nv), m4);
                } else {
                    Check(is.slot == 0, "у этой игры NvRmMemQueryHandleParams никто не импортирует");
                }
                if (inSelf != 0) {
                    Check(win == sim.bases[Self] + inSelf,
                          "наш экспорт NvRmMemQueryHandleParams ВЫИГРЫВАЕТ у rtld (модуль раньше sdk)");
                    Check(inSdk == 0 || defs == 2,
                          "определений два: наше и sdk (оба видны диагностике)");
                } else {
                    Check(inSdk == 0 ? win == 0 : win == sim.bases[Sdk] + inSdk,
                          "сборка без исправления: NvRmMemQueryHandleParams разрешается в sdk");
                    Check(defs == (inSdk != 0 ? 1 : 0), "определение одно — в sdk");
                }
            }

            const bool setAlloc = nxr::HasImportAnywhere(
                sim, "_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE");
            std::printf("        эта игра зовёт nn::fs::SetAllocator: %s"
                        " (от этого зависит путь открытия затвора ФС)\n",
                        setAlloc ? "ДА" : "нет");
        }

        /*
         * Причина №3 (Data Abort в GetSymbolByName шаблона): поиск через DT_HASH
         * падает там, где этой таблицы нет. У одних игр её нет (Well Dweller, SiN —
         * только DT_GNU_HASH), у других есть (TotK: у всех модулей). Поэтому здесь
         * только СВОДКА; проверка того, что поиск от DT_HASH не зависит, — блок [6]:
         * та же игра с вырезанным DT_HASH обязана давать те же адреса.
         * *Раньше здесь была проверка «ни у одного модуля игры нет DT_HASH»:
         * она проверяла игру, а не наш поиск, и краснела на TotK, где всё находится.*
         */
        int withHash = 0;
        for (const Nso& m : mods) {
            bool h = false;
            LookupInImage(m, "nnMain", &h);
            std::printf("        DT_HASH у %-8s: %s\n", m.name.c_str(), h ? "ЕСТЬ" : "нет");
            if (m.name != "self" && h) ++withHash;
        }
        std::printf("        модулей игры с DT_HASH: %d из %d (бывает и так, и так)\n",
                    withHash, total - 1);

        /* Разбор таблиц должен удаваться у всех пяти модулей. */
        int parsed = 0;
        nxr::ForEachModule(sim, [&](std::uint64_t base, const nxr::Dyn* dyn) {
            nxr::Tables t {};
            if (nxr::ResolveTables(sim, base, dyn, &t)) ++parsed;
            return false;
        });
        std::snprintf(head, sizeof(head),
                      ".dynsym/.dynstr разобраны у всех %d модулей (разобрано %d)", total, parsed);
        Check(parsed == total, head);

        /* НЕГАТИВ №1: старое ограничение MOD0 размером области кода. */
        const int limited = CountModulesCodeLimited(sim);
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "НЕГАТИВ: «MOD0 в пределах области кода» находит %d модулей вместо 5",
                      limited);
        Check(limited == 0, msg);

        /* НЕГАТИВ №2: сканер exlaunch (ровно три области) теряет sdk. */
        std::vector<std::uint64_t> exl;
        const int exlCount = CountModulesExlaunchStyle(sim, &exl);
        std::snprintf(msg, sizeof(msg),
                      "НЕГАТИВ: сканер exlaunch находит %d модулей из %d (теряет sdk с relro)",
                      exlCount, total);
        Check(exlCount == total - 1, msg);
        Check(std::find(exl.begin(), exl.end(), sim.bases[Sdk]) == exl.end(),
              "НЕГАТИВ: среди найденных сканером exlaunch нет sdk");

        sim.Free();
    }

    /* ------------------------------- проба: у sdk три области (без relro) */
    std::printf("\n[2] раскладка без relro — должно работать так же\n");
    {
        Sim sim;
        Damage d {};
        d.sdkRelro = false;
        if (!BuildSim(mods, d, &sim)) return 2;
        Check(nxr::CountModules(sim) == static_cast<std::uint32_t>(total), "найдены все модули");
        Check(nxr::FindSymbolAnywhere(sim, expected[1].sym) != 0, "OpenFile найден");
        Check(CountModulesExlaunchStyle(sim, nullptr) == total,
              "контроль: без relro сканер exlaunch находит все 5 (значит, дело именно в relro)");
        sim.Free();
    }

    /* ------------------ проба: чужая Rx-область (внедрённый saltysd_core) */
    std::printf("\n[3] чужая область кода перед модулями (SaltyNX / saltysd_core)\n");
    {
        Sim sim;
        Damage d {};
        d.foreignCodeRegion = true;
        if (!BuildSim(mods, d, &sim)) return 2;
        Check(nxr::CountModules(sim) == static_cast<std::uint32_t>(total),
              "чужая область не принята за модуль");
        Check(nxr::FindSymbolAnywhere(sim, expected[0].sym) == sim.bases[Main] +
                  LookupInImage(mods[Main], expected[0].sym, nullptr),
              "nnMain по-прежнему найден");
        Check(nxr::FindSymbolAnywhere(sim, expected[1].sym) != 0, "OpenFile по-прежнему найден");
        sim.Free();
    }

    /* --------- проба: слово по +4 указывает в неотображённую память */
    std::printf("\n[4] НЕГАТИВ: у rtld смещение MOD0 указывает в пустоту\n");
    {
        Sim sim;
        Damage d {};
        d.badMod0OffsetModule = Rtld;
        if (!BuildSim(mods, d, &sim)) return 2;
        Check(nxr::CountModules(sim) == static_cast<std::uint32_t>(total - 1),
              "испорченный модуль пропущен (на один меньше), без падения");
        Check(nxr::FindSymbolAnywhere(sim, expected[1].sym) != 0, "OpenFile всё равно найден");
        sim.Free();
    }

    /* ---------------------------- проба: испорчена магия MOD0 у main */
    std::printf("\n[5] НЕГАТИВ: у main испорчена магия MOD0\n");
    {
        Sim sim;
        Damage d {};
        d.badMod0MagicModule = Main;
        if (!BuildSim(mods, d, &sim)) return 2;
        Check(nxr::CountModules(sim) == static_cast<std::uint32_t>(total - 1),
              "модуль без MOD0 не считается модулем");
        Check(nxr::FindSymbolAnywhere(sim, "nnMain") == 0, "nnMain больше не находится");
        Check(nxr::FindSymbolAnywhere(sim, expected[1].sym) != 0, "OpenFile всё равно найден");
        sim.Free();
    }

    /*
     * [6] ПОИСК НЕ ЗАВИСИТ ОТ DT_HASH. Та же игра, но из .dynamic
     * каждого модуля (и нашего тоже) вырезан DT_HASH. Важно не то, есть ли таблица
     * у игры, а то, что символы находятся одинаково с ней и без неё: все модули
     * разобраны, каждый ожидаемый символ — по тому же адресу, что по образу файла.
     * Поиск, требующий DT_HASH (как GetSymbolByName шаблона), здесь краснеет на
     * любой игре — в том числе на TotK, где в блоке [1] он ничем бы не выдал себя.
     * Ослабленный тест (-DNXR_SYMFIND_WEAK_HASH_CHECK, поломка weak-hash-check в
     * run_hosttest.sh) этот блок пропускает — для негативной пробы самого теста.
     */
#if !defined(NXR_SYMFIND_WEAK_HASH_CHECK)
    std::printf("\n[6] та же игра без DT_HASH — символы обязаны находиться так же\n");
    {
        Sim sim;
        Damage d {};
        d.stripHash = true;
        if (!BuildSim(mods, d, &sim)) return 2;
        std::printf("        DT_HASH вырезан у модулей: %d из %d\n", sim.hashStripped, total);
        Check(nxr::CountModules(sim) == static_cast<std::uint32_t>(total),
              "без DT_HASH найдены все модули");
        int parsedNoHash = 0;
        nxr::ForEachModule(sim, [&](std::uint64_t base, const nxr::Dyn* dyn) {
            nxr::Tables t {};
            if (nxr::ResolveTables(sim, base, dyn, &t) && !t.hasHash) ++parsedNoHash;
            return false;
        });
        char m6[256];
        std::snprintf(m6, sizeof(m6),
                      "без DT_HASH .dynsym/.dynstr разобраны у всех %d модулей (разобрано %d)",
                      total, parsedNoHash);
        Check(parsedNoHash == total, m6);
        for (const Expect& e : expected) {
            const std::uint64_t want = LookupInImage(mods[e.module], e.sym, nullptr);
            const std::uint64_t got  = nxr::FindSymbolAnywhere(sim, e.sym);
            std::snprintf(m6, sizeof(m6), "без DT_HASH %s: %#llx (ожидалось %#llx)", e.human,
                          (unsigned long long)got,
                          (unsigned long long)(want ? sim.bases[e.module] + want : 0));
            Check(want != 0 && got == sim.bases[e.module] + want, m6);
        }
        Check(nxr::FindSymbolAnywhere(sim, "_ZN2nn2fs17NoSuchFunctionXyzEv") == 0,
              "НЕГАТИВ: без DT_HASH выдуманный символ по-прежнему не найден");
        sim.Free();
    }
#else
    std::printf("\n[6] ПРОПУЩЕН: ослабленная сборка теста (NXR_SYMFIND_WEAK_HASH_CHECK)\n");
#endif

    std::printf("\n");
    if (g_Ran < kMinChecks) {
        std::printf("[хост-тест] НИЧЕГО НЕ ПРОВЕРЕНО: выполнено проб %d, "
                    "а их обязано быть не меньше %d — это КРАСНЫЙ, а не зелёный\n",
                    g_Ran, kMinChecks);
        return 2;
    }
    if (g_Fail == 0) {
        std::printf("[хост-тест] ВСЁ ЗЕЛЁНОЕ (проб выполнено: %d)\n", g_Ran);
        return 0;
    }
    std::printf("[хост-тест] ПРОБ ПРОВАЛЕНО: %d\n", g_Fail);
    return 1;
}
