// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — обновление приложения: сеть и консоль (только Switch-сборка). См. platform/app_update.hpp.
//
// Сеть — libcurl из портлибов devkitPro (switch-curl). Его TLS — системная служба ssl консоли (бэкенд libnx):
// сертификат сервера проверяется по встроенному в прошивку хранилищу корневых сертификатов, отдельный
// cacert.pem не нужен. Проверка включена явно (VERIFYPEER=1, VERIFYHOST=2). Частая причина отказа TLS на
// консоли — сбитые часы: в журнал пишется подсказка.
//
// Сборка: нужен libcurl (pkg-config libcurl) и определение MK_HAVE_CURL=1. Без него код собирается, а проверка
// обновлений тихо отвечает «нет сети в этой сборке» (в журнал) — приложение работает как раньше.
#ifdef __SWITCH__

#include <switch.h>

#include <atomic>
#include <chrono>
#include <thread>

#if defined(MK_HAVE_CURL)
#include <curl/curl.h>
#endif

#include "platform/app_update.hpp"
#include "platform/core_fs.hpp"
#include "platform/log.hpp"
#include "platform/paths.hpp"

namespace mk::plat::update {

namespace {

std::string g_self;
std::atomic<bool> g_abort{false};
std::atomic<int> g_busy{0};

constexpr size_t kMaxJson = 1u << 20;          // ответ API — килобайты; больше — что-то не то
constexpr size_t kMaxNro = 96u * 1024 * 1024;  // .nro приложения — единицы МБ; потолок от бесконечного потока

#if defined(MK_HAVE_CURL)
struct Sink {
    std::string* text = nullptr;
    std::vector<uint8_t>* bin = nullptr;
    size_t limit = 0;
};

size_t onWrite(char* p, size_t sz, size_t n, void* ud) {
    auto* s = static_cast<Sink*>(ud);
    const size_t len = sz * n;
    const size_t have = s->text ? s->text->size() : s->bin->size();
    if (have + len > s->limit) return 0;   // CURLE_WRITE_ERROR
    if (s->text) s->text->append(p, len);
    else s->bin->insert(s->bin->end(), reinterpret_cast<uint8_t*>(p), reinterpret_cast<uint8_t*>(p) + len);
    return len;
}

struct Progress {
    const std::function<bool(uint64_t, uint64_t)>* cb = nullptr;
};

int onProgress(void* ud, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    if (g_abort) return 1;
    auto* pr = static_cast<Progress*>(ud);
    if (pr && pr->cb && *pr->cb && !(*pr->cb)(static_cast<uint64_t>(now), static_cast<uint64_t>(total))) return 1;
    return 0;
}

// Один GET: TLS с проверкой сертификата, переходы по 302 (ассет отдаётся с objects.githubusercontent.com).
bool get(const std::string& url, Sink& sink, const std::function<bool(uint64_t, uint64_t)>* progress, bool api,
         std::string& error) {
    CURL* c = curl_easy_init();
    if (!c) {
        error = "curl init";
        return false;
    }
    char errbuf[CURL_ERROR_SIZE] = {0};
    struct curl_slist* hdr = nullptr;
    if (api) {
        hdr = curl_slist_append(hdr, "Accept: application/vnd.github+json");
        hdr = curl_slist_append(hdr, "X-GitHub-Api-Version: 2022-11-28");
    }
    Progress pr{progress};
    const std::string ua = std::string(MK_APP_NAME) + "/" + MK_APP_VERSION;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, ua.c_str());   // GitHub API без User-Agent отвечает 403
    if (hdr) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, api ? 20L : 600L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_PROXY, "");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &pr);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    const CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    if (hdr) curl_slist_free_all(hdr);
    if (rc != CURLE_OK) {
        error = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT)
            error += " (certificate check failed: is the console clock right?)";
        return false;
    }
    // API отдаёт тело и при 403/404 ({"message": …}) — его разберёт parseRelease; файл — только 200.
    if (!api && status != 200) {
        error = "http " + std::to_string(status);
        return false;
    }
    if (api && status != 200 && status != 403 && status != 404) {
        error = "http " + std::to_string(status);
        return false;
    }
    return true;
}
#endif

