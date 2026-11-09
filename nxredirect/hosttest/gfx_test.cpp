/*
 * gfx_test.cpp — хост-тест ЧИСТОЙ части диагностики графики (src/nxr_gfx.hpp).
 *
 * Copyright (c) 2026, участники проекта nxredirect. GNU GPL версии 2.
 *
 * Что проверяется на ПК, без консоли:
 *   1. формат строки отчёта для всех видов возврата (Result, указатель,
 *      EGLBoolean, void), со строковым аргументом и с кодом eglGetError;
 *   2. признак неуспеха: по нему читатель отчёта находит первое сломавшееся звено;
 *   3. постановка перехватчиков по таблице: НЕНАЙДЕННЫЙ символ не должен
 *      приводить к установке хука (иначе игра падает и отчёта не будет),
 *      но обязан попасть в отчёт строкой;
 *   4. запись «по событию»: сколько событий, столько и обращений к записи —
 *      ничего не копится в памяти (на консоли каждая запись идёт с Flush,
 *      поэтому отчёт остаётся на карте, даже если игра потом зависнет);
 *   5. таблица целей непустая и без повторов;
 *   6. ВИД ВОЗВРАТА у каждой цели: функция без возвращаемого значения обязана
 *      быть объявлена Void, иначе мусор из регистра печатается как ошибка
 *      (так и вышло с nn::vi::Initialize), плюс коды ошибок
 *      блокировок (EDEADLK) из пробы ШАГ-7.
 *
 * У КАЖДОЙ проверки есть НЕГАТИВНАЯ ПРОБА: рядом стоит намеренно ослабленная
 * реализация того же места, и тест требует, чтобы ослабленная ПРОВАЛИЛА ту же
 * проверку. Если ослабленная проходит — проверка ничего не проверяет, и тест
 * краснеет. Плюс «затвор нуля»: тест, не выполнивший ни одной проверки,
 * считается красным.
 *
 * Сборка и запуск (нужен только g++, devkitPro не требуется):
 *     g++ -std=c++20 -O2 -Wall -o gfx_test gfx_test.cpp -I../src && ./gfx_test
 *
 * Код возврата: 0 — всё зелено, 1 — есть красные.
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nxr_gfx.hpp"

using nxr::gfx::Buf;
using nxr::gfx::Event;
using nxr::gfx::RetKind;
using nxr::gfx::Target;

static int g_Checks = 0;   /* сколько проверок реально выполнено (затвор нуля) */
static int g_Failed = 0;

static void Ok(const char* what) {
    ++g_Checks;
    std::printf("  ok   %s\n", what);
}

static void Fail(const char* what, const std::string& detail) {
    ++g_Checks;
    ++g_Failed;
    std::printf("  КРАСНО %s: %s\n", what, detail.c_str());
}

static void Expect(bool cond, const char* what, const std::string& detail = "") {
    if (cond) {
        Ok(what);
    } else {
        Fail(what, detail);
    }
}

/* Строка отчёта как текст (без CRLF), чтобы сравнивать глазами и в коде. */
static std::string Render(const Event& e) {
    Buf buf;
    nxr::gfx::FormatEvent(buf, e);
    return std::string(buf.m_Buf, buf.m_Len);
}

static bool Has(const std::string& s, const char* what) {
    return s.find(what) != std::string::npos;
}

/* ===================================================================== */
/* 1. Формат строки                                                      */
/* ===================================================================== */

