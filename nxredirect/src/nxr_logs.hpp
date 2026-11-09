/*
 * nxr_logs — где и под каким именем отладочный модуль пишет отчёты, и ротация.
 *
 * Copyright (c) 2026, участники проекта nxredirect. GNU GPL версии 2.
 *
 * ОТЧЁТЫ ПО ИГРАМ. Раньше отчёты лежали в КОРНЕ карты одним файлом
 * на вид (nxredirect-diag.txt, nxredirect-gfx.txt) и перезаписывались при каждом
 * запуске любой игры: отчёт второй игры молча затирал первый, а к какой игре
 * относится файл, было видно только изнутри. Теперь:
 *
 *   sd:/switch/ModKitNX/logs/<TitleID 16 hex ЗАГЛАВНЫМИ>/diag-<NNNN>.txt
 *   sd:/switch/ModKitNX/logs/<TitleID 16 hex ЗАГЛАВНЫМИ>/gfx-<NNNN>.txt
 *
 * (внутри процесса игры — под нашей точкой монтирования, "nxrsd:/switch/…").
 * NNNN — монотонный номер запуска, не меньше 4 цифр с ведущими нулями
 * (0001, 0002, …, 9999, 10000, …; предел 99999999). Каждого вида на игру
 * хранится не больше Keep = 5 файлов: перед созданием нового модуль удаляет
 * самые старые (с наименьшими номерами), чтобы с новым их стало ровно 5.
 * Папки создаются модулем (nn::fs::CreateDirectory по каждому звену пути).
 *
 * ПОЧЕМУ НОМЕР, А НЕ ДАТА В ИМЕНИ. Календарное время модулю
 * доступно только через nn::time игры: nn::time::IsInitialized — простая проверка,
 * а StandardUserSystemClock::GetCurrentTime и ToCalendarTime — обращения к службе
 * времени через сессию, которую открыла САМА игра. Звать nn::time::Initialize
 * самим нельзя: права игры на службу time:* мы не проверяем, а отказ в nnSdk —
 * Abort. Отчёт же заводится при открытии затвора ФС — в самом начале nnMain
 * (SetAllocator или первый OpenFile), когда игра свой nn::time обычно ЕЩЁ НЕ
 * запустила. Значит, дата в имени была бы то есть, то нет, и имена шли бы
 * вперемешку. Номер берётся из самой папки (наибольший + 1), не зависит ни от
 * часов, ни от перезагрузки консоли и сортируется. Время — СТРОКОЙ ВНУТРИ
 * отчёта: если nn::time уже запущен — сразу, иначе дописывается позже, как
 * только игра его запустит (диагностика — при очередном OpenFile, графика — на
 * первых кадрах). Плюс всегда — счётчик тиков ядра с включения консоли
 * (cntpct_el0, 19,2 МГц): он есть всегда и ничего не вызывает.
 *
 * ПЕРВАЯ СТРОКА ОТЧЁТА (после BOM UTF-8) — привязка к игре, машиночитаемо:
 *   TitleID=<16 HEX> BuildID=<40 HEX | ?> report=<вид>-<NNNN>
 * BuildID — GNU build-id модуля main игры (первые 20 байт ModuleId из заголовка
 * NSO; его же Atmosphère показывает как BID и по нему ищет IPS-патчи). «?» —
 * не нашёлся в памяти. Строки дальше — CRLF, текст по-русски.
 *
 * Чистая часть (раскладка пути, разбор имени, ротация, первая строка, формат
 * даты) — без exlaunch и nnSdk; её прогоняет хост-тест hosttest/logs_test.cpp
 * (в составе run_fsgatetest.sh) на поддельной файловой системе. Часть для
 * консоли (вызовы nn::fs и nn::time по найденным символам) — под
 * NXR_LOGS_DEVICE, её включает только src/main.cpp и nxr_gfx.hpp.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "nxr_path.hpp"   /* NXR_SD_MOUNT — наше имя точки монтирования SD */

