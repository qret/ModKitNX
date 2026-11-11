/*
 * nxredirect — универсальный перенаправитель файлов игры Nintendo Switch
 * с romfs игры на SD-карту, БЕЗ LayeredFS.
 *
 * Copyright (c) 2026, участники проекта nxredirect
 * Основано на exlaunch (github.com/shadowninja108/exlaunch), лицензия GPL-2.0.
 * Этот файл распространяется на тех же условиях: GNU GPL версии 2.
 *
 * Как это работает
 * ----------------
 * Модуль собирается как свободный `subsdkN` (по умолчанию subsdk9, слот задаётся
 * параметром сборки MODULE_NAME) и кладётся в exefs-мод Atmosphère:
 * `atmosphere/contents/<TID>/exefs/subsdkN` + правленый `main.npdm`.
 * Loader грузит subsdk0..subsdk9; у игр обычно занят только subsdk0, поэтому
 * старший слот почти всегда свободен.
 *
 * ВАЖНО о моменте установки хука. ОТКРЫТЫЙ ВОПРОС:
 * готов ли модуль `sdk` к нашему init (exl_main). Записи расходятся: по порядку
 * загрузки (rtld, main, subsdk0..9, sdk) выходит «ещё нет»; по разбору кода rtld
 * TotK (см. блок NXR_EARLY_FS_HOOK ниже) инициализация идёт от старшего
 * адреса к младшему, и `sdk` инициализируется ПЕРВЫМ. Консолью порядок не проверен.
 * Факт с консоли (проект-предшественник): попытка поставить хук на
 * nn::fs::OpenFile прямо в exl_main ушла по нулевому адресу и уронила игру
 * («приложение закрыто с ошибкой»); почему адрес был нулевым — не выяснено.
 * Поэтому код делает так: в exl_main мы хукаем только nnMain — точку входа игры из модуля `main`,
 * который загружен раньше нас. А уже внутри nnMain (все модули загружены) находим
 * адреса nn::fs::OpenFile и nn::fs::MountSdCard поиском по символу и ставим хук на
 * открытие файлов. Если символ не найден — ничего не хукаем, игра идёт как обычно.
 *
 * Дальше: nn::fs::OpenFile — функция, через которую движок открывает ВСЕ файлы.
 * SD-карту монтируем под СВОИМ именем "nxrsd" (раньше — общим "sd", которое
 * занимают и чужие моды; см. nxr_path.hpp) лениво, при первом же OpenFile — но НЕ РАНЬШЕ,
 * чем откроется затвор файловой системы (src/nxr_fsgate.hpp): до этого момента
 * модуль не делает с nn::fs ни одного вызова. Иначе взводится флаг дефолтного
 * распределителя nn::fs, и игра падает на своём nn::fs::SetAllocator с
 * Result 0x327602 — ровно так наш модуль ломал Well Dweller.
 *
 * ЧТО ПОДМЕНЯЕТСЯ (границы, чтобы не задеть данные игрока):
 *   - только ЧТЕНИЕ: mode обязан быть ровно nn::fs::OpenMode_Read; открытие на
 *     запись/дозапись идёт в оригинал (nxr::IsRedirectableMode);
 *   - только данные игры: точки монтирования save/saveData/bcat/system/user/cache/
 *     temp/cal/host/sdmc/sd и любые "@…" не подменяются никогда
 *     (nxr::IsBlockedMount); "content" — только если под этим именем на глазах
 *     модуля смонтирована romfs (перехват nn::fs::MountRom, nxr::RomMounts,
 *     раньше "content" был в чёрном списке безусловно).
 * Если путь начинается с
 * ЛЮБОГО другого префикса вида "имя:/" (кроме нашего "nxrsd:/"), пробуем сначала открыть
 * "nxrsd:/atmosphere/contents/<TID>/files/<остаток>"; получилось — отдаём этот файл,
 * не получилось — открываем оригинал. Игра без файлов на SD работает как обычно.
 *
 * Почему не нужен LayeredFS: fs.mitm строит таблицу romfs только когда в
 * contents/<TID>/ есть romfs.bin или НЕПУСТАЯ romfs/ (HasSdRomfsContent,
 * amsmitm_fs_utils.cpp — проверено по исходнику Atmosphère: проверяются ровно эти
 * два пути, произвольные подпапки не считаются, поэтому подпапка `files/` таблицу
 * НЕ включает). У нас в contents/<TID>/ только exefs/ и files/, поэтому 12-МБ куча
 * ams_mitm не тратится и тяжёлые игры не зависают.
 *
 * ------------------------------------------------------------------------------
 * Шесть исправлений, без которых модуль не работает (каждое подтверждено пробой на
 * консоли; коды ошибок — ниже, у каждого пункта):
 *   1. crt0.s: слово по +8 от начала модуля — честное смещение на структуру
 *      module-runtime-info. В шаблоне exlaunch там было выравнивание (NOP), rtld
 *      игры принимал NOP за смещение и ронял процесс (Data Abort в rtld+0x1fb8).
 *      → exlaunch/source/lib/init/crt0.s.
 *   2. Права npdm: в ACI0 и ACID дописываются svc 0x74/0x75 (Map/UnmapProcessMemory —
 *      их требует установка хуков) и бит SdCard в правах FS. У игр разрешены только
 *      группы вызовов 0..3, поэтому без правки — «Undefined System Call».
 *      → scripts/npdm_syscalls.py, scripts/npdm_sdcard.py.
 *   3. Собственный перебор .dynsym вместо DT_HASH: ModuleObject::GetSymbolByName из
 *      шаблона ищет через DT_HASH, а у модулей игр на nnSdk есть только DT_GNU_HASH —
 *      указатель на корзины нулевой, и поиск падает. → nxr_symfind.hpp, FindSymbolIn.
 *   4. Собственный обход модулей через svcQueryMemory: сканер exlaunch ждёт у модуля
 *      ровно три области подряд, а у nnSdk их больше (часть данных после релокации
 *      переводится в relro), из-за чего nnSdk теряется и ни одна nn::fs-функция не
 *      находится (код 0xD1040400: 4 модуля из 5). → nxr_symfind.hpp, ForEachModule.
 *   5. Предел смещения MOD0: MOD0 лежит не в области кода, а в rodata сразу за ней,
 *      поэтому ограничивать смещение размером области кода нельзя (код 0xDEAD0001 —
 *      ни один модуль не распознан). → nxr_symfind.hpp, Limits::maxMod0Offset.
 *   6. Правильный префикс пути игры: движок открывает файлы не через "rom:/", а через
 *      своё имя точки монтирования ("app0:/" у Trails, бывает "rom:/", "asset:/",
 *      "Contents:/"). Пока проверялся только "rom:/", перенаправление молча не
 *      срабатывало. → BuildSdPath принимает ЛЮБОЙ префикс вида "имя:/".
 * ------------------------------------------------------------------------------
 */

#include "lib.hpp"

#include <atomic>

#include "nn/fs.hpp"
#include "rtld.hpp"

/*
 * Обход модулей процесса и поиск символа в .dynsym вынесены в отдельный файл:
 * ровно этот код прогоняется на ПК хост-тестом (hosttest/symfind_test.cpp)
 * по настоящим модулям игры со смоделированной раскладкой памяти.
 */
#include "nxr_symfind.hpp"
#include "nxr_rofix.hpp"   /* поиск nn::ro в старой sdk без DT_GNU_HASH */

/*
 * Построение пути мода и подмена префикса тоже вынесены отдельно и тоже прогоняются
 * на ПК — hosttest/path_test.cpp. Именно в этой логике прежний проект ошибался
 * (фиксированный префикс "rom:/" — подмена молча не работала).
 */
#include "nxr_path.hpp"

/*
 * ЗАТВОР ФАЙЛОВОЙ СИСТЕМЫ. Модуль не трогает nn::fs, пока игра не задала свой
 * распределитель (nn::fs::SetAllocator) либо сама впервые не обратилась к
 * nn::fs. Причина — проба на консоли: раннее монтирование SD взводило флаг
 * дефолтного распределителя, и игра падала на своём SetAllocator с
 * Result 0x327602. Вся чистая логика и её разбор — в nxr_fsgate.hpp, она же
 * прогоняется на ПК хост-тестом hosttest/fsgate_test.cpp.
 */
#include "nxr_fsgate.hpp"

/*
 * Расхождения чужой `sdk`: std::__1::__hash_memory, nn::ldn::SetProtocol,
 * NvRmMemQueryHandleParams. Теперь — АВТОИСПРАВЛЕНИЕ: все три экспорта
 * есть в каждой сборке, но каждый — прокси с запасом: при первом вызове
 * смотрит, что лежит в загруженной `sdk`, и либо зовёт её настоящую функцию
 * (родная библиотека — поведение как без мода), либо работает своей
 * реализацией (подменённая библиотека). Правило — nxr_autofix.hpp, каталог —
 * nxr_autofix_catalog.h, экспорты — nxr_compat_exports.hpp; выключается ключом
 * -DNXR_NO_AUTOFIX. *Было раньше:* «ПО УМОЛЧАНИЮ НЕ ЭКСПОРТИРУЮТСЯ,
 * включаются ключами сборки -DNXR_COMPAT_*» — ключ перекрывал настоящие
 * функции на консоли с родной библиотекой; прокси эту опасность снимает.
 * Свои реализации сверяются на ПК с настоящим кодом SDK (run_hashtest.py,
 * run_nvrmtest.py), выбор «своя/прокси» — на настоящих библиотеках в эмуляторе
 * (run_autofix.py).
 */
#include "nxr_compat.hpp"
#include "nxr_compat_exports.hpp"

/*
 * ОТЧЁТЫ ОТЛАДОЧНЫХ СБОРОК не в корень карты,
 * а по играм — sd:/switch/ModKitNX/logs/<TitleID>/diag-NNNN.txt и gfx-NNNN.txt,
 * последние 5 каждого вида. Раскладка, ротация и первая строка — nxr_logs.hpp
 * (прогоняется на ПК: hosttest/logs_test.cpp). BuildID main берётся тем же
 * разбором rodata, что и у паспорта (nxr_autofix.hpp — чистый заголовок,
 * в сборке без автоисправления подключается только ради него).
 */
#if defined(NXR_DIAG) || defined(NXR_DIAG_GFX)
#  define NXR_LOGS_DEVICE 1
#  include "nxr_logs.hpp"
#  include "nxr_autofix.hpp"
#endif

/*
 * Диагностика ГРАФИЧЕСКОГО пути (чёрный экран Well Dweller на подменённой sdk).
 * Ключ сборки -DNXR_DIAG_GFX; без него ни одного байта этого файла в модуле нет,
 * и обычная сборка совпадает с прежней по всему исполняемому содержимому —
 * .text, .rodata и .data байт в байт, различается только 20-байтовый build-id,
 * который линкер выдаёт новый при каждой сборке (hosttest/gfx_unchanged.py).
 * Чистая часть (формат строки
 * отчёта и раскладка целей перехвата) прогоняется на ПК — hosttest/gfx_test.cpp.
 */
#ifdef NXR_DIAG_GFX
#  define NXR_GFX_DEVICE 1
#  include "nxr_gfx.hpp"
#endif

/*
 * В шаблоне exlaunch объявлен символ nn::ro::detail::g_LookupGlobalManualFunctionPointer
 * (старое имя из nnSdk). nnSdk современных игр экспортирует уже переименованный
 * g_pLookupGlobalManualFunctionPointer, поэтому старое имя осталось бы
 * неразрешённым импортом, а код source/rtld/ModuleObject.cpp его читает.
 * Определяем его у себя нулём: "ручной" поиск символов просто выключен — он и не
 * нужен, мы грузимся обычным модулем, релокации делает rtld игры.
 * После этого у модуля не остаётся ни одного импорта, который игра не экспортирует.
 */
namespace nn::ro::detail {
    rtld::lookup_global_t g_LookupGlobalManualFunctionPointer = nullptr;

