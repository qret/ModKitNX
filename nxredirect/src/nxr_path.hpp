/*
 * nxr_path — построение пути мода на SD и преобразование пути игры в путь на SD.
 *
 * Copyright (c) 2026, участники проекта nxredirect
 * Лицензия GNU GPL версии 2 (как и весь модуль, основанный на exlaunch).
 *
 * Зачем отдельный файл
 * --------------------
 * Ровно этот код исполняется и в модуле на консоли (src/main.cpp), и в хост-тесте на
 * ПК (hosttest/path_test.cpp). Логика подмены путей — то место, где прежний проект
 * ошибся дважды (фиксированный префикс "rom:/", из-за которого перенаправление молча
 * не работало, и жёсткий путь мода в корне SD), поэтому она проверяется на ПК, а не
 * пробой на консоли.
 *
 * Никаких зависимостей от exlaunch, libnx и nnSdk здесь нет — только <cstdint>
 * и <atomic> (реестр точек romfs, RomMounts, читается из нескольких потоков).
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

/*
 * ИМЯ НАШЕЙ ТОЧКИ МОНТИРОВАНИЯ SD — СВОЁ, А НЕ ОБЩЕЕ "sd".
 *
 * Что было. Модуль монтировал SD под именем "sd". Разбор краха Zelda: Tears of the
 * Kingdom с чужим модом UltraCam показал, к чему это ведёт: UltraCam в своём хуке
 * nnMain сам зовёт nn::fs::MountSdCard("sd") (результат не проверяет), а мы
 * монтируем позже. nn::fs::detail::MountTable::Mount (nnSdk 15.3.1 @0x9abf0)
 * сравнивает имена и отдаёт 0x7802 «имя уже занято» (mov w21,#0x7802 @0x9ac30) —
 * наш MountSdOnce ставил Failed, и подмена файлов выключалась до конца сессии.
 * Чужой мод, который сам монтирует "sd", — обычное дело (так делает любой
 * exlaunch-мод с настройками на карте).
 *
 * Почему не «считать 0x7802 успехом»: тогда мы читали бы ЧУЖУЮ точку "sd", не зная,
 * что за ней (чужой мод мог смонтировать под этим именем что угодно, или
 * размонтировать её в любой момент). Своё имя делает конфликт невозможным.
 *
 * Имя задаётся макросом, чтобы из него же собирались строковые литералы путей
 * отчётов (сейчас "nxrsd:/switch/ModKitNX/logs/…", nxr_logs.hpp; в прежних сборках
 * "nxrsd:/nxredirect-diag.txt") — одно место, без ручных копий.
 * Пределы nn::fs: не длиннее 15 байт, не начинается с '@'. Хост-тест path_test
 * проверяет, что имя не совпадает с общими ("sd", "sdmc", "sdcard"), и что все
 * пути модуля начинаются именно с него.
 */
#define NXR_SD_MOUNT "nxrsd"

namespace nxr {

    /* Имя точки монтирования SD, под которым её монтирует ТОЛЬКО наш модуль. */
    constexpr char   SdMountName[]   = NXR_SD_MOUNT;
    constexpr size_t SdMountNameLen  = sizeof(SdMountName) - 1;

    /*
     * Путь мода на SD: nxrsd:/atmosphere/contents/<TID 16 hex ЗАГЛАВНЫМИ>/files/
     * (на самой карте это sd:/atmosphere/contents/<TID>/files/ — имя точки
     * монтирования есть только внутри процесса игры).
     *
     * Почему именно так (решение гибрида №2): папка мода лежит внутри каталога самой
     * игры в Atmosphère, рядом с exefs/, куда кладётся сам модуль, — весь мод
     * удаляется одной папкой. Подпапка `files` LayeredFS НЕ включает: fs.mitm строит
     * таблицу romfs только при romfs.bin или непустой romfs/ (HasSdRomfsContent,
     * amsmitm_fs_utils.cpp — проверено по исходнику: ровно эти два пути).
     * Регистр заглавный — так каталоги называет сама Atmosphère.
     */
    constexpr char   SdRoot[]      = NXR_SD_MOUNT ":/atmosphere/contents/";
    constexpr size_t SdRootLen     = sizeof(SdRoot) - 1;
    constexpr char   SdLeaf[]      = "/files/";
    constexpr size_t SdLeafLen     = sizeof(SdLeaf) - 1;
    constexpr size_t TitleIdChars  = 16;
    /* Сколько байт нужно под префикс вместе с нулём. */
    constexpr size_t ModPrefixSize = SdRootLen + TitleIdChars + SdLeafLen + 1;

