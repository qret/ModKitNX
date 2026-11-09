/*
 * nxr_gfx.hpp — диагностика ГРАФИЧЕСКОГО пути игры.
 *
 * Copyright (c) 2026, участники проекта nxredirect. GNU GPL версии 2
 * (тот же файл лицензии, что и у остального модуля).
 *
 * Зачем этот файл
 * ---------------
 * Well Dweller (0100217023F6C000) на консоли с ПОДМЕНЁННОЙ библиотекой
 * `sdk` (nnSdk 19.3.13 вместо родной 22.2.0) запускается, звучит и реагирует
 * на кнопки, но НЕ РИСУЕТ: чёрный экран без отчёта о падении.
 *
 * Статический разбор показал, что НЕРАЗРЕШЁННЫХ СИМВОЛОВ НЕТ:
 * все 646 импортов `main`, 416 импортов `subsdk0` (NintendoSDK_OpenGL 22.2.0)
 * и 643 импорта `subsdk1` (movie 22.2.0) находятся в экспортах nnSdk 19.3.13
 * (плюс два наших символа совместимости). Значит «молчит» не загрузчик, а
 * что-то внутри графики возвращает ошибку, которую игра не проверяет.
 * Проверено там же: WD.main НЕ импортирует eglGetError — то есть игра
 * действительно не смотрит на коды ошибок EGL.
 *
 * Поэтому диагностика не гадает, а ПЕРЕХВАТЫВАЕТ графическую цепочку и пишет
 * коды возврата каждого звена на SD-карту:
 *     nn::vi::Initialize -> OpenDefaultDisplay -> CreateLayer ->
 *     GetNativeWindow -> eglGetDisplay -> eglInitialize -> eglChooseConfig ->
 *     eglCreateContext -> eglCreateWindowSurface -> eglMakeCurrent ->
 *     eglSwapBuffers (первый кадр)
 * плюс два «добытчика указателей», по которым видно, что именно старый
 * драйвер не отдаёт: nvnBootstrapLoader / nvnBootstrapLoaderInternal
 * и eglGetProcAddress (для них пишутся ТОЛЬКО нулевые ответы и первые
 * несколько ненулевых — иначе отчёт распухнет на сотни строк).
 *
 * ПЕРВАЯ СТРОКА С «!!» — ГЛАВНЫЙ ПОДОЗРЕВАЕМЫЙ.
 *
 * ПЛЮС УЗКАЯ ПРОБА БЛОКИРОВОК (ШАГ-8, переделана после падения
 * на консоли; чем была раньше и почему сломала запуск — ниже).
 * Разбор реализации показал, что eglCreateWindowSurface отдаёт EGL_BAD_ALLOC
 * (0x3003) в ЕДИНСТВЕННОЙ ветке, и рядом с ней лежит строка «Could not acquire
 * EGLDisplay lock»: это не нехватка памяти, а НЕВЗЯТАЯ БЛОКИРОВКА ДИСПЛЕЯ.
 * Захват идёт раньше, чем функция посмотрит на config/win/attrib_list, поэтому
 * подбирать параметры поверхности бессмысленно.
 *
 * ЧТО ЭТО ЗА ЗАХВАТ — РАЗОБРАНО ПО КОДУ NintendoSDK_OpenGL-22_2_0:
 *   eglCreateWindowSurface -> внутренняя реализация -> AcquireLock(объект+0xC0,
 *   write=false) @0x3bba0, а она:
 *       if ([замок+0x30] != 0) return false;            // замок помечен нерабочим
 *       rc = write ? pthread_rwlock_wrlock : pthread_rwlock_rdlock (замок);
 *       return rc == 0;
 *   Ровно та же строка «Could not acquire EGLDisplay lock» и код 0x3003 стоят в
 *   ветке `tbz w0,#0` сразу после этого вызова.
 * А pthread_rwlock_rdlock подменённой nnSdk 19.3.13 (@0x1480a0) отдаёт
 * ненулевое ТОЛЬКО в двух случаях: EINVAL (0x16) при нулевом объекте и
 * EDEADLK (0x23), если nn::os::IsWriteLockHeldByCurrentThread сказала «да».
 * В отчёте с консоли eglCreateContext СРАЗУ ПОСЛЕ отказа берёт ТОТ ЖЕ замок
 * того же дисплея тем же путём и успешно — значит поле [замок+0x30] было нулём.
 * Отсюда вывод (по коду, не гипотеза): отказ = EDEADLK, то есть в этот момент
 * ТОТ ЖЕ ПОТОК уже держал на этом замке запись.
 * Открытым остаётся только «кто и где взял запись» — ради этого и проба.
 *
 * ЧЕГО ЗДЕСЬ БОЛЬШЕ НЕТ И ПОЧЕМУ (факт с консоли, ШАГ-7).
 * Прежняя проба перехватывала ПЯТЬ функций сразу —
 * pthread_rwlock_init/rdlock/wrlock/tryrdlock/unlock. Игра не запустилась
 * вовсе: отчёт `atmosphere/crash_reports` показал Data Abort по адресу
 * 0x6969696900000000 в нашем модуле. Это НЕ «чтение неинициализированной
 * памяти», а НАМЕРЕННАЯ авария exlaunch: exl::diag::AbortWithCtx пишет по
 * адресу (0x69696969 << 32) | значение. Дизассемблирование собранного subsdk9
 * (sha 61b5952…, PC = +0x12a0, LR = +0x3438) показало виновника точно:
 * в exl::hook::nx64::AllocForTrampoline пул трамплинов ограничен
 *     HookMax = JitSize / (MaxInstructions * 10 * 4) = 0x1000 / 200 = 20,
 * а модуль ставил 1 (nnMain) + 15 (графика) + 5 (блокировки) + 1 (nn::fs::
 * OpenFile) = 22 трамплина. На 22-м пул кончился, и exlaunch аварийно завершил
 * процесс — ровно на установке хука перенаправления файлов, последней по
 * порядку. Поэтому в отчёте о графике заголовок раздела есть, а строк нет:
 * упали сразу после окончания gfx::Start, до первого вызова EGL.
 * УРОК: широкий перехват общих функций синхронизации на живой игре недопустим
 * не только из-за цены вызова — он ещё и молча съедает общий ресурс модуля.
 *
 * ЧТО ВМЕСТО НЕГО. Один-единственный хук (pthread_rwlock_rdlock), и тот —
 * прозрачный, пока не выполнены ОБА условия: проба готова (g_LockArmed) и
 * текущий поток — тот самый, который сейчас исполняет eglCreateWindowSurface.
 * Вне этого окна перехватчик делает ровно одно: `return Orig(obj)`. Внутри
 * окна он ничего не пишет на карту, а кладёт запись в буфер фиксированного
 * размера в памяти; на карту буфер сбрасывает обёртка eglCreateWindowSurface
 * ПОСЛЕ возврата из оригинала, в своём же потоке. Включается отдельным ключом
 * сборки -DNXR_DIAG_LOCK, по умолчанию ВЫКЛЮЧЕН.
 *
 * ЧЕГО ЗДЕСЬ БОЛЬШЕ НЕТ: раньше перехватчик nn::vi::Initialize был
 * объявлен возвращающим Result, хотя эта функция не возвращает ничего. Мусор
 * из регистра печатался как «ОШИБКА 0xda0a3e40» — ложный след первой же
 * строкой отчёта. Теперь вид возврата лежит в таблице целей, перехватчик
 * берёт его оттуда, а расхождение ловят static_assert при сборке,
 * hosttest/gfx_test.cpp и hosttest/gfx_targets.py на ПК.
 *
 * Отчёт: `sd:/switch/ModKitNX/logs/<TitleID>/
 * gfx-NNNN.txt`, новый на каждый запуск, последние 5 на игру (nxr_logs.hpp);
 * первая строка — TitleID, BuildID main и номер. Прежние сборки — один
 * `nxredirect-gfx.txt` в КОРНЕ карты. Отдельный от отчёта diag:: (`diag-NNNN.txt`
 * в той же папке) он потому, что
 * diag:: закрывает свой файл после 60 путей OpenFile, а графика начинается
 * позже — иначе события потерялись бы.
 * Каждая строка пишется с WriteOptionFlag_Flush, то есть попадает на карту
 * сразу: если игра после этого зависнет, отчёт всё равно уже на карте.
 *
 * Включается ТОЛЬКО ключом сборки -DNXR_DIAG_GFX. Без него в модуле нет ни
 * одного байта этого файла, и обычная сборка совпадает с прежней по всему
 * исполняемому содержимому: .text, .rodata и .data байт в байт, различается
 * только 20-байтовый GNU build-id (линкер выдаёт новый при каждой сборке).
 * Проверяется hosttest/gfx_unchanged.py; у него есть своя негативная проба.
 *
 * Что здесь НЕЛЬЗЯ перехватить и почему
 * -------------------------------------
 *  - сами вызовы gl* (glClear, glDrawElements…): игра получает их указатели
 *    через eglGetProcAddress и зовёт по указателю, в .dynsym их нет, ставить
 *    хук не на что. Вместо этого логируется сам eglGetProcAddress.
 *  - внутренние функции драйвера NVN: они приходят из системного блоба через
 *    nvnBootstrapLoader, имени в таблице символов у них тоже нет. Логируется
 *    bootstrap-загрузчик — по нему видно, какое имя драйвер не отдал.
 *
 * Устройство файла: сверху — ЧИСТАЯ часть (формат строки и раскладка целей),
 * ровно она прогоняется на ПК хост-тестом hosttest/gfx_test.cpp. Ниже, под
 * NXR_GFX_DEVICE, — часть, которой нужен exlaunch (хуки и nn::fs).
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace nxr {
namespace gfx {

    /* ==================================================================== */
    /* ЧИСТАЯ ЧАСТЬ — компилируется и на ПК, прогоняется hosttest/gfx_test.cpp */
    /* ==================================================================== */

    /*
     * Как истолковать возвращённое значение при печати.
     *
     * ВАЖНО, ценой ложного следа. Для функции, которая НИЧЕГО не возвращает,
     * годится только Void: в w0/x0 после неё лежит мусор от последнего
     * внутреннего вызова, и любой другой вид превратит этот мусор в «ОШИБКА
     * 0x…» — то есть в несуществующую поломку в самом начале отчёта.
     * Так и случилось с nn::vi::Initialize (см. RetKindOf ниже).
     */
    enum class RetKind : std::uint8_t {
        Void,     /* функция ничего не возвращает */
        Result,   /* nn::Result: 0 — успех */
        Pointer,  /* указатель: 0 — провал */
        Bool,     /* EGLBoolean: 0 — провал */
        Errno,    /* код ошибки POSIX: 0 — успех (pthread_rwlock_*) */
    };

    /* Описание одного перехваченного вызова. */
    struct Event {
        const char*   name    = nullptr;   /* человеческое имя функции */
        const char*   strArg  = nullptr;   /* строковый аргумент, если есть */
        int           argc    = 0;         /* сколько числовых аргументов */
        std::uint64_t argv[4] = {0, 0, 0, 0};
        RetKind       kind    = RetKind::Void;
        std::uint64_t ret     = 0;
        bool          hasEglError = false; /* дозваниваться до eglGetError имеет
                                              смысл только для EGL-функций */
        std::uint32_t eglError    = 0;
    };

    /* Размер строки отчёта. Строки короткие; буфер живёт на стеке чужого потока. */
    constexpr std::size_t BufSize = 0x200;

    /* Буфер одной строки. Куча не используется нигде. */
    struct Buf {
        char        m_Buf[BufSize];
        std::size_t m_Len = 0;

        void Reset() { m_Len = 0; }

        /*
         * Единственное место, где что-то кладётся в буфер. Сравнение именно с
         * константой BufSize - 1 (а не «m_Len + 1 < BufSize»): при сложении
         * компилятор не может доказать отсутствие переполнения и с -Werror
         * ругается «writing 1 byte into a region of size 0» — проверено, сборка
         * на этом падала.
         */
        void Put(char c) {
            if (m_Len < BufSize - 1) {
                m_Buf[m_Len++] = c;
            }
        }

        void Add(const char* s) {
            if (s == nullptr) {
                return;
            }
            while (*s != '\0') {
                Put(*s++);
            }
        }

        void AddHex(std::uint64_t value) {
            Add("0x");
            char tmp[16];
            int n = 0;
            do {
                const std::uint32_t d = static_cast<std::uint32_t>(value & 0xF);
                tmp[n++] = static_cast<char>(d < 10 ? ('0' + d) : ('a' + d - 10));
                value >>= 4;
            } while (value != 0 && n < 16);
            while (n > 0) {
                Put(tmp[--n]);
            }
        }

        void AddDec(std::uint32_t value) {
            char tmp[12];
            int n = 0;
            do {
                tmp[n++] = static_cast<char>('0' + (value % 10));
                value /= 10;
            } while (value != 0 && n < 12);
            while (n > 0) {
                Put(tmp[--n]);
            }
        }

        /* CRLF: отчёт читают блокнотом Windows. */
        void End() {
            Put('\r');
            Put('\n');
        }
    };

    /*
     * Неуспех ли это. Вынесено отдельно, потому что по этому признаку читатель отчёта
     * (и хост-тест) ищет первое сломавшееся звено.
     */
    inline bool IsFailure(const Event& e) {
        switch (e.kind) {
            case RetKind::Result:  return e.ret != 0;
            case RetKind::Pointer: return e.ret == 0;
            case RetKind::Bool:    return e.ret == 0;
            case RetKind::Errno:   return e.ret != 0;
            case RetKind::Void:    return false;
        }
        return false;
    }

    /* Метка начала строки: по ней видно неуспех с первого взгляда и через поиск. */
    constexpr char MarkFail[] = "!! ";
    constexpr char MarkOk[]   = "   ";

    /*
     * EDEADLK. Тот самый код, которым nnSdk отвечает на попытку взять
     * блокировку на чтение, когда ЭТОТ ЖЕ поток уже держит её на запись
     * (разбор pthread_rwlock_rdlock, USE-1 §1.4). Для пробы ШАГ-7 он и есть
     * ответ на вопрос «взаимная это блокировка или замок просто сломан».
     */
    constexpr std::uint64_t Edeadlk = 0x23;

    /*
     * Одна строка отчёта о вызове. Вид:
     *   !! eglInitialize(0x7100000000, 0x0, 0x0) = FALSE (провал), eglGetError=0x3001
     *      nn::vi::OpenDefaultDisplay(0x7100000010) = успех
     *   !! nvnBootstrapLoader("nvnDeviceGetProcAddress") = НОЛЬ (провал)
     */
    inline void FormatEvent(Buf& buf, const Event& e) {
        buf.Reset();
        buf.Add(IsFailure(e) ? MarkFail : MarkOk);
        buf.Add(e.name != nullptr ? e.name : "(без имени)");
        buf.Add("(");
        bool first = true;
        if (e.strArg != nullptr) {
            buf.Add("\"");
            buf.Add(e.strArg);
            buf.Add("\"");
            first = false;
        }
        for (int i = 0; i < e.argc && i < 4; ++i) {
            if (!first) {
                buf.Add(", ");
            }
            buf.AddHex(e.argv[i]);
            first = false;
        }
        buf.Add(") = ");
        switch (e.kind) {
            case RetKind::Void:
                buf.Add("вызван");
                break;
            case RetKind::Result:
                if (e.ret == 0) {
                    buf.Add("успех");
                } else {
                    buf.Add("ОШИБКА ");
                    buf.AddHex(e.ret);
                }
                break;
            case RetKind::Pointer:
                if (e.ret == 0) {
                    buf.Add("НОЛЬ (провал)");
                } else {
                    buf.AddHex(e.ret);
                }
                break;
            case RetKind::Bool:
                buf.Add(e.ret != 0 ? "TRUE" : "FALSE (провал)");
                break;
            case RetKind::Errno:
                if (e.ret == 0) {
                    buf.Add("успех");
                } else {
                    buf.Add("КОД ОШИБКИ ");
                    buf.AddHex(e.ret);
                    /* Ради этого кода вся проба и затевалась: именно он
                       отличает «замок занят чужим» от «замок уже держит на
                       запись ЭТОТ ЖЕ поток» (взаимная блокировка). */
                    if (e.ret == Edeadlk) {
                        buf.Add(" (EDEADLK: этот же поток уже держит запись)");
                    }
                }
                break;
        }
        if (e.hasEglError) {
            buf.Add(", eglGetError=");
            buf.AddHex(e.eglError);
        }
        buf.End();
    }

    /*
     * Одна цель перехвата: манглированное имя, человеческое и ВИД ВОЗВРАТА.
     *
     * Вид возврата лежит здесь, а не в самом перехватчике, нарочно: таблица —
     * единственный источник правды, перехватчик берёт вид ИЗ НЕЁ
     * (см. RetKindOf и static_assert рядом с каждым хуком), а гейты на ПК
     * проверяют таблицу. Пока вид был вписан в каждый хук руками, ошибка в
     * одном хуке не ловилась ничем.
     */
    struct Target {
        const char* sym   = nullptr;
        const char* human = nullptr;
        RetKind     kind  = RetKind::Void;
    };

    /*
     * Раскладка перехвата. Порядок = порядок графической цепочки, так что в
     * отчёте «этапы» идут сверху вниз и сразу видно, до какого дошли.
     *
     * Манглинг nn::vi сверен с .dynsym файла
     * NintendoSdk_nnSdk-19_3_13-Release из папки SDK (все пять — DEF)
     * и с импортами main обеих игр.
     * EGL-имена — из экспортов NintendoSDK_OpenGL-22_2_0-Release (subsdk0
     * у Well Dweller), имена не манглируются (extern "C").
     */
    enum TargetIndex : int {
        T_ViInitialize = 0,
        T_ViOpenDefaultDisplay,
        T_ViCreateLayer,
        T_ViCreateLayerWh,
        T_ViGetNativeWindow,
        T_EglGetDisplay,
        T_EglInitialize,
        T_EglChooseConfig,
        T_EglCreateContext,
        T_EglCreateWindowSurface,
        T_EglMakeCurrent,
        T_EglSwapBuffers,
        T_EglGetProcAddress,
        T_NvnBootstrapLoader,
        T_NvnBootstrapLoaderInternal,
        T_Count
    };

    /*
     * Вид возврата у каждой цели — ПРОВЕРЕН ПО ДИЗАССЕМБЛЕРУ
     * (NintendoSdk_nnSdk-19_3_13-Release, capstone; те же адреса в 22.2.0).
     * Признак: кладётся ли что-нибудь в w0/x0 на пути к ret.
     *
     *   nn::vi::Initialize        @0x435760 — перед `ret` (0x435970) w0 НЕ
     *       трогается, последнее туда писал чужой `bl 0x6322d0`  ->  Void.
     *       Именно отсюда взялась строка «!! nn::vi::Initialize() = ОШИБКА
     *       0xda0a3e40» в отчёте с консоли: мусор из чужого вызова.
     *   nn::vi::OpenDefaultDisplay @0x43cff0 — хвостовой прыжок в
     *       nn::vi::OpenDisplay @0x43d000, а та явно ставит w0
     *       (`mov w0, w21`, ветка отказа `mov w21, #0xe72`)  ->  Result.
     *   nn::vi::CreateLayer (обе) @0x440610/0x440670 — возвращают результат
     *       внутренней 0x4406d0, а та перед ret делает `mov w0, w24` -> Result.
     *   nn::vi::GetNativeWindow   @0x440b10 — `mov w0, wzr` перед ret -> Result.
     *   EGL-функции — по спецификации EGL и по экспортам OpenGL-22_2_0:
     *       eglGetDisplay / eglCreateContext / eglCreateWindowSurface /
     *       eglGetProcAddress отдают указатель, eglInitialize /
     *       eglChooseConfig / eglMakeCurrent / eglSwapBuffers — EGLBoolean.
     *   nvnBootstrapLoader(Internal) — отдают указатель на функцию драйвера.
     *
     * Таблица constexpr: только так вид возврата можно сверить с сигнатурой
     * перехватчика прямо при сборке (static_assert ниже).
     */
    constexpr Target kTargets[T_Count] = {
        { "_ZN2nn2vi10InitializeEv",                       "nn::vi::Initialize",
          RetKind::Void },
        { "_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", "nn::vi::OpenDefaultDisplay",
          RetKind::Result },
        { "_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE",   "nn::vi::CreateLayer",
          RetKind::Result },
        { "_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayEii", "nn::vi::CreateLayer(w,h)",
          RetKind::Result },
        { "_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE",    "nn::vi::GetNativeWindow",
          RetKind::Result },
        { "eglGetDisplay",           "eglGetDisplay",           RetKind::Pointer },
        { "eglInitialize",           "eglInitialize",           RetKind::Bool },
        { "eglChooseConfig",         "eglChooseConfig",         RetKind::Bool },
        { "eglCreateContext",        "eglCreateContext",        RetKind::Pointer },
        { "eglCreateWindowSurface",  "eglCreateWindowSurface",  RetKind::Pointer },
        { "eglMakeCurrent",          "eglMakeCurrent",          RetKind::Bool },
        { "eglSwapBuffers",          "eglSwapBuffers",          RetKind::Bool },
        { "eglGetProcAddress",       "eglGetProcAddress",       RetKind::Pointer },
        { "nvnBootstrapLoader",          "nvnBootstrapLoader",          RetKind::Pointer },
        { "nvnBootstrapLoaderInternal",  "nvnBootstrapLoaderInternal",  RetKind::Pointer },
    };

    inline const Target* Targets() {
        return kTargets;
    }

    /* Вид возврата цели. Перехватчики зовут только её, своего мнения не имеют. */
    constexpr RetKind RetKindOf(int index) {
        return kTargets[index].kind;
    }

    /* Человеческое имя цели — тоже из таблицы, чтобы не разъехалось с ней. */
    constexpr const char* HumanOf(int index) {
        return kTargets[index].human;
    }

    /* ------------------------------------------------------------------ */
    /* БЮДЖЕТ ТРАМПЛИНОВ exlaunch — то, на чём сломался ШАГ-7             */
    /* ------------------------------------------------------------------ */
    /*
     * exl::hook::nx64::AllocForTrampoline выдаёт трамплины из JIT-области
     * фиксированного размера:
     *     HookMax = exl::setting::JitSize / (MaxInstructions * 10 * sizeof(u32))
     * При исчерпании пула exlaunch НЕ отказывает мягко, а аварийно завершает
     * процесс (R_ABORT_UNLESS -> запись по адресу 0x69696969_00000000). Ровно
     * это и случилось на консоли с ШАГОМ-7: 22 хука при пуле на 20.
     *
     * Поэтому бюджет живёт ЗДЕСЬ, в чистой части, и проверяется тремя местами:
     *   - static_assert ниже (не даст собрать модуль, где хуков больше бюджета);
     *   - hosttest/gfx_test.cpp (проверка на ПК, со своей негативной пробой);
     *   - hosttest/gfx_targets.py — сверяет эти числа с настоящими константами
     *     exlaunch (setting.hpp и hook_impl.cpp), чтобы бюджет не разъехался
     *     с тем, что на самом деле умеет пул.
     * И сверх того InstallTargets НЕ СТАВИТ хук, если бюджет исчерпан: лучше
     * неполная диагностика, чем не запустившаяся игра.
     */
    constexpr int TrampolineBytes = 200;   /* MaxInstructions(5) * 10 * 4 */
    constexpr int JitSizeBytes    = 0x2000;/* exl::setting::JitSize после правки */
    constexpr int HookMax         = JitSizeBytes / TrampolineBytes;  /* = 40 */

    /*
     * Хуки вне этого файла (src/main.cpp): nnMain, nn::fs::OpenFile и
     * nn::fs::SetAllocator. Третий добавлен вместе с затвором
     * файловой системы (src/nxr_fsgate.hpp) — без него игра падала на своём
     * SetAllocator с Result 0x327602.
     * Ещё +3: nn::fs::MountRom (без кеша и с кешем) и nn::fs::Unmount —
     * модуль сам узнаёт имя точки romfs игры (nxr::RomMounts, nxr_path.hpp).
     */
    constexpr int HooksOutsideGfx = 6;

    /* ------------------------------------------------------------------ */
    /* ЦЕЛИ УЗКОЙ ПРОБЫ БЛОКИРОВОК (ШАГ-8)                                */
    /* ------------------------------------------------------------------ */
    /*
     * ОСТАЛАСЬ ОДНА ЦЕЛЬ — pthread_rwlock_rdlock.
     *
     * Опровергнутое не стираем: в ШАГЕ-7 целей было пять (init, rdlock, wrlock,
     * tryrdlock, unlock). Проба сломала ЗАПУСК игры (см. шапку файла), а сверх
     * того четыре из пяти целей и не были нужны:
     *   - eglCreateWindowSurface берёт ИМЕННО read-блокировку: в AcquireLock
     *     @0x3bba0 вызов идёт по ветке `tbz w1,#0` -> PLT 0x26ab010 ->
     *     pthread_rwlock_rdlock (проверено по .rela.plt библиотеки OpenGL);
     *   - именно rdlock и отдаёт EDEADLK, ради которого проба делается;
     *   - объект замка (аргумент rdlock) — это и есть замок дисплея
     *     (внутренний объект + 0xC0), а из него читаются владелец записи
     *     [замок+0x18] и счётчик записей [замок+0x20]. То есть ответ «кто
     *     держит запись» даёт сам объект, а не перехват чужих wrlock.
     *
     * Адреса в nnSdk: 19.3.13 rdlock@0x1480a0, 22.2.0 rdlock@0x153e80.
     * Хук ставится на функцию в nnSdk, поэтому ловятся все вызывающие.
     */
    enum LockTargetIndex : int {
        L_RwlockRdlock = 0,
        L_Count
    };

    /* pthread_rwlock_rdlock отдаёт код ошибки POSIX: 0 — успех. */
    constexpr Target kLockTargets[L_Count] = {
        { "pthread_rwlock_rdlock",    "pthread_rwlock_rdlock",    RetKind::Errno },
    };

    inline const Target* LockTargets() {
        return kLockTargets;
    }

    constexpr RetKind LockRetKindOf(int index) {
        return kLockTargets[index].kind;
    }

    constexpr const char* LockHumanOf(int index) {
        return kLockTargets[index].human;
    }

    /* ------------------------------------------------------------------ */
    /* ПРОБА SetError: строка сообщения EGL прямо называет ветку            */
    /* ------------------------------------------------------------------ */
    /*
     * Зачем. eglCreateWindowSurface у Well Dweller отдаёт 0x3003, а веток с этим
     * кодом в пути создания поверхности ПЯТЬ (четыре прямыми переходами — запись
     * журнала проб — и пятая через vtable: импорт буфера окна,
     * «Unable to allocate surface resources», разбор кода Well Dweller). Каждая
     * ветка кладёт код через внутреннюю функцию записи ошибки EGL вместе со
     * СВОЕЙ строкой — по строке ветка и узнаётся однозначно.
     *
     * Функций записи две; регистры сверены по дизассемблеру
     * NintendoSDK_OpenGL-22_2_0-Release:
     *   база+0xe060: x0 — поле ошибки (`str w1,[x0]`), w1 — код
     *                (`cmp w1,#0x3000` — успех пропускается), x5 — имя функции
     *                EGL («In %s», NULL -> "internal function"), x6 — формат
     *                сообщения (в vsnprintf идёт x2 = x6), x7 и стек — аргументы;
     *   база+0xe160: w0 — код (`cmp w0,#0x3000`), x4 — имя функции, x5 — формат,
     *                x6, x7 и стек — аргументы.
     * Пример (ветка пятая): `0x26620: adrp x6 / add x6 -> «Unable to allocate
     * surface resources»; mov w1,#0x3003; b 0x26300 -> bl 0xe060`.
     *
     * Как перехватываем. Функции ВАРИАДИЧЕСКИЕ: трамплин с обёрткой на C++ потерял
     * бы аргументы на стеке. Поэтому — встроенный (inline) хук exlaunch: колбэк
     * получает регистры НА ВХОДЕ и ничего не меняет, дальше исполняется оригинал
     * как был. Колбэк только кладёт запись в память (PendingRing ниже): никаких
     * файлов и замков внутри GL. Строки — указатели в .rodata GL, читаются при
     * выводе. Успех (0x3000) не пишется.
     *
     * Цена: каждый inline-хук берёт ОДИН трамплин из общего пула (hook_impl.cpp,
     * HookInline -> Hook(..., true)) плюс запись из отдельного пула inline (0x1000
     * байт = 170 записей). Учтено в HooksTotal ниже.
     *
     * Защита от чужой версии GL. Хук ставится только если (1) модуль, в котором
     * найден экспорт eglCreateWindowSurface, имеет его ровно на смещении 0xe670
     * (так в 22.2.0) и (2) первые 8 инструкций по смещению совпадают с подписью
     * ниже. Иначе — строка «не поставлен» и игре не мешаем.
     */
    enum SetErrorIndex : int {
        S_SetErrorA = 0,   /* база+0xe060 */
        S_SetErrorB,       /* база+0xe160 */
        S_Count
    };

    struct SetErrorTarget {
        std::uint64_t offset  = 0;    /* от базы модуля GL */
        int           codeReg = 0;    /* w-регистр с кодом EGL */
        int           funcReg = 0;    /* x-регистр с именем функции EGL */
        int           fmtReg  = 0;    /* x-регистр с форматом сообщения */
        std::uint32_t sig[8]  = {};   /* первые 8 инструкций (GL 22.2.0) */
        const char*   human   = nullptr;
    };

    constexpr char          GlAnchorSym[]  = "eglCreateWindowSurface";
    constexpr std::uint64_t GlAnchorOffset = 0xe670;

    constexpr SetErrorTarget kSetErrorTargets[S_Count] = {
        { 0xe060, 1, 5, 6,
          { 0xA9BC7BFDu, 0xF9000BFCu, 0x910003FDu, 0xA90257F6u,
            0xA9034FF4u, 0xD11303FFu, 0x71400C3Fu, 0xF9000FA7u },
          "SetError@GL+0xe060 (w1=код, x5=функция, x6=сообщение)" },
        { 0xe160, 0, 4, 5,
          { 0xA9BC7BFDu, 0xF9000BFCu, 0x910003FDu, 0xA90257F6u,
            0xA9034FF4u, 0xD11343FFu, 0x71400C1Fu, 0xA9081FE6u },
          "SetError@GL+0xe160 (w0=код, x4=функция, x5=сообщение)" },
    };

    inline bool SetErrorSignatureMatches(const std::uint32_t* code, int index) {
        if (code == nullptr || index < 0 || index >= S_Count) {
            return false;
        }
        for (int i = 0; i < 8; ++i) {
            if (code[i] != kSetErrorTargets[index].sig[i]) {
                return false;
            }
        }
        return true;
    }

    /* Код EGL_SUCCESS: функция записи зовётся и с ним (сброс ошибки) — не пишем. */
    constexpr std::uint32_t EglSuccess = 0x3000;

    inline bool SetErrorWorthLogging(std::uint32_t code) {
        return code != EglSuccess;
    }

    /* Сколько трамплинов просит весь модуль в самой тяжёлой сборке. Замена
       заглушки NvRmMemQueryHandleParams (nxr_compat.hpp) — хук БЕЗ трамплина
       (HOOK_DEFINE_REPLACE / Hook(..., false)), в счёт не входит. */
    constexpr int HooksTotal = static_cast<int>(T_Count)
                               + static_cast<int>(L_Count)
                               + static_cast<int>(S_Count) + HooksOutsideGfx;

    static_assert(HooksTotal <= HookMax,
                  "хуков больше, чем трамплинов в пуле exlaunch: при исчерпании "
                  "пула exlaunch аварийно завершает игру (так упал ШАГ-7)");

    /*
     * Строка пробы блокировок. Аргументы в отчёте:
     *   argv[0] — объект замка (он же замок дисплея EGL: объект дисплея + 0xC0);
     *   argv[1] — владелец записи: [замок+0x18], адрес nn::os::ThreadType;
     *   argv[2] — 1, если запись на этом замке держит ТЕКУЩИЙ поток.
     * Отдельная функция, чтобы раскладка полей была в одном месте и её мог
     * проверить хост-тест.
     */
    inline void FillLockEvent(Event& e, int index, std::uint64_t obj,
                              std::uint64_t owner, bool heldByThisThread,
                              std::uint64_t ret) {
        e.name = LockHumanOf(index);
        e.kind = LockRetKindOf(index);
        e.argc = 3;
        e.argv[0] = obj;
        e.argv[1] = owner;
        e.argv[2] = heldByThisThread ? 1u : 0u;
        e.ret = ret;
    }

    /* ------------------------------------------------------------------ */
    /* ОКНО ПРОБЫ: «внутри eglCreateWindowSurface и в ТОМ ЖЕ потоке»      */
    /* ------------------------------------------------------------------ */
    /*
     * Почему именно так и почему это не может заклинить.
     *
     * Окно — ДВА обычных 64-битных поля в .bss, а не объект с конструктором:
     * загрузчик обнуляет .bss ДО первой нашей инструкции, поэтому перехватчик
     * безопасен с самого раннего момента, даже если сработает раньше, чем
     * выполнится хоть одна строка нашей инициализации. Никакой статической
     * инициализации, никакой кучи, никаких атомиков.
     *
     *   m_Armed — поднимается ОДИН раз, последним действием gfx::Start, после
     *             того как все указатели проставлены и хук уже стоит. Пока он
     *             ноль, перехватчик обязан просто вернуть Orig(obj);
     *   m_Owner — адрес TLS-области потока (tpidrro_el0), который СЕЙЧАС
     *             исполняет eglCreateWindowSurface; 0 = окно закрыто.
     *
     * Признак «наш поток» выбран как tpidrro_el0 потому, что это одна
     * инструкция без единого вызова: ничего не блокирует, ни от чего не
     * зависит и не требует инициализации (в отличие от настоящего TLS-слота
     * модуля, которого у нас может не быть вовсе).
     *
     * Заклинить нечему: нет ни ожидания, ни спина, ни взаимной зависимости
     * потоков. Поле m_Owner пишет только тот поток, который сам его и снимет
     * после возврата из оригинала. Если оригинал не вернётся (зависнет), окно
     * останется открытым — но это никого не задержит: другие потоки его просто
     * не узнают (m_Owner != их tls) и уйдут в Orig.
     */
    struct LockWindow {
        std::uint64_t m_Armed = 0;
        std::uint64_t m_Owner = 0;
    };

    /* Открыть окно может только готовая проба. Возвращает прежнего владельца. */
    inline std::uint64_t LockWindowOpen(LockWindow& w, std::uint64_t tls) {
        if (w.m_Armed == 0 || tls == 0) {
            return 0;
        }
        const std::uint64_t prev = w.m_Owner;
        w.m_Owner = tls;
        return prev;
    }

    inline void LockWindowClose(LockWindow& w, std::uint64_t prev) {
        w.m_Owner = prev;
    }

    /*
     * ЕДИНСТВЕННОЕ условие, при котором перехватчик вообще что-то делает.
     * Всё остальное время он прозрачен. Негативная проба хост-теста
     * (window-ignored) снимает эту проверку и обязана покраснеть.
     */
    inline bool LockWindowShouldLog(const LockWindow& w, std::uint64_t tls) {
        return w.m_Armed != 0 && tls != 0 && w.m_Owner == tls;
    }

    /* ------------------------------------------------------------------ */
    /* БУФЕР В ПАМЯТИ: внутри окна пишем только сюда                      */
    /* ------------------------------------------------------------------ */
    /*
     * Запись на карту идёт через nn::fs, а nn::fs сам берёт блокировки — то
     * есть из перехватчика блокировок писать на карту НЕЛЬЗЯ ни под каким
     * видом. Поэтому внутри окна собираем записи в массив фиксированного
     * размера, а на карту его выливает обёртка eglCreateWindowSurface уже
     * ПОСЛЕ возврата из оригинала.
     *
     * Ни одного атомика: в буфер пишет ровно один поток — владелец окна.
     * Переполнение не растёт и не портит память: лишние записи считаются
     * отдельно (m_Dropped) и попадают в отчёт числом.
     */
    constexpr int LockRingSize = 24;

    struct LockRecord {
        std::uint64_t m_Obj   = 0;   /* объект замка (замок дисплея EGL) */
        std::uint64_t m_Owner = 0;   /* [замок+0x18] — владелец записи */
        std::uint64_t m_Ret   = 0;   /* код возврата pthread_rwlock_rdlock */
        std::uint32_t m_WriteCount = 0; /* [замок+0x20] — счётчик записей */
        bool m_HeldByThisThread = false;
    };

    struct LockRing {
        LockRecord m_Items[LockRingSize];
        int        m_Count   = 0;
        int        m_Dropped = 0;

        void Reset() {
            m_Count = 0;
            m_Dropped = 0;
        }
    };

    inline void LockRingPush(LockRing& ring, const LockRecord& rec) {
        if (ring.m_Count >= LockRingSize) {
            ++ring.m_Dropped;
            return;
        }
        ring.m_Items[ring.m_Count++] = rec;
    }

    /*
     * Постановка перехватчиков по таблице.
     *
     * ДВА ГЛАВНЫХ ПРАВИЛА, ради которых это вынесено в отдельную функцию и
     * проверяется на ПК:
     *  1. если символ НЕ НАЙДЕН (резолвер вернул 0), ставить хук НЕЛЬЗЯ —
     *     установка по нулевому адресу роняет игру, и тогда отчёта не будет
     *     вовсе. Про ненайденный символ надо написать строку и идти дальше;
     *  2. если БЮДЖЕТ ТРАМПЛИНОВ исчерпан, ставить хук тоже НЕЛЬЗЯ: exlaunch
     *     на исчерпании пула аварийно завершает процесс (так упал ШАГ-7).
     *     Лучше неполная диагностика, чем не запустившаяся игра.
     * Негативные пробы хост-теста (no-null-check, no-budget-check) снимают эти
     * проверки и обязаны покраснеть.
     *
     *   resolve(sym)         -> адрес или 0
     *   install(index, addr) -> ставит конкретный хук (свитч по индексу)
     *   report(index, addr, installed) -> пишет строку о судьбе цели
     *
     * budgetLeft — сколько трамплинов ещё можно занять.
     * Возвращает, сколько хуков поставлено.
     */
    template <class Resolve, class Install, class Report>
    inline int InstallTargets(const Target* table, int count, int budgetLeft,
                              Resolve&& resolve, Install&& install, Report&& report) {
        int installed = 0;
        for (int i = 0; i < count; ++i) {
            const std::uint64_t addr = resolve(table[i].sym);
            bool done = false;
            if (addr != 0 && installed < budgetLeft) {
                install(i, addr);
                ++installed;
                done = true;
            }
            report(i, addr, done);
        }
        return installed;
    }

    /* ------------------------------------------------------------------ */
    /* РАННИЕ СОБЫТИЯ ГРАФИКИ: до открытия затвора ФС                    */
    /* ------------------------------------------------------------------ */
    /*
     * Что было. Перехватчики графики ставились только при открытии затвора ФС
     * (gfx::Start из OpenFsGate), а у Well Dweller графика стартует РАНЬШЕ его
     * nn::fs::SetAllocator: в отчёте nxredirect-gfx (4).txt нет ни nn::vi, ни
     * eglCreateWindowSurface — только 400 eglSwapBuffers(0x1, 0x0) с 0x300d.
     * Ранние события не «терялись при записи» — их было некому поймать.
     *
     * Как теперь. Перехватчики ставятся в хуке nnMain, ДО вызова оригинала
     * (ставить хук можно: это правка кода в памяти, nn::fs не трогается).
     * Пока затвор закрыт, готовые строки отчёта копятся здесь, в .bss, и при
     * открытии затвора выливаются в файл первыми, по порядку. То же правило, что
     * у ранних OpenFile (nxr_fsgate.hpp, EarlyLog): без конструкторов, без кучи,
     * строки, не поместившиеся ЦЕЛИКОМ, честно считаются (обрывков нет).
     */
    template <std::uint32_t Size>
    struct EarlyTextT {
        static constexpr std::uint32_t CanaryValue = 0x5A5AA5A5u;

        char          m_Buf[Size];
        std::uint32_t m_Len;
        std::uint32_t m_Lines;
        std::uint32_t m_Dropped;
        /* Контрольное слово сразу за полями: хост-тест ловит выход за край. */
        std::uint32_t m_Canary;

        void Init() {
            m_Len     = 0;
            m_Lines   = 0;
            m_Dropped = 0;
            m_Canary  = CanaryValue;
        }
        bool CanaryOk() const { return m_Canary == CanaryValue; }
    };

    /* Добавить строку целиком или не добавлять вовсе (и посчитать). */
    template <std::uint32_t Size>
    inline bool EarlyTextAppend(EarlyTextT<Size>& t, const char* s, std::size_t n) {
        if (t.m_Len > Size || n > static_cast<std::size_t>(Size - t.m_Len)) {
            ++t.m_Dropped;
            return false;
        }
        for (std::size_t i = 0; i < n; ++i) {
            t.m_Buf[t.m_Len + i] = s[i];
        }
        t.m_Len += static_cast<std::uint32_t>(n);
        ++t.m_Lines;
        return true;
    }

    /* 24 КБ: ~300 строк до открытия затвора. Хватает с запасом на всю цепочку
       nn::vi -> EGL и раздел перехватчиков; кадры eglSwapBuffers сверх лимитов
       отчёта сюда не пишутся (их режет MaxFrames/MaxLines). */
    constexpr std::uint32_t EarlyTextSize = 0x6000;
    using EarlyText = EarlyTextT<EarlyTextSize>;

    /* ------------------------------------------------------------------ */
    /* ОТЛОЖЕННЫЕ ЗАПИСИ ИЗ ЧУЖИХ ПОТОКОВ (SetError, NvRmMemQueryHandle…) */
    /* ------------------------------------------------------------------ */
    /*
     * Пишут сюда места, откуда писать в файл НЕЛЬЗЯ: колбэк SetError (внутри GL,
     * возможно под её замками) и наша NvRmMemQueryHandleParams (внутри импорта
     * буфера окна). Здесь нет ни замка, ни файла: слот берётся атомарным
     * fetch_add, поля пишутся, затем флаг готовности (release). Выливает записи
     * в отчёт ТОЛЬКО писатель отчёта, под своим замком, и только готовые, по
     * порядку. Кольца нет: хранятся ПЕРВЫЕ N записей (они и важны — дальше
     * повторы), остальные честно считаются (Reserved - N).
     *
     * Поля — простые числа в .bss; атомарность — встроенными __atomic_*, чтобы
     * у структуры не было конструктора (тот же приём, что у LockWindow).
     */
    template <class T, int N>
    struct PendingRing {
        T             m_Items[N];
        std::uint32_t m_Ready[N];
        std::uint32_t m_Reserved;      /* сколько слотов роздано (может превысить N) */
        std::uint32_t m_Drained;       /* сколько выведено (только писатель отчёта) */
        std::uint32_t m_DropReported;  /* о скольких непоместившихся уже сказано */
    };

    /* Взять слот: индекс или -1, если места нет (тогда запись посчитана). */
    template <class T, int N>
    inline int PendingReserve(PendingRing<T, N>& r) {
        const std::uint32_t i = __atomic_fetch_add(&r.m_Reserved, 1u, __ATOMIC_ACQ_REL);
        if (i >= static_cast<std::uint32_t>(N)) {
            return -1;
        }
        return static_cast<int>(i);
    }

    template <class T, int N>
    inline void PendingCommit(PendingRing<T, N>& r, int i) {
        __atomic_store_n(&r.m_Ready[i], 1u, __ATOMIC_RELEASE);
    }

    template <class T, int N>
    inline bool PendingPush(PendingRing<T, N>& r, const T& v) {
        const int i = PendingReserve(r);
        if (i < 0) {
            return false;
        }
        r.m_Items[i] = v;
        PendingCommit(r, i);
        return true;
    }

    template <class T, int N>
    inline std::uint32_t PendingDropped(const PendingRing<T, N>& r) {
        const std::uint32_t res = __atomic_load_n(&r.m_Reserved, __ATOMIC_ACQUIRE);
        return res > static_cast<std::uint32_t>(N) ? res - static_cast<std::uint32_t>(N) : 0u;
    }

    template <class T, int N>
    inline bool PendingHasWork(const PendingRing<T, N>& r) {
        const std::uint32_t d = r.m_Drained;
        return d < static_cast<std::uint32_t>(N) &&
               __atomic_load_n(&r.m_Ready[d], __ATOMIC_ACQUIRE) != 0;
    }

    /* Вылить готовые записи по порядку: emit(const T&). Возвращает сколько. */
    template <class T, int N, class Emit>
    inline int PendingDrain(PendingRing<T, N>& r, Emit&& emit) {
        int n = 0;
        while (r.m_Drained < static_cast<std::uint32_t>(N) &&
               __atomic_load_n(&r.m_Ready[r.m_Drained], __ATOMIC_ACQUIRE) != 0) {
            emit(r.m_Items[r.m_Drained]);
            ++r.m_Drained;
            ++n;
        }
        return n;
    }

    /* Сколько непоместившихся появилось с прошлого раза (и запомнить). */
    template <class T, int N>
    inline std::uint32_t PendingNewDrops(PendingRing<T, N>& r) {
        const std::uint32_t d = PendingDropped(r);
        const std::uint32_t fresh = d > r.m_DropReported ? d - r.m_DropReported : 0u;
        r.m_DropReported = d;
        return fresh;
    }

    /* Запись SetError. Строки — указатели в .rodata GL: живут всё время игры. */
    /* Без инициализаторов полей: тривиальный тип, глобальное кольцо — чистая .bss. */
    struct SetErrorRecord {
        std::uint32_t code;
        std::uint32_t which;     /* S_SetErrorA / S_SetErrorB */
        std::uint64_t func;      /* const char* имя функции EGL или 0 */
        std::uint64_t fmt;       /* const char* формат сообщения или 0 */
        std::uint64_t retAddr;   /* LR на входе: откуда в GL позвали */
    };
    constexpr int SetErrorRingSize = 32;

    /* Вызов NvRmMemQueryHandleParams (наша функция, nxr_compat.hpp). */
    struct NvrmCallRecord {
        std::uint32_t handle;
        std::uint32_t rc;
        std::uint64_t size;
        std::uint64_t out;
        std::uint64_t retAddr;
    };
    constexpr int NvrmRingSize = 48;

    /* Строка из чужой памяти: не длиннее max и только до нуля. */
    inline void AddBounded(Buf& buf, const char* s, std::size_t max) {
        if (s == nullptr) {
            buf.Add("(нет)");
            return;
        }
        for (std::size_t i = 0; i < max && s[i] != '\0'; ++i) {
            buf.Put(s[i]);
        }
    }

    /* «GL+0x…», если адрес внутри модуля GL (по базе), иначе голый адрес. */
    inline void AddCodeAddr(Buf& buf, std::uint64_t addr, std::uint64_t glBase) {
        if (glBase != 0 && addr >= glBase && addr - glBase < 0x10000000ull) {
            buf.Add("GL+");
            buf.AddHex(addr - glBase);
        } else {
            buf.AddHex(addr);
        }
    }

    /*
     * Строка отчёта о SetError. Вид:
     * !! SetError#1 код 0x3003 «Unable to allocate surface resources»
     *    в «eglCreateWindowSurface», вызвана из GL+0x26300
     */
    inline void FormatSetError(Buf& buf, const SetErrorRecord& r, std::uint64_t glBase) {
        buf.Reset();
        buf.Add(MarkFail);
        buf.Add("SetError#");
        buf.AddDec(r.which + 1);
        buf.Add(" код ");
        buf.AddHex(r.code);
        buf.Add(" «");
        AddBounded(buf, reinterpret_cast<const char*>(static_cast<std::uintptr_t>(r.fmt)), 160);
        buf.Add("» в «");
        AddBounded(buf, reinterpret_cast<const char*>(static_cast<std::uintptr_t>(r.func)), 64);
        buf.Add("», вызвана из ");
        AddCodeAddr(buf, r.retAddr, glBase);
        buf.End();
    }

    /*
     * Строка отчёта о вызове NvRmMemQueryHandleParams. Вид:
     *    NvRmMemQueryHandleParams(хэндл 0x2a, out 0x…) = 0 (успех), размер 0x3fc000,
     *    вызвана из GL+0x266f358
     */
    inline void FormatNvrmCall(Buf& buf, const NvrmCallRecord& r, std::uint64_t glBase) {
        buf.Reset();
        buf.Add(r.rc != 0 ? MarkFail : MarkOk);
        buf.Add("NvRmMemQueryHandleParams(хэндл ");
        buf.AddHex(r.handle);
        buf.Add(", out ");
        buf.AddHex(r.out);
        buf.Add(") = ");
        buf.AddHex(r.rc);
        if (r.rc == 0) {
            buf.Add(" (успех), размер ");
            buf.AddHex(r.size);
        } else {
            buf.Add(" (ОТКАЗ)");
        }
        buf.Add(", вызвана из ");
        AddCodeAddr(buf, r.retAddr, glBase);
        buf.End();
    }

    /*
     * Куда разрешился импорт NvRmMemQueryHandleParams у GL и чей символ выиграл
     * бы по правилу rtld. Заполняет main.cpp (у него обход памяти), печатает
     * отчёт графики. Простая структура: только числа.
     */
    struct SymbolReport {
        std::uint64_t importerBase = 0;   /* модуль, который импортирует символ */
        std::uint64_t slot         = 0;   /* адрес ячейки GOT импортёра */
        std::uint64_t bound        = 0;   /* что в ячейке сейчас (BIND_NOW: уже итог) */
        std::uint64_t defs[4]      = {0, 0, 0, 0};   /* адреса определений */
        std::uint64_t defBases[4]  = {0, 0, 0, 0};   /* базы их модулей */
        int           defCount     = 0;
        std::uint64_t rtldWinner   = 0;   /* первый определённый не-LOCAL */
        std::uint64_t ours         = 0;   /* наш экспорт (0 — сборка без него) */
        std::uint64_t selfBase     = 0;   /* база нашего модуля */
        int           path         = 0;   /* nxr::compat::NvrmPath или 0 */
        bool          hooked       = false;
    };

    /* Человеческий вердикт: куда указывает ячейка GOT. */
    inline const char* BoundVerdict(const SymbolReport& s) {
        if (s.slot == 0) {
            return "импортёра нет (ни один модуль не зовёт эту функцию)";
        }
        if (s.ours != 0 && s.bound == s.ours) {
            return "НАШ МОДУЛЬ — экспорт выиграл";
        }
        for (int i = 0; i < s.defCount && i < 4; ++i) {
            if (s.bound == s.defs[i] && s.defBases[i] != s.selfBase) {
                return "чужой модуль (sdk) — наш экспорт НЕ выиграл";
            }
        }
        if (s.bound == 0) {
            return "ячейка пуста (импорт не связан)";
        }
        return "неизвестный адрес";
    }

}  // namespace gfx
}  // namespace nxr