    /*
     * И g_pAutoLoadList: его экспортирует модуль `sdk`, который грузится ПОЗЖЕ нашего,
     * поэтому импорт остался бы неразрешённым (адрес 0). Мы этим списком не пользуемся
     * (символы ищем сами), но код rtld из шаблона на него ссылается.
     * Определяем свой ЗАВЕДОМО ПУСТОЙ список: обход такого списка сразу возвращает 0,
     * а не уходит по нулевому адресу.
     */
    rtld::ModuleObjectList g_pAutoLoadList = {
        reinterpret_cast<rtld::ModuleObject*>(&g_pAutoLoadList),
        reinterpret_cast<rtld::ModuleObject*>(&g_pAutoLoadList),
    };
}

namespace {

    /*
     * Имя точки входа игры (экспорт модуля `main`). Ищем по имени, а НЕ импортом:
     * неразрешённый импорт на этапе init означает обращение по нулевому адресу и
     * падение процесса. У модуля не должно быть ни одного импорта.
     */
    constexpr char SymNnMain[] = "nnMain";

    /*
     * Поиск символа в модулях игры БЕЗ участия nnSdk.
     * Сама логика (обход областей памяти, разбор .dynamic и .dynsym) живёт в
     * nxr_symfind.hpp — её же слово в слово прогоняет хост-тест на ПК.
     * Здесь только переходник: запрос памяти у ядра через svcQueryMemory.
     *
     * Готовый список модулей exlaunch не годится (исправление №4): его сканер ждёт у
     * модуля ровно три области подряд, а у nnSdk их больше (relro), и nnSdk в список
     * не попадает — проба на консоли, код 0xD1040400 (4 модуля из 5).
     * Использовать rtld::lookup_global_auto тоже нельзя: он обращается к переменной
     * nn::ro::detail::g_pAutoLoadList, которую экспортирует `sdk`, — на этапе init
     * её адрес не разрешён (из-за этого игра и падала на консоли).
     */
    struct MemQuery {
        bool operator()(u64 addr, nxr::MemRegion* out) const {
            MemoryInfo info {};
            u32 pageInfo = 0;
            if (R_FAILED(svcQueryMemory(&info, &pageInfo, addr))) {
                return false;
            }
            out->addr = info.addr;
            out->size = info.size;
            out->type = info.type;
            out->perm = info.perm;
            return true;
        }
    };

    /* Один экземпляр на всё: FindSymbolAnywhere принимает ссылку. */
    MemQuery g_MemQuery {};

#ifndef NXR_NO_AUTOFIX
    /* Поиск символов nn::ro в старой sdk (nxr_rofix.hpp, ApplyRoFix):
       0 — не проверяли, 1 — не нужно, 2 — поставлен, 3 — образец не совпал. */
    int g_RoFix = 0;

    const char* RoFixText() {
        switch (g_RoFix) {
            case 1: return "не нужно (sdk знает DT_GNU_HASH или у всех модулей есть DT_HASH)";
            case 2: return "СВОЙ — sdk ищет только по DT_HASH, у модулей игры его нет: поиск nn::ro подменён";
            case 3: return "НЕ ПОСТАВЛЕН — код nn::ro::LookupModuleSymbol не узнан";
            default: return nullptr;
        }
    }
#endif

    /* Поиск символа во ВСЕХ модулях процесса. Возвращает адрес или 0. */
    uintptr_t FindSymbolAnywhere(const char* name) {
        return static_cast<uintptr_t>(nxr::FindSymbolAnywhere(g_MemQuery, name));
    }

#ifdef NXR_DIAG_GFX
    /* Тот же поиск, но обычной функцией с фиксированным типом: диагностика
       графики принимает его указателем. (Раньше им пользовалось и
       исправление NvRm; теперь оно ищет сам модуль `sdk` — nxr_autofix.hpp.) */
    std::uint64_t GfxResolveSymbol(const char* name) {
        return nxr::FindSymbolAnywhere(g_MemQuery, name);
    }
#endif

#ifdef NXR_DIAG
    /* Поиск символа для nxr::logs::device (nn::fs каталогов, nn::time) в diag::.
       Графика берёт тот же поиск через GfxResolveSymbol. */
    std::uint64_t LogsResolveSymbol(const char* name) {
        return nxr::FindSymbolAnywhere(g_MemQuery, name);
    }
#endif

#ifdef NXR_COMPAT_NVRM_QUERY
    /*
     * ИСПРАВЛЕНИЕ NvRmMemQueryHandleParams ДЛЯ ПОДМЕНЁННОЙ sdk.
     * Основной путь — наш экспорт (nxr_compat.hpp); здесь — проверка в рантайме,
     * выиграл ли он, и запасной путь, если нет. Подробно — у DecideNvrmPath.
     */
    struct NvrmState {
        std::uint64_t ours        = 0;
        std::uint64_t importer    = 0;
        std::uint64_t slot        = 0;
        std::uint64_t bound       = 0;
        std::uint64_t sdkDef      = 0;
        std::uint64_t rtldWinner  = 0;
        int           path        = 0;
        bool          hooked      = false;
    };
    NvrmState g_Nvrm {};

    /* База модуля, в котором лежит адрес (по нашему обходу), или 0. */
    std::uint64_t ModuleBaseOf(std::uint64_t addr) {
        std::uint64_t best = 0;
        nxr::ForEachModule(g_MemQuery, [&](std::uint64_t base, const nxr::Dyn*) {
            if (base <= addr && base > best) {
                best = base;
            }
            return false;
        });
        return best;
    }

    void ApplyNvrmFix() {
        using namespace nxr::compat;
        /*
         * Решение автоисправления — ДО любой правки кода. Порядок
         * важен: после замены заглушки переходом на нас по адресу sdk лежала бы
         * уже не заглушка, и расчёт, сделанный позже, принял бы её за настоящую
         * функцию и проксировал бы в неё — то есть в самих себя. Решение
         * публикуется один раз (nxr::autofix::ResolveOnce), дальше только читается.
         */
        const nxr::autofix::Resolution decision =
            nxr::autofix::Resolve(nxr::autofix::FixNvrmQuery);
        const bool ownChosen = decision.choice == nxr::autofix::Choice::Own;
        g_Nvrm.ours = reinterpret_cast<std::uint64_t>(&nxr_compat_nvrm_query);
        const std::uint64_t selfBase = ModuleBaseOf(g_Nvrm.ours);

        const nxr::ImportSlot imp = nxr::FindImportSlotAnywhere(g_MemQuery, SymNvrmQuery);
        g_Nvrm.importer = imp.moduleBase;
        g_Nvrm.slot     = imp.slot;
        g_Nvrm.bound    = imp.slot != 0
            ? *reinterpret_cast<const volatile std::uint64_t*>(static_cast<uintptr_t>(imp.slot))
            : 0;
        g_Nvrm.rtldWinner = nxr::RtldWinnerOf(g_MemQuery, SymNvrmQuery);
        nxr::ForEachDefinitionOf(g_MemQuery, SymNvrmQuery,
                                 [&](std::uint64_t base, std::uint64_t addr) {
            if (base != selfBase && g_Nvrm.sdkDef == 0) {
                g_Nvrm.sdkDef = addr;
            }
        });

        const bool stub = g_Nvrm.sdkDef != 0 && IsNvrmStub(
            reinterpret_cast<const std::uint32_t*>(static_cast<uintptr_t>(g_Nvrm.sdkDef)),
            g_Nvrm.sdkDef);
        const NvrmPath path = DecideNvrmPath(g_Nvrm.bound, g_Nvrm.ours, g_Nvrm.sdkDef,
                                             g_Nvrm.rtldWinner, stub, ownChosen);
        g_Nvrm.path = static_cast<int>(path);
        if (path == NvrmPath::HookStub) {
            /*
             * Замена заглушки: хук БЕЗ трамплина (пул exlaunch не тратится).
             * Рядом с модулем — одна инструкция `b`, далеко — 4 инструкции
             * (адрес кратен 8: hook_impl.cpp берёт 4, а не 5) — ровно 16 байт
             * заглушки `mov w0,#2; ret; udf; udf`, соседнюю функцию не задевает.
             * Цель — сама наша функция, без обёртки: адрес возврата в её записи
             * остаётся адресом в GL.
             */
            exl::hook::Hook(static_cast<uintptr_t>(g_Nvrm.sdkDef), &nxr_compat_nvrm_query);
            g_Nvrm.hooked = true;
        }
    }
#endif

#ifdef NXR_DIAG_GFX
    /* Сведения об импорте для отчёта графики (nxr::gfx::SymbolReportFn). */
    bool GfxSymbolReport(const char* name, nxr::gfx::SymbolReport* out) {
        if (out == nullptr) {
            return false;
        }
        *out = nxr::gfx::SymbolReport {};
        const nxr::ImportSlot imp = nxr::FindImportSlotAnywhere(g_MemQuery, name);
        out->importerBase = imp.moduleBase;
        out->slot         = imp.slot;
        out->bound        = imp.slot != 0
            ? *reinterpret_cast<const volatile std::uint64_t*>(static_cast<uintptr_t>(imp.slot))
            : 0;
        out->rtldWinner   = nxr::RtldWinnerOf(g_MemQuery, name);
        nxr::ForEachDefinitionOf(g_MemQuery, name, [&](std::uint64_t base, std::uint64_t addr) {
            if (out->defCount < 4) {
                out->defBases[out->defCount] = base;
                out->defs[out->defCount]     = addr;
                ++out->defCount;
            }
        });
#ifdef NXR_COMPAT_NVRM_QUERY
        out->ours     = g_Nvrm.ours;
        out->selfBase = ModuleBaseOf(g_Nvrm.ours);
        out->path     = g_Nvrm.path;
        out->hooked   = g_Nvrm.hooked;
#else
        /* Без исправления своего экспорта нет, и «наше» определение искать не
           надо: все найденные определения — чужие (sdk). */
        out->selfBase = 0;
#endif
        return true;
    }
#endif

    /*
     * Имена символов nnSdk в том виде, как их экспортирует модуль `sdk` игры
     * (проверено разбором .dynsym: у Trails the 2nd OpenFile @0xb8de0,
     * MountSdCard @0xfa6b0). Манглинг у всех игр на nnSdk один и тот же.
     * Адреса ищем во время работы, а не линкуемся импортом: импорт в нашем модуле
     * на момент init ещё не разрешён.
     */
    constexpr char SymOpenFile[]    = "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci";
    constexpr char SymMountSdCard[] = "_ZN2nn2fs11MountSdCardEPKc";
    /*
     * nn::fs::SetAllocator — точка, после которой нам МОЖНО работать с nn::fs.
     * Манглинг проверен по .dynsym настоящих библиотек `sdk` из дампов:
     * nnSdk 19.3.13 @0xdc810, 20.5.17 @0xde1d0, 22.2.0 @0xe8ab0 — имя во всех
     * трёх одно и то же. Сигнатура:
     *   void SetAllocator(void* (*alloc)(size_t), void (*dealloc)(void*, size_t))
     */
    constexpr char SymSetAllocator[] = "_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE";

    /*
     * МОНТИРОВАНИЕ ROMFS ИГРЫ. Модуль сам узнаёт, под каким именем
     * смонтирована romfs игры, — и игрой, и чужим модом (UltraCam для TotK зовёт
     * MountRom("content")). Имя попадает в реестр nxr::RomMounts (nxr_path.hpp);
     * по нему подменяются двусмысленные имена ("content"), которые иначе защищены
     * как возможный nn::fs::MountContent чужого NCA.
     *
     * Перехватываются ровно те варианты, что монтируют romfs СВОЕЙ программы:
     *   MountRom(const char*)                — без кеша (им пользуется UltraCam);
     *   MountRom(const char*, void*, ulong)  — с кешем (им пользуется сама TotK).
     * Не перехватываются: MountRom(name, ApplicationId, …) — romfs ДРУГОГО
     * приложения; MountRom(name, int index, …) — romfs другой программы того же
     * приложения (у неё свой program id и своя папка LayeredFS); MountRomOnFile —
     * romfs из файла. Это не данные игры в смысле папки files/.
     * Оба публичных варианта внутри зовут один безымянный MountRomImpl
     * (TotK sdk 15.3.1: 0xd2bf0 и 0xd2f00 -> bl 0xd3b00), символа у него нет —
     * поэтому хуков два. Манглинг проверен по .dynsym sdk TotK 15.3.1, SiN/WD
     * 20.x и nnSdk 19.3.13 — один и тот же; не нашёлся — хук просто не ставится.
     *
     * nn::fs::Unmount(name) — имя вычёркивается: после него под тем же именем
     * может появиться MountContent, и подменять его уже нельзя.
     */
    constexpr char SymMountRom[]      = "_ZN2nn2fs8MountRomEPKc";
    constexpr char SymMountRomCache[] = "_ZN2nn2fs8MountRomEPKcPvm";
    constexpr char SymUnmount[]       = "_ZN2nn2fs7UnmountEPKc";

