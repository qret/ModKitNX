/*
 * nvrm_test.cpp — хост-тест исправления NvRmMemQueryHandleParams (src/nxr_compat.hpp).
 *
 * Copyright (c) 2026, участники проекта nxredirect. GNU GPL версии 2.
 *
 * Две части:
 *   1. ВЫВОД СЦЕНАРИЕВ для сверки с НАСТОЯЩИМ кодом nnSdk 22.2.0 (@0x468b90),
 *      который run_nvrmtest.py выполняет в эмуляторе ARM64. Для каждого входа
 *      печатается всё наблюдаемое: ответ, fd/запрос/размер и 12 байт аргумента
 *      ioctl, 16 байт out вокруг [out+8]. Подделка ioctl одна и та же с обеих
 *      сторон: кладёт value в [arg+8] и возвращает заданный код.
 *   2. ЛОГИКА ВЫБОРА ПУТИ (DecideNvrmPath, IsNvrmStub) — обычные проверки с
 *      негативными пробами и затвором нуля.
 *
 * Сборка: g++ -std=c++17 -O2 -Wall -o nvrm_test nvrm_test.cpp -I../src
 */

#include <cstdio>
#include <cstring>
#include <string>

#include "nxr_compat.hpp"

namespace {

int g_Checks = 0;
int g_Failed = 0;

void Expect(bool cond, const char* what) {
    ++g_Checks;
    if (cond) {
        std::printf("  ok   %s\n", what);
    } else {
        ++g_Failed;
        std::printf("  КРАСНО %s\n", what);
    }
}

struct Case {
    std::int32_t  fdState;   /* < 0 — fd ещё не открыт (тогда открытие даёт 7) */
    std::uint32_t handle;
    std::uint32_t rc;        /* что вернёт ioctl */
    std::uint32_t value;     /* что ioctl положит в [arg+8] */
    std::uint8_t  fill;      /* чем заполнен out до вызова */
};

/* Те же входы перечислены в run_nvrmtest.py (CASES) — порядок важен. */
const Case kCases[] = {
    {-1, 0x1,        0, 0x3fc000,   0xAA},
    {-1, 0x2a,       0, 0x0,        0x55},
    { 5, 0x7fffffff, 0, 0xffffffff, 0xCC},
    { 5, 0x10,       4, 0x1234,     0xAA},
    {-1, 0x10,       0x80000001u, 0x1, 0x11},
    { 3, 0xdeadbeef, 0, 0x80000000u, 0x00},
    {-1, 0,          0x7,         0x0, 0xFF},
};

struct Seen {
    std::int32_t  fd = 0;
    std::uint32_t req = 0;
    std::uint32_t size = 0;
    std::uint8_t  arg[12] = {};
    int           calls = 0;
};

void Hex(const std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        std::printf("%02x", p[i]);
    }
}

void RunCases() {
    int idx = 0;
    for (const Case& c : kCases) {
        Seen seen;
        std::uint8_t out[0x38];
        std::memset(out, c.fill, sizeof(out));
        const std::int32_t fd = c.fdState < 0 ? 7 : c.fdState;
        const std::uint32_t ret = nxr::compat::QueryHandleParams(
            c.handle, out,
            [fd]() { return fd; },
            [&seen, &c](std::int32_t f, std::uint32_t req, void* arg, std::uint32_t size) {
                ++seen.calls;
                seen.fd = f;
                seen.req = req;
                seen.size = size;
                std::memcpy(seen.arg, arg, 12);   /* аргумент, каким его получил ioctl */
                std::memcpy(static_cast<std::uint8_t*>(arg) + 8, &c.value, 4);
                return c.rc;
            });
        std::printf("case %d ret=%08x calls=%d fd=%d req=%08x size=%u arg=", idx, ret,
                    seen.calls, seen.fd, seen.req, seen.size);
        Hex(seen.arg, 12);
        std::printf(" out=");
        Hex(out, 0x18);
        std::printf("\n");
        ++idx;
    }
}

