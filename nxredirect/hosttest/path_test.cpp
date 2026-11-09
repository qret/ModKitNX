/*
 * path_test — проверка на ПК той же логики путей, что исполняется в модуле.
 *
 * Copyright (c) 2026, участники проекта nxredirect
 * Лицензия GNU GPL версии 2 (как и весь модуль, основанный на exlaunch).
 *
 * Компилируется ТОТ ЖЕ заголовок, что включает модуль: ../src/nxr_path.hpp.
 * Проверяется то, на чём прежний проект ошибался:
 *   - префикс пути игры любой ("app0:/", "rom:/", "asset:/", "Contents:/", …),
 *     а не фиксированный список;
 *   - путь мода собирается из TitleID в рантайме, 16 hex ЗАГЛАВНЫМИ;
 *   - собственный префикс "nxrsd:/" не перенаправляется (self-guard: пути к самой SD);
 *   - имя нашей точки монтирования СВОЁ, не общее "sd" (с "sd" чужой
 *     мод UltraCam занимал имя первым, MountSdCard давал 0x7802, подмена выключалась);
 *   - точки монтирования из чёрного списка (сейвы, кеш, cal, host, sdmc, sd)
 *     не перенаправляются НИКОГДА, в любом регистре;
 *   - двусмысленное "content" подменяется ТОЛЬКО после того, как модуль увидел
 *     под этим именем nn::fs::MountRom (реестр RomMounts; сценарий
 *     UltraCam для TotK), и перестаёт после Unmount; реестр выдерживает гонку
 *     записи/вычёркивания с чтением из другого потока;
 *   - режим открытия: подменяется только чтение (mode == OpenMode_Read);
 *   - пределы: длина имени точки монтирования, пустой остаток, переполнение буфера.
 *
 * Код возврата 0 — все пробы как надо, 1 — что-то не так.
 */

#include "nxr_path.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {

int g_Fail = 0;
int g_Ran = 0;

/* Сколько проверок обязано быть выполнено, чтобы зелёный итог что-то значил.
   «Затвор нуля»: тест, не выполнивший ни одной проверки, раньше печатал
   «ВСЁ ЗЕЛЁНОЕ» и возвращал 0 — считался только счётчик провалов. Порог записан
   ЯВНЫМ числом, а не выводится из самого прогона: иначе вырезанный блок проверок
   опустил бы заодно и порог. */
const int kMinChecks = 90;  /* было 40 (при 76 пробах); +21 проба реестра romfs */

void Check(bool cond, const char* what) {
    ++g_Ran;
    std::printf("   %s %s\n", cond ? "OK  " : "ПЛОХО", what);
    if (!cond) ++g_Fail;
}

/* Прогон подмены с готовым префиксом мода. */
bool Build(char* out, std::size_t outSize, const char* prefix, const char* gamePath) {
    return nxr::BuildSdPath(out, outSize, prefix, std::strlen(prefix), gamePath,
                            nxr::SdMountName);
}

void CheckMapped(const char* prefix, const char* gamePath, const char* want) {
    char buf[0x301] = {};
    const bool ok = Build(buf, sizeof(buf), prefix, gamePath);
    char msg[0x400];
    std::snprintf(msg, sizeof(msg), "%s -> %s", gamePath, ok ? buf : "(не подменено)");
    Check(ok && std::strcmp(buf, want) == 0, msg);
}

void CheckNotMapped(const char* prefix, const char* gamePath, const char* why) {
    char buf[0x301] = {};
    const bool ok = Build(buf, sizeof(buf), prefix, gamePath);
    char msg[0x400];
    std::snprintf(msg, sizeof(msg), "НЕГАТИВ: %s не подменяется (%s)%s",
                  gamePath, why, ok ? " — А ПОДМЕНИЛСЯ!" : "");
    Check(!ok, msg);
}

}  // namespace

