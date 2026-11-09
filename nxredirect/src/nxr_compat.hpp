/*
 * nxr_compat.hpp — символы, которых не хватает играм, когда вместо родной
 * библиотеки игры грузится системная `sdk` другой версии.
 *
 * Copyright (c) 2026, участники проекта nxredirect. Лицензия — GNU GPL версии 2
 * (те же условия, что у остального модуля).
 *
 * ЗАЧЕМ ЭТОТ ФАЙЛ
 * ---------------
 * На консоли с HOS 19 (например, 19.0.1 + Atmosphère 1.11.1) вместо библиотеки, с
 * которой игра собиралась, в процесс грузится `NintendoSdk_nnSdk-19_3_13-Release`
 * (опознан по BuildId 7BBA185A35EE9B4D0AF66A4DF910A8234D1C9CF5). В этой сборке
 * нет части символов, которые игра ждёт, и связыватель (rtld) роняет процесс на
 * неразрешённом символе:
 *
 *   _ZNSt3__113__hash_memoryEPKvm            — std::__1::__hash_memory(void const*, unsigned long)
 *       Нужен обеим играм. В `Well Dweller` — уже внутри nn::init::Start
 *       (хешируется строка "camera_create", 13 байт), в `SiN Reloaded` — позже.
 *
 *   _ZN2nn3ldn11SetProtocolENS0_8ProtocolE   — nn::ldn::SetProtocol(nn::ldn::Protocol)
 *       Нужен только `SiN Reloaded` и только для локального мультиплеера (LDN).
 *
 * Здесь — СВОИ РЕАЛИЗАЦИИ (чистые, сверяются на ПК с оригиналом). Экспорты
 * модуля — в nxr_compat_exports.hpp: это прокси, которые сами
 * решают в рантайме, звать настоящую функцию загруженной `sdk` или свою
 * (nxr_autofix.hpp). *Было:* «ЭТИ ЭКСПОРТЫ ПО УМОЛЧАНИЮ ВЫКЛЮЧЕНЫ, каждый
 * включается своим ключом сборки» — заменено автоисправлением, см. ниже.
 *
 * КАК ЭТО РАБОТАЕТ
 * ----------------
 * rtld ищет символ ПО ВСЕМ загруженным модулям (DT_NEEDED он не читает) в порядке
 * загрузки: rtld, main, subsdk0..subsdk9, sdk. Выигрывает ПЕРВЫЙ найденный
 * GLOBAL-символ. Наш модуль — subsdkN, то есть идёт РАНЬШЕ `sdk`, поэтому его
 * экспорт и будет выбран. Выполняться нашему коду для этого не нужно: символ
 * разрешается по таблице .dynsym.
 *
 * Отсюда требования к обеим функциям:
 *   - имя символа должно совпадать с ожидаемым ДОСЛОВНО (задаётся asm-меткой,
 *     а не именем функции в C++ — так исключены сюрпризы манглинга);
 *   - символ обязан быть GLOBAL FUNC с видимостью DEFAULT и не должен быть
 *     выброшен сборкой (весь проект собирается с -fvisibility=hidden и
 *     --gc-sections, поэтому здесь стоят visibility("default") и used).
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace nxr::compat {

/* ---------------------------------------------------------------- __hash_memory
 *
 * Это ровно тот алгоритм, что в libc++: std::__1::__murmur2_or_cityhash<size_t>
 * для 64-битного size_t, то есть вариант CityHash64. Оригинал сверен по
 * дизассемблеру NintendoSdk_nnSdk-22_2_0-Release: обёртка 0x5F6120 (44 байта)
 * зовёт тело 0x5F6150 (928 байт); в теле нет ни одного внешнего вызова и ни
 * одного системного вызова — только арифметика, поэтому повторить его можно
 * точно, и на ПК результат сверяется с оригиналом (hosttest/hash_test.cpp
 * против эмулятора настоящего кода SDK).
 *
 * Константы взяты из того же дизассемблера (movk-цепочки видны дословно).
 */