/* ====================================================================== */
/* ЧАСТЬ ДЛЯ КОНСОЛИ. Нужен exlaunch (HOOK_DEFINE_TRAMPOLINE) и nn::fs.    */
/* Включается только из main.cpp, где всё это уже подключено.              */
/* ====================================================================== */
#ifdef NXR_GFX_DEVICE

#include "nxr_path.hpp"   /* NXR_SD_MOUNT — наше имя точки монтирования SD */
#ifndef NXR_LOGS_DEVICE
#  define NXR_LOGS_DEVICE 1
#endif
#include "nxr_logs.hpp"   /* путь отчёта по TitleID, номер и ротация */

namespace nxr {
namespace gfx {

    /* Путь отчёта: sd:/switch/ModKitNX/logs/<TitleID>/gfx-NNNN.txt, собирается
       в StartReport (nxr::logs::Prepare). Папка — по TitleID, не по пути мода:
       отчёт нужен, даже если путь мода не собрался. Точка монтирования — НАША
       (nxrsd, см. nxr_path.hpp): общее имя "sd" может быть занято чужим модом. */
    inline char ReportPath[nxr::logs::PathSize] = {};
    inline nxr::logs::Prepared       g_Prepared {};
    inline nxr::logs::device::Clock  g_Clock {};
    /* Строка времени ещё не записана (nn::time игрой не запущен) — 0/1. */
    inline std::uint32_t             g_TimePending = 0;

