// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — exefs из сырого NCA через службы консоли (ncm, spl, es). См. platform/exefs_raw.hpp.
#include "platform/exefs_raw.hpp"

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <cstring>
#include <switch.h>
#include <sys/iosupport.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>

#include <mk/core.hpp>
#include <mk/nca.hpp>

#include "platform/core_fs.hpp"
#include "platform/log.hpp"
#include "platform/service_lock.hpp"

namespace mk::plat {

namespace {

// Источники KAEK (application, ocean, system) — общеизвестные константы из .rodata FS (как в nxdumptool
// include/core/key_sources.h, Awoo). Это не ключи: KAEK из них выводит spl внутри консоли и наружу не отдаёт.
constexpr uint8_t kKaekSource[3][0x10] = {
    {0x7F, 0x59, 0x97, 0x1E, 0x62, 0x9F, 0x36, 0xA1, 0x30, 0x98, 0x06, 0x6F, 0x21, 0x44, 0xC3, 0x0D},
    {0x32, 0x7D, 0x36, 0x08, 0x5A, 0xD1, 0x75, 0x8D, 0xAB, 0x4E, 0x6F, 0xBA, 0xA5, 0x55, 0xD8, 0x82},
    {0x87, 0x45, 0xF1, 0xBB, 0xA6, 0xBE, 0x79, 0x64, 0x7D, 0x04, 0x8B, 0xA6, 0x7B, 0x5F, 0xDA, 0x4A}};

constexpr size_t kSplChunk = 0x10000;   // буфер spl ComputeCtr: 64 КБ, выровнен на 64 КБ (не пересекает 4 МБ)
constexpr size_t kNcmChunk = 0x40000;   // ncm ReadContentIdFile за раз

JournalFn g_jlog = nullptr;

void jl(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void jl(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    MK_LOG("exefs-raw: %s", buf);
    if (g_jlog) g_jlog(buf);
}

// ---------------- spl:es: слот и CTR (раскладка аргументов — Atmosphère sf: сортировка по выравниванию,
// у ключевых типов spl выравнивание 1, поэтому 16 байт ключа/счётчика идут первыми, s32 — за ними).
Result esAllocateSlot(Service* s, s32* out) { return serviceDispatchOut(s, 21, *out); }
Result esDeallocateSlot(Service* s, s32 slot) { return serviceDispatchIn(s, 22, slot); }
Result esLoadPreparedAesKey(Service* s, s32 slot, const uint8_t accessKey[16]) {
    struct {
        uint8_t key[16];
        s32 slot;
    } in{};
    std::memcpy(in.key, accessKey, 16);
    in.slot = slot;
    return serviceDispatchIn(s, 32, in);
}
Result esComputeCtr(Service* s, s32 slot, const uint8_t ctr[16], const void* src, void* dst, size_t n) {
    struct {
        uint8_t ctr[16];
        s32 slot;
    } in{};
    std::memcpy(in.ctr, ctr, 16);
    in.slot = slot;
    return serviceDispatchIn(s, 15, in,
                             .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out |
                                                  SfBufferAttr_HipcMapTransferAllowsNonSecure,
                                              SfBufferAttr_HipcMapAlias | SfBufferAttr_In |
                                                  SfBufferAttr_HipcMapTransferAllowsNonSecure},
                             .buffers = {{dst, n}, {src, n}});
}

// ---------------- es: общий билет по rights id (команды 14, 16 IETicketService)
Result esTicketSize(Service* s, const uint8_t rid[16], u64* out) {
    struct {
        uint8_t rid[16];
    } in{};
    std::memcpy(in.rid, rid, 16);
    return serviceDispatchInOut(s, 14, in, *out);
}
Result esTicketData(Service* s, const uint8_t rid[16], void* buf, size_t size, u64* out) {
    struct {
        uint8_t rid[16];
    } in{};
    std::memcpy(in.rid, rid, 16);
    return serviceDispatchInOut(s, 16, in, *out, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                .buffers = {{buf, size}});
}

void ctrAdd(uint8_t c[16], uint64_t blocks) {
    for (int i = 15; i >= 0 && blocks; --i) {
        const uint64_t v = uint64_t(c[i]) + (blocks & 0xFF);
        c[i] = static_cast<uint8_t>(v);
        blocks = (blocks >> 8) + (v >> 8);
    }
}

// ---------------- состояние открытой exefs (одна на процесс, как и устройство)
struct State {
    std::mutex mtx;
    bool mounted = false;
    std::string device;
    NcmContentStorage cs{};
    bool csOpen = false;
    NcmContentId cid{};
    mk::nca::Header hdr;
    // ключ: либо открытый ключ секции (без rights id), либо слот spl:es (rights id)
    bool softKey = false;
    std::array<uint8_t, 16> key{};
    bool esOpen = false;
    bool slotAllocated = false;
    s32 slot = -1;
    uint8_t* bufIn = nullptr;
    uint8_t* bufOut = nullptr;
    std::unique_ptr<mk::nca::SectionReader> reader;
    std::unique_ptr<mk::nca::ExefsReader> exefs;
    uint32_t lastIoRc = 0;
};
State g;

void releaseLocked() {
    g.exefs.reset();
    g.reader.reset();
    if (g.slotAllocated) {
        ServiceLock lock(serviceMutex());
        const Result rc = esDeallocateSlot(splEsGetServiceSession(), g.slot);
        if (R_FAILED(rc)) MK_LOG("exefs-raw: DeallocateAesKeySlot rc=0x%X", rc);
        g.slotAllocated = false;
    }
    if (g.esOpen) {
        ServiceLock lock(serviceMutex());
        splEsExit();
        g.esOpen = false;
    }
    if (g.csOpen) {
        ServiceLock lock(serviceMutex());
        ncmContentStorageClose(&g.cs);
        g.csOpen = false;
    }
    std::memset(g.key.data(), 0, g.key.size());
    g.softKey = false;
    if (g.bufIn) std::free(g.bufIn);
    if (g.bufOut) std::free(g.bufOut);
    g.bufIn = g.bufOut = nullptr;
}

void rawRead(uint64_t off, uint8_t* out, size_t n) {
    while (n > 0) {
        const size_t take = n < kNcmChunk ? n : kNcmChunk;
        Result rc;
        {
            ServiceLock lock(serviceMutex());
            rc = ncmContentStorageReadContentIdFile(&g.cs, out, take, &g.cid, static_cast<s64>(off));
        }
        if (R_FAILED(rc)) {
            g.lastIoRc = rc;
            char b[16];
            std::snprintf(b, sizeof(b), "0x%X", rc);
            throw mk::Error("NCA_IO", "ncm ReadContentIdFile отказал", {b});
        }
        out += take;
        off += take;
        n -= take;
    }
}

void softCtr(const uint8_t ctr[16], uint8_t* data, size_t n) {
    Aes128CtrContext c;
    aes128CtrContextCreate(&c, g.key.data(), ctr);
    aes128CtrCrypt(&c, data, data, n);
    std::memset(&c, 0, sizeof(c));
}

void slotCtr(const uint8_t ctr[16], uint8_t* data, size_t n) {
    uint8_t c[16];
    std::memcpy(c, ctr, 16);
    while (n > 0) {
        const size_t take = n < kSplChunk ? n : kSplChunk;   // n кратно 16 (SectionReader)
        std::memcpy(g.bufIn, data, take);
        Result rc;
        {
            ServiceLock lock(serviceMutex());
            rc = esComputeCtr(splEsGetServiceSession(), g.slot, c, g.bufIn, g.bufOut, take);
        }
        if (R_FAILED(rc)) {
            g.lastIoRc = rc;
            char b[16];
            std::snprintf(b, sizeof(b), "0x%X", rc);
            throw mk::Error("NCA_CTR", "spl ComputeCtr отказал", {b});
        }
        std::memcpy(data, g.bufOut, take);
        ctrAdd(c, take / 16);
        data += take;
        n -= take;
    }
}

// ---------------- devoptab «mkexefs:» (только чтение, один уровень — файлы PFS0)
struct FileH {
    int index;
    uint64_t pos;
};
struct DirH {
    size_t next;
};

const mk::nca::Pfs0Entry* entryOf(const char* path, bool& isRoot) {
    const char* p = std::strchr(path, ':');
    p = p ? p + 1 : path;
    while (*p == '/') ++p;
    isRoot = (*p == 0);
    if (isRoot || !g.exefs) return nullptr;
    return g.exefs->pfs0().find(p);
}

void fillStat(struct stat* st, const mk::nca::Pfs0Entry* e) {
    std::memset(st, 0, sizeof(*st));
    if (e) {
        st->st_mode = S_IFREG | 0444;
        st->st_size = static_cast<off_t>(e->size);
        st->st_nlink = 1;
    } else {
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 1;
    }
}

int dOpen(struct _reent* r, void* fs, const char* path, int flags, int) {
    std::lock_guard<std::mutex> lk(g.mtx);
    if ((flags & O_ACCMODE) != O_RDONLY) {
        r->_errno = EROFS;
        return -1;
    }
    bool root = false;
    const auto* e = entryOf(path, root);
    if (!e) {
        r->_errno = root ? EISDIR : ENOENT;
        return -1;
    }
    auto* f = static_cast<FileH*>(fs);
    f->index = static_cast<int>(e - g.exefs->pfs0().entries.data());
    f->pos = 0;
    return 0;
}

int dClose(struct _reent*, void*) { return 0; }

ssize_t dRead(struct _reent* r, void* fd, char* ptr, size_t len) {
    std::lock_guard<std::mutex> lk(g.mtx);
    auto* f = static_cast<FileH*>(fd);
    if (!g.exefs) {
        r->_errno = EBADF;
        return -1;
    }
    const auto& e = g.exefs->pfs0().entries.at(static_cast<size_t>(f->index));
    if (f->pos >= e.size) return 0;
    const size_t n = static_cast<size_t>(std::min<uint64_t>(len, e.size - f->pos));
    try {
        g.exefs->read(e, f->pos, reinterpret_cast<uint8_t*>(ptr), n);
    } catch (const mk::Error& ex) {
        MK_LOG("exefs-raw: read %s @0x%llX+%zu: %s %s", e.name.c_str(), static_cast<unsigned long long>(f->pos), n,
               ex.code().c_str(), ex.what());
        r->_errno = EIO;
        return -1;
    }
    f->pos += n;
    return static_cast<ssize_t>(n);
}

off_t dSeek(struct _reent* r, void* fd, off_t pos, int dir) {
    std::lock_guard<std::mutex> lk(g.mtx);
    auto* f = static_cast<FileH*>(fd);
    if (!g.exefs) {
        r->_errno = EBADF;
        return -1;
    }
    const auto& e = g.exefs->pfs0().entries.at(static_cast<size_t>(f->index));
    int64_t base = 0;
    if (dir == SEEK_CUR) base = static_cast<int64_t>(f->pos);
    else if (dir == SEEK_END) base = static_cast<int64_t>(e.size);
    else if (dir != SEEK_SET) {
        r->_errno = EINVAL;
        return -1;
    }
    const int64_t np = base + pos;
    if (np < 0) {
        r->_errno = EINVAL;
        return -1;
    }
    f->pos = static_cast<uint64_t>(np);
    return static_cast<off_t>(np);
}

int dFstat(struct _reent* r, void* fd, struct stat* st) {
    std::lock_guard<std::mutex> lk(g.mtx);
    if (!g.exefs) {
        r->_errno = EBADF;
        return -1;
    }
    auto* f = static_cast<FileH*>(fd);
    fillStat(st, &g.exefs->pfs0().entries.at(static_cast<size_t>(f->index)));
    return 0;
}

int dStat(struct _reent* r, const char* path, struct stat* st) {
    std::lock_guard<std::mutex> lk(g.mtx);
    bool root = false;
    const auto* e = entryOf(path, root);
    if (!e && !root) {
        r->_errno = ENOENT;
        return -1;
    }
    fillStat(st, e);
    return 0;
}

DIR_ITER* dDirOpen(struct _reent* r, DIR_ITER* it, const char* path) {
    std::lock_guard<std::mutex> lk(g.mtx);
    bool root = false;
    entryOf(path, root);
    if (!root || !g.exefs) {
        r->_errno = ENOTDIR;
        return nullptr;
    }
    static_cast<DirH*>(it->dirStruct)->next = 0;
    return it;
}

int dDirReset(struct _reent*, DIR_ITER* it) {
    static_cast<DirH*>(it->dirStruct)->next = 0;
    return 0;
}

int dDirNext(struct _reent* r, DIR_ITER* it, char* name, struct stat* st) {
    std::lock_guard<std::mutex> lk(g.mtx);
    auto* d = static_cast<DirH*>(it->dirStruct);
    if (!g.exefs || d->next >= g.exefs->pfs0().entries.size()) {
        r->_errno = ENOENT;
        return -1;
    }
    const auto& e = g.exefs->pfs0().entries[d->next++];
    std::snprintf(name, NAME_MAX, "%s", e.name.c_str());
    if (st) fillStat(st, &e);
    return 0;
}

int dDirClose(struct _reent*, DIR_ITER*) { return 0; }

devoptab_t g_devoptab;

bool fail(RawExefsOutcome& out, const char* step, uint32_t rc, const std::string& core = {}) {
    out.ok = false;
    out.step = step;
    out.rc = rc;
    out.coreCode = core;
    jl("FAIL step=%s rc=0x%X core=%s", step, rc, core.empty() ? "-" : core.c_str());
    releaseLocked();
    return false;
}

// Распознать exefs: заголовок PFS0. Ошибка ядра — в out (pfs0), без освобождения ресурсов (решает вызывающий).
bool tryPfs0(RawExefsOutcome& out, const mk::nca::FsSection& sec, mk::nca::CtrFn fn, const char* what) {
    g.reader = std::make_unique<mk::nca::SectionReader>(rawRead, std::move(fn), sec);
    try {
        g.exefs = std::make_unique<mk::nca::ExefsReader>(*g.reader);
    } catch (const mk::Error& e) {
        jl("pfs0 (%s): %s %s (io rc=0x%X)", what, e.code().c_str(), e.what(), g.lastIoRc);
        out.coreCode = e.code();
        out.rc = g.lastIoRc;
        g.exefs.reset();
        g.reader.reset();
        return false;
    }
    std::string names;
    for (const auto& e : g.exefs->pfs0().entries) {
        if (!names.empty()) names += ' ';
        names += e.name;
    }
    jl("pfs0 ok (%s): %zu files: %s", what, g.exefs->pfs0().entries.size(), names.c_str());
    return true;
}

}  // namespace

bool rawExefsMount(const ProgramNca& nca, const char* deviceName, RawExefsOutcome& out, JournalFn jlog) {
    std::lock_guard<std::mutex> lk(g.mtx);
    out = RawExefsOutcome{};
    g_jlog = jlog;
    if (g.mounted) {   // чужую открытую exefs не трогаем (ExefsMount и так не пускает вторую)
        jl("already mounted");
        out.step = "mount";
        out.coreCode = "BUSY";
        return false;
    }
    releaseLocked();
    jl("start: storage=%d from=%s v%u", static_cast<int>(nca.storage), nca.fromPatch ? "patch" : "base",
       nca.key.version);

    // 1. header
    std::array<uint8_t, 0xC00> dec{};
    uint32_t hrc = 0;
    if (!readProgramNcaHeaderDecrypted(nca, dec, hrc)) return fail(out, "header", hrc);
    mk::nca::Header h;
    try {
        h = mk::nca::parse_header(dec.data(), dec.size(), nxHash());
    } catch (const mk::Error& e) {
        jl("parse: %s %s", e.code().c_str(), e.what());
        return fail(out, "parse", 0, e.code());
    }
    out.rightsId = h.has_rights_id;
    out.keyGeneration = mk::nca::effective_key_generation(h);
    jl("header ok: type=%u keygen=%u (old %u, new %u) kaek=%u rights=%d size=0x%llX program=%016llX",
       h.content_type, out.keyGeneration, h.key_generation_old, h.key_generation, h.kaek_index, h.has_rights_id,
       static_cast<unsigned long long>(h.content_size), static_cast<unsigned long long>(h.program_id));
    for (const auto& s : h.sections) {
        if (!s.present) continue;
        jl("section %d: off=0x%llX size=0x%llX fs=%u hash=%u enc=%u pfs0=0x%llX+0x%llX sparse=%d compr=%d", s.index,
           static_cast<unsigned long long>(s.offset), static_cast<unsigned long long>(s.size), s.fs_type, s.hash_type,
           s.encryption_type, static_cast<unsigned long long>(s.pfs0_offset),
           static_cast<unsigned long long>(s.pfs0_size), s.has_sparse, s.has_compression);
    }
    const mk::nca::FsSection* sec = nullptr;
    try {
        sec = &mk::nca::exefs_section(h);
    } catch (const mk::Error& e) {
        jl("parse: %s %s", e.code().c_str(), e.what());
        return fail(out, "parse", 0, e.code());
    }
    g.hdr = h;
    const mk::nca::FsSection secCopy = *sec;

    // ncm: хранилище держим открытым, пока смонтировано
    {
        ServiceLock lock(serviceMutex());
        const Result rc = ncmOpenContentStorage(&g.cs, nca.storage);
        if (R_FAILED(rc)) return fail(out, "header", rc);
        g.csOpen = true;
        g.cid = nca.contentId;
    }

    if (!h.has_rights_id) {
        // 2a. key area через spl (ключ — только в памяти процесса)
        if (h.kaek_index > 2) return fail(out, "key-area", 0, "NCA_KAEK_INDEX");
        Result rc;
        {
            ServiceLock lock(serviceMutex());
            rc = splCryptoInitialize();
            if (R_SUCCEEDED(rc)) {
                uint8_t sealedKek[0x10] = {};
                rc = splCryptoGenerateAesKek(kKaekSource[h.kaek_index], out.keyGeneration, 0, sealedKek);
                if (R_FAILED(rc)) jl("key-area: GenerateAesKek(kaek=%u, gen=%u) rc=0x%X", h.kaek_index, out.keyGeneration, rc);
                if (R_SUCCEEDED(rc)) {
                    rc = splCryptoGenerateAesKey(sealedKek, h.key_area[mk::nca::kKeyAreaCtr].data(), g.key.data());
                    if (R_FAILED(rc)) jl("key-area: GenerateAesKey rc=0x%X", rc);
                }
                std::memset(sealedKek, 0, sizeof(sealedKek));
                splCryptoExit();
            } else {
                jl("key-area: splCryptoInitialize rc=0x%X", rc);
            }
        }
        if (R_FAILED(rc)) return fail(out, "key-area", rc);
        g.softKey = true;
        jl("key-area ok (spl, gen=%u)", out.keyGeneration);
        if (!tryPfs0(out, secCopy, softCtr, "key-area")) return fail(out, "pfs0", out.rc, out.coreCode);
    } else {
        // 2b. общий билет из es
        jl("rights id %s", mk::nca::rights_id_hex(h.rights_id).c_str());
        mk::nca::Ticket tk;
        {
            ServiceLock lock(serviceMutex());
            Service es{};
            Result rc = smGetService(&es, "es");
            if (R_FAILED(rc)) {
                jl("ticket: smGetService(es) rc=0x%X", rc);
                return fail(out, "ticket", rc);
            }
            u64 size = 0;
            rc = esTicketSize(&es, h.rights_id.data(), &size);
            jl("ticket: GetCommonTicketSize rc=0x%X size=%llu", rc, static_cast<unsigned long long>(size));
            if (R_FAILED(rc) || size == 0 || size > 0x10000) {
                serviceClose(&es);
                out.personalized = R_FAILED(rc);
                return fail(out, "ticket", R_FAILED(rc) ? rc : 0, R_FAILED(rc) ? "" : "TICKET_SIZE");
            }
            mk::Bytes buf(static_cast<size_t>(size));
            u64 got = 0;
            rc = esTicketData(&es, h.rights_id.data(), buf.data(), buf.size(), &got);
            serviceClose(&es);
            jl("ticket: GetCommonTicketData rc=0x%X got=%llu", rc, static_cast<unsigned long long>(got));
            if (R_FAILED(rc)) return fail(out, "ticket", rc);
            try {
                tk = mk::nca::parse_ticket(buf.data(), static_cast<size_t>(got && got <= size ? got : size));
            } catch (const mk::Error& e) {
                jl("ticket: %s %s", e.code().c_str(), e.what());
                return fail(out, "ticket", 0, e.code());
            }
            std::memset(buf.data(), 0, buf.size());
        }
        jl("ticket ok: sig=0x%X type=%u keygen=%u rights=%s", tk.sig_type, tk.titlekey_type, tk.key_generation,
           tk.rights_id == h.rights_id ? "match" : "MISMATCH");
        if (tk.titlekey_type != 0) {
            out.personalized = true;
            return fail(out, "ticket", 0, "TICKET_PERSONALIZED");
        }
        // 3. титульный ключ в слот spl:es
        g.bufIn = static_cast<uint8_t*>(memalign(kSplChunk, kSplChunk));
        g.bufOut = static_cast<uint8_t*>(memalign(kSplChunk, kSplChunk));
        if (!g.bufIn || !g.bufOut) return fail(out, "titlekey", 0, "NO_MEMORY");
        {
            ServiceLock lock(serviceMutex());
            Result rc = splEsInitialize();
            if (R_FAILED(rc)) {
                jl("titlekey: splEsInitialize rc=0x%X", rc);
                return fail(out, "titlekey", rc);
            }
            g.esOpen = true;
            rc = esAllocateSlot(splEsGetServiceSession(), &g.slot);
            jl("titlekey: AllocateAesKeySlot rc=0x%X slot=%d", rc, g.slot);
            if (R_FAILED(rc)) return fail(out, "titlekey", rc);
            g.slotAllocated = true;
        }
        // поколение: из билета; если ключ не подошёл (нет PFS0) — пробуем поколение NCA
        uint8_t gens[2] = {tk.key_generation, out.keyGeneration};
        const int ngen = (gens[0] == gens[1]) ? 1 : 2;
        bool ok = false;
        for (int i = 0; i < ngen && !ok; ++i) {
            Result rc;
            {
                ServiceLock lock(serviceMutex());
                uint8_t access[16] = {};
                rc = splEsUnwrapAesWrappedTitlekey(tk.enc_titlekey.data(), gens[i], access);
                if (R_FAILED(rc)) jl("titlekey: PrepareCommonEsTitleKey(gen=%u) rc=0x%X", gens[i], rc);
                if (R_SUCCEEDED(rc)) {
                    rc = esLoadPreparedAesKey(splEsGetServiceSession(), g.slot, access);
                    if (R_FAILED(rc)) jl("titlekey: LoadPreparedAesKey rc=0x%X", rc);
                }
                std::memset(access, 0, sizeof(access));
            }
            if (R_FAILED(rc)) {
                out.rc = rc;
                continue;
            }
            jl("titlekey loaded into slot (gen=%u)", gens[i]);
            char what[24];
            std::snprintf(what, sizeof(what), "titlekey gen %u", gens[i]);
            ok = tryPfs0(out, secCopy, slotCtr, what);
        }
        std::memset(&tk, 0, sizeof(tk));
        if (!ok) return fail(out, out.coreCode.empty() ? "titlekey" : "pfs0", out.rc, out.coreCode);
    }

    // 4. устройство
    g.device = deviceName;
    std::memset(&g_devoptab, 0, sizeof(g_devoptab));
    g_devoptab.name = g.device.c_str();
    g_devoptab.structSize = sizeof(FileH);
    g_devoptab.open_r = dOpen;
    g_devoptab.close_r = dClose;
    g_devoptab.read_r = dRead;
    g_devoptab.seek_r = dSeek;
    g_devoptab.fstat_r = dFstat;
    g_devoptab.stat_r = dStat;
    g_devoptab.dirStateSize = sizeof(DirH);
    g_devoptab.diropen_r = dDirOpen;
    g_devoptab.dirreset_r = dDirReset;
    g_devoptab.dirnext_r = dDirNext;
    g_devoptab.dirclose_r = dDirClose;
    if (AddDevice(&g_devoptab) < 0) return fail(out, "mount", 0, "ADD_DEVICE");
    g.mounted = true;
    out.ok = true;
    out.step.clear();
    jl("mounted %s: (via %s)", deviceName, h.has_rights_id ? "titlekey slot" : "key area");
    return true;
}

void rawExefsUnmount() {
    std::lock_guard<std::mutex> lk(g.mtx);
    if (g.mounted) {
        const std::string dev = g.device + ":";
        RemoveDevice(dev.c_str());
        g.mounted = false;
    }
    releaseLocked();
    g_jlog = nullptr;
}

}  // namespace mk::plat