    /* Максимальная длина имени точки монтирования в nn::fs. */
    constexpr size_t MaxMountNameLen = 15;

    /*
     * ============ ЧТО ПОДМЕНЯТЬ МОЖНО: РЕЖИМ ОТКРЫТИЯ ============
     *
     * nn::fs::OpenFile(FileHandle*, char const*, int mode). Биты mode (nn::fs::OpenMode,
     * exlaunch/source/nn/fs/fs_types.hpp, там же у nnSdk и libnx):
     *     BIT(0) Read, BIT(1) Write, BIT(2) Append (в свежем nnSdk — AllowAppend).
     * Старшие биты не документированы: в разных версиях nnSdk там появлялись
     * внутренние флаги (например «не создавать», кеш), и их значение мы проверить
     * не можем.
     *
     * Поэтому правило СТРОГОЕ: перенаправляем ТОЛЬКО mode == Read (ровно 1).
     * Любой другой mode — чтение с записью, дозапись, неизвестный флаг — уходит в
     * оригинал без подмены. Иначе игра, открывшая файл на запись, писала бы в наш
     * файл на SD (порча данных мода) или, того хуже, получала бы наш файл там, где
     * ждёт свой изменяемый. Отказ в подмене стоит лишь того, что подмена не
     * сработает: игра прочитает свой файл, как будто мода нет.
     */
    constexpr int OpenModeRead = 1;

    inline bool IsRedirectableMode(int mode) {
        return mode == OpenModeRead;
    }

    /*
     * ============ ЧТО ПОДМЕНЯТЬ НЕЛЬЗЯ: ТОЧКИ МОНТИРОВАНИЯ ============
     *
     * Подменяем только ДАННЫЕ игры (её romfs под своим именем: "app0", "rom",
     * "asset", "Contents", …). Всё, что ниже, — не данные игры, а хранилища, где
     * подмена либо теряет запись игрока, либо просто бессмысленна. Имя сравнивается
     * БЕЗ УЧЁТА РЕГИСТРА ASCII: nnSdk имя точки монтирования не нормализует, и
     * "Save:/" открыл бы то же хранилище, что "save:/".
     *
     * Каждое имя и почему оно здесь (имена типовые для nnSdk/homebrew; сама nnSdk
     * имя задаёт вызывающий, поэтому список — по принятым именам, а не по константам):
     *   save, saveData — nn::fs::MountSaveData: сохранения игрока. Подмена чтения
     *                    сейва отдала бы игре чужой файл, а игра затем перезаписала
     *                    бы им настоящий сейв. Самое дорогое, что есть на консоли.
     *   bcat           — nn::fs::MountBcatSaveData: доставленные данные BCAT, тоже
     *                    сейв-раздел приложения.
     *   system         — nn::fs::MountSystemSaveData: системные сохранения.
     *   user           — принятое имя для пользовательского сейва/каталога игрока
     *                    (MountSaveDataForDebug и самодельные моды зовут его так).
     *   cache          — nn::fs::MountCacheStorage: кеш-хранилище приложения,
     *                    запись игры.
     *   temp           — nn::fs::MountTemporaryStorage: временное хранилище, запись.
     *   cal            — калибровочные данные консоли (CalibrationFile): читать их
     *                    подменёнными нельзя ни при каких условиях.
     *   host           — nn::fs::MountHost: файловая система ПК разработчика.
     *   sdmc, sd       — сама SD-карта (homebrew зовёт её "sdmc", чужие exlaunch-моды —
     *                    "sd"; так, например, UltraCam для TotK). Её файлы не данные
     *                    игры. Раньше "sd" было и НАШИМ именем; теперь наше —
     *                    "nxrsd" (NXR_SD_MOUNT выше), его стережёт self-guard.
     *   content        — ПЕРЕНЕСЕНО в отдельный список «только если это
     *                    romfs игры» (RomOnlyMounts ниже). Было: «осознанный
     *                    компромисс — игра, читающая данные как content:/, подмены
     *                    не получит». Опровергнуто модом UltraCam для TotK: он сам
     *                    монтирует romfs игры как MountRom("content") и читает оттуда
     *                    свои настройки.
     * Лишнего в списке нет: "app0", "rom", "asset" и прочие имена данных игры здесь
     * отсутствуют намеренно.
     *
     * Отдельно: любое имя, начинающееся с '@' ("@Sdcard", "@User", "@System"), —
     * внутреннее имя устройства nnSdk, тоже не данные игры.
     *
     * Этот список ЖЁСТКИЙ: имя из него не подменяется никогда, даже если под ним
     * кто-то смонтировал romfs (реестр RomMounts такое имя и не запоминает).
     */
    constexpr const char* BlockedMounts[] = {
        "save", "savedata", "bcat", "system", "user", "cache",
        "temp", "cal", "host", "sdmc", "sd",
    };
    constexpr size_t BlockedMountsCount = sizeof(BlockedMounts) / sizeof(BlockedMounts[0]);