    /* Сколько кадров логировать (eglSwapBuffers зовётся 60 раз в секунду). */
    constexpr int MaxFrames = 5;
    /* Сколько первых кадров пробовать дописать строку времени (nxr_logs.hpp). */
    constexpr int TimeRetryFrames = 600;
    /* Сколько УСПЕШНЫХ ответов «добытчиков указателей» логировать.
       Неуспешные (ноль) пишутся ВСЕ — именно они и есть улика. */
    constexpr int MaxLookupOk = 12;
    /* Общий предел строк: отчёт не должен расти без края, если игра живёт. */
    constexpr int MaxLines = 400;

    using ResolveFn = std::uint64_t (*)(const char*);

    /* Функции записи из nnSdk — те же, что использует diag::. */
    using CreateFileFn = Result (*)(char const*, s64);
    using DeleteFileFn = Result (*)(char const*);
    using OpenFileFn2  = Result (*)(nn::fs::FileHandle*, char const*, int);
    using WriteFileFn  = Result (*)(nn::fs::FileHandle, s64, void const*, u64,
                                    nn::fs::WriteOption const&);
    using FlushFileFn  = Result (*)(nn::fs::FileHandle);
    using CloseFileFn  = void   (*)(nn::fs::FileHandle);
    using MountSdFn    = Result (*)(char const*);
    using EglGetErrorFn = std::uint32_t (*)();
    /* nn::os::IsWriteLockHeldByCurrentThread: её мы не перехватываем, а зовём —
       она и отвечает, держит ли запись текущий поток (проба ШАГ-7). */
    using IsWriteHeldFn = bool (*)(const void*);