int main() {
    std::printf("[хост-тест путей] префикс мода из TitleID\n");

    char prefix[nxr::ModPrefixSize] = {};

    /* TitleID Trails in the Sky the 2nd — на нём всё и проверялось на консоли. */
    const std::size_t len = nxr::BuildModPrefix(prefix, sizeof(prefix), 0x01007A2027548000ull);
    {
        char msg[0x200];
        std::snprintf(msg, sizeof(msg), "путь мода = %s", prefix);
        Check(len == std::strlen(prefix) &&
              std::strcmp(prefix, "nxrsd:/atmosphere/contents/01007A2027548000/files/") == 0, msg);
    }

    /* Старшие нули и буквы: hex обязан быть ЗАГЛАВНЫМ и ровно из 16 цифр. */
    {
        char p2[nxr::ModPrefixSize] = {};
        nxr::BuildModPrefix(p2, sizeof(p2), 0x0100000000abcdefull);
        Check(std::strcmp(p2, "nxrsd:/atmosphere/contents/0100000000ABCDEF/files/") == 0,
              "TitleID с буквами и нулями: 16 hex-цифр заглавными");
    }

    /* Ядро не ответило (TitleID 0) — префикса нет, подменять нечего. */
    {
        char p3[nxr::ModPrefixSize] = {};
        Check(nxr::BuildModPrefix(p3, sizeof(p3), 0) == 0,
              "НЕГАТИВ: TitleID 0 — префикс не собирается");
        char p4[8] = {};
        Check(nxr::BuildModPrefix(p4, sizeof(p4), 0x0100000000010000ull) == 0,
              "НЕГАТИВ: маленький буфер — префикс не собирается");
    }

    std::printf("\n[хост-тест путей] подмена пути: префикс игры ЛЮБОЙ\n");

    /* Так открывает файлы Trails the 2nd (проверено диагностикой на консоли). */
    CheckMapped(prefix, "app0:/asset/battle/bt0001.dat",
                "nxrsd:/atmosphere/contents/01007A2027548000/files/asset/battle/bt0001.dat");
    /* Обычное имя romfs в nnSdk. */
    CheckMapped(prefix, "rom:/script_en/t_book.tbl",
                "nxrsd:/atmosphere/contents/01007A2027548000/files/script_en/t_book.tbl");
    /* Имена, встречающиеся у других игр. */
    CheckMapped(prefix, "asset:/data/x.bin",
                "nxrsd:/atmosphere/contents/01007A2027548000/files/data/x.bin");
    CheckMapped(prefix, "Contents:/a/b/c.dat",
                "nxrsd:/atmosphere/contents/01007A2027548000/files/a/b/c.dat");
    /* Совсем незнакомое имя — тоже работает: список префиксов не нужен. */
    CheckMapped(prefix, "Data9:/f.txt",
                "nxrsd:/atmosphere/contents/01007A2027548000/files/f.txt");
    /* Регистр и разделители не меняются: на SD та же структура, что в игре. */
    CheckMapped(prefix, "app0:/Asset/MiXeD Case/Файл.dat",
                "nxrsd:/atmosphere/contents/01007A2027548000/files/Asset/MiXeD Case/Файл.dat");

    std::printf("\n[хост-тест путей] что подменять НЕЛЬЗЯ\n");

    /* Главное: свой же префикс. Иначе чтение с SD ушло бы по кругу. */
    CheckNotMapped(prefix, "nxrsd:/atmosphere/contents/01007A2027548000/files/a.dat",
                   "это наш собственный путь на SD");
    CheckNotMapped(prefix, "sd:/atmosphere/contents/01007A2027548000/files/a.dat",
                   "чужая точка \"sd\" (её монтирует, например, UltraCam) — в чёрном списке");
    CheckNotMapped(prefix, "sd:/nxredirect-diag.txt", "префикс sd: — чужая точка, чёрный список");
    CheckNotMapped(prefix, "nxrsd:/nxredirect-diag.txt", "префикс nxrsd: — наш (прежний отчёт модуля)");
    /* Отчёты — в папке игры под switch/ModKitNX/logs (nxr_logs.hpp). */
    CheckNotMapped(prefix, "nxrsd:/switch/ModKitNX/logs/01007A2027548000/diag-0001.txt",
                   "префикс nxrsd: — наш (отчёт модуля в папке логов)");
    /*
     * Self-guard проверяется НЕЗАВИСИМО от чёрного списка: имя "sd" в нём и так есть,
     * поэтому берём имя монтирования, которого в списке нет ("nxrsd"). Путь с ИМЕНЕМ,
     * под которым модуль сам смонтировал SD, подменяться не должен никогда.
     */
    {
        char buf[0x301] = {};
        const bool ok = nxr::BuildSdPath(buf, sizeof(buf), prefix, std::strlen(prefix),
                                         "nxrsd:/atmosphere/contents/x/files/a.dat", "nxrsd");
        Check(!ok, "НЕГАТИВ: свой префикс монтирования SD (любое имя) не подменяется");
        /* А чужое имя при том же sdMountName подменяться обязано. */
        char buf2[0x301] = {};
        const bool ok2 = nxr::BuildSdPath(buf2, sizeof(buf2), prefix, std::strlen(prefix),
                                          "app0:/a.dat", "nxrsd");
        Check(ok2, "контроль: при другом имени монтирования SD подмена работает");
    }

    /* Похожие, но другие имена подменяться должны (проверка, что сравнение точное). */
    CheckMapped(prefix, "sdx:/a.dat",
                "nxrsd:/atmosphere/contents/01007A2027548000/files/a.dat");
    CheckMapped(prefix, "s:/a.dat",
                "nxrsd:/atmosphere/contents/01007A2027548000/files/a.dat");

    CheckNotMapped(prefix, "/rom/no_prefix.dat", "нет префикса \"имя:/\"");
    CheckNotMapped(prefix, "plain_name.dat", "нет префикса вовсе");
    CheckNotMapped(prefix, "rom:no_slash.dat", "после двоеточия нет слэша");
    CheckNotMapped(prefix, ":/empty_name.dat", "пустое имя точки монтирования");
    CheckNotMapped(prefix, "app0:/", "пустой остаток пути");
    CheckNotMapped(prefix, "", "пустой путь");
    CheckNotMapped(prefix, "оченьдлинноеимяточкимонтирования:/a.dat",
                   "имя точки монтирования длиннее 15 байт");

    /* Путь, не влезающий в буфер: подмены нет, но и порчи памяти нет. */
    {
        std::string longPath = "app0:/";
        longPath.append(0x400, 'x');
        char small[0x40] = {};
        Check(!Build(small, sizeof(small), prefix, longPath.c_str()),
              "НЕГАТИВ: путь не влезает в буфер — подмены нет");
        Check(small[sizeof(small) - 1] == '\0', "буфер не испорчен за пределами");
    }

    /* Префикс не собран (TitleID не получен) — подмены быть не должно. */
    {
        char buf[0x301] = {};
        Check(!nxr::BuildSdPath(buf, sizeof(buf), prefix, 0, "app0:/a.dat", "sd"),
              "НЕГАТИВ: пустой префикс мода — подмены нет");
    }

    std::printf("\n[хост-тест путей] чёрный список точек монтирования\n");

    /*
     * Каждый пункт чёрного списка — своя проба: подмена сейва или кеша означала бы
     * потерю записи игрока, подмена cal/host/sdmc — чтение чужих данных.
     */
    {
        const char* blocked[] = {
            "save:/data.bin", "saveData:/data.bin", "bcat:/d.bin", "system:/s.bin",
            "user:/u.bin", "cache:/c.bin", "temp:/t.bin", "cal:/calib.bin",
            "host:/C/x.dat", "sdmc:/atmosphere/x", "sd:/x", "content:/a.nca",
            /* Регистр значения не имеет: nnSdk имя не нормализует. */
            "SAVE:/data.bin", "SaveData:/d.bin", "Cal:/c.bin",
            /* Внутренние имена устройств nnSdk. */
            "@Sdcard:/x", "@User:/x", "@System:/x",
        };
        for (const char* p : blocked) {
            CheckNotMapped(prefix, p, "точка монтирования в чёрном списке");
        }
        /* Похожие имена данных игры блокироваться НЕ должны. */
        CheckMapped(prefix, "savegamedata:/a.dat",
                    "nxrsd:/atmosphere/contents/01007A2027548000/files/a.dat");
        CheckMapped(prefix, "contents:/a.dat",
                    "nxrsd:/atmosphere/contents/01007A2027548000/files/a.dat");
        CheckMapped(prefix, "host0:/a.dat",
                    "nxrsd:/atmosphere/contents/01007A2027548000/files/a.dat");
        Check(nxr::IsBlockedMount("save", 4) && !nxr::IsBlockedMount("saved", 5),
              "IsBlockedMount сравнивает имя целиком, а не по началу");
    }

    std::printf("\n[хост-тест путей] причина отказа (её печатает диагностический отчёт)\n");

    /*
     * Диагностике нужно отличать блэклист от отсутствия префикса и от режима записи,
     * иначе «подменено НЕТ» в отчёте ничего не объясняет. Проверяем каждую причину.
     */
    {
        struct Case { const char* path; nxr::SkipReason want; const char* what; };
        const Case cases[] = {
            {"app0:/a.dat",      nxr::SkipReason::None,          "подмена — причины нет"},
            {"save:/s.bin",      nxr::SkipReason::BlockedMount,  "блэклист"},
            {"sd:/x",            nxr::SkipReason::BlockedMount,  "sd тоже в блэклисте"},
            {"plain.dat",        nxr::SkipReason::NoMountPrefix, "нет префикса"},
            {"rom:no_slash",     nxr::SkipReason::NoMountPrefix, "после двоеточия нет слэша"},
            {"app0:/",           nxr::SkipReason::EmptyRest,     "пустой остаток"},
            {"оченьдлинноеимяточкимонтирования:/a", nxr::SkipReason::MountTooLong,
             "слишком длинное имя точки монтирования"},
        };
        for (const Case& c : cases) {
            char buf[0x301] = {};
            nxr::SkipReason why = nxr::SkipReason::NotOnSd;
            nxr::BuildSdPathWhy(buf, sizeof(buf), prefix, std::strlen(prefix),
                                c.path, "sd", &why);
            char msg[0x300];
            std::snprintf(msg, sizeof(msg), "причина «%s»: %s",
                          nxr::SkipReasonText(why), c.what);
            Check(why == c.want, msg);
        }
        /* Self-guard: имя монтирования не из блэклиста — причина именно SelfPath. */
        char buf[0x301] = {};
        nxr::SkipReason why = nxr::SkipReason::None;
        nxr::BuildSdPathWhy(buf, sizeof(buf), prefix, std::strlen(prefix),
                            "nxrsd:/a.dat", "nxrsd", &why);
        Check(why == nxr::SkipReason::SelfPath, "причина self-guard отличается от блэклиста");
        /* Переполнение буфера. */
        std::string longPath = "app0:/";
        longPath.append(0x400, 'x');
        char small[0x40] = {};
        why = nxr::SkipReason::None;
        nxr::BuildSdPathWhy(small, sizeof(small), prefix, std::strlen(prefix),
                            longPath.c_str(), "sd", &why);
        Check(why == nxr::SkipReason::TooLong, "причина «путь не влезает в буфер»");
        /* Префикс мода не собран. */
        why = nxr::SkipReason::None;
        nxr::BuildSdPathWhy(buf, sizeof(buf), prefix, 0, "app0:/a.dat", "sd", &why);
        Check(why == nxr::SkipReason::NoModPrefix, "причина «путь мода не собран»");
    }

    std::printf("\n[хост-тест путей] имя нашей точки монтирования SD (04.10.2026)\n");

    /*
     * Баг 1 из разбора краха TotK: модуль монтировал SD под общим именем "sd",
     * чужой мод (UltraCam) монтировал его раньше, nn::fs отдавал 0x7802
     * «имя занято», и подмена выключалась на всю сессию. Имя обязано быть
     * своим, и все пути модуля обязаны начинаться именно с него.
     * Негативная проба: поломка shared-mount-name в run_hosttest.sh
     * возвращает "sd" — этот блок обязан покраснеть.
     */
    {
        const char* shared[] = {"sd", "sdmc", "sdcard", "SD", "Sd"};
        bool unique = true;
        for (const char* s : shared) {
            unique = unique && std::strcmp(nxr::SdMountName, s) != 0;
        }
        Check(unique, "имя точки монтирования не общее (не sd/sdmc/sdcard)");
        Check(nxr::SdMountNameLen >= 1 && nxr::SdMountNameLen <= nxr::MaxMountNameLen &&
              nxr::SdMountName[0] != '@',
              "имя в пределах nn::fs: 1..15 байт, не начинается с @");
        const std::size_t n = nxr::SdMountNameLen;
        Check(std::strncmp(nxr::SdRoot, nxr::SdMountName, n) == 0 &&
              nxr::SdRoot[n] == ':' && nxr::SdRoot[n + 1] == '/',
              "корень пути мода начинается с нашего имени и \":/\"");
        Check(std::strncmp(prefix, nxr::SdMountName, n) == 0 && prefix[n] == ':',
              "собранный путь мода начинается с нашего имени");
        Check(!nxr::IsBlockedMount(nxr::SdMountName, n),
              "наше имя не в чёрном списке (его стережёт self-guard, причина SelfPath)");
        char buf[0x301] = {};
        nxr::SkipReason why = nxr::SkipReason::None;
        std::string own = std::string(nxr::SdMountName) + ":/atmosphere/x.dat";
        nxr::BuildSdPathWhy(buf, sizeof(buf), prefix, std::strlen(prefix), own.c_str(),
                            nxr::SdMountName, &why);
        Check(why == nxr::SkipReason::SelfPath, "путь с нашим именем — self-guard (SelfPath)");
        why = nxr::SkipReason::None;
        nxr::BuildSdPathWhy(buf, sizeof(buf), prefix, std::strlen(prefix), "sd:/UltraCam/x.ini",
                            nxr::SdMountName, &why);
        Check(why == nxr::SkipReason::BlockedMount,
              "путь чужой точки \"sd\" не подменяется (чёрный список)");
    }

    std::printf("\n[хост-тест путей] точка romfs игры узнаётся по MountRom (05.10.2026)\n");

    /*
     * Сценарий TotK + UltraCam (по разбору кода TotK): мод зовёт
     * nn::fs::MountRom("content") и читает "content:/UltraCam/maxlastbreath.ini",
     * при неудаче — "content:/ultracam/maxlastbreath.ini". Имя "content" занимает
     * и nn::fs::MountContent (чужой NCA), поэтому без узнавания оно не подменяется.
     * Перехватчик MountRom в модуле зовёт RomMounts::Remember после успешного
     * оригинала, перехватчик Unmount — Forget. Здесь — та же логика без консоли.
     */
    {
        char totk[nxr::ModPrefixSize] = {};
        nxr::BuildModPrefix(totk, sizeof(totk), 0x0100F2C0115B6000ull);
        const char* ini = "content:/UltraCam/maxlastbreath.ini";
        const char* want = "nxrsd:/atmosphere/contents/0100F2C0115B6000/files/UltraCam/maxlastbreath.ini";
        nxr::RomMounts rom {};
        char buf[0x301] = {};
        nxr::SkipReason why = nxr::SkipReason::None;

        /* 1. MountRom ещё не видели: content — возможно чужой NCA, подмены нет. */
        bool ok = nxr::BuildSdPathWhy(buf, sizeof(buf), totk, std::strlen(totk), ini,
                                      nxr::SdMountName, &why, &rom);
        Check(!ok && why == nxr::SkipReason::NotRomMount,
              "НЕГАТИВ: content:/ без MountRom не подменяется (причина NotRomMount)");
        ok = nxr::BuildSdPathWhy(buf, sizeof(buf), totk, std::strlen(totk), ini,
                                 nxr::SdMountName, &why, nullptr);
        Check(!ok && why == nxr::SkipReason::NotRomMount,
              "НЕГАТИВ: без реестра (nullptr) content:/ не подменяется — как до 05.10.2026");

        /* 2. MountRom("content") прошёл — путь уходит на карту. */
        Check(rom.Remember("content"), "MountRom(\"content\"): имя записано в реестр");
        ok = nxr::BuildSdPathWhy(buf, sizeof(buf), totk, std::strlen(totk), ini,
                                 nxr::SdMountName, &why, &rom);
        {
            char msg[0x700];
            std::snprintf(msg, sizeof(msg), "%s -> %s", ini, ok ? buf : "(не подменено)");
            Check(ok && why == nxr::SkipReason::None && std::strcmp(buf, want) == 0, msg);
        }
        ok = nxr::BuildSdPath(buf, sizeof(buf), totk, std::strlen(totk),
                              "content:/ultracam/maxlastbreath.ini", nxr::SdMountName, &rom);
        Check(ok && std::strcmp(buf, "nxrsd:/atmosphere/contents/0100F2C0115B6000/files/"
                                     "ultracam/maxlastbreath.ini") == 0,
              "второй путь UltraCam (нижний регистр) тоже уходит на карту");

        /* 3. Узнанное имя не открывает чужое: регистр, жёсткий список, sd. */
        ok = nxr::BuildSdPathWhy(buf, sizeof(buf), totk, std::strlen(totk),
                                 "CONTENT:/UltraCam/maxlastbreath.ini", nxr::SdMountName, &why, &rom);
        Check(!ok && why == nxr::SkipReason::NotRomMount,
              "НЕГАТИВ: CONTENT:/ (другой регистр) — не то имя, что видели в MountRom");
        Check(!nxr::BuildSdPath(buf, sizeof(buf), totk, std::strlen(totk),
                                "sd:/UltraCam/maxlastbreath.ini", nxr::SdMountName, &rom),
              "НЕГАТИВ: sd:/ (точка SD, её монтирует UltraCam) по-прежнему не подменяется");
        Check(!nxr::BuildSdPath(buf, sizeof(buf), totk, std::strlen(totk),
                                "save:/progress.sav", nxr::SdMountName, &rom),
              "НЕГАТИВ: save:/ не подменяется и при заполненном реестре");

        /* 4. Системные имена и своё в реестр не попадают НИКОГДА. */
        const char* never[] = {"save", "SAVE", "saveData", "bcat", "cache", "temp", "cal",
                               "host", "sdmc", "sd", "system", "user", "@Sdcard",
                               nxr::SdMountName, "", "a:b", "a/b",
                               "sixteen-bytes-xx"};
        bool none = true;
        for (const char* n : never) {
            none = none && !rom.Remember(n);
        }
        Check(none, "НЕГАТИВ: системные точки, @-имена, своё nxrsd, пустое, с ':'/'/', "
                    "длиннее 15 байт — в реестр не записываются");
        rom.Remember("save");
        Check(!nxr::BuildSdPath(buf, sizeof(buf), totk, std::strlen(totk), "save:/a.sav",
                                nxr::SdMountName, &rom),
              "НЕГАТИВ: «MountRom(\"save\")» не делает сейв подменяемым");

        /* 5. Несколько MountRom с разными именами — подменяются все. */
        Check(rom.Remember("rom2") && rom.Remember("content"),
              "второе имя записано, повтор первого — не дубль");
        Check(rom.Count() == 2, "в реестре ровно два имени (повтор не размножается)");
        Check(nxr::BuildSdPath(buf, sizeof(buf), totk, std::strlen(totk), "rom2:/a.bin",
                               nxr::SdMountName, &rom) &&
              nxr::BuildSdPath(buf, sizeof(buf), totk, std::strlen(totk), ini,
                               nxr::SdMountName, &rom),
              "обе узнанные точки подменяются");

        /* 6. Unmount вычёркивает: дальше под именем может быть MountContent. */
        rom.Forget("content");
        ok = nxr::BuildSdPathWhy(buf, sizeof(buf), totk, std::strlen(totk), ini,
                                 nxr::SdMountName, &why, &rom);
        Check(!ok && why == nxr::SkipReason::NotRomMount && rom.Count() == 1,
              "НЕГАТИВ: после Unmount(\"content\") подмены снова нет");
        Check(rom.Remember("content") && rom.Count() == 2 &&
              nxr::BuildSdPath(buf, sizeof(buf), totk, std::strlen(totk), ini,
                               nxr::SdMountName, &rom),
              "повторный MountRom после Unmount снова включает подмену");

        /* 7. Переполнение реестра: девятое имя не записывается, прежние целы. */
        nxr::RomMounts full {};
        int stored = 0;
        for (int i = 0; i < 9; ++i) {
            char name[8];
            std::snprintf(name, sizeof(name), "rom%d", i);
            stored += full.Remember(name) ? 1 : 0;
        }
        Check(stored == static_cast<int>(nxr::RomMounts::Capacity) && full.Contains("rom0", 4) &&
              !full.Contains("rom8", 4),
              "реестр на 8 имён: девятое отклонено, первые на месте");
        char copy[nxr::MaxMountNameLen + 1] = {};
        Check(full.CopyAt(0, copy, sizeof(copy)) && std::strcmp(copy, "rom0") == 0 &&
              !full.CopyAt(8, copy, sizeof(copy)),
              "CopyAt отдаёт заполненные ячейки и только их (отчёт диагностики)");

        /* 8. Обычные имена не зависят от реестра — русификатор Trails (app0:/). */
        nxr::RomMounts empty {};
        Check(nxr::BuildSdPath(buf, sizeof(buf), prefix, std::strlen(prefix),
                               "app0:/asset/battle/bt0001.dat", nxr::SdMountName, &empty) &&
              std::strcmp(buf, "nxrsd:/atmosphere/contents/01007A2027548000/files/"
                               "asset/battle/bt0001.dat") == 0,
              "app0:/ (Trails) подменяется и при пустом реестре — как прежде");
        Check(nxr::BuildSdPath(buf, sizeof(buf), prefix, std::strlen(prefix),
                               "rom:/script_en/t_book.tbl", nxr::SdMountName, &rom),
              "rom:/ подменяется и при непустом реестре (реестр только расширяет)");
    }

    std::printf("\n[хост-тест путей] реестр romfs из нескольких потоков\n");

    /*
     * MountRom/Unmount и OpenFile на консоли идут из разных потоков. Один поток
     * без остановки записывает и вычёркивает "content", второй всё это время
     * спрашивает: постоянное имя обязано находиться ВСЕГДА, ни разу не
     * записанное — НИКОГДА. Ловит порчу ячеек и «полузаписанное» имя.
     */
    {
        nxr::RomMounts rom {};
        rom.Remember("app0");
        std::atomic<bool> stop {false};
        std::thread churn([&] {
            for (int i = 0; i < 200000; ++i) {
                rom.Remember("content");
                rom.Remember("rom9");
                rom.Forget("content");
                rom.Forget("rom9");
            }
            stop.store(true);
        });
        long bad = 0, asked = 0;
        while (!stop.load()) {
            bad += rom.Contains("app0", 4) ? 0 : 1;
            bad += rom.Contains("never", 5) ? 1 : 0;
            bad += rom.Contains("conten", 6) ? 1 : 0;
            ++asked;
        }
        churn.join();
        char msg[0x200];
        std::snprintf(msg, sizeof(msg),
                      "%ld опросов во время записи/вычёркивания: ошибок %ld", asked, bad);
        Check(bad == 0 && asked > 0, msg);
        Check(rom.Count() == 1 && rom.Contains("app0", 4),
              "после гонки в реестре осталось только постоянное имя");
    }

    std::printf("\n[хост-тест путей] режим открытия: подменяется только чтение\n");

    {
        Check(nxr::IsRedirectableMode(1), "mode = Read (1) — подмена разрешена");
        Check(!nxr::IsRedirectableMode(2), "НЕГАТИВ: mode = Write (2) — подмены нет");
        Check(!nxr::IsRedirectableMode(3), "НЕГАТИВ: mode = Read|Write (3) — подмены нет");
        Check(!nxr::IsRedirectableMode(4), "НЕГАТИВ: mode = Append (4) — подмены нет");
        Check(!nxr::IsRedirectableMode(5), "НЕГАТИВ: mode = Read|Append (5) — подмены нет");
        Check(!nxr::IsRedirectableMode(7), "НЕГАТИВ: mode = Read|Write|Append (7) — подмены нет");
        Check(!nxr::IsRedirectableMode(0), "НЕГАТИВ: mode = 0 — подмены нет");
        Check(!nxr::IsRedirectableMode(0x11),
              "НЕГАТИВ: mode с неизвестным старшим битом (0x11) — подмены нет");
        Check(!nxr::IsRedirectableMode(-1), "НЕГАТИВ: mode = -1 — подмены нет");
    }

    std::printf("\n");
    if (g_Ran < kMinChecks) {
        std::printf("[хост-тест путей] НИЧЕГО НЕ ПРОВЕРЕНО: выполнено проб %d, "
                    "а их обязано быть не меньше %d — это КРАСНЫЙ, а не зелёный\n",
                    g_Ran, kMinChecks);
        return 2;
    }
    if (g_Fail == 0) {
        std::printf("[хост-тест путей] ВСЁ ЗЕЛЁНОЕ (проб выполнено: %d)\n", g_Ran);
        return 0;
    }
    std::printf("[хост-тест путей] ПРОБ ПРОВАЛЕНО: %d\n", g_Fail);
    return 1;
}