struct Busy {
    Busy() { busyBegin(); }
    ~Busy() { busyEnd(); }
};

}  // namespace

void init(int argc, char** argv) {
    g_self = (argc > 0 && argv) ? selfNroPath(argv[0]) : "";
    MK_LOG("update: running from %s", g_self.empty() ? "(unknown: updating disabled)" : g_self.c_str());
    // Успешный старт: .old прошлой замены больше не нужен (своего .nro нет — ничего не трогаем).
    const std::string c = cleanupAfterStart(g_self);
    if (!c.empty()) MK_LOG("update: after start: %s", c.c_str());
}

const std::string& selfPath() { return g_self; }

bool networkUp() {
    NifmInternetConnectionStatus st{};
    NifmInternetConnectionType type{};
    u32 strength = 0;
    const Result rc = nifmGetInternetConnectionStatus(&type, &strength, &st);
    return R_SUCCEEDED(rc) && st == NifmInternetConnectionStatus_Connected;
}

bool fetchLatest(Release& out, std::string& error) {
#if defined(MK_HAVE_CURL)
    Busy b;
    std::string text;
    Sink s;
    s.text = &text;
    s.limit = kMaxJson;
    if (!get(latestUrl(), s, nullptr, true, error)) return false;
    out = parseRelease(text, assetName());
    if (!out.ok) {
        error = out.error;
        return false;
    }
    return true;
#else
    (void)out;
    error = "this build has no network (MK_HAVE_CURL)";
    return false;
#endif
}

bool download(const Release& r, std::vector<uint8_t>& out, const std::function<bool(uint64_t, uint64_t)>& progress,
              std::string& error) {
#if defined(MK_HAVE_CURL)
    Busy b;
    out.clear();
    if (r.assetSize > 0 && r.assetSize <= kMaxNro) out.reserve(r.assetSize);
    Sink s;
    s.bin = &out;
    s.limit = kMaxNro;
    return get(r.assetUrl, s, &progress, false, error);
#else
    (void)r;
    (void)out;
    (void)progress;
    error = "this build has no network (MK_HAVE_CURL)";
    return false;
#endif
}

NroInfo verifyDownloaded(const std::vector<uint8_t>& data, const Release& r) {
    mk::plat::NxHash hash;
    return verifyNro(data.data(), data.size(), r, MK_APP_NAME, hash);
}

void applyPendingAtExit() {
    // .new не новее работающей версии — hasPending отодвигает его в .new.stale и отвечает «нет».
    if (g_self.empty() || !hasPending(g_self, MK_APP_VERSION)) return;
    // Работающий .nro открыт romfs (его смонтировал userAppInit borealis): отмонтировать, иначе rename не пройдёт.
    // После этой строки romfs не читается; userAppExit зовёт romfsExit ещё раз — безвредно.
    romfsExit();
    std::string err;
    if (swapInPending(g_self, MK_APP_VERSION, err)) MK_LOG("update: new version installed into %s", g_self.c_str());
    else MK_LOG("update: could not install the downloaded version: %s", err.c_str());
}

bool restartNow() {
    if (g_self.empty() || !envHasNextLoad()) {
        MK_LOG("update: loader cannot relaunch the app");
        return false;
    }
    // argv — строка, её режет argvSetup libnx по пробелам: путь с пробелом — в кавычках (restartArgv).
    const std::string args = restartArgv(g_self);
    envSetNextLoad(g_self.c_str(), args.c_str());
    return true;
}

void abortAll() { g_abort = true; }

bool waitIdle(int ms) {
    for (int t = 0; t < ms && g_busy.load() > 0; t += 20) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return g_busy.load() == 0;
}

void busyBegin() { ++g_busy; }
void busyEnd() { --g_busy; }
bool aborting() { return g_abort; }

}  // namespace mk::plat::update

#endif  // __SWITCH__