inline constexpr std::uint64_t k0 = 0xC3A5C85C97CB3127ULL;
inline constexpr std::uint64_t k1 = 0xB492B66FBE98F273ULL;
inline constexpr std::uint64_t k2 = 0x9AE16A3B2F90404FULL;
inline constexpr std::uint64_t k3 = 0xC949D7C7509E6557ULL;
inline constexpr std::uint64_t k_mul = 0x9DDFEA08EB382D69ULL;

inline std::uint64_t LoadWord64(const char* p) {
    std::uint64_t v;
    __builtin_memcpy(&v, p, sizeof(v));
    return v;
}

inline std::uint32_t LoadWord32(const char* p) {
    std::uint32_t v;
    __builtin_memcpy(&v, p, sizeof(v));
    return v;
}

/* Сдвиг 0 в оригинале обрабатывается отдельно: сдвиг на 64 — неопределённое
   поведение, и компилятор вправе выдать что угодно. */
inline std::uint64_t Rotate(std::uint64_t val, int shift) {
    return shift == 0 ? val : ((val >> shift) | (val << (64 - shift)));
}

/* Вариант для случая, когда сдвиг заведомо не нуль (len от 9 до 16). */
inline std::uint64_t RotateByAtLeast1(std::uint64_t val, int shift) {
    return (val >> shift) | (val << (64 - shift));
}

inline std::uint64_t ShiftMix(std::uint64_t val) { return val ^ (val >> 47); }

inline std::uint64_t HashLen16(std::uint64_t u, std::uint64_t v) {
    std::uint64_t a = (u ^ v) * k_mul;
    a ^= (a >> 47);
    std::uint64_t b = (v ^ a) * k_mul;
    b ^= (b >> 47);
    b *= k_mul;
    return b;
}

inline std::uint64_t HashLen0To16(const char* s, std::uint64_t len) {
    if (len > 8) {
        const std::uint64_t a = LoadWord64(s);
        const std::uint64_t b = LoadWord64(s + len - 8);
        return HashLen16(a, RotateByAtLeast1(b + len, static_cast<int>(len))) ^ b;
    }
    if (len >= 4) {
        const std::uint32_t a = LoadWord32(s);
        const std::uint32_t b = LoadWord32(s + len - 4);
        /* ВНИМАНИЕ: (a << 3) считается в 32 битах и обрезается — в оригинале
           `a` имеет тип uint32_t. Сдвиг в 64 битах даёт другие значения на всех
           длинах 4..8 (поймано сверкой с оригиналом: len=4 дало 8fd8... вместо
           c670...). В дизассемблере видно `lsl w9, w9, #3` — регистр w, не x. */
        return HashLen16(len + static_cast<std::uint64_t>(a << 3), b);
    }
    if (len > 0) {
        const unsigned char a = static_cast<unsigned char>(s[0]);
        const unsigned char b = static_cast<unsigned char>(s[len >> 1]);
        const unsigned char c = static_cast<unsigned char>(s[len - 1]);
        const std::uint32_t y = static_cast<std::uint32_t>(a) + (static_cast<std::uint32_t>(b) << 8);
        const std::uint32_t z = static_cast<std::uint32_t>(len) + (static_cast<std::uint32_t>(c) << 2);
        return ShiftMix(y * k2 ^ z * k3) * k2;
    }
    return k2;
}

inline std::uint64_t HashLen17To32(const char* s, std::uint64_t len) {
    const std::uint64_t a = LoadWord64(s) * k1;
    const std::uint64_t b = LoadWord64(s + 8);
    const std::uint64_t c = LoadWord64(s + len - 8) * k2;
    const std::uint64_t d = LoadWord64(s + len - 16) * k0;
    return HashLen16(Rotate(a - b, 43) + Rotate(c, 30) + d,
                     a + Rotate(b ^ k3, 20) - c + len);
}

struct Pair64 {
    std::uint64_t first;
    std::uint64_t second;
};

inline Pair64 WeakHashLen32WithSeeds(std::uint64_t w, std::uint64_t x, std::uint64_t y,
                                     std::uint64_t z, std::uint64_t a, std::uint64_t b) {
    a += w;
    b = Rotate(b + a + z, 21);
    const std::uint64_t c = a;
    a += x;
    a += y;
    b += Rotate(a, 44);
    return Pair64{a + z, b + c};
}

