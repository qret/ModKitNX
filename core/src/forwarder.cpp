// ModKitNX core — форвардер: сборка NCA/PFS0/romfs/NACP/cnmt. См. include/mk/forwarder.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later
// Based on sphaira by ITotalJustice (GPL-3.0): sphaira/source/owo.cpp
//
// Раскладка — побайтно как у sphaira (owo.cpp: build_romfs_into_file, build_pfs0, write_nca_pfs0, write_nca_romfs,
// create_meta_nca, patch_npdm, patch_nacp; romfs — по hacbrewpack) и у порта forwarder.c, проверенного на консоли.
// Поля пишутся по смещениям (little-endian), без упакованных структур: тот же код — на консоли и на ПК.
#include "mk/forwarder.hpp"

#include <cstring>

namespace mk::forwarder {

namespace {

constexpr uint32_t kIvfcLevels = 6;
constexpr uint32_t kIvfcBlock = 0x4000;
constexpr uint32_t kExefsHashBlock = 0x10000;
constexpr uint32_t kMetaHashBlock = 0x1000;
constexpr uint32_t kPfs0Padding = 0x200;
constexpr uint32_t kRomfsEmpty = 0xFFFFFFFFu;
constexpr uint32_t kRomfsFileData = 0x200;
constexpr size_t kNcaHeader = 0xC00;
constexpr size_t kFsHeader = 0x200;
constexpr size_t kMaxIcon = 0x20000;

enum : uint8_t { kNcaProgram = 0, kNcaMeta = 1, kNcaControl = 2 };
enum : uint8_t { kFsRomfs = 0, kFsPfs0 = 1 };
enum : uint8_t { kHashSha256 = 2, kHashIntegrity = 3 };
enum : uint8_t { kEncNone = 1 };

void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i)); }
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) | uint64_t(get32(p + 4)) << 32; }

[[noreturn]] void bad(const char* code, const std::string& what) { throw Error(code, what); }

// Буфер с позицией записи, как BufHelper sphaira: запись за конец растит буфер, seek назад — перезапись.
struct Buf {
    Bytes b;
    size_t off = 0;
    void write(const void* data, size_t n) {
        if (!n) return;
        if (off + n > b.size()) b.resize(off + n);
        std::memcpy(b.data() + off, data, n);
        off += n;
    }
    void write(const Bytes& d) { write(d.data(), d.size()); }
    void zeros(size_t n) {
        if (!n) return;
        if (off + n > b.size()) b.resize(off + n);
        std::memset(b.data() + off, 0, n);
        off += n;
    }
    // До кратного block — и целый block, когда уже кратно (как write_padding у sphaira и hacbrewpack).
    size_t pad(size_t at, size_t block) {
        const size_t n = block - (at % block);
        zeros(n);
        return n;
    }
};