    /*
     * ============ «ТОЛЬКО ЕСЛИ ЭТО ROMFS ИГРЫ» ============
     *
     * Имена, под которыми бывает и romfs игры, и ЧУЖОЕ хранилище. Сейчас одно —
     * "content": его занимает и nn::fs::MountContent (данные NCA, в том числе
     * ДРУГОГО приложения), и мод UltraCam для TotK, который зовёт
     * nn::fs::MountRom("content") и читает "content:/UltraCam/maxlastbreath.ini".
     *
     * Правило: такое имя подменяется ТОЛЬКО если модуль сам видел, что под этим
     * именем смонтирована romfs игры, — успешный nn::fs::MountRom(имя, …) прошёл
     * через наш перехватчик и имя записано в реестр RomMounts (ниже). Не видел —
     * не подменяем: «контент другого приложения» по-прежнему защищён.
     *
     * Почему не «подменять ТОЛЬКО узнанные имена» вообще (белый список). Пропуск
     * MountRom (romfs смонтирована до нашего перехватчика или иначе — например,
     * MountRomOnFile) означал бы МОЛЧА неработающий мод — ровно та ошибка, на
     * которой проект обжёгся дважды (фиксированный "rom:/" и жёсткий путь мода).
     * Для обычных имён настоящий фильтр — наличие файла на карте, а жёсткий список
     * выше защищает хранилища с записью; узнавание нужно только там, где имя
     * двусмысленно.
     *
     * КЛЮЧ СБОРКИ -DNXR_ALLOW_CONTENT_MOUNT — УСТАРЕЛ. Раньше он снимал
     * "content" из чёрного списка вручную (так собрана рабочая сборка TotK
     * `ИСПРАВЛЕННЫЙ-subsdk1`). Теперь то же делает узнавание через MountRom, и
     * конвейер ключ не задаёт. Оставлен как аварийный обход для случая, когда
     * MountRom прошёл мимо перехватчика (чужой модуль смонтировал romfs в своей
     * инициализации РАНЬШЕ нашей): с ключом "content" подменяется всегда.
     */
#ifdef NXR_ALLOW_CONTENT_MOUNT
    constexpr bool ContentMountForced = true;
#else
    constexpr bool ContentMountForced = false;
#endif
    constexpr const char* RomOnlyMounts[] = {
        "content",
    };
    constexpr size_t RomOnlyMountsCount = sizeof(RomOnlyMounts) / sizeof(RomOnlyMounts[0]);

    inline char LowerAscii(char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    /* Имя (len байт) совпадает с одним из списка без учёта регистра ASCII. */
    inline bool NameInList(const char* name, std::size_t len,
                           const char* const* list, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            const char* item = list[i];
            std::size_t k = 0;
            while (k < len && item[k] != '\0' && LowerAscii(name[k]) == item[k]) {
                ++k;
            }
            if (k == len && item[k] == '\0') {
                return true;
            }
        }
        return false;
    }

    /* true, если точку монтирования с таким именем (len байт) подменять нельзя НИКОГДА. */
    inline bool IsBlockedMount(const char* name, std::size_t len) {
        if (name == nullptr || len == 0) {
            return true;
        }
        if (name[0] == '@') {
            return true;
        }
        return NameInList(name, len, BlockedMounts, BlockedMountsCount);
    }

