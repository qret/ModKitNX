/*
 * fsgate_test.cpp — хост-тест ЗАТВОРА ФАЙЛОВОЙ СИСТЕМЫ (src/nxr_fsgate.hpp).
 *
 * Copyright (c) 2026, участники проекта nxredirect. GNU GPL версии 2.
 *
 * Что доказывается на ПК, без консоли:
 *   1. ДО разрешающего момента модуль не делает с nn::fs ни одного обращения —
 *      ни монтирования SD, ни открытия своих файлов;
 *   2. ПОСЛЕ разрешающего момента обращается и работает как прежде
 *      (перенаправление снова включается);
 *   3. ранние события (обращения игры к OpenFile, пока затвор закрыт) не
 *      теряются: они копятся в памяти и выливаются в отчёт, а то, что не
 *      поместилось, честно считается;
 *   4. игра, которая ЗОВЁТ nn::fs::SetAllocator, ждёт именно его: открывать
 *      затвор по первому же OpenFile у такой игры нельзя (её вызов мог ничего
 *      не выделить, и тогда флаг дефолтного распределителя взвели бы МЫ);
 *   5. игра, которая SetAllocator не зовёт вовсе (так устроен SiN Reloaded),
 *      получает затвор по своему первому обращению к nn::fs;
 *   6. затвор открывается ровно один раз, и причина первого открытия не
 *      перезаписывается — иначе в отчёте была бы неверная история;
 *   7. SD монтируется ровно один раз, даже когда OpenFsGate и ленивый
 *      EnsureSdMounted из другого потока сходятся (nxr::SdMount).
 *
 * Модель игры и nnSdk здесь намеренно грубая, но воспроизводит ровно тот
 * механизм, на котором игра падала (проба на консоли, Well Dweller):
 * любое обращение к nn::fs взводит флаг дефолтного распределителя, а
 * nn::fs::SetAllocator при взведённом флаге уходит в Abort (Result 0x327602).
 *
 * У КАЖДОЙ проверки есть НЕГАТИВНАЯ ПРОБА: рядом прогоняется ослабленная
 * копия той же логики на ТОМ ЖЕ входе, и тест требует, чтобы ослабленная
 * ПРОВАЛИЛАСЬ. Плюс «затвор нуля»: тест, не выполнивший ни одной проверки,
 * считается красным.
 *
 * Сборка и запуск (нужен только g++, devkitPro не требуется):
 *     g++ -std=c++20 -O2 -Wall -pthread -o fsgate_test fsgate_test.cpp -I../src && ./fsgate_test
 *
 * Код возврата: 0 — всё зелено, 1 — есть красные.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "nxr_fsgate.hpp"

static int g_Checks = 0;   /* сколько проверок реально выполнено (затвор нуля) */
static int g_Failed = 0;

static void Expect(bool cond, const char* what, const std::string& detail = "") {
    ++g_Checks;
    if (cond) {
        std::printf("  ok   %s\n", what);
    } else {
        ++g_Failed;
        std::printf("  КРАСНО %s: %s\n", what, detail.c_str());
    }
}

/* ===================================================================== */
/* МОДЕЛЬ nn::fs и игры                                                  */
/* ===================================================================== */

struct FakeSdk {
    /* Флаг «дефолтным распределителем nn::fs уже пользовались» — тот самый
       байт [0xb9398c], который проверяет nn::fs::SetAllocator. */
    bool allocatorUsed = false;
    /* Задан ли распределитель игры. */
    bool allocatorSet  = false;
    /* Игра остановлена аварийно (SetAllocator при взведённом флаге). */
    bool aborted       = false;
    /* Сколько раз nn::fs трогал НАШ модуль. */
    int  moduleTouches = 0;

    /* Обращение модуля к nn::fs: монтирование SD, запись отчёта и т. п. */
    void ModuleTouchesFs() {
        ++moduleTouches;
        allocatorUsed = true;
    }