    inline CreateFileFn  g_CreateFile = nullptr;
    inline DeleteFileFn  g_DeleteFile = nullptr;
    inline OpenFileFn2   g_OpenFile   = nullptr;
    inline WriteFileFn   g_WriteFile  = nullptr;
    inline FlushFileFn   g_FlushFile  = nullptr;
    inline CloseFileFn   g_CloseFile  = nullptr;
    inline EglGetErrorFn g_EglGetError = nullptr;

    inline nn::fs::FileHandle g_Handle {};
    inline s64                g_Offset = 0;
    inline std::atomic<bool>  g_Open   { false };
    inline std::atomic<int>   g_Lines  { 0 };
    inline std::atomic<int>   g_Frames { 0 };
    inline std::atomic<int>   g_LookupOk { 0 };
    /*
     * Узкая проба блокировок (ШАГ-8). Объявлена здесь, выше перехватчиков,
     * потому что окно пробы открывает eglCreateWindowSurface, а он идёт раньше.
     *
     * ВСЁ ЗДЕСЬ — ПРОСТЫЕ ПОЛЯ, БЕЗ КОНСТРУКТОРОВ И БЕЗ АТОМИКОВ. Они лежат
     * в .bss, которую загрузчик обнуляет до первой нашей инструкции: значит
     * перехватчик, вызванный раньше нашей инициализации, увидит m_Armed == 0
     * и просто уйдёт в оригинал. Ровно этого требует урок ШАГА-7.
     */
    inline IsWriteHeldFn      g_IsWriteHeld = nullptr;
    inline LockWindow         g_LockWindow {};
    inline LockRing           g_LockRing {};
    /* Сколько раз окно открывалось: по нему видно, доходило ли дело до пробы. */
    inline int                g_LockWindows = 0;
    /* Графику игра трогает из нескольких потоков — запись под замком. */
    inline std::atomic_flag   g_Lock   {};