    /* true, если имя двусмысленное и подменяется только как узнанная romfs игры. */
    inline bool IsRomOnlyMount(const char* name, std::size_t len) {
        if (ContentMountForced || name == nullptr || len == 0) {
            return false;
        }
        return NameInList(name, len, RomOnlyMounts, RomOnlyMountsCount);
    }

    /*
     * ============ РЕЕСТР ТОЧЕК МОНТИРОВАНИЯ ROMFS ИГРЫ ============
     *
     * Сюда модуль записывает имя после КАЖДОГО успешного nn::fs::MountRom(имя, …)
     * (перехватчики в main.cpp) и вычёркивает его по nn::fs::Unmount(имя). Игра и
     * чужой мод могут смонтировать romfs под разными именами — запоминаются все.
     *
     * Потоки. MountRom/Unmount и OpenFile игра зовёт из разных потоков, поэтому
     * ячейка — слово версии и имя из атомарных байтов; замков нет (хук OpenFile
     * не должен ждать). В слове версии младшие 2 бита — состояние (0 пусто,
     * 1 пишется, 2 готово), старшие — поколение. Вычёркивание поднимает поколение,
     * поэтому читатель, сверявший имя «на лету», увидит, что слово сменилось, и
     * совпадение не засчитает (защита от ABA: вычеркнули и записали в ту же ячейку
     * другое имя, пока он сравнивал).
     *
     * Сравнение имени ТОЧНОЕ, с учётом регистра. Ошибиться здесь можно только в
     * одну сторону: если nn::fs различает регистр, "CONTENT" — уже другая точка, и
     * подменять её нельзя; если не различает — мы лишь не подменим файл, который
     * могли бы. Второе безопасно, первое — нет.
     *
     * Не запоминаются: пустое имя, длиннее 15 байт, с ':' или '/', на '@', имена
     * жёсткого списка и наше собственное (NXR_SD_MOUNT).
     */
    class RomMounts {
    public:
        static constexpr std::size_t Capacity = 8;

        /* Записать имя. false — имя негодное или мест нет (тогда и подмены нет). */
        bool Remember(const char* name) {
            const std::size_t len = ValidLength(name);
            if (len == 0) {
                return false;
            }
            if (Contains(name, len)) {
                return true;
            }
            for (std::size_t i = 0; i < Capacity; ++i) {
                Slot& s = m_Slots[i];
                std::uint32_t v = s.ver.load(std::memory_order_acquire);
                if ((v & StateMask) != StateEmpty) {
                    continue;
                }
                if (!s.ver.compare_exchange_strong(v, (v & ~StateMask) | StateWriting,
                                                   std::memory_order_acq_rel)) {
                    continue;
                }
                for (std::size_t k = 0; k <= MaxMountNameLen; ++k) {
                    s.name[k].store(k < len ? name[k] : '\0', std::memory_order_relaxed);
                }
                s.ver.store((v & ~StateMask) | StateReady, std::memory_order_release);
                return true;
            }
            return false;
        }

        /* Вычеркнуть имя (nn::fs::Unmount). Чего нет — того и нет. */
        void Forget(const char* name) {
            const std::size_t len = ValidLength(name);
            if (len == 0) {
                return;
            }
            for (std::size_t i = 0; i < Capacity; ++i) {
                Slot& s = m_Slots[i];
                std::uint32_t v = s.ver.load(std::memory_order_acquire);
                if ((v & StateMask) != StateReady || !SameName(s, name, len)) {
                    continue;
                }
                /* Следующее поколение, состояние «пусто». */
                s.ver.compare_exchange_strong(v, ((v & ~StateMask) + GenStep) | StateEmpty,
                                              std::memory_order_acq_rel);
            }
        }

        /* Есть ли в реестре имя (len байт, без ":/"). */
        bool Contains(const char* name, std::size_t len) const {
            if (name == nullptr || len == 0 || len > MaxMountNameLen) {
                return false;
            }
            for (std::size_t i = 0; i < Capacity; ++i) {
                const Slot& s = m_Slots[i];
                const std::uint32_t v1 = s.ver.load(std::memory_order_acquire);
                if ((v1 & StateMask) != StateReady) {
                    continue;
                }
                const bool same = SameName(s, name, len);
                std::atomic_thread_fence(std::memory_order_acquire);
                const std::uint32_t v2 = s.ver.load(std::memory_order_relaxed);
                if (same && v1 == v2) {
                    return true;
                }
            }
            return false;
        }