/* Корень отчётов на карте. Общий для всех видов и всех игр. */
#define NXR_LOGS_ROOT NXR_SD_MOUNT ":/switch/ModKitNX/logs/"

namespace nxr::logs {

    /*
     * ШАБЛОНЫ ПУТИ — по одному литералу на вид. "{TID}" заменяется на TitleID,
     * последнее звено после '/' — приставка имени файла. Литерал целиком лежит
     * в образе модуля и служит ПРИМЕТОЙ сборки для scripts/check_nso.py
     * (--diag ищет первый, --diag-gfx — второй; без ключа их быть не должно).
     */
    constexpr char DiagTemplate[] = NXR_LOGS_ROOT "{TID}/diag-";
    constexpr char GfxTemplate[]  = NXR_LOGS_ROOT "{TID}/gfx-";

    constexpr char          TidMark[]  = "{TID}";
    constexpr std::size_t   TidMarkLen = sizeof(TidMark) - 1;
    constexpr char          Ext[]      = ".txt";
    constexpr std::size_t   ExtLen     = sizeof(Ext) - 1;

    /* Сколько отчётов КАЖДОГО вида хранить на игру (вместе с новым). */
    constexpr int           Keep        = 5;
    constexpr int           MinDigits   = 4;
    constexpr std::uint32_t MaxNumber   = 99999999u;
    /* Сколько старых файлов удалить за один запуск (остальные — в следующих). */
    constexpr int           MaxDeletePerRun = 16;
    /* Буфер полного пути: корень 28 + TID 16 + '/' + "diag-" + 8 цифр + ".txt". */
    constexpr std::size_t   PathSize    = 128;

    /* Минимальный построитель строки; переполнение — молча обрезает. */
    struct Out {
        char*       m_Buf;
        std::size_t m_Cap;
        std::size_t m_Len = 0;

        Out(char* buf, std::size_t cap) : m_Buf(buf), m_Cap(cap) {
            if (m_Cap != 0) {
                m_Buf[0] = '\0';
            }
        }
        bool Ok() const { return m_Cap != 0 && m_Len + 1 < m_Cap; }
        void Ch(char c) {
            if (m_Cap != 0 && m_Len + 1 < m_Cap) {
                m_Buf[m_Len++] = c;
                m_Buf[m_Len] = '\0';
            } else {
                m_Full = true;
            }
        }
        void Add(const char* s) {
            while (s != nullptr && *s != '\0') {
                Ch(*s++);
            }
        }
        void AddN(const char* s, std::size_t n) {
            for (std::size_t i = 0; i < n; ++i) {
                Ch(s[i]);
            }
        }
        /* Десятичное число, не короче width цифр (ведущие нули). */
        void Dec(std::uint64_t v, int width = 1) {
            char tmp[24];
            int n = 0;
            do {
                tmp[n++] = static_cast<char>('0' + (v % 10));
                v /= 10;
            } while (v != 0 && n < 20);
            while (n < width && n < 20) {
                tmp[n++] = '0';
            }
            while (n > 0) {
                Ch(tmp[--n]);
            }
        }
        /* Ровно digits шестнадцатеричных цифр, ЗАГЛАВНЫЕ (как каталоги Atmosphère). */
        void HexUpper(std::uint64_t v, int digits) {
            for (int i = digits - 1; i >= 0; --i) {
                const std::uint32_t nib = static_cast<std::uint32_t>((v >> (i * 4)) & 0xF);
                Ch(static_cast<char>(nib < 10 ? ('0' + nib) : ('A' + nib - 10)));
            }
        }
        bool m_Full = false;
    };

    inline std::size_t Len(const char* s) {
        std::size_t n = 0;
        while (s != nullptr && s[n] != '\0') {
            ++n;
        }
        return n;
    }