inline Pair64 WeakHashLen32WithSeeds(const char* s, std::uint64_t a, std::uint64_t b) {
    return WeakHashLen32WithSeeds(LoadWord64(s), LoadWord64(s + 8), LoadWord64(s + 16),
                                  LoadWord64(s + 24), a, b);
}

inline std::uint64_t HashLen33To64(const char* s, std::uint64_t len) {
    std::uint64_t z = LoadWord64(s + 24);
    std::uint64_t a = LoadWord64(s) + (len + LoadWord64(s + len - 16)) * k0;
    std::uint64_t b = Rotate(a + z, 52);
    std::uint64_t c = Rotate(a, 37);
    a += LoadWord64(s + 8);
    c += Rotate(a, 7);
    a += LoadWord64(s + 16);
    const std::uint64_t vf = a + z;
    const std::uint64_t vs = b + Rotate(a, 31) + c;
    a = LoadWord64(s + 16) + LoadWord64(s + len - 32);
    /* Именно `+=`, а не `=`: во втором блоке z накапливается (оригинал CityHash,
       libc++ его сохранил). В дизассемблере 22.2.0 это видно как
       `add x13, x13, x14`, где x13 = W(len-8), x14 = W(24) — второе слагаемое
       осталось от первого блока. Ошибка здесь ломает все длины 33..64. */
    z += LoadWord64(s + len - 8);
    b = Rotate(a + z, 52);
    c = Rotate(a, 37);
    a += LoadWord64(s + len - 24);
    c += Rotate(a, 7);
    a += LoadWord64(s + len - 16);
    const std::uint64_t wf = a + z;
    const std::uint64_t ws = b + Rotate(a, 31) + c;
    const std::uint64_t r = ShiftMix((vf + ws) * k2 + (wf + vs) * k0);
    return ShiftMix(r * k0 + vs) * k2;
}

/* Общая точка: ровно тело std::__1::__murmur2_or_cityhash<size_t,64>::operator(). */
inline std::uint64_t HashMemory(const void* key, std::uint64_t len) {
    const char* s = static_cast<const char*>(key);
    if (len <= 32) {
        if (len <= 16) {
            return HashLen0To16(s, len);
        }
        return HashLen17To32(s, len);
    }
    if (len <= 64) {
        return HashLen33To64(s, len);
    }

    /* Строки длиннее 64 байт: сначала хешируется хвост, дальше цикл по 64 байта
       с 56 байтами состояния. */
    std::uint64_t x = LoadWord64(s + len - 40);
    std::uint64_t y = LoadWord64(s + len - 16) + LoadWord64(s + len - 56);
    std::uint64_t z = HashLen16(LoadWord64(s + len - 48) + len, LoadWord64(s + len - 24));
    Pair64 v = WeakHashLen32WithSeeds(s + len - 64, len, z);
    Pair64 w = WeakHashLen32WithSeeds(s + len - 32, y + k1, x);
    x = x * k1 + LoadWord64(s);

    len = (len - 1) & ~static_cast<std::uint64_t>(63);
    do {
        x = Rotate(x + y + v.first + LoadWord64(s + 8), 37) * k1;
        y = Rotate(y + v.second + LoadWord64(s + 48), 42) * k1;
        x ^= w.second;
        y += v.first + LoadWord64(s + 40);
        z = Rotate(z + w.first, 33) * k1;
        v = WeakHashLen32WithSeeds(s, v.second * k1, x + w.first);
        w = WeakHashLen32WithSeeds(s + 32, z + w.second, y + LoadWord64(s + 16));
        const std::uint64_t t = z;
        z = x;
        x = t;
        s += 64;
        len -= 64;
    } while (len != 0);

    return HashLen16(HashLen16(v.first, w.first) + ShiftMix(y) * k1 + z,
                     HashLen16(v.second, w.second) + x);
}