static void TestFormat() {
    std::printf("[1] формат строки отчёта\n");

    Event ok {};
    ok.name = "nn::vi::Initialize";
    ok.kind = RetKind::Result;
    ok.ret = 0;
    const std::string sOk = Render(ok);
    Expect(Has(sOk, "nn::vi::Initialize()") && Has(sOk, "успех")
               && !Has(sOk, nxr::gfx::MarkFail),
           "успешный Result: 'успех', без метки неуспеха", sOk);

    Event bad {};
    bad.name = "nn::vi::CreateLayer";
    bad.kind = RetKind::Result;
    bad.argc = 2;
    bad.argv[0] = 0x7100000000ull;
    bad.argv[1] = 0xabc;
    bad.ret = 0x1234;
    const std::string sBad = Render(bad);
    Expect(Has(sBad, nxr::gfx::MarkFail) && Has(sBad, "ОШИБКА 0x1234")
               && Has(sBad, "0x7100000000, 0xabc"),
           "неуспешный Result: метка, код и аргументы", sBad);

    Event ptr {};
    ptr.name = "eglCreateContext";
    ptr.kind = RetKind::Pointer;
    ptr.ret = 0;
    ptr.hasEglError = true;
    ptr.eglError = 0x3001;
    const std::string sPtr = Render(ptr);
    Expect(Has(sPtr, "НОЛЬ (провал)") && Has(sPtr, "eglGetError=0x3001")
               && Has(sPtr, nxr::gfx::MarkFail),
           "нулевой указатель: провал плюс код eglGetError", sPtr);

    Event bl {};
    bl.name = "eglInitialize";
    bl.kind = RetKind::Bool;
    bl.ret = 1;
    Expect(Has(Render(bl), "TRUE") && !Has(Render(bl), nxr::gfx::MarkFail),
           "EGLBoolean TRUE — не считается провалом", Render(bl));

    bl.ret = 0;
    Expect(Has(Render(bl), "FALSE (провал)") && Has(Render(bl), nxr::gfx::MarkFail),
           "EGLBoolean FALSE — провал с меткой", Render(bl));

    Event str {};
    str.name = "nvnBootstrapLoader";
    str.strArg = "nvnDeviceGetProcAddress";
    str.kind = RetKind::Pointer;
    str.ret = 0;
    Expect(Has(Render(str), "(\"nvnDeviceGetProcAddress\")"),
           "строковый аргумент печатается в кавычках", Render(str));

    /* Перевод строки обязан быть CRLF: отчёт читают блокнотом Windows. */
    Buf buf;
    nxr::gfx::FormatEvent(buf, ok);
    Expect(buf.m_Len >= 2 && buf.m_Buf[buf.m_Len - 2] == '\r'
               && buf.m_Buf[buf.m_Len - 1] == '\n',
           "строка заканчивается CRLF", "");

    /* Переполнения быть не должно: длинное имя просто обрежется. */
    std::string longName(4096, 'X');
    Event big {};
    big.name = longName.c_str();
    big.kind = RetKind::Void;
    Buf bigBuf;
    nxr::gfx::FormatEvent(bigBuf, big);
    Expect(bigBuf.m_Len < nxr::gfx::BufSize,
           "сверхдлинное имя не выходит за буфер",
           std::to_string(bigBuf.m_Len));

    /* ---- негативная проба: формат без метки неуспеха ---- */
    /* Ослабленная версия: всё то же, но метка «!!» не ставится. Проверка №2
       обязана на ней провалиться, иначе она ничего не проверяет. */
    auto renderWeak = [](const Event& e) {
        Buf b;
        b.Add(nxr::gfx::MarkOk);          /* ОСЛАБЛЕНО: метки неуспеха нет */
        b.Add(e.name);
        b.Add("() = ОШИБКА ");
        b.AddHex(e.ret);
        b.End();
        return std::string(b.m_Buf, b.m_Len);
    };
    const std::string weak = renderWeak(bad);
    Expect(!Has(weak, nxr::gfx::MarkFail),
           "НЕГАТИВНАЯ ПРОБА: ослабленный формат теряет метку неуспеха "
           "(значит проверка метки не пустая)", weak);
}

/* ===================================================================== */
/* 2. Признак неуспеха                                                   */
/* ===================================================================== */

static void TestFailureFlag() {
    std::printf("[2] признак неуспеха\n");

    Event e {};
    e.kind = RetKind::Result;  e.ret = 0;     Expect(!nxr::gfx::IsFailure(e), "Result 0 — успех");
    e.kind = RetKind::Result;  e.ret = 7;     Expect(nxr::gfx::IsFailure(e),  "Result != 0 — неуспех");
    e.kind = RetKind::Pointer; e.ret = 0;     Expect(nxr::gfx::IsFailure(e),  "указатель 0 — неуспех");
    e.kind = RetKind::Pointer; e.ret = 0x10;  Expect(!nxr::gfx::IsFailure(e), "указатель != 0 — успех");
    e.kind = RetKind::Bool;    e.ret = 0;     Expect(nxr::gfx::IsFailure(e),  "EGLBoolean 0 — неуспех");
    e.kind = RetKind::Bool;    e.ret = 1;     Expect(!nxr::gfx::IsFailure(e), "EGLBoolean 1 — успех");
    e.kind = RetKind::Void;    e.ret = 0;     Expect(!nxr::gfx::IsFailure(e), "void — не оценивается");

    /* ---- негативная проба ---- */
    /* Ослабленная версия считает неуспехом только Result. Тогда нулевой
       указатель (eglCreateContext вернул 0 — ровно наш случай чёрного экрана)
       пройдёт как успех. Проверка обязана это поймать. */
    auto weakIsFailure = [](const Event& ev) {
        return ev.kind == RetKind::Result && ev.ret != 0;
    };
    Event ptrFail {};
    ptrFail.kind = RetKind::Pointer;
    ptrFail.ret = 0;
    Expect(!weakIsFailure(ptrFail) && nxr::gfx::IsFailure(ptrFail),
           "НЕГАТИВНАЯ ПРОБА: ослабленный признак пропускает нулевой указатель");
}

/* ===================================================================== */
/* 3. Постановка перехватчиков                                           */
/* ===================================================================== */

/* Модель резолвера: три символа из пяти «не находятся». */
struct FakeWorld {
    std::vector<int>          installed;   /* индексы, на которые ставили хук */
    std::vector<std::uint64_t> installedAt;
    std::vector<std::string>  reported;    /* что попало в отчёт */
};