    /*
     * РАННИЕ СОБЫТИЯ. Перехватчики стоят с хука nnMain, а файл
     * отчёта можно открыть только после затвора ФС. До этого готовые строки
     * копятся в g_EarlyText; g_ReportLive = 1 ставит StartReport, когда
     * накопленное уже вылито в файл. Всё — простые поля в .bss.
     */
    inline EarlyText     g_EarlyText;
    inline std::uint32_t g_ReportLive = 0;
    /* База модуля GL (для «GL+0x…» в строках) и отложенные записи. */
    inline std::uint64_t g_GlBase = 0;
    inline PendingRing<SetErrorRecord, SetErrorRingSize> g_SetErrorRing;
    inline PendingRing<NvrmCallRecord, NvrmRingSize>     g_NvrmRing;
    /* Сколько раз вызвана наша NvRmMemQueryHandleParams всего (и сверх кольца). */
    inline std::uint32_t g_NvrmCalls = 0;

    /* Прямая запись в файл. Замок g_Lock уже взят вызывающим. */
    inline void WriteFileLocked(const char* data, std::size_t len) {
        if (len == 0 || g_WriteFile == nullptr || !g_Open.load(std::memory_order_relaxed)) {
            return;
        }
        /* Flush на КАЖДОЙ записи: если игра сразу после этого зависнет,
           строка уже лежит на карте. Ради этого и заведён отдельный файл. */
        const nn::fs::WriteOption opt =
            nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush);
        if (R_SUCCEEDED(g_WriteFile(g_Handle, g_Offset, data, len, opt))) {
            g_Offset += static_cast<s64>(len);
        }
    }

    /* Куда идёт строка: до открытия отчёта — в память, после — в файл. */
    inline void SinkLocked(const char* data, std::size_t len) {
        if (__atomic_load_n(&g_ReportLive, __ATOMIC_ACQUIRE) == 0) {
            EarlyTextAppend(g_EarlyText, data, len);
        } else {
            WriteFileLocked(data, len);
        }
    }

    /* Вылить готовые отложенные записи (SetError, NvRm). Замок уже взят. */
    inline void DrainPendingLocked() {
        Buf buf;
        PendingDrain(g_SetErrorRing, [&buf](const SetErrorRecord& r) {
            FormatSetError(buf, r, g_GlBase);
            SinkLocked(buf.m_Buf, buf.m_Len);
        });
        if (const std::uint32_t lost = PendingNewDrops(g_SetErrorRing); lost != 0) {
            buf.Reset();
            buf.Add("   (записей SetError не поместилось в буфер: ещё ");
            buf.AddDec(lost);
            buf.Add(")");
            buf.End();
            SinkLocked(buf.m_Buf, buf.m_Len);
        }
        PendingDrain(g_NvrmRing, [&buf](const NvrmCallRecord& r) {
            FormatNvrmCall(buf, r, g_GlBase);
            SinkLocked(buf.m_Buf, buf.m_Len);
        });
        if (const std::uint32_t lost = PendingNewDrops(g_NvrmRing); lost != 0) {
            buf.Reset();
            buf.Add("   (вызовов NvRmMemQueryHandleParams не поместилось в буфер: ещё ");
            buf.AddDec(lost);
            buf.Add(", всего вызовов ");
            buf.AddDec(__atomic_load_n(&g_NvrmCalls, __ATOMIC_ACQUIRE));
            buf.Add(")");
            buf.End();
            SinkLocked(buf.m_Buf, buf.m_Len);
        }
    }

    /* Строка отчёта: сперва всё отложенное (оно случилось раньше), потом она. */
    inline void Emit(const char* data, std::size_t len) {
        while (g_Lock.test_and_set(std::memory_order_acquire)) {
        }
        DrainPendingLocked();
        SinkLocked(data, len);
        g_Lock.clear(std::memory_order_release);
    }

    /* Отложенное, когда своих строк нет (кадры сверх лимита): дёшево, если пусто. */
    inline void FlushPending() {
        if (!PendingHasWork(g_SetErrorRing) && !PendingHasWork(g_NvrmRing)) {
            return;
        }
        while (g_Lock.test_and_set(std::memory_order_acquire)) {
        }
        DrainPendingLocked();
        g_Lock.clear(std::memory_order_release);
    }

    /* Прямо в файл, минуя ранний буфер (шапка отчёта в StartReport). */
    inline void WriteDirect(const char* data, std::size_t len) {
        while (g_Lock.test_and_set(std::memory_order_acquire)) {
        }
        WriteFileLocked(data, len);
        g_Lock.clear(std::memory_order_release);
    }

    inline void WriteRaw(const char* data, std::size_t len) {
        Emit(data, len);
    }

    inline void WriteBuf(Buf& buf) {
        Emit(buf.m_Buf, buf.m_Len);
    }

    inline void WriteText(const char* a, const char* b = nullptr) {
        Buf buf;
        buf.Add(a);
        buf.Add(b);
        buf.End();
        WriteBuf(buf);
    }

    inline void WriteTextDirect(const char* a, const char* b = nullptr) {
        Buf buf;
        buf.Add(a);
        buf.Add(b);
        buf.End();
        WriteDirect(buf.m_Buf, buf.m_Len);
    }

    /*
     * Строка времени по часам nn::time игры (nxr_logs.hpp). false — часов ещё
     * нет. direct — прямо в файл (шапка), иначе обычным порядком строк.
     */
    inline bool WriteTimeLine(bool late, bool direct) {
        Buf buf;
        buf.Reset();
        buf.Add(late ? "время (дописано позже, часы консоли): " : "время (часы консоли): ");
        nxr::logs::Out o(buf.m_Buf + buf.m_Len, BufSize - 2 - buf.m_Len);
        if (!g_Clock.Format(o)) {
            return false;
        }
        buf.m_Len += o.m_Len;
        buf.End();
        if (direct) {
            WriteDirect(buf.m_Buf, buf.m_Len);
        } else {
            WriteBuf(buf);
        }
        return true;
    }

    /* Отложенная строка времени: зовётся на первых кадрах (EglSwapBuffers).
       Пишет ровно один поток — забирает флаг атомарным обменом. */
    inline void TryLateTime() {
        if (__atomic_load_n(&g_TimePending, __ATOMIC_ACQUIRE) == 0 ||
            __atomic_exchange_n(&g_TimePending, 0u, __ATOMIC_ACQ_REL) == 0) {
            return;
        }
        if (!WriteTimeLine(true, false)) {
            __atomic_store_n(&g_TimePending, 1u, __ATOMIC_RELEASE);
        }
    }

    /* Запись одного перехваченного вызова. Возвращает false, если предел строк. */
    inline bool LogEvent(const Event& e) {
        if (g_Lines.fetch_add(1, std::memory_order_relaxed) >= MaxLines) {
            return false;
        }
        Buf buf;
        FormatEvent(buf, e);
        WriteBuf(buf);
        return true;
    }

    /* eglGetError зовём сами и только при неуспехе: игра его не зовёт вообще. */
    inline void FillEglError(Event& e) {
        if (g_EglGetError != nullptr && IsFailure(e)) {
            e.hasEglError = true;
            e.eglError = g_EglGetError();
        }
    }

    /* ---------------------- сами перехватчики ---------------------- */

    /*
     * nn::vi::Initialize НИЧЕГО НЕ ВОЗВРАЩАЕТ.
     *
     * Было: хук был объявлен `static Result Callback()`, читал
     * мусор из w0 как nn::Result и выдавал первой строкой отчёта
     * «!! nn::vi::Initialize() = ОШИБКА 0xda0a3e40». Несуществующая ошибка
     * стояла ровно там, куда инструкция велит смотреть в первую очередь, и
     * увела разбор в сторону. Здесь и сигнатура, и вид возврата — void, и
     * static_assert не даст разойтись им с таблицей.
     */
    static_assert(RetKindOf(T_ViInitialize) == RetKind::Void,
                  "nn::vi::Initialize возвращает void: любой иной вид напечатает "
                  "мусор из w0 как ошибку");

    HOOK_DEFINE_TRAMPOLINE(ViInitializeHook) {
        static void Callback() {
            Orig();
            Event e; e.name = HumanOf(T_ViInitialize);
            e.kind = RetKindOf(T_ViInitialize);
            /* ret намеренно не заполняем: возвращать нечего. */
            LogEvent(e);
        }
    };

    static_assert(RetKindOf(T_ViOpenDefaultDisplay) == RetKind::Result,
                  "nn::vi::OpenDefaultDisplay отдаёт nn::Result");

    HOOK_DEFINE_TRAMPOLINE(ViOpenDefaultDisplayHook) {
        static Result Callback(void** outDisplay) {
            const Result r = Orig(outDisplay);
            Event e; e.name = HumanOf(T_ViOpenDefaultDisplay);
            e.kind = RetKindOf(T_ViOpenDefaultDisplay);
            e.argc = 1;
            e.argv[0] = (outDisplay != nullptr)
                        ? reinterpret_cast<std::uint64_t>(*outDisplay) : 0;
            e.ret = static_cast<std::uint64_t>(r);
            LogEvent(e);
            return r;
        }
    };

    static_assert(RetKindOf(T_ViCreateLayer) == RetKind::Result
                      && RetKindOf(T_ViCreateLayerWh) == RetKind::Result,
                  "обе nn::vi::CreateLayer отдают nn::Result внутренней 0x4406d0");

    HOOK_DEFINE_TRAMPOLINE(ViCreateLayerHook) {
        static Result Callback(void** outLayer, void* display) {
            const Result r = Orig(outLayer, display);
            Event e; e.name = HumanOf(T_ViCreateLayer);
            e.kind = RetKindOf(T_ViCreateLayer);
            e.argc = 2;
            e.argv[0] = (outLayer != nullptr)
                        ? reinterpret_cast<std::uint64_t>(*outLayer) : 0;
            e.argv[1] = reinterpret_cast<std::uint64_t>(display);
            e.ret = static_cast<std::uint64_t>(r);
            LogEvent(e);
            return r;
        }
    };

    HOOK_DEFINE_TRAMPOLINE(ViCreateLayerWhHook) {
        static Result Callback(void** outLayer, void* display, int w, int h) {
            const Result r = Orig(outLayer, display, w, h);
            Event e; e.name = HumanOf(T_ViCreateLayerWh);
            e.kind = RetKindOf(T_ViCreateLayerWh);
            e.argc = 3;
            e.argv[0] = reinterpret_cast<std::uint64_t>(display);
            e.argv[1] = static_cast<std::uint64_t>(static_cast<std::uint32_t>(w));
            e.argv[2] = static_cast<std::uint64_t>(static_cast<std::uint32_t>(h));
            e.ret = static_cast<std::uint64_t>(r);
            LogEvent(e);
            return r;
        }
    };

    static_assert(RetKindOf(T_ViGetNativeWindow) == RetKind::Result,
                  "nn::vi::GetNativeWindow отдаёт nn::Result (mov w0, wzr перед ret)");

    HOOK_DEFINE_TRAMPOLINE(ViGetNativeWindowHook) {
        static Result Callback(void** outWindow, void* layer) {
            const Result r = Orig(outWindow, layer);
            Event e; e.name = HumanOf(T_ViGetNativeWindow);
            e.kind = RetKindOf(T_ViGetNativeWindow);
            e.argc = 2;
            e.argv[0] = (outWindow != nullptr)
                        ? reinterpret_cast<std::uint64_t>(*outWindow) : 0;
            e.argv[1] = reinterpret_cast<std::uint64_t>(layer);
            e.ret = static_cast<std::uint64_t>(r);
            LogEvent(e);
            return r;
        }
    };

    static_assert(RetKindOf(T_EglGetDisplay) == RetKind::Pointer
                      && RetKindOf(T_EglCreateContext) == RetKind::Pointer
                      && RetKindOf(T_EglCreateWindowSurface) == RetKind::Pointer
                      && RetKindOf(T_EglInitialize) == RetKind::Bool
                      && RetKindOf(T_EglChooseConfig) == RetKind::Bool
                      && RetKindOf(T_EglMakeCurrent) == RetKind::Bool
                      && RetKindOf(T_EglSwapBuffers) == RetKind::Bool,
                  "виды возврата EGL: указатель у Get/Create, EGLBoolean у прочих");

    HOOK_DEFINE_TRAMPOLINE(EglGetDisplayHook) {
        static void* Callback(void* nativeDisplay) {
            void* const p = Orig(nativeDisplay);
            Event e; e.name = HumanOf(T_EglGetDisplay);
            e.kind = RetKindOf(T_EglGetDisplay);
            e.argc = 1; e.argv[0] = reinterpret_cast<std::uint64_t>(nativeDisplay);
            e.ret = reinterpret_cast<std::uint64_t>(p);
            FillEglError(e);
            LogEvent(e);
            return p;
        }
    };

    HOOK_DEFINE_TRAMPOLINE(EglInitializeHook) {
        static std::uint32_t Callback(void* dpy, int* major, int* minor) {
            const std::uint32_t ok = Orig(dpy, major, minor);
            Event e; e.name = HumanOf(T_EglInitialize);
            e.kind = RetKindOf(T_EglInitialize);
            e.argc = 3;
            e.argv[0] = reinterpret_cast<std::uint64_t>(dpy);
            /* версия EGL, которую отдал драйвер: по ней видно его поколение */
            e.argv[1] = (major != nullptr) ? static_cast<std::uint64_t>(*major) : 0;
            e.argv[2] = (minor != nullptr) ? static_cast<std::uint64_t>(*minor) : 0;
            e.ret = ok;
            FillEglError(e);
            LogEvent(e);
            return ok;
        }
    };