    /* Обращение самой игры, которое ВЫДЕЛЯЕТ память в nn::fs. */
    void GameUsesFs() { allocatorUsed = true; }

    /* nn::fs::SetAllocator глазами nnSdk. */
    void SetAllocator() {
        if (allocatorUsed) {
            aborted = true;   /* Result 0x327602 -> Abort -> svcBreak */
            return;
        }
        allocatorSet  = true;
        allocatorUsed = true;
    }
};

/* Модель нашего модуля: та же последовательность действий, что в src/main.cpp. */
struct Module {
    FakeSdk*     sdk = nullptr;
    nxr::FsGate  gate;
    bool         gameCallsSetAllocator = false;
    /* Маленький буфер намеренно: переполнение — отдельная проверка. */
    nxr::EarlyLog<4, 32> early;
    /* Строки «отчёта»: сюда выливаются ранние события. */
    std::vector<std::string> report;
    int redirects = 0;
    int deferredRuns = 0;

    /* Ослабленная политика для негативных проб (см. ниже). */
    bool weakAlwaysNormal = false;
    bool weakNoEarlyLog   = false;
    bool weakNoRetry      = false;   /* как раньше: вызов, открывший затвор, не повторяется */

    /* Откуда отдан файл в последнем OnOpenFileEx: 0 — ошибка, 1 — romfs, 2 — SD. */
    enum Served { NotFound = 0, FromRomfs = 1, FromSd = 2 };

    void Init(FakeSdk& s, bool callsSetAllocator) {
        sdk = &s;
        gameCallsSetAllocator = callsSetAllocator;
        early.Init();
    }

    /* OpenFsGate из main.cpp: отложенная работа выполняется один раз. */
    void OpenFsGate(nxr::GateReason reason) {
        if (!gate.Open(reason)) {
            return;
        }
        ++deferredRuns;
        sdk->ModuleTouchesFs();        /* монтирование SD */
        for (int i = 0; i < early.Count(); ++i) {
            report.push_back(std::string("ранний ") + early.At(i).m_Path);
        }
        if (early.Dropped() != 0) {
            report.push_back("не поместилось: " + std::to_string(early.Dropped()));
        }
        early.Clear();
    }

    /* Хук nn::fs::SetAllocator: сначала оригинал, только потом наша работа. */
    void OnSetAllocator() {
        sdk->SetAllocator();                      /* Orig() */
        OpenFsGate(nxr::GateReason::SetAllocator);
    }

    /* Хук nn::fs::OpenFile. */
    void OnOpenFile(const char* path, int mode, bool gameAllocates) {
        nxr::OpenFileAction action =
            nxr::DecideOpenFile(gate, gameCallsSetAllocator);
        if (weakAlwaysNormal) {
            action = nxr::OpenFileAction::Normal;   /* ОСЛАБЛЕНО */
        }

        if (action != nxr::OpenFileAction::Normal) {
            if (!weakNoEarlyLog) {
                early.Add(path, mode);
            }
            if (gameAllocates) {
                sdk->GameUsesFs();                  /* Orig(): работает сама игра */
            }
            if (action == nxr::OpenFileAction::PassThroughThenOpenGate) {
                OpenFsGate(nxr::GateReason::GameFsUse);
            }
            return;
        }

        /* Обычная работа: тут модуль и трогает nn::fs. */
        if (gameAllocates) {
            sdk->GameUsesFs();
        }
        sdk->ModuleTouchesFs();
        ++redirects;
    }