    /* Типы функций nnSdk, которые вызываем по найденному адресу. */
    using OpenFileFn    = Result (*)(nn::fs::FileHandle*, char const*, int);
    using MountSdCardFn = Result (*)(char const*);
    using FsAllocFn     = void* (*)(size_t);
    using FsDeallocFn   = void  (*)(void*, size_t);

    MountSdCardFn g_MountSdCard = nullptr;

    /*
     * Затвор файловой системы и адреса, которые понадобятся отложенной работе.
     * Адреса находим рано (поиск символа памяти не выделяет и nn::fs не трогает),
     * а пользуемся ими только после открытия затвора.
     */
    nxr::FsGate g_FsGate {};
    uintptr_t   g_AddrNnMain      = 0;
    uintptr_t   g_AddrOpenFile    = 0;
    uintptr_t   g_AddrMountSdCard = 0;
    /* Нашёлся ли символ SetAllocator: если нет, затвор откроется запасным путём. */
    bool        g_HaveSetAllocator = false;
    /*
     * Импортирует ли хоть один модуль игры nn::fs::SetAllocator, то есть будет
     * ли игра его звать. От этого зависит, можно ли открывать затвор запасным
     * путём (по первому OpenFile) — см. nxr::DecideOpenFile.
     * Проверено разбором дампов: Well Dweller — импортирует (main), SiN Reloaded —
     * не импортирует ни в одном из main, subsdk0..3.
     */
    bool        g_GameCallsSetAllocator = false;
    /*
     * Встал ли хук на nn::fs::OpenFile ЕЩЁ В exl_main (ключ сборки
     * -DNXR_EARLY_FS_HOOK, по умолчанию выключен). Нужен, чтобы хук nnMain не
     * ставил второй трамплин на тот же адрес.
     */
    bool        g_EarlyHookInstalled = false;
    /* То же для перехватчиков MountRom/Unmount (ставятся вместе с OpenFile). */
    bool        g_MountHooksInstalled = false;

    /*
     * Имена, под которыми на глазах модуля смонтирована romfs игры.
     * Пишут перехватчики MountRom/Unmount, читает хук OpenFile (BuildSdPathWhy).
     * Сам реестр к nn::fs не обращается и памяти не выделяет — его можно
     * заполнять и до открытия затвора ФС.
     */
    nxr::RomMounts g_RomMounts {};

    /*
     * Имя точки монтирования SD — СВОЁ: nxr::SdMountName = "nxrsd" (nxr_path.hpp).
     * Раньше здесь стояло общее "sd": чужой мод (UltraCam для TotK)
     * монтировал "sd" сам, наш MountSdCard получал 0x7802 «имя занято», и
     * подмена выключалась до конца сессии. Путь с нашим префиксом мы не
     * перенаправляем (self-guard в BuildSdPathWhy) — имя одно, в одном месте.
     */
    using nxr::SdMountName;

    /*
     * ПУТЬ МОДА СОБИРАЕТСЯ В РАНТАЙМЕ (решение гибрида №1 и №2).
     *
     * Почему не жёсткий путь и не плейсхолдер в бинарнике:
     *   - жёсткий путь (`sd:/<имя мода>/`) привязывает модуль к одной игре и не даёт
     *     складывать всё в одну удаляемую папку;
     *   - плейсхолдер из 16 нулей, который постсборочный скрипт заменяет на TitleID,
     *     не работает вовсе: сегменты NSO сжаты LZ4, и строки в готовом файле просто
     *     нет — заменять нечего (проверено поиском строки в собранном NSO).
     * Поэтому TitleID берём у ядра во время работы: svcGetInfo(InfoType_ProgramId=18,
     * CUR_PROCESS_HANDLE). Вызов 0x29 (GetInfo) играм разрешён — проверено разбором
     * прав main.npdm игры (в ACI0 и ACID разрешены группы 0..3, номер 0x29 входит).
     * Один и тот же бинарник годится для любой игры.
     *
     * Папка мода — внутри каталога самой игры в Atmosphère:
     *   sd:/atmosphere/contents/<TID 16 hex ЗАГЛАВНЫМИ>/files/
     * Рядом с exefs/, куда кладётся сам модуль: весь мод удаляется одной папкой.
     * Подпапка `files` LayeredFS не включает (см. HasSdRomfsContent в шапке файла).
     *
     * Сама сборка строки и подмена префикса живут в nxr_path.hpp — их прогоняет
     * хост-тест на ПК.
     */

    /* Готовый префикс пути мода. Собирается один раз в exl_main. */
    char   g_SdPrefix[nxr::ModPrefixSize] = {};
    size_t g_SdPrefixLen = 0;
    u64    g_TitleId     = 0;

    /* Максимальная длина пути в nn::fs (с учётом нуля). Буфер живёт на стеке. */
    constexpr size_t PathBufferSize = 0x301;

    /*
     * Спрашивает у ядра TitleID своего процесса и собирает из него префикс пути мода.
     * Возвращает false, если ядро отказало — тогда перенаправления просто не будет.
     */
    bool InitSdPrefix() {
        u64 tid = 0;
        if (R_FAILED(svcGetInfo(&tid, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0))) {
            return false;
        }
        if (tid == 0) {
            return false;
        }
        g_TitleId = tid;
        g_SdPrefixLen = nxr::BuildModPrefix(g_SdPrefix, sizeof(g_SdPrefix), tid);
        return g_SdPrefixLen != 0;
    }

#if defined(NXR_DIAG) || defined(NXR_DIAG_GFX)
    /*
     * Сведения для ПЕРВОЙ строки отчётов: TitleID и BuildID модуля main игры.
     * BuildID — GNU build-id из хвоста rodata main (заметка "GNU", тип 3); он же
     * — первые 20 байт ModuleId заголовка NSO (сверено на main
     * Trails, TotK, SiN, WD: заметка лежит за 0x24 байта до конца rodata).
     * Только чтение памяти; зовётся после затвора ФС, из OpenFsGate.
     */
    nxr::logs::Head MakeLogHead() {
        nxr::logs::Head h {};
        h.titleId = g_TitleId;
        if (g_AddrNnMain != 0) {
            const std::uint64_t mainBase = nxr::autofix::ModuleBaseOf(g_MemQuery, g_AddrNnMain);
            const unsigned char* tail = nullptr;
            std::uint64_t tailLen = 0;
            if (mainBase != 0 && nxr::autofix::RodataTail(g_MemQuery, mainBase, &tail, &tailLen)) {
                h.haveBuildId = nxr::autofix::FindBuildId(tail, tailLen, h.buildId);
            }
        }
        return h;
    }
#endif

    /*
     * Состояние монтирования SD. OpenFile игра зовёт из нескольких потоков, поэтому
     * само монтирование выполняется ровно один раз: право на попытку забирает один
     * поток через compare_exchange (nxr::SdMount, nxr_fsgate.hpp) — и в ленивом
     * пути, и в OpenFsGate. Без этого второй поток вызвал бы MountSdCard повторно,
     * получил бы ошибку "уже смонтировано" и навсегда выключил перенаправление.
     * Раньше OpenFsGate забирал право без compare_exchange — гонка с
     * EnsureSdMounted из другого потока была возможна (разбор в nxr_fsgate.hpp).
     */
    nxr::SdMount g_SdMount;

    /*
     * Единственное место в модуле, откуда вызывается nn::fs::MountSdCard.
     * Звать ТОЛЬКО через g_SdMount (право забрано) и при открытом затворе ФС.
     *
     * На время вызова состояние — InProgress: MountSdCard внутри сама открывает
     * файлы, и наш хук OpenFile не должен уйти во второе монтирование.
     */
    Result CallMountSdCard() {
        if (g_MountSdCard == nullptr) {
            /* Кода «функции нет» в nn::fs не бывает — отдаём свой узнаваемый. */
            return static_cast<Result>(0xDEAD0003);
        }
        /*
         * Отказ — это отказ, в том числе 0x7802 («имя уже занято»): считать его
         * успехом нельзя, мы читали бы чужую точку монтирования вслепую. Своё
         * имя (nxrsd) делает такой конфликт невозможным, а не «обработанным».
         */
        return g_MountSdCard(SdMountName);
    }

    /*
     * Монтирование из OpenFsGate: итог единственной попытки. Если её прямо сейчас
     * делает другой поток (EnsureSdMounted), ждём исхода короткими паузами.
     */
#if defined(NXR_DIAG) || defined(NXR_DIAG_GFX) || (defined(NXR_AUTOFIX) && !defined(NXR_NO_PASSPORT))
    /* То же условие, что у единственного вызова в OpenFsGate: без него (сборка
       --no-autofix) функция не используется, а -Werror=unused-function роняет сборку. */
    Result MountSdOnce() {
        return static_cast<Result>(g_SdMount.ForGate(
            [] { return static_cast<std::uint32_t>(CallMountSdCard()); },
            [] { svcSleepThread(1'000'000); /* 1 мс */ }));
    }
#endif

    /*
     * Возвращает true, если SD смонтирована. Монтируем ЛЕНИВО — при первом OpenFile,
     * а не при инициализации модуля: внутрь OpenFile мы попадаем уже из самого nnSdk,
     * значит, `sdk` загружен и его функции вызывать безопасно.
     * Вложенный вызов OpenFile из самой MountSdCard в рекурсию не уйдёт: на время
     * попытки состояние — InProgress, и такой вызов просто пройдёт без перенаправления.
     */
    bool EnsureSdMounted() {
        /*
         * ГЛАВНАЯ ЗАЩИТА ПОРЯДКА. Пока затвор закрыт, монтировать SD нельзя:
         * MountSdCard выделяет память через дефолтный распределитель nn::fs и
         * взводит флаг, из-за которого SetAllocator игры уходит в Abort
         * (Result 0x327602). Подробности — в nxr_fsgate.hpp.
         */
        if (!g_FsGate.IsOpen()) {
            return false;
        }

        /* Право на единственную попытку забирает один поток (compare_exchange);
           при чужой попытке — не ждём, отвечаем «не смонтировано». */
        return g_SdMount.Ensure(
            [] { return static_cast<std::uint32_t>(CallMountSdCard()); });
    }

    /*
     * Собирает "nxrsd:/atmosphere/contents/<TID>/files/<остаток>" из пути игры.
     * Вся логика (любой префикс "имя:/", защита от собственного "nxrsd:/", пределы
     * длины) живёт в nxr_path.hpp и проверяется хост-тестом на ПК; здесь — только
     * подстановка собранного префикса и имени точки монтирования.
     */
    /*
     * Причина отказа (`why`) нужна диагностическому отчёту: без неё «подменено НЕТ»
     * не отличить от блокированной точки монтирования и от режима записи. В обычной
     * сборке значение просто не печатается.
     */
    bool BuildSdPathWhy(char* out, size_t outSize, const char* gamePath,
                        nxr::SkipReason* why) {
        return nxr::BuildSdPathWhy(out, outSize, g_SdPrefix, g_SdPrefixLen,
                                   gamePath, SdMountName, why, &g_RomMounts);
    }

#ifdef NXR_DIAG
    /*
     * ============================ ДИАГНОСТИКА ============================
     * Включается ТОЛЬКО сборкой с -DNXR_DIAG. В обычной сборке всего этого блока
     * в модуле нет (и check_nso.py следит, чтобы сборки не перепутали).
     *
     * Задача: за один запуск игры ответить, почему подмена не работает.
     * Отчёт: `sd:/switch/ModKitNX/logs/<TitleID>/diag-NNNN.txt`,
     * новый на каждый запуск, последние 5 на игру (nxr_logs.hpp). Прежние сборки писали
     * один `sd:/nxredirect-diag.txt` в корне, перезаписывался любой игрой.
     *
     * Если писать отчёт некуда (SD не смонтировалась, символы не нашлись), сказать
     * об этом иначе нельзя — поэтому модуль намеренно падает по УЗНАВАЕМОМУ адресу,
     * и код виден в crash report Atmosphère (поле Address). Коды — в enum Crash.
     */
    namespace diag {

