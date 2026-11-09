/*
 * ModKitNX core — переходник upstream zstd 1.5.7 (BSD) к таблицам частот ZBIC.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ZBIC — это zstd с магией кадра "ZBIC" и таблицами нормированных частот FSE в binary
 * interpolative coding; всё остальное — стандартный zstd. Upstream zstd НЕ правится:
 * его entropy_common.c включается сюда с переименованием FSE_readNCount / _bmi2, а
 * настоящие имена определяет этот файл — диспетчер. Все места zstd, читающие таблицу
 * частот (последовательности, веса Huffman), зовут FSE_readNCount[_bmi2] и попадают сюда:
 * в кадре ZBIC — наш разбор BIC, в обычном кадре — штатный разбор zstd.
 * Сборка: entropy_common.c из zstd НЕ компилировать отдельно (он уже внутри этого файла).
 *
 * Разбор BIC — порт read_ncount_bic из nxredirect/scripts/zbic_pure.py; там
 * алгоритм выверен по FSE_readNCount_bic из zstd.c Atmosphere-NX (GPL-2.0). Таблица
 * BIC_table не скопирована: порождается обходом дерева (как bic_table() в zbic_pure.py).
 */
#define FSE_readNCount mkzu_std_FSE_readNCount
#define FSE_readNCount_bmi2 mkzu_std_FSE_readNCount_bmi2
#include "common/entropy_common.c"
#undef FSE_readNCount
#undef FSE_readNCount_bmi2

#include "zstd_errors.h"

#include <stdint.h>
#include <string.h>

size_t FSE_readNCount_bmi2(short* normalizedCounter, unsigned* maxSVPtr, unsigned* tableLogPtr,
                           const void* headerBuffer, size_t hbSize, int bmi2);
size_t FSE_readNCount(short* normalizedCounter, unsigned* maxSVPtr, unsigned* tableLogPtr,
                      const void* headerBuffer, size_t hbSize);

/* Флаг режима — обычная статическая переменная, НЕ thread_local: в фоновых потоках на консоли блока TLS нет
 * (tp = 0), запись в _Thread_local падала Data Abort по адресу 0x18 — вылет при открытии игры с ZBIC
 * в фоновом потоке. Распаковки сериализует замок в zbic_decompress (core/src/zbic.cpp). */
static int g_mk_bic = 0;
static int g_mk_bic_calls = 0;

void mk_zbic_set_bic(int on) { g_mk_bic = on; g_mk_bic_calls = 0; }
int mk_zbic_bic_calls(void) { return g_mk_bic_calls; }

/* (middle, first, last): для отрезка [f, l] — середина, затем ПРАВАЯ половина, затем ЛЕВАЯ;
 * последняя запись (0,0,1) — вырожденная. */
static uint16_t g_bic_table[256][3];
static int g_bic_n = 0;

static void bic_walk(int f, int l) {
    if (l - f < 2) return;
    int m = (f + l) / 2;
    g_bic_table[g_bic_n][0] = (uint16_t)m;
    g_bic_table[g_bic_n][1] = (uint16_t)f;
    g_bic_table[g_bic_n][2] = (uint16_t)l;
    g_bic_n++;
    bic_walk(m, l);
    bic_walk(f, m);
}

void mk_bic_table_init(void) {
    if (g_bic_n) return;
    bic_walk(0, 0x100);
    g_bic_table[g_bic_n][0] = 0;
    g_bic_table[g_bic_n][1] = 0;
    g_bic_table[g_bic_n][2] = 1;
    g_bic_n++;
}

int mk_bic_table_get(int i, int* mid, int* first, int* last) {
    if (i < 0 || i >= g_bic_n) return 0;
    *mid = g_bic_table[i][0];
    *first = g_bic_table[i][1];
    *last = g_bic_table[i][2];
    return 1;
}

#define MK_CORRUPT ((size_t) - (int)ZSTD_error_corruption_detected)

static int16_t mk_i16(int64_t v) {
    uint16_t u = (uint16_t)(v & 0xFFFF);
    return (int16_t)u;
}