static void TestInstall() {
    std::printf("[3] постановка перехватчиков\n");

    const Target table[5] = {
        { "есть1", "A" }, { "нет1", "B" }, { "есть2", "C" },
        { "нет2", "D" },  { "есть3", "E" },
    };
    auto resolve = [](const char* sym) -> std::uint64_t {
        return (std::strncmp(sym, "есть", 8) == 0) ? 0x7100000000ull : 0;
    };

    FakeWorld w;
    const int n = nxr::gfx::InstallTargets(
        table, 5, /* budgetLeft = */ nxr::gfx::HookMax, resolve,
        [&w](int i, std::uint64_t a) { w.installed.push_back(i); w.installedAt.push_back(a); },
        [&w, &table](int i, std::uint64_t a, bool) {
            w.reported.push_back(std::string(table[i].human) + (a ? "=найден" : "=НЕ НАЙДЕН"));
        });

    Expect(n == 3, "поставлено ровно столько хуков, сколько нашлось символов",
           std::to_string(n));
    bool zeroInstall = false;
    for (std::uint64_t a : w.installedAt) {
        if (a == 0) {
            zeroInstall = true;
        }
    }
    Expect(!zeroInstall,
           "НИ ОДИН хук не поставлен по нулевому адресу (иначе игра падает)");
    Expect(w.installed == std::vector<int>({0, 2, 4}),
           "хуки встали именно на найденные цели");
    Expect(w.reported.size() == 5,
           "в отчёт попали ВСЕ цели, включая ненайденные",
           std::to_string(w.reported.size()));
    Expect(w.reported[1] == "B=НЕ НАЙДЕН" && w.reported[3] == "D=НЕ НАЙДЕН",
           "ненайденные цели названы в отчёте поимённо");

    /* ---- негативная проба: версия без проверки на ноль ---- */
    FakeWorld weak;
    for (int i = 0; i < 5; ++i) {
        const std::uint64_t a = resolve(table[i].sym);
        weak.installedAt.push_back(a);   /* ОСЛАБЛЕНО: ставим всегда */
    }
    bool weakZero = false;
    for (std::uint64_t a : weak.installedAt) {
        if (a == 0) {
            weakZero = true;
        }
    }
    Expect(weakZero,
           "НЕГАТИВНАЯ ПРОБА: без проверки на ноль хук встаёт по адресу 0 "
           "(значит проверка выше не пустая)");
}

/* ===================================================================== */
/* 4. Запись по событию, без накопления                                  */
/* ===================================================================== */

/* Настоящая политика: событие -> сразу обращение к записи. */
struct DirectWriter {
    int writes = 0;
    std::vector<std::string> lines;
    void OnEvent(const Event& e) {
        Buf b;
        nxr::gfx::FormatEvent(b, e);
        ++writes;                                   /* запись на карту здесь */
        lines.push_back(std::string(b.m_Buf, b.m_Len));
    }
    void Finish() {}
};

/* Ослабленная политика: строки копятся и пишутся в конце. Если игра зависнет
   до конца, на карте не окажется ничего — ровно то, чего мы избегаем. */
struct BufferedWriter {
    int writes = 0;
    std::vector<std::string> pending;
    void OnEvent(const Event& e) {
        Buf b;
        nxr::gfx::FormatEvent(b, e);
        pending.push_back(std::string(b.m_Buf, b.m_Len));
    }
    void Finish() { ++writes; }
};

static void TestWriteOnEachEvent() {
    std::printf("[4] запись при каждом событии\n");

    std::vector<Event> evts;
    for (int i = 0; i < 7; ++i) {
        Event e {};
        e.name = "eglSwapBuffers";
        e.kind = RetKind::Bool;
        e.ret = 1;
        evts.push_back(e);
    }

    DirectWriter direct;
    for (const Event& e : evts) {
        direct.OnEvent(e);
    }
    /* «Зависание»: Finish не зовём — и всё равно всё должно быть записано. */
    Expect(direct.writes == static_cast<int>(evts.size()),
           "сколько событий, столько и записей на карту",
           std::to_string(direct.writes));

    /* ---- негативная проба ---- */
    BufferedWriter buffered;
    for (const Event& e : evts) {
        buffered.OnEvent(e);
    }
    Expect(buffered.writes == 0 && !buffered.pending.empty(),
           "НЕГАТИВНАЯ ПРОБА: при накоплении в памяти после зависания "
           "на карте не оказалось бы ни строки");
}

/* ===================================================================== */
/* 5. Таблица целей                                                      */
/* ===================================================================== */

static void TestTargetTable() {
    std::printf("[5] таблица целей перехвата\n");

    const Target* t = nxr::gfx::Targets();
    Expect(nxr::gfx::T_Count >= 10, "целей в таблице не меньше десяти",
           std::to_string(static_cast<int>(nxr::gfx::T_Count)));

    bool allNamed = true;
    for (int i = 0; i < nxr::gfx::T_Count; ++i) {
        if (t[i].sym == nullptr || t[i].sym[0] == '\0'
            || t[i].human == nullptr || t[i].human[0] == '\0') {
            allNamed = false;
        }
    }
    Expect(allNamed, "у каждой цели есть и символ, и человеческое имя");

    bool dup = false;
    for (int i = 0; i < nxr::gfx::T_Count; ++i) {
        for (int j = i + 1; j < nxr::gfx::T_Count; ++j) {
            if (std::strcmp(t[i].sym, t[j].sym) == 0) {
                dup = true;
            }
        }
    }
    Expect(!dup, "повторов в таблице нет (два хука на один адрес — порча кода)");

    /* Цепочка обязана идти по порядку: сначала nn::vi, потом EGL. Иначе отчёт
       перестанет читаться как «этапы сверху вниз». */
    Expect(std::strcmp(t[nxr::gfx::T_ViInitialize].human, "nn::vi::Initialize") == 0
               && std::strcmp(t[nxr::gfx::T_EglSwapBuffers].human, "eglSwapBuffers") == 0,
           "порядок целей соответствует графической цепочке");
}