    HOOK_DEFINE_TRAMPOLINE(EglChooseConfigHook) {
        static std::uint32_t Callback(void* dpy, const int* attrib, void** configs,
                                      int size, int* num) {
            const std::uint32_t ok = Orig(dpy, attrib, configs, size, num);
            Event e; e.name = HumanOf(T_EglChooseConfig);
            e.kind = RetKindOf(T_EglChooseConfig);
            e.argc = 2;
            e.argv[0] = reinterpret_cast<std::uint64_t>(dpy);
            /* сколько конфигураций подошло: ноль при TRUE — тоже чёрный экран */
            e.argv[1] = (num != nullptr) ? static_cast<std::uint64_t>(*num) : 0;
            e.ret = ok;
            FillEglError(e);
            LogEvent(e);
            return ok;
        }
    };

    HOOK_DEFINE_TRAMPOLINE(EglCreateContextHook) {
        static void* Callback(void* dpy, void* config, void* share, const int* attrib) {
            void* const ctx = Orig(dpy, config, share, attrib);
            Event e; e.name = HumanOf(T_EglCreateContext);
            e.kind = RetKindOf(T_EglCreateContext);
            e.argc = 2;
            e.argv[0] = reinterpret_cast<std::uint64_t>(dpy);
            e.argv[1] = reinterpret_cast<std::uint64_t>(config);
            e.ret = reinterpret_cast<std::uint64_t>(ctx);
            FillEglError(e);
            LogEvent(e);
            return ctx;
        }
    };

    /* Объявления пробы блокировок: нужны обёртке ниже, а сама проба идёт
       дальше по файлу (там же, где её хук). Без ключа -DNXR_DIAG_LOCK обе
       функции пустые, и в модуле от пробы не остаётся ничего. */
    inline std::uint64_t LockProbeBegin();
    inline void          LockProbeEnd(std::uint64_t prev);

    HOOK_DEFINE_TRAMPOLINE(EglCreateWindowSurfaceHook) {
        static void* Callback(void* dpy, void* config, void* win, const int* attrib) {
            /* Окно пробы блокировок: ТОЛЬКО на время этого вызова и ТОЛЬКО
               для этого потока. Открывается ДО оригинала — иначе самого
               интересного (неудавшегося захвата) мы бы не увидели. */
            const std::uint64_t prev = LockProbeBegin();
            void* const surf = Orig(dpy, config, win, attrib);
            LockProbeEnd(prev);
            Event e; e.name = HumanOf(T_EglCreateWindowSurface);
            e.kind = RetKindOf(T_EglCreateWindowSurface);
            e.argc = 3;
            e.argv[0] = reinterpret_cast<std::uint64_t>(dpy);
            e.argv[1] = reinterpret_cast<std::uint64_t>(config);
            e.argv[2] = reinterpret_cast<std::uint64_t>(win);
            e.ret = reinterpret_cast<std::uint64_t>(surf);
            FillEglError(e);
            LogEvent(e);
            return surf;
        }
    };

    HOOK_DEFINE_TRAMPOLINE(EglMakeCurrentHook) {
        static std::uint32_t Callback(void* dpy, void* draw, void* read, void* ctx) {
            const std::uint32_t ok = Orig(dpy, draw, read, ctx);
            Event e; e.name = HumanOf(T_EglMakeCurrent);
            e.kind = RetKindOf(T_EglMakeCurrent);
            e.argc = 3;
            e.argv[0] = reinterpret_cast<std::uint64_t>(draw);
            e.argv[1] = reinterpret_cast<std::uint64_t>(read);
            e.argv[2] = reinterpret_cast<std::uint64_t>(ctx);
            e.ret = ok;
            FillEglError(e);
            LogEvent(e);
            return ok;
        }
    };

    HOOK_DEFINE_TRAMPOLINE(EglSwapBuffersHook) {
        static std::uint32_t Callback(void* dpy, void* surf) {
            const std::uint32_t ok = Orig(dpy, surf);
            const int n = g_Frames.fetch_add(1, std::memory_order_relaxed);
            /* Кадров бывают тысячи. Пишем первые MaxFrames, дальше — только
               если кадр НЕУСПЕШНЫЙ (такое видеть надо всегда). */
            if (n < TimeRetryFrames && __atomic_load_n(&g_ReportLive, __ATOMIC_ACQUIRE) != 0) {
                /* Время, если в шапке его не было (nn::time игра запускает позже).
                   Пробуем первые TimeRetryFrames кадров (~10 с), дальше не тратимся. */
                TryLateTime();
            }
            if (n < MaxFrames || ok == 0) {
                Event e; e.name = HumanOf(T_EglSwapBuffers);
                e.kind = RetKindOf(T_EglSwapBuffers);
                e.argc = 3;
                e.argv[0] = reinterpret_cast<std::uint64_t>(dpy);
                e.argv[1] = reinterpret_cast<std::uint64_t>(surf);
                e.argv[2] = static_cast<std::uint64_t>(n) + 1;  /* номер кадра */
                e.ret = ok;
                FillEglError(e);
                if (!LogEvent(e)) {
                    /* Предел строк исчерпан, но отложенные записи (SetError,
                       NvRm) — другое дело: их выливаем всё равно. */
                    FlushPending();
                    return ok;
                }
                if (n == 0) {
                    /* Раньше строка «первый кадр отдан» печаталась и после
                       НЕУСПЕШНОГО вызова (nxredirect-gfx (4).txt: FALSE, 0x300d,
                       и следом «ЭТАП: ПЕРВЫЙ КАДР ОТДАН») — ложный след. */
                    WriteText(ok != 0
                              ? "*** ЭТАП: ПЕРВЫЙ КАДР ОТДАН НА ЭКРАН ***"
                              : "*** ПЕРВЫЙ eglSwapBuffers НЕУСПЕШЕН: кадр на экран НЕ отдан ***");
                }
            } else {
                FlushPending();
            }
            return ok;
        }
    };

    /*
     * «Добытчики указателей». Их ответ — главное доказательство гипотезы
     * «драйвер старой прошивки не отдаёт то, чего просит библиотека 22.2.0».
     * Ноль в ответе пишем ВСЕГДА, ненулевые — только первые MaxLookupOk штук.
     */
    inline void LogLookup(int index, const char* what, std::uint64_t got) {
        if (got != 0) {
            if (g_LookupOk.fetch_add(1, std::memory_order_relaxed) >= MaxLookupOk) {
                return;
            }
        }
        Event e;
        e.name = HumanOf(index);
        e.strArg = what;
        e.kind = RetKindOf(index);
        e.ret = got;
        LogEvent(e);
    }

    static_assert(RetKindOf(T_EglGetProcAddress) == RetKind::Pointer
                      && RetKindOf(T_NvnBootstrapLoader) == RetKind::Pointer
                      && RetKindOf(T_NvnBootstrapLoaderInternal) == RetKind::Pointer,
                  "добытчики указателей отдают адрес функции");

    HOOK_DEFINE_TRAMPOLINE(EglGetProcAddressHook) {
        static void* Callback(const char* name) {
            void* const p = Orig(name);
            LogLookup(T_EglGetProcAddress, name, reinterpret_cast<std::uint64_t>(p));
            return p;
        }
    };

    HOOK_DEFINE_TRAMPOLINE(NvnBootstrapLoaderHook) {
        static void* Callback(const char* name) {
            void* const p = Orig(name);
            LogLookup(T_NvnBootstrapLoader, name, reinterpret_cast<std::uint64_t>(p));
            return p;
        }
    };

    HOOK_DEFINE_TRAMPOLINE(NvnBootstrapLoaderInternalHook) {
        static void* Callback(const char* name) {
            void* const p = Orig(name);
            LogLookup(T_NvnBootstrapLoaderInternal, name,
                      reinterpret_cast<std::uint64_t>(p));
            return p;
        }
    };

    /* ------------------------------------------------------------------ */
    /* УЗКАЯ ПРОБА БЛОКИРОВОК (ШАГ-8). Ключ сборки -DNXR_DIAG_LOCK.       */
    /* ------------------------------------------------------------------ */
    /*
     * Правило записи, переписанное после падения ШАГА-7 на консоли:
     *   - перехватчик ОДИН (pthread_rwlock_rdlock) — см. разбор в шапке файла;
     *   - он что-то делает ТОЛЬКО когда проба готова И текущий поток сейчас
     *     исполняет eglCreateWindowSurface. Во всех остальных случаях —
     *     ровно `return Orig(obj)`, ни одного обращения к нашим данным, ни
     *     одного счётчика, ни одной записи;
     *   - внутри окна пишем ТОЛЬКО в память (LockRing), потому что запись на
     *     карту идёт через nn::fs, а nn::fs сам берёт блокировки: писать файл
     *     из перехватчика блокировок — верный способ уйти в самозацикливание
     *     или встать на чужом замке;
     *   - на карту буфер выливает обёртка eglCreateWindowSurface ПОСЛЕ
     *     возврата из оригинала и ПОСЛЕ закрытия окна.
     *
     * Раскладка nn::os::ReaderWriterLockType — из дизассемблера
     * nn::os::IsWriteLockHeldByCurrentThread (19.3.13 @0x13c390 и 22.2.0
     * @0x1471f0, байт в байт одинаковые):
     *     ldr x8, [x0, #0x18]        ; владелец записи (nn::os::ThreadType*)
     *     mrs x9, tpidrro_el0
     *     ldr x9, [x9, #0x1f8]       ; ThreadType текущего потока
     *     cmp x8, x9  ... ldr w8, [x0, #0x20] ; счётчик взятых записей
     * Отсюда же взят и признак «наш поток»: tpidrro_el0.
     */
    constexpr std::uint64_t LockOwnerOffset = 0x18;  /* владелец записи */
    constexpr std::uint64_t LockCountOffset = 0x20;  /* счётчик записей */

    /* Адрес TLS-области текущего потока. Одна инструкция, без вызовов,
       без блокировок и без инициализации — потому и выбрана признаком потока. */
    inline std::uint64_t CurrentThreadTls() {
#if defined(__aarch64__)
        std::uint64_t v = 0;
        __asm__ __volatile__("mrs %0, tpidrro_el0" : "=r"(v));
        return v;
#else
        return 0;
#endif
    }

    inline std::uint64_t ReadLockField(const void* obj, std::uint64_t off) {
        if (obj == nullptr) {
            return 0;
        }
        const auto* p = reinterpret_cast<const volatile std::uint64_t*>(
            reinterpret_cast<const unsigned char*>(obj) + off);
        return *p;
    }

    inline bool WriteLockHeldHere(const void* obj) {
        return (g_IsWriteHeld != nullptr && obj != nullptr) ? g_IsWriteHeld(obj)
                                                            : false;
    }

#ifdef NXR_DIAG_LOCK

    /*
     * Сам перехватчик. Всё «дорогое» — строго после проверки окна.
     * ВАЖНО: ни одного вызова, который сам берёт блокировку, здесь нет.
     * g_IsWriteHeld (nn::os::IsWriteLockHeldByCurrentThread) блокировок не
     * берёт: это два чтения и сравнение (разбор выше).
     */
    HOOK_DEFINE_TRAMPOLINE(RwlockRdlockHook) {
        static int Callback(void* obj) {
            if (!LockWindowShouldLog(g_LockWindow, CurrentThreadTls())) {
                return Orig(obj);          /* вне окна мы полностью прозрачны */
            }
            const std::uint64_t owner = ReadLockField(obj, LockOwnerOffset);
            const std::uint64_t cnt   = ReadLockField(obj, LockCountOffset);
            const bool held = WriteLockHeldHere(obj);
            const int rc = Orig(obj);
            LockRecord rec;
            rec.m_Obj = reinterpret_cast<std::uint64_t>(obj);
            rec.m_Owner = owner;
            rec.m_WriteCount = static_cast<std::uint32_t>(cnt);
            rec.m_HeldByThisThread = held;
            rec.m_Ret = static_cast<std::uint64_t>(static_cast<std::uint32_t>(rc));
            LockRingPush(g_LockRing, rec);
            return rc;
        }
    };

    inline void InstallOneLock(int index, std::uint64_t addr) {
        const uintptr_t a = static_cast<uintptr_t>(addr);
        switch (index) {
            case L_RwlockRdlock:    RwlockRdlockHook::InstallAtPtr(a); break;
            default: break;
        }
    }

    inline std::uint64_t LockProbeBegin() {
        g_LockRing.Reset();
        const std::uint64_t prev = LockWindowOpen(g_LockWindow, CurrentThreadTls());
        if (g_LockWindow.m_Armed != 0) {
            ++g_LockWindows;
        }
        return prev;
    }

    /* Окно закрывается ПЕРВЫМ действием — дальше запись на карту уже безопасна. */
    inline void LockProbeEnd(std::uint64_t prev) {
        LockWindowClose(g_LockWindow, prev);
        if (g_LockWindow.m_Armed == 0) {
            return;
        }
        Buf buf;
        buf.Reset();
        buf.Add("   проба блокировок: захватов внутри eglCreateWindowSurface = ");
        buf.AddDec(static_cast<std::uint32_t>(g_LockRing.m_Count));
        if (g_LockRing.m_Dropped != 0) {
            buf.Add(", не поместилось в буфер = ");
            buf.AddDec(static_cast<std::uint32_t>(g_LockRing.m_Dropped));
        }
        buf.End();
        WriteBuf(buf);
        for (int i = 0; i < g_LockRing.m_Count; ++i) {
            const LockRecord& r = g_LockRing.m_Items[i];
            Event e;
            FillLockEvent(e, L_RwlockRdlock, r.m_Obj, r.m_Owner,
                          r.m_HeldByThisThread, r.m_Ret);
            LogEvent(e);
            buf.Reset();
            buf.Add("      замок дисплея, счётчик взятых записей = ");
            buf.AddDec(r.m_WriteCount);
            buf.Add(r.m_HeldByThisThread
                        ? "; запись держит ЭТОТ ЖЕ поток — взаимная блокировка"
                        : "; запись этим потоком не держится");
            buf.End();
            WriteBuf(buf);
        }
        g_LockRing.Reset();
    }

#else  /* без -DNXR_DIAG_LOCK пробы нет вовсе */