        /* Путь отчёта собирается при открытии (номер — из папки игры). Префикс —
           наша точка монтирования (nxrsd); на карте это sd:/switch/ModKitNX/logs/…
           Папка — по TitleID, а не по пути мода: отчёт нужен и тогда, когда путь
           мода не собрался (TitleID без него в diag не бывает — Crash_NoTitleId). */
        char                    ReportPath[nxr::logs::PathSize] = {};
        nxr::logs::Prepared     g_Prepared {};
        nxr::logs::Head         g_Head {};
        nxr::logs::device::Clock g_Clock {};
        /* Строка времени ещё не записана: дописать, когда игра запустит nn::time. */
        std::atomic<bool>       g_TimePending { false };

        /* Сколько путей OpenFile записать. Дальше файл закрывается. */
        constexpr int MaxLoggedPaths = 60;

        /*
         * Манглинг функций записи в nnSdk. Проверено разбором .dynsym модуля `sdk`
         * (у Trails the 2nd: CreateFile @0xb7bc0, DeleteFile @0xb7bd0,
         * WriteFile @0xb7280, FlushFile @0xb73b0, CloseFile @0xb6ed0 — все DEF).
         */
        constexpr char SymCreateFile[] = "_ZN2nn2fs10CreateFileEPKcl";
        constexpr char SymDeleteFile[] = "_ZN2nn2fs10DeleteFileEPKc";
        constexpr char SymWriteFile[]  = "_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE";
        constexpr char SymFlushFile[]  = "_ZN2nn2fs9FlushFileENS0_10FileHandleE";
        constexpr char SymCloseFile[]  = "_ZN2nn2fs9CloseFileENS0_10FileHandleE";

        using CreateFileFn = Result (*)(char const*, s64);
        using DeleteFileFn = Result (*)(char const*);
        using WriteFileFn  = Result (*)(nn::fs::FileHandle, s64, void const*, u64,
                                        nn::fs::WriteOption const&);
        using FlushFileFn  = Result (*)(nn::fs::FileHandle);
        using CloseFileFn  = void   (*)(nn::fs::FileHandle);

        CreateFileFn g_CreateFile = nullptr;
        DeleteFileFn g_DeleteFile = nullptr;
        WriteFileFn  g_WriteFile  = nullptr;
        FlushFileFn  g_FlushFile  = nullptr;
        CloseFileFn  g_CloseFile  = nullptr;
        /* Оригинал OpenFile: берём ДО установки хука, чтобы не попасть в себя же. */
        OpenFileFn   g_OpenFile   = nullptr;

        /* Коды аварий. Именно они видны в crash report как адрес обращения. */
        enum Crash : u32 {
            Crash_NoNnMain     = 0xDEAD0001,  /* не найден символ nnMain в модуле main */
            Crash_NoFsSymbols  = 0xDEAD0002,  /* не найдены функции nn::fs в модуле sdk */
            Crash_MountFailed  = 0xDEAD0003,  /* nn::fs::MountSdCard("nxrsd") вернул ошибку */
            Crash_NoReportFile = 0xDEAD0004,  /* SD есть, но файл отчёта создать не вышло */
            Crash_NoTitleId    = 0xDEAD0005,  /* svcGetInfo(ProgramId) отказал: пути мода нет */
        };

        /*
         * Намеренная авария по заданному адресу. Адрес попадает в crash report
         * Atmosphère (поле Address), по нему и опознаётся причина.
         * Обращение volatile — компилятор его не выбросит; цикл на случай, если
         * запись каким-то чудом не вызовет исключение.
         */
        void Die(u32 code) {
            volatile u8* p = reinterpret_cast<volatile u8*>(static_cast<uintptr_t>(code));
            while (true) {
                *p = 0xFF;
            }
        }

        /*
         * Строка отчёта собирается на стеке; куча не используется вовсе.
         * Буфер намеренно небольшой: LogOpen вызывается из хука, в чужих потоках
         * игры, и лишние килобайты на кадр стека там ни к чему.
         */
        constexpr size_t LineBufferSize = 0x400;

        struct Line {
            char   m_Buf[LineBufferSize];
            size_t m_Len = 0;

            void Reset() { m_Len = 0; }

            void Add(const char* s) {
                while (*s != '\0' && m_Len + 1 < LineBufferSize) {
                    m_Buf[m_Len++] = *s++;
                }
            }

            void AddHex(u64 value) {
                Add("0x");
                char tmp[16];
                int n = 0;
                do {
                    const u32 d = static_cast<u32>(value & 0xF);
                    tmp[n++] = static_cast<char>(d < 10 ? ('0' + d) : ('a' + d - 10));
                    value >>= 4;
                } while (value != 0 && n < 16);
                while (n > 0 && m_Len + 1 < LineBufferSize) {
                    m_Buf[m_Len++] = tmp[--n];
                }
            }

            void AddDec(u32 value) {
                char tmp[12];
                int n = 0;
                do {
                    tmp[n++] = static_cast<char>('0' + (value % 10));
                    value /= 10;
                } while (value != 0 && n < 12);
                while (n > 0 && m_Len + 1 < LineBufferSize) {
                    m_Buf[m_Len++] = tmp[--n];
                }
            }

            /* Перевод строки в CRLF: отчёт читают в блокноте Windows. */
            void End() {
                if (m_Len + 2 < LineBufferSize) {
                    m_Buf[m_Len++] = '\r';
                    m_Buf[m_Len++] = '\n';
                }
            }
        };

        nn::fs::FileHandle g_Handle {};
        s64                g_Offset = 0;
        std::atomic<bool>  g_Open   { false };
        std::atomic<int>   g_Count  { 0 };
        /* OpenFile игра зовёт из нескольких потоков — запись в отчёт под замком. */
        std::atomic_flag   g_Lock   {};

        /*
         * РАННИЕ СОБЫТИЯ. Хук OpenFile стоит с самого начала nnMain, а писать в
         * файл нельзя, пока не открыт затвор ФС (nxr_fsgate.hpp). Поэтому всё,
         * что случилось раньше, копится здесь и выливается в отчёт первым делом.
         * Буфер маленький: ранних обращений единицы (у Well Dweller — ни одного,
         * у SiN Reloaded — ровно одно, то самое, которым затвор и открывается).
         */
        constexpr int MaxEarly     = 24;
        constexpr int EarlyPathMax = 192;
        nxr::EarlyLog<MaxEarly, EarlyPathMax> g_Early {};

        void NoteEarly(const char* path, int mode) {
            while (g_Lock.test_and_set(std::memory_order_acquire)) {
            }
            g_Early.Add(path, mode);
            g_Lock.clear(std::memory_order_release);
        }

        void WriteRaw(const char* data, size_t len) {
            if (len == 0 || g_WriteFile == nullptr) {
                return;
            }
            if (!g_Open.load(std::memory_order_acquire)) {
                return;
            }

            while (g_Lock.test_and_set(std::memory_order_acquire)) {
                /* ждём: записей всего несколько десятков, спор короткий */
            }
            /* Перепроверяем под замком: файл мог закрыться между проверкой и захватом. */
            if (g_Open.load(std::memory_order_relaxed)) {
                const nn::fs::WriteOption opt =
                    nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush);
                if (R_SUCCEEDED(g_WriteFile(g_Handle, g_Offset, data, len, opt))) {
                    g_Offset += static_cast<s64>(len);
                }
            }
            g_Lock.clear(std::memory_order_release);
        }

        void WriteLine(Line& line) {
            line.End();
            WriteRaw(line.m_Buf, line.m_Len);
        }

        void CloseReport() {
            while (g_Lock.test_and_set(std::memory_order_acquire)) {
            }
            if (g_Open.load(std::memory_order_relaxed)) {
                g_Open.store(false, std::memory_order_relaxed);
                if (g_FlushFile != nullptr) {
                    g_FlushFile(g_Handle);
                }
                if (g_CloseFile != nullptr) {
                    g_CloseFile(g_Handle);
                }
            }
            g_Lock.clear(std::memory_order_release);
        }

