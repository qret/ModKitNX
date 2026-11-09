// main.npdm: бит SdCard + svc 0x74/0x75 в ACI0 и ACID одинаково (побайтно как эталон).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Порт: nxredirect/scripts/npdm_sdcard.py, nxredirect/scripts/npdm_syscalls.py (class Npdm, check, compare),
// make_npdm / make_mod_npdm сборщика модуля nxredirect.
#pragma once

#include "mk/core.hpp"

#include <map>
#include <set>

namespace mk {

constexpr uint64_t kSdCardBit = 1ull << 21;
const std::vector<unsigned>& npdm_default_svcs();   // {0x74, 0x75}

class Npdm {
public:
    explicit Npdm(const Bytes& raw);   // бросает Error("NPDM_BAD", ...)
    Bytes raw, acid, aci;
    uint32_t aci_off = 0, aci_size = 0, acid_off = 0, acid_size = 0;

    struct Block { uint32_t off = 0, size = 0; };
    // blocks["ACID"|"ACI0"]["fs"|"srv"|"kern"]
    std::map<std::string, std::map<std::string, Block>> blocks;

    Bytes block(const std::string& sec, const std::string& key) const;
    std::vector<uint32_t> caps(const std::string& sec) const;
    uint64_t fs_mask(const std::string& sec) const;
    uint64_t program_id() const;

    static std::pair<std::set<unsigned>, std::vector<uint32_t>> syscalls_of(const std::vector<uint32_t>& caps);
    static std::map<unsigned, uint32_t> syscall_words(const std::vector<uint32_t>& caps);
    static std::vector<uint32_t> add_syscalls(const std::vector<uint32_t>& caps, const std::vector<unsigned>& svcs);

    Bytes with_syscalls(const std::vector<unsigned>& svcs) const;
    Npdm with_sdcard_bit() const;   // _with_sdcard_bit (для npdm мода)

private:
    const Bytes& section(const std::string& name) const { return name == "ACID" ? acid : aci; }
    Bytes rebuild_section(const std::string& name, const std::vector<uint32_t>& new_caps) const;
};

std::vector<std::string> npdm_check(const Npdm& n, const std::vector<unsigned>& svcs,
                                    const uint64_t* program_id, bool need_sdcard);
std::vector<std::string> npdm_compare(const Npdm& src, const Npdm& now, const std::vector<unsigned>& svcs);

// npdm_sdcard.py: бит SdCard в ACI0/FAH и ACID/FAC; ровно 2 изменённых байта, иначе отказ
// (Error "NPDM_SDCARD"), как у эталона.
Bytes npdm_add_sdcard(const Bytes& game_npdm);

// make_npdm: npdm игры -> SdCard -> svc -> гейт (program id = tid). Error "FAIL_GATE_NPDM".
Bytes make_game_npdm(const Bytes& game_npdm, uint64_t tid);

// make_mod_npdm: основа — npdm мода; "as-is" если права уже покрывают наши, иначе
// "merged" (дописано недостающее). header_diffs — отличия шапки от npdm игры.
struct ModNpdm {
    Bytes data;
    std::string mode;                     // "as-is" | "merged"
    std::vector<std::string> added, header_diffs;
};
ModNpdm make_mod_npdm(const Bytes& mod_npdm, const Bytes* game_npdm, uint64_t tid);

std::string tid_hex(uint64_t tid);        // "%016X"
bool parse_tid(const std::string& s, uint64_t& out);

}  // namespace mk