    /*
     * Раскладка шаблона под игру: каталог "<корень><TID>" и приставка ("diag-").
     * Каталог пишется в dir (без завершающего '/'), приставка — указатель внутрь
     * шаблона. false — шаблон без "{TID}" или буфер мал.
     */
    struct Layout {
        char        dir[PathSize] = {};
        std::size_t dirLen        = 0;
        const char* prefix        = nullptr;
        std::size_t prefixLen     = 0;
    };

    inline bool Expand(Layout* L, const char* tmpl, std::uint64_t tid) {
        if (L == nullptr || tmpl == nullptr) {
            return false;
        }
        const std::size_t n = Len(tmpl);
        std::size_t mark = n;
        for (std::size_t i = 0; i + TidMarkLen <= n; ++i) {
            std::size_t k = 0;
            while (k < TidMarkLen && tmpl[i + k] == TidMark[k]) {
                ++k;
            }
            if (k == TidMarkLen) {
                mark = i;
                break;
            }
        }
        if (mark == n) {
            return false;
        }
        std::size_t lastSlash = n;
        for (std::size_t i = mark + TidMarkLen; i < n; ++i) {
            if (tmpl[i] == '/') {
                lastSlash = i;
            }
        }
        if (lastSlash == n || lastSlash + 1 >= n) {
            return false;   /* нет приставки имени файла */
        }
        Out o(L->dir, sizeof(L->dir));
        o.AddN(tmpl, mark);
        o.HexUpper(tid, 16);
        o.AddN(tmpl + mark + TidMarkLen, lastSlash - (mark + TidMarkLen));
        if (o.m_Full) {
            return false;
        }
        L->dirLen    = o.m_Len;
        L->prefix    = tmpl + lastSlash + 1;
        L->prefixLen = n - (lastSlash + 1);
        return true;
    }

    /*
     * Звенья каталога по порядку, от корня карты: для
     * "nxrsd:/switch/ModKitNX/logs/0100…" это "nxrsd:/switch", "nxrsd:/switch/ModKitNX",
     * "nxrsd:/switch/ModKitNX/logs", "nxrsd:/switch/ModKitNX/logs/0100…".
     * Сам корень "nxrsd:/" не создаётся. Звать f(const char* path).
     */
    template <typename F>
    inline int ForEachDirLink(const Layout& L, F&& f) {
        char tmp[PathSize];
        std::size_t start = 0;
        for (std::size_t i = 0; i + 1 < L.dirLen; ++i) {
            if (L.dir[i] == ':' && L.dir[i + 1] == '/') {
                start = i + 2;
                break;
            }
        }
        int count = 0;
        for (std::size_t i = start; i <= L.dirLen; ++i) {
            if (i == L.dirLen || (L.dir[i] == '/' && i > start)) {
                for (std::size_t k = 0; k < i; ++k) {
                    tmp[k] = L.dir[k];
                }
                tmp[i] = '\0';
                f(static_cast<const char*>(tmp));
                ++count;
            }
        }
        return count;
    }

    /* Полный путь файла с номером n: "<dir>/<prefix><NNNN>.txt". 0 — не влез. */
    inline std::size_t FilePath(char* out, std::size_t outSize, const Layout& L, std::uint32_t n) {
        Out o(out, outSize);
        o.AddN(L.dir, L.dirLen);
        o.Ch('/');
        o.AddN(L.prefix, L.prefixLen);
        o.Dec(n, MinDigits);
        o.Add(Ext);
        return o.m_Full ? 0 : o.m_Len;
    }

    /*
     * Номер из имени файла: "<prefix><цифры>.txt" (цифр 1..9, ничего лишнего).
     * Чужие и посторонние файлы в папке (другой вид, «diag-0003 (1).txt»,
     * «readme.txt») — false: их ротация не трогает.
     */
    inline bool ParseNumber(const char* name, const char* prefix, std::size_t prefixLen,
                            std::uint32_t* out) {
        if (name == nullptr || prefix == nullptr) {
            return false;
        }
        for (std::size_t i = 0; i < prefixLen; ++i) {
            if (name[i] != prefix[i]) {
                return false;
            }
        }
        std::size_t p = prefixLen;
        std::uint64_t v = 0;
        int digits = 0;
        while (name[p] >= '0' && name[p] <= '9') {
            v = v * 10 + static_cast<std::uint64_t>(name[p] - '0');
            ++digits;
            ++p;
            if (digits > 9) {
                return false;
            }
        }
        if (digits == 0) {
            return false;
        }
        for (std::size_t i = 0; i < ExtLen; ++i) {
            if (name[p + i] != Ext[i]) {
                return false;
            }
        }
        if (name[p + ExtLen] != '\0') {
            return false;
        }
        if (out != nullptr) {
            *out = static_cast<std::uint32_t>(v);
        }
        return true;
    }