        /*
         * Заводит НОВЫЙ отчёт этого запуска. Открываем на запись с Append, иначе
         * запись за конец файла нулевой длины вернёт ошибку и отчёт останется пустым.
         * Папки, номер и удаление старых (сверх 5) — nxr::logs::Prepare.
         */
        bool OpenReport() {
            if (g_CreateFile == nullptr || g_OpenFile == nullptr) {
                return false;
            }
            nxr::logs::device::NnFs fs {};
            fs.Init(&LogsResolveSymbol);
            g_Prepared = nxr::logs::Prepare(fs, nxr::logs::DiagTemplate, g_TitleId,
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
            /* BOM: чтобы блокнот показал русский текст, а не «кракозябры». */
            WriteRaw("\xEF\xBB\xBF", 3);
            return true;
        }

        /*
         * Список модулей берём СВОИМ обходом (nxr::ForEachModule), а не через
         * exl::util::GetModuleInfo: сканер exlaunch пропускает nnSdk (у него больше
         * трёх областей памяти из-за relro), а при неожиданной раскладке ещё и
         * роняет процесс через R_ABORT_UNLESS. К тому же в процесс игры может быть
         * внедрён saltysd_core (SaltyNX), то есть модулей заведомо больше, чем ждёт
         * шаблон. Свой обход просто перечисляет всё, что видит.
         */
        void ListModules(Line& line) {
            u32 n = 0;
            nxr::ForEachModule(g_MemQuery, [&](u64 base, const nxr::Dyn* dyn) {
                ++n;
                line.Reset();
                line.Add("модуль #");
                line.AddDec(n);
                line.Add(" база ");
                line.AddHex(base);
                line.Add("  .dynamic ");
                line.AddHex(reinterpret_cast<u64>(dyn));
                line.Add("  nnMain ");
                line.AddHex(nxr::FindSymbolIn(g_MemQuery, base, dyn, SymNnMain));
                WriteLine(line);
                return false;  /* обойти все */
            });
            line.Reset();
            line.Add("всего модулей найдено: ");
            line.AddDec(n);
            WriteLine(line);
        }

        void LineSym(const char* title, uintptr_t addr) {
            Line line;
            line.Add(title);
            if (addr == 0) {
                line.Add("НЕ НАЙДЕН (0)");
            } else {
                line.AddHex(addr);
            }
            WriteLine(line);
        }

        void LineResult(const char* title, Result result) {
            Line line;
            line.Add(title);
            if (R_SUCCEEDED(result)) {
                line.Add("успех");
            } else {
                line.Add("ошибка ");
                line.AddHex(result);
            }
            WriteLine(line);
        }

        /* Одна строка за раз, буфер переиспользуется: экономим стек. */
        void WriteText(Line& line, const char* a, const char* b = nullptr) {
            line.Reset();
            line.Add(a);
            if (b != nullptr) {
                line.Add(b);
            }
            WriteLine(line);
        }

        /* Пишется один раз, из Run (то есть из OpenFsGate при открытии затвора ФС). */
        /*
         * Строка времени. false — часов ещё нет (игра не запустила nn::time).
         * late — дописывается позже шапки (из LogOpen).
         */
        bool WriteTimeLine(bool late) {
            Line line;
            line.Add(late ? "время (дописано позже, часы консоли): "
                          : "время (часы консоли): ");
            nxr::logs::Out o(line.m_Buf + line.m_Len, LineBufferSize - line.m_Len);
            if (!g_Clock.Format(o)) {
                return false;
            }
            line.m_Len += o.m_Len;
            WriteLine(line);
            return true;
        }

        void Begin(uintptr_t nnMainAddr, uintptr_t openFile, uintptr_t mountSd, Result mountResult) {
            Line line;

            /* ПЕРВАЯ СТРОКА — привязка к игре: TitleID, BuildID main, номер. */
            line.Reset();
            line.m_Len = nxr::logs::FormatHead(line.m_Buf, LineBufferSize - 2, g_Head,
                                               nxr::logs::KindPrefix(nxr::logs::DiagTemplate),
                                               g_Prepared.number);
            WriteLine(line);
            WriteText(line, "nxredirect — диагностический отчёт (сборка с -DNXR_DIAG)");
            WriteText(line, "файл: ", ReportPath);
            line.Reset();
            line.Add("отчётов diag этой игры было: ");
            line.AddDec(static_cast<u32>(g_Prepared.found));
            line.Add(", удалено старых: ");
            line.AddDec(static_cast<u32>(g_Prepared.deleted));
            line.Add(g_Prepared.listed ? " (хранятся последние 5)"
                                       : " (папку прочитать не удалось — номер 1, перезапись)");
            WriteLine(line);
            if (!WriteTimeLine(false)) {
                WriteText(line, "время: nn::time игрой ещё не запущен — строка времени "
                                "будет дописана ниже, когда игра его запустит");
                g_TimePending.store(true, std::memory_order_release);
            }
            line.Reset();
            line.Add("с включения консоли (счётчик тиков ядра): ");
            {
                nxr::logs::Out o(line.m_Buf + line.m_Len, LineBufferSize - line.m_Len);
                nxr::logs::AddUptime(o, nxr::logs::device::Ticks());
                line.m_Len += o.m_Len;
            }
            WriteLine(line);

            /*
             * ЧЕСТНАЯ СТРОКА О ПОРЯДКЕ. Отчёт начинается ПОЗЖЕ старта модуля —
             * это не сбой, а исправление: раньше мы монтировали SD до того, как
             * игра задаст свой распределитель nn::fs, и игра падала
             * (Result 0x327602). Подробности — src/nxr_fsgate.hpp.
             */
            WriteText(line, "--- порядок работы с файловой системой ---");
            WriteText(line, "запись отчёта начата ПОЗЖЕ старта модуля: ждали разрешающего "
                            "момента, потому что обращение к nn::fs раньше игры ломало её "
                            "(nn::fs::SetAllocator -> Result 0x327602).");
            WriteText(line, "затвор ФС открыт: ", nxr::GateReasonText(g_FsGate.Reason()));
            WriteText(line, "символ nn::fs::SetAllocator: ",
                      g_HaveSetAllocator ? "найден и перехвачен"
                                         : "НЕ найден — работал запасной путь");
            /* NXR_MODULE_SLOT задаётся сборкой (-D из Makefile) = MODULE_NAME. */
#ifdef NXR_MODULE_SLOT
            WriteText(line, "слот модуля: ", NXR_MODULE_SLOT);
#endif

            /* TitleID и собранный путь мода — главное, что проверяет эта сборка. */
            line.Reset();
            line.Add("TitleID (svcGetInfo ProgramId) = ");
            if (g_TitleId == 0) {
                line.Add("НЕ ПОЛУЧЕН");
            } else {
                line.AddHex(g_TitleId);
            }
            WriteLine(line);

            WriteText(line, "путь мода на SD: ",
                      g_SdPrefixLen != 0 ? g_SdPrefix : "(не собран)");

            /*
             * Точки romfs игры, узнанные через nn::fs::MountRom. По ним
             * подменяются двусмысленные имена ("content"). Пусто — MountRom к этому
             * моменту не звали (или он прошёл мимо перехватчика).
             */
            WriteText(line, "перехват MountRom/Unmount: ",
                      g_MountHooksInstalled ? "стоит" : "НЕ стоит (символы не найдены)");
            line.Reset();
            line.Add("точки romfs игры, узнанные через MountRom: ");
            line.AddDec(static_cast<u32>(g_RomMounts.Count()));
            for (size_t i = 0; i < nxr::RomMounts::Capacity; ++i) {
                char name[nxr::MaxMountNameLen + 1] = {};
                if (!g_RomMounts.CopyAt(i, name, sizeof(name))) {
                    break;
                }
                line.Add(i == 0 ? " — " : ", ");
                line.Add(name);
            }
            WriteLine(line);

            WriteText(line, "--- модули, найденные своим обходом памяти ---");

            ListModules(line);

            WriteText(line, "--- адреса символов (0 = не найден) ---");

            LineSym("nnMain              = ", nnMainAddr);
            LineSym("nn::fs::OpenFile    = ", openFile);
            LineSym("nn::fs::MountSdCard = ", mountSd);
            LineSym("nn::fs::CreateFile  = ", reinterpret_cast<uintptr_t>(g_CreateFile));
            LineSym("nn::fs::DeleteFile  = ", reinterpret_cast<uintptr_t>(g_DeleteFile));
            LineSym("nn::fs::WriteFile   = ", reinterpret_cast<uintptr_t>(g_WriteFile));
            LineSym("nn::fs::FlushFile   = ", reinterpret_cast<uintptr_t>(g_FlushFile));
            LineSym("nn::fs::CloseFile   = ", reinterpret_cast<uintptr_t>(g_CloseFile));

            WriteText(line, "--- SD-карта ---");

            LineResult("MountSdCard(\"" NXR_SD_MOUNT "\") = ", mountResult);

            /*
             * Проба: существует ли САМА ПАПКА мода. Конкретного файла мы не знаем
             * (мод универсальный), поэтому проверяем путь мода как файл: ошибка
             * «путь не найден» и «это каталог» различаются кодом, и по нему видно,
             * лежит ли папка на карте вообще.
             */
            if (g_SdPrefixLen != 0 && g_OpenFile != nullptr) {
                nn::fs::FileHandle probe {};
                const Result probeResult = g_OpenFile(&probe, g_SdPrefix, nn::fs::OpenMode_Read);
                line.Reset();
                line.Add("проба открытия папки мода ");
                line.Add(g_SdPrefix);
                line.Add(" = ");
                if (R_SUCCEEDED(probeResult)) {
                    line.Add("успех (неожиданно: это должен быть каталог)");
                    if (g_CloseFile != nullptr) {
                        g_CloseFile(probe);
                    }
                } else {
                    line.Add("код ");
                    line.AddHex(probeResult);
                    line.Add(" (0x7d402 = путь не найден: папки мода на карте НЕТ)");
                }
                WriteLine(line);
            }

            /*
             * Слив ранних событий: то, что игра успела открыть, пока затвор был
             * закрыт. Эти вызовы прошли в оригинал БЕЗ подмены — так и написано.
             */
            line.Reset();
            line.Add("--- ранние обращения к OpenFile (до открытия затвора, подмены не было): ");
            line.AddDec(static_cast<u32>(g_Early.Count()));
            if (g_Early.Dropped() != 0) {
                line.Add(", не поместилось в буфер ");
                line.AddDec(static_cast<u32>(g_Early.Dropped()));
            }
            line.Add(" ---");
            WriteLine(line);
            for (int i = 0; i < g_Early.Count(); ++i) {
                line.Reset();
                line.Add("ранний #");
                line.AddDec(static_cast<u32>(i) + 1);
                line.Add("  mode=");
                line.AddHex(static_cast<u32>(g_Early.At(i).m_Mode));
                line.Add(" ");
                line.Add(g_Early.At(i).m_Path);
                WriteLine(line);
            }
            g_Early.Clear();

            line.Reset();
            line.Add("--- пути, которые игра открывает через nn::fs::OpenFile (первые ");
            line.AddDec(MaxLoggedPaths);
            line.Add(") ---");
            WriteLine(line);
        }

        /*
         * Запись одного обращения к OpenFile.
         * sdPath != nullptr означает, что путь имел префикс "имя:/" и мы пробовали
         * открыть вариант с SD; result — что вернула эта попытка.
         */
        void LogOpen(const char* path, const char* sdPath, Result result,
                     int mode, nxr::SkipReason reason) {
            if (!g_Open.load(std::memory_order_acquire)) {
                return;
            }

            /* Время не записано в шапке — пробуем сейчас: игра могла уже
               запустить nn::time. Пишет ровно один поток (exchange). */
            if (g_TimePending.load(std::memory_order_acquire) &&
                g_TimePending.exchange(false, std::memory_order_acq_rel)) {
                if (!WriteTimeLine(true)) {
                    g_TimePending.store(true, std::memory_order_release);
                }
            }

            const int n = g_Count.fetch_add(1, std::memory_order_relaxed);
            if (n >= MaxLoggedPaths) {
                if (n == MaxLoggedPaths) {
                    Line tail;
                    tail.Add("--- собрано ");
                    tail.AddDec(MaxLoggedPaths);
                    tail.Add(" путей, дальше не пишем; отчёт закрыт ---");
                    WriteLine(tail);
                    CloseReport();
                }
                return;
            }

            Line line;
            line.Add("#");
            line.AddDec(static_cast<u32>(n) + 1);
            line.Add(sdPath != nullptr ? "  подменено ДА  " : "  подменено НЕТ ");
            /* Режим открытия: по нему сразу видно, чтение это или запись. */
            line.Add("mode=");
            line.AddHex(static_cast<u32>(mode));
            line.Add(nxr::IsRedirectableMode(mode) ? "(Read) " : "(НЕ Read) ");
            line.Add(path != nullptr ? path : "(nullptr)");
            if (sdPath != nullptr) {
                line.Add("  ->  ");
                line.Add(sdPath);
                if (R_SUCCEEDED(result)) {
                    line.Add("  = успех, файл взят с SD");
                } else {
                    line.Add("  = ошибка ");
                    line.AddHex(result);
                    line.Add(", отдан оригинал из romfs; причина: ");
                    line.Add(nxr::SkipReasonText(nxr::SkipReason::NotOnSd));
                }
            } else {
                /* Почему подмены не было: блэклист, режим, отсутствие префикса… */
                line.Add("  = причина: ");
                line.Add(nxr::SkipReasonText(reason));
            }
            WriteLine(line);
        }

        /*
         * Событие монтирования romfs: MountRom/Unmount и что стало с
         * реестром. Пишется, только если отчёт уже открыт; всё, что было раньше,
         * видно в начале отчёта списком узнанных точек.
         */
        void LogMount(const char* what, const char* name, Result result, bool remembered) {
            if (!g_Open.load(std::memory_order_acquire)) {
                return;
            }
            Line line;
            line.Add(what);
            line.Add("(\"");
            line.Add(name != nullptr ? name : "(nullptr)");
            line.Add("\") = ");
            if (R_SUCCEEDED(result)) {
                line.Add("успех");
            } else {
                line.Add("ошибка ");
                line.AddHex(result);
            }
            line.Add(remembered ? "; имя в реестре romfs игры" : "; имя НЕ в реестре");
            WriteLine(line);
        }

        /*
         * Вся диагностическая часть. Вызывается не из хука nnMain, а
         * один раз из OpenFsGate — когда затвор ФС открыт (SetAllocator игры или её
         * первый перехваченный OpenFile) и SD уже смонтирована вызывающим. Каждая
         * невозможность продолжить — авария со своим кодом.
         *
         * Поэтому «нет ни отчёта, ни аварии» значит: затвор не открылся. Хук nnMain
         * при этом мог отработать — например, игра не позвала ни SetAllocator, ни
         * перехваченный OpenFile (в том числе когда OpenFile не найден и хук на нём
         * не встал). Аварии из Run (0xD1…, 0xDEAD0002…0xDEAD0004) возможны только
         * после открытия затвора.
         */
        void Run(uintptr_t nnMainAddr, uintptr_t openFile, uintptr_t mountSd,
                 Result mountResult) {
            if (openFile == 0 || mountSd == 0) {
                /*
                 * Отчёт писать нечем (нет даже OpenFile), поэтому положение дел
                 * кодируем в адрес аварии — он виден в crash report Atmosphère:
                 *   0xD1 MM PP SS
                 *   MM — сколько модулей нашёл наш обход памяти (svcQueryMemory),
                 *   PP — у скольких из них удалось разобрать .dynsym/.dynstr,
                 *   SS — сколько символов из двух нашлось (OpenFile, MountSdCard).
                 * Так за один запуск видно, где обрывается цепочка.
                 */
                u32 modules = 0;
                u32 parsed  = 0;
                nxr::ForEachModule(g_MemQuery, [&](u64 base, const nxr::Dyn* dyn) {
                    ++modules;
                    nxr::Tables tables {};
                    if (nxr::ResolveTables(g_MemQuery, base, dyn, &tables)) {
                        ++parsed;
                    }
                    return false;  /* обойти все */
                });
                const u32 syms = (openFile != 0 ? 1u : 0u) + (mountSd != 0 ? 1u : 0u);
                Die(0xD1000000u | ((modules & 0xFF) << 16) | ((parsed & 0xFF) << 8) | (syms & 0xFF));
            }

            /* Ищем во всех модулях: индекс Sdk врёт, наш модуль сдвигает нумерацию. */
            g_OpenFile   = reinterpret_cast<OpenFileFn>(openFile);
            g_CreateFile = reinterpret_cast<CreateFileFn>(FindSymbolAnywhere(SymCreateFile));
            g_DeleteFile = reinterpret_cast<DeleteFileFn>(FindSymbolAnywhere(SymDeleteFile));
            g_WriteFile  = reinterpret_cast<WriteFileFn> (FindSymbolAnywhere(SymWriteFile));
            g_FlushFile  = reinterpret_cast<FlushFileFn> (FindSymbolAnywhere(SymFlushFile));
            g_CloseFile  = reinterpret_cast<CloseFileFn> (FindSymbolAnywhere(SymCloseFile));

            /* Без этих трёх отчёт писать нечем. */
            if (g_CreateFile == nullptr || g_WriteFile == nullptr || g_CloseFile == nullptr) {
                Die(Crash_NoFsSymbols);
            }

            /*
             * SD К ЭТОМУ МОМЕНТУ УЖЕ СМОНТИРОВАНА вызывающим (OpenFsGate), и
             * только после открытия затвора ФС. Раньше монтирование стояло прямо
             * здесь — из-за этого игра и падала на своём nn::fs::SetAllocator
             * (проба на консоли, Result 0x327602). Сюда результат приходит
             * параметром, чтобы монтирование осталось в одном месте.
             */
            if (!R_SUCCEEDED(mountResult)) {
                Die(Crash_MountFailed);
            }

            /* Первая строка и часы: только чтение памяти и поиск символов. */
            g_Head = MakeLogHead();
            g_Clock.Init(&LogsResolveSymbol);

            if (!OpenReport()) {
                Die(Crash_NoReportFile);
            }

            Begin(nnMainAddr, openFile, mountSd, mountResult);

            /*
             * Путь мода не собрался — отчёт об этом уже записан, поэтому не падаем
             * сразу, а даём отчёту закрыться на следующих OpenFile. Но сам факт
             * отмечаем в отчёте выше, а код аварии остаётся на случай, когда отчёта
             * нет вовсе (см. exl_main).
             */
        }

    }
#endif

#ifdef NXR_AUTOFIX
    /*
     * ПАСПОРТ КОНСОЛИ. Короткий файл на карте рядом с модом:
     *   sd:/atmosphere/contents/<TID>/nxredirect-passport.txt
     * что за sdk реально загружена (версия, BuildId), подменена ли она, есть
     * ли Vulkan, и по каждому исправлению — СВОЯ или ПРОКСИ и почему. Пишется
     * ОДИН раз за запуск, ТОЛЬКО после открытия затвора ФС (из OpenFsGate) и в
     * обычной сборке тоже. Ни одной новой привилегии: svcGetInfo (0x29) и
     * запись на SD (бит SdCard в main.npdm уже есть). Отказ любой ступени —
     * молча без паспорта, игра не падает. Выключается -DNXR_NO_PASSPORT.
     *
     * Версия HOS сюда НЕ пишется: из процесса игры её без новых прав не
     * достать. svcGetSystemInfo (0x6F) вне разрешённых игре групп 0..3, а
     * nn::settings::system::GetFirmwareVersion и nn::oe::GetFirmwareVersionForDebug
     * — IPC к службам настроек, при отказе sdk уходит в Abort (дизассемблер
     * 19.3.13 @0x3f6940: bl на обработчик ошибки). Вместо неё — ответ ядра
     * Atmosphère: svcGetInfo(InfoType_MesosphereMeta = 65000, 0, 0).
     */
#  ifndef NXR_NO_PASSPORT
    namespace passport {
        constexpr char SymCreateFile[] = "_ZN2nn2fs10CreateFileEPKcl";
        constexpr char SymDeleteFile[] = "_ZN2nn2fs10DeleteFileEPKc";
        constexpr char SymWriteFile[]  = "_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE";
        constexpr char SymCloseFile[]  = "_ZN2nn2fs9CloseFileENS0_10FileHandleE";