        /* Сколько имён сейчас записано (для отчёта диагностики). */
        std::size_t Count() const {
            std::size_t n = 0;
            for (std::size_t i = 0; i < Capacity; ++i) {
                if ((m_Slots[i].ver.load(std::memory_order_acquire) & StateMask) == StateReady) {
                    ++n;
                }
            }
            return n;
        }

        /* Копия index-й ЗАПОЛНЕННОЙ ячейки (для отчёта). false — такой нет. */
        bool CopyAt(std::size_t index, char* out, std::size_t outSize) const {
            if (out == nullptr || outSize < MaxMountNameLen + 1) {
                return false;
            }
            std::size_t seen = 0;
            for (std::size_t i = 0; i < Capacity; ++i) {
                const Slot& s = m_Slots[i];
                if ((s.ver.load(std::memory_order_acquire) & StateMask) != StateReady) {
                    continue;
                }
                if (seen++ != index) {
                    continue;
                }
                for (std::size_t k = 0; k <= MaxMountNameLen; ++k) {
                    out[k] = s.name[k].load(std::memory_order_relaxed);
                }
                out[MaxMountNameLen] = '\0';
                return true;
            }
            return false;
        }

        /*
         * Годна ли строка в имена реестра; возвращает её длину или 0.
         * Своё имя (NXR_SD_MOUNT) не запоминается: его стережёт self-guard.
         */
        static std::size_t ValidLength(const char* name) {
            if (name == nullptr || name[0] == '\0' || name[0] == '@') {
                return 0;
            }
            std::size_t len = 0;
            while (name[len] != '\0') {
                if (name[len] == ':' || name[len] == '/' || len >= MaxMountNameLen) {
                    return 0;
                }
                ++len;
            }
            if (IsBlockedMount(name, len)) {
                return 0;
            }
            if (len == SdMountNameLen) {
                bool own = true;
                for (std::size_t i = 0; i < len; ++i) {
                    own = own && name[i] == SdMountName[i];
                }
                if (own) {
                    return 0;
                }
            }
            return len;
        }

    private:
        static constexpr std::uint32_t StateMask    = 3;
        static constexpr std::uint32_t StateEmpty   = 0;
        static constexpr std::uint32_t StateWriting = 1;
        static constexpr std::uint32_t StateReady   = 2;
        static constexpr std::uint32_t GenStep      = 4;

        struct Slot {
            std::atomic<std::uint32_t> ver {0};
            std::atomic<char>          name[MaxMountNameLen + 1] {};
        };

        static bool SameName(const Slot& s, const char* name, std::size_t len) {
            for (std::size_t k = 0; k < len; ++k) {
                if (s.name[k].load(std::memory_order_relaxed) != name[k]) {
                    return false;
                }
            }
            return s.name[len].load(std::memory_order_relaxed) == '\0';
        }

        Slot m_Slots[Capacity] {};
    };

    /*
     * Собирает префикс пути мода по TitleID. Возвращает его длину или 0, если
     * TitleID нулевой (ядро не ответило) либо буфер мал.
     * TitleID берётся в рантайме через svcGetInfo — см. комментарий в main.cpp.
     */
    inline std::size_t BuildModPrefix(char* out, std::size_t outSize, std::uint64_t titleId) {
        if (out == nullptr || outSize < ModPrefixSize || titleId == 0) {
            return 0;
        }
        char* p = out;
        for (std::size_t i = 0; i < SdRootLen; ++i) {
            *p++ = SdRoot[i];
        }
        for (std::size_t i = 0; i < TitleIdChars; ++i) {
            const std::uint32_t nib =
                static_cast<std::uint32_t>((titleId >> (60 - i * 4)) & 0xF);
            *p++ = static_cast<char>(nib < 10 ? ('0' + nib) : ('A' + nib - 10));
        }
        for (std::size_t i = 0; i < SdLeafLen; ++i) {
            *p++ = SdLeaf[i];
        }
        *p = '\0';
        return static_cast<std::size_t>(p - out);
    }