    inline void InstallOneLock(int, std::uint64_t) {}
    inline std::uint64_t LockProbeBegin() { return 0; }
    inline void LockProbeEnd(std::uint64_t) {}

#endif  /* NXR_DIAG_LOCK */

    /* Постановка хука по номеру цели. Свитч, а не таблица указателей:
       у каждого хука своя сигнатура, единого типа у них нет. */
    inline void InstallOne(int index, std::uint64_t addr) {
        const uintptr_t a = static_cast<uintptr_t>(addr);
        switch (index) {
            case T_ViInitialize:         ViInitializeHook::InstallAtPtr(a); break;
            case T_ViOpenDefaultDisplay: ViOpenDefaultDisplayHook::InstallAtPtr(a); break;
            case T_ViCreateLayer:        ViCreateLayerHook::InstallAtPtr(a); break;
            case T_ViCreateLayerWh:      ViCreateLayerWhHook::InstallAtPtr(a); break;
            case T_ViGetNativeWindow:    ViGetNativeWindowHook::InstallAtPtr(a); break;
            case T_EglGetDisplay:          EglGetDisplayHook::InstallAtPtr(a); break;
            case T_EglInitialize:          EglInitializeHook::InstallAtPtr(a); break;
            case T_EglChooseConfig:        EglChooseConfigHook::InstallAtPtr(a); break;
            case T_EglCreateContext:       EglCreateContextHook::InstallAtPtr(a); break;
            case T_EglCreateWindowSurface: EglCreateWindowSurfaceHook::InstallAtPtr(a); break;
            case T_EglMakeCurrent:         EglMakeCurrentHook::InstallAtPtr(a); break;
            case T_EglSwapBuffers:         EglSwapBuffersHook::InstallAtPtr(a); break;
            case T_EglGetProcAddress:      EglGetProcAddressHook::InstallAtPtr(a); break;
            case T_NvnBootstrapLoader:         NvnBootstrapLoaderHook::InstallAtPtr(a); break;
            case T_NvnBootstrapLoaderInternal: NvnBootstrapLoaderInternalHook::InstallAtPtr(a); break;
            default: break;
        }
    }

    /* ------------------------------------------------------------------ */
    /* ПРОБА SetError (inline-хуки exlaunch, только запись в память)      */
    /* ------------------------------------------------------------------ */
    /*
     * Колбэк зовётся на ВХОДЕ функции записи ошибки EGL, до её пролога; регистры
     * x0..x29 и LR exlaunch сохраняет и восстанавливает сам (inline_asm.s),
     * оригинал после нас исполняется как был — со всеми аргументами на стеке.
     * Вещественные регистры обычный inline-хук НЕ сохраняет, а функция кладёт
     * q0..q7 в область va_list (вещественные аргументы формата). Поэтому колбэк
     * собран без FP/SIMD (general-regs-only) и пишет только целые поля.
     * Вариант exlaunch с сохранением вещественных (InlineFloatCtx) не годится:
     * в exl_inline_float_hook_impl x0 указывает на ВЕЩЕСТВЕННУЮ область, а
     * InlineFloatCtx ждёт в начале целые регистры — раскладки расходятся.
     */
    template <int Index>
    __attribute__((target("general-regs-only")))
    inline void SetErrorNote(exl::hook::InlineCtx* ctx) {
        const SetErrorTarget& t = kSetErrorTargets[Index];
        const std::uint32_t code = static_cast<std::uint32_t>(ctx->m_Gpr.m_Gp[t.codeReg].W);
        if (!SetErrorWorthLogging(code)) {
            return;
        }
        /* То же, что PendingReserve/PendingCommit, но прямо здесь: вызов
           функции без general-regs-only мог бы задеть q0..q7. */
        const std::uint32_t i = __atomic_fetch_add(&g_SetErrorRing.m_Reserved, 1u,
                                                   __ATOMIC_ACQ_REL);
        if (i >= static_cast<std::uint32_t>(SetErrorRingSize)) {
            return;   /* не поместилось — посчитано в PendingDropped */
        }
        SetErrorRecord& r = g_SetErrorRing.m_Items[i];
        r.code    = code;
        r.which   = static_cast<std::uint32_t>(Index);
        r.func    = ctx->m_Gpr.m_Gp[t.funcReg].X;
        r.fmt     = ctx->m_Gpr.m_Gp[t.fmtReg].X;
        r.retAddr = ctx->m_Gpr.m_Lr.X;
        __atomic_store_n(&g_SetErrorRing.m_Ready[i], 1u, __ATOMIC_RELEASE);
    }

    HOOK_DEFINE_INLINE(SetErrorProbeA) {
        __attribute__((target("general-regs-only")))
        static void Callback(exl::hook::InlineCtx* ctx) { SetErrorNote<S_SetErrorA>(ctx); }
    };

    HOOK_DEFINE_INLINE(SetErrorProbeB) {
        __attribute__((target("general-regs-only")))
        static void Callback(exl::hook::InlineCtx* ctx) { SetErrorNote<S_SetErrorB>(ctx); }
    };

    inline void InstallSetError(int index, std::uint64_t addr) {
        const uintptr_t a = static_cast<uintptr_t>(addr);
        switch (index) {
            case S_SetErrorA: SetErrorProbeA::InstallAtPtr(a); break;
            case S_SetErrorB: SetErrorProbeB::InstallAtPtr(a); break;
            default: break;
        }
    }

    /*
     * Крючок нашей NvRmMemQueryHandleParams (nxr_compat.hpp): каждый вызов —
     * одна запись в память. Зовётся внутри импорта буфера окна GL, поэтому
     * никаких файлов и замков; в отчёт запись выльет писатель отчёта.
     */
    inline void NoteNvrmCall(std::uint32_t handle, const void* out, std::uint32_t rc,
                             std::uint64_t size, std::uint64_t retAddr) {
        __atomic_fetch_add(&g_NvrmCalls, 1u, __ATOMIC_ACQ_REL);
        const int i = PendingReserve(g_NvrmRing);
        if (i < 0) {
            return;
        }
        NvrmCallRecord& r = g_NvrmRing.m_Items[i];
        r.handle  = handle;
        r.rc      = rc;
        r.size    = size;
        r.out     = reinterpret_cast<std::uint64_t>(out);
        r.retAddr = retAddr;
        PendingCommit(g_NvrmRing, i);
    }

    /* Создание файла отчёта. Отдельный от diag::, со своим хэндлом.
       Папки, номер и удаление старых (сверх 5) — nxr::logs::Prepare. */
    inline bool OpenReport(ResolveFn resolve, std::uint64_t titleId) {
        if (g_CreateFile == nullptr || g_OpenFile == nullptr) {
            return false;
        }
        nxr::logs::device::NnFs fs {};
        fs.Init(resolve);
        g_Prepared = nxr::logs::Prepare(fs, nxr::logs::GfxTemplate, titleId,
                                        ReportPath, sizeof(ReportPath));
        if (g_Prepared.number == 0) {
            return false;
        }
        /* Файл с этим номером мог остаться (папку не прочли — номер 1). */
        if (g_DeleteFile != nullptr) {
            g_DeleteFile(ReportPath);
        }
        g_CreateFile(ReportPath, 0);
        const int mode = nn::fs::OpenMode_Write | nn::fs::OpenMode_Append;
        if (!R_SUCCEEDED(g_OpenFile(&g_Handle, ReportPath, mode))) {
            return false;
        }
        g_Offset = 0;
        g_Open.store(true, std::memory_order_release);
        WriteDirect("\xEF\xBB\xBF", 3);  /* BOM: блокнот покажет русский текст */
        return true;
    }

    /* Поиск сведений о символе (импортёр, ячейка GOT, определения) — у main.cpp. */
    using SymbolReportFn = bool (*)(const char* name, SymbolReport* out);

    inline void ReportLine(Buf& buf, const char* title, std::uint64_t addr, std::uint64_t glBase) {
        buf.Reset();
        buf.Add("   ");
        buf.Add(title);
        if (addr == 0) {
            buf.Add("0 (нет)");
        } else {
            AddCodeAddr(buf, addr, glBase);
        }
        buf.End();
        WriteBuf(buf);
    }

    /* Раздел «куда разрешился импорт NvRmMemQueryHandleParams». */
    inline void ReportNvrmResolution(SymbolReportFn symReport) {
        WriteText("");
        WriteText("--- NvRmMemQueryHandleParams: куда разрешился импорт GL (исправление для подменённой sdk) ---");
        SymbolReport s {};
        if (symReport == nullptr || !symReport("NvRmMemQueryHandleParams", &s)) {
            WriteText("!! сведения о символе собрать не удалось");
            return;
        }
        Buf buf;
        buf.Reset();
        buf.Add(s.ours != 0 ? MarkOk : "   ");
        buf.Add("наш экспорт в этой сборке: ");
        if (s.ours != 0) {
            buf.AddHex(s.ours);
            buf.Add(" (модуль ");
            buf.AddHex(s.selfBase);
            buf.Add(")");
        } else {
            buf.Add("НЕТ (сборка без -DNXR_COMPAT_NVRM_QUERY)");
        }
        buf.End();
        WriteBuf(buf);
        for (int i = 0; i < s.defCount && i < 4; ++i) {
            buf.Reset();
            buf.Add("   определение #");
            buf.AddDec(static_cast<std::uint32_t>(i) + 1);
            buf.Add(": ");
            buf.AddHex(s.defs[i]);
            buf.Add(" в модуле ");
            buf.AddHex(s.defBases[i]);
            buf.Add(s.defBases[i] == s.selfBase ? " (НАШ)" : " (чужой: sdk)");
            buf.End();
            WriteBuf(buf);
        }
        ReportLine(buf, "импортёр (модуль GL): ", s.importerBase, 0);
        ReportLine(buf, "ячейка GOT импорта: ", s.slot, s.importerBase);
        ReportLine(buf, "по правилу rtld выигрывает: ", s.rtldWinner, 0);
        buf.Reset();
        buf.Add((s.ours != 0 && s.bound == s.ours) ? MarkOk : MarkFail);
        buf.Add("импорт GL сейчас указывает на ");
        buf.AddHex(s.bound);
        buf.Add(" = ");
        buf.Add(BoundVerdict(s));
        buf.End();
        WriteBuf(buf);
#ifdef NXR_COMPAT_NVRM_QUERY
        buf.Reset();
        buf.Add(s.path == static_cast<int>(nxr::compat::NvrmPath::LeaveAlone) ? MarkFail : MarkOk);
        buf.Add("решение модуля: ");
        buf.Add(nxr::compat::NvrmPathText(static_cast<nxr::compat::NvrmPath>(s.path)));
        if (s.hooked) {
            buf.Add(" [заглушка sdk заменена]");
        }
        buf.End();
        WriteBuf(buf);
        WriteText("каждый вызов ниже: хэндл, ответ, размер [out+8], адрес возврата (GL+0x… — "
                  "по разбору ждём GL+0x266f358 и GL+0x2652f2c)");
#endif
    }

