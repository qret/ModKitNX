// main.npdm: порт npdm_sdcard.py, npdm_syscalls.py (Npdm/check/compare), make_npdm/make_mod_npdm.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mk/npdm.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mk {

namespace {
constexpr uint32_t kCapSyscall = 0x0F;
constexpr unsigned kPerGroup = 24;
constexpr unsigned kMaxSvc = 8 * kPerGroup - 1;
constexpr uint32_t kMetaHdr = 0x80;
const char* kKeys[3] = {"fs", "srv", "kern"};

struct Layout { uint32_t base, hdr; };
Layout layout(const std::string& n) { return n == "ACID" ? Layout{0x220, 0x240} : Layout{0x020, 0x040}; }
size_t align16(size_t v) { return (v + 15) & ~size_t(15); }
std::string hx(uint64_t v, int w = 0) {
    char b[32];
    std::snprintf(b, sizeof b, w ? "%#0*llx" : "%#llx", w, (unsigned long long)v);
    return b;
}
}  // namespace

const std::vector<unsigned>& npdm_default_svcs() {
    static const std::vector<unsigned> v{0x74, 0x75};
    return v;
}

Npdm::Npdm(const Bytes& r) : raw(r) {
    if (raw.size() < kMetaHdr || std::memcmp(raw.data(), "META", 4) != 0)
        throw Error("NPDM_BAD", "это не npdm: нет сигнатуры META");
    aci_off = rd32(&raw[0x70]); aci_size = rd32(&raw[0x74]);
    acid_off = rd32(&raw[0x78]); acid_size = rd32(&raw[0x7C]);
    struct S { const char* n; uint32_t off, size, hdr; } secs[2] = {{"ACI0", aci_off, aci_size, 0x40},
                                                                   {"ACID", acid_off, acid_size, 0x240}};
    for (auto& s : secs)
        if (s.off < kMetaHdr || s.size < s.hdr || uint64_t(s.off) + s.size > raw.size())
            throw Error("NPDM_BAD", std::string("секция ") + s.n + " не помещается в файл");
    acid.assign(raw.begin() + acid_off, raw.begin() + acid_off + acid_size);
    aci.assign(raw.begin() + aci_off, raw.begin() + aci_off + aci_size);
    if (std::memcmp(&acid[0x200], "ACID", 4) != 0) throw Error("NPDM_BAD", "нет сигнатуры ACID по смещению +0x200");
    if (std::memcmp(&aci[0], "ACI0", 4) != 0) throw Error("NPDM_BAD", "нет сигнатуры ACI0");
    for (const char* name : {"ACID", "ACI0"}) {
        const Bytes& sec = section(name);
        Layout L = layout(name);
        for (int i = 0; i < 3; i++) {
            uint32_t off = rd32(&sec[L.base + i * 8]), size = rd32(&sec[L.base + i * 8 + 4]);
            if (off < L.hdr || uint64_t(off) + size > sec.size())
                throw Error("NPDM_BAD", std::string(name) + ": подблок " + kKeys[i] + " выходит за пределы секции");
            blocks[name][kKeys[i]] = {off, size};
        }
        if (blocks[name]["kern"].size % 4) throw Error("NPDM_BAD", std::string(name) + ": размер kernel caps не кратен 4");
    }
}

Bytes Npdm::block(const std::string& sec, const std::string& key) const {
    const Bytes& s = section(sec);
    Block b = blocks.at(sec).at(key);
    return Bytes(s.begin() + b.off, s.begin() + b.off + b.size);
}

std::vector<uint32_t> Npdm::caps(const std::string& sec) const {
    Bytes d = block(sec, "kern");
    std::vector<uint32_t> v(d.size() / 4);
    for (size_t i = 0; i < v.size(); i++) v[i] = rd32(&d[i * 4]);
    return v;
}

uint64_t Npdm::fs_mask(const std::string& sec) const {
    Bytes d = block(sec, "fs");
    if (d.size() < 12) throw Error("NPDM_BAD", sec + ": блок прав FS короче 12 байт");
    return rd64(&d[4]);
}

uint64_t Npdm::program_id() const { return rd64(&aci[0x10]); }

std::pair<std::set<unsigned>, std::vector<uint32_t>> Npdm::syscalls_of(const std::vector<uint32_t>& caps) {
    std::set<unsigned> allowed;
    std::vector<uint32_t> others;
    for (uint32_t v : caps) {
        if ((v & 0x1F) == kCapSyscall) {
            uint32_t mask = (v >> 5) & 0xFFFFFF, group = v >> 29;
            for (unsigned b = 0; b < kPerGroup; b++)
                if (mask & (1u << b)) allowed.insert(group * kPerGroup + b);
        } else {
            others.push_back(v);
        }
    }
    return {allowed, others};
}