    /*
     * Собирает "<префикс мода><остаток пути игры>" из пути, с которым игра зовёт
     * nn::fs::OpenFile.
     *
     * ПРЕФИКС ИГРЫ — ЛЮБОЙ вида "имя:/" (решение гибрида №3, исправление №6).
     * Движок открывает файлы через своё имя точки монтирования: у Trails the 2nd это
     * "app0:/" (диагностика на консоли: все 60 путей в отчёте были такими),
     * у других игр бывает "rom:/", "asset:/", "Contents:/". Пока проверялся только
     * "rom:/", подмена молча не срабатывала. Фиксированный список пришлось бы
     * дополнять под каждую игру, поэтому отрезаем ЛЮБОЙ префикс до ":/".
     *
     * Не перенаправляем:
     *   - путь без "имя:/" в начале (не файл из данных игры);
     *   - путь с НАШИМ именем точки монтирования ("nxrsd:/", раньше — "sd:/").
     *     Имя приходит параметром sdMountName (в модуле — nxr::SdMountName). Это self-guard, а НЕ защита
     *     от рекурсии: рекурсии здесь нет архитектурно — подменённый путь мы открываем
     *     вызовом ОРИГИНАЛА OpenFile (Orig), минуя свой хук. Self-guard нужен,
     *     чтобы не подменять пути к самой SD-карте: наш файл мода, отчёт диагностики
     *     и файлы Atmosphère должны читаться ровно оттуда, откуда их просят;
     *   - точку монтирования из чёрного списка (сейвы, кеш, host, cal — IsBlockedMount);
     *   - двусмысленное имя ("content"), если под ним не видели MountRom (NotRomMount);
     *   - имя точки монтирования длиннее 15 байт (в nn::fs таких не бывает);
     *   - пустой остаток и путь, не влезающий в буфер.
     *
     * Режим открытия здесь не виден: его проверяет вызывающий (IsRedirectableMode).
     *
     * Регистр и разделители не трогаем: на SD должна лежать ровно та же структура,
     * что внутри игры. Никаких аллокаций: пишем в переданный буфер (на консоли — на
     * стеке вызывающего потока).
     */
    /*
     * Почему подмены не было. Нужно диагностической сборке: в отчёте «подменено НЕТ»
     * иначе не отличить блокированную точку монтирования от неподходящего режима и от
     * просто отсутствующего файла на карте (без этого признака).
     */
    enum class SkipReason {
        None = 0,        /* подмена состоялась */
        NoModPrefix,     /* путь мода не собран (ядро не дало TitleID) */
        NoMountPrefix,   /* в пути нет "имя:/" — это не файл из данных игры */
        MountTooLong,    /* имя точки монтирования длиннее 15 байт */
        BlockedMount,    /* точка монтирования в чёрном списке */
        SelfPath,        /* наш собственный путь на SD (self-guard) */
        EmptyRest,       /* только префикс, подменять нечего */
        TooLong,         /* не влезает в буфер пути */
        WriteMode,       /* mode != Read — ставит сам вызывающий (хук) */
        NotOnSd,         /* путь подменён, но файла на карте нет — тоже ставит хук */
        NotRomMount,     /* имя двусмысленное ("content"), а MountRom под ним не видели */
    };

    inline const char* SkipReasonText(SkipReason reason) {
        switch (reason) {
            case SkipReason::None:          return "подменено";
            case SkipReason::NoModPrefix:   return "путь мода не собран (нет TitleID)";
            case SkipReason::NoMountPrefix: return "нет префикса \"имя:/\"";
            case SkipReason::MountTooLong:  return "имя точки монтирования длиннее 15 байт";
            case SkipReason::BlockedMount:  return "точка монтирования в чёрном списке";
            case SkipReason::SelfPath:      return "это путь к самой SD (self-guard)";
            case SkipReason::EmptyRest:     return "пустой остаток пути";
            case SkipReason::TooLong:       return "путь не влезает в буфер";
            case SkipReason::WriteMode:     return "mode != Read (открытие на запись)";
            case SkipReason::NotOnSd:       return "файла нет на SD";
            case SkipReason::NotRomMount:   return "под этим именем romfs игры не монтировалась (MountRom не видели)";
        }
        return "неизвестно";
    }

