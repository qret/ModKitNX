// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — хост-тесты обновления приложения и настроек (platform/src/app_update.cpp, app_config.cpp) на
// подставных ответах GitHub и собранных в памяти .nro. Сеть не нужна.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "mk/core.hpp"
#include "platform/app_config.hpp"
#include "platform/app_update.hpp"

namespace up = mk::plat::update;
namespace cfg = mk::plat::config;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, what)                                              \
    do {                                                               \
        if (cond) {                                                    \
            ++g_pass;                                                  \
        } else {                                                       \
            ++g_fail;                                                  \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, what); \
        }                                                              \
    } while (0)

namespace {

void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
void put64(std::vector<uint8_t>& b, size_t at, uint64_t v) {
    for (int i = 0; i < 8; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

// Минимальный .nro: заголовок NRO0 (образ 0x100), ASET, NACP с именем и версией.
std::vector<uint8_t> makeNro(const std::string& name, const std::string& version, size_t titleSlot = 0) {
    const size_t image = 0x100, aset = image, nacpOff = 0x38, nacp = aset + nacpOff;
    std::vector<uint8_t> b(nacp + 0x4000, 0);
    std::memcpy(&b[0x10], "NRO0", 4);
    put32(b, 0x18, image);
    std::memcpy(&b[aset], "ASET", 4);
    put64(b, aset + 0x18, nacpOff);
    put64(b, aset + 0x20, 0x4000);
    std::memcpy(&b[nacp + titleSlot * 0x300], name.data(), name.size());
    std::memcpy(&b[nacp + 0x3060], version.data(), version.size());
    return b;
}

std::string json(const std::string& tag, const std::string& assets, const std::string& body = "notes",
                 bool prerelease = false) {
    return "{\"tag_name\":\"" + tag + "\",\"name\":\"ModKitNX " + tag + "\",\"draft\":false,\"prerelease\":" +
           (prerelease ? "true" : "false") + ",\"body\":\"" + body + "\",\"assets\":[" + assets + "]}";
}

std::string asset(const std::string& name, uint64_t size, const std::string& digest = "") {
    std::string s = "{\"name\":\"" + name + "\",\"size\":" + std::to_string(size) +
                    ",\"browser_download_url\":\"https://github.com/qret/ModKitNX/releases/download/x/" + name + "\"";
    if (!digest.empty()) s += ",\"digest\":\"" + digest + "\"";
    return s + "}";
}

std::string sha(const std::vector<uint8_t>& b) { return mk::hex(mk::soft_hash().sha256(b.data(), b.size())); }

bool exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

std::string readText(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void writeBytes(const std::string& p, const std::vector<uint8_t>& b) {
    FILE* f = std::fopen(p.c_str(), "wb");
    std::fwrite(b.data(), 1, b.size(), f);
    std::fclose(f);
}

void testVersions() {
    CHECK(up::parseVersion("v1.2.3").valid && up::parseVersion("v1.2.3").patch == 3, "v1.2.3");
    CHECK(up::parseVersion("1.2").valid && up::parseVersion("1.2").patch == 0, "1.2 = 1.2.0");
    CHECK(up::parseVersion("1.2.3-beta").valid, "хвост -beta");
    CHECK(!up::parseVersion("latest").valid, "не версия");
    CHECK(!up::parseVersion("1").valid, "одно число — не версия");
    CHECK(!up::parseVersion("1.2.3x").valid, "мусор после версии");
    CHECK(up::isNewer("0.2.0", "0.1.0"), "0.2.0 > 0.1.0");
    // Схема версий: 1.0.0 → 1.0.1 … 1.0.9 → 1.1.0 → … → 1.1.9 → 1.2.0.
    CHECK(up::isNewer("1.0.1", "1.0.0"), "1.0.1 > 1.0.0");
    CHECK(up::isNewer("1.1.0", "1.0.9"), "1.1.0 > 1.0.9");
    CHECK(up::isNewer("1.2.0", "1.1.9"), "1.2.0 > 1.1.9");
    CHECK(up::isNewer("1.0.0", "0.1.0"), "1.0.0 > 0.1.0 (пробные сборки)");
    CHECK(up::isNewer("v0.1.10", "0.1.9"), "числа, а не строки: 0.1.10 > 0.1.9");
    CHECK(up::isNewer("1.0.0", "0.9.9"), "мажор");
    CHECK(!up::isNewer("0.1.0", "0.1.0"), "та же — не новее");
    CHECK(!up::isNewer("0.0.9", "0.1.0"), "старее — не новее");
    CHECK(!up::isNewer("garbage", "0.1.0"), "кривой тег не навязывает обновление");
    CHECK(up::sameVersion("v0.2", "0.2.0"), "v0.2 == 0.2.0");
}

void testParse() {
    const std::string hexA(64, 'a'), hexB(64, 'b');
    // Обычный релиз: ассет по имени, размер, sha256 из описания (как пишет tools/release.ps1).
    up::Release r = up::parseRelease(
        json("v0.2.0", asset("ModKitNX.zip", 5) + "," + asset("ModKitNX.nro", 1234),
             "Fixes\\n\\n---\\nsha256 (ModKitNX.nro): `" + hexA + "`\\n"),
        "ModKitNX.nro");
    CHECK(r.ok && r.version == "0.2.0" && r.tag == "v0.2.0", "разбор тега");
    CHECK(r.assetUrl.find("ModKitNX.nro") != std::string::npos && r.assetSize == 1234, "ассет .nro, не .zip");
    CHECK(r.sha256 == hexA, "sha256 из описания");

    // digest ассета (GitHub) важнее; совпадает с описанием — ок.
    r = up::parseRelease(json("v0.2.0", asset("ModKitNX.nro", 10, "sha256:" + hexA), "sha256: " + hexA), "ModKitNX.nro");
    CHECK(r.ok && r.sha256 == hexA && !r.assetUrl.empty(), "digest = описание");
    // Расходятся — ассет не предлагается.
    r = up::parseRelease(json("v0.2.0", asset("ModKitNX.nro", 10, "sha256:" + hexA), "sha256: " + hexB), "ModKitNX.nro");
    CHECK(r.ok && r.assetUrl.empty() && !r.error.empty(), "digest != описание — не ставить");

    // Нет ассета .nro — релиз есть, ставить нечего.
    r = up::parseRelease(json("v0.2.0", asset("ModKitNX.zip", 5)), "ModKitNX.nro");
    CHECK(r.ok && r.assetUrl.empty(), "нет .nro");
    // Единственный .nro с другим именем — берётся; два чужих — нет.
    r = up::parseRelease(json("v0.2.0", asset("modkitnx-0.2.0.nro", 7)), "ModKitNX.nro");
    CHECK(r.assetSize == 7, "единственный .nro");
    r = up::parseRelease(json("v0.2.0", asset("a.nro", 7) + "," + asset("b.nro", 8)), "ModKitNX.nro");
    CHECK(r.assetUrl.empty(), "два чужих .nro — не угадывать");

    // Ответы-ошибки GitHub.
    r = up::parseRelease("{\"message\":\"Not Found\",\"documentation_url\":\"x\"}", "ModKitNX.nro");
    CHECK(!r.ok && r.error.find("Not Found") != std::string::npos, "404: релизов нет");
    r = up::parseRelease("{\"message\":\"API rate limit exceeded for 1.2.3.4.\"}", "ModKitNX.nro");
    CHECK(!r.ok && r.error.find("rate limit") != std::string::npos, "403: лимит");
    r = up::parseRelease("<html>proxy</html>", "ModKitNX.nro");
    CHECK(!r.ok, "не JSON");
    r = up::parseRelease(json("nightly", asset("ModKitNX.nro", 1)), "ModKitNX.nro");
    CHECK(!r.ok, "тег не версия");
}

void testDecide() {
    const std::string hexA(64, 'a'), hexB(64, 'b');
    up::Release r = up::parseRelease(json("v0.2.0", asset("ModKitNX.nro", 1, "sha256:" + hexA)), "ModKitNX.nro");
    up::Decision d = up::decide(r, "0.1.0", "");
    CHECK(d.newer && d.installable && !d.fileMismatch && !d.skipped, "новее — предложить");
    d = up::decide(r, "0.1.0", "0.2.0");
    CHECK(d.newer && d.skipped, "пропущенная версия");
    d = up::decide(r, "0.1.0", "0.1.5");
    CHECK(!d.skipped, "пропущена другая версия — снова предложить");
    d = up::decide(r, "0.2.0", "");
    CHECK(!d.newer && !d.installable && !d.fileMismatch, "уже последняя");
    r = up::parseRelease(json("v0.3.0", asset("ModKitNX.nro", 1, "sha256:" + hexA), "x", true), "ModKitNX.nro");
    CHECK(!up::decide(r, "0.1.0", "").newer, "pre-release не предлагается");
    r = up::parseRelease(json("v0.3.0", asset("x.zip", 1)), "ModKitNX.nro");
    d = up::decide(r, "0.1.0", "");
    CHECK(d.newer && !d.installable && !d.fileMismatch, "новее, но без .nro — «нет ассета»");
    // sha256 нет ни в digest, ни в описании — скачанное не с чем сверить: не ставить, статус «файл не сходится».
    r = up::parseRelease(json("v0.3.0", asset("ModKitNX.nro", 1)), "ModKitNX.nro");
    d = up::decide(r, "0.1.0", "");
    CHECK(r.ok && !r.assetUrl.empty() && r.sha256.empty() && d.newer && !d.installable && d.fileMismatch,
          "без sha256 — не installable, «файл не сходится»");
    // sha256 только в описании — ставится.
    r = up::parseRelease(json("v0.3.0", asset("ModKitNX.nro", 1), "sha256 (ModKitNX.nro): " + hexA), "ModKitNX.nro");
    d = up::decide(r, "0.1.0", "");
    CHECK(d.installable && !d.fileMismatch, "sha256 из описания — installable");
    // digest ≠ описание — «файл не сходится», а не «нет ассета».
    r = up::parseRelease(json("v0.3.0", asset("ModKitNX.nro", 1, "sha256:" + hexA), "sha256: " + hexB), "ModKitNX.nro");
    d = up::decide(r, "0.1.0", "");
    CHECK(r.shaMismatch && d.newer && !d.installable && d.fileMismatch, "digest != описание — «файл не сходится»");
}

void testParseBadTypes() {
    // draft/prerelease не bool: j.value() бросал type_error вне try (std::terminate).
    const std::string hexA(64, 'a');
    const std::string a = asset("ModKitNX.nro", 1, "sha256:" + hexA);
    bool threw = false;
    up::Release r1, r2, r3;
    try {
        r1 = up::parseRelease("{\"tag_name\":\"v0.2.0\",\"draft\":\"false\",\"prerelease\":null,\"assets\":[" + a + "]}",
                              "ModKitNX.nro");
        r2 = up::parseRelease("{\"tag_name\":\"v0.2.0\",\"draft\":0,\"prerelease\":\"true\",\"assets\":[" + a + "]}",
                              "ModKitNX.nro");
        r3 = up::parseRelease("{\"tag_name\":\"v0.2.0\",\"draft\":true,\"prerelease\":[],\"assets\":[" + a + "]}",
                              "ModKitNX.nro");
    } catch (...) {
        threw = true;
    }
    CHECK(!threw, "неверные типы draft/prerelease — без исключения");
    CHECK(r1.ok && !r1.draft && !r1.prerelease && up::decide(r1, "0.1.0", "").installable,
          "draft:\"false\", prerelease:null — считаются false");
    CHECK(r2.ok && !r2.draft && !r2.prerelease, "draft:0, prerelease:\"true\" — не bool, не флаг");
    CHECK(r3.ok && r3.draft && !up::decide(r3, "0.1.0", "").newer, "настоящий draft:true — не предлагается");
}

void testNotes() {
    const std::string body =
        "## What's new\r\n\r\n\r\n* **One** thing\r\n- two\r\n\r\n---\r\nsha256 (ModKitNX.nro): `" + std::string(64, 'c') + "`\r\n";
    const std::string n = up::notesForDisplay(body, 700, 12);
    CHECK(n == "What's new\n• One thing\n• two", "разметка, пустые строки, sha256 и --- убраны");
    std::string longBody;
    for (int i = 0; i < 40; ++i) longBody += "строка номер " + std::to_string(i) + "\n";
    const std::string cut = up::notesForDisplay(longBody, 100, 5);
    CHECK(cut.size() <= 100 + 3 && cut.find("…") != std::string::npos, "обрезка с …");
    const std::string ru(300, 'x');
    const std::string cyr = up::notesForDisplay("Привет мир, это длинная строка " + std::string(200, 'z'), 41, 3);
    // Обрезанная строка — целые символы UTF-8 (последний байт перед «…» — не середина символа).
    const std::string head = cyr.substr(0, cyr.size() - std::strlen("…"));
    CHECK(!head.empty() && (static_cast<unsigned char>(head.back()) & 0xC0) != 0xC0, "UTF-8 не разрезан");
    (void)ru;
}

void testNro() {
    auto& h = mk::soft_hash();
    const auto nro = makeNro("ModKitNX", "0.2.0");
    up::Release r = up::parseRelease(json("v0.2.0", asset("ModKitNX.nro", nro.size(), "sha256:" + sha(nro))), "ModKitNX.nro");
    up::NroInfo i = up::verifyNro(nro.data(), nro.size(), r, "ModKitNX", h);
    CHECK(i.ok && i.name == "ModKitNX" && i.version == "0.2.0", "целый .nro проходит");

    i = up::verifyNro(nro.data(), nro.size() - 1, r, "ModKitNX", h);
    CHECK(!i.ok && i.error == "size", "размер не как у ассета");
    up::Release noSize = r;
    noSize.assetSize = 0;
    i = up::verifyNro(nro.data(), nro.size() - 100, noSize, "ModKitNX", h);
    CHECK(!i.ok && i.error == "no_nacp", "оборванный NACP");
    std::vector<uint8_t> html(nro.size(), 'x');
    i = up::verifyNro(html.data(), html.size(), r, "ModKitNX", h);
    CHECK(!i.ok && i.error == "not_nro", "страница ошибки вместо файла");
    const auto other = makeNro("OtherApp", "0.2.0");
    up::Release ro = r;
    ro.sha256.clear();
    i = up::verifyNro(other.data(), other.size(), ro, "ModKitNX", h);
    CHECK(!i.ok && i.error == "name", "чужое приложение");
    const auto oldv = makeNro("ModKitNX", "0.1.0");
    i = up::verifyNro(oldv.data(), oldv.size(), ro, "ModKitNX", h);
    CHECK(!i.ok && i.error == "version", "версия NACP не та");
    auto bad = nro;
    bad[0x200] ^= 1;   // байт внутри NACP (не имя и не версия): sha256 не совпадёт
    i = up::verifyNro(bad.data(), bad.size(), r, "ModKitNX", h);
    CHECK(!i.ok && i.error == "sha256", "sha256 не совпал");
    const auto slot = makeNro("ModKitNX", "0.2.0", 3);   // имя только в записи другого языка
    i = up::verifyNro(slot.data(), slot.size(), ro, "ModKitNX", h);
    CHECK(i.ok, "имя в записи любого языка");
}

void testFiles(const std::string& dir) {
    CHECK(up::selfNroPath("sdmc:/switch/ModKitNX/ModKitNX.nro") == "sdmc:/switch/ModKitNX/ModKitNX.nro", "argv0 sdmc");
    CHECK(up::selfNroPath("/switch/ModKitNX.nro") == "sdmc:/switch/ModKitNX.nro", "argv0 без тома");
    CHECK(up::selfNroPath("ModKitNX") == "", "argv0 не .nro");
    CHECK(up::selfNroPath(nullptr) == "", "argv0 нет");
    CHECK(up::restartArgv("sdmc:/switch/ModKitNX.nro") == "sdmc:/switch/ModKitNX.nro", "argv без пробела — как есть");
    CHECK(up::restartArgv("sdmc:/switch/My Apps/ModKitNX.nro") == "\"sdmc:/switch/My Apps/ModKitNX.nro\"",
          "argv с пробелом — в кавычках (argvSetup libnx)");

    const std::string self = dir + "/ModKitNX.nro";
    const auto cur = makeNro("ModKitNX", "0.1.0"), next = makeNro("ModKitNX", "0.2.0");
    const std::string curS(cur.begin(), cur.end()), nextS(next.begin(), next.end());
    writeBytes(self, cur);
    std::string err;
    CHECK(!up::hasPending(self, "0.1.0"), "нет .new");
    CHECK(!up::swapInPending(self, "0.1.0", err) && err == "no_pending", "нечего ставить");
    CHECK(up::stagePending(self, next, err), "запись .new");
    CHECK(exists(up::pendingPath(self)) && !exists(up::partPath(self)), ".part переименован в .new");
    CHECK(up::hasPending(self, "0.1.0"), ".new целый и новее");

    // Последовательность замены: выход (swap) → .old остаётся → следующий успешный старт убирает .old.
    CHECK(up::swapInPending(self, "0.1.0", err), "замена");
    CHECK(readText(self) == nextS, "на месте — новая версия");
    CHECK(!exists(up::pendingPath(self)) && exists(up::oldPath(self)) && readText(up::oldPath(self)) == curS,
          ".new ушёл, .old (прежняя версия) остался до старта новой");
    CHECK(up::cleanupAfterStart(self) == "old_removed" && !exists(up::oldPath(self)) && readText(self) == nextS,
          "старт новой версии: .old убран, свой .nro цел");
    CHECK(up::cleanupAfterStart(self) == "", "второй старт — нечего делать");

    // Своего .nro нет, а .old и .new лежат (замена оборвалась снаружи) — при старте ничего не трогать.
    std::rename(self.c_str(), up::oldPath(self).c_str());
    writeBytes(up::pendingPath(self), next);
    CHECK(up::cleanupAfterStart(self) == "self_missing" && exists(up::oldPath(self)) && exists(up::pendingPath(self)),
          "своего .nro нет — .old и .new не тронуты");
    std::rename(up::oldPath(self).c_str(), self.c_str());   // вернуть как было
    std::rename(up::pendingPath(self).c_str(), (dir + "/pending.moved").c_str());

    // Битый .new не применяется, свой .nro не тронут.
    writeBytes(up::pendingPath(self), std::vector<uint8_t>(100, 'x'));
    CHECK(!up::hasPending(self, "0.1.0") && !up::swapInPending(self, "0.1.0", err), "битый .new не ставится");
    CHECK(readText(self) == nextS, "свой .nro цел");
    std::rename(up::pendingPath(self).c_str(), (dir + "/broken.moved").c_str());

    // Залежавшийся .new не новее работающей версии (0.2.0 уже стоит) — не откат: в .new.stale, свой .nro цел.
    writeBytes(up::pendingPath(self), cur);   // 0.1.0
    CHECK(!up::hasPending(self, "0.2.0"), "старый .new — не ожидает установки");
    CHECK(!exists(up::pendingPath(self)) && exists(up::stalePath(self)) && readText(up::stalePath(self)) == curS,
          "старый .new переименован в .new.stale");
    writeBytes(up::pendingPath(self), next);   // та же версия, что работает
    CHECK(!up::swapInPending(self, "0.2.0", err) && err == "no_pending" && readText(self) == nextS,
          ".new той же версии не ставится, свой .nro цел");
    CHECK(!exists(up::pendingPath(self)) && readText(up::stalePath(self)) == nextS, "прежний .new.stale заменён новым");

    // Своего .nro нет (переименовали снаружи) — .new всё равно встаёт на место.
    const auto third = makeNro("ModKitNX", "0.3.0");
    CHECK(up::stagePending(self, third, err), "запись .new 2");
    std::rename(self.c_str(), (dir + "/self.moved").c_str());
    CHECK(up::swapInPending(self, "0.2.0", err), "замена без старого файла");
    CHECK(readText(self) == std::string(third.begin(), third.end()), "новый на месте");
}

// Перезапуск при выходе: порядок «замена → проверка пути → next load». Загрузчик падает на отсутствующем файле
// или не-NRO, поэтому planRelaunch смотрит файл уже после замены.
void testRelaunch(const std::string& dir) {
    const std::string self = dir + "/relaunch/ModKitNX.nro";
    ::mkdir((dir + "/relaunch").c_str(), 0777);
    up::RelaunchPlan p = up::planRelaunch(false, true, self);
    CHECK(!p.relaunch && p.reason == "not_requested", "не просили — обычный выход");
    p = up::planRelaunch(true, false, self);
    CHECK(!p.relaunch && p.reason == "no_loader", "загрузчик без next load — закрыть");
    p = up::planRelaunch(true, true, "");
    CHECK(!p.relaunch && p.reason == "no_self", "путь неизвестен — закрыть");
    p = up::planRelaunch(true, true, self);
    CHECK(!p.relaunch && p.reason == "self_missing", "файла нет — загрузчику не отдавать");
    writeBytes(self, std::vector<uint8_t>(0x80, 'x'));
    p = up::planRelaunch(true, true, self);
    CHECK(!p.relaunch && p.reason == "not_nro", "на месте не NRO — загрузчику не отдавать");
    writeBytes(self, std::vector<uint8_t>(0x12, 0));
    p = up::planRelaunch(true, true, self);
    CHECK(!p.relaunch && p.reason == "not_nro", "обрезанный заголовок — не NRO");

    // Полный путь: старая версия на месте, .new скачан → выход: замена → план → новая версия по тому же пути.
    const auto cur = makeNro("ModKitNX", "0.1.0"), next = makeNro("ModKitNX", "0.2.0");
    writeBytes(self, cur);
    std::string err;
    CHECK(up::stagePending(self, next, err), "relaunch: .new записан");
    CHECK(up::swapInPending(self, "0.1.0", err), "relaunch: замена при выходе");
    p = up::planRelaunch(true, true, self);
    CHECK(p.relaunch && p.reason == "ok" && p.path == self && p.argv == self, "после замены — свой путь загрузчику");
    CHECK(readText(p.path) == std::string(next.begin(), next.end()), "по пути next load лежит новая версия");

    // Путь с пробелом — argv в кавычках; путь не влезает в буфер загрузчика — не отдавать.
    const std::string spaced = dir + "/relaunch/My Apps/ModKitNX.nro";
    ::mkdir((dir + "/relaunch/My Apps").c_str(), 0777);
    writeBytes(spaced, next);
    p = up::planRelaunch(true, true, spaced);
    CHECK(p.relaunch && p.path == spaced && p.argv == "\"" + spaced + "\"", "путь с пробелом — argv в кавычках");
    const std::string longDir = dir + "/relaunch/" + std::string(up::kLoaderPathMax, 'a');
    p = up::planRelaunch(true, true, longDir + "/ModKitNX.nro");
    CHECK(!p.relaunch && p.reason == "too_long", "путь длиннее буфера загрузчика");
}

void testConfig(const std::string& dir) {
    const std::string p = dir + "/config.json";
    cfg::setPathForTests(p);
    CHECK(cfg::getBool(cfg::kUpdateCheck, true), "нет файла — по умолчанию");
    CHECK(cfg::getString(cfg::kTheme, "system") == "system", "тема по умолчанию");
    CHECK(cfg::setString(cfg::kLanguage, "uk"), "запись языка");
    CHECK(cfg::setBool(cfg::kUpdateCheck, false), "запись флажка");
    CHECK(cfg::setString(cfg::kTheme, "light"), "запись темы");
    CHECK(cfg::getString(cfg::kLanguage) == "uk" && !cfg::getBool(cfg::kUpdateCheck, true) &&
              cfg::getString(cfg::kTheme) == "light",
          "поля не затирают друг друга");
    CHECK(exists(p + ".prev") && !exists(p + ".part"), "одна прежняя копия .prev, без .part");
    CHECK(cfg::getBool(cfg::kTheme, true), "тип не тот — по умолчанию");
    FILE* f = std::fopen(p.c_str(), "wb");
    std::fputs("{broken", f);
    std::fclose(f);
    CHECK(cfg::getString(cfg::kLanguage, "x") == "x", "битый файл — по умолчанию");
    CHECK(cfg::setString(cfg::kLanguage, "ru") && cfg::getString(cfg::kLanguage) == "ru", "битый файл перезаписан");
    // Запись оборвалась между «config.json → .prev» и «.part → config.json»: config.json нет, есть .prev.
    // Следующая запись поля не должна потерять остальные поля (берутся из .prev).
    CHECK(cfg::setString(cfg::kTheme, "dark"), "запись темы перед обрывом");
    std::rename(p.c_str(), (p + ".prev").c_str());
    CHECK(!exists(p) && cfg::getString(cfg::kLanguage) == "ru", "config.json нет — читается .prev");
    CHECK(cfg::setBool(cfg::kUpdateCheck, true) && cfg::getString(cfg::kLanguage) == "ru" &&
              cfg::getString(cfg::kTheme) == "dark" && cfg::getBool(cfg::kUpdateCheck, false),
          "запись поля без config.json: прочие поля взяты из .prev, не потеряны");
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/mk_update_test_XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    if (!dir) {
        std::fprintf(stderr, "mkdtemp failed\n");
        return 2;
    }
    testVersions();
    testParse();
    testDecide();
    testParseBadTypes();
    testNotes();
    testNro();
    testFiles(dir);
    testRelaunch(dir);
    testConfig(dir);
    std::printf("app_update_tests: %d passed, %d failed (temp %s)\n", g_pass, g_fail, dir);
    return g_fail == 0 ? 0 : 1;
}