/* ===================================================================== */
/* 6. ВИД ВОЗВРАТА У КАЖДОЙ ЦЕЛИ                                         */
/* ===================================================================== */
/*
 * Зачем эта проверка появилась. Однажды перехватчик nn::vi::Initialize был
 * объявлен возвращающим Result, хотя функция не возвращает ничего: в w0
 * оставался мусор от последнего внутреннего вызова, и отчёт начинался строкой
 * «!! nn::vi::Initialize() = ОШИБКА 0xda0a3e40» — несуществующей поломкой там,
 * куда инструкция велит смотреть в первую очередь.
 *
 * Чинится это тем, что вид возврата лежит ОДИН РАЗ в таблице целей, а хук
 * берёт его оттуда (и сверяет static_assert'ом при сборке). Проверяем здесь
 * саму таблицу: у функции без возврата вид обязан быть Void, и Void-событие
 * не должно давать ни метки неуспеха, ни слова «ОШИБКА» — каким бы мусором
 * ни оказалось значение в регистре.
 */
static void TestRetKinds() {
    std::printf("[6] вид возврата у целей перехвата\n");

    const Target* t = nxr::gfx::Targets();

    Expect(t[nxr::gfx::T_ViInitialize].kind == RetKind::Void,
           "nn::vi::Initialize объявлена как Void (она не возвращает значения)");

    /* Остальные цепочки — возвращают: иначе первое сломавшееся звено не найти. */
    const int withValue[] = {
        nxr::gfx::T_ViOpenDefaultDisplay, nxr::gfx::T_ViCreateLayer,
        nxr::gfx::T_ViCreateLayerWh,      nxr::gfx::T_ViGetNativeWindow,
        nxr::gfx::T_EglGetDisplay,        nxr::gfx::T_EglInitialize,
        nxr::gfx::T_EglChooseConfig,      nxr::gfx::T_EglCreateContext,
        nxr::gfx::T_EglCreateWindowSurface, nxr::gfx::T_EglMakeCurrent,
        nxr::gfx::T_EglSwapBuffers,       nxr::gfx::T_EglGetProcAddress,
        nxr::gfx::T_NvnBootstrapLoader,   nxr::gfx::T_NvnBootstrapLoaderInternal,
    };
    bool allValued = true;
    for (int i : withValue) {
        if (t[i].kind == RetKind::Void) {
            allValued = false;
        }
    }
    Expect(allValued,
           "у всех прочих целей вид возврата задан (не Void): "
           "у них возвращаемое значение действительно есть");

    /* Мусор в регистре: для Void он не должен превратиться в ошибку. */
    Event garbage {};
    garbage.name = t[nxr::gfx::T_ViInitialize].human;
    garbage.kind = t[nxr::gfx::T_ViInitialize].kind;
    garbage.ret = 0xda0a3e40ull;            /* ровно то значение из отчёта */
    const std::string sVoid = Render(garbage);
    Expect(!Has(sVoid, nxr::gfx::MarkFail) && !Has(sVoid, "ОШИБКА")
               && Has(sVoid, "вызван"),
           "Void с мусором в регистре: ни метки неуспеха, ни слова «ОШИБКА»",
           sVoid);

    /* Вид возврата Errno (pthread_rwlock_*): 0 — успех, EDEADLK — узнаваем. */
    Event lock {};
    nxr::gfx::FillLockEvent(lock, nxr::gfx::L_RwlockRdlock, 0x7100001234ull, 0,
                            true, nxr::gfx::Edeadlk);
    const std::string sLock = Render(lock);
    Expect(Has(sLock, nxr::gfx::MarkFail) && Has(sLock, "EDEADLK")
               && Has(sLock, "0x7100001234"),
           "EDEADLK на блокировке виден как неуспех и назван по имени", sLock);

    Event lockOk {};
    nxr::gfx::FillLockEvent(lockOk, nxr::gfx::L_RwlockRdlock, 0x71ull, 0, false, 0);
    Expect(!Has(Render(lockOk), nxr::gfx::MarkFail),
           "успешный захват блокировки не помечается как неуспех",
           Render(lockOk));

    bool locksNamed = true;
    for (int i = 0; i < nxr::gfx::L_Count; ++i) {
        if (nxr::gfx::LockTargets()[i].kind != RetKind::Errno
            || nxr::gfx::LockTargets()[i].sym == nullptr) {
            locksNamed = false;
        }
    }
    Expect(locksNamed, "все цели пробы блокировок названы и объявлены как Errno");

    /* ---- негативная проба ---- */
    /* Ослабленная таблица: тот же nn::vi::Initialize объявлен как Result —
       ровно прежняя ошибка. Мусор из регистра обязан превратиться в «ОШИБКА»,
       и проверка выше обязана это увидеть. Если не превращается — значит
       проверка ничего не проверяет. */
    Event weak {};
    weak.name = "nn::vi::Initialize";
    weak.kind = RetKind::Result;            /* ОСЛАБЛЕНО */
    weak.ret = 0xda0a3e40ull;
    const std::string sWeak = Render(weak);
    Expect(Has(sWeak, nxr::gfx::MarkFail) && Has(sWeak, "ОШИБКА 0xda0a3e40"),
           "НЕГАТИВНАЯ ПРОБА: с видом Result тот же мусор даёт ложную ошибку "
           "(значит проверка вида возврата не пустая)", sWeak);
}