std::map<unsigned, uint32_t> Npdm::syscall_words(const std::vector<uint32_t>& caps) {
    std::map<unsigned, uint32_t> m;
    for (uint32_t v : caps)
        if ((v & 0x1F) == kCapSyscall) m[v >> 29] = v;
    return m;
}

std::vector<uint32_t> Npdm::add_syscalls(const std::vector<uint32_t>& caps, const std::vector<unsigned>& svcs) {
    std::map<unsigned, uint32_t> need;
    for (unsigned s : svcs) {
        if (s > kMaxSvc) throw Error("NPDM_BAD", "номер вызова " + hx(s) + " вне диапазона");
        need[s / kPerGroup] |= 1u << (s % kPerGroup);
    }
    std::vector<uint32_t> out = caps;
    for (auto& v : out) {
        if ((v & 0x1F) == kCapSyscall && need.count(v >> 29)) {
            unsigned group = v >> 29;
            uint32_t mask = ((v >> 5) & 0xFFFFFF) | need[group];
            need.erase(group);
            v = (group << 29) | (mask << 5) | kCapSyscall;
        }
    }
    for (auto& [group, mask] : need) out.push_back((group << 29) | (mask << 5) | kCapSyscall);
    return out;
}

Bytes Npdm::rebuild_section(const std::string& name, const std::vector<uint32_t>& new_caps) const {
    const Bytes& sec = section(name);
    const auto& bl = blocks.at(name);
    uint32_t base = layout(name).base;
    uint32_t head_len = UINT32_MAX;
    for (auto& [k, b] : bl) head_len = std::min(head_len, b.off);
    Bytes body(sec.begin(), sec.begin() + head_len);
    // ключи по смещению; при равных — в порядке ("fs","srv","kern") как sorted() по dict
    std::vector<std::string> keys{"fs", "srv", "kern"};
    std::stable_sort(keys.begin(), keys.end(), [&](auto& a, auto& b) { return bl.at(a).off < bl.at(b).off; });
    std::map<std::string, Block> nb;
    for (auto& key : keys) {
        Block b = bl.at(key);
        Bytes data;
        if (key == "kern") {
            data.resize(new_caps.size() * 4);
            for (size_t i = 0; i < new_caps.size(); i++) wr32(&data[i * 4], new_caps[i]);
        } else {
            data.assign(sec.begin() + b.off, sec.begin() + b.off + b.size);
        }
        body.resize(align16(body.size()), 0);
        nb[key] = {uint32_t(body.size()), uint32_t(data.size())};
        body.insert(body.end(), data.begin(), data.end());
    }
    for (int i = 0; i < 3; i++) {
        wr32(&body[base + i * 8], nb[kKeys[i]].off);
        wr32(&body[base + i * 8 + 4], nb[kKeys[i]].size);
    }
    if (name == "ACID") wr32(&body[0x204], uint32_t(body.size() - 0x100));
    return body;
}

Bytes Npdm::with_syscalls(const std::vector<unsigned>& svcs) const {
    std::map<std::string, Bytes> rebuilt;
    for (const char* name : {"ACID", "ACI0"}) rebuilt[name] = rebuild_section(name, add_syscalls(caps(name), svcs));
    std::vector<std::string> order{"ACID", "ACI0"};
    std::stable_sort(order.begin(), order.end(), [&](auto& a, auto& b) {
        return (a == "ACID" ? acid_off : aci_off) < (b == "ACID" ? acid_off : aci_off);
    });
    uint32_t first = std::min(acid_off, aci_off);
    Bytes out(raw.begin(), raw.begin() + first);
    std::map<std::string, uint32_t> offs;
    for (auto& n : order) {
        out.resize(align16(out.size()), 0);
        offs[n] = uint32_t(out.size());
        out.insert(out.end(), rebuilt[n].begin(), rebuilt[n].end());
    }
    wr32(&out[0x70], offs["ACI0"]);
    wr32(&out[0x74], uint32_t(rebuilt["ACI0"].size()));
    wr32(&out[0x78], offs["ACID"]);
    wr32(&out[0x7C], uint32_t(rebuilt["ACID"].size()));
    return out;
}

Npdm Npdm::with_sdcard_bit() const {
    Bytes r = raw;
    for (auto [name, off] : {std::pair<const char*, uint32_t>{"ACI0", aci_off}, {"ACID", acid_off}}) {
        uint32_t fo = blocks.at(name).at("fs").off;
        wr64(&r[off + fo + 4], fs_mask(name) | kSdCardBit);
    }
    return Npdm(r);
}