    /*
     * То же с моделью наличия файла (баг 2): есть ли он в romfs
     * игры и есть ли на SD. Повторяет порядок OpenFileHook из main.cpp:
     * при закрытом затворе — оригинал; если затвор открыл ЭТОТ путь и оригинал
     * файл не нашёл — повтор с SD (nxr::RetryAfterGateOpen). При открытом —
     * сначала SD, потом оригинал.
     */
    Served OnOpenFileEx(const char* path, int mode, bool inRomfs, bool onSd) {
        const nxr::OpenFileAction action = nxr::DecideOpenFile(gate, gameCallsSetAllocator);
        if (action != nxr::OpenFileAction::Normal) {
            early.Add(path, mode);
            const bool origOk = inRomfs;
            if (origOk) {
                sdk->GameUsesFs();                      /* Orig() нашёл файл: выделил память */
            }
            if (action == nxr::OpenFileAction::PassThroughThenOpenGate) {
                OpenFsGate(nxr::GateReason::GameFsUse);
            }
            if (!weakNoRetry && nxr::RetryAfterGateOpen(action, !origOk) && gate.IsOpen()) {
                sdk->ModuleTouchesFs();                 /* монтирование SD + Orig(sdPath) */
                if (onSd) {
                    ++redirects;
                    return FromSd;
                }
            }
            return origOk ? FromRomfs : NotFound;
        }
        sdk->ModuleTouchesFs();
        if (onSd) {
            ++redirects;
            return FromSd;
        }
        return inRomfs ? FromRomfs : NotFound;
    }
};

/* ===================================================================== */
/* 1. Игра, которая ЗОВЁТ SetAllocator (так устроен Well Dweller)          */
/* ===================================================================== */

static void TestGameWithSetAllocator() {
    std::printf("\n--- игра зовёт nn::fs::SetAllocator (Well Dweller) ---\n");

    FakeSdk sdk;
    Module  mod;
    mod.Init(sdk, /* callsSetAllocator = */ true);

    /* Раннее обращение игры, которое НИЧЕГО не выделяет (romfs ещё не
       смонтирован — вызов вернёт ошибку). Затвор обязан остаться закрытым. */
    mod.OnOpenFile("rom:/boot.cfg", 1, /* gameAllocates = */ false);

    Expect(sdk.moduleTouches == 0 && !sdk.allocatorUsed,
           "до SetAllocator модуль не сделал с nn::fs ни одного обращения",
           "обращений " + std::to_string(sdk.moduleTouches));
    Expect(!mod.gate.IsOpen(),
           "затвор у такой игры НЕ открывается по первому OpenFile");
    Expect(mod.early.Count() == 1,
           "раннее событие запомнено в памяти",
           std::to_string(mod.early.Count()));

    /* Игра задаёт свой распределитель. */
    mod.OnSetAllocator();

    Expect(!sdk.aborted && sdk.allocatorSet,
           "игра приняла свой распределитель и НЕ упала (нет 0x327602)");
    Expect(mod.gate.IsOpen() && mod.gate.Reason() == nxr::GateReason::SetAllocator,
           "затвор открыт именно возвратом из SetAllocator");
    Expect(mod.deferredRuns == 1 && sdk.moduleTouches == 1,
           "отложенная работа выполнена ровно один раз",
           std::to_string(mod.deferredRuns));
    Expect(mod.report.size() == 1 && mod.report[0] == "ранний rom:/boot.cfg",
           "раннее событие вылито в отчёт, а не потеряно",
           std::to_string(mod.report.size()));
    Expect(mod.early.Count() == 0,
           "буфер ранних событий после слива пуст");

    /* Дальше всё работает как прежде. */
    mod.OnOpenFile("rom:/a.dat", 1, true);
    mod.OnOpenFile("rom:/b.dat", 1, true);
    Expect(mod.redirects == 2 && sdk.moduleTouches == 3,
           "после разрешающего момента модуль снова работает с nn::fs",
           std::to_string(mod.redirects));
    Expect(!sdk.aborted, "игра жива до конца сценария");

    /* ---- негативная проба ---- */
    /* Ослабленная политика: затвора нет, модуль работает с nn::fs сразу.
       На ТОМ ЖЕ входе она обязана убить игру — значит проверка не пустая. */
    {
        FakeSdk weakSdk;
        Module  weak;
        weak.Init(weakSdk, true);
        weak.weakAlwaysNormal = true;
        weak.OnOpenFile("rom:/boot.cfg", 1, false);
        const bool touchedEarly = weakSdk.moduleTouches > 0;
        weak.OnSetAllocator();
        Expect(touchedEarly && weakSdk.aborted,
               "НЕГАТИВНАЯ ПРОБА: без затвора модуль трогает nn::fs до "
               "SetAllocator и игра аварийно останавливается");
    }
}