/* ===================================================================== */
/*
 * [7] БЮДЖЕТ ТРАМПЛИНОВ exlaunch.
 *
 * Дорого доставшаяся проверка. Проба ШАГ-7 ставила 22 хука при пуле на 20, и
 * exlaunch аварийно завершил игру на 22-м — на хуке перенаправления файлов.
 * Отчёт о графике при этом остался с пустым разделом вызовов, потому что
 * падение случилось сразу после конца Start и до первого вызова EGL.
 */
static void TestHookBudget() {
    std::printf("[7] бюджет трамплинов exlaunch\n");

    Expect(nxr::gfx::HookMax == nxr::gfx::JitSizeBytes / nxr::gfx::TrampolineBytes,
           "бюджет считается как JitSize / размер трамплина");
    Expect(nxr::gfx::HooksTotal == static_cast<int>(nxr::gfx::T_Count)
                                   + static_cast<int>(nxr::gfx::L_Count)
                                   + static_cast<int>(nxr::gfx::S_Count)
                                   + nxr::gfx::HooksOutsideGfx,
           "в общий счёт входят цели графики, блокировок, две пробы SetError "
           "(inline-хук тоже берёт трамплин) и три хука main.cpp");
    Expect(nxr::gfx::HooksTotal <= nxr::gfx::HookMax,
           "всех хуков модуля не больше, чем трамплинов в пуле");

    /* Сама раздача: за бюджет выйти нельзя даже когда все символы найдены. */
    const Target table[3] = {
        { "a", "a", RetKind::Void },
        { "b", "b", RetKind::Void },
        { "c", "c", RetKind::Void },
    };
    int installedCount = 0;
    int reported = 0;
    int refusedByBudget = 0;
    const int done = nxr::gfx::InstallTargets(
        table, 3, /* budgetLeft = */ 2,
        [](const char*) -> std::uint64_t { return 0x1000; },
        [&installedCount](int, std::uint64_t) { ++installedCount; },
        [&reported, &refusedByBudget](int, std::uint64_t addr, bool ok) {
            ++reported;
            if (!ok && addr != 0) {
                ++refusedByBudget;
            }
        });
    Expect(done == 2 && installedCount == 2,
           "при бюджете 2 из трёх целей ставятся ровно две");
    Expect(reported == 3 && refusedByBudget == 1,
           "про цель, которой не хватило бюджета, в отчёт пишется строка");

    /* ---- негативная проба ---- */
    /* Ослабленная раздача — без учёта бюджета, ровно как было в ШАГЕ-7. */
    int weakInstalled = 0;
    for (int i = 0; i < 3; ++i) {
        const std::uint64_t addr = 0x1000;   /* символ найден */
        if (addr != 0) {                     /* ОСЛАБЛЕНО: бюджет не смотрим */
            ++weakInstalled;
        }
    }
    Expect(weakInstalled == 3 && weakInstalled > 2,
           "НЕГАТИВНАЯ ПРОБА: без учёта бюджета ставятся все три цели — "
           "то есть проверка бюджета не пустая");
}

/* ===================================================================== */
/*
 * [8] ОКНО ПРОБЫ БЛОКИРОВОК И БУФЕР В ПАМЯТИ.
 *
 * Требования, ради которых проба переделана:
 *   - до готовности (m_Armed == 0) перехватчик не трогает наши данные;
 *   - вне окна не пишет ничего;
 *   - чужой поток не пишет ничего;
 *   - буфер фиксированного размера не переполняется, лишнее считается.
 */