std::vector<std::string> npdm_check(const Npdm& n, const std::vector<unsigned>& svcs, const uint64_t* pid,
                                    bool need_sdcard) {
    std::vector<std::string> p;
    if (Npdm::syscall_words(n.caps("ACID")) != Npdm::syscall_words(n.caps("ACI0")))
        p.push_back("маски вызовов в ACI0 и ACID не совпадают слово в слово (процесс не создастся)");
    for (const char* name : {"ACID", "ACI0"}) {
        auto allowed = Npdm::syscalls_of(n.caps(name)).first;
        std::string miss;
        std::set<unsigned> want(svcs.begin(), svcs.end());
        for (unsigned s : want)
            if (!allowed.count(s)) miss += (miss.empty() ? "" : ", ") + hx(s);
        if (!miss.empty()) p.push_back(std::string(name) + ": не разрешены вызовы " + miss);
        if (need_sdcard && !(n.fs_mask(name) & kSdCardBit))
            p.push_back(std::string(name) + ": в правах FS нет бита SdCard (0x200000)");
    }
    if (pid && n.program_id() != *pid)
        p.push_back("program id " + hx(n.program_id(), 18) + ", ожидался " + hx(*pid, 18));
    return p;
}

std::vector<std::string> npdm_compare(const Npdm& src, const Npdm& now, const std::vector<unsigned>& svcs) {
    std::vector<std::string> p;
    std::set<unsigned> sv(svcs.begin(), svcs.end());
    for (const char* name : {"ACID", "ACI0"}) {
        auto [oa, oo] = Npdm::syscalls_of(src.caps(name));
        auto [na, no] = Npdm::syscalls_of(now.caps(name));
        if (!std::includes(na.begin(), na.end(), oa.begin(), oa.end()))
            p.push_back(std::string(name) + ": пропали разрешённые вызовы");
        for (unsigned s : na)
            if (!oa.count(s) && !sv.count(s)) { p.push_back(std::string(name) + ": добавились лишние вызовы"); break; }
        if (oo != no) p.push_back(std::string(name) + ": изменились прочие возможности ядра");
        for (const char* key : {"fs", "srv"})
            if (src.block(name, key) != now.block(name, key))
                p.push_back(std::string(name) + ": изменился блок " + key + " (должен остаться как в игре)");
    }
    if (src.program_id() != now.program_id()) p.push_back("изменился program id");
    if (!std::equal(src.acid.begin() + 0x208, src.acid.begin() + 0x220, now.acid.begin() + 0x208))
        p.push_back("изменились флаги ACID или диапазон program id");
    if (!std::equal(src.acid.begin(), src.acid.begin() + 0x200, now.acid.begin()))
        p.push_back("изменилась подпись/ключ ACID");
    if (!std::equal(src.raw.begin(), src.raw.begin() + 0x70, now.raw.begin())) p.push_back("изменилась шапка META");
    return p;
}

Bytes npdm_add_sdcard(const Bytes& src) {
    if (src.size() < 0x80 || std::memcmp(src.data(), "META", 4) != 0)
        throw Error("NPDM_SDCARD", "это не npdm: в начале файла нет сигнатуры META");
    uint32_t aci0_off = rd32(&src[0x70]), aci0_size = rd32(&src[0x74]);
    uint32_t acid_off = rd32(&src[0x78]), acid_size = rd32(&src[0x7C]);
    if (uint64_t(aci0_off) + aci0_size > src.size()) throw Error("NPDM_SDCARD", "секция ACI0 выходит за пределы файла");
    if (uint64_t(acid_off) + acid_size > src.size()) throw Error("NPDM_SDCARD", "секция ACID выходит за пределы файла");
    if (uint64_t(aci0_off) + 4 > src.size() || std::memcmp(&src[aci0_off], "ACI0", 4) != 0)
        throw Error("NPDM_SDCARD", "нет сигнатуры ACI0");
    if (uint64_t(acid_off) + 0x204 > src.size() || std::memcmp(&src[acid_off + 0x200], "ACID", 4) != 0)
        throw Error("NPDM_SDCARD", "нет сигнатуры ACID");
    if (uint64_t(aci0_off) + 0x24 > src.size() || uint64_t(acid_off) + 0x224 > src.size())
        throw Error("NPDM_SDCARD", "заголовок секции обрезан");
    uint64_t fah = uint64_t(aci0_off) + rd32(&src[aci0_off + 0x20]);
    uint64_t fac = uint64_t(acid_off) + rd32(&src[acid_off + 0x220]);
    for (uint64_t o : {fah, fac})
        if (o + 12 > src.size()) throw Error("NPDM_SDCARD", "права FS выходят за пределы файла");
    Bytes out = src;
    for (uint64_t o : {fah + 4, fac + 4}) wr64(&out[o], rd64(&out[o]) | kSdCardBit);
    size_t changed = 0;
    for (size_t i = 0; i < src.size(); i++) changed += src[i] != out[i];
    if (changed != 2)
        throw Error("NPDM_SDCARD", "ожидалось ровно 2 изменённых байта, получилось " + std::to_string(changed) +
                                       ". Файл не записан.");
    return out;
}