    /*
     * Тот же BuildSdPath, но с причиной отказа. Сама логика живёт здесь.
     * romMounts — реестр имён, под которыми модуль видел успешный nn::fs::MountRom
     * (перехват MountRom). nullptr — реестра нет: двусмысленные имена ("content") тогда
     * не подменяются, как раньше.
     */
    inline bool BuildSdPathWhy(char* out, std::size_t outSize,
                               const char* modPrefix, std::size_t modPrefixLen,
                               const char* gamePath, const char* sdMountName,
                               SkipReason* why, const RomMounts* romMounts = nullptr) {
        SkipReason localReason = SkipReason::None;
        SkipReason& reason = (why != nullptr) ? *why : localReason;
        reason = SkipReason::None;

        if (out == nullptr || gamePath == nullptr || modPrefix == nullptr ||
            sdMountName == nullptr || modPrefixLen == 0) {
            reason = SkipReason::NoModPrefix;
            return false;
        }

        /* Ищем ":/" в начале пути, не выходя за допустимую длину имени. */
        std::size_t n = 0;
        while (n <= MaxMountNameLen && gamePath[n] != '\0' &&
               gamePath[n] != ':' && gamePath[n] != '/') {
            ++n;
        }
        if (n == 0) {
            reason = SkipReason::NoMountPrefix;
            return false;
        }
        if (n > MaxMountNameLen) {
            reason = SkipReason::MountTooLong;
            return false;
        }
        if (gamePath[n] != ':' || gamePath[n + 1] != '/') {
            reason = SkipReason::NoMountPrefix;
            return false;
        }

        /*
         * Точки монтирования, которые не трогаем никогда (сейвы, кеш, host, cal, сама
         * SD). Проверяется ДО всего остального, что касается остатка пути.
         */
        if (IsBlockedMount(gamePath, n)) {
            reason = SkipReason::BlockedMount;
            return false;
        }

        /*
         * Двусмысленное имя ("content": и romfs игры, и nn::fs::MountContent чужого
         * NCA) подменяется, только если под ним на глазах модуля смонтировали romfs
         * (см. RomOnlyMounts и RomMounts выше).
         */
        if (IsRomOnlyMount(gamePath, n) &&
            (romMounts == nullptr || !romMounts->Contains(gamePath, n))) {
            reason = SkipReason::NotRomMount;
            return false;
        }

        /* Свой собственный префикс не перенаправляем — self-guard (см. шапку). */
        std::size_t sdLen = 0;
        while (sdMountName[sdLen] != '\0') {
            ++sdLen;
        }
        if (n == sdLen) {
            bool same = true;
            for (std::size_t i = 0; i < n; ++i) {
                if (gamePath[i] != sdMountName[i]) {
                    same = false;
                    break;
                }
            }
            if (same) {
                reason = SkipReason::SelfPath;
                return false;
            }
        }

        /* Остаток — сразу после "имя:/", без ведущего слэша. */
        const char* rest = gamePath + n + 2;

        /* Считаем длину остатка, попутно контролируя, что всё влезет. */
        std::size_t restLen = 0;
        while (rest[restLen] != '\0') {
            ++restLen;
            if (modPrefixLen + restLen + 1 > outSize) {
                reason = SkipReason::TooLong;
                return false;
            }
        }

        /* Пустой остаток (только префикс) перенаправлять бессмысленно. */
        if (restLen == 0) {
            reason = SkipReason::EmptyRest;
            return false;
        }

        for (std::size_t i = 0; i < modPrefixLen; ++i) {
            out[i] = modPrefix[i];
        }
        for (std::size_t i = 0; i <= restLen; ++i) {
            out[modPrefixLen + i] = rest[i];
        }
        return true;
    }

    /* Обёртка без причины — ею пользуется обычная (не диагностическая) сборка. */
    inline bool BuildSdPath(char* out, std::size_t outSize,
                            const char* modPrefix, std::size_t modPrefixLen,
                            const char* gamePath, const char* sdMountName,
                            const RomMounts* romMounts = nullptr) {
        return BuildSdPathWhy(out, outSize, modPrefix, modPrefixLen,
                              gamePath, sdMountName, nullptr, romMounts);
    }

}