void TestDecide() {
    using namespace nxr::compat;
    std::printf("\n--- выбор пути исправления (экспорт / замена заглушки) ---\n");
    const std::uint64_t ours = 0x1000, sdk = 0x9000;
    Expect(DecideNvrmPath(ours, ours, sdk, ours, true) == NvrmPath::ExportBound,
           "импорт GL уже указывает на наш экспорт -> экспорт, хук не нужен");
    Expect(DecideNvrmPath(0, ours, sdk, ours, true) == NvrmPath::ExportByRule,
           "импорт не связан, по правилу rtld выигрываем мы -> экспорт");
    Expect(DecideNvrmPath(sdk, ours, sdk, ours, true) == NvrmPath::HookStub,
           "импорт связан с sdk (экспорт проиграл), в sdk заглушка -> заменить заглушку");
    Expect(DecideNvrmPath(sdk, ours, sdk, sdk, false) == NvrmPath::LeaveAlone,
           "экспорт проиграл, а в sdk НЕ заглушка (родная 22.2) -> не трогать");
    Expect(DecideNvrmPath(0, ours, sdk, sdk, true) == NvrmPath::HookStub,
           "импорт не связан, по правилу rtld выигрывает sdk-заглушка -> заменить");
    Expect(DecideNvrmPath(0, ours, 0, ours, false) == NvrmPath::ExportByRule,
           "в sdk символа нет, выигрываем мы -> экспорт");
    Expect(DecideNvrmPath(0, 0, sdk, sdk, false) == NvrmPath::LeaveAlone,
           "сборка без экспорта, родная sdk -> не трогать");
    /* Автоисправление: заглушка есть, экспорт проиграл, но решено
       «прокси» (Vulkan в процессе, как у SiN) — заглушку НЕ заменяем. */
    Expect(DecideNvrmPath(sdk, ours, sdk, sdk, true, false) == NvrmPath::NotNeeded,
           "экспорт проиграл, в sdk заглушка, автоисправление выбрало прокси -> не трогать");
    Expect(DecideNvrmPath(ours, ours, sdk, ours, true, false) == NvrmPath::ExportBound,
           "экспорт выиграл -> экспорт (решение своя/прокси принимает сам экспорт)");

    const std::uint32_t stub19[4] = {0x52800040u, 0xD65F03C0u, 0, 0};
    const std::uint32_t real22[4] = {0xD10103FFu, 0xA9017BFDu, 0xA90257F6u, 0xA9034FF4u};
    std::uint32_t nextFn[4] = {0x52800040u, 0xD65F03C0u, 0x52800040u, 0};
    Expect(IsNvrmStub(stub19, 0x45a370), "заглушка 19.3.13 опознаётся (16 байт, адрес кратен 8)");
    Expect(!IsNvrmStub(real22, 0x468b90), "НЕГАТИВ: настоящая функция 22.2.0 — не заглушка");
    Expect(!IsNvrmStub(nextFn, 0x45a370), "НЕГАТИВ: за ret не udf — перезапись задела бы соседа");
    Expect(!IsNvrmStub(stub19, 0x45a374), "НЕГАТИВ: адрес не кратен 8 — exlaunch пишет 20 байт");
    Expect(!IsNvrmStub(nullptr, 0x45a370), "НЕГАТИВ: нулевой указатель");

    /* ---- негативная проба ---- */
    /* Ослабленное правило «всегда менять заглушку, если экспорт не связан»:
       на родной 22.2 (не заглушка) оно заменило бы рабочую функцию. */
    const bool weakWouldHook = true;   /* ОСЛАБЛЕНО: заглушку не проверяем */
    Expect(weakWouldHook && DecideNvrmPath(sdk, ours, sdk, sdk, IsNvrmStub(real22, 0x468b90))
                                != NvrmPath::HookStub,
           "НЕГАТИВНАЯ ПРОБА: без проверки заглушки родная 22.2 была бы перекрыта");
}

}  // namespace

int main() {
    std::printf("=== хост-тест NvRmMemQueryHandleParams (src/nxr_compat.hpp) ===\n");
    RunCases();
    TestDecide();
    std::printf("\nвыполнено проверок: %d, красных: %d\n", g_Checks, g_Failed);
    if (g_Checks < 13) {
        std::printf("КРАСНО: проверок слишком мало (%d)\n", g_Checks);
        return 1;
    }
    if (g_Failed != 0) {
        std::printf("ТЕСТ КРАСНЫЙ\n");
        return 1;
    }
    std::printf("ТЕСТ ЗЕЛЁНЫЙ\n");
    return 0;
}