uint64_t align64(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
uint32_t align32(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

struct FileEntry {
    std::string name;   // romfs — с ведущим '/'
    const Bytes* data;
};

// ---------------- romfs: все файлы в корне ----------------

uint32_t romfs_hash(uint32_t parent, const std::string& name, size_t start, size_t len) {
    uint32_t h = parent ^ 123456789u;
    for (size_t i = 0; i < len; ++i) {
        h = (h >> 5) | (h << 27);
        h ^= uint8_t(name[start + i]);
    }
    return h;
}

uint32_t romfs_hash_count(uint32_t n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    uint32_t c = n;
    while (c % 2 == 0 || c % 3 == 0 || c % 5 == 0 || c % 7 == 0 || c % 11 == 0 || c % 13 == 0 || c % 17 == 0) ++c;
    return c;
}

constexpr size_t kRomfsHeader = 0x50, kRomfsDirEntry = 0x18, kRomfsFileEntry = 0x20;

// Образ romfs; unpadded — размер до выравнивания на 0x4000.
Bytes romfs_build(const std::vector<FileEntry>& files, uint64_t& unpadded) {
    const uint32_t count = uint32_t(files.size());
    const uint32_t dirHashCount = romfs_hash_count(1), fileHashCount = romfs_hash_count(count);
    std::vector<uint32_t> dirHash(dirHashCount, kRomfsEmpty), fileHash(fileHashCount, kRomfsEmpty);
    std::vector<uint64_t> dataOff(count);
    std::vector<uint32_t> entryOff(count);
    uint32_t fileTableSize = 0;
    uint64_t partition = 0;
    for (uint32_t i = 0; i < count; ++i) {
        partition = align64(partition, 0x10);
        dataOff[i] = partition;
        partition += files[i].data->size();
        entryOff[i] = fileTableSize;
        fileTableSize += uint32_t(kRomfsFileEntry + align32(uint32_t(files[i].name.size() - 1), 4));
    }
    Bytes fileTable(fileTableSize, 0), dirTable(kRomfsDirEntry, 0);
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t* e = fileTable.data() + entryOff[i];
        const uint32_t nameLen = uint32_t(files[i].name.size() - 1);
        const uint32_t h = romfs_hash(0, files[i].name, 1, nameLen);
        put32(e + 0x00, 0);                                                     // parent — корень
        put32(e + 0x04, i + 1 < count ? entryOff[i + 1] : kRomfsEmpty);         // sibling
        put64(e + 0x08, dataOff[i]);
        put64(e + 0x10, files[i].data->size());
        put32(e + 0x18, fileHash[h % fileHashCount]);                          // nextHash
        fileHash[h % fileHashCount] = entryOff[i];
        put32(e + 0x1C, nameLen);
        std::memcpy(e + kRomfsFileEntry, files[i].name.data() + 1, nameLen);
    }
    {
        uint8_t* d = dirTable.data();
        const uint32_t h = romfs_hash(0, std::string(), 0, 0);
        put32(d + 0x00, 0);
        put32(d + 0x04, kRomfsEmpty);
        put32(d + 0x08, kRomfsEmpty);
        put32(d + 0x0C, count ? entryOff[0] : kRomfsEmpty);
        put32(d + 0x10, dirHash[h % dirHashCount]);
        dirHash[h % dirHashCount] = 0;
        put32(d + 0x14, 0);
    }
    uint8_t hdr[kRomfsHeader] = {};
    const uint64_t dirHashOff = align64(partition + kRomfsFileData, 4);
    const uint64_t dirTableOff = dirHashOff + dirHashCount * 4ULL;
    const uint64_t fileHashOff = dirTableOff + kRomfsDirEntry;
    const uint64_t fileTableOff = fileHashOff + fileHashCount * 4ULL;
    put64(hdr + 0x00, kRomfsHeader);
    put64(hdr + 0x08, dirHashOff);
    put64(hdr + 0x10, dirHashCount * 4ULL);
    put64(hdr + 0x18, dirTableOff);
    put64(hdr + 0x20, kRomfsDirEntry);
    put64(hdr + 0x28, fileHashOff);
    put64(hdr + 0x30, fileHashCount * 4ULL);
    put64(hdr + 0x38, fileTableOff);
    put64(hdr + 0x40, fileTableSize);
    put64(hdr + 0x48, kRomfsFileData);

    Buf out;
    out.write(hdr, sizeof(hdr));
    for (uint32_t i = 0; i < count; ++i) {
        out.off = dataOff[i] + kRomfsFileData;
        out.write(*files[i].data);
    }
    out.off = dirHashOff;
    auto writeTable = [&out](const std::vector<uint32_t>& t) {
        for (uint32_t v : t) {
            uint8_t w[4];
            put32(w, v);
            out.write(w, 4);
        }
    };
    writeTable(dirHash);
    out.write(dirTable);
    writeTable(fileHash);
    out.write(fileTable);
    out.off = out.b.size();
    unpadded = out.off;
    out.pad(out.off, kIvfcBlock);
    return std::move(out.b);
}

// ---------------- PFS0 и слои хешей ----------------

Bytes pfs0_build(const std::vector<FileEntry>& files) {
    std::string strings;
    Bytes table(files.size() * 0x18, 0);
    uint64_t dataOff = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        uint8_t* e = table.data() + i * 0x18;
        put64(e + 0x00, dataOff);
        put64(e + 0x08, files[i].data->size());
        put32(e + 0x10, uint32_t(strings.size()));
        put32(e + 0x14, 0);
        dataOff += files[i].data->size();
        strings.append(files[i].name);
        strings.push_back('\0');
    }
    strings.resize((strings.size() + 0x1F) & ~size_t(0x1F), '\0');
    uint8_t hdr[0x10] = {};
    std::memcpy(hdr, "PFS0", 4);
    put32(hdr + 4, uint32_t(files.size()));
    put32(hdr + 8, uint32_t(strings.size()));
    Buf out;
    out.write(hdr, sizeof(hdr));
    out.write(table);
    out.write(strings.data(), strings.size());
    for (const auto& f : files) out.write(*f.data);
    return std::move(out.b);
}