    /*
     * РОТАЦИЯ. Первый проход по папке — Add(номер) для каждого своего файла:
     * запоминаются наибольший номер и Keep-1 самых больших (они остаются).
     * Next() — номер нового отчёта. Второй проход — ShouldDelete(номер): всё,
     * что меньше оставляемых, удаляется. Итог: Keep-1 старых + новый = Keep.
     * Без массивов по числу файлов: папка может быть любой длины.
     */
    struct Rotation {
        std::uint32_t m_Top[Keep - 1] = {};   /* по убыванию */
        int           m_TopCount      = 0;
        std::uint32_t m_Max           = 0;
        int           m_Seen          = 0;

        void Add(std::uint32_t n) {
            ++m_Seen;
            if (n > m_Max) {
                m_Max = n;
            }
            for (int i = 0; i < m_TopCount; ++i) {
                if (m_Top[i] == n) {
                    return;   /* тот же номер дважды (быть не должно) */
                }
            }
            int pos = m_TopCount;
            while (pos > 0 && m_Top[pos - 1] < n) {
                --pos;
            }
            if (pos >= Keep - 1) {
                return;
            }
            const int last = (m_TopCount < Keep - 1) ? m_TopCount : Keep - 2;
            for (int i = last; i > pos; --i) {
                m_Top[i] = m_Top[i - 1];
            }
            m_Top[pos] = n;
            if (m_TopCount < Keep - 1) {
                ++m_TopCount;
            }
        }

        /* Номер нового отчёта. На пределе — последний номер (файл перезапишется). */
        std::uint32_t Next() const {
            return m_Max >= MaxNumber ? MaxNumber : m_Max + 1;
        }

        bool ShouldDelete(std::uint32_t n) const {
            if (m_TopCount < Keep - 1) {
                return false;   /* меньше Keep-1 файлов — удалять нечего */
            }
            return n < m_Top[m_TopCount - 1];
        }
    };

    /* Что сделала подготовка: для строки в отчёте и для хост-теста. */
    struct Prepared {
        std::uint32_t number    = 0;      /* 0 — путь не построен, отчёта не будет */
        bool          listed    = false;  /* папку удалось прочитать */
        int           found     = 0;      /* своих отчётов этого вида было */
        int           deleted   = 0;      /* удалено старых */
        int           dirsMade  = 0;      /* звеньев каталога пройдено */
    };