/* ===================================================================== */
/* 2. Игра, которая SetAllocator НЕ зовёт (так устроен SiN Reloaded)       */
/* ===================================================================== */

static void TestGameWithoutSetAllocator() {
    std::printf("\n--- игра не зовёт SetAllocator (SiN Reloaded) ---\n");

    FakeSdk sdk;
    Module  mod;
    mod.Init(sdk, /* callsSetAllocator = */ false);

    mod.OnOpenFile("app0:/data/first.bin", 1, /* gameAllocates = */ true);

    Expect(mod.deferredRuns == 1 && mod.gate.Reason() == nxr::GateReason::GameFsUse,
           "затвор открыт запасным путём — по первому обращению игры к nn::fs");
    Expect(mod.redirects == 0,
           "самый первый вызов пропущен в оригинал БЕЗ подмены",
           std::to_string(mod.redirects));
    Expect(mod.report.size() == 1,
           "этот первый вызов не потерян: он в отчёте",
           std::to_string(mod.report.size()));
    Expect(!sdk.aborted, "игра не упала");

    mod.OnOpenFile("app0:/data/second.bin", 1, true);
    mod.OnOpenFile("app0:/data/third.bin", 1, true);
    Expect(mod.redirects == 2,
           "со второго вызова перенаправление работает как прежде",
           std::to_string(mod.redirects));

    /* ---- негативная проба ---- */
    /* Ослабление: затвор по первому OpenFile не открывается никогда. На том же
       входе мод не заработал бы вовсе — проверка «после разрешения работает»
       обязана это поймать. */
    {
        FakeSdk weakSdk;
        Module  weak;
        weak.Init(weakSdk, /* callsSetAllocator = */ true);  /* ОСЛАБЛЕНО: ждём
                                     SetAllocator у игры, которая его не зовёт */
        weak.OnOpenFile("app0:/data/first.bin", 1, true);
        weak.OnOpenFile("app0:/data/second.bin", 1, true);
        Expect(weak.redirects == 0 && !weak.gate.IsOpen(),
               "НЕГАТИВНАЯ ПРОБА: если ждать SetAllocator у игры, которая его "
               "не зовёт, перенаправление не включится никогда");
    }
}

/* ===================================================================== */
/* 2а. Первый файл ЧУЖОГО мода при открытии затвора (баг 2)                */
/* ===================================================================== */