Bytes hash_layer(IHash& h, const Bytes& src, size_t block) {
    Bytes out;
    for (size_t i = 0; i < src.size(); i += block) {
        const size_t n = i + block >= src.size() ? src.size() - i : block;
        const Digest d = h.sha256(src.data() + i, n);
        out.insert(out.end(), d.begin(), d.end());
    }
    return out;
}

// ---------------- NCA ----------------

uint8_t* fs_header(Bytes& hdr, int index) { return hdr.data() + 0x400 + index * kFsHeader; }

void write_section(Bytes& hdr, int index, uint64_t start, uint64_t end) {
    uint8_t* e = hdr.data() + 0x240 + index * 0x10;
    put32(e + 0, uint32_t(start / 0x200));
    put32(e + 4, uint32_t(end / 0x200));
    e[8] = 1;   // всегда 1
}

uint64_t section_start(const Bytes& hdr, int index) {
    if (index == 0) return kNcaHeader;
    return uint64_t(get32(hdr.data() + 0x240 + (index - 1) * 0x10 + 4)) * 0x200;
}

void finish_fs_header(IHash& h, Bytes& hdr, int index) {
    const Digest d = h.sha256(fs_header(hdr, index), kFsHeader);
    std::memcpy(hdr.data() + 0x280 + index * 0x20, d.data(), d.size());
}

void nca_pfs0(IHash& h, Bytes& hdr, int index, const std::vector<FileEntry>& files, uint32_t block, Buf& out) {
    const uint64_t start = section_start(hdr, index);
    const Bytes pfs0 = pfs0_build(files);
    const Bytes table = hash_layer(h, pfs0, block);
    const Digest master = h.sha256(table.data(), table.size());
    out.write(table);
    const size_t padding = out.pad(table.size(), kPfs0Padding);
    out.write(pfs0);
    out.pad(out.off, 0x200);
    write_section(hdr, index, start, out.off);

    uint8_t* fs = fs_header(hdr, index);
    put16(fs + 0x0, 2);
    fs[0x2] = kFsPfs0;
    fs[0x3] = kHashSha256;
    fs[0x4] = kEncNone;
    std::memcpy(fs + 0x8, master.data(), master.size());   // master_hash
    put32(fs + 0x28, block);                                 // block_size
    put32(fs + 0x2C, 2);                                     // layer_count
    put64(fs + 0x30, 0);                                     // hash_layer.offset
    put64(fs + 0x38, table.size());                          // hash_layer.size
    put64(fs + 0x40, table.size() + padding);                // pfs0_layer.offset
    put64(fs + 0x48, pfs0.size());                           // pfs0_layer.size
    finish_fs_header(h, hdr, index);
}

void nca_romfs(IHash& h, Bytes& hdr, int index, const std::vector<FileEntry>& files, Buf& out) {
    const uint64_t start = section_start(hdr, index);
    Bytes ivfc[kIvfcLevels];
    uint64_t romfsSize = 0;
    ivfc[5] = romfs_build(files, romfsSize);
    uint64_t sizes[kIvfcLevels] = {};
    sizes[5] = romfsSize;
    for (int b = 4; b >= 0; --b) {
        ivfc[b] = hash_layer(h, ivfc[b + 1], kIvfcBlock);
        Buf pad;
        pad.b = std::move(ivfc[b]);
        pad.off = pad.b.size();
        pad.pad(pad.off, kIvfcBlock);
        ivfc[b] = std::move(pad.b);
        sizes[b] = ivfc[b].size();
    }
    for (const auto& lv : ivfc) out.write(lv);
    out.pad(out.off, 0x200);
    const Digest master = h.sha256(ivfc[0].data(), ivfc[0].size());
    write_section(hdr, index, start, out.off);

    uint8_t* fs = fs_header(hdr, index);
    put16(fs + 0x0, 2);
    fs[0x2] = kFsRomfs;
    fs[0x3] = kHashIntegrity;
    fs[0x4] = kEncNone;
    std::memcpy(fs + 0x8, "IVFC", 4);
    put32(fs + 0xC, 0x20000);       // version
    put32(fs + 0x10, 0x20);         // master_hash_size
    put32(fs + 0x14, 7);            // max_layers
    uint64_t logical = 0;
    for (uint32_t i = 0; i < kIvfcLevels; ++i) {
        uint8_t* lv = fs + 0x18 + i * 0x18;
        put64(lv + 0x0, logical);
        put64(lv + 0x8, sizes[i]);
        put32(lv + 0x10, 0x0E);     // log2(0x4000)
        logical += sizes[i];
    }
    std::memcpy(fs + 0xC8, master.data(), master.size());
    finish_fs_header(h, hdr, index);
}

