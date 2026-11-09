// ZBIC: распаковка (upstream zstd + BIC), несжатый NSO (zbic_nso.rebuild), гейт (zbic_nso.check).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mk/zbic.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

extern "C" {
void mk_zbic_set_bic(int on);
int mk_zbic_bic_calls(void);
void mk_bic_table_init(void);
}

namespace mk {

namespace {
constexpr uint32_t kZbicMagic = 0x4349425A;   // "ZBIC"
constexpr uint32_t kZstdMagic = 0xFD2FB528;
const char* kSeg[3] = {"text", "rodata", "data"};

struct BicGuard {
    explicit BicGuard(int on) { mk_zbic_set_bic(on); }
    ~BicGuard() { mk_zbic_set_bic(0); }
};
}  // namespace

Bytes zbic_decompress(const uint8_t* src, size_t n, size_t expected) {
    static std::once_flag once;
    std::call_once(once, [] { mk_bic_table_init(); });
    // Флаг BIC в zstd_bic_glue.c общий для процесса (не thread_local — см. там): одна распаковка за раз.
    static std::mutex lock;
    std::lock_guard<std::mutex> hold(lock);
    Bytes buf(src, src + n);
    Bytes out(expected);
    ZSTD_DCtx* d = ZSTD_createDCtx();
    if (!d) throw Error("ZBIC_DECODE", "zstd: нет памяти под контекст");
    ZSTD_DCtx_setParameter(d, ZSTD_d_windowLogMax, 31);
    size_t p = 0, o = 0;
    std::string err;
    while (p < n) {
        if (n - p < 5) { err = "кадр короче заголовка"; break; }
        uint32_t magic = rd32(buf.data() + p);
        int bic;
        if (magic == kZbicMagic) { bic = 1; wr32(buf.data() + p, kZstdMagic); }
        else if (magic == kZstdMagic) bic = 0;
        else {
            char m[16];
            std::snprintf(m, sizeof m, "%08X", magic);
            err = std::string("не кадр ZBIC/zstd: магия ") + m;
            break;
        }
        BicGuard g(bic);
        size_t fs = ZSTD_findFrameCompressedSize(buf.data() + p, n - p);
        if (ZSTD_isError(fs)) { err = std::string("кадр не разобрался: ") + ZSTD_getErrorName(fs); break; }
        size_t r = ZSTD_decompressDCtx(d, out.data() + o, expected - o, buf.data() + p, fs);
        if (ZSTD_isError(r)) { err = std::string("кадр не распаковался: ") + ZSTD_getErrorName(r); break; }
        o += r;
        p += fs;
    }
    ZSTD_freeDCtx(d);
    if (!err.empty()) throw Error("ZBIC_DECODE", "ZBIC: " + err);
    if (o != expected)
        throw Error("ZBIC_DECODE", "ZBIC: распаковано " + std::to_string(o) + " байт, а ожидалось " +
                                       std::to_string(expected));
    return out;
}

Bytes zbic_rebuild(const Bytes& raw, IHash& hash, ZbicRebuildInfo* info) {
    NsoHeader h;
    if (!parse_nso_header(raw.data(), raw.size(), h)) throw Error("ZBIC_NOT_NSO", "не NSO0");
    if (!h.zbic()) {
        char f[16];
        std::snprintf(f, sizeof f, "0x%02x", h.flags);
        throw Error("ZBIC_NOT_ZBIC", std::string("нет бита ZBIC (flags = ") + f + ") — пересобирать нечего");
    }
    size_t mno = std::min<size_t>(h.module_name_offset, raw.size());
    size_t mlen = std::min<size_t>(h.module_name_size, raw.size() - mno);
    uint64_t total = kNsoHeaderSize + mlen;
    for (auto& s : h.seg) total += s.size;
    Bytes out;
    out.reserve(size_t(total));
    out.insert(out.end(), raw.begin(), raw.begin() + kNsoHeaderSize);
    out.insert(out.end(), raw.begin() + mno, raw.begin() + mno + mlen);
    wr32(out.data() + 0x1C, uint32_t(kNsoHeaderSize));
    wr32(out.data() + 0x2C, h.module_name_size);
    wr32(out.data() + 0x0C, kNsoFlagsUncompressed);
    uint64_t pos = kNsoHeaderSize + uint64_t(h.module_name_size);
    for (int n = 0; n < 3; n++) {
        const NsoSegment& s = h.seg[n];
        Bytes data;
        try {
            data = nso_segment(raw, h, n, nullptr);
        } catch (const Error& e) {
            throw Error("ZBIC_DECODE", std::string("сегмент ") + kSeg[n] + " не распаковался: " + e.what());
        }
        if (data.size() != s.size)
            throw Error("ZBIC_DECODE", std::string("сегмент ") + kSeg[n] + ": размер не сошёлся с заголовком");
        if (hash.sha256(data.data(), data.size()) != s.hash)
            throw Error("ZBIC_HASH", std::string("сегмент ") + kSeg[n] + ": sha256 не сошлась с заголовком NSO");
        wr32(out.data() + 0x10 + n * 0x10, uint32_t(pos));
        wr32(out.data() + 0x14 + n * 0x10, s.memory_offset);
        wr32(out.data() + 0x18 + n * 0x10, s.size);
        wr32(out.data() + 0x60 + n * 4, s.size);
        out.insert(out.end(), data.begin(), data.end());
        pos += s.size;
    }
    if (info) {
        info->size_before = raw.size();
        info->size_after = out.size();
        info->flags_before = h.flags;
        info->module_id = h.module_id_hex();
        info->src_sha256 = sha256_hex(hash, raw);
        info->dst_sha256 = sha256_hex(hash, out);
    }
    return out;
}

GateResult zbic_check(const Bytes& rebuilt, const Bytes& orig, IHash& hash) {
    GateResult g;
    auto& P = g.problems;
    NsoHeader o, r;
    if (!parse_nso_header(orig.data(), orig.size(), o)) { P.push_back("оригинал не NSO"); return g; }
    g.checks++;   // 1
    if (!o.zbic()) P.push_back("у оригинала нет бита ZBIC: этот гейт проверяет снятие ZBIC");
    if (!parse_nso_header(rebuilt.data(), rebuilt.size(), r)) { P.push_back("пересобранный файл не NSO"); return g; }
    g.checks++;   // 2
    g.checks++;   // 3
    if (r.flags != kNsoFlagsUncompressed) P.push_back("флаги пересобранного NSO не 0x38");
    g.checks++;   // 4
    if (r.zbic()) P.push_back("в пересобранном NSO остался бит ZBIC");
    g.checks++;   // 5
    for (int i = 0; i < 3; i++)
        if (r.seg[i].compressed) P.push_back(std::string("сегмент помечен сжатым: ") + kSeg[i]);
    g.checks++;   // 6
    for (int i = 0; i < 3; i++)
        if (r.seg[i].compressed_size != r.seg[i].size) P.push_back(std::string("сегмент ") + kSeg[i] + ": CompressedSize != размер");
    g.checks++;   // 7
    for (int i = 0; i < 3; i++)
        if (o.seg[i].size != r.seg[i].size) P.push_back(std::string("сегмент ") + kSeg[i] + ": размер не как в оригинале");
    g.checks++;   // 8
    for (int i = 0; i < 3; i++)
        if (o.seg[i].hash != r.seg[i].hash) P.push_back(std::string("сегмент ") + kSeg[i] + ": хеш в заголовке подменён");
    g.checks++;   // 9
    for (int i = 0; i < 3; i++) {
        const NsoSegment& s = r.seg[i];
        if (uint64_t(s.file_offset) + s.size > rebuilt.size()) {
            P.push_back(std::string("сегмент ") + kSeg[i] + ": в файле меньше байт, чем по заголовку");
            continue;
        }
        if (hash.sha256(rebuilt.data() + s.file_offset, s.size) != s.hash)
            P.push_back(std::string("сегмент ") + kSeg[i] + ": sha256 содержимого не сошлась с заголовком");
    }
    g.checks++;   // 10
    if (r.module_id != o.module_id) P.push_back("ModuleId не совпал с оригиналом — это другая версия игры");
    if (r.bss_size != o.bss_size) P.push_back("bss не как в оригинале");
    g.checks++;   // 11
    for (int i = 0; i < 3; i++)
        if (o.seg[i].memory_offset != r.seg[i].memory_offset)
            P.push_back(std::string("сегмент ") + kSeg[i] + ": memory_offset не как в оригинале");
    g.checks++;   // 12
    if (r.seg[0].memory_offset != 0) P.push_back("text начинается не с нуля");
    for (int i = 0; i < 3; i++)
        if (r.seg[i].memory_offset % 0x1000) P.push_back(std::string("сегмент ") + kSeg[i] + " не выровнен на страницу");
    for (int i = 0; i < 2; i++)
        if (uint64_t(r.seg[i].memory_offset) + r.seg[i].size > r.seg[i + 1].memory_offset)
            P.push_back(std::string("сегмент ") + kSeg[i] + " налезает на " + kSeg[i + 1]);
    g.checks++;   // 13
    uint64_t pos = kNsoHeaderSize + uint64_t(r.module_name_size);
    if (r.module_name_offset != kNsoHeaderSize) P.push_back("имя модуля лежит не сразу за заголовком");
    for (int i = 0; i < 3; i++) {
        if (r.seg[i].file_offset != pos) P.push_back(std::string("сегмент ") + kSeg[i] + ": дыра или нахлёст в файле");
        pos += r.seg[i].size;
    }
    if (rebuilt.size() != pos) P.push_back("размер файла не по раскладке (лишний хвост или обрезан)");
    return g;
}

ZbicScan zbic_scan(IFileSystem& fs, const std::string& exefs) {
    ZbicScan s;
    if (!fs.is_dir(exefs)) return s;
    s.ok = true;
    for (auto& e : fs.list_dir(exefs)) {
        if (e.is_dir) continue;
        std::string p = path_join(exefs, e.name);
        Bytes head = fs.read_range(p, 0, kNsoHeaderSize);
        NsoHeader h;
        if (!parse_nso_header(head.data(), head.size(), h)) { s.not_nso.push_back(e.name); continue; }
        s.nso_files.push_back(e.name);
        if (h.zbic()) s.zbic_files.push_back(e.name);
    }
    return s;
}

ZbicNeed zbic_need(const std::array<int, 3>* hos, const std::array<int, 3>* ams, bool zbic_in_exefs) {
    auto mk_ = [](const char* v) { return ZbicNeed{v, std::string("core/zbic/") + v}; };
    if (!zbic_in_exefs) return mk_("not_compressed");
    if (hos && *hos >= std::array<int, 3>{22, 0, 0}) return mk_("not_needed_hos22");
    if (ams && *ams >= std::array<int, 3>{1, 12, 0}) return mk_("not_needed_ams112");
    return mk_("needed");
}

}  // namespace mk