static void TestGateOpenerRetry() {
    std::printf("\n--- первый файл чужого мода (UltraCam для TotK) ---\n");

    /* TotK SetAllocator не зовёт; первым OpenFile UltraCam открывает свой ini,
       которого в romfs игры НЕТ, а на SD (папка files мода) он лежит. */
    FakeSdk sdk;
    Module  mod;
    mod.Init(sdk, /* callsSetAllocator = */ false);
    const Module::Served first =
        mod.OnOpenFileEx("content:/UltraCam/maxlastbreath.ini", 1, /*inRomfs*/ false, /*onSd*/ true);
    Expect(mod.gate.IsOpen() && mod.gate.Reason() == nxr::GateReason::GameFsUse,
           "затвор открыт этим самым вызовом (запасной путь)");
    Expect(first == Module::FromSd,
           "вызов, открывший затвор, НЕ нашёл файл в romfs — и получил его с SD (повтор)",
           std::to_string(static_cast<int>(first)));
    Expect(!sdk.aborted, "игра не упала");

    /* Файл есть в romfs: отдаётся найденное, повтора нет (поведение как раньше). */
    {
        FakeSdk s2;
        Module  m2;
        m2.Init(s2, false);
        const Module::Served r = m2.OnOpenFileEx("app0:/data/first.bin", 1, true, true);
        Expect(r == Module::FromRomfs && m2.redirects == 0,
               "оригинал файл нашёл — отдаём его, повтора с SD нет",
               std::to_string(static_cast<int>(r)));
    }

    /* У игры, которая ЗОВЁТ SetAllocator, затвор после такого вызова закрыт:
       повторять нельзя (это было бы обращение к nn::fs раньше игры). */
    {
        FakeSdk s3;
        Module  m3;
        m3.Init(s3, true);
        const Module::Served r = m3.OnOpenFileEx("rom:/boot.cfg", 1, false, true);
        Expect(r == Module::NotFound && s3.moduleTouches == 0,
               "игра с SetAllocator: до затвора повтора нет и nn::fs не тронут",
               std::to_string(s3.moduleTouches));
        m3.OnSetAllocator();
        Expect(!s3.aborted, "и после её SetAllocator игра жива");
    }

    /* ---- негативная проба ---- */
    /* Ослабление: повтор снят (как было раньше). На ТОМ ЖЕ входе первый
       файл чужого мода теряется — проверка выше обязана это ловить. */
    {
        FakeSdk ws;
        Module  weak;
        weak.Init(ws, false);
        weak.weakNoRetry = true;
        const Module::Served r =
            weak.OnOpenFileEx("content:/UltraCam/maxlastbreath.ini", 1, false, true);
        Expect(r == Module::NotFound,
               "НЕГАТИВНАЯ ПРОБА: без повтора первый файл чужого мода теряется");
    }
}

/* ===================================================================== */
/* 3. Буфер ранних событий: переполнение и обрезка пути                   */
/* ===================================================================== */

static void TestEarlyLog() {
    std::printf("\n--- буфер ранних событий ---\n");

    FakeSdk sdk;
    Module  mod;
    mod.Init(sdk, /* callsSetAllocator = */ true);

    for (int i = 0; i < 10; ++i) {
        char path[32];
        std::snprintf(path, sizeof(path), "rom:/f%d", i);
        mod.OnOpenFile(path, 1, false);
    }

    Expect(mod.early.Count() == 4 && mod.early.Dropped() == 6,
           "в буфер вошло сколько влезло, остальное ЧЕСТНО посчитано",
           std::to_string(mod.early.Count()) + "/" + std::to_string(mod.early.Dropped()));
    Expect(mod.early.CanaryOk(),
           "запись не вышла за край массива (контрольное слово цело)");

    mod.OnSetAllocator();
    Expect(mod.report.size() == 5 && mod.report.back() == "не поместилось: 6",
           "отчёт получил и события, и честную строку о непоместившихся",
           std::to_string(mod.report.size()));

    /* Обрезка длинного пути: буфер заранее замусорен, чтобы ловилось
       отсутствие завершающего нуля. */
    {
        nxr::EarlyLog<2, 16> log;
        std::memset(&log, 'X', sizeof(log));
        log.Init();
        const char* longPath = "rom:/очень-длинный-путь-который-не-влезет.bin";
        log.Add(longPath, 1);
        Expect(log.Count() == 1, "длинный путь записан");
        Expect(std::strlen(log.At(0).m_Path) == 15,
               "длинный путь обрезан по размеру буфера и завершён нулём",
               std::to_string(std::strlen(log.At(0).m_Path)));
        Expect(std::memcmp(log.At(0).m_Path, longPath, 15) == 0,
               "обрезано именно с конца: начало пути сохранено");
        Expect(log.CanaryOk(), "контрольное слово цело и после длинного пути");

        /* ---- негативная проба ---- */
        /* Ослабление: «буфер», который ничего не хранит. На том же входе
           проверка «ранние события не потеряны» обязана провалиться. */
        Module weak;
        FakeSdk weakSdk;
        weak.Init(weakSdk, true);
        weak.weakNoEarlyLog = true;
        weak.OnOpenFile("rom:/boot.cfg", 1, false);
        weak.OnSetAllocator();
        Expect(weak.report.empty(),
               "НЕГАТИВНАЯ ПРОБА: без буфера ранние события теряются молча");
    }
}