        using CreateFileFn = Result (*)(char const*, s64);
        using DeleteFileFn = Result (*)(char const*);
        using WriteFileFn  = Result (*)(nn::fs::FileHandle, s64, void const*, u64,
                                        nn::fs::WriteOption const&);
        using CloseFileFn  = void   (*)(nn::fs::FileHandle);

        /* Запись файла целиком через nn::fs игры. OpenFile — по адресу
           оригинала: с хуком это наш перехватчик, режим записи он не трогает. */
        struct GameFs {
            bool Write(const char* path, const char* data, std::size_t len) const {
                const auto create = reinterpret_cast<CreateFileFn>(FindSymbolAnywhere(SymCreateFile));
                const auto remove = reinterpret_cast<DeleteFileFn>(FindSymbolAnywhere(SymDeleteFile));
                const auto write  = reinterpret_cast<WriteFileFn>(FindSymbolAnywhere(SymWriteFile));
                const auto close  = reinterpret_cast<CloseFileFn>(FindSymbolAnywhere(SymCloseFile));
                const auto open   = reinterpret_cast<OpenFileFn>(g_AddrOpenFile);
                if (create == nullptr || write == nullptr || close == nullptr || open == nullptr) {
                    return false;
                }
                if (remove != nullptr) {
                    remove(path);   /* прошлый паспорт: отказ «нет файла» — норма */
                }
                if (R_FAILED(create(path, static_cast<s64>(len)))) {
                    return false;
                }
                nn::fs::FileHandle h {};
                if (R_FAILED(open(&h, path, nn::fs::OpenMode_Write))) {
                    return false;
                }
                const nn::fs::WriteOption opt =
                    nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush);
                const Result rc = write(h, 0, data, static_cast<u64>(len), opt);
                close(h);
                return R_SUCCEEDED(rc);
            }
        };

        /* Буфер в .bss, не на стеке: пишет ровно один поток — открывший затвор. */
        nxr::autofix::TextBuf<0x1000> g_Text;
        char g_Path[PathBufferSize];

        void Write() {
            using namespace nxr::autofix;
            PassportFacts f {};
            f.titleId = g_TitleId;
#    ifdef NXR_MODULE_SLOT
            f.slot = NXR_MODULE_SLOT;
#    else
            f.slot = "subsdk?";
#    endif
            u64 meta = 0;
            const Result metaRc = svcGetInfo(&meta, 65000 /* InfoType_MesosphereMeta */, 0,
                                             0 /* MesosphereMetaInfo_KernelVersion */);
            f.kernelMetaOk = R_SUCCEEDED(metaRc);
            f.kernelMeta   = meta;
            f.kernelMetaRc = static_cast<std::uint32_t>(metaRc);
            SvcQuery q;
            f.view = ComputeView(q, SelfAddr());
            for (int i = 0; i < FixCount; ++i) {
                f.res[i] = Resolve(i);
                f.calls[i] = __atomic_load_n(&g_Calls[i], __ATOMIC_RELAXED);
            }
            f.nvrmPath = g_Nvrm.path;
            f.nvrmPathText = g_Nvrm.path != 0
                ? nxr::compat::NvrmPathText(static_cast<nxr::compat::NvrmPath>(g_Nvrm.path))
                : nullptr;
            f.roFixText = RoFixText();
            g_Text.m_Len = 0;
            FormatPassport(g_Text, f);
            if (PassportPath(g_Path, sizeof(g_Path), g_SdPrefix, g_SdPrefixLen) == 0) {
                return;
            }
            GameFs fs;
            WritePassport(g_FsGate, fs, g_Path, g_Text.Data(), g_Text.Size());
        }
    }
#  endif
#endif

    /*
     * ОТКРЫТИЕ ЗАТВОРА ФС — и вся отложенная работа модуля.
     *
     * До этого момента модуль не сделал с nn::fs ни одного вызова. Здесь их уже
     * можно делать: игра либо задала свой распределитель (вернулась из
     * nn::fs::SetAllocator), либо обратилась к nn::fs сама.
     *
     * Работу выполняет ровно тот вызов, который открыл затвор первым;
     * остальные (другие потоки игры) просто уходят.
     */
    void OpenFsGate(nxr::GateReason reason) {
        if (!g_FsGate.Open(reason)) {
            return;
        }

#if defined(NXR_DIAG) || defined(NXR_DIAG_GFX) ||     (defined(NXR_AUTOFIX) && !defined(NXR_NO_PASSPORT))
        /*
         * Диагностика и паспорт консоли пишут на SD, значит карта нужна прямо
         * сейчас. Без них (сборка -DNXR_NO_AUTOFIX или -DNXR_NO_PASSPORT)
         * монтирование остаётся ЛЕНИВЫМ (EnsureSdMounted при первом
         * подменяемом пути), как было и раньше.
         */
        const Result mountResult = MountSdOnce();
        /* В сборке только с -DNXR_DIAG_GFX результат никому не нужен: отчёт о
           графике заводится своим кодом, а сама SD уже смонтирована. */
        EXL_UNUSED(mountResult);
#endif

#ifdef NXR_DIAG
        /* Диагностика: заводит отчёт и пишет TitleID, путь мода и адреса.
           Если что-то не вышло — падает с узнаваемым адресом (enum Crash). */
        diag::Run(g_AddrNnMain, g_AddrOpenFile, g_AddrMountSdCard, mountResult);
#endif

#ifdef NXR_DIAG_GFX
        /*
         * Диагностика графики: ОТЧЁТ на карту. Перехватчики уже стоят с хука
         * nnMain (nxr::gfx::Install) и до этого момента копили строки в памяти;
         * здесь заводится файл и в него первыми выливаются ранние события.
         * История: раньше диагностика стартовала в хуке nnMain и сразу
         * монтировала SD — это ломало Well Dweller (0x327602); тогда весь
         * старт перенесли сюда, к затвору, — и перехватчики вставали ПОЗЖЕ, чем
         * Well Dweller начинал рисовать (nxredirect-gfx (4).txt: ни nn::vi, ни
         * eglCreateWindowSurface). Теперь разделено: перехват — рано, файл —
         * после затвора. Отказ НЕ роняет игру.
         */
        nxr::gfx::StartReport(&GfxResolveSymbol, MakeLogHead());
#endif

#if defined(NXR_AUTOFIX) && !defined(NXR_NO_PASSPORT)
        /* Паспорт консоли — после затвора и монтирования; отказ не роняет игру. */
        if (R_SUCCEEDED(mountResult)) {
            passport::Write();
        }
#endif
    }

}

/* Запись одного обращения к OpenFile — только в диагностической сборке. */
#ifdef NXR_DIAG
#  define NXR_DIAG_LOG_OPEN(path, sdPath, result, mode, reason)        diag::LogOpen((path), (sdPath), (result), (mode), (reason))
/* Раннее событие (затвор ФС ещё закрыт, писать в файл нельзя) — в память. */
#  define NXR_DIAG_NOTE_EARLY(path, mode)                              diag::NoteEarly((path), (mode))
/* Монтирование romfs (MountRom/Unmount) — строка в отчёт, если он уже открыт. */
#  define NXR_DIAG_LOG_MOUNT(what, name, result, remembered)           diag::LogMount((what), (name), (result), (remembered))
#else
#  define NXR_DIAG_LOG_OPEN(path, sdPath, result, mode, reason) ((void)0)
#  define NXR_DIAG_NOTE_EARLY(path, mode) ((void)0)
#  define NXR_DIAG_LOG_MOUNT(what, name, result, remembered) ((void)0)
#endif