    /*
     * ПОДГОТОВКА НОВОГО ОТЧЁТА: создать папки, прочитать номера, удалить лишние
     * старые, выдать путь нового файла. Сам файл заводит вызывающий (Delete на
     * случай остатка, Create, Open) — как и раньше.
     *
     * Fs — переходник: MakeDir(path) (ответ не важен: «уже есть» — норма),
     * List(dir, f) → false, если папку не открыть (f(const char* имя) на каждый
     * ФАЙЛ), Remove(path). На консоли это nn::fs игры (device::NnFs), в хост-тесте —
     * поддельная ФС в памяти.
     *
     * Если папку прочитать не удалось (нет функций каталогов) — номер 1:
     * отчёт всё равно будет, просто один и перезаписываемый, как в прежних сборках.
     */
    template <typename Fs>
    inline Prepared Prepare(Fs& fs, const char* tmpl, std::uint64_t tid,
                            char* outPath, std::size_t outSize) {
        Prepared r {};
        Layout L {};
        if (!Expand(&L, tmpl, tid)) {
            return r;
        }
        r.dirsMade = ForEachDirLink(L, [&](const char* p) { fs.MakeDir(p); });

        Rotation rot {};
        r.listed = fs.List(L.dir, [&](const char* name) {
            std::uint32_t n = 0;
            if (ParseNumber(name, L.prefix, L.prefixLen, &n)) {
                rot.Add(n);
            }
        });
        r.found = rot.m_Seen;

        if (r.listed && rot.m_TopCount >= Keep - 1) {
            /* Второй проход: собрать, кого удалить; удалять — после закрытия папки. */
            std::uint32_t victims[MaxDeletePerRun] = {};
            int nv = 0;
            fs.List(L.dir, [&](const char* name) {
                std::uint32_t n = 0;
                if (nv < MaxDeletePerRun && ParseNumber(name, L.prefix, L.prefixLen, &n) &&
                    rot.ShouldDelete(n)) {
                    victims[nv++] = n;
                }
            });
            char victimPath[PathSize];
            for (int i = 0; i < nv; ++i) {
                if (FilePath(victimPath, sizeof(victimPath), L, victims[i]) != 0) {
                    fs.Remove(victimPath);
                    ++r.deleted;
                }
            }
        }

        const std::uint32_t next = r.listed ? rot.Next() : 1u;
        if (FilePath(outPath, outSize, L, next) == 0) {
            return r;
        }
        r.number = next;
        return r;
    }

    /* Сведения для первой строки. */
    struct Head {
        std::uint64_t titleId    = 0;
        bool          haveBuildId = false;
        unsigned char buildId[20] = {};
    };

    /*
     * ПЕРВАЯ СТРОКА: "TitleID=<16 HEX> BuildID=<40 HEX|?> report=<вид>-<NNNN>".
     * kindPrefix — приставка имени ("diag-"/"gfx-"), как в шаблоне. Без CRLF.
     */
    inline std::size_t FormatHead(char* out, std::size_t outSize, const Head& h,
                                  const char* kindPrefix, std::uint32_t number) {
        Out o(out, outSize);
        o.Add("TitleID=");
        o.HexUpper(h.titleId, 16);
        o.Add(" BuildID=");
        if (h.haveBuildId) {
            for (int i = 0; i < 20; ++i) {
                o.HexUpper(h.buildId[i], 2);
            }
        } else {
            o.Ch('?');
        }
        o.Add(" report=");
        o.Add(kindPrefix);
        o.Dec(number, MinDigits);
        return o.m_Len;
    }

    /* Приставка вида из шаблона ("diag-"): последнее звено после '/'. */
    inline const char* KindPrefix(const char* tmpl) {
        const char* p = tmpl;
        const char* last = tmpl;
        while (p != nullptr && *p != '\0') {
            if (*p == '/') {
                last = p + 1;
            }
            ++p;
        }
        return last;
    }

    /* Дата-время: "ГГГГ-ММ-ДД ЧЧ:ММ:СС". */
    inline void AddDateTime(Out& o, int y, int mo, int d, int h, int mi, int s) {
        o.Dec(static_cast<std::uint64_t>(y < 0 ? 0 : y), 4);
        o.Ch('-');
        o.Dec(static_cast<std::uint64_t>(mo), 2);
        o.Ch('-');
        o.Dec(static_cast<std::uint64_t>(d), 2);
        o.Ch(' ');
        o.Dec(static_cast<std::uint64_t>(h), 2);
        o.Ch(':');
        o.Dec(static_cast<std::uint64_t>(mi), 2);
        o.Ch(':');
        o.Dec(static_cast<std::uint64_t>(s), 2);
    }

