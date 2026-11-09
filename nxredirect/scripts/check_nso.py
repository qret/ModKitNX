#!/usr/bin/env python3
"""
check_nso.py — проверка собранного модуля nxredirect НА ПК, до консоли.

Что проверяется:
  1. Файл — валидный NSO: магия NSO0, три сегмента (text/rodata/data), MOD0 в образе.
  2. У модуля НЕТ импортов: он инициализируется раньше загрузки `sdk`, поэтому любой
     неразрешённый импорт = обращение по нулевому адресу и падение игры.
  3. Имена символов, которые модуль ищет во время работы (nnMain, nn::fs::OpenFile,
     nn::fs::MountSdCard), лежат строками в образе И экспортируются модулями игры.
  4. Слово по +8 (module-runtime-info) указывает внутрь образа — исправление №1,
     без него rtld игры роняет процесс.
  5. Путь мода собирается В РАНТАЙМЕ: в образе есть куски "sd:/atmosphere/contents/"
     и "/files/", НЕТ плейсхолдера из 16 нулей (подход romfsex, который не работает:
     сегменты NSO сжаты LZ4, заменять в готовом файле нечего) и есть системный вызов
     svc 0x29 (GetInfo) — им берётся TitleID.
  6. DT_NEEDED пуст либо ссылается только на модули игры.
  7. Размер в разумных пределах.
  8. С --diag (модуль собран с -DNXR_DIAG): дополнительно требуются символы записи
     на SD и папок (отчёты по играм с ротацией) и шаблон пути отчёта
     nxrsd:/switch/ModKitNX/logs/{TID}/diag- (src/nxr_logs.hpp). Без --diag шаблон,
     наоборот, запрещён — так обычная и диагностическая сборки не путаются.
     Прежних путей в корне карты (nxredirect-diag.txt, nxredirect-gfx.txt) не
     должно быть ни в одной сборке: их наличие значит, что собран старый исходник.
 11. С --compat-nvrm (модуль собран с -DNXR_COMPAT_NVRM_QUERY): имена
     NvRm_MemmgrGetIoctlFile и NvOsDrvIoctl в образе и в экспортах игры; без ключа
     они запрещены.
 12. В образе нет общей точки монтирования "sd" (строк "sd:/…" и "sd"): своё имя
     nxrsd (конфликт 0x7802 с UltraCam).
  9. С --diag-gfx (модуль собран с -DNXR_DIAG_GFX): дополнительно требуются шаблон
     пути отчёта nxrsd:/switch/ModKitNX/logs/{TID}/gfx- и имена перехватываемых функций графики
     (nn::vi, egl*, nvnBootstrapLoader*). Без --diag-gfx и строка, и эти имена,
     наоборот, ЗАПРЕЩЕНЫ: рабочая сборка не должна нести ни байта диагностики.

Папка exefs игры ОБЯЗАТЕЛЬНА: без неё проверка 3 ничего не проверяет. Модули берут
из своего дампа игры; если папки нет, гейт краснеет, а не пропускается.

Использование:
  python check_nso.py <путь к subsdkN> --game-exefs <папка с rtld/main/subsdk*/sdk>
  python check_nso.py <путь к subsdkN> --game-exefs <...> --diag
  python check_nso.py <путь к subsdkN> --game-exefs <...> --diag-gfx
"""

import argparse
import struct
import sys
from pathlib import Path


