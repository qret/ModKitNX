/*
 * autofix_test — хост-тест чистой логики автоисправления (src/nxr_autofix.hpp).
 *
 * Copyright (c) 2026, участники проекта nxredirect. Лицензия — GNU GPL версии 2.
 *
 * Что проверяется (g++ на ПК, тот же заголовок, что идёт в модуль):
 *   1. ПРАВИЛО Decide на всех сочетаниях фактов (таблица ниже).
 *   2. ПОТОКОБЕЗОПАСНОСТЬ ResolveOnce:
 *      а) детерминированно: между расчётом и публикацией первого «потока»
 *         целиком проходит второй и публикует ДРУГОЕ решение — первый обязан
 *         вернуть опубликованное, а не своё;
 *      б) 32 настоящих потока одновременно: у всех одно решение, слово одно.
 *   3. ЗАТВОР ПАСПОРТА: WritePassport при закрытом затворе (настоящий
 *      nxr::FsGate) — НОЛЬ обращений к ФС; после Open — одно.
 *   4. Разбор байтов: имя модуля (оба формата), строки SDK MW, build-id,
 *      «вернуть константу», путь паспорта, текст паспорта.
 * Код возврата: 0 — всё зелёное, 1 — есть провал.
 * Негативные пробы — hosttest/run_autofixtest.py (порча копии заголовка).
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "nxr_fsgate.hpp"
#include "nxr_autofix.hpp"

using namespace nxr::autofix;

static int g_Fail = 0;
static int g_Checks = 0;

static void Expect(bool cond, const char* what) {
    ++g_Checks;
    if (!cond) {
        ++g_Fail;
        std::printf("  ПРОВАЛ: %s\n", what);
    }
}

/* ------------------------------------------------------------- 1. правило */

static void TestDecide() {
    const FixInfo& hash = Catalog[FixHashMemory];
    const FixInfo& ldn  = Catalog[FixLdnSetProtocol];
    const FixInfo& nvrm = Catalog[FixNvrmQuery];
    const std::uint32_t v19 = EncodeVersion(19, 3, 13), v20 = EncodeVersion(20, 5, 17),
                        v22 = EncodeVersion(22, 2, 0), v15 = EncodeVersion(15, 3, 1);
    struct Row {
        const FixInfo* fix;
        FixFacts       f;
        Choice         c;
        Why            w;
        const char*    what;
    };
    const Row rows[] = {
        {&hash, {true, 0, false, v19, v22, false}, Choice::Own, Why::SdkMissing,
         "hash: 19.3.13 без символа -> своя"},
        {&hash, {true, 0x5f6120, false, v22, v22, false}, Choice::Proxy, Why::SdkDefined,
         "hash: родная 22.2.0 -> прокси"},
        {&ldn, {true, 0, false, v19, v22, true}, Choice::Own, Why::SdkMissing,
         "ldn: 19.3.13 -> своя заглушка"},
        {&ldn, {true, 0x55eb50, false, v20, v22, true}, Choice::Proxy, Why::SdkDefined,
         "ldn: 20.5.17 есть -> прокси (LDN работает)"},
        {&ldn, {true, 0x55eb50, true, v20, v22, true}, Choice::Proxy, Why::SdkDefined,
         "ldn: даже заглушка в sdk у MISSING -> прокси (родное)"},
        {&nvrm, {true, 0x45a370, true, v19, v22, false}, Choice::Own, Why::StubOlderSdk,
         "nvrm: WD на 19.3.13 -> своя"},
        {&nvrm, {true, 0x45a370, true, v19, v22, true}, Choice::Proxy, Why::StubVulkan,
         "nvrm: SiN (Vulkan) на 19.3.13 -> прокси в заглушку"},
        {&nvrm, {true, 0x468b90, false, v22, v22, false}, Choice::Proxy, Why::SdkReal,
         "nvrm: родная 22.2.0 -> прокси"},
        {&nvrm, {true, 0x45a370, true, v19, v15, false}, Choice::Proxy, Why::StubNative,
         "nvrm: старая игра (15.3.1) на 19.3.13 -> родная заглушка"},
        {&nvrm, {true, 0x45a370, true, v19, v19, false}, Choice::Proxy, Why::StubNative,
         "nvrm: равные версии -> родная заглушка"},
        {&nvrm, {true, 0x45a370, true, 0, v22, false}, Choice::Proxy, Why::StubNoVersion,
         "nvrm: версия sdk не прочлась -> прокси"},
        {&nvrm, {true, 0x45a370, true, v19, 0, false}, Choice::Proxy, Why::StubNoVersion,
         "nvrm: версия игры не прочлась -> прокси"},
        {&nvrm, {true, 0, false, v19, v22, true}, Choice::Own, Why::SdkMissing,
         "nvrm: символа нет вовсе -> своя (без нас ноль)"},
        {&nvrm, {false, 0x45a370, true, 0, v22, false}, Choice::Proxy, Why::NoSdkDefined,
         "nvrm: sdk не опознан -> как без нас"},
        {&hash, {false, 0, false, 0, 0, false}, Choice::Own, Why::NoSdkMissing,
         "hash: sdk не опознан и нигде нет -> своя"},
    };
    for (const Row& r : rows) {
        const Resolution got = Decide(*r.fix, r.f);
        Expect(got.choice == r.c && got.why == r.w, r.what);
        if (got.choice == Choice::Proxy) {
            Expect(got.target == r.f.def, "цель прокси = найденное определение");
        } else {
            Expect(got.target == 0, "у своей реализации цели нет");
        }
    }
}