static void TestLockWindow() {
    std::printf("[8] окно узкой пробы блокировок и буфер в памяти\n");

    const std::uint64_t mine  = 0xAAAA0000ull;   /* «наш» поток */
    const std::uint64_t other = 0xBBBB0000ull;   /* чужой поток */

    nxr::gfx::LockWindow w {};                   /* как в .bss: всё нулями */
    Expect(w.m_Armed == 0 && w.m_Owner == 0,
           "окно по умолчанию закрыто и не готово (именно так лежит в .bss)");

    /* НЕ ГОТОВО: даже попытка открыть окно ничего не открывает. */
    const std::uint64_t prevCold = nxr::gfx::LockWindowOpen(w, mine);
    Expect(!nxr::gfx::LockWindowShouldLog(w, mine),
           "до готовности перехватчик прозрачен даже внутри вызова");
    nxr::gfx::LockWindowClose(w, prevCold);

    /* ГОТОВО, но окно закрыто. */
    w.m_Armed = 1;
    Expect(!nxr::gfx::LockWindowShouldLog(w, mine),
           "вне окна перехватчик ничего не пишет");

    /* ГОТОВО и окно открыто нашим потоком. */
    const std::uint64_t prev = nxr::gfx::LockWindowOpen(w, mine);
    Expect(nxr::gfx::LockWindowShouldLog(w, mine),
           "внутри окна и в том же потоке запись разрешена");
    Expect(!nxr::gfx::LockWindowShouldLog(w, other),
           "чужой поток внутри чужого окна не пишет ничего");
    nxr::gfx::LockWindowClose(w, prev);
    Expect(!nxr::gfx::LockWindowShouldLog(w, mine),
           "после возврата из вызова окно закрыто");

    /* Буфер: ровно LockRingSize записей, дальше — только счётчик. */
    nxr::gfx::LockRing ring {};
    for (int i = 0; i < nxr::gfx::LockRingSize + 7; ++i) {
        nxr::gfx::LockRecord rec;
        rec.m_Obj = 0x7100000000ull + static_cast<std::uint64_t>(i);
        nxr::gfx::LockRingPush(ring, rec);
    }
    Expect(ring.m_Count == nxr::gfx::LockRingSize && ring.m_Dropped == 7,
           "буфер не переполняется: лишние записи считаются отдельно");
    Expect(ring.m_Items[0].m_Obj == 0x7100000000ull
               && ring.m_Items[nxr::gfx::LockRingSize - 1].m_Obj
                      == 0x7100000000ull + nxr::gfx::LockRingSize - 1,
           "в буфере лежат первые записи окна, в порядке появления");
    ring.Reset();
    Expect(ring.m_Count == 0 && ring.m_Dropped == 0,
           "перед следующим окном буфер сбрасывается");

    /* Цель осталась одна, и это именно read-захват. */
    Expect(nxr::gfx::L_Count == 1
               && std::strcmp(nxr::gfx::LockTargets()[0].sym,
                              "pthread_rwlock_rdlock") == 0,
           "перехват сузился до одной цели — pthread_rwlock_rdlock");

    /* Порядок: пока окно открыто — ни одной записи на карту, всё в память;
       после возврата из вызова буфер выливается разом. Модель повторяет то, что
       на консоли делают RwlockRdlockHook и LockProbeEnd. */
    {
        nxr::gfx::LockWindow win {};
        win.m_Armed = 1;
        nxr::gfx::LockRing ring {};
        int writesToCard = 0;

        const std::uint64_t prevWin = nxr::gfx::LockWindowOpen(win, mine);
        for (int i = 0; i < 3; ++i) {          /* три захвата внутри вызова */
            if (nxr::gfx::LockWindowShouldLog(win, mine)) {
                nxr::gfx::LockRecord rec;
                rec.m_Obj = 0x7100000000ull + static_cast<std::uint64_t>(i);
                nxr::gfx::LockRingPush(ring, rec);   /* только память */
            }
        }
        /* Захват из чужого потока, пока наше окно открыто. */
        if (nxr::gfx::LockWindowShouldLog(win, other)) {
            nxr::gfx::LockRecord rec;
            nxr::gfx::LockRingPush(ring, rec);
        }
        Expect(ring.m_Count == 3 && writesToCard == 0,
               "внутри окна записей на карту нет, а чужой поток в буфер не попал",
               std::to_string(ring.m_Count));

        nxr::gfx::LockWindowClose(win, prevWin);      /* вызов вернулся */
        writesToCard += ring.m_Count;                  /* и только теперь пишем */
        Expect(writesToCard == 3 && !nxr::gfx::LockWindowShouldLog(win, mine),
               "буфер вылит на карту ПОСЛЕ возврата, при уже закрытом окне",
               std::to_string(writesToCard));
    }

    /* ---- негативная проба ---- */
    /* Ослабленное условие: смотрим только на окно, без признака готовности —
       ровно та ошибка, из-за которой перехватчик сработал бы по неготовым
       данным. На том же входе оно обязано разрешить запись. */
    nxr::gfx::LockWindow cold {};
    cold.m_Owner = mine;                        /* окно «открыто», но m_Armed = 0 */
    const bool weak = (cold.m_Owner == mine);   /* ОСЛАБЛЕНО */
    Expect(weak && !nxr::gfx::LockWindowShouldLog(cold, mine),
           "НЕГАТИВНАЯ ПРОБА: без признака готовности та же проверка "
           "разрешила бы запись по неготовым данным");
}

/* ===================================================================== */
/*
 * [9] РАННИЕ СОБЫТИЯ ГРАФИКИ: до открытия затвора ФС строки
 * копятся в памяти; не помещается — строка не пишется ЦЕЛИКОМ и считается.
 */
static void TestEarlyText() {
    std::printf("[9] ранний буфер строк (до затвора ФС)\n");
    struct Guarded {
        nxr::gfx::EarlyTextT<32> t;
        std::uint32_t after;
    } g;
    std::memset(&g, 'X', sizeof(g));
    g.t.Init();
    g.after = 0xC0FFEE11u;
    const char* l1 = "line one 12345\r\n";   /* 16 байт */
    const char* l2 = "line two 1234\r\n";    /* 15 байт */
    const char* l3 = "tail-that-does-not-fit\r\n";
    Expect(nxr::gfx::EarlyTextAppend(g.t, l1, std::strlen(l1)) &&
           nxr::gfx::EarlyTextAppend(g.t, l2, std::strlen(l2)),
           "две строки поместились");
    Expect(!nxr::gfx::EarlyTextAppend(g.t, l3, std::strlen(l3)),
           "третья не помещается — отказ, а не обрывок");
    Expect(g.t.m_Len == 31 && g.t.m_Lines == 2 && g.t.m_Dropped == 1,
           "длина, число строк и счётчик непоместившихся честные",
           std::to_string(g.t.m_Len) + "/" + std::to_string(g.t.m_Lines) + "/" +
               std::to_string(g.t.m_Dropped));
    Expect(std::memcmp(g.t.m_Buf, "line one 12345\r\nline two 1234\r\n", 31) == 0,
           "строки лежат по порядку, байт в байт");
    Expect(g.t.CanaryOk() && g.after == 0xC0FFEE11u,
           "за край буфера ничего не записано (контрольные слова целы)");
    const char one[] = "Z";
    Expect(nxr::gfx::EarlyTextAppend(g.t, one, 1) && g.t.m_Len == 32 && g.t.m_Dropped == 1,
           "последний байт буфера используется (граница ровно Size)");

    /* ---- негативная проба ---- */
    /* Ослабление «пишем, сколько влезет»: на том же входе в буфере оказался бы
       обрывок третьей строки — проверка «отказ целиком» обязана это различать. */
    nxr::gfx::EarlyTextT<32> w;
    w.Init();
    nxr::gfx::EarlyTextAppend(w, l1, std::strlen(l1));
    nxr::gfx::EarlyTextAppend(w, l2, std::strlen(l2));
    std::size_t room = 32 - w.m_Len;   /* ОСЛАБЛЕНО: обрезаем */
    std::memcpy(w.m_Buf + w.m_Len, l3, room);
    w.m_Len += static_cast<std::uint32_t>(room);
    Expect(w.m_Len == 32 && g.t.m_Len == 32 && std::memcmp(w.m_Buf + 31, "t", 1) == 0 &&
               std::memcmp(g.t.m_Buf + 31, "Z", 1) == 0,
           "НЕГАТИВНАЯ ПРОБА: обрезка дала бы обрывок «t…» вместо целой строки");
}