Bytes make_game_npdm(const Bytes& game, uint64_t tid) {
    Bytes mid = npdm_add_sdcard(game);
    Npdm src(mid);
    const auto& svcs = npdm_default_svcs();
    Bytes data = src.with_syscalls(svcs);
    Npdm now(data);
    uint64_t sp = src.program_id();
    auto p = npdm_compare(src, now, svcs);
    auto c = npdm_check(now, svcs, &sp, (src.fs_mask("ACI0") & kSdCardBit) != 0);
    p.insert(p.end(), c.begin(), c.end());
    if (!p.empty()) throw Error("FAIL_NPDM_SYSCALLS", "npdm_syscalls: " + p[0]);
    // гейт make_npdm: --check --program-id <tid>
    auto g = npdm_check(now, svcs, &tid, true);
    if (!g.empty()) throw Error("FAIL_GATE_NPDM", "гейт npdm не пройден: " + g[0]);
    return data;
}

ModNpdm make_mod_npdm(const Bytes& mod_npdm, const Bytes* game_npdm, uint64_t tid) {
    ModNpdm r;
    std::unique_ptr<Npdm> src;
    try {
        src.reset(new Npdm(mod_npdm));
    } catch (const Error& e) {
        throw Error("FAIL_MOD_NPDM_BAD", std::string("main.npdm мода не разбирается: ") + e.what());
    }
    if (src->program_id() != tid)
        throw Error("FAIL_MOD_NPDM_TID", "main.npdm мода от другой программы: program id " + tid_hex(src->program_id()) +
                                             ", а игра " + tid_hex(tid));
    const auto& svcs = npdm_default_svcs();
    auto problems = npdm_check(*src, svcs, &tid, true);
    if (problems.empty()) {
        r.data = mod_npdm;
        r.mode = "as-is";
    } else {
        Npdm cur = *src;
        if (!((cur.fs_mask("ACI0") & kSdCardBit) && (cur.fs_mask("ACID") & kSdCardBit))) {
            cur = cur.with_sdcard_bit();
            r.added.push_back("бит SdCard");
        }
        auto a1 = Npdm::syscalls_of(cur.caps("ACI0")).first, a2 = Npdm::syscalls_of(cur.caps("ACID")).first;
        std::vector<unsigned> missing;
        for (unsigned s : std::set<unsigned>(svcs.begin(), svcs.end()))
            if (!(a1.count(s) && a2.count(s))) missing.push_back(s);
        Bytes data = cur.raw;
        if (!missing.empty()) {
            data = cur.with_syscalls(svcs);
            Npdm now(data);
            auto bad = npdm_compare(cur, now, svcs);
            if (!bad.empty()) throw Error("FAIL_NPDM_SYSCALLS", "правка main.npdm мода разошлась: " + bad[0]);
            std::string s = "вызовы ";
            for (size_t i = 0; i < missing.size(); i++) s += (i ? ", " : "") + hx(missing[i]);
            r.added.push_back(s);
        }
        r.data = data;
        r.mode = "merged";
    }
    auto g = npdm_check(Npdm(r.data), svcs, &tid, true);
    if (!g.empty()) throw Error("FAIL_GATE_NPDM", "гейт npdm не пройден: " + g[0]);
    if (game_npdm && game_npdm->size() >= 0x20 && mod_npdm.size() >= 0x20) {
        const Bytes& gm = *game_npdm;
        struct F { size_t off, size; const char* what; } fs[] = {{0x14, 4, "системный ресурс"},
                                                                 {0x1C, 4, "стек главного потока"},
                                                                 {0x0E, 1, "приоритет главного потока"},
                                                                 {0x0F, 1, "ядро главного потока"},
                                                                 {0x0C, 1, "флаги"}};
        for (auto& f : fs) {
            uint32_t a = f.size == 4 ? rd32(&gm[f.off]) : gm[f.off];
            uint32_t b = f.size == 4 ? rd32(&mod_npdm[f.off]) : mod_npdm[f.off];
            if (a != b) r.header_diffs.push_back(std::string(f.what) + ": у игры " + hx(a) + ", у мода " + hx(b));
        }
    }
    return r;
}

std::string tid_hex(uint64_t tid) {
    char b[20];
    std::snprintf(b, sizeof b, "%016llX", (unsigned long long)tid);
    return b;
}

bool parse_tid(const std::string& s, uint64_t& out) {
    if (s.size() != 16) return false;
    uint64_t v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = v << 4 | uint64_t(d);
    }
    out = v;
    return true;
}

}  // namespace mk