/*
 * Хук nn::fs::OpenFile. Trampoline — оригинал нужен, мы его вызываем сами.
 * Сигнатура обязана совпадать с Result nn::fs::OpenFile(FileHandle*, char const*, int).
 */
HOOK_DEFINE_TRAMPOLINE(OpenFileHook) {

    static Result Callback(nn::fs::FileHandle* outHandle, char const* path, int mode) {

        /*
         * ЗАТВОР ФАЙЛОВОЙ СИСТЕМЫ (nxr_fsgate.hpp). Пока он закрыт, нам нельзя
         * ни монтировать SD, ни открывать свои файлы: раннее обращение к nn::fs
         * взводит флаг дефолтного распределителя, и игра падает на своём
         * nn::fs::SetAllocator (Result 0x327602, проба на консоли).
         * Поэтому вызов прозрачно уходит в оригинал, а событие запоминается в
         * памяти и попадёт в отчёт позже.
         */
        const nxr::OpenFileAction action =
            nxr::DecideOpenFile(g_FsGate, g_GameCallsSetAllocator);
        if (action != nxr::OpenFileAction::Normal) {
            NXR_DIAG_NOTE_EARLY(path, mode);
            const Result early = Orig(outHandle, path, mode);
            if (action == nxr::OpenFileAction::PassThroughThenOpenGate) {
                /* Игра SetAllocator не зовёт — ждать больше нечего. Открываем
                   затвор ПОСЛЕ возврата: пусть флаг взведёт сама игра. */
                OpenFsGate(nxr::GateReason::GameFsUse);
            }
            /*
             * Вызов, открывший затвор, прошёл БЕЗ подмены. Если оригинал файл НЕ
             * нашёл, повторяем его с SD — затвор уже открыт (nxr::RetryAfterGateOpen,
             * исправлено: так терялся первый файл чужого мода —
             * "content:/UltraCam/maxlastbreath.ini" у TotK). Нашёл — отдаём
             * найденное, как раньше.
             */
            if (nxr::RetryAfterGateOpen(action, R_FAILED(early)) &&
                path != nullptr && nxr::IsRedirectableMode(mode) && EnsureSdMounted()) {
                char sdPath[PathBufferSize];
                nxr::SkipReason reason = nxr::SkipReason::None;
                if (BuildSdPathWhy(sdPath, sizeof(sdPath), path, &reason)) {
                    const Result retry = Orig(outHandle, sdPath, mode);
                    NXR_DIAG_LOG_OPEN(path, sdPath, retry, mode, reason);
                    if (R_SUCCEEDED(retry)) {
                        return retry;
                    }
                }
            }
            return early;
        }

        /*
         * ПОДМЕНЯЕМ ТОЛЬКО ЧТЕНИЕ. mode с битами записи/дозаписи (и вообще любой
         * mode, кроме ровно nn::fs::OpenMode_Read) уходит в оригинал: правило и
         * причины — в nxr_path.hpp, IsRedirectableMode. В диагностике такой путь
         * всё равно виден в отчёте как «подменено НЕТ».
         */
        if (path != nullptr && !nxr::IsRedirectableMode(mode)) {
            NXR_DIAG_LOG_OPEN(path, nullptr, 0, mode, nxr::SkipReason::WriteMode);
            return Orig(outHandle, path, mode);
        }

        if (path != nullptr && EnsureSdMounted()) {
            char sdPath[PathBufferSize];
            nxr::SkipReason reason = nxr::SkipReason::None;

            if (BuildSdPathWhy(sdPath, sizeof(sdPath), path, &reason)) {
                const Result result = Orig(outHandle, sdPath, mode);
                NXR_DIAG_LOG_OPEN(path, sdPath, result, mode, reason);

                if (R_SUCCEEDED(result)) {
                    /* Лог только в отладочной сборке: Logging здесь — SvcLogger,
                       то есть настоящий системный вызов на каждый файл. */
#if defined(EXL_DEBUG)
                    Logging.Log("[nxredirect] %s -> %s\n", path, sdPath);
#endif
                    return result;
                }
                /* Файла на SD нет (или SD недоступна) — молча падаем на оригинал.
                   Результат неудачной попытки наружу не отдаём. */
            } else {
                /* Путь не из данных игры (блэклист, нет префикса, не влез в буфер) —
                   в диагностике он всё равно интересен вместе с причиной отказа. */
                NXR_DIAG_LOG_OPEN(path, nullptr, 0, mode, reason);
            }
        }

        return Orig(outHandle, path, mode);
    }

};

/*
 * Хук nn::fs::SetAllocator — РАЗРЕШАЮЩИЙ МОМЕНТ.
 *
 * Игра этим вызовом отдаёт nn::fs свой распределитель памяти. nnSdk принимает
 * его только до первого обращения к nn::fs: если дефолтным распределителем уже
 * пользовались, SetAllocator возвращает 0x327602 (2002-6459) и уходит в Abort.
 * Поэтому мы сначала пропускаем вызов в оригинал и лишь ПОТОМ, на возврате,
 * открываем затвор и начинаем свою работу с файлами.
 */
HOOK_DEFINE_TRAMPOLINE(SetAllocatorHook) {

    static void Callback(FsAllocFn alloc, FsDeallocFn dealloc) {
        Orig(alloc, dealloc);
        OpenFsGate(nxr::GateReason::SetAllocator);
    }

};

/*
 * Перехватчики монтирования romfs. Задача одна — запомнить ИМЯ,
 * под которым смонтирована romfs игры (nxr::RomMounts, nxr_path.hpp).
 *
 * ЗАТВОР ФС НЕ НАРУШАЕТСЯ: сами мы к nn::fs здесь не обращаемся. Вызов
 * оригинала — это вызов ИГРЫ (или чужого мода), он случился бы и без нас;
 * реестр — память модуля, без выделений. Поэтому перехватчики можно ставить
 * рано (ключ -DNXR_EARLY_FS_HOOK), до того, как игра задала распределитель.
 *
 * Имя запоминается ПОСЛЕ успешного оригинала: неудачный MountRom (например,
 * 0x7802 «имя уже занято» — под ним смонтировано что-то ДРУГОЕ) ничего не
 * разрешает.
 */
HOOK_DEFINE_TRAMPOLINE(MountRomHook) {

    static Result Callback(char const* name) {
        const Result result = Orig(name);
        const bool remembered = R_SUCCEEDED(result) && g_RomMounts.Remember(name);
        NXR_DIAG_LOG_MOUNT("MountRom", name, result, remembered);
        static_cast<void>(remembered);
        return result;
    }

};

HOOK_DEFINE_TRAMPOLINE(MountRomCacheHook) {

    static Result Callback(char const* name, void* cache, u64 cacheSize) {
        const Result result = Orig(name, cache, cacheSize);
        const bool remembered = R_SUCCEEDED(result) && g_RomMounts.Remember(name);
        NXR_DIAG_LOG_MOUNT("MountRom(кеш)", name, result, remembered);
        static_cast<void>(remembered);
        return result;
    }

};

/* void nn::fs::Unmount(const char*): имя вычёркивается ДО оригинала — после
   него под этим именем может появиться уже не romfs. */
HOOK_DEFINE_TRAMPOLINE(UnmountHook) {

    static void Callback(char const* name) {
        g_RomMounts.Forget(name);
        NXR_DIAG_LOG_MOUNT("Unmount", name, 0, false);
        Orig(name);
    }

};

/*
 * Ставит три перехватчика монтирования. Только все вместе: без Unmount имя,
 * однажды запомненное, нельзя было бы вычеркнуть, а подменять то, что потом
 * смонтировали под ним (MountContent), нельзя. Не нашлись символы — реестр
 * остаётся пустым, и двусмысленные имена не подменяются (как раньше).
 * Трамплинов — три (бюджет exlaunch 40: nxr_gfx.hpp, HooksOutsideGfx).
 */
void InstallMountHooks() {
    if (g_MountHooksInstalled) {
        return;
    }
    const uintptr_t mountRom      = FindSymbolAnywhere(SymMountRom);
    const uintptr_t mountRomCache = FindSymbolAnywhere(SymMountRomCache);
    const uintptr_t unmount       = FindSymbolAnywhere(SymUnmount);
    if (unmount == 0 || (mountRom == 0 && mountRomCache == 0)) {
        return;
    }
    if (mountRom != 0) {
        MountRomHook::InstallAtPtr(mountRom);
    }
    if (mountRomCache != 0) {
        MountRomCacheHook::InstallAtPtr(mountRomCache);
    }
    UnmountHook::InstallAtPtr(unmount);
    g_MountHooksInstalled = true;
}

#ifndef NXR_NO_AUTOFIX
/*
 * ПОИСК СИМВОЛОВ nn::ro В СТАРОЙ sdk (nxr_rofix.hpp). sdk 19.3.13 ищет символ в
 * модуле только по DT_HASH; модули игры на SDK 20+ бывают только с DT_GNU_HASH —
 * nn::ro::LookupSymbol падал (Biped 2). Подменяем внутреннюю функцию поиска
 * одного модуля: модуль с DT_HASH — оригинал, без — свой перебор .dynsym.
 * Ставится, только если функция старая И есть модуль без DT_HASH. Трамплин — один
 * (бюджет: nxr_gfx.hpp, HooksOutsideGfx).
 */
namespace {
    /* Снимок модулей для перехвата: внутри него ядро звать нельзя (nxr_rofix.hpp). */
    nxr::rofix::Snapshot g_RoSnapshot {};
}

HOOK_DEFINE_TRAMPOLINE(RoLookupHook) {

    static const nxr::Sym* Callback(const void* obj, const char* name) {
        const std::uint64_t base = obj != nullptr
            ? *reinterpret_cast<const std::uint64_t*>(
                  static_cast<const std::uint8_t*>(obj) + nxr::rofix::ModuleObjectBase)
            : 0;
        const nxr::rofix::ModuleTables* m = g_RoSnapshot.Find(base);
        if (m == nullptr || m->hasHash) {
            return Orig(obj, name);
        }
        return nxr::rofix::FindDefinedSym(*m, name);
    }

};

void ApplyRoFix() {
    const uintptr_t lms = FindSymbolAnywhere(nxr::rofix::SymLookupModuleSymbol);
    if (lms == 0) {
        g_RoFix = 1;   /* nn::ro нет — чинить нечего */
        return;
    }
    const std::uint64_t f = nxr::rofix::FindInternalLookup(
        reinterpret_cast<const std::uint32_t*>(lms), lms);
    if (f == 0) {
        g_RoFix = 3;
        return;
    }
    if (!nxr::rofix::IsSysvOnly(reinterpret_cast<const std::uint32_t*>(static_cast<uintptr_t>(f)))) {
        g_RoFix = 1;
        return;
    }
    nxr::rofix::TakeSnapshot(g_MemQuery, &g_RoSnapshot);
    if (!g_RoSnapshot.anyWithoutHash) {
        g_RoFix = 1;
        return;
    }
    RoLookupHook::InstallAtPtr(static_cast<uintptr_t>(f));
    g_RoFix = 2;
}

#endif

/*
 * Хук точки входа игры. К этому моменту загружены и проинициализированы все модули,
 * включая `sdk`, поэтому адреса функций nnSdk уже можно найти по символу.
 */