def setup_stdio():
    """
    stdout/stderr — UTF-8, даже когда это КАНАЛ, а не консоль.

    Замер: этот гейт печатает по-русски, и при выводе в канал (а его
    зовут именно так — из build.py, install.py и из веб-оболочки) без
    PYTHONIOENCODING он падал на UnicodeEncodeError ещё до первой своей строки:
    код возврата 1 на ЗАВЕДОМО ГОДНОМ модуле. Это ложная находка гейта.
    С PYTHONIOENCODING=utf-8 тот же прогон давал 0.
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

# ---------------------------------------------------------------- NSO-разбор
# (логика распаковки заимствована из scratchpad\tools\nsosym.py — тот же способ)


def lz4_decompress(src: bytes) -> bytes:
    """Минимальный распаковщик LZ4 block — NSO сжимает сегменты именно им."""
    dst = bytearray()
    i = 0
    while i < len(src):
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        dst += src[i:i + lit]
        i += lit
        if i >= len(src):
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        match = token & 15
        if match == 15:
            while True:
                b = src[i]
                i += 1
                match += b
                if b != 255:
                    break
        match += 4
        start = len(dst) - off
        for k in range(match):
            dst.append(dst[start + k])
    return bytes(dst)


ZBIC_FLAG = 0x80        # бит 7 флагов NSO0: сегменты сжаты не LZ4, а zstd-ZBIC


def zbic_decompress(src: bytes) -> bytes:
    """
    Распаковка сегмента, сжатого ZBIC (HOS 22.0.0+).

    Это zstd с магией кадра «ZBIC» и FSE-таблицами в binary interpolative
    coding: стандартный zstd такой поток не берёт. Путей два: pip-пакет `zbic`
    (kinnay, GPL-2.0) — быстрый, если он есть, и наш scripts/zbic_pure.py —
    чистый Python без зависимостей, он лежит рядом и работает всегда. Импорт
    отложенный: гейту без ZBIC распаковщик не нужен вовсе.
    """
    try:
        import zbic
        return zbic.decompress(src)
    except ImportError:
        pass
    try:
        import importlib.util
        p = Path(__file__).resolve().with_name("zbic_pure.py")
        if not p.is_file():
            raise ImportError(str(p))
        spec = importlib.util.spec_from_file_location("nxr_zbic_pure", p)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod.decompress(src)
    except ImportError:
        raise ValueError("сегмент сжат ZBIC (бит 7 флагов NSO, HOS 22.0.0+), "
                         "а распаковать его нечем: рядом нет zbic_pure.py и не "
                         "установлен пакет `zbic`. Верните zbic_pure.py в "
                         "scripts/ (он идёт в том же архиве).")


class Nso:
    def __init__(self, path: Path):
        self.path = path
        self.raw = path.read_bytes()
        self.problems = []
        if self.raw[:4] != b"NSO0":
            raise ValueError(f"{path}: нет магии NSO0")
        self.flags = struct.unpack_from("<I", self.raw, 0xC)[0]
        self.zbic = bool(self.flags & ZBIC_FLAG)
        self.segments = []
        image = bytearray()
        for n in range(3):
            foff, moff, size = struct.unpack_from("<III", self.raw, 0x10 + n * 0x10)
            csize = struct.unpack_from("<I", self.raw, 0x60 + n * 4)[0]
            self.segments.append((foff, moff, size, csize, bool(self.flags & (1 << n))))
            seg = self.raw[foff:foff + csize]
            if self.flags & (1 << n):
                # Способ сжатия задаёт бит 7 флагов, а не имя файла: у модулей
                # игры с HOS 22.0.0+ это может быть ZBIC вместо LZ4.
                seg = zbic_decompress(seg) if self.zbic else lz4_decompress(seg)
            if len(image) < moff:
                image += b"\0" * (moff - len(image))
            image[moff:moff + size] = seg[:size]
        self.bss_size = struct.unpack_from("<I", self.raw, 0x3C)[0]
        self.image = bytes(image)

        mod_off = struct.unpack_from("<I", self.image, 4)[0]
        if self.image[mod_off:mod_off + 4] != b"MOD0":
            raise ValueError(f"{path}: не найден MOD0 по смещению {mod_off:#x}")
        self.mod_off = mod_off
        self.dyn_off = mod_off + struct.unpack_from("<i", self.image, mod_off + 4)[0]
        self.tags = self._read_dynamic()

    def _read_dynamic(self):
        tags = {}
        needed = []
        o = self.dyn_off
        while True:
            t, v = struct.unpack_from("<qQ", self.image, o)
            o += 16
            if t == 0:
                break
            if t == 1:
                needed.append(v)
            else:
                tags[t] = v
        self.needed_offsets = needed
        return tags

    def _string(self, off):
        strtab = self.tags[5]
        end = self.image.index(b"\0", strtab + off)
        return self.image[strtab + off:end].decode("latin1")

    def needed(self):
        return [self._string(o) for o in self.needed_offsets]

    def symbols(self):
        """[(имя, is_import, значение)] по всей таблице .dynsym."""
        strtab, symtab, strsz = self.tags[5], self.tags[6], self.tags[10]
        # Число символов: из DT_HASH (nchain), иначе — до начала .dynstr.
        count = None
        if 4 in self.tags:
            count = struct.unpack_from("<I", self.image, self.tags[4] + 4)[0]
        end = symtab + count * 24 if count else strtab
        out = []
        o = symtab
        while o + 24 <= end and o + 24 <= len(self.image):
            name, info, other, shndx, val, _sz = struct.unpack_from("<IBBHQQ", self.image, o)
            o += 24
            if name >= strsz:
                continue
            nm = self._string(name)
            if not nm:
                continue
            out.append((nm, shndx == 0, val, info >> 4))
        return out


# ---------------------------------------------------------------- проверки

# Модуль линкуется импортом только с nnMain (модуль `main` загружен раньше нашего).
# Функции nnSdk ищутся во время работы по имени — их манглинг обязан лежать в образе
# и обязан совпадать с тем, что экспортирует `sdk` игры (проверяется ниже).
REQUIRED_IMPORTS = []  # импортов быть не должно: всё ищется по имени во время работы

REQUIRED_RUNTIME_SYMBOLS = [
    "nnMain",                                      # точка входа игры (модуль main)
    "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",   # nn::fs::OpenFile(FileHandle*, char const*, int)
    "_ZN2nn2fs11MountSdCardEPKc",                  # nn::fs::MountSdCard(char const*)
    # nn::fs::SetAllocator(void*(*)(size_t), void(*)(void*, size_t)) — затвор
    # файловой системы (src/nxr_fsgate.hpp). Без этого символа модуль не узнает
    # разрешающий момент и вернулся бы к раннему обращению к nn::fs, от которого
    # игра падает с Result 0x327602. Опечатка в манглинге здесь и краснеет.
    "_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE",
]

# Узнавание точки монтирования romfs игры (nxr::RomMounts): модуль
# перехватывает nn::fs::MountRom (без кеша и с кешем) и nn::fs::Unmount. Имена
# ОБЯЗАНЫ лежать в образе (иначе модуль собран без узнавания, и "content:/" мода
# UltraCam не подменится никогда). А вот экспорт у игры — НЕ обязателен: у старых
# nnSdk варианта MountRom(const char*) может не быть, модуль это переживает (хук не
# ставится, двусмысленные имена просто не подменяются). Поэтому отсутствие в sdk —
# примечание, а не провал.
# Примета сборки с ранним перехватом nn::fs (src/main.cpp, nxr_early_fs_mark).
EARLY_FS_MARK = "nxr-early-fs-hook"

OPTIONAL_RUNTIME_SYMBOLS = [
    "_ZN2nn2fs8MountRomEPKc",       # nn::fs::MountRom(char const*)
    "_ZN2nn2fs8MountRomEPKcPvm",    # nn::fs::MountRom(char const*, void*, unsigned long)
    "_ZN2nn2fs7UnmountEPKc",        # nn::fs::Unmount(char const*)
]

# Дополнительно для диагностической сборки (-DNXR_DIAG): модуль пишет отчёт на SD,
# значит ищет по имени ещё и функции записи. Их манглинг обязан совпадать с экспортом
# `sdk` игры, иначе отчёт писать будет нечем.
DIAG_RUNTIME_SYMBOLS = [
    "_ZN2nn2fs10CreateFileEPKcl",                                      # CreateFile(char const*, s64)
    "_ZN2nn2fs10DeleteFileEPKc",                                       # DeleteFile(char const*)
    "_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE",  # WriteFile(...)
    "_ZN2nn2fs9FlushFileENS0_10FileHandleE",                           # FlushFile(FileHandle)
    "_ZN2nn2fs9CloseFileENS0_10FileHandleE",                           # CloseFile(FileHandle)
]

# Путь отчёта: шаблон обязан быть в образе — иначе собрана не та версия.
# Отчёты — по играм, с номером, последние 5:
# sd:/switch/ModKitNX/logs/<TitleID>/diag-NNNN.txt; в образе лежит ШАБЛОН
# (src/nxr_logs.hpp, DiagTemplate), TitleID и номер подставляются на консоли.
DIAG_REQUIRED_STRINGS = ["nxrsd:/switch/ModKitNX/logs/{TID}/diag-"]

# Функции папок: ими отчёт заводит папку игры, читает номера и удаляет старые
# (сверх 5). Нужны и диагностике (--diag), и графике (--diag-gfx).
LOGS_RUNTIME_SYMBOLS = [
    "_ZN2nn2fs15CreateDirectoryEPKc",                                          # CreateDirectory
    "_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci",                    # OpenDirectory
    "_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl",  # ReadDirectory
    "_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE",                        # CloseDirectory
    "_ZN2nn2fs10DeleteFileEPKc",                                               # DeleteFile
]
# Часы nn::time игры — для строки времени ВНУТРИ отчёта. Необязательны: нет
# экспорта — строки времени просто не будет (примечание, не провал).
LOGS_OPTIONAL_SYMBOLS = [
    "_ZN2nn4time13IsInitializedEv",
    "_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE",
    "_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE",
]
# Прежние пути отчётов в КОРНЕ карты (прежние сборки). Запрещены во ВСЕХ сборках.
OLD_REPORT_STRINGS = ["nxredirect-diag.txt", "nxredirect-gfx.txt"]

# Диагностика ГРАФИКИ (-DNXR_DIAG_GFX, src/nxr_gfx.hpp). Её отчёт — отдельный файл
# в корне карты, а перехватываемые функции модуль ищет по именам, значит имена лежат
# строками в образе. Обе приметы работают в обе стороны: в обычной сборке их быть НЕ
# должно (иначе рабочий модуль несёт диагностический код, о котором никто не просил).
GFX_REQUIRED_STRINGS = ["nxrsd:/switch/ModKitNX/logs/{TID}/gfx-"]

# Имена целей перехвата. Список НАРОЧНО короче таблицы Targets() в nxr_gfx.hpp: здесь
# нужны приметы «эта сборка графическая», а полноту и совпадение имён с заголовком
# проверяет hosttest/gfx_targets.py (он читает имена из самого заголовка). Что эти
# имена есть в таблице заголовка, сверяет self_test_gfx() в nxr_build.py.
GFX_TARGET_SYMBOLS = [
    "_ZN2nn2vi10InitializeEv",                       # nn::vi::Initialize
    "_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE",  # nn::vi::OpenDefaultDisplay
    "_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE",     # nn::vi::GetNativeWindow
    "eglCreateWindowSurface",
    "eglMakeCurrent",
    "eglSwapBuffers",
    "eglGetError",
    "nvnBootstrapLoader",
]

# Приметы УЗКОЙ ПРОБЫ БЛОКИРОВОК (-DNXR_DIAG_LOCK, ШАГ-8). Вынесены из списка
# выше: проба стала отдельным ключом и по умолчанию выключена, а
# прежняя, широкая (пять целей pthread_rwlock_*), не дала игре запуститься —
# модуль вышел за пул трамплинов exlaunch, и тот аварийно завершил процесс.
# Проверяются в обе стороны: с ключом обязаны быть, без ключа обязаны
# отсутствовать (иначе в сборке есть перехват синхронизации, о котором не просили).
LOCK_REQUIRED_STRINGS = [
    # Имя цели перехвата: без ключа таблица kLockTargets не используется, и
    # строка в образ не попадает (проверено сборкой обоих вариантов).
    "pthread_rwlock_rdlock",
    # Строка из сброса буфера — она лежит внутри самого #ifdef NXR_DIAG_LOCK.
    # Взята нарочно НЕ из заголовка раздела: заголовок печатается в обоих
    # вариантах («проба выключена»), и приметой служить не может.
    "захватов внутри eglCreateWindowSurface",
]

# Путь мода строится в рантайме из TitleID, поэтому в образе должны лежать ровно эти
# два куска, а собранной строки с TitleID там быть не может.
# Имя точки монтирования — СВОЁ, "nxrsd": под общим "sd" чужой мод
# (UltraCam для TotK) монтировал карту первым, наш MountSdCard получал 0x7802
# «имя занято», и подмена выключалась на всю сессию. См. src/nxr_path.hpp.
RUNTIME_PATH_PARTS = ["nxrsd:/atmosphere/contents/", "/files/"]

# Прежнее, ОБЩЕЕ имя точки монтирования. Строк, начинающихся с "sd:/", в образе
# быть не должно (строки в .rodata разделены нулём — ищем "\0sd:/"). Отдельная
# строка "sd" в образе ЕСТЬ законно: это пункт чёрного списка (чужая точка "sd"
# не подменяется), поэтому по ней не судим — само монтирование видно хост-тесту
# path_test (имя не общее) и по путям отчётов/мода выше.
FORBIDDEN_SHARED_MOUNT = [b"\0sd:/"]

# Исправление NvRmMemQueryHandleParams для подменённой sdk (-DNXR_COMPAT_NVRM_QUERY,
# src/nxr_compat.hpp): модуль ищет по имени две функции sdk. В обе стороны, как у
# графики: с ключом --compat-nvrm имена обязаны лежать в образе И экспортироваться
# библиотекой игры; без ключа их в образе быть не должно.
NVRM_RUNTIME_SYMBOLS = ["NvRm_MemmgrGetIoctlFile", "NvOsDrvIoctl"]
MIN_NVRM_CHECKS = 2

# Плейсхолдер из 16 нулей — подход romfsex (postbuild-замена скриптом). Он не работает:
# сегменты NSO сжаты LZ4, в готовом файле строки нет. Мы его не используем, и если он
# вдруг появится, значит собран не тот исходник.
FORBIDDEN_PLACEHOLDER = b"0000000000000000"

# svc 0x29 (GetInfo) — им берётся TitleID своего процесса. Кодирование A64:
# 0xD4000001 | (imm16 << 5); для 0x29 это 0xD4000521 (в файле байты 21 05 00 D4).
SVC_GETINFO_INSN = bytes((0x21, 0x05, 0x00, 0xD4))

MAX_SIZE = 1024 * 1024

# «Затвор нуля» гейта: сколько проверок ПО СПИСКАМ обязано быть выполнено, чтобы
# зелёный итог хоть что-то значил. Число записано ЯВНО, а не выведено из длины тех
# же списков: иначе опустошение списка опустошило бы заодно и порог, и гейт остался
# бы зелёным, ничего не проверив. 3 символа поиска (nnMain, OpenFile, MountSdCard)
# + 2 куска пути мода ("nxrsd:/atmosphere/contents/", "/files/").
MIN_LISTED_CHECKS = 5

# «Затвор нуля» проверки 10 (узкая проба блокировок). Две приметы, обе обязаны
# быть проверены — иначе ключ --diag-lock не значил бы ничего.
MIN_LOCK_CHECKS = 2

# «Затвор нуля» проверки 9 (графическая диагностика). Считается ОТДЕЛЬНО от общего:
# приметы графики проверяются и в --diag-gfx («обязаны быть»), и без него («обязаны
# отсутствовать»), и в обоих случаях пустые списки превратили бы проверку в пустое
# место. Число записано явно: 8 имён целей графической цепочки + 1 строка пути
# отчёта. (Примета пробы блокировок переехала в свою проверку 10 — см. ниже.)
MIN_GFX_CHECKS = 9


def main():
    setup_stdio()
    ap = argparse.ArgumentParser()
    ap.add_argument("nso", type=Path)
    ap.add_argument("--game-exefs", type=Path, required=True,
                    help="папка с распакованным exefs игры (main, subsdk0, sdk, rtld)")
    ap.add_argument("--max-size", type=int, default=MAX_SIZE)
    ap.add_argument("--diag", action="store_true",
                    help="модуль собран с -DNXR_DIAG: требовать ещё и символы записи на SD")
    ap.add_argument("--diag-gfx", action="store_true",
                    help="модуль собран с -DNXR_DIAG_GFX: требовать строку отчёта "
                         "nxrsd:/switch/ModKitNX/logs/{TID}/gfx- и имена перехватываемых функций графики. "
                         "Без ключа они, наоборот, запрещены")
    ap.add_argument("--compat-nvrm", action="store_true",
                    help="модуль собран с -DNXR_COMPAT_NVRM_QUERY: требовать имена "
                         "NvRm_MemmgrGetIoctlFile и NvOsDrvIoctl (и их экспорт игрой). "
                         "Без ключа они, наоборот, запрещены")
    ap.add_argument("--early-fs", action="store_true",
                    help="модуль собран с ранним перехватом nn::fs (-DNXR_EARLY_FS_HOOK): "
                         "так собирается мод со своим модулем exefs (05.10.2026). Примета "
                         "проверяется в обе стороны")
    ap.add_argument("--diag-lock", action="store_true",
                    help="модуль собран с -DNXR_DIAG_LOCK: требовать приметы УЗКОЙ "
                         "пробы блокировок. Без ключа они, наоборот, запрещены — "
                         "перехват функций синхронизации в сборку попадать не должен")
    args = ap.parse_args()

    fails = []
    notes = []
    # Счётчик РЕАЛЬНО выполненных проверок. Нужен для «затвора нуля»: гейт,
    # который ничего не проверил, обязан быть красным, а не зелёным (иначе пустой
    # список обязательных символов молча превратил бы проверку 3 в пустое место).
    checks = 0
    listed = 0  # из них — проверки по спискам (см. MIN_LISTED_CHECKS)

    # Негодный вход — НАХОДКА гейта, а не его авария. Раньше отсутствующий файл
    # улетал FileNotFoundError, а не-NSO — ValueError: прогон обрывался трассировкой
    # Python, и по коду возврата 1 нельзя было отличить «модуль плохой» от «гейт
    # сам сломался» (так уже было).
    if not args.nso.is_file():
        print("ПРОВАЛ:")
        print(f"  - нет файла модуля: {args.nso}")
        return 1
    try:
        nso = Nso(args.nso)
    except Exception as e:
        print("ПРОВАЛ:")
        print(f"  - {args.nso}: не разбирается как NSO ({e})")
        return 1
    checks += 1
    size = len(nso.raw)
    notes.append(f"NSO0 ok, размер {size} Б, MOD0 @ {nso.mod_off:#x}, .bss {nso.bss_size:#x}")
    for i, (foff, moff, sz, csz, comp) in enumerate(nso.segments):
        name = ("text", "rodata", "data")[i]
        notes.append(f"  сегмент {name}: file {foff:#x} (+{csz:#x}{' lz4' if comp else ''}) "
                     f"-> mem {moff:#x} размер {sz:#x}")
        if sz == 0:
            fails.append(f"сегмент {name} пустой")

    if size > args.max_size:
        fails.append(f"размер {size} Б превышает лимит {args.max_size} Б")

    # Экспорт модулей игры.
    game_exports = set()
    game_modules = []
    for name in ("rtld", "main", "sdk") + tuple(f"subsdk{n}" for n in range(10)):
        p = args.game_exefs / name
        if not p.is_file():
            continue
        game_modules.append(name)
        try:
            game_syms = Nso(p).symbols()
        except Exception as e:
            # Битый модуль игры — тоже находка: проверять по нему нечего.
            fails.append(f"модуль игры {name} не разбирается как NSO ({e}) — "
                         "возьмите полный распакованный exefs своей игры")
            continue
        for nm, is_import, _v, _b in game_syms:
            if not is_import:
                game_exports.add(nm)
    for req in ("rtld", "main", "sdk"):
        if req not in game_modules:
            fails.append(f"в {args.game_exefs} нет обязательного модуля игры {req} "
                         "— проверять нечем (возьмите распакованный exefs своей игры)")
    notes.append(f"модули игры: {', '.join(game_modules)}; экспортов всего {len(game_exports)}")

    syms = nso.symbols()
    imports = sorted({nm for nm, is_import, _v, _b in syms if is_import})
    notes.append(f"символов в .dynsym: {len(syms)}, из них IMPORT: {len(imports)}")

    for req in REQUIRED_IMPORTS:
        checks += 1
        if req in imports:
            notes.append(f"  IMPORT ok: {req}")
        else:
            fails.append(f"нет обязательного IMPORT: {req}")

    # Урок пробы на консоли: хук на nn::fs::OpenFile прямо в init (exl_main)
    # ушёл по нулевому адресу и уронил игру; почему адрес был нулевым — не выяснено.
    # Готов ли `sdk` к нашему init — ОТКРЫТЫЙ ВОПРОС (
    # src/main.cpp, шапка): по порядку загрузки выходит «ещё нет», по разбору кода rtld
    # TotK — `sdk` инициализируется первым; консолью порядок не проверен. Пока вопрос
    # открыт, неразрешённый на init импорт может означать обращение по нулевому адресу
    # и падение процесса игры, поэтому импортов у модуля быть не должно вовсе.
    extra_imports = [nm for nm in imports if nm not in REQUIRED_IMPORTS]
    if extra_imports:
        for nm in extra_imports:
            fails.append(f"лишний IMPORT (адрес может быть не разрешён на init): {nm}")
    else:
        notes.append("  импортов нет — падение из-за неразрешённого символа исключено")

    # Урок пробы на консоли: rtld игры читает слово по смещению +8 от начала
    # модуля как смещение до структуры module-runtime-info и копирует оттуда 12 байт.
    # У шаблона там лежало выравнивание (NOP) — rtld уходил по мусорному адресу и ронял
    # игру. Проверяем, что смещение указывает внутрь образа и структура читаема.
    rt_off = struct.unpack_from("<I", nso.image, 8)[0]
    if rt_off == 0 or rt_off + 12 > len(nso.image):
        fails.append(f"слово по +8 (module-runtime-info) не указывает внутрь модуля: {rt_off:#x}")
    else:
        ver, a, b = struct.unpack_from("<III", nso.image, rt_off)
        if ver >= 0x12:
            fails.append(f"module-runtime-info: версия {ver:#x} >= 0x12 — rtld пойдёт по строгой ветке")
        else:
            notes.append(f"  module-runtime-info @ {rt_off:#x} = ({ver:#x}, {a:#x}, {b:#x}) — ok")

    # Имена, которые модуль ищет во время работы: должны лежать строками в образе
    # и обязаны экспортироваться игрой, иначе поиск вернёт 0 и перенаправления не будет.
    runtime_symbols = list(REQUIRED_RUNTIME_SYMBOLS)
    if args.diag:
        runtime_symbols += DIAG_RUNTIME_SYMBOLS
        for s in DIAG_REQUIRED_STRINGS:
            if s.encode() in nso.image:
                notes.append(f"  диагностика: в образе есть строка {s}")
            else:
                fails.append(f"диагностика: в образе нет строки {s} "
                             "— похоже, собрано без -DNXR_DIAG")
    else:
        # Обычная сборка не должна содержать диагностического кода.
        for s in DIAG_REQUIRED_STRINGS:
            if s.encode() in nso.image:
                fails.append(f"обычная сборка содержит диагностическую строку {s}")
    if args.diag or args.diag_gfx:
        runtime_symbols += [s for s in LOGS_RUNTIME_SYMBOLS if s not in runtime_symbols]
        for s in LOGS_OPTIONAL_SYMBOLS:
            checks += 1
            if s.encode() not in nso.image:
                fails.append(f"отчёты: в образе нет имени {s} — строка времени в отчёте "
                             "не собрана (src/nxr_logs.hpp)")
            elif s not in game_exports:
                notes.append(f"  отчёты: игра не экспортирует {s} — строки времени не будет")
            else:
                notes.append(f"  отчёты: часы nn::time, поиск по символу ok: {s}")
    # Прежние пути отчётов (корень карты, прежние сборки) — признак старого исходника.
    for s in OLD_REPORT_STRINGS:
        checks += 1
        if s.encode() in nso.image:
            fails.append(f"в образе прежний путь отчёта {s} (корень карты): собран "
                         "исходник до 09.11.2026 — отчёты теперь в switch/ModKitNX/logs/<TID>/")

    # --- проверка 9: приметы диагностики графики (-DNXR_DIAG_GFX) ---
    # Проверка идёт в обе стороны и это не перестраховка: «примет нет в рабочей
    # сборке» — такое же требование, как «приметы есть в графической». Рабочий
    # модуль, несущий перехват графики, ставил бы хуки на весь путь вывода у всех.
    gfx_checks = 0
    for s in GFX_REQUIRED_STRINGS + GFX_TARGET_SYMBOLS:
        checks += 1
        gfx_checks += 1
        present = s.encode() in nso.image
        if args.diag_gfx and not present:
            fails.append(f"диагностика графики: в образе нет приметы {s} "
                         "— похоже, собрано без -DNXR_DIAG_GFX")
        elif args.diag_gfx:
            notes.append(f"  диагностика графики: в образе есть примета {s}")
        elif present:
            fails.append(f"обычная сборка содержит примету диагностики графики: {s} "
                         "— этот модуль собран с -DNXR_DIAG_GFX")
    if not args.diag_gfx and gfx_checks:
        notes.append("  примет диагностики графики нет — сборка рабочая")

    # --- проверка 9а: ранний перехват nn::fs (-DNXR_EARLY_FS_HOOK) ---
    # В обе стороны. Мод со своим модулем без раннего перехвата теряет файлы,
    # которые тот читает в своём хуке nnMain (ini UltraCam); а ранний перехват у
    # игры БЕЗ модуля мода на консоли не проверен — подсовывать его молча нельзя.
    checks += 1
    early_present = EARLY_FS_MARK.encode() in nso.image
    if args.early_fs and not early_present:
        fails.append(f"ранний перехват: в образе нет приметы {EARLY_FS_MARK} "
                     "— модуль собран без -DNXR_EARLY_FS_HOOK, а мод несёт свой модуль")
    elif args.early_fs:
        notes.append(f"  ранний перехват nn::fs: примета {EARLY_FS_MARK} есть")
    elif early_present:
        fails.append(f"модуль собран с ранним перехватом ({EARLY_FS_MARK}), а он не "
                     "заказан: у мода нет своего модуля (запустите с --early-fs, если так задумано)")
    else:
        notes.append("  раннего перехвата nn::fs нет — перехват в хуке nnMain")

    # --- проверка 10: приметы УЗКОЙ ПРОБЫ БЛОКИРОВОК (-DNXR_DIAG_LOCK) ---
    # Тоже в обе стороны. «Примет нет без ключа» — требование не меньшее: именно
    # перехват функций синхронизации, попавший в сборку без спроса, и сломал
    # запуск игры на консоли (ШАГ-7).
    lock_checks = 0
    for s in LOCK_REQUIRED_STRINGS:
        checks += 1
        lock_checks += 1
        present = s.encode() in nso.image
        if args.diag_lock and not present:
            fails.append(f"проба блокировок: в образе нет приметы {s} "
                         "— похоже, собрано без -DNXR_DIAG_LOCK")
        elif args.diag_lock:
            notes.append(f"  проба блокировок: в образе есть примета {s}")
        elif present:
            fails.append(f"сборка без ключа содержит примету пробы блокировок: {s} "
                         "— этот модуль собран с -DNXR_DIAG_LOCK")
    if not args.diag_lock and lock_checks:
        notes.append("  примет пробы блокировок нет — перехвата синхронизации в "
                     "сборке не осталось")

    # --- проверка 11: исправление NvRmMemQueryHandleParams ---
    nvrm_checks = 0
    for s in NVRM_RUNTIME_SYMBOLS:
        checks += 1
        nvrm_checks += 1
        present = s.encode() + b"\0" in nso.image
        if args.compat_nvrm and not present:
            fails.append(f"исправление NvRm: в образе нет имени {s} — собрано без "
                         "-DNXR_COMPAT_NVRM_QUERY")
        elif args.compat_nvrm and s not in game_exports:
            fails.append(f"исправление NvRm: игра (sdk) не экспортирует {s}")
        elif args.compat_nvrm:
            notes.append(f"  исправление NvRm: поиск по символу ok: {s}")
        elif present:
            fails.append(f"сборка без --compat-nvrm содержит имя {s} — модуль собран "
                         "с -DNXR_COMPAT_NVRM_QUERY")

    # --- проверка 12: прежнее общее имя точки монтирования "sd" ---
    shared = [pat for pat in FORBIDDEN_SHARED_MOUNT if pat in nso.image]
    checks += 1
    if shared:
        fails.append("в образе есть строка общей точки монтирования %r: модуль "
                     "монтирует SD под именем, которое занимают чужие моды "
                     "(0x7802 у UltraCam/TotK) — нужно своё (nxrsd)" % shared[0])
    else:
        notes.append("  общей точки монтирования \"sd\" в образе нет — имя своё")

    for sym in runtime_symbols:
        checks += 1
        listed += 1
        if sym.encode() not in nso.image:
            fails.append(f"в образе нет имени символа для поиска: {sym}")
        elif sym not in game_exports:
            fails.append(f"игра не экспортирует символ, который ищет модуль: {sym}")
        else:
            notes.append(f"  поиск по символу ok: {sym}")

    for sym in OPTIONAL_RUNTIME_SYMBOLS:
        checks += 1
        if sym.encode() not in nso.image:
            fails.append(f"в образе нет имени символа узнавания romfs: {sym} "
                         "— модуль собран без перехвата MountRom/Unmount")
        elif sym not in game_exports:
            notes.append(f"  игра не экспортирует {sym}: узнавание точки romfs без "
                         "этого варианта (модуль это переживает)")
        else:
            notes.append(f"  узнавание romfs, поиск по символу ok: {sym}")

    unresolved = [nm for nm in imports if nm not in game_exports]
    if unresolved:
        for nm in unresolved:
            fails.append(f"IMPORT не разрешается модулями игры: {nm}")
    else:
        notes.append("  все IMPORT разрешаются экспортами игры")

    needed = nso.needed()
    notes.append(f"DT_NEEDED: {needed if needed else '(пусто)'}")
    allowed_needed = {"nnSdk", "main", "rtld"} | {f"subsdk{n}" for n in range(10)}
    for n in needed:
        if n.split(".")[0] not in allowed_needed:
            fails.append(f"DT_NEEDED на неизвестный модуль: {n}")

    # --- путь мода собирается в рантайме (решения гибрида №1 и №2) ---
    for part in RUNTIME_PATH_PARTS:
        checks += 1
        listed += 1
        if part.encode() in nso.image:
            notes.append(f"  в образе есть кусок пути мода: {part}")
        else:
            fails.append(f"в образе нет куска пути мода: {part} "
                         "— путь мода собран не так, как ожидается")
    if FORBIDDEN_PLACEHOLDER in nso.image:
        fails.append("в образе найден плейсхолдер из 16 нулей: TitleID подставляется "
                     "постсборочно, а этот подход не работает (сегменты NSO сжаты LZ4)")
    else:
        notes.append("  плейсхолдера TitleID нет — номер берётся у ядра в рантайме")
    checks += 1
    if SVC_GETINFO_INSN in nso.image:
        notes.append("  в образе есть svc 0x29 (GetInfo) — TitleID спрашивается у ядра")
    else:
        fails.append("в образе нет инструкции svc 0x29 (GetInfo): TitleID взять негде")

    print("\n".join(notes))
    print()
    # «Затвор нуля». Содержательная часть гейта — проверки ПО СПИСКАМ (символы,
    # которые модуль ищет во время работы, и куски пути мода). Если списки пусты,
    # гейт перебирает пустоту и выдаёт зелёный отчёт ни о чём. Считаем их отдельно
    # от одиночных проверок и требуем, чтобы их было хотя бы столько, сколько
    # обязательных символов в REQUIRED_RUNTIME_SYMBOLS.
    if listed < MIN_LISTED_CHECKS:
        print("ПРОВАЛ:")
        print("  - ничего (или почти ничего) не проверив: по спискам выполнено %d "
              "проверок, а их обязано быть не меньше %d — это красный, а не зелёный."
              % (listed, MIN_LISTED_CHECKS))
        print("    Списки REQUIRED_RUNTIME_SYMBOLS и RUNTIME_PATH_PARTS укоротили "
              "или опустошили: гейт перебирал бы пустоту и всегда был бы зелёным.")
        return 1
    # Свой затвор нуля у проверки 9: пустые списки примет графики сделали бы и
    # «приметы есть», и «примет нет» одинаково зелёными — то есть бессмысленными.
    if lock_checks < MIN_LOCK_CHECKS:
        print("ПРОВАЛ:")
        print("  - приметы пробы блокировок не проверялись: выполнено %d проверок "
              "из обязательных %d." % (lock_checks, MIN_LOCK_CHECKS))
        print("    Список LOCK_REQUIRED_STRINGS укоротили: тогда перехват функций "
              "синхронизации мог бы попасть в сборку незамеченным — именно он "
              "сломал запуск игры 28.09.2026.")
        return 1

    if nvrm_checks < MIN_NVRM_CHECKS:
        print("ПРОВАЛ:")
        print("  - приметы исправления NvRm не проверялись: %d из %d."
              % (nvrm_checks, MIN_NVRM_CHECKS))
        return 1

    if gfx_checks < MIN_GFX_CHECKS:
        print("ПРОВАЛ:")
        print("  - приметы диагностики графики не проверялись: выполнено %d проверок "
              "из обязательных %d." % (gfx_checks, MIN_GFX_CHECKS))
        print("    Списки GFX_REQUIRED_STRINGS и GFX_TARGET_SYMBOLS укоротили или "
              "опустошили: тогда рабочая и графическая сборки перестали бы "
              "отличаться, и гейт был бы зелёным на любой из них.")
        return 1
    if fails:
        print("ПРОВАЛ:")
        for f in fails:
            print("  -", f)
        return 1
    print("ВСЕ ПРОВЕРКИ ПРОЙДЕНЫ (выполнено проверок: %d)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