/* Перевод read_ncount_bic шаг в шаг. Возвращает число занятых байт или код ошибки zstd. */
size_t mk_read_ncount_bic(short* ncount, unsigned* maxSVPtr, unsigned* tableLogPtr,
                          const void* src, size_t hb) {
    const uint8_t* buf = (const uint8_t*)src;
    unsigned max_sv_in = *maxSVPtr;
    if (hb == 0) return MK_CORRUPT;
    uint8_t b0 = buf[0];
    size_t raw = b0 & 0x7F;
    int ulpc = b0 >> 7;
    if (raw >= hb) return MK_CORRUPT;
    size_t data_size = raw + 1;
    if (data_size >= hb) return MK_CORRUPT;

#define FEED(val_in, out)                                       \
    do {                                                        \
        uint64_t v_ = (val_in);                                 \
        while (raw) {                                           \
            if ((v_ >> 32) >= 0x100) break;                     \
            v_ = (uint64_t)buf[raw] | (v_ << 8);                \
            raw -= 1;                                           \
        }                                                       \
        (out) = v_;                                             \
    } while (0)

    uint64_t i, enc, char_table, l;
    FEED(0, i);
    FEED(i / 0x34, enc);
    uint64_t j = i % 0x34;
    uint64_t char_num = j + 1;
    if (char_num > max_sv_in) return MK_CORRUPT;
    FEED(enc >> 3, char_table);
    uint64_t k = enc & 7;
    unsigned table_log = (unsigned)(k + 5);
    int64_t remaining = (int64_t)1 << table_log;
    FEED(char_table / (uint64_t)remaining, l);

    int64_t char_last = (int64_t)(char_table % (uint64_t)remaining) + 1;
    if (ulpc) char_last = (int64_t)char_num + (int64_t)(char_table % (uint64_t)remaining) + 2;

    uint64_t np2 = char_num;
    np2 |= np2 >> 1;
    np2 |= np2 >> 2;
    np2 |= np2 >> 4;
    np2 |= np2 >> 8;
    np2 |= np2 >> 16;
    np2 += 1;
    if (np2 > 0xFF) return MK_CORRUPT;

    int64_t bc[257];
    memset(bc, 0, sizeof(bc));
    bc[np2] = char_last;

    if (np2 != 0xFF) {
        uint64_t cnt = 0;
        for (;;) {
            int idx = (int)(cnt - np2 + 0x100);
            if (idx < 0 || idx >= g_bic_n) return MK_CORRUPT;
            int mid = g_bic_table[idx][0], first = g_bic_table[idx][1], last = g_bic_table[idx][2];
            int64_t lo = bc[first], hi = bc[last];
            if (lo == hi) {
                for (int x = first + 1; x < last; x++) bc[x] = lo;
            } else {
                int64_t span = hi - lo + 1;
                if (span <= 0) return MK_CORRUPT;   /* у годного потока не бывает */
                int64_t entry = (int64_t)(l % (uint64_t)span);
                FEED(l / (uint64_t)span, l);
                bc[mid] = entry + lo;
            }
            cnt += 1;
            if (cnt >= np2) break;
        }
    }

    for (unsigned s = 0; s <= max_sv_in; s++) ncount[s] = 0;
    if (char_num != 0xFF) {
        int64_t acc = 0;
        int p = 1;
        for (uint64_t s = 0; s < char_num + 1; s++) {
            int16_t cur = mk_i16(bc[p] & 0xFFFF);
            p++;
            int16_t dist = mk_i16((int64_t)cur - acc);
            int16_t count = mk_i16((int64_t)dist - ulpc);
            acc += dist;
            if (s <= max_sv_in) ncount[s] = count;
            else return MK_CORRUPT;
            remaining -= count >= 0 ? count : -count;
        }
    }
    if (remaining) return MK_CORRUPT;
    if (raw) return MK_CORRUPT;
    *maxSVPtr = (unsigned)char_num;
    *tableLogPtr = table_log;
    g_mk_bic_calls++;
    return data_size;
#undef FEED
}

size_t FSE_readNCount_bmi2(short* normalizedCounter, unsigned* maxSVPtr, unsigned* tableLogPtr,
                           const void* headerBuffer, size_t hbSize, int bmi2) {
    if (g_mk_bic) return mk_read_ncount_bic(normalizedCounter, maxSVPtr, tableLogPtr, headerBuffer, hbSize);
    return mkzu_std_FSE_readNCount_bmi2(normalizedCounter, maxSVPtr, tableLogPtr, headerBuffer, hbSize, bmi2);
}

size_t FSE_readNCount(short* normalizedCounter, unsigned* maxSVPtr, unsigned* tableLogPtr,
                      const void* headerBuffer, size_t hbSize) {
    return FSE_readNCount_bmi2(normalizedCounter, maxSVPtr, tableLogPtr, headerBuffer, hbSize, 0);
}