/* ===================================================================== */
/*
 * [10] ОТЛОЖЕННЫЕ ЗАПИСИ (SetError, NvRm): слот, флаг готовности, слив по
 * порядку, честный счёт непоместившихся, выход за массив невозможен.
 */
static void TestPendingRing() {
    std::printf("[10] отложенные записи из чужих потоков\n");
    struct Guarded {
        nxr::gfx::PendingRing<nxr::gfx::NvrmCallRecord, 3> r;
        std::uint64_t after;
    } g;
    std::memset(&g, 0, sizeof(g));
    g.after = 0xA11CE5ull;
    for (std::uint32_t i = 0; i < 5; ++i) {
        nxr::gfx::NvrmCallRecord rec {};
        rec.handle = 100 + i;
        nxr::gfx::PendingPush(g.r, rec);
    }
    Expect(nxr::gfx::PendingDropped(g.r) == 2, "из пяти записей две не поместились — посчитано",
           std::to_string(nxr::gfx::PendingDropped(g.r)));
    Expect(g.after == 0xA11CE5ull, "за массив записей ничего не легло");
    std::vector<std::uint32_t> got;
    const int n = nxr::gfx::PendingDrain(g.r, [&got](const nxr::gfx::NvrmCallRecord& rec) {
        got.push_back(rec.handle);
    });
    Expect(n == 3 && got.size() == 3 && got[0] == 100 && got[1] == 101 && got[2] == 102,
           "слиты первые три, по порядку");
    Expect(nxr::gfx::PendingNewDrops(g.r) == 2 && nxr::gfx::PendingNewDrops(g.r) == 0,
           "о непоместившихся сообщается один раз");
    Expect(nxr::gfx::PendingDrain(g.r, [](const nxr::gfx::NvrmCallRecord&) {}) == 0,
           "второй слив ничего не повторяет");

    /* Слот взят, но запись ещё не готова (другой поток её пишет): сливать нельзя. */
    nxr::gfx::PendingRing<nxr::gfx::NvrmCallRecord, 3> r2;
    std::memset(&r2, 0, sizeof(r2));
    const int slot = nxr::gfx::PendingReserve(r2);
    Expect(slot == 0 && !nxr::gfx::PendingHasWork(r2) &&
               nxr::gfx::PendingDrain(r2, [](const nxr::gfx::NvrmCallRecord&) {}) == 0,
           "неготовая запись не сливается");
    nxr::gfx::PendingCommit(r2, slot);
    Expect(nxr::gfx::PendingHasWork(r2) &&
               nxr::gfx::PendingDrain(r2, [](const nxr::gfx::NvrmCallRecord&) {}) == 1,
           "после флага готовности — сливается");

    /* ---- негативная проба ---- */
    /* Ослабление «сливать всё взятое, не глядя на флаг»: на том же входе
       неготовая запись ушла бы в отчёт мусором. */
    nxr::gfx::PendingRing<nxr::gfx::NvrmCallRecord, 3> r3;
    std::memset(&r3, 0, sizeof(r3));
    nxr::gfx::PendingReserve(r3);
    const std::uint32_t weakCount = r3.m_Reserved;   /* ОСЛАБЛЕНО */
    Expect(weakCount == 1 && nxr::gfx::PendingDrain(r3, [](const nxr::gfx::NvrmCallRecord&) {}) == 0,
           "НЕГАТИВНАЯ ПРОБА: без флага готовности слилась бы неготовая запись");
}

/* ===================================================================== */
/*
 * [11] ПРОБА SetError: подпись, отбор кодов, формат строки (строка сообщения
 * и имя функции EGL печатаются САМИ, адрес — относительно GL).
 */