/* -------------------------------------------------- 2. потокобезопасность */

static std::uint64_t g_Word = 0;

static void TestInterleave() {
    g_Word = 0;
    int computes = 0;
    Resolution inner {};
    /* «Поток A» считает своё решение (своя), но ДО публикации целиком
       проходит «поток B» и публикует другое (прокси). */
    const Resolution a = ResolveOnce(&g_Word, [&]() {
        ++computes;
        inner = ResolveOnce(&g_Word, [&]() {
            ++computes;
            Resolution r {};
            r.choice = Choice::Proxy;
            r.why = Why::SdkReal;
            r.target = 0x468b90;
            return r;
        });
        Resolution r {};
        r.choice = Choice::Own;
        r.why = Why::StubOlderSdk;
        return r;
    });
    Expect(computes == 2, "оба потока считали (ожидания нет)");
    Expect(inner.choice == Choice::Proxy, "поток B опубликовал своё");
    Expect(a.choice == Choice::Proxy && a.why == Why::SdkReal && a.target == 0x468b90,
           "поток A проиграл публикацию и взял ОПУБЛИКОВАННОЕ решение, а не своё");
    const Resolution later = ResolveOnce(&g_Word, []() {
        Resolution r {};
        r.choice = Choice::Own;
        return r;
    });
    Expect(later.choice == Choice::Proxy, "после публикации решение больше не меняется");
}

static void TestThreads() {
    for (int round = 0; round < 50; ++round) {
        std::uint64_t word = 0;
        std::atomic<int> go {0};
        std::vector<Resolution> got(32);
        std::vector<std::thread> th;
        for (int i = 0; i < 32; ++i) {
            th.emplace_back([&, i]() {
                go.fetch_add(1);
                while (go.load() < 32) {
                }
                got[i] = ResolveOnce(&word, [i]() {
                    Resolution r {};
                    r.choice = (i & 1) ? Choice::Own : Choice::Proxy;
                    r.why = Why::SdkReal;
                    r.target = 0x1000u + static_cast<std::uint64_t>(i) * 4;
                    return r;
                });
            });
        }
        for (auto& t : th) {
            t.join();
        }
        const Resolution pub = Unpack(word);
        bool same = word != 0;
        for (const Resolution& r : got) {
            same &= r.choice == pub.choice && r.target == pub.target && r.why == pub.why;
        }
        if (!same) {
            Expect(false, "32 потока получили РАЗНЫЕ решения");
            return;
        }
    }
    Expect(true, "32 потока x 50 раундов: решение у всех одно и равно опубликованному");
}

static void TestPack() {
    Resolution r {};
    r.choice = Choice::Proxy;
    r.why = Why::StubVulkan;
    r.target = 0x7fffff1234ull;
    const Resolution u = Unpack(Pack(r));
    Expect(u.choice == r.choice && u.why == r.why && u.target == r.target, "упаковка обратима");
    r.target = 1ull << 50;
    const Resolution far = Packable(r);
    Expect(far.choice == Choice::Own && far.why == Why::TargetTooFar, "цель вне 48 бит -> своя");
    Expect(Pack(Packable(Resolution {})) != 0, "принятое решение никогда не 0");
}