/* ===================================================================== */
/* 4. Сам затвор: открывается один раз, причина не переписывается          */
/* ===================================================================== */

static void TestGateOnce() {
    std::printf("\n--- затвор открывается один раз ---\n");

    nxr::FsGate gate;
    Expect(!gate.IsOpen() && gate.Reason() == nxr::GateReason::None,
           "новый затвор закрыт, причины нет");

    Expect(gate.Open(nxr::GateReason::GameFsUse),
           "первый Open() получает право выполнить отложенную работу");
    Expect(gate.IsOpen(), "после Open() затвор открыт");
    Expect(!gate.Open(nxr::GateReason::SetAllocator),
           "второй Open() права не получает");
    Expect(gate.Reason() == nxr::GateReason::GameFsUse,
           "причина ПЕРВОГО открытия не перезаписана");

    Expect(std::strlen(nxr::GateReasonText(nxr::GateReason::SetAllocator)) > 10 &&
           std::strlen(nxr::GateReasonText(nxr::GateReason::GameFsUse)) > 10 &&
           std::strcmp(nxr::GateReasonText(nxr::GateReason::SetAllocator),
                       nxr::GateReasonText(nxr::GateReason::GameFsUse)) != 0,
           "у обеих причин свой человеческий текст для отчёта");

    /* Решение при открытом затворе — всегда обычная работа. */
    Expect(nxr::DecideOpenFile(gate, true) == nxr::OpenFileAction::Normal &&
           nxr::DecideOpenFile(gate, false) == nxr::OpenFileAction::Normal,
           "при открытом затворе модуль работает обычным порядком");

    nxr::FsGate closed;
    Expect(nxr::DecideOpenFile(closed, true) == nxr::OpenFileAction::PassThroughAndWait,
           "закрытый затвор + игра зовёт SetAllocator = только в оригинал, ждём");
    Expect(nxr::DecideOpenFile(closed, false) ==
               nxr::OpenFileAction::PassThroughThenOpenGate,
           "закрытый затвор + игра не зовёт SetAllocator = открыть после возврата");
}

/* ===================================================================== */
/* 5. Монтирование SD — ровно одна попытка (гонка OpenFsGate)              */
/* ===================================================================== */

/* Модель nn::fs::MountSdCard("nxrsd"): первый вызов успешен, каждый следующий —
   0x7802 («имя уже занято»), как у настоящей nn::fs. */
struct FakeMount {
    std::atomic<int> calls { 0 };
    std::uint32_t operator()() {
        return calls.fetch_add(1) == 0 ? 0u : 0x7802u;
    }
};