/* --------------------------------------------- NvRmMemQueryHandleParams
 *
 * ЗАЧЕМ. NintendoSDK_OpenGL-22_2_0 (subsdk0 Well Dweller) при создании поверхности
 * окна импортирует буферы окна и спрашивает их размер через
 * NvRmMemQueryHandleParams. В nnSdk 19.3.13 эта функция — ПУСТАЯ ЗАГЛУШКА
 * (@0x45a370: `mov w0,#2; ret`), в 22.2.0 — настоящая (@0x468b90). GL получает
 * отказ, импорт буфера проваливается, eglCreateWindowSurface отдаёт 0x3003
 * «Unable to allocate surface resources» — чёрный экран (разбор
 * кода Well Dweller; доказано кодом, НЕ консолью).
 *
 * ЧТО ДЕЛАЕТ 22.2.0 (дизассемблер @0x468b90, дословно):
 *     fd = [глобальный fd nvmap]; если fd < 0 — под мьютексом
 *          NvOsDrvOpen("/dev/nvmap") и запомнить          ; = NvRm_MemmgrGetIoctlFile
 *     struct { u32 handle = w0; u32 param = 1; u32 value = 0; } a;   ; stp w20,w8,[sp,#4]; str wzr,[sp,#0xc]
 *     rc = NvOsDrvIoctl(fd, 0xC00C0109, &a, 0xC);                     ; NVMAP_IOC_PARAM
 *     if (rc != 0) return rc;               ; out НЕ трогается
 *     *(u64*)(out + 8) = (u64)a.value;      ; ldr w8,[sp,#0xc]; str x8,[x19,#8] — 64 бита, старшие нули
 *     return 0;
 *   x1 и w3 (у вызывающего w3 = 0x38) не используются. Других полей out не пишет.
 *
 * Чем повторяем в 19.3.13. Ровно теми же частями, которые она сама экспортирует:
 *   NvRm_MemmgrGetIoctlFile (@0x458f40) — тот же глобальный fd nvmap, что у
 *     NvRmMemGetSize/NvRmMemAlloc/NvRmMemHandleFree и прочих (одна ячейка
 *     [0xb89220]); логика открытия совпадает с 22.2.0 (@0x467740) по шагам.
 *     Важно, что fd ОБЩИЙ: хэндлы nvmap живут в том клиенте, который их создал;
 *     свой второй fd здесь был бы ошибкой;
 *   NvOsDrvIoctl (@0x456a50) — тот же вызов; прошивка эпохи 19 этот ioctl
 *     обслуживает: тем же 0xC00C0109 с param=1 пользуется NvRmMemGetSize
 *     самой 19.3.13 (@0x45a094).
 * Через NvRmMemGetSize повторить точно нельзя: она теряет код ошибки ioctl
 * (возвращает 0) и не отличает «размер 0» от отказа.
 *
 * Чистая часть ниже — шаблон с подставляемыми fd и ioctl: на ПК она сверяется
 * с НАСТОЯЩИМ кодом 22.2.0 в эмуляторе ARM64 (hosttest/run_nvrmtest.py):
 * одинаковые входы — одинаковые аргументы ioctl, ответ и байты out.
 */

/* Аргумент NVMAP_IOC_PARAM в той раскладке, что строит 22.2.0 на стеке. */
struct NvmapParamArgs {
    std::uint32_t handle;
    std::uint32_t param;
    std::uint32_t value;
};
static_assert(sizeof(NvmapParamArgs) == 12, "размер аргумента ioctl обязан быть 0xC");

inline constexpr std::uint32_t NvmapIocParam      = 0xC00C0109u;  /* _IOWR('\x01', 9, 12) */
inline constexpr std::uint32_t NvmapParamSize     = 1u;           /* NVMAP_HANDLE_PARAM_SIZE */
inline constexpr std::uint64_t QueryOutSizeOffset = 8;            /* [out+8] — u64 размер */

/*
 * getFd()                         -> s32 fd (как NvRm_MemmgrGetIoctlFile)
 * ioctl(fd, request, arg, size)   -> u32 код (как NvOsDrvIoctl)
 */
template <class GetFd, class Ioctl>
inline std::uint32_t QueryHandleParams(std::uint32_t handle, void* out,
                                       GetFd&& getFd, Ioctl&& ioctl) {
    NvmapParamArgs a;
    a.handle = handle;
    a.param  = NvmapParamSize;
    a.value  = 0;
    const std::int32_t fd = getFd();
    const std::uint32_t rc = ioctl(fd, NvmapIocParam, &a,
                                   static_cast<std::uint32_t>(sizeof(a)));
    if (rc != 0) {
        return rc;
    }
    const std::uint64_t size = a.value;
    __builtin_memcpy(static_cast<unsigned char*>(out) + QueryOutSizeOffset, &size,
                     sizeof(size));
    return 0;
}

