// Квитанция nxredirect.json: порт write_receipt/read_receipt и exefs_ours_known эталона.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mk/receipt.hpp"

#include <algorithm>

namespace mk {

ReceiptRead parse_receipt_text(const std::string& text) {
    ReceiptRead r;
    r.present = true;
    try {
        r.data = ojson::parse(text);
    } catch (const std::exception& e) {
        r.broken = true;
        r.broken_why = e.what();
        r.data = ojson();
        return r;
    }
    if (!r.data.is_object()) {
        r.broken = true;
        r.broken_why = "это не объект JSON";
    }
    return r;
}

ReceiptRead read_receipt(Env& env, const std::string& title_dir) {
    std::string p = path_join(title_dir, kReceiptName);
    if (!env.fs.is_file(p)) return {};
    Bytes b = env.fs.read_file(p);
    return parse_receipt_text(std::string(b.begin(), b.end()));
}

static std::vector<std::string> str_list(const ojson& o, const char* key) {
    std::vector<std::string> v;
    if (o.is_object() && o.contains(key) && o[key].is_array())
        for (auto& x : o[key])
            if (x.is_string()) v.push_back(x.get<std::string>());
    return v;
}

static std::vector<std::string> sorted_unique(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

static ojson opt_or_base(const std::optional<std::string>& v, const ojson& base, const char* key) {
    if (v && !v->empty()) return *v;
    if (base.contains(key)) return base[key];
    return nullptr;
}

ojson make_receipt(const ReceiptFields& f, const ReceiptRead& base_r) {
    ojson base = base_r.sane() ? base_r.data : ojson::object();
    ojson d = ojson::object();
    d["schema"] = 1;
    d["tool"] = "nxredirect";
    d["version"] = f.version;
    d["game_tid"] = f.tid;
    d["slot"] = opt_or_base(f.slot, base, "slot");
    d["installed"] = "";
    d["module_sha256"] = opt_or_base(f.module_sha256, base, "module_sha256");
    d["npdm_sha256"] = opt_or_base(f.npdm_sha256, base, "npdm_sha256");
    d["module_name"] = opt_or_base(f.module_name, base, "module_name");
    auto z = f.zbic_files;
    auto bz = str_list(base, "zbic_files");
    z.insert(z.end(), bz.begin(), bz.end());
    d["zbic_files"] = sorted_unique(z);
    std::vector<std::string> nb = f.neighbours;
    std::sort(nb.begin(), nb.end());
    d["neighbours"] = nb;
    auto modes = str_list(base, "modes");
    modes.push_back(f.mode);
    d["modes"] = sorted_unique(modes);
    if (f.compat) {
        auto c = *f.compat;
        std::sort(c.begin(), c.end());
        d["compat"] = c;
    } else if (base.contains("compat") && !base["compat"].is_null() &&
               !(base["compat"].is_array() && base["compat"].empty())) {
        d["compat"] = base["compat"];
    } else {
        d["compat"] = ojson::array();
    }
    if (f.mod) d["mod"] = *f.mod;
    else d["mod"] = base.contains("mod") ? base["mod"] : ojson(nullptr);
    // sdk_swap — поле формата 1 (SDKSWAP-FORMAT): null — подмены нет
    if (f.sdk_swap) d["sdk_swap"] = *f.sdk_swap;
    else d["sdk_swap"] = base.contains("sdk_swap") ? base["sdk_swap"] : ojson(nullptr);
    // diag — отладочная сборка модуля (формат 1)
    if (f.diag) d["diag"] = *f.diag;
    else d["diag"] = base.contains("diag") && base["diag"].is_boolean() ? base["diag"].get<bool>() : false;
    // diag_gfx — второй отчёт gfx-NNNN.txt (формат 1): bool(base.get("diag_gfx"))
    if (f.diag_gfx) d["diag_gfx"] = *f.diag_gfx;
    else d["diag_gfx"] = base.contains("diag_gfx") && base["diag_gfx"].is_boolean() ? base["diag_gfx"].get<bool>() : false;
    // module_variant — вид сборки модуля одним словом (формат 1); null — модуль не ставился
    if (f.module_variant) d["module_variant"] = *f.module_variant;
    else d["module_variant"] = base.contains("module_variant") ? base["module_variant"] : ojson(nullptr);
    // owned / own_backups / foreign_backups / created_dirs и installed_by дописывает исполнитель
    return d;
}

std::string dump_receipt(const ojson& r) {
    return r.dump(2, ' ', false, nlohmann::detail::error_handler_t::replace) + "\n";
}

std::set<std::string> exefs_ours_known(const ReceiptRead& r) {
    std::set<std::string> k;
    if (!r.sane()) return k;
    const ojson& d = r.data;
    for (const char* key : {"slot", "module_name"})
        if (d.contains(key) && d[key].is_string() && !d[key].get<std::string>().empty()) k.insert(d[key].get<std::string>());
    for (auto& z : str_list(d, "zbic_files")) k.insert(z);
    if (d.contains("npdm_sha256") && d["npdm_sha256"].is_string() && !d["npdm_sha256"].get<std::string>().empty()) {
        k.insert("main.npdm");
        k.insert("main.npdm.orig");
    }
    if (d.contains("mod") && d["mod"].is_object())
        for (auto& x : str_list(d["mod"], "exefs")) k.insert(x);
    if (d.contains("sdk_swap") && d["sdk_swap"].is_object()) {
        const ojson& w = d["sdk_swap"];
        k.insert(w.contains("file") && w["file"].is_string() ? w["file"].get<std::string>() : std::string("sdk"));
        if (w.contains("foreign_backup") && w["foreign_backup"].is_string()) k.insert(w["foreign_backup"].get<std::string>());
    }
    return k;
}

}  // namespace mk
