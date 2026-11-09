/*
 * hash_test.cpp — хост-тест реализации недостающих символов (src/nxr_compat.hpp).
 *
 * Собирается обычным g++ на ПК и печатает по строке на случай:
 *     <смещение> <длина> <хеш в hex>
 * плюс отдельную строку "ldn <код возврата заглушки>".
 *
 * Сверяет результат не этот файл, а hosttest/run_hashtest.py: он гоняет
 * НАСТОЯЩИЙ код _ZNSt3__113__hash_memoryEPKvm из NintendoSdk_nnSdk-22_2_0-Release
 * в эмуляторе ARM64 и сравнивает значения один в один.
 *
 * Данные детерминированы (простой ЛКГ), чтобы обе стороны считали по одному и
 * тому же буферу без передачи файлов.
 */

#include <cstdint>
#include <cstddef>
#include <initializer_list>
#include <cstdio>
#include <cstring>

/* Хост-тест проверяет САМ КОД, поэтому включает оба экспорта явно. В модуле они
   по умолчанию выключены и включаются ключами сборки (см. nxr_compat.hpp). */
#define NXR_COMPAT_HASH_MEMORY 1
#define NXR_COMPAT_LDN_STUB 1
#include "../src/nxr_compat.hpp"

/* Те же объявления, что в модуле: имена символов заданы asm-меткой, поэтому
   зовём функции по их C++-именам. */
extern "C" std::size_t nxr_compat_hash_memory(const void* key, std::size_t len);
extern "C" std::uint32_t nxr_compat_ldn_set_protocol(std::int32_t protocol);

static unsigned char g_buf[8192];

static void FillBuf() {
    std::uint32_t s = 123456789u;
    for (std::size_t i = 0; i < sizeof(g_buf); ++i) {
        s = s * 1103515245u + 12345u;
        g_buf[i] = static_cast<unsigned char>((s >> 16) & 0xFFu);
    }
}

static void Emit(std::size_t off, std::size_t len) {
    const std::size_t h = nxr_compat_hash_memory(g_buf + off, len);
    std::printf("%zu %zu %016llx\n", off, len, static_cast<unsigned long long>(h));
}

int main() {
    FillBuf();

    /* Отдельный случай: строка "camera_create" (13 байт) — ровно та, на которой
       падает Well Dweller внутри nn::init::Start. */
    {
        const char* s = "camera_create";
        const std::size_t h = nxr_compat_hash_memory(s, std::strlen(s));
        std::printf("camera_create %zu %016llx\n", std::strlen(s),
                    static_cast<unsigned long long>(h));
    }

    /* Пустая строка, 0..64 байт подряд — все ветки коротких длин. */
    for (std::size_t len = 0; len <= 64; ++len) {
        Emit(0, len);
    }
    /* Невыровненные начала — важны, потому что чтение 8 байт идёт memcpy. */
    for (std::size_t off : {std::size_t(1), std::size_t(3), std::size_t(7), std::size_t(9)}) {
        for (std::size_t len : {std::size_t(0), std::size_t(1), std::size_t(4), std::size_t(8),
                                std::size_t(9), std::size_t(13), std::size_t(16), std::size_t(17),
                                std::size_t(31), std::size_t(32), std::size_t(33), std::size_t(64),
                                std::size_t(65)}) {
            Emit(off, len);
        }
    }
    /* Длинные: цикл по 64 байта, включая границы 64/65/127/128/129. */
    for (std::size_t len : {std::size_t(65), std::size_t(96), std::size_t(127), std::size_t(128),
                            std::size_t(129), std::size_t(255), std::size_t(256), std::size_t(257),
                            std::size_t(1000), std::size_t(4096), std::size_t(8000)}) {
        Emit(0, len);
        Emit(5, len);
    }

    std::printf("ldn %u\n", nxr_compat_ldn_set_protocol(1));
    std::printf("ldn %u\n", nxr_compat_ldn_set_protocol(3));
    std::printf("ldn %u\n", nxr_compat_ldn_set_protocol(0));
    return 0;
}