    /*
     * Секунды POSIX -> дата UTC (алгоритм days_from_civil Говарда Хиннанта,
     * обратный ход). Нужен, только если ToCalendarTime игры отказал, а сами
     * часы ответили: тогда пишем UTC и так и помечаем.
     */
    inline void CivilFromPosix(std::int64_t t, int* y, int* mo, int* d, int* h, int* mi, int* s) {
        std::int64_t days = t / 86400;
        std::int64_t rem  = t % 86400;
        if (rem < 0) {
            rem += 86400;
            --days;
        }
        *h  = static_cast<int>(rem / 3600);
        *mi = static_cast<int>((rem % 3600) / 60);
        *s  = static_cast<int>(rem % 60);
        const std::int64_t z   = days + 719468;
        const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const std::int64_t doe = z - era * 146097;
        const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const std::int64_t mp  = (5 * doy + 2) / 153;
        const std::int64_t dd  = doy - (153 * mp + 2) / 5 + 1;
        const std::int64_t mm  = mp < 10 ? mp + 3 : mp - 9;
        *y  = static_cast<int>(yoe + era * 400 + (mm <= 2 ? 1 : 0));
        *mo = static_cast<int>(mm);
        *d  = static_cast<int>(dd);
    }

    /* Тики ядра (19,2 МГц) -> "Ч:ММ:СС с включения консоли". */
    constexpr std::uint64_t TickHz = 19200000ull;
    inline void AddUptime(Out& o, std::uint64_t ticks) {
        const std::uint64_t sec = ticks / TickHz;
        o.Dec(sec / 3600);
        o.Ch(':');
        o.Dec((sec / 60) % 60, 2);
        o.Ch(':');
        o.Dec(sec % 60, 2);
    }

}  // namespace nxr::logs

/* ====================================================================== */
/* ЧАСТЬ ДЛЯ КОНСОЛИ: nn::fs и nn::time игры по найденным символам.        */
/* Включается из src/main.cpp (#define NXR_LOGS_DEVICE перед include).     */
/* Ни одного вызова до открытия затвора ФС: зовёт её только OpenFsGate.    */
/* ====================================================================== */
#ifdef NXR_LOGS_DEVICE

namespace nxr::logs::device {

    using ResolveFn = std::uint64_t (*)(const char*);

    /*
     * nn::fs::DirectoryEntry по раскладке nnSdk: имя 0x301, 3 байта запаса,
     * тип (s8) на +0x304, 3 байта, размер (s64) на +0x308; всего 0x310.
     * Своя структура, а не из exlaunch: там поле типа стоит на +0x301 (неверно).
     * Тип мы не читаем — каталог открывается только на файлы.
     */
    struct DirEntry {
        char          name[0x301];
        char          reserved0[3];
        std::int8_t   type;
        char          reserved1[3];
        std::int64_t  size;
    };
    static_assert(sizeof(DirEntry) == 0x310, "раскладка nn::fs::DirectoryEntry");

    constexpr int OpenDirectoryModeFile = 2;

    /* Переходник Prepare() на nn::fs игры. */
    struct NnFs {
        using CreateDirFn = Result (*)(const char*);
        using OpenDirFn   = Result (*)(nn::fs::DirectoryHandle*, const char*, int);
        using ReadDirFn   = Result (*)(s64*, DirEntry*, nn::fs::DirectoryHandle, s64);
        using CloseDirFn  = void   (*)(nn::fs::DirectoryHandle);
        using DeleteFn    = Result (*)(const char*);

        CreateDirFn m_Mk    = nullptr;
        OpenDirFn   m_Open  = nullptr;
        ReadDirFn   m_Read  = nullptr;
        CloseDirFn  m_Close = nullptr;
        DeleteFn    m_Del   = nullptr;

        void Init(ResolveFn resolve) {
            m_Mk    = reinterpret_cast<CreateDirFn>(resolve("_ZN2nn2fs15CreateDirectoryEPKc"));
            m_Open  = reinterpret_cast<OpenDirFn>(resolve("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci"));
            m_Read  = reinterpret_cast<ReadDirFn>(resolve("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl"));
            m_Close = reinterpret_cast<CloseDirFn>(resolve("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE"));
            m_Del   = reinterpret_cast<DeleteFn>(resolve("_ZN2nn2fs10DeleteFileEPKc"));
        }