/*
 * КАКИМ ПУТЁМ ИСПРАВЛЕНИЕ ДОХОДИТ ДО GL — решение в рантайме (хук nnMain).
 *
 * Основной путь — ЭКСПОРТ: rtld отдаёт импорт первому определённому не-LOCAL
 * символу при обходе модулей по возрастанию адреса (разбор кода rtld — в
 * nxr_symfind.hpp у RtldWinnerOf), наш subsdkN лежит раньше sdk. Ни main, ни
 * subsdk0 (GL), ни subsdk1 (movie) этот символ не определяют (проверено по
 * .dynsym дампа). Значит, по коду rtld выигрывает наш.
 *
 * Запасной путь — замена заглушки в sdk (хук exlaunch без трамплина,
 * 1–4 инструкции поверх `mov w0,#2; ret; udf; udf`). Включается ТОЛЬКО если
 * проверка в рантайме показала, что экспорт не выиграл, и ТОЛЬКО если по
 * адресу sdk лежит ровно заглушка 19.3.13 (16 байт): настоящую функцию родной
 * 22.2 мы не трогаем никогда.
 *
 *   bound       — что лежит в слоте GOT импортёра (GL) сейчас (0 — импортёра нет);
 *   ours        — адрес нашего экспорта;
 *   sdkDef      — адрес определения в другом модуле (sdk), 0 — нет;
 *   rtldWinner  — чей адрес по правилу rtld (первый определённый не-LOCAL);
 *   sdkIsStub   — по sdkDef лежат ровно 16 байт заглушки и адрес кратен 8.
 */
enum class NvrmPath : int {
    ExportBound   = 1,  /* слот импортёра уже указывает на наш экспорт */
    ExportByRule  = 2,  /* слот ещё не связан, но по правилу rtld выиграет наш */
    HookStub      = 3,  /* экспорт не выиграл — заменяем заглушку sdk */
    LeaveAlone    = 4,  /* экспорт не выиграл, а в sdk НЕ заглушка — не трогаем */
    NotNeeded     = 5,  /* экспорт не выиграл, в sdk заглушка, но автоисправление
                           решило «прокси» (родное поведение) — заменять нечем и незачем */
};

/*
 * ownChosen (автоисправление): решение nxr::autofix для этого
 * символа — СВОЯ реализация. Заглушку sdk заменяем ТОЛЬКО тогда: если решено
 * «прокси» (например, в процессе Vulkan, как у SiN Reloaded), переход на нашу
 * функцию вернул бы вызов обратно в ту же заглушку — смысла нет, а поведение
 * должно остаться родным. По умолчанию true — прежнее поведение.
 */
inline NvrmPath DecideNvrmPath(std::uint64_t bound, std::uint64_t ours,
                               std::uint64_t sdkDef, std::uint64_t rtldWinner,
                               bool sdkIsStub, bool ownChosen = true) {
    if (ours != 0 && bound == ours) {
        return NvrmPath::ExportBound;
    }
    if (bound == 0 || (bound != sdkDef)) {
        /* Импорт ещё не связан (или связан с чем-то третьим): судим по правилу rtld. */
        if (ours != 0 && rtldWinner == ours) {
            return NvrmPath::ExportByRule;
        }
    }
    if (sdkDef != 0 && sdkIsStub) {
        return ownChosen ? NvrmPath::HookStub : NvrmPath::NotNeeded;
    }
    return NvrmPath::LeaveAlone;
}

inline const char* NvrmPathText(NvrmPath p) {
    switch (p) {
        case NvrmPath::ExportBound:
            return "ЭКСПОРТ: импорт GL уже связан с нашей функцией";
        case NvrmPath::ExportByRule:
            return "ЭКСПОРТ: по правилу rtld выигрывает наш символ";
        case NvrmPath::HookStub:
            return "ЗАПАСНОЙ ПУТЬ: экспорт не выиграл, заглушка sdk заменена переходом на нашу функцию";
        case NvrmPath::LeaveAlone:
            return "НЕ ПРИМЕНЕНО: экспорт не выиграл, а в sdk не заглушка 19.3.13 — не трогаем";
        case NvrmPath::NotNeeded:
            return "НЕ НУЖНО: экспорт не выиграл, в sdk заглушка, но автоисправление выбрало родное поведение (прокси)";
    }
    return "неизвестно";
}