/* -------------------------------------------------------- 3. затвор паспорта */

struct FakeFs {
    int calls = 0;
    std::string lastPath;
    bool Write(const char* path, const char*, std::size_t) {
        ++calls;
        lastPath = path;
        return true;
    }
};

static void TestPassportGate() {
    nxr::FsGate gate;
    FakeFs fs;
    const char text[] = "x";
    const bool before = WritePassport(gate, fs, "nxrsd:/p.txt", text, 1);
    Expect(!before && fs.calls == 0, "затвор закрыт: паспорт НЕ пишется, обращений к ФС ноль");
    gate.Open(nxr::GateReason::SetAllocator);
    const bool after = WritePassport(gate, fs, "nxrsd:/p.txt", text, 1);
    Expect(after && fs.calls == 1, "затвор открыт: ровно одна запись");
}

/* ------------------------------------------------------------ 4. разбор байтов */

static void TestBytes() {
    /* имя модуля: старый формат nnSdk 19.3.13 */
    const unsigned char old[] = {0, 0, 0, 0, 5, 0, 0, 0, 'n', 'n', 'S', 'd', 'k', 0, 0, 0};
    const char* nm = nullptr;
    std::uint32_t n = ModuleNameView(old, sizeof(old), &nm);
    Expect(n == 5 && NameIs(nm, n, "nnSdk"), "имя: старый формат (19.3.13)");
    /* новый формат 20.5.17/22.2.0 */
    const unsigned char nw[] = {1, 0, 0, 0, 0x1c, 0, 0, 0, 0x50, 0, 0, 0, 0, 0, 0, 0,
                                5, 0, 0, 0, 'n', 'n', 'S', 'd', 'k', 0, 0, 0, 'M', 'O', 'D', '0'};
    n = ModuleNameView(nw, sizeof(nw), &nm);
    Expect(n == 5 && NameIs(nm, n, "nnSdk"), "имя: новый формат (22.2.0)");
    /* путь с расширением — только базовое имя */
    const char path[] = "C:\\Jenkins\\build\\isis_NX64.nss";
    std::vector<unsigned char> p(8 + sizeof(path) - 1);
    p[4] = static_cast<unsigned char>(sizeof(path) - 1);
    std::memcpy(p.data() + 8, path, sizeof(path) - 1);
    n = ModuleNameView(p.data(), p.size(), &nm);
    Expect(n == 9 && NameIs(nm, n, "isis_NX64"), "имя: путь Windows -> базовое имя без .nss");
    Expect(!NameIs("nnSdkX", 6, "nnSdk") && !NameIs("nnSd", 4, "nnSdk"), "имя: только точное совпадение");

    const char mw[] = "....SDK MW+Nintendo+NintendoSDK_libz-19_3_13-Release\0"
                      "SDK MW+Nintendo+NintendoSdk_nnSdk-19_3_13-Release\0"
                      "SDK MW+YoYoGames+GameMaker_Studio_2-2026.0.0.23-Release\0"
                      "SDK MW+Nintendo+NintendoWare_Vfx2-25_3_1-Release\0";
    Expect(ScanMwVersion(reinterpret_cast<const unsigned char*>(mw), sizeof(mw)) ==
               EncodeVersion(19, 3, 13),
           "версия SDK MW: 19.3.13; чужие поставщики и NintendoWare не считаются");
    const char mw2[] = "SDK MW+Nintendo+NintendoSDK_OpenGL-22_2_0-Release\0"
                       "SDK MW+Nintendo+NintendoSDK_movie-22_2_5-Release";
    Expect(ScanMwVersion(reinterpret_cast<const unsigned char*>(mw2), sizeof(mw2)) ==
               EncodeVersion(22, 2, 5), "версия SDK MW: наибольшая (22.2.5)");
    Expect(EncodeVersion(19, 3, 13) < EncodeVersion(20, 5, 17) &&
               EncodeVersion(20, 5, 17) < EncodeVersion(22, 2, 0) &&
               EncodeVersion(22, 2, 0) < EncodeVersion(22, 2, 5),
           "порядок версий 19.3.13 < 20.5.17 < 22.2.0 < 22.2.5");

    unsigned char note[60] = {};
    const unsigned char hdr[16] = {4, 0, 0, 0, 0x14, 0, 0, 0, 3, 0, 0, 0, 'G', 'N', 'U', 0};
    std::memcpy(note + 8, hdr, 16);
    for (int i = 0; i < 20; ++i) note[24 + i] = static_cast<unsigned char>(0x7B + i);
    unsigned char id[20] = {};
    Expect(FindBuildId(note, sizeof(note), id) && id[0] == 0x7B && id[19] == 0x7B + 19,
           "build-id из note GNU");

    Expect(IsReturnConstStub(0x52800040u, 0xD65F03C0u), "заглушка 19.3.13 `mov w0,#2; ret`");
    Expect(IsReturnConstStub(0x2A1F03E0u, 0xD65F03C0u), "заглушка `mov w0,wzr; ret`");
    Expect(!IsReturnConstStub(0xD10303FFu, 0xA9017BFDu), "пролог 22.2.0 — НЕ заглушка");
    Expect(!IsReturnConstStub(0x52800040u, 0x00000000u), "без ret — не заглушка");
    Expect(!IsReturnConstStub(0x52800041u, 0xD65F03C0u), "mov w1 — не заглушка (ответ не в w0)");

    char out[128];
    const char pre[] = "nxrsd:/atmosphere/contents/0100217023F6C000/files/";
    const std::size_t len = PassportPath(out, sizeof(out), pre, sizeof(pre) - 1);
    Expect(len != 0 && std::strcmp(out, "nxrsd:/atmosphere/contents/0100217023F6C000/"
                                         "nxredirect-passport.txt") == 0,
           "путь паспорта — рядом с exefs/ и files/");
    Expect(PassportPath(out, sizeof(out), "nxrsd:/x/", 9) == 0, "чужой префикс -> нет пути");
    Expect(PassportPath(out, 20, pre, sizeof(pre) - 1) == 0, "не влез -> нет пути");
}