Nca nca_finish(IHash& h, Bytes& hdr, uint64_t tid, uint8_t ncaType, uint8_t ncmType, Buf& out,
               const HeaderCryptFn& crypt) {
    std::memcpy(hdr.data() + 0x200, "NCA3", 4);
    hdr[0x204] = 0;                    // distribution: System
    hdr[0x205] = ncaType;
    put64(hdr.data() + 0x208, out.b.size());
    put64(hdr.data() + 0x210, tid);
    put32(hdr.data() + 0x21C, 0x000C1100);   // sdk_version, как у sphaira
    crypt(hdr.data(), hdr.size());
    std::memcpy(out.b.data(), hdr.data(), kNcaHeader);
    Nca n;
    n.data = std::move(out.b);
    n.hash = h.sha256(n.data.data(), n.data.size());
    n.ncm_type = ncmType;
    return n;
}

// NcmContentInfo (0x18): content id (16 байт хеша), размер 40 бит, attr, тип, id_offset.
void content_info(uint8_t* p, const Digest& hash, uint64_t size, uint8_t ncmType) {
    std::memcpy(p, hash.data(), 0x10);
    put32(p + 0x10, uint32_t(size));
    p[0x14] = uint8_t(size >> 32);
    p[0x15] = 0;
    p[0x16] = ncmType;
    p[0x17] = 0;
}

}  // namespace

std::string next_argv(const std::string& nro_path, const std::string& args) {
    return args.empty() ? nro_path : nro_path + ' ' + args;
}