HOOK_DEFINE_TRAMPOLINE(NnMainHook) {

    static void Callback() {

        /*
         * Пересканировать модули не нужно и ОПАСНО: наш обход (nxr::ForEachModule)
         * каждый раз спрашивает память у ядра заново и видит уже загруженный `sdk`.
         * А exl::util::impl::InitMemLayout() — сканер шаблона: он ждёт у модуля ровно
         * три области подряд, пропускает nnSdk с relro и роняет процесс через
         * R_ABORT_UNLESS, если модулей окажется больше тринадцати (а в процесс может
         * быть внедрён ещё и saltysd_core от SaltyNX). Поэтому его здесь нет.
         */
        const uintptr_t openFile = FindSymbolAnywhere(SymOpenFile);
        const uintptr_t mountSd  = FindSymbolAnywhere(SymMountSdCard);

        /*
         * ЗДЕСЬ МЫ НЕ ТРОГАЕМ nn::fs. Поиск символов идёт через svcQueryMemory и
         * чтение таблиц .dynsym — ни одного вызова nnSdk, ни одного выделения
         * памяти в nn::fs. Вся работа с файлами отложена до открытия затвора
         * (OpenFsGate): до правки модуль монтировал SD прямо отсюда, и игра
         * падала на своём nn::fs::SetAllocator (Result 0x327602).
         */
        g_AddrNnMain      = FindSymbolAnywhere(SymNnMain);
        g_AddrOpenFile    = openFile;
        g_AddrMountSdCard = mountSd;

        /* Будет ли игра звать SetAllocator — видно по импортам её модулей. */
        g_GameCallsSetAllocator =
            nxr::HasImportAnywhere(g_MemQuery, SymSetAllocator);

        if (openFile != 0 && mountSd != 0 && !g_EarlyHookInstalled) {
            g_MountSdCard = reinterpret_cast<MountSdCardFn>(mountSd);
            OpenFileHook::InstallAtPtr(openFile);
            /* Узнавание точек romfs — там же, где OpenFile; при
               раннем перехвате оно уже стоит с exl_main и здесь не повторяется. */
            InstallMountHooks();
        }
        /* Не нашли — ничего не перенаправляем, игра работает как без мода. */

        /*
         * Хук на nn::fs::SetAllocator: он и открывает затвор — сразу после того,
         * как оригинал вернулся и распределитель игры встал на место.
         */
        const uintptr_t setAllocator = FindSymbolAnywhere(SymSetAllocator);
        if (setAllocator != 0) {
            g_HaveSetAllocator = true;
            SetAllocatorHook::InstallAtPtr(setAllocator);
        }

#ifndef NXR_NO_AUTOFIX
        /* nn::ro в старой sdk: до кода игры, которая зовёт LookupSymbol. */
        ApplyRoFix();
#endif

#ifdef NXR_COMPAT_NVRM_QUERY
        /*
         * NvRmMemQueryHandleParams: решение автоисправления (своя/прокси) и
         * проверка, выиграл ли наш экспорт у rtld; если нет и решено «своя» —
         * заменить заглушку sdk. Только чтение памяти и правка кода, nn::fs не
         * трогается. Игра ещё не рисовала.
         */
        ApplyNvrmFix();
#endif

#ifdef NXR_DIAG_GFX
        /*
         * Перехватчики графики — СЕЙЧАС, до оригинала nnMain: у
         * Well Dweller графика стартует раньше его nn::fs::SetAllocator, и
         * ставить их при открытии затвора поздно. nn::fs здесь не трогается:
         * строки отчёта копятся в памяти до затвора (nxr::gfx::StartReport).
         */
        nxr::gfx::Install(&GfxResolveSymbol, &GfxSymbolReport);
#endif

        Orig();

        /*
         * Оригинальный nnMain вернулся — игра закончилась. Если затвор так и не
         * открылся (игра не звала ни SetAllocator, ни OpenFile), открывать его
         * теперь незачем: работать уже не с чем.
         */
    }

};

#ifdef NXR_EARLY_FS_HOOK
/*
 * Примета сборки с ранним перехватом — для гейта
 * scripts/check_nso.py --early-fs: по готовому NSO иначе не отличить, с каким
 * ключом он собран, а мод со своим модулем без раннего перехвата теряет файлы,
 * которые тот читает в своём хуке nnMain. Строку держит ссылка из exl_main.
 */
extern "C" __attribute__((used)) const char nxr_early_fs_mark[] = "nxr-early-fs-hook";
#endif

extern "C" void exl_main(void* x0, void* x1) {
    EXL_UNUSED(x0, x1);
#ifdef NXR_EARLY_FS_HOOK
    /* Ссылка на примету: компоновщик не выбросит строку (--gc-sections). */
    asm volatile("" :: "r"(nxr_early_fs_mark));
#endif

    /* Готовим окружение для хуков (JIT-область под трамплины). */
    exl::hook::Initialize();

#ifdef NXR_DIAG
    /* Буфер ранних событий: контрольное слово ставится здесь, до любых хуков. */
    diag::g_Early.Init();
#endif

    /*
     * TitleID спрашиваем у ЯДРА, а не жёстко вписываем и не подставляем скриптом
     * (решение гибрида №1). svcGetInfo к nnSdk не обращается, поэтому его можно
     * звать уже здесь: путь мода готов до первого OpenFile.
     */
    const bool haveTitleId = InitSdPrefix();
#ifdef NXR_DIAG
    if (!haveTitleId) {
        /* Без TitleID перенаправлять некуда, а отчёт писать ещё нечем: сообщаем
           узнаваемым адресом аварии. */
        diag::Die(diag::Crash_NoTitleId);
    }
#else
    EXL_UNUSED(haveTitleId);
#endif

    /*
     * Хукаем только nnMain, и адрес берём поиском по имени. Если модуль `main` ещё
     * не виден (или символа нет) — ничего не делаем: игра запустится как без мода.
     * Всё, что касается nnSdk, делаем уже внутри этого хука — см. NnMainHook.
     */
    const uintptr_t nnMainAddr = FindSymbolAnywhere(SymNnMain);
    if (nnMainAddr != 0) {
        NnMainHook::InstallAtPtr(nnMainAddr);
    }
#ifdef NXR_DIAG
    /*
     * Без nnMain диагностика вообще не начнётся и отчёта не будет — поэтому
     * сообщаем об этом единственным доступным способом.
     * ИСПРАВЛЕНО: раньше здесь стоял `else` к if выше, но ПОСЛЕ
     * блока #ifdef NXR_EARLY_FS_HOOK; при обоих ключах между if и else
     * оказывался чужой блок, и сборка не компилировалась. Теперь проверка —
     * отдельным условием сразу за if, от соседних ключей не зависит.
     */
    if (nnMainAddr == 0) {
        diag::Die(diag::Crash_NoNnMain);
    }
#endif

#ifdef NXR_EARLY_FS_HOOK
    /*
     * РАННИЙ ХУК nn::fs::OpenFile (ключ сборки -DNXR_EARLY_FS_HOOK,
     * по умолчанию ВЫКЛЮЧЕН).
     *
     * Зачем. Обычный момент установки — хук nnMain, то есть точка входа ИГРЫ.
     * К этому моменту все модули exefs уже отработали свою инициализацию. Если
     * файл надо подменить ДРУГОМУ exefs-модулю (чужому моду, который читает свои
     * настройки в своей init), обычный момент безнадёжно опаздывает.
     *
     * ИСПРАВЛЕНО (было неверно): здесь стояло «UltraCam для TotK читает
     * "content:/UltraCam/maxlastbreath.ini" ВТОРЫМ вызовом своей точки входа,
     * задолго до nnMain». Разбор его subsdk3 показал иное: ini читается в СВОЁМ
     * хуке nnMain (колбэк 0x137f8 -> 0x16c2c), сначала MountSdCard("sd") и
     * MountRom("content") (0x15bd0), затем OpenFile. И он, и мы ставим хук на
     * nnMain, и OpenFile у нас встаёт ещё в нашем хуке nnMain. Без ключа этот
     * вызов проходит через наш перехватчик ТОЛЬКО если наш слот МЛАДШЕ слота мода
     * (так в рабочей сборке TotK: наш subsdk1, UltraCam subsdk3; по разбору
     * кода TotK; консолью не подтверждено). При нашем слоте
     * старше слота мода без ключа — нет: см. «КТО ВКЛЮЧАЕТ» ниже; поэтому для
     * модов со своим модулем exefs конвейер включает ключ сам. Модов, читающих
     * настройки в своём exl_main / init, пока не встречено.
     *
     * Что здесь можно и что нельзя. Поставить ХУК — можно: InstallAtPtr правит
     * код в памяти и к nn::fs не обращается. ОБРАЩАТЬСЯ к nn::fs — нельзя, и это
     * правило не нарушено: затвор (nxr_fsgate.hpp) остаётся закрытым, первый
     * перехваченный OpenFile по-прежнему уходит в оригинал БЕЗ подмены, и SD
     * монтируется только после того, как затвор откроется. Ранний хук сдвигает
     * момент ПЕРЕХВАТА, а не момент обращения к файловой системе.
     *
     * Почему это не поведение по умолчанию. Готов ли `sdk` к exl_main — ОТКРЫТЫЙ
     * ВОПРОС: по порядку загрузки (subsdk0..9, потом
     * sdk) выходит «ещё нет», по разбору кода rtld (абзац «КТО ВКЛЮЧАЕТ» ниже) —
     * `sdk` инициализируется первым; консолью порядок не проверен. Факт консоли
     * с консоли: попытка хукнуть nn::fs::OpenFile прямо в exl_main ушла по
     * нулевому адресу и уронила игру; почему адрес был нулевым — не выяснено.
     * Поэтому код делает так: оба адреса (OpenFile и MountSdCard) берутся поиском
     * и проверяются на 0; не нашли хоть один — ничего не ставим, и всё идёт
     * обычным путём через хук nnMain. Гарантии, что `sdk` уже виден, нет, и ради
     * одной чужой игры менять общий путь нельзя.
     *
     * КТО ВКЛЮЧАЕТ. Конвейер сам: если мод несёт СВОЙ модуль
     * exefs (subsdkN), сборка идёт с этим ключом (nxr_build.py, early_fs). Почему
     * без него нельзя: порядок инициализации модулей — по коду rtld TotK
     * (функция 0x860 идёт по списку от СТАРШЕГО адреса к младшему: sdk, subsdk9,
     * …, subsdk0, main; список строится вставкой в голову при обходе памяти снизу
     * вверх, 0x5b0..0x5c8). Хук, поставленный позже, срабатывает РАНЬШЕ. Наш
     * модуль в старшем слоте инициализируется раньше модуля мода — и его хук
     * nnMain отработает ПОСЛЕ хука мода: UltraCam прочёл бы ini до того, как наш
     * хук nnMain поставит OpenFile. Ранний перехват снимает зависимость от
     * порядка совсем. На консоли порядок не проверялся — это вывод по коду.
     * Для игр без модуля мода ключ по-прежнему выключен: на них он не проверен.
     *
     * g_GameCallsSetAllocator здесь считается сразу (обычно это делает хук
     * nnMain): без него затвор решал бы судьбу первого же перехваченного вызова
     * по значению по умолчанию, а не по импортам игры. Сам подсчёт идёт по памяти
     * процесса (nxr::HasImportAnywhere), nn::fs не трогает.
     */
    {
        const uintptr_t earlyOpenFile = FindSymbolAnywhere(SymOpenFile);
        const uintptr_t earlyMountSd  = FindSymbolAnywhere(SymMountSdCard);
        if (earlyOpenFile != 0 && earlyMountSd != 0) {
            g_GameCallsSetAllocator =
                nxr::HasImportAnywhere(g_MemQuery, SymSetAllocator);
            g_AddrOpenFile       = earlyOpenFile;
            g_AddrMountSdCard    = earlyMountSd;
            g_MountSdCard        = reinterpret_cast<MountSdCardFn>(earlyMountSd);
            OpenFileHook::InstallAtPtr(earlyOpenFile);
            g_EarlyHookInstalled = true;
            /* Узнавание точек romfs — тоже рано: мод, читающий
               файлы в своём хуке nnMain, монтирует romfs там же (UltraCam:
               MountRom("content") в 0x15bd0), и к этому моменту перехват
               обязан стоять. Сами перехватчики к nn::fs не обращаются. */
            InstallMountHooks();
        }
    }
#endif

    /*
     * SD здесь НЕ монтируем — и не только потому, что nnSdk ещё не готов.
     * Главное правило теперь такое: модуль не трогает nn::fs, пока не открыт
     * затвор (nxr_fsgate.hpp), то есть пока игра не задала свой распределитель
     * nn::fs или не обратилась к nn::fs сама. Раннее монтирование ломало игру
     * (Result 0x327602, проба на консоли с Well Dweller).
     * Если смонтировать потом не удастся, хук просто зовёт оригинал и игра
     * работает как обычно.
     */
}

extern "C" NORETURN void exl_exception_entry() {
    /* Актуально только для апплетов/сисмодулей; мы грузимся в процесс игры. */
    EXL_ABORT("Default exception handler called!");
}