        void MakeDir(const char* path) {
            if (m_Mk != nullptr) {
                m_Mk(path);   /* «уже есть» (0x402) — норма */
            }
        }

        template <typename F>
        bool List(const char* dir, F&& f) {
            if (m_Open == nullptr || m_Read == nullptr || m_Close == nullptr) {
                return false;
            }
            nn::fs::DirectoryHandle h {};
            if (R_FAILED(m_Open(&h, dir, OpenDirectoryModeFile))) {
                return false;
            }
            DirEntry e;
            /* Предохранитель от бесконечного чтения: столько файлов в папке
               отчётов не бывает. */
            for (int guard = 0; guard < 4096; ++guard) {
                s64 n = 0;
                if (R_FAILED(m_Read(&n, &e, h, 1)) || n <= 0) {
                    break;
                }
                e.name[sizeof(e.name) - 1] = '\0';
                f(static_cast<const char*>(e.name));
            }
            m_Close(h);
            return true;
        }

        void Remove(const char* path) {
            if (m_Del != nullptr) {
                m_Del(path);
            }
        }
    };

    /*
     * ЧАСЫ КОНСОЛИ через nn::time ИГРЫ. Ничего не запускает сам: если игра ещё
     * не вызвала nn::time::Initialize (IsInitialized() == false) — ответа нет.
     * Символы ищутся один раз (Init, после затвора ФС).
     */
    struct PosixTime { std::int64_t value; };
    struct CalendarTime {
        std::int16_t year;
        std::int8_t  month, day, hour, minute, second;
        std::int8_t  pad;
    };

    struct Clock {
        using IsInitFn = bool   (*)();
        using NowFn    = Result (*)(PosixTime*);
        using ToCalFn  = Result (*)(CalendarTime*, void*, const PosixTime&);

        IsInitFn m_IsInit = nullptr;
        NowFn    m_Now    = nullptr;
        ToCalFn  m_ToCal  = nullptr;

        void Init(ResolveFn resolve) {
            m_IsInit = reinterpret_cast<IsInitFn>(resolve("_ZN2nn4time13IsInitializedEv"));
            m_Now    = reinterpret_cast<NowFn>(resolve(
                "_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE"));
            m_ToCal  = reinterpret_cast<ToCalFn>(resolve(
                "_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE"));
        }

        bool Available() const { return m_IsInit != nullptr && m_Now != nullptr; }

        /*
         * Дописывает в o "ГГГГ-ММ-ДД ЧЧ:ММ:СС (местное)" или "(UTC)".
         * false — часов нет (символов нет, nn::time игрой не запущен, отказ).
         */
        bool Format(Out& o) const {
            if (!Available() || !m_IsInit()) {
                return false;
            }
            PosixTime t {0};
            if (R_FAILED(m_Now(&t))) {
                return false;
            }
            if (m_ToCal != nullptr) {
                CalendarTime c {};
                /* CalendarAdditionalInfo — 0x18 байт; берём с запасом. */
                alignas(8) unsigned char info[0x40] = {};
                if (R_SUCCEEDED(m_ToCal(&c, info, t))) {
                    AddDateTime(o, c.year, c.month, c.day, c.hour, c.minute, c.second);
                    o.Add(" (местное время консоли)");
                    return true;
                }
            }
            int y, mo, d, h, mi, s;
            CivilFromPosix(t.value, &y, &mo, &d, &h, &mi, &s);
            AddDateTime(o, y, mo, d, h, mi, s);
            o.Add(" (UTC: перевод в местное отказал)");
            return true;
        }
    };

    /* Тики ядра с включения консоли: чтение регистра, без вызовов. */
    inline std::uint64_t Ticks() {
        std::uint64_t v;
        __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(v));
        return v;
    }

}  // namespace nxr::logs::device

#endif  /* NXR_LOGS_DEVICE */