    /*
     * ПОСТАНОВКА перехватчиков графики. Зовётся из ХУКА nnMain (src/main.cpp),
     * ДО вызова оригинала: все модули загружены, а nn::fs ещё нельзя трогать
     * (затвор ФС закрыт). Здесь только поиск символов (чтение памяти) и правка
     * кода (хуки) — ни одного вызова nn::fs. Всё, что пишется в отчёт, ложится
     * в ранний буфер (g_EarlyText) и попадёт в файл в StartReport.
     *
     * Почему не при открытии затвора, как было раньше: у Well Dweller
     * графика стартует РАНЬШЕ nn::fs::SetAllocator, и ранние вызовы
     * (nn::vi, eglCreateWindowSurface) перехватывать было уже поздно —
     * в nxredirect-gfx (4).txt их нет.
     *
     * resolve — поиск символа во всех модулях (nxr::FindSymbolAnywhere);
     * symReport — сведения об импорте (для раздела NvRmMemQueryHandleParams).
     * Возвращает false, если писать отчёт будет нечем (нет функций nn::fs);
     * перехватчики при этом не ставятся вовсе.
     */
    inline bool Install(ResolveFn resolve, SymbolReportFn symReport) {
        g_EarlyText.Init();

        g_CreateFile = reinterpret_cast<CreateFileFn>(resolve("_ZN2nn2fs10CreateFileEPKcl"));
        g_DeleteFile = reinterpret_cast<DeleteFileFn>(resolve("_ZN2nn2fs10DeleteFileEPKc"));
        g_OpenFile   = reinterpret_cast<OpenFileFn2>(resolve("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci"));
        g_WriteFile  = reinterpret_cast<WriteFileFn>(resolve("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE"));
        g_FlushFile  = reinterpret_cast<FlushFileFn>(resolve("_ZN2nn2fs9FlushFileENS0_10FileHandleE"));
        g_CloseFile  = reinterpret_cast<CloseFileFn>(resolve("_ZN2nn2fs9CloseFileENS0_10FileHandleE"));
        g_EglGetError = reinterpret_cast<EglGetErrorFn>(resolve("eglGetError"));
        g_IsWriteHeld = reinterpret_cast<IsWriteHeldFn>(resolve(
            "_ZN2nn2os30IsWriteLockHeldByCurrentThreadEPKNS0_20ReaderWriterLockTypeE"));

        if (g_CreateFile == nullptr || g_OpenFile == nullptr || g_WriteFile == nullptr) {
            return false;
        }

        WriteText("--- перехватчики: что удалось поставить (поставлены в хуке nnMain, "
                  "ДО затвора ФС) ---");

        Buf buf;
        /*
         * БЮДЖЕТ ТРАМПЛИНОВ. Шесть хуков (nnMain, nn::fs::OpenFile, SetAllocator,
         * MountRom ×2 и Unmount — последние три добавлены позже)
         * модуль ставит сам в src/main.cpp, поэтому диагностике остаётся меньше.
         * Исчерпать пул нельзя: на этом ШАГ-7 уронил игру (см. шапку файла).
         */
        int budget = HookMax - HooksOutsideGfx;
        buf.Reset();
        buf.Add("бюджет трамплинов exlaunch: всего ");
        buf.AddDec(static_cast<std::uint32_t>(HookMax));
        buf.Add(", занято модулем вне диагностики ");
        buf.AddDec(static_cast<std::uint32_t>(HooksOutsideGfx));
        buf.Add(", остаток диагностике ");
        buf.AddDec(static_cast<std::uint32_t>(budget));
        buf.End();
        WriteBuf(buf);

        const Target* table = Targets();
        const int installed = InstallTargets(
            table, T_Count, budget,
            resolve,
            [](int index, std::uint64_t addr) { InstallOne(index, addr); },
            [&buf, table](int index, std::uint64_t addr, bool done) {
                buf.Reset();
                buf.Add(done ? MarkOk : MarkFail);
                buf.Add(table[index].human);
                buf.Add(" = ");
                if (done) {
                    buf.AddHex(addr);
                    buf.Add(" (перехвачен)");
                } else if (addr == 0) {
                    buf.Add("СИМВОЛ НЕ НАЙДЕН — не перехвачен, игре не мешаем");
                } else {
                    buf.Add("НЕ СТАВИМ: кончился бюджет трамплинов exlaunch");
                }
                buf.End();
                WriteBuf(buf);
            });
        budget -= installed;

        buf.Reset();
        buf.Add("поставлено перехватчиков: ");
        buf.AddDec(static_cast<std::uint32_t>(installed));
        buf.Add(" из ");
        buf.AddDec(static_cast<std::uint32_t>(T_Count));
        buf.End();
        WriteBuf(buf);

        /*
         * Узкая проба блокировок. Ставится ПОСЛЕ основной цепочки: хук на
         * pthread_rwlock_rdlock, встань он раньше, ловил бы и служебные
         * блокировки времени постановки остальных хуков.
         * Без ключа -DNXR_DIAG_LOCK этого раздела в модуле нет вовсе.
         */
#ifdef NXR_DIAG_LOCK
        WriteText("");
        WriteText("--- узкая проба блокировок (ключ -DNXR_DIAG_LOCK) ---");
        const Target* locks = LockTargets();
        const int lockInstalled = InstallTargets(
            locks, L_Count, budget,
            resolve,
            [](int index, std::uint64_t addr) { InstallOneLock(index, addr); },
            [&buf, locks](int index, std::uint64_t addr, bool done) {
                buf.Reset();
                buf.Add(done ? MarkOk : MarkFail);
                buf.Add(locks[index].human);
                buf.Add(" = ");
                if (done) {
                    buf.AddHex(addr);
                    buf.Add(" (перехвачен)");
                } else if (addr == 0) {
                    buf.Add("СИМВОЛ НЕ НАЙДЕН — не перехвачен, игре не мешаем");
                } else {
                    buf.Add("НЕ СТАВИМ: кончился бюджет трамплинов exlaunch");
                }
                buf.End();
                WriteBuf(buf);
            });
        budget -= lockInstalled;

        WriteText("nn::os::IsWriteLockHeldByCurrentThread найдена: ",
                  g_IsWriteHeld != nullptr ? "да" : "НЕТ (столбец «держит запись» "
                                                     "будет всегда 0)");
        WriteText("строки блокировок: замок, владелец записи [замок+0x18], "
                  "держит ли запись этот поток; пишутся ТОЛЬКО захваты внутри "
                  "eglCreateWindowSurface и только из того же потока.");
        /*
         * ВОТ ЗДЕСЬ, и только здесь, проба становится боевой. До этого места
         * перехватчик уже стоит, но видит m_Armed == 0 и прозрачно уходит в
         * оригинал: наши данные могут быть ещё не готовы.
         */
        if (lockInstalled == L_Count) {
            g_LockWindow.m_Armed = 1;
        } else {
            WriteText("проба блокировок ВЫКЛЮЧЕНА: перехватчик встал не полностью");
        }
#else
        WriteText("");
        WriteText("узкая проба блокировок: ВЫКЛЮЧЕНА (сборка без -DNXR_DIAG_LOCK)");
#endif

        /*
         * Проба SetError. Модуль GL опознаётся по экспорту
         * eglCreateWindowSurface: база = адрес - 0xe670 (так в 22.2.0). Хук
         * ставится, только если по смещению лежит ровно подпись 22.2.0 и бюджет
         * трамплинов не исчерпан.
         */
        WriteText("");
        WriteText("--- проба SetError: строка сообщения EGL называет ветку ---");
        {
            const std::uint64_t anchor = resolve(GlAnchorSym);
            const std::uint64_t glBase = anchor >= GlAnchorOffset ? anchor - GlAnchorOffset : 0;
            g_GlBase = glBase;
            buf.Reset();
            buf.Add("модуль GL: eglCreateWindowSurface = ");
            buf.AddHex(anchor);
            buf.Add(", база (адрес - 0xe670) = ");
            buf.AddHex(glBase);
            buf.End();
            WriteBuf(buf);
            int seInstalled = 0;
            for (int i = 0; i < S_Count; ++i) {
                const SetErrorTarget& t = kSetErrorTargets[i];
                const std::uint64_t addr = glBase != 0 ? glBase + t.offset : 0;
                const bool sigOk = addr != 0 && SetErrorSignatureMatches(
                    reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(addr)), i);
                const bool canInstall = sigOk && budget > 0;
                if (canInstall) {
                    InstallSetError(i, addr);
                    --budget;
                    ++seInstalled;
                }
                buf.Reset();
                buf.Add(canInstall ? MarkOk : MarkFail);
                buf.Add(t.human);
                buf.Add(" = ");
                if (canInstall) {
                    buf.AddHex(addr);
                    buf.Add(" (перехвачена, подпись GL 22.2.0 совпала)");
                } else if (addr == 0) {
                    buf.Add("модуль GL не найден — не перехвачена");
                } else if (!sigOk) {
                    buf.Add("ПОДПИСЬ НЕ СОВПАЛА (не GL 22.2.0?) — не перехвачена, игре не мешаем");
                } else {
                    buf.Add("НЕ СТАВИМ: кончился бюджет трамплинов exlaunch");
                }
                buf.End();
                WriteBuf(buf);
            }
            buf.Reset();
            buf.Add("строки «SetError#N код … «сообщение» в «функция»» идут в общем потоке "
                    "ниже, перед вызовом EGL, внутри которого случились; перехвачено ");
            buf.AddDec(static_cast<std::uint32_t>(seInstalled));
            buf.Add(" из ");
            buf.AddDec(static_cast<std::uint32_t>(S_Count));
            buf.End();
            WriteBuf(buf);
        }

        WriteText("eglGetError найден: ", g_EglGetError != nullptr ? "да" : "НЕТ");

        /*
         * Наши символы совместимости. Смотрим не «есть ли они у нас» (это видно
         * из сборки), а ЧЕЙ адрес отдаёт обход памяти: если подменённая sdk
         * своего такого символа не экспортирует, найдётся наш. Адрес рядом с
         * базой нашего модуля = сработал наш, далёкий = чужой.
         */
        WriteText("");
        WriteText("--- символы совместимости: по какому адресу они разрешились ---");
        {
            static const char* const compat[2] = {
                "_ZNSt3__113__hash_memoryEPKvm",
                "_ZN2nn3ldn11SetProtocolENS0_8ProtocolE",
            };
            for (int i = 0; i < 2; ++i) {
                const std::uint64_t a = resolve(compat[i]);
                buf.Reset();
                buf.Add(a != 0 ? MarkOk : MarkFail);
                buf.Add(compat[i]);
                buf.Add(" = ");
                if (a != 0) {
                    buf.AddHex(a);
                } else {
                    buf.Add("НЕ РАЗРЕШИЛСЯ НИГДЕ");
                }
                buf.End();
                WriteBuf(buf);
            }
        }

        /* Куда разрешился импорт NvRmMemQueryHandleParams; каждый вызов — дальше. */
        ReportNvrmResolution(symReport);
#ifdef NXR_COMPAT_NVRM_QUERY
        __atomic_store_n(&nxr::compat::g_NvrmNote, &NoteNvrmCall, __ATOMIC_RELEASE);
#endif

        WriteText("");
        WriteText("--- перехваченные вызовы графики (по порядку) ---");
        return true;
    }

    /*
     * ОТЧЁТ НА КАРТУ. Зовётся из OpenFsGate (src/main.cpp) — сразу после
     * открытия затвора файловой системы. До этого момента звать нельзя: здесь
     * монтируется SD (если ещё не) и создаётся файл.
     * Сначала шапка, затем ВСЁ накопленное до затвора — по порядку, затем
     * строка-разделитель; дальше строки идут в файл по мере событий.
     * Возвращает false, если отчёт завести не удалось; звать Die() отсюда
     * НЕЛЬЗЯ: у diag:: свой файл и свои коды аварий.
     */
    inline bool StartReport(ResolveFn resolve, const nxr::logs::Head& head) {
        if (g_CreateFile == nullptr || g_OpenFile == nullptr || g_WriteFile == nullptr) {
            return false;   /* Install не нашёл функций записи — отчёта не будет */
        }

        /*
         * SD к этому моменту уже смонтирована вызывающим (OpenFsGate в
         * src/main.cpp) под НАШИМ именем — и только ПОСЛЕ открытия затвора.
         * Пробуем ещё раз лишь на случай, если монтирование не удалось:
         * повторное монтирование своего же имени вернёт «уже смонтировано»
         * (0x7802), и это не помеха. Под общим именем "sd" не монтируем:
         * его может держать чужой мод (так было у UltraCam для TotK).
         * САМОСТОЯТЕЛЬНО звать это раньше затвора нельзя — именно такое раннее
         * монтирование ломало Well Dweller (Result 0x327602).
         */
        MountSdFn mount = reinterpret_cast<MountSdFn>(resolve("_ZN2nn2fs11MountSdCardEPKc"));
        if (mount != nullptr) {
            mount(NXR_SD_MOUNT);
        }

        if (!OpenReport(resolve, head.titleId)) {
            return false;
        }
        g_Clock.Init(resolve);

        {
            /* ПЕРВАЯ СТРОКА — привязка к игре: TitleID, BuildID main, номер. */
            Buf buf;
            buf.Reset();
            buf.m_Len = nxr::logs::FormatHead(buf.m_Buf, sizeof(buf.m_Buf) - 2, head,
                                              nxr::logs::KindPrefix(nxr::logs::GfxTemplate),
                                              g_Prepared.number);
            buf.End();
            WriteDirect(buf.m_Buf, buf.m_Len);
        }
        WriteTextDirect("nxredirect — отчёт о ГРАФИКЕ (сборка с -DNXR_DIAG_GFX)");
        WriteTextDirect("файл: ", ReportPath);
        {
            Buf buf;
            buf.Reset();
            buf.Add("отчётов gfx этой игры было: ");
            buf.AddDec(static_cast<std::uint32_t>(g_Prepared.found));
            buf.Add(", удалено старых: ");
            buf.AddDec(static_cast<std::uint32_t>(g_Prepared.deleted));
            buf.Add(g_Prepared.listed ? " (хранятся последние 5)"
                                      : " (папку прочитать не удалось — номер 1, перезапись)");
            buf.End();
            WriteDirect(buf.m_Buf, buf.m_Len);
        }
        if (!WriteTimeLine(false, true)) {
            WriteTextDirect("время: nn::time игрой ещё не запущен — строка времени будет "
                            "дописана на первых кадрах, когда игра его запустит");
            __atomic_store_n(&g_TimePending, 1u, __ATOMIC_RELEASE);
        }
        {
            Buf buf;
            buf.Reset();
            buf.Add("с включения консоли (счётчик тиков ядра): ");
            nxr::logs::Out o(buf.m_Buf + buf.m_Len, sizeof(buf.m_Buf) - buf.m_Len);
            nxr::logs::AddUptime(o, nxr::logs::device::Ticks());
            buf.m_Len += o.m_Len;
            buf.End();
            WriteDirect(buf.m_Buf, buf.m_Len);
        }
        WriteTextDirect("Первая строка, начинающаяся с '!!', — главный подозреваемый.");
        WriteTextDirect("");

        /* Слив раннего буфера и переключение на файл — под одним замком:
           строка из другого потока не встрянет посередине и не потеряется. */
        while (g_Lock.test_and_set(std::memory_order_acquire)) {
        }
        {
            Buf buf;
            buf.Reset();
            buf.Add("--- ранние события: записаны ДО открытия затвора ФС (перехватчики стоят "
                    "с хука nnMain), строк ");
            buf.AddDec(g_EarlyText.m_Lines);
            if (g_EarlyText.m_Dropped != 0) {
                buf.Add(", НЕ ПОМЕСТИЛОСЬ в буфер ");
                buf.AddDec(g_EarlyText.m_Dropped);
            }
            if (!g_EarlyText.CanaryOk()) {
                buf.Add(", !! КОНТРОЛЬНОЕ СЛОВО БУФЕРА ИСПОРЧЕНО");
            }
            buf.Add(" ---");
            buf.End();
            WriteFileLocked(buf.m_Buf, buf.m_Len);
            if (g_EarlyText.m_Len <= EarlyTextSize) {
                WriteFileLocked(g_EarlyText.m_Buf, g_EarlyText.m_Len);
            }
            __atomic_store_n(&g_ReportLive, 1u, __ATOMIC_RELEASE);
            DrainPendingLocked();
            const char tail[] = "--- конец ранних событий; дальше — по мере вызовов ---\r\n";
            WriteFileLocked(tail, sizeof(tail) - 1);
        }
        g_Lock.clear(std::memory_order_release);
        return true;
    }

}  // namespace gfx
}  // namespace nxr

#endif  /* NXR_GFX_DEVICE */
