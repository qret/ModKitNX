// core_cli — ядро ModKitNX на ПК: те же действия, что эталонная реализация (install.py, zbic_nso.py, npdm_*.py,
// stubdiff.py), для сверки с ней (tests/host/compare_pc.py). Ничего не пишет без --apply (кроме явного -o у разовых команд).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "host_fs.hpp"
#include "mk/install.hpp"
#include "mk/nacp.hpp"
#include "mk/npdm.hpp"
#include "mk/sdk.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>

using namespace mk;
using oj = nlohmann::ordered_json;

namespace {

struct Args {
    std::string cmd;
    std::vector<std::string> pos;
    std::map<std::string, std::vector<std::string>> opt;
    bool has(const std::string& k) const { return opt.count(k) > 0; }
    std::string get(const std::string& k, const std::string& d = "") const {
        auto it = opt.find(k);
        return it == opt.end() || it->second.empty() ? d : it->second.back();
    }
};

const std::vector<std::string> kFlags = {"--apply", "--zbic-fix", "--zbic-only", "--json", "--swapped-sdk",
                                         "--no-autofix", "--allow-other-mods", "--keep-files", "--diag", "--diag-gfx"};

Args parse(int argc, char** argv) {
    Args a;
    if (argc > 1) a.cmd = argv[1];
    for (int i = 2; i < argc; i++) {
        std::string s = argv[i];
        if (s.size() > 1 && s[0] == '-') {
            bool flag = std::find(kFlags.begin(), kFlags.end(), s) != kFlags.end();
            if (flag || i + 1 >= argc) a.opt[s];
            else a.opt[s].push_back(argv[++i]);
        } else {
            a.pos.push_back(s);
        }
    }
    return a;
}

const char* kind_name(ActKind k) {
    switch (k) {
    case ActKind::MakeDir: return "mkdir";
    case ActKind::PutFile: return "put-file";
    case ActKind::PutBytes: return "put-bytes";
    case ActKind::SaveOrig: return "save-orig";
    case ActKind::PutReceipt: return "receipt";
    case ActKind::Delete: return "delete";
    case ActKind::Rename: return "rename";
    case ActKind::RemoveEmptyDir: return "rmdir-empty";
    }
    return "?";
}

oj plan_json(const Plan& p) {
    oj a = oj::array();
    for (auto& x : p.actions) {
        oj o = {{"kind", kind_name(x.kind)}, {"what", x.what}, {"dst", x.dst}};
        if (!x.src.empty()) o["src"] = x.src;
        if (!x.rel.empty()) o["rel"] = x.rel;
        if (!x.expect_sha256.empty()) o["sha256"] = x.expect_sha256;
        if (x.replaces_foreign) o["replaces_foreign"] = true;
        if (x.kind == ActKind::PutReceipt) o["receipt"] = x.receipt;
        a.push_back(o);
    }
    return {{"stamp", p.stamp}, {"actions", a}, {"lines", p.lines}, {"warnings", p.warnings}};
}

void out(const oj& j) { std::cout << j.dump(1, ' ', false, nlohmann::detail::error_handler_t::replace) << "\n"; }

int usage() {
    std::cerr << "core_cli <команда> ...\n"
                 "  zbic-scan <exefs> | zbic-rebuild <nso> -o <out> | zbic-check <rebuilt> --orig <nso>\n"
                 "  npdm-game <main.npdm> -o <out> [--tid T] | npdm-mod <mod npdm> --game <npdm> --tid T -o <out>\n"
                 "  install --exefs X --dest D --module M [--module-early M2] [--zbic-fix|--zbic-only] [--mod P]\n"
                 "          [--mod-files P] [--slot S] [--tid T] [--swap-sdk F] [--installed-by console|none] [--apply]\n"
                 "          [--now 2026-11-08T00:00:00] [--stamp 20261108-000000]\n"
                 "  uninstall --dest D --tid T [--keep-files] [--apply] | status --dest D --tid T\n"
                 "  profile --game-exefs X [--cnmt F] | forecast --sdk F --game-exefs X | detect --old F --new F --game-exefs X\n"
                 "  sdk-scan <папка>=<подпись> ... | sdk-swap --dest D --tid T --sdk F [--apply] | sdk-unswap --dest D --tid T [--apply]\n"
                 "  nacp <control.nacp> | classify <nso> | nca-sdk <расшифрованный заголовок>\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    Args a = parse(argc, argv);
    mkhost::StdFs fs;
    mkhost::Clock clock;
    clock.fixed_iso = a.get("--now");
    clock.fixed_stamp = a.get("--stamp");
    Env env{fs, soft_hash(), clock};
    try {
        if (a.cmd == "zbic-scan") {
            auto s = zbic_scan(fs, a.pos.at(0));
            out({{"ok", s.ok}, {"nso_files", s.nso_files}, {"not_nso", s.not_nso}, {"zbic_files", s.zbic_files},
                 {"zbic", s.zbic()}});
            return 0;
        }
        if (a.cmd == "zbic-rebuild") {
            ZbicRebuildInfo info;
            Bytes r = zbic_rebuild(fs.read_file(a.pos.at(0)), env.hash, &info);
            fs.write_file(a.get("-o"), r.data(), r.size());
            out({{"size_before", info.size_before}, {"size_after", info.size_after}, {"dst_sha256", info.dst_sha256}});
            return 0;
        }
        if (a.cmd == "zbic-check") {
            auto g = zbic_check(fs.read_file(a.pos.at(0)), fs.read_file(a.get("--orig")), env.hash);
            out({{"checks", g.checks}, {"problems", g.problems}});
            return g.ok(kMinZbicChecks) ? 0 : 1;
        }
        if (a.cmd == "npdm-game") {
            Bytes in = fs.read_file(a.pos.at(0));
            uint64_t tid = Npdm(in).program_id();
            if (a.has("--tid") && !parse_tid(a.get("--tid"), tid)) throw Error("FAIL_BAD_TID", "плохой TID");
            Bytes o = make_game_npdm(in, tid);
            fs.write_file(a.get("-o"), o.data(), o.size());
            out({{"sha256", sha256_hex(env.hash, o)}, {"size", o.size()}});
            return 0;
        }
        if (a.cmd == "npdm-mod") {
            uint64_t tid = 0;
            if (!parse_tid(a.get("--tid"), tid)) throw Error("FAIL_BAD_TID", "плохой TID");
            Bytes g = fs.read_file(a.get("--game"));
            ModNpdm m = make_mod_npdm(fs.read_file(a.pos.at(0)), &g, tid);
            fs.write_file(a.get("-o"), m.data.data(), m.data.size());
            out({{"mode", m.mode}, {"added", m.added}, {"header_diffs", m.header_diffs}, {"sha256", sha256_hex(env.hash, m.data)}});
            return 0;
        }
        if (a.cmd == "install") {
            InstallOptions o;
            o.exefs = a.get("--exefs");
            o.dest = a.get("--dest");
            o.tid = a.get("--tid");
            o.module.path = a.get("--module");
            o.module_early.path = a.get("--module-early", o.module.path);
            o.slot = a.get("--slot");
            o.mod = a.get("--mod");
            o.mod_files = a.get("--mod-files");
            o.swap_sdk = a.get("--swap-sdk");
            o.zbic_fix = a.has("--zbic-fix");
            o.swapped_sdk = a.has("--swapped-sdk");
            o.no_autofix = a.has("--no-autofix");
            o.allow_other_mods = a.has("--allow-other-mods");
            o.diag = a.has("--diag");
            o.diag_gfx = a.has("--diag-gfx");
            std::string ib = a.get("--installed-by", "console");
            o.installed_by = ib == "none" ? "" : ib;
            InstallPlan p = a.has("--zbic-only") ? plan_zbic_only(env, o) : plan_install(env, o);
            oj j = plan_json(p);
            j["tid"] = p.tid;
            j["slot"] = p.slot;
            j["updating"] = p.updating;
            j["npdm_mode"] = p.npdm_mode;
            j["early"] = p.early;
            j["busy_game"] = p.busy_game;
            j["zbic_files"] = p.zbic.zbic_files;
            j["keeping"] = p.keeping;
            j["replacing_foreign"] = p.replacing_foreign;
            j["module_variant"] = p.module_variant;
            if (a.has("--apply")) {
                ApplyReport r = apply_plan(env, p);
                j["applied"] = {{"done", r.done}, {"skipped", r.skipped}, {"problems", r.problems}};
            }
            out(j);
            return 0;
        }
        if (a.cmd == "uninstall") {
            Plan p = plan_uninstall(env, a.get("--dest"), a.get("--tid"), !a.has("--keep-files"));
            oj j = plan_json(p);
            if (a.has("--apply")) {
                ApplyReport r = apply_plan(env, p);
                j["applied"] = {{"done", r.done}, {"skipped", r.skipped}, {"problems", r.problems}};
            }
            out(j);
            return 0;
        }
        if (a.cmd == "status") {
            auto s = inspect_installed(env, a.get("--dest"), a.get("--tid"));
            oj mg = nullptr, stale = oj::array();
            if (a.has("--game-exefs")) {
                std::vector<std::string> st;
                auto m = installed_matches_game(env, a.get("--dest"), a.get("--tid"), a.get("--game-exefs"), &st);
                if (m) mg = *m;
                stale = st;
            }
            oj slots = oj::object();
            for (auto& [k, v] : s.slots) slots[k] = {v.first, v.second};
            out({{"tid", s.tid}, {"dir_exists", s.dir_exists}, {"receipt", s.receipt.sane() ? s.receipt.data : oj(nullptr)},
                 {"receipt_broken", s.receipt.broken}, {"slots", slots}, {"has_npdm_orig", s.has_npdm_orig},
                 {"has_romfs", s.has_romfs}, {"deferred", s.deferred},
                 {"module_matches", s.module_matches ? oj(*s.module_matches) : oj(nullptr)},
                 {"matches_game", mg}, {"stale", stale}});
            return 0;
        }
        if (a.cmd == "profile") {
            std::string c = a.get("--cnmt");
            out(sdk_profile(env, a.get("--game-exefs"), c.empty() ? nullptr : &c));
            return 0;
        }
        if (a.cmd == "forecast") { out(sdk_forecast(env, a.get("--sdk"), a.get("--game-exefs"))); return 0; }
        if (a.cmd == "detect") {
            oj d = sdk_detect(env, a.get("--old"), a.get("--new"), a.get("--game-exefs"));
            out(d);
            return d["new_unfixed"].empty() ? 0 : 1;
        }
        if (a.cmd == "sdk-scan") {
            std::vector<SdkRoot> roots;
            for (auto& p : a.pos) {
                auto eq = p.rfind('=');
                roots.push_back({eq == std::string::npos ? p : p.substr(0, eq), eq == std::string::npos ? "" : p.substr(eq + 1)});
            }
            oj arr = oj::array();
            for (auto& e : scan_sdk(env, roots)) {
                oj src = oj::array();
                for (auto& [l, p] : e.sources) src.push_back({{"label", l}, {"path", p}});
                arr.push_back({{"version", e.file.version ? oj(version_text(*e.file.version)) : oj(nullptr)},
                               {"module_id", e.file.module_id}, {"sha256", e.file.sha256}, {"sources", src},
                               {"same_version_other_id", e.same_version_other_id}});
            }
            out(arr);
            return 0;
        }
        if (a.cmd == "sdk-swap" || a.cmd == "sdk-unswap") {
            Plan p;
            if (a.cmd == "sdk-swap") {
                auto f = identify_sdk(env, a.get("--sdk"), "cli");
                if (!f) throw Error("FAIL_SDK_NOT_NNSDK", "не NSO: " + a.get("--sdk"));
                p = plan_swap_sdk(env, a.get("--dest"), a.get("--tid"), *f);
            } else {
                p = plan_unswap_sdk(env, a.get("--dest"), a.get("--tid"));
            }
            oj j = plan_json(p);
            if (a.has("--apply")) {
                ApplyReport r = apply_plan(env, p);
                j["applied"] = {{"done", r.done}, {"skipped", r.skipped}, {"problems", r.problems}};
            }
            out(j);
            return 0;
        }
        if (a.cmd == "nacp") {
            Bytes b = fs.read_file(a.pos.at(0));
            NacpInfo n = parse_nacp(b.data(), b.size());
            auto o = [](const std::optional<std::string>& s) { return s ? oj(*s) : oj(nullptr); };
            out({{"title", o(n.title)}, {"publisher", o(n.publisher)}, {"title_lang", o(n.title_lang)},
                 {"display_version", o(n.display_version)}, {"languages", n.languages},
                 {"compressed", n.table_was_compressed}});
            return 0;
        }
        if (a.cmd == "classify") {
            auto c = classify_nso(fs.read_file(a.pos.at(0)));
            out({{"class", c.first}, {"why", c.second}});
            return 0;
        }
        if (a.cmd == "nca-sdk") {
            Bytes b = fs.read_file(a.pos.at(0));
            auto v = nca_sdk_addon_version(b.data(), b.size());
            if (!v) throw Error("NCA_BAD", "нет магии NCA3/NCA2");
            out({{"raw", *v}, {"text", SdkAddonVersion::from_u32(*v).text()}});
            return 0;
        }
    } catch (const Error& e) {
        out({{"error", e.code()}, {"message", e.what()}});
        return 1;
    } catch (const std::exception& e) {
        out({{"error", "EXCEPTION"}, {"message", e.what()}});
        return 1;
    }
    return usage();
}