uint64_t title_id(const std::string& nro_path, const std::string& args, IHash& hash) {
    const std::string s = nro_path + next_argv(nro_path, args);
    const Digest d = hash.sha256(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    return kPrefix | (get64(d.data()) & 0x00FFFFFFFFFFF000ULL);
}

// ---------------- NPDM ----------------

uint32_t kernel_flags_three_cores() {
    const uint32_t lowestPriority = 59, highestPriority = 28, lowestCpu = 0, highestCpu = 2;
    const uint32_t d = (((highestCpu << 8) | lowestCpu) << 6 | highestPriority) << 6 | lowestPriority;
    return d << 4;
}

namespace {
bool patch_capability(Bytes& npdm, uint64_t off, uint64_t size, uint32_t bits, uint32_t value) {
    const uint32_t pattern = (1u << bits) - 1;
    const uint32_t mask = (1u << bits) | pattern;
    if (off > npdm.size() || size > npdm.size() - off) return false;
    for (uint64_t i = 0; i + 4 <= size; i += 4) {
        uint8_t* p = npdm.data() + off + i;
        if ((get32(p) & mask) == pattern) {
            put32(p, value | pattern);
            return true;
        }
    }
    return false;
}
}  // namespace

void patch_npdm(Bytes& npdm, uint64_t tid, bool force_debug) {
    if (npdm.size() < 0x80 || std::memcmp(npdm.data(), "META", 4) != 0) bad("NPDM_BAD", "main.npdm: нет заголовка META");
    const uint32_t aci0 = get32(npdm.data() + 0x70), acid = get32(npdm.data() + 0x78);
    if (uint64_t(aci0) + 0x40 > npdm.size() || uint64_t(acid) + 0x240 > npdm.size() ||
        std::memcmp(npdm.data() + aci0, "ACI0", 4) != 0 || std::memcmp(npdm.data() + acid + 0x200, "ACID", 4) != 0)
        bad("NPDM_BAD", "main.npdm: нет ACI0/ACID");
    uint8_t* meta = npdm.data();
    std::memset(meta + 0x20, 0, 0x10);
    std::memcpy(meta + 0x20, "Application", 11);
    std::memset(meta + 0x30, 0, 0x10);                                    // product_code
    meta[0xC] = uint8_t((meta[0xC] & ~0x0E) | (1u << 1));                 // адресное пространство 36 бит
    put64(npdm.data() + aci0 + 0x10, tid);
    put64(npdm.data() + acid + 0x210, tid);
    put64(npdm.data() + acid + 0x218, tid);
    const uint64_t aciKac = aci0 + uint64_t(get32(npdm.data() + aci0 + 0x30));
    const uint64_t aciKacSize = get32(npdm.data() + aci0 + 0x34);
    const uint64_t acidKac = acid + uint64_t(get32(npdm.data() + acid + 0x230));
    const uint64_t acidKacSize = get32(npdm.data() + acid + 0x234);
    const uint32_t flags = kernel_flags_three_cores();
    const bool a = patch_capability(npdm, aciKac, aciKacSize, 3, flags);
    const bool b = patch_capability(npdm, acidKac, acidKacSize, 3, flags);
    if (force_debug) {
        patch_capability(npdm, aciKac, aciKacSize, 16, 1u << 19);
        patch_capability(npdm, acidKac, acidKacSize, 16, 1u << 19);
    }
    if (!a || !b) bad("NPDM_BAD", "main.npdm: нет описателя флагов ядра");
}

// ---------------- NACP ----------------

Bytes default_nacp() {
    Bytes n(kNacpSize, 0);
    std::memset(n.data() + nacp_off::kRatingAge, 0xFF, 0x20);
    return n;
}

std::optional<Bytes> nacp_from_nro(IFileSystem& fs, const std::string& path) {
    try {
        if (!fs.is_file(path)) return std::nullopt;
        const Bytes head = fs.read_range(path, 0, 0x20);
        if (head.size() < 0x20 || std::memcmp(head.data() + 0x10, "NRO0", 4) != 0) return std::nullopt;
        const uint32_t size = get32(head.data() + 0x18);
        if (!size) return std::nullopt;
        const Bytes aset = fs.read_range(path, size, 0x38);
        if (aset.size() < 0x38 || std::memcmp(aset.data(), "ASET", 4) != 0) return std::nullopt;
        const uint64_t off = get64(aset.data() + 0x18), len = get64(aset.data() + 0x20);
        if (len < kNacpSize) return std::nullopt;
        Bytes nacp = fs.read_range(path, size + off, kNacpSize);
        if (nacp.size() != kNacpSize) return std::nullopt;
        return nacp;
    } catch (const Error&) {
        return std::nullopt;
    }
}

namespace {
void put_text(uint8_t* field, size_t size, const std::string& s) {
    std::memset(field, 0, size);
    std::memcpy(field, s.data(), s.size() < size - 1 ? s.size() : size - 1);
}
}  // namespace

void patch_nacp(Bytes& nacp, const NacpPatch& p) {
    using namespace nacp_off;
    if (nacp.size() != kNacpSize) bad("NACP_BAD", "control.nacp: размер не 0x4000");
    uint8_t* n = nacp.data();
    for (size_t i = 0; i < 16; ++i) {
        if (!p.name.empty()) put_text(n + i * kLangEntry, kName, p.name);
        if (!p.author.empty()) put_text(n + i * kLangEntry + kName, kAuthor, p.author);
    }
    if (!p.version.empty()) put_text(n + kDisplayVersion, 0x10, p.version);
    put32(n + kSupportedLanguageFlag, 1u << 0);   // AmericanEnglish — для него и есть иконка
    n[kTitlesDataFormat] = 0;                     // таблица названий несжатая
    n[kStartupUserAccount] = 0;                   // профиль не спрашивается
    n[kUserAccountSwitchLock] = 0;
    n[kAocRegistrationType] = 1;                  // on demand
    n[kScreenshot] = 0;                           // разрешены
    n[kVideoCapture] = 2;                         // автоматически
    n[kLogoType] = 2;
    n[kLogoHandling] = 0;
    n[kDataLossConfirmation] = 0;
    n[kRequiredNetworkLicense] = 0;
    std::memset(n + kErrorCodeCategory, 0, 8);
    std::memcpy(n + kErrorCodeCategory, p.error_code_category.data(),
                p.error_code_category.size() < 8 ? p.error_code_category.size() : 8);
    put64(n + kPresenceGroupId, p.tid);
    put64(n + kSaveDataOwnerId, p.tid);
    put64(n + kPseudoDeviceIdSeed, p.tid);
    put64(n + kAocBaseId, p.tid ^ 0x1000);
    for (size_t i = 0; i < 8; ++i) put64(n + kLocalCommunicationId + i * 8, p.tid);
    n[kPlayLogPolicy] = 0;
    n[kPlayLogQueryCapability] = 0;
    for (size_t off : {kUserAccountSaveDataSize, kUserAccountSaveDataJournalSize, kDeviceSaveDataSize,
                       kDeviceSaveDataJournalSize, kUserAccountSaveDataSizeMax, kUserAccountSaveDataJournalSizeMax,
                       kDeviceSaveDataSizeMax, kDeviceSaveDataJournalSizeMax})
        put64(n + off, 0);
}

// ---------------- сборка ----------------

std::string cnmt_name(uint64_t tid) {
    static const char* hexd = "0123456789ABCDEF";
    std::string s = "Application_";
    for (int i = 15; i >= 0; --i) s.push_back(hexd[(tid >> (i * 4)) & 0xF]);
    return s + ".cnmt";
}

Output build(const Input& in, IHash& h, const HeaderCryptFn& crypt) {
    if (!in.tid || in.hbl_main.empty() || in.npdm.empty() || in.icon.empty() || in.icon.size() > kMaxIcon ||
        in.nacp.size() != kNacpSize || in.nro_path.empty() || in.argv.empty() || !crypt)
        bad("FWD_BAD_INPUT", "форвардер: неполные входные данные");
    Output o;

    // Program: exefs (hbl) + romfs (что запустить).
    {
        const Bytes argv(in.argv.begin(), in.argv.end()), path(in.nro_path.begin(), in.nro_path.end());
        Bytes hdr(kNcaHeader, 0);
        Buf out;
        out.zeros(kNcaHeader);
        nca_pfs0(h, hdr, 0, {{"main", &in.hbl_main}, {"main.npdm", &in.npdm}}, kExefsHashBlock, out);
        nca_romfs(h, hdr, 1, {{"/nextArgv", &argv}, {"/nextNroPath", &path}}, out);
        o.program = nca_finish(h, hdr, in.tid, kNcaProgram, kNcmProgram, out, crypt);
    }
    // Control: имя и иконка.
    {
        Bytes hdr(kNcaHeader, 0);
        Buf out;
        out.zeros(kNcaHeader);
        nca_romfs(h, hdr, 0, {{"/control.nacp", &in.nacp}, {"/icon_AmericanEnglish.dat", &in.icon}}, out);
        o.control = nca_finish(h, hdr, in.tid, kNcaControl, kNcmControl, out, crypt);
    }
    // Meta: cnmt (заголовок 0x20, расширенный 0x10, две записи 0x38, дайджест 0x20).
    {
        Bytes& c = o.cnmt;
        c.assign(0x20 + 0x10 + 2 * 0x38 + 0x20, 0);
        put64(c.data() + 0x00, in.tid);
        put32(c.data() + 0x08, 0);                     // title_version
        c[0x0C] = kMetaTypeApplication;
        put16(c.data() + 0x0E, 0x10);                  // extended_header_size
        put16(c.data() + 0x10, 2);                     // content_count: program + control
        put16(c.data() + 0x12, 1);                     // content_meta_count
        c[0x14] = 0;                                   // attributes
        c[0x15] = in.storage_id;
        c[0x16] = kInstallTypeFull;
        put64(c.data() + 0x20, in.tid | 0x800);        // patch_id
        const Nca* two[2] = {&o.program, &o.control};
        for (int i = 0; i < 2; ++i) {
            uint8_t* e = c.data() + 0x30 + i * 0x38;
            std::memcpy(e, two[i]->hash.data(), 0x20);
            content_info(e + 0x20, two[i]->hash, two[i]->data.size(), two[i]->ncm_type);
        }
        Bytes hdr(kNcaHeader, 0);
        Buf out;
        out.zeros(kNcaHeader);
        nca_pfs0(h, hdr, 0, {{cnmt_name(in.tid), &c}}, kMetaHashBlock, out);
        o.meta = nca_finish(h, hdr, in.tid, kNcaMeta, kNcmMeta, out, crypt);
    }
    // Запись базы ncm: заголовок (3 содержимых, хранилище 0), расширенный, meta + program + control.
    // Размер записи meta — размер cnmt, а не NCA: так пишут sphaira и forwarder.c (проверено на консоли).
    {
        Bytes& r = o.meta_db_record;
        r.assign(8 + 0x10 + 3 * 0x18, 0);
        put16(r.data() + 0, 0x10);
        put16(r.data() + 2, 3);
        put16(r.data() + 4, 1);
        put64(r.data() + 8, in.tid | 0x800);
        content_info(r.data() + 0x18, o.meta.hash, o.cnmt.size(), kNcmMeta);
        content_info(r.data() + 0x30, o.program.hash, o.program.data.size(), kNcmProgram);
        content_info(r.data() + 0x48, o.control.hash, o.control.data.size(), kNcmControl);
    }
    return o;
}

}  // namespace mk::forwarder
