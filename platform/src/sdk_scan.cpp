// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — найденные sdk через ядро (mk::scan_sdk). См. platform/sdk_scan.hpp.
#include "platform/sdk_scan.hpp"

#include <mk/nso.hpp>
#include <mk/sdk.hpp>
#include <mk/sdk_harvest.hpp>

#include "platform/core_fs.hpp"
#include "platform/log.hpp"
#include "platform/paths.hpp"

namespace mk::plat {

namespace {
constexpr const char* kLabelOwn = "modkit";
constexpr const char* kLabelDbi = "dbi";

// Журнал поиска sdk (бывало: файл DBI лежал в папке, а приложение писало «sdk не найдены» без
// причин). Для каждого файла в папках sdk — размер и почему он принят или отвергнут; исключение ядра — текстом.
void logSdkCandidates(mk::IFileSystem& fs, mk::Env& env, const std::string& dir, int depth) {
    std::vector<mk::DirEntry> list;
    try {
        list = fs.list_dir(dir);
    } catch (const std::exception& ex) {
        MK_LOG("sdk-scan: list %s failed: %s", dir.c_str(), ex.what());
        return;
    }
    MK_LOG("sdk-scan: %s — %zu entries", dir.c_str(), list.size());
    for (const auto& e : list) {
        const std::string p = mk::path_join(dir, e.name);
        if (e.is_dir) {
            MK_LOG("sdk-scan:   [dir] %s", p.c_str());
            if (depth < 2) logSdkCandidates(fs, env, p, depth + 1);
            continue;
        }
        try {
            const uint64_t size = fs.file_size(p);
            mk::Bytes head = fs.read_range(p, 0, mk::kNsoHeaderSize);
            mk::NsoHeader h;
            if (!mk::parse_nso_header(head.data(), head.size(), h)) {
                MK_LOG("sdk-scan:   %s size=%llu — not NSO0", p.c_str(), (unsigned long long)size);
                continue;
            }
            mk::Bytes raw = fs.read_file(p);
            mk::NsoImage n(raw, &env.hash);
            const auto name = n.module_name();
            const auto ver = n.mw_version();
            MK_LOG("sdk-scan:   %s size=%llu module=%s version=%s", p.c_str(), (unsigned long long)size,
                   name ? name->c_str() : "(none)", ver ? mk::version_text(*ver).c_str() : "(none)");
        } catch (const std::exception& ex) {
            MK_LOG("sdk-scan:   %s — error: %s", p.c_str(), ex.what());
        }
    }
}

std::string baseName(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}
}  // namespace

std::vector<FoundSdk> scanSdk(SdkScanStats& stats) {
    stats = SdkScanStats{};
    std::vector<FoundSdk> out;
    PosixFs& fs = posixFs();
    stats.ownDirExists = fs.is_dir(paths::kSdkDirOwn);
    stats.dbiDirExists = fs.is_dir(paths::kSdkDirDbi);
    // Раньше здесь была std::pair в обратном порядке (метка, папка): ядро искало папки «modkit»/«dbi», не находило
    // их и молча возвращало пусто — sdk на консоли не находились. Теперь — mk::SdkRoot.
    std::vector<mk::SdkRoot> roots;
    if (stats.ownDirExists) roots.push_back({paths::kSdkDirOwn, kLabelOwn});
    if (stats.dbiDirExists) roots.push_back({paths::kSdkDirDbi, kLabelDbi});
    if (roots.empty()) return out;

    mk::Env env{fs, nxHash(), nxClock()};
    for (const auto& r : roots) logSdkCandidates(fs, env, r.dir, 0);
    std::vector<mk::SdkEntry> entries;
    try {
        entries = mk::scan_sdk(env, roots);
    } catch (const mk::Error& e) {
        stats.error = e.what();
        MK_LOG("sdk: scan failed %s: %s", e.code().c_str(), e.what());
        return out;
    }
    for (const auto& e : entries) {
        FoundSdk f;
        f.path = e.file.path;
        f.fileName = baseName(e.file.path);
        if (e.file.version) {
            const auto& v = *e.file.version;
            f.version = Version3::make(static_cast<uint8_t>(v[0]), static_cast<uint8_t>(v[1]), static_cast<uint8_t>(v[2]));
            f.versionText = mk::version_text(v);
        }
        f.moduleIdHex = e.file.module_id;
        f.sha256Hex = e.file.sha256;
        for (const auto& s : e.sources) {
            if (s.first == kLabelOwn) f.inOwn = true;
            if (s.first == kLabelDbi) f.inDbi = true;
            f.allPaths.push_back(s.second);
        }
        if (f.allPaths.empty()) f.allPaths.push_back(f.path);
        f.sameVersionOtherBuild = e.same_version_other_id;
        // Подпись «из игры …» — только если source.json рядом называет ровно этот файл (sha256).
        if (const auto src = mk::harvest::read_source(env, e.file.path, e.file.sha256)) {
            f.fromGame = src->game;
            f.fromGameVersion = src->game_version;
            // Файл в такой папке всегда зовётся «sdk» — в списке показывается имя папки «nnSdk 19.3.13 (Celeste)».
            const std::string parent = mk::path_parent(e.file.path);
            if (f.fileName == "sdk" && !parent.empty()) f.fileName = baseName(parent);
        }
        out.push_back(std::move(f));
    }
    MK_LOG("sdk: %zu unique nnSdk (own dir %d, dbi dir %d)", out.size(), stats.ownDirExists, stats.dbiDirExists);
    return out;
}

}  // namespace mk::plat