static void TestSdMountOnce() {
    std::printf("\n--- монтирование SD: одна попытка на два пути ---\n");

    /* а) Затвор открыт; ДО того как OpenFsGate дошёл до монтирования, другой
          поток в хуке OpenFile (EnsureSdMounted) уже смонтировал SD. */
    {
        nxr::SdMount sd;
        FakeMount    m;
        const bool lazy = sd.Ensure([&] { return m(); });
        const std::uint32_t gate = sd.ForGate([&] { return m(); }, [] {});
        Expect(lazy && gate == 0 && m.calls == 1 && sd.State() == nxr::SdState::Mounted,
               "OpenFsGate после ленивого монтирования не монтирует второй раз (нет 0x7802)",
               "вызовов MountSdCard " + std::to_string(m.calls.load()) +
               ", итог затвора " + std::to_string(gate));
    }

    /* б) OpenFsGate приходит, пока другой поток ВНУТРИ MountSdCard: ждёт исхода. */
    {
        nxr::SdMount      sd;
        FakeMount         m;
        std::atomic<bool> gateWaiting { false };
        std::atomic<bool> lazyResult  { false };
        std::thread lazy([&] {
            lazyResult = sd.Ensure([&] {
                /* Держим попытку, пока поток затвора не начнёт ждать (с пределом,
                   чтобы сломанная логика не повесила тест). */
                for (int i = 0; i < 2000 && !gateWaiting.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return m();
            });
        });
        while (sd.State() == nxr::SdState::NotTried) {
            std::this_thread::yield();
        }
        const std::uint32_t gate = sd.ForGate([&] { return m(); },
                                              [&] { gateWaiting = true; std::this_thread::yield(); });
        lazy.join();
        Expect(gate == 0 && lazyResult && m.calls == 1 && sd.State() == nxr::SdState::Mounted,
               "OpenFsGate во время чужой попытки ждёт её исхода и не монтирует сам",
               "вызовов MountSdCard " + std::to_string(m.calls.load()) +
               ", итог затвора " + std::to_string(gate));
        Expect(gateWaiting.load(), "поток затвора действительно ждал (сценарий воспроизведён)");
    }

    /* в) Вложенный OpenFile из самой MountSdCard: не ждёт и не монтирует. */
    {
        nxr::SdMount sd;
        FakeMount    m;
        bool nested = true;
        const std::uint32_t gate = sd.ForGate([&] {
            nested = sd.Ensure([&] { return m(); });
            return m();
        }, [] {});
        Expect(gate == 0 && !nested && m.calls == 1,
               "вложенный OpenFile во время монтирования проходит без подмены, без рекурсии",
               std::to_string(m.calls.load()));
    }

    /* г) Отказ: вторая попытка не делается, итог отказа виден обоим путям. */
    {
        nxr::SdMount sd;
        int calls = 0;
        const std::uint32_t gate = sd.ForGate([&] { ++calls; return 0xDEAD0003u; }, [] {});
        const bool lazy = sd.Ensure([&] { ++calls; return 0u; });
        const std::uint32_t again = sd.ForGate([&] { ++calls; return 0u; }, [] {});
        Expect(gate == 0xDEAD0003u && !lazy && again == 0xDEAD0003u && calls == 1 &&
                   sd.State() == nxr::SdState::Failed,
               "после отказа повторных попыток нет, код отказа сохранён",
               std::to_string(calls));
    }

    /* ---- негативная проба ---- */
    /* Ослабление (как было раньше): путь затвора монтирует без compare-exchange.
       На входе а) это второй MountSdCard, 0x7802 и Failed до конца сессии. */
    {
        nxr::SdMount sd;
        FakeMount    m;
        sd.Ensure([&] { return m(); });
        const std::uint32_t weakGate = m();      /* OpenFsGate без CAS: монтирует сам */
        Expect(m.calls == 2 && weakGate == 0x7802u,
               "НЕГАТИВНАЯ ПРОБА: без compare-exchange в пути затвора SD монтируется "
               "дважды и второй вызов получает 0x7802");
    }
}

/* ===================================================================== */

int main() {
    std::printf("=== хост-тест затвора файловой системы (src/nxr_fsgate.hpp) ===\n");
    TestGameWithSetAllocator();
    TestGameWithoutSetAllocator();
    TestGateOpenerRetry();
    TestEarlyLog();
    TestGateOnce();
    TestSdMountOnce();

    std::printf("\nвыполнено проверок: %d, красных: %d\n", g_Checks, g_Failed);

    /* ЗАТВОР НУЛЯ: тест, который ничего не проверил, зелёным быть не может. */
    if (g_Checks < 37) {
        std::printf("КРАСНО: проверок выполнено слишком мало (%d) — "
                    "тест не мог ничего доказать\n", g_Checks);
        return 1;
    }
    if (g_Failed != 0) {
        std::printf("ТЕСТ КРАСНЫЙ\n");
        return 1;
    }
    std::printf("ТЕСТ ЗЕЛЁНЫЙ\n");
    return 0;
}