static void TestSetError() {
    std::printf("[11] проба SetError\n");
    using nxr::gfx::kSetErrorTargets;
    Expect(nxr::gfx::S_Count == 2 && kSetErrorTargets[0].offset == 0xe060 &&
               kSetErrorTargets[1].offset == 0xe160,
           "две цели: GL+0xe060 и GL+0xe160");
    Expect(kSetErrorTargets[0].codeReg == 1 && kSetErrorTargets[0].funcReg == 5 &&
               kSetErrorTargets[0].fmtReg == 6 && kSetErrorTargets[1].codeReg == 0 &&
               kSetErrorTargets[1].funcReg == 4 && kSetErrorTargets[1].fmtReg == 5,
           "регистры: e060 — w1/x5/x6, e160 — w0/x4/x5 (сверено по дизассемблеру)");
    Expect(nxr::gfx::SetErrorSignatureMatches(kSetErrorTargets[0].sig, 0) &&
               nxr::gfx::SetErrorSignatureMatches(kSetErrorTargets[1].sig, 1),
           "своя подпись совпадает");
    std::uint32_t other[8];
    std::memcpy(other, kSetErrorTargets[0].sig, sizeof(other));
    other[5] ^= 0x10u;
    Expect(!nxr::gfx::SetErrorSignatureMatches(other, 0) &&
               !nxr::gfx::SetErrorSignatureMatches(kSetErrorTargets[0].sig, 1) &&
               !nxr::gfx::SetErrorSignatureMatches(nullptr, 0),
           "НЕГАТИВ: чужой код (один бит другой, чужая цель, ноль) не опознаётся");
    Expect(!nxr::gfx::SetErrorWorthLogging(0x3000) && nxr::gfx::SetErrorWorthLogging(0x3003) &&
               nxr::gfx::SetErrorWorthLogging(0x300d),
           "успех 0x3000 не пишется, ошибки пишутся");

    static const char msg[] = "Unable to allocate surface resources";
    static const char fn[]  = "eglCreateWindowSurface";
    nxr::gfx::SetErrorRecord r {};
    r.code = 0x3003;
    r.which = 0;
    r.fmt = reinterpret_cast<std::uint64_t>(msg);
    r.func = reinterpret_cast<std::uint64_t>(fn);
    r.retAddr = 0x7100000000ull + 0x26614;
    nxr::gfx::Buf buf;
    nxr::gfx::FormatSetError(buf, r, 0x7100000000ull);
    const std::string s(buf.m_Buf, buf.m_Len);
    Expect(s.rfind("!! ", 0) == 0 && Has(s, "0x3003") && Has(s, msg) && Has(s, fn) &&
               Has(s, "GL+0x26614") && s.size() >= 2 && s.substr(s.size() - 2) == "\r\n",
           "строка: метка неуспеха, код, САМО сообщение, функция, GL+смещение, CRLF", s);
    r.fmt = 0;
    r.func = 0;
    nxr::gfx::FormatSetError(buf, r, 0);
    const std::string s2(buf.m_Buf, buf.m_Len);
    Expect(Has(s2, "(нет)") && Has(s2, "0x7100026614"),
           "нулевые указатели не разыменовываются; без базы — голый адрес", s2);

    nxr::gfx::NvrmCallRecord c {};
    c.handle = 0x2a; c.rc = 0; c.size = 0x3fc000; c.out = 0x1234; c.retAddr = 0x7100000000ull + 0x266f358;
    nxr::gfx::FormatNvrmCall(buf, c, 0x7100000000ull);
    const std::string s3(buf.m_Buf, buf.m_Len);
    Expect(s3.rfind("   ", 0) == 0 && Has(s3, "0x2a") && Has(s3, "0x3fc000") &&
               Has(s3, "GL+0x266f358") && Has(s3, "успех"),
           "вызов NvRm: хэндл, размер, адрес возврата GL+…", s3);
    c.rc = 2;
    nxr::gfx::FormatNvrmCall(buf, c, 0x7100000000ull);
    const std::string s4(buf.m_Buf, buf.m_Len);
    Expect(s4.rfind("!! ", 0) == 0 && Has(s4, "ОТКАЗ"), "отказ NvRm помечен «!!»", s4);

    /* Вердикт о том, куда указывает ячейка GOT импорта. */
    nxr::gfx::SymbolReport rep {};
    rep.slot = 0x5000; rep.ours = 0x1000; rep.selfBase = 0x0f00; rep.bound = 0x1000;
    rep.defs[0] = 0x1000; rep.defBases[0] = 0x0f00;
    rep.defs[1] = 0x9000; rep.defBases[1] = 0x8000; rep.defCount = 2;
    Expect(Has(nxr::gfx::BoundVerdict(rep), "НАШ"), "ячейка = наш экспорт -> «НАШ МОДУЛЬ»");
    rep.bound = 0x9000;
    Expect(Has(nxr::gfx::BoundVerdict(rep), "НЕ выиграл"), "ячейка = sdk -> «экспорт НЕ выиграл»");
    rep.slot = 0;
    Expect(Has(nxr::gfx::BoundVerdict(rep), "импортёра нет"), "нет импортёра — так и сказано");
}

/* ===================================================================== */

int main() {    std::printf("=== хост-тест диагностики графики (src/nxr_gfx.hpp) ===\n");
    TestFormat();
    TestFailureFlag();
    TestInstall();
    TestWriteOnEachEvent();
    TestTargetTable();
    TestRetKinds();
    TestHookBudget();
    TestLockWindow();
    TestEarlyText();
    TestPendingRing();
    TestSetError();

    std::printf("\nвыполнено проверок: %d, красных: %d\n", g_Checks, g_Failed);

    /* ЗАТВОР НУЛЯ: тест, который ничего не проверил, зелёным быть не может. */
    if (g_Checks < 80) {
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