/* Ровно эти 16 байт лежат по NvRmMemQueryHandleParams в nnSdk 19.3.13 и 20.5.17. */
inline constexpr std::uint32_t NvrmStubWords[4] = {
    0x52800040u,   /* mov w0, #2 */
    0xD65F03C0u,   /* ret        */
    0x00000000u,   /* udf #0     */
    0x00000000u,   /* udf #0     */
};

inline bool IsNvrmStub(const std::uint32_t* code, std::uint64_t addr) {
    if (code == nullptr || (addr & 7u) != 0) {
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (code[i] != NvrmStubWords[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace nxr::compat

/* ------------------------------------------------------------------- экспорты
 *
 * ИЗМЕНЕНО: экспорты модуля переехали в nxr_compat_exports.hpp и
 * стали АВТОИСПРАВЛЕНИЕМ — прокси с запасом, которое само решает в рантайме,
 * звать настоящую функцию sdk или свою (nxr_autofix.hpp). Ключи сборки
 * -DNXR_COMPAT_HASH_MEMORY / -DNXR_COMPAT_LDN_STUB / -DNXR_COMPAT_NVRM_QUERY
 * модулем больше не управляют.
 *
 * *Было раньше (сохранено как история решения):* «Экспортов по
 * умолчанию НЕТ. Оба включаются отдельными ключами сборки, потому что наш
 * subsdkN ищется РАНЬШЕ sdk и перекрывает одноимённый символ у КАЖДОГО, кто
 * поставит модуль: __hash_memory перекрыл бы рабочую функцию, заглушка
 * SetProtocol — настоящий LDN.» Опасность была настоящей; ответ на неё теперь —
 * не ключ, а прокси: на консоли с родной sdk вызов уходит в её функцию.
 *
 * Ниже остались ТОЛЬКО экспорты для хост-теста hosttest/hash_test.cpp: он
 * определяет NXR_COMPAT_HASH_MEMORY и NXR_COMPAT_LDN_STUB и вызывает чистую
 * свою реализацию по тем же asm-именам (без прокси и без svc). В модуль эти
 * блоки не попадают: nxr_compat_exports.hpp при этих ключах останавливает
 * сборку.
 */

#ifdef NXR_COMPAT_HASH_MEMORY

extern "C" __attribute__((visibility("default"), used, noinline))
std::size_t nxr_compat_hash_memory(const void* key, std::size_t len)
    asm("_ZNSt3__113__hash_memoryEPKvm");

std::size_t nxr_compat_hash_memory(const void* key, std::size_t len) {
    return static_cast<std::size_t>(nxr::compat::HashMemory(key, static_cast<std::uint64_t>(len)));
}

#endif  /* NXR_COMPAT_HASH_MEMORY */

#ifdef NXR_COMPAT_LDN_STUB

/*
 * nn::ldn::SetProtocol(nn::ldn::Protocol) — ЗАГЛУШКА (своя реализация).
 *
 * В оригинале (22.2.0, 0x583FC0, 80 байт) это тонкая обёртка: проверяет аргумент
 * (0 -> 3, допустимы 1 и 3, иначе уход в обработчик ошибки) и хвостовым вызовом
 * зовёт виртуальный метод LDN-клиента по смещению 0x80 в его таблице. Снаружи
 * честно повторить это нечем: нужного объекта у нас нет. Возвращает успех.
 */
extern "C" __attribute__((visibility("default"), used, noinline))
std::uint32_t nxr_compat_ldn_set_protocol(std::int32_t protocol)
    asm("_ZN2nn3ldn11SetProtocolENS0_8ProtocolE");

std::uint32_t nxr_compat_ldn_set_protocol(std::int32_t protocol) {
    (void)protocol;
    return 0;  /* nn::ResultSuccess */
}

#endif  /* NXR_COMPAT_LDN_STUB */