static void TestPassportText() {
    PassportFacts f {};
    f.titleId = 0x0100217023F6C000ull;
    f.slot = "subsdk9";
    f.kernelMetaOk = true;
    f.kernelMeta = 0x1234;
    f.view.sdkNamed = 1;
    f.view.sdkBase = 0x1713d21000ull;
    f.view.sdkVer = EncodeVersion(19, 3, 13);
    f.view.gameVer = EncodeVersion(22, 2, 0);
    f.view.haveBuildId = true;
    const unsigned char id[20] = {0x7B, 0xBA, 0x18, 0x5A};
    std::memcpy(f.view.sdkBuildId, id, 20);
    f.res[FixHashMemory] = {Choice::Own, Why::SdkMissing, 0};
    f.res[FixLdnSetProtocol] = {Choice::Own, Why::SdkMissing, 0};
    f.res[FixNvrmQuery] = {Choice::Own, Why::StubOlderSdk, 0};
    static TextBuf<0x1000> b;
    FormatPassport(b, f);
    const std::string s(b.Data(), b.Size());
    Expect(s.find("0100217023F6C000") != std::string::npos, "паспорт: TitleID 16 цифр");
    Expect(s.find("версия 19.3.13") != std::string::npos, "паспорт: версия sdk");
    Expect(s.find("7BBA185A") != std::string::npos, "паспорт: BuildId");
    Expect(s.find("ДА — sdk старше") != std::string::npos, "паспорт: подмена видна");
    Expect(s.find("nvrm_query: СВОЯ") != std::string::npos, "паспорт: решения по исправлениям");
    Expect(s.find("\r\n") != std::string::npos, "паспорт: строки CRLF");
    Expect(b.Size() < 0x1000 - 8, "паспорт помещается в буфер");
}

int main() {
    std::printf("autofix_test: чистая логика автоисправления (src/nxr_autofix.hpp)\n");
    TestDecide();
    TestPack();
    TestInterleave();
    TestThreads();
    TestPassportGate();
    TestBytes();
    TestPassportText();
    const int MinChecks = 40;
    if (g_Checks < MinChecks) {
        std::printf("ЗАТВОР НУЛЯ: проверок %d < %d\n", g_Checks, MinChecks);
        return 1;
    }
    std::printf("проверок: %d, провалов: %d\n", g_Checks, g_Fail);
    std::printf("ИТОГ: %s\n", g_Fail == 0 ? "ВСЕ ПРОВЕРКИ ЗЕЛЁНЫЕ" : "ЕСТЬ ПРОВАЛЫ");
    return g_Fail == 0 ? 0 : 1;
}
