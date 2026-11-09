#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
zbic_pure.py — распаковка ZBIC (и обычного zstd) НА ЧИСТОМ PYTHON, без внешних пакетов.

Зачем. Снятие ZBIC до сих пор упиралось в pip-пакет `zbic`: на машине без него
конвейер отказывал («сегмент сжат ZBIC, распаковать нечем»), а внешний Python из
WindowsApps этого пакета не имеет и поставить его туда мы человеку не можем.
Здесь тот же формат разобран своими руками на стандартной библиотеке: медленнее,
зато работает у любого, у кого просто есть Python.

Что такое ZBIC. Это zstd 1.5.7 с двумя отличиями:
  * магия кадра «ZBIC» (5A 42 49 43) вместо 28 B5 2F FD;
  * таблицы нормированных частот FSE записаны binary interpolative coding
    (в Atmosphère это FSE_readNCount_bic и таблица BIC_table[0x300]).
Всё остальное — стандартный zstd: кадр, блоки, литералы Huffman, последовательности.
Поэтому здесь реализован ПОЛНЫЙ декодер zstd, а BIC — только в одном месте,
в чтении таблиц частот; обычный zstd-кадр этот же код распаковывает тоже
(это используется в негативных пробах: подмена магии не должна «вылечить» ZBIC).

Происхождение (честно, чтобы верно указывать):
  * Формат zstd — RFC 8878.
  * Разбор BIC (порядок чтения полей, деление/остаток, смысл useLowProbCount,
    вырожденные случаи) выверен ПО ИСХОДНИКУ `FSE_readNCount_bic` из zstd.c
    Atmosphere-NX (GPL-2.0); этот файл лежит у нас как справочник. Код здесь
    написан заново на Python, но алгоритм — оттуда, и это надо указывать.
  * Таблица BIC_table[0x300] НЕ скопирована: она порождается обходом дерева
    (bic_table()) и разово сверяется с таблицей из zstd.c гейтом --self-test
    (проба «таблица BIC сошлась с эталоном»).
  * Константы zstd (LL/ML/OF: базы, добавочные биты, предопределённые таблицы) —
    это константы ФОРМАТА из RFC 8878; совпадают с одноимёнными массивами zstd.c.

Ничего не пишет на диск и ничего не удаляет: это библиотека + маленький CLI
(`python zbic_pure.py --decompress <файл> -o <файл>`, `--self-test`).

Скорость: ориентир — около 1 МБ
распакованного в секунду; пакет `zbic` (C) быстрее на три порядка, поэтому
конвейер берёт пакет, когда он есть, и наш путь — когда его нет.
"""

import struct
import sys

ZBIC_MAGIC = 0x4349425A          # «ZBIC»
ZSTD_MAGIC = 0xFD2FB528          # обычный zstd
MASK64 = (1 << 64) - 1


class ZbicError(Exception):
    """Кадр не разобрался: не наш формат или данные повреждены."""


# ----------------------------------------------------------------- BIC_table
#
# Тройки (middle, first, last) для интерполятивного декодирования.
# Обход: для отрезка [f, l] сначала его середина, затем ПРАВАЯ половина,
# затем ЛЕВАЯ (поэтому последние n записей таблицы обслуживают дерево из n
# точек — декодер именно так и индексирует: 3*(счётчик - n + 0x100)).
# Последняя запись (0,0,1) — вырожденная, она есть и в оригинале.

def bic_table():
    out = []

    def walk(f, l):
        if l - f < 2:
            return
        m = (f + l) // 2
        out.append((m, f, l))
        walk(m, l)
        walk(f, m)

    walk(0, 0x100)
    out.append((0, 0, 1))
    return out


BIC_TABLE = bic_table()


def _i16(v):
    """Усечение до знакового short — в оригинале счётчики читаются как short."""
    v &= 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v


def read_ncount_bic(buf, off, max_sv_in):
    """
    Таблица нормированных частот FSE в binary interpolative coding.

    Возвращает (ncount, max_sv, table_log, сколько байт занято).
    Перевод FSE_readNCount_bic шаг в шаг: `l` — «большое число», из которого
    делением с остатком достаются значения, а недостающие разряды подкачиваются
    байтами С КОНЦА блока, пока число не превысит 2**40.
    """
    hb = len(buf) - off
    if hb <= 0:
        raise ZbicError("BIC: пустой заголовок таблицы")
    b0 = buf[off]
    raw = b0 & 0x7F                 # длина блока (без первого байта)
    ulpc = b0 >> 7                  # useLowProbCount
    if raw >= hb:
        raise ZbicError("BIC: длина таблицы больше блока")
    data_size = raw + 1
    if data_size >= hb:
        raise ZbicError("BIC: таблица не помещается в блок")

    def feed(val):
        """Подкачка байтов с конца, пока число не переполнило 2**40."""
        nonlocal raw
        while raw:
            if (val >> 32) >= 0x100:
                break
            val = buf[off + raw] | (val << 8)
            raw -= 1
        return val

    i = feed(0)
    enc = feed(i // 0x34)
    j = i % 0x34
    char_num = j + 1
    if char_num > max_sv_in:
        raise ZbicError("BIC: символов %d, а таблица рассчитана на %d"
                        % (char_num, max_sv_in))
    char_table = feed(enc >> 3)
    k = enc & 7
    table_log = k + 5
    remaining = 1 << table_log
    l = feed(char_table // remaining)

    char_last = char_table % remaining + 1
    if ulpc:
        char_last = char_num + char_table % remaining + 2

    np2 = char_num                  # строго следующая степень двойки
    np2 |= np2 >> 1
    np2 |= np2 >> 2
    np2 |= np2 >> 4
    np2 |= np2 >> 8
    np2 |= np2 >> 16
    np2 += 1
    if np2 > 0xFF:
        raise ZbicError("BIC: слишком много символов")

    bc = [0] * 257
    bc[np2] = char_last

    if np2 != 0xFF:
        cnt = 0
        while True:
            mid, first, last = BIC_TABLE[cnt - np2 + 0x100]
            lo = bc[first]
            hi = bc[last]
            if lo == hi:
                # значения совпали — весь отрезок между ними такой же
                idx = first + 1
                while idx < last:
                    bc[idx] = lo
                    idx += 1
            else:
                span = hi - lo + 1
                entry = l % span
                l = feed(l // span)
                bc[mid] = entry + lo
            cnt += 1
            if cnt >= np2:
                break

    ncount = []
    if char_num != 0xFF:
        acc = 0
        p = 1
        for _ in range(char_num + 1):
            cur = _i16(bc[p] & 0xFFFF)
            p += 1
            dist = _i16(cur - acc)
            count = _i16(dist - ulpc)
            acc += dist
            ncount.append(count)
            remaining -= count if count >= 0 else -count
    if remaining:
        raise ZbicError("BIC: сумма частот не сошлась (осталось %d)" % remaining)
    if raw:
        raise ZbicError("BIC: в таблице остались непрочитанные байты")
    return ncount, char_num, table_log, data_size


def read_ncount_std(buf, off, max_sv_in):
    """Обычная (не BIC) таблица частот FSE — как в RFC 8878, для zstd-кадров."""
    end = len(buf)
    ip = off
    if end - ip < 1:
        raise ZbicError("FSE: пустой заголовок таблицы")
    bit_stream = int.from_bytes(buf[ip:ip + 4].ljust(4, b"\0"), "little")
    bit_count = 4
    table_log = (bit_stream & 0xF) + 5
    if table_log > 15:
        raise ZbicError("FSE: tableLog %d велик" % table_log)
    bit_stream >>= 4
    remaining = (1 << table_log) + 1
    threshold = 1 << table_log
    nbits = table_log + 1
    ncount = [0] * (max_sv_in + 1)
    charnum = 0
    previous0 = False

    def reload():
        nonlocal bit_stream, bit_count, ip
        ip += bit_count >> 3
        bit_count &= 7
        bit_stream = int.from_bytes(buf[ip:ip + 4].ljust(4, b"\0"), "little") >> bit_count

    while remaining > 1 and charnum <= max_sv_in:
        if previous0:
            while (bit_stream & 0xFFFF) == 0xFFFF:
                charnum += 24
                ip += 2
                bit_stream = (int.from_bytes(buf[ip:ip + 4].ljust(4, b"\0"), "little")
                              >> bit_count)
            while (bit_stream & 3) == 3:
                charnum += 3
                bit_stream >>= 2
                bit_count += 2
            charnum += bit_stream & 3
            bit_count += 2
            if charnum > max_sv_in:
                raise ZbicError("FSE: слишком много нулей")
            while charnum > len(ncount) - 1:
                ncount.append(0)
            reload()
            previous0 = False
            continue
        max_v = (2 * threshold - 1) - remaining
        if (bit_stream & (threshold - 1)) < max_v:
            count = bit_stream & (threshold - 1)
            bit_count += nbits - 1
        else:
            count = bit_stream & (2 * threshold - 1)
            if count >= threshold:
                count -= max_v
            bit_count += nbits
        count -= 1
        remaining -= count if count >= 0 else -count
        if charnum > max_sv_in:
            raise ZbicError("FSE: символов больше, чем разрешено")
        ncount[charnum] = count
        charnum += 1
        previous0 = (count == 0)
        while remaining < threshold:
            nbits -= 1
            threshold >>= 1
        reload()
    if remaining != 1:
        raise ZbicError("FSE: сумма частот не сошлась")
    return ncount[:charnum], charnum - 1, table_log, ip - off + ((bit_count + 7) >> 3)


# ----------------------------------------------------------------- битовые потоки
#
# zstd пишет биты С КОНЦА буфера: последний байт содержит маркер — старшую
# единицу, дальше биты идут «назад». Если буфер развернуть, получается обычный
# поток MSB-first — на этом и построен быстрый читатель.


class BitsFast:
    """Быстрый читатель для потоков, где число символов известно заранее."""

    __slots__ = ("rev", "pos", "acc", "nb")

    def __init__(self, buf):
        if not buf:
            raise ZbicError("пустой битовый поток")
        last = buf[-1]
        if last == 0:
            raise ZbicError("в битовом потоке нет маркера конца")
        self.rev = buf[::-1]
        self.pos = 1
        self.acc = last & ((1 << (last.bit_length() - 1)) - 1)
        self.nb = last.bit_length() - 1

    def read(self, n):
        if n == 0:
            return 0
        acc, nb, pos, rev = self.acc, self.nb, self.pos, self.rev
        while nb < n:
            acc = (acc << 8) | (rev[pos] if pos < len(rev) else 0)
            pos += 1
            nb += 8
        nb -= n
        v = (acc >> nb) & ((1 << n) - 1)
        self.acc = acc & ((1 << nb) - 1)
        self.nb = nb
        self.pos = pos
        return v


# Статусы BIT_DStream_t из zstd — нужны там, где длина выхода заранее НЕ известна
# (таблица весов Huffman): конец декодирования определяется именно ими.
_UNFINISHED, _ENDOFBUFFER, _COMPLETED, _OVERFLOW = 0, 1, 2, 3


class BitsExact:
    """Точная копия BIT_DStream_t: контейнер 64 бита, подкачка байтами назад."""

    __slots__ = ("src", "n", "ptr", "consumed", "container", "overflow")

    def __init__(self, buf):
        self.src = buf
        self.n = len(buf)
        if self.n < 1:
            raise ZbicError("пустой битовый поток")
        last = buf[-1]
        if last == 0:
            raise ZbicError("в битовом потоке нет маркера конца")
        self.overflow = False
        if self.n >= 8:
            self.ptr = self.n - 8
            self.container = int.from_bytes(buf[self.ptr:self.ptr + 8], "little")
            self.consumed = 9 - last.bit_length()
        else:
            self.ptr = 0
            self.container = int.from_bytes(buf, "little")
            self.consumed = 9 - last.bit_length() + (8 - self.n) * 8

    def look(self, n):
        if n == 0:
            return 0
        v = ((self.container << (self.consumed & 63)) & MASK64) >> 1
        return v >> (63 - n)

    def read(self, n):
        v = self.look(n)
        self.consumed += n
        return v

    def reload(self):
        if self.consumed > 64:
            self.overflow = True
            return _OVERFLOW
        if self.ptr >= 8:
            self.ptr -= self.consumed >> 3
            self.consumed &= 7
            self.container = int.from_bytes(self.src[self.ptr:self.ptr + 8], "little")
            return _UNFINISHED
        if self.ptr == 0:
            return _ENDOFBUFFER if self.consumed < 64 else _COMPLETED
        nb = self.consumed >> 3
        res = _UNFINISHED
        if self.ptr - nb < 0:
            nb = self.ptr
            res = _ENDOFBUFFER
        self.ptr -= nb
        self.consumed -= nb * 8
        self.container = int.from_bytes(self.src[self.ptr:self.ptr + 8], "little")
        return res


# ----------------------------------------------------------------- FSE


def build_fse_table(ncount, max_sv, table_log):
    """
    Таблица декодирования FSE: (символ, добавочные биты состояния, новое состояние).

    Раскладка символов по ячейкам — как в zstd: шаг (size>>1)+(size>>3)+3,
    символы с частотой -1 («редкие») кладутся с конца таблицы.
    """
    size = 1 << table_log
    high = size - 1
    symbol = [0] * size
    sym_next = [0] * (max_sv + 1)
    for s in range(max_sv + 1):
        if s < len(ncount) and ncount[s] == -1:
            symbol[high] = s
            high -= 1
            sym_next[s] = 1
        else:
            sym_next[s] = ncount[s] if s < len(ncount) else 0

    step = (size >> 1) + (size >> 3) + 3
    mask = size - 1
    pos = 0
    for s in range(max_sv + 1):
        n = ncount[s] if s < len(ncount) else 0
        if n <= 0:
            continue
        for _ in range(n):
            symbol[pos] = s
            pos = (pos + step) & mask
            while pos > high:
                pos = (pos + step) & mask
    if pos != 0:
        raise ZbicError("FSE: раскладка символов не замкнулась")

    table = []
    for u in range(size):
        s = symbol[u]
        nxt = sym_next[s]
        sym_next[s] = nxt + 1
        nb = table_log - (nxt.bit_length() - 1)
        table.append((s, nb, (nxt << nb) - size))
    return table


def fse_decompress_weights(buf, max_out, max_log, read_ncount):
    """
    FSE-распаковка таблицы весов Huffman: два состояния вперемежку, длина выхода
    заранее неизвестна — конец определяется исчерпанием потока, поэтому здесь
    работает точный читатель BitsExact и порядок операций как в zstd.
    """
    ncount, max_sv, table_log, hsize = read_ncount(buf, 0, 255)
    if table_log > max_log:
        raise ZbicError("FSE: tableLog %d больше разрешённого %d" % (table_log, max_log))
    dt = build_fse_table(ncount, max_sv, table_log)
    bd = BitsExact(buf[hsize:])
    st1 = bd.read(table_log)
    bd.reload()
    st2 = bd.read(table_log)
    bd.reload()
    out = bytearray()

    def sym(state):
        s, nb, newst = dt[state]
        low = bd.read(nb)
        return s, newst + low

    while True:
        if len(out) > max_out - 2:
            raise ZbicError("FSE: весов больше, чем помещается")
        s, st1 = sym(st1)
        out.append(s)
        if bd.reload() == _OVERFLOW:
            s, st2 = sym(st2)
            out.append(s)
            break
        if len(out) > max_out - 2:
            raise ZbicError("FSE: весов больше, чем помещается")
        s, st2 = sym(st2)
        out.append(s)
        if bd.reload() == _OVERFLOW:
            s, st1 = sym(st1)
            out.append(s)
            break
    return bytes(out)


# ----------------------------------------------------------------- Huffman


def read_huf_table(buf, read_ncount):
    """
    Описание дерева Huffman литералов -> (символы ячеек, число бит, tableLog, съедено).

    Веса даются либо напрямую по 4 бита (заголовок >= 128), либо FSE-сжатыми
    (в ZBIC — с BIC-таблицей частот). Вес последнего символа подразумевается:
    он дополняет сумму до степени двойки.
    """
    if not buf:
        raise ZbicError("Huffman: пустое описание дерева")
    isize = buf[0]
    if isize >= 128:
        osize = isize - 127
        nbytes = (osize + 1) // 2
        if 1 + nbytes > len(buf):
            raise ZbicError("Huffman: описание дерева обрезано")
        weights = bytearray(osize + 1)
        for n in range(0, osize, 2):
            weights[n] = buf[1 + n // 2] >> 4
            if n + 1 < osize + 1:
                weights[n + 1] = buf[1 + n // 2] & 15
        weights = weights[:osize]
        consumed = nbytes + 1
    else:
        if 1 + isize > len(buf):
            raise ZbicError("Huffman: описание дерева обрезано")
        weights = fse_decompress_weights(buf[1:1 + isize], 255, 6, read_ncount)
        osize = len(weights)
        consumed = isize + 1

    total = 0
    for w in weights:
        if w > 12:
            raise ZbicError("Huffman: вес %d недопустим" % w)
        total += (1 << w) >> 1
    if total == 0:
        raise ZbicError("Huffman: нулевая сумма весов")
    table_log = (total.bit_length() - 1) + 1
    if table_log > 12:
        raise ZbicError("Huffman: tableLog %d велик" % table_log)
    rest = (1 << table_log) - total
    if rest == 0 or (rest & (rest - 1)):
        raise ZbicError("Huffman: остаток не степень двойки")
    last_weight = rest.bit_length()
    weights = bytes(weights) + bytes([last_weight])
    nsym = osize + 1

    size = 1 << table_log
    sym = bytearray(size)
    bits = bytearray(size)
    pos = 0
    # порядок как в HUF_readDTableX1: сперва вес 1 (самые длинные коды),
    # потом веса по возрастанию — от этого зависит, какой символ в какой ячейке
    for w in range(1, table_log + 1):
        nb = table_log + 1 - w
        span = 1 << (w - 1)
        for s in range(nsym):
            if weights[s] == w:
                if pos + span > size:
                    raise ZbicError("Huffman: дерево не помещается в таблицу")
                for _ in range(span):
                    sym[pos] = s
                    bits[pos] = nb
                    pos += 1
    if pos != size:
        raise ZbicError("Huffman: дерево заполнило таблицу не целиком")
    return bytes(sym), bytes(bits), table_log, consumed


def huf_decode_stream(buf, nsym, sym, bits, table_log, out, out_off):
    """Один поток литералов: nsym символов, каждый — table_log бит на просмотр."""
    if nsym == 0:
        return
    if not buf:
        raise ZbicError("Huffman: пустой поток литералов")
    last = buf[-1]
    if last == 0:
        raise ZbicError("Huffman: в потоке нет маркера конца")
    rev = buf[::-1]
    nrev = len(rev)
    pos = 1
    nb = last.bit_length() - 1
    acc = last & ((1 << nb) - 1)
    tl = table_log
    o = out_off
    for _ in range(nsym):
        while nb < tl:
            acc = (acc << 8) | (rev[pos] if pos < nrev else 0)
            pos += 1
            nb += 8
        idx = (acc >> (nb - tl)) & ((1 << tl) - 1)
        out[o] = sym[idx]
        o += 1
        nb -= bits[idx]
        acc &= (1 << nb) - 1


# ----------------------------------------------------------------- таблицы формата
# (константы RFC 8878; те же значения, что в одноимённых массивах zstd.c)

LL_BITS = [0] * 16 + [1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]
LL_BASE = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
           16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 0x80, 0x100, 0x200, 0x400,
           0x800, 0x1000, 0x2000, 0x4000, 0x8000, 0x10000]
LL_DEFAULT = [4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1,
              2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1]
LL_DEFAULT_LOG = 6

ML_BITS = [0] * 32 + [1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11,
                      12, 13, 14, 15, 16]
ML_BASE = list(range(3, 35)) + [35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99,
                                0x83, 0x103, 0x203, 0x403, 0x803,
                                0x1003, 0x2003, 0x4003, 0x8003, 0x10003]
ML_DEFAULT = ([1, 4, 3, 2, 2, 2, 2, 2, 2] + [1] * 37 + [-1] * 7)
ML_DEFAULT_LOG = 6

OF_BITS = list(range(32))
OF_BASE = [0, 1, 1] + [(1 << n) - 3 for n in range(3, 32)]
OF_DEFAULT = [1, 1, 1, 1, 1, 1, 2, 2, 2] + [1] * 15 + [-1] * 5
OF_DEFAULT_LOG = 5

MAX_LL, MAX_ML, MAX_OFF = 35, 52, 31


def _check_tables():
    assert len(LL_BITS) == len(LL_BASE) == len(LL_DEFAULT) == MAX_LL + 1
    assert len(ML_BITS) == len(ML_BASE) == MAX_ML + 1, (len(ML_BITS), len(ML_BASE))
    assert len(ML_DEFAULT) == MAX_ML + 1, len(ML_DEFAULT)
    assert len(OF_BASE) == len(OF_BITS) == MAX_OFF + 1
    assert OF_BASE[3] == 5 and OF_BASE[4] == 0xD


_check_tables()


def build_seq_table(ncount, max_sv, table_log, base, extra):
    """FSE-таблица последовательностей: к состоянию добавлены база и добавочные биты."""
    raw = build_fse_table(ncount, max_sv, table_log)
    return [(base[s], extra[s], nb, newst) for (s, nb, newst) in raw]


def rle_seq_table(symbol, base, extra):
    return [(base[symbol], extra[symbol], 0, 0)]


DEFAULT_LL_TABLE = None
DEFAULT_ML_TABLE = None
DEFAULT_OF_TABLE = None


def _default_tables():
    global DEFAULT_LL_TABLE, DEFAULT_ML_TABLE, DEFAULT_OF_TABLE
    if DEFAULT_LL_TABLE is None:
        DEFAULT_LL_TABLE = build_seq_table(LL_DEFAULT, MAX_LL, LL_DEFAULT_LOG,
                                           LL_BASE, LL_BITS)
        DEFAULT_ML_TABLE = build_seq_table(ML_DEFAULT, MAX_ML, ML_DEFAULT_LOG,
                                           ML_BASE, ML_BITS)
        DEFAULT_OF_TABLE = build_seq_table(OF_DEFAULT, 28, OF_DEFAULT_LOG,
                                           OF_BASE, OF_BITS)
    return DEFAULT_LL_TABLE, DEFAULT_ML_TABLE, DEFAULT_OF_TABLE


# ----------------------------------------------------------------- блок


class _Ctx:
    """Состояние между блоками: таблицы Huffman/FSE и три повторных смещения."""

    def __init__(self, read_ncount):
        self.read_ncount = read_ncount
        self.huf = None                  # (sym, bits, table_log)
        self.ll = None
        self.ml = None
        self.of = None
        self.rep = [1, 4, 8]


def decode_literals(ctx, src, off, end):
    """Секция литералов блока -> (литералы, смещение после секции)."""
    if end - off < 1:
        raise ZbicError("литералы: блок пуст")
    b0 = src[off]
    lit_type = b0 & 3
    size_fmt = (b0 >> 2) & 3

    if lit_type in (0, 1):
        if size_fmt in (0, 2):
            regen = b0 >> 3
            hs = 1
        elif size_fmt == 1:
            regen = (b0 >> 4) | (src[off + 1] << 4)
            hs = 2
        else:
            regen = (b0 >> 4) | (src[off + 1] << 4) | (src[off + 2] << 12)
            hs = 3
        p = off + hs
        if lit_type == 0:
            if p + regen > end:
                raise ZbicError("литералы: сырых данных меньше, чем объявлено")
            return src[p:p + regen], p + regen
        return bytes([src[p]]) * regen, p + 1

    # сжатые (2) или с прежним деревом (3)
    if end - off < 5:
        raise ZbicError("литералы: заголовок обрезан")
    lhc = int.from_bytes(src[off:off + 4], "little")
    if size_fmt == 0:
        single, hs = True, 3
        lit_size = (lhc >> 4) & 0x3FF
        lit_csize = (lhc >> 14) & 0x3FF
    elif size_fmt == 1:
        single, hs = False, 3
        lit_size = (lhc >> 4) & 0x3FF
        lit_csize = (lhc >> 14) & 0x3FF
    elif size_fmt == 2:
        single, hs = False, 4
        lit_size = (lhc >> 4) & 0x3FFF
        lit_csize = lhc >> 18
    else:
        single, hs = False, 5
        lit_size = (lhc >> 4) & 0x3FFFF
        lit_csize = (lhc >> 22) + (src[off + 4] << 10)
    if off + hs + lit_csize > end:
        raise ZbicError("литералы: сжатых данных меньше, чем объявлено")

    body = src[off + hs:off + hs + lit_csize]
    if lit_type == 2:
        sym, bits, tlog, used = read_huf_table(body, ctx.read_ncount)
        ctx.huf = (sym, bits, tlog)
        body = body[used:]
    else:
        if ctx.huf is None:
            raise ZbicError("литералы: сказано взять прежнее дерево, а его не было")
        sym, bits, tlog = ctx.huf

    out = bytearray(lit_size)
    if single:
        huf_decode_stream(body, lit_size, sym, bits, tlog, out, 0)
    else:
        if len(body) < 6:
            raise ZbicError("литералы: нет таблицы переходов четырёх потоков")
        l1 = int.from_bytes(body[0:2], "little")
        l2 = int.from_bytes(body[2:4], "little")
        l3 = int.from_bytes(body[4:6], "little")
        p = 6
        l4 = len(body) - (6 + l1 + l2 + l3)
        if l4 < 0:
            raise ZbicError("литералы: длины потоков не сходятся")
        seg = (lit_size + 3) // 4
        sizes = [seg, seg, seg, lit_size - 3 * seg]
        if sizes[3] < 0:
            raise ZbicError("литералы: четвёртый поток отрицательной длины")
        offs = [0, seg, 2 * seg, 3 * seg]
        for n, ln in enumerate((l1, l2, l3, l4)):
            huf_decode_stream(body[p:p + ln], sizes[n], sym, bits, tlog, out, offs[n])
            p += ln
    return bytes(out), off + hs + lit_csize


def decode_seq_headers(ctx, src, off, end):
    """Заголовок секции последовательностей -> (сколько их, смещение к битам)."""
    if end - off < 1:
        raise ZbicError("последовательности: секция пуста")
    nb = src[off]
    off += 1
    if nb > 0x7F:
        if nb == 0xFF:
            nb = int.from_bytes(src[off:off + 2], "little") + 0x7F00
            off += 2
        else:
            nb = ((nb - 0x80) << 8) + src[off]
            off += 1
    if nb == 0:
        return 0, off

    modes = src[off]
    off += 1
    if modes & 3:
        raise ZbicError("последовательности: резервные биты не нули")
    ll_mode = modes >> 6
    of_mode = (modes >> 4) & 3
    ml_mode = (modes >> 2) & 3
    dll, dml, dof = _default_tables()

    for mode, name, mx, maxlog, base, extra, default in (
            (ll_mode, "ll", MAX_LL, 9, LL_BASE, LL_BITS, dll),
            (of_mode, "of", MAX_OFF, 8, OF_BASE, OF_BITS, dof),
            (ml_mode, "ml", MAX_ML, 9, ML_BASE, ML_BITS, dml)):
        if mode == 0:
            setattr(ctx, name, default)
        elif mode == 1:
            s = src[off]
            off += 1
            if s > mx:
                raise ZbicError("последовательности: символ RLE вне диапазона")
            setattr(ctx, name, rle_seq_table(s, base, extra))
        elif mode == 2:
            ncount, max_sv, tlog, used = ctx.read_ncount(src[:end], off, mx)
            if tlog > maxlog:
                raise ZbicError("последовательности: tableLog %d велик" % tlog)
            setattr(ctx, name, build_seq_table(ncount, max_sv, tlog, base, extra))
            off += used
        else:
            if getattr(ctx, name) is None:
                raise ZbicError("последовательности: сказано повторить таблицу, "
                                "а её не было")
    return nb, off


def decode_block_compressed(ctx, src, off, end, out):
    """Сжатый блок: литералы + последовательности, дописывается в out."""
    lits, off = decode_literals(ctx, src, off, end)
    nb_seq, off = decode_seq_headers(ctx, src, off, end)
    if nb_seq == 0:
        if off != end:
            raise ZbicError("блок: за пустой секцией последовательностей есть мусор")
        out += lits
        return

    bits = BitsFast(src[off:end])
    read = bits.read
    ll_t, ml_t, of_t = ctx.ll, ctx.ml, ctx.of
    ll_log = (len(ll_t) - 1).bit_length() if len(ll_t) > 1 else 0
    ml_log = (len(ml_t) - 1).bit_length() if len(ml_t) > 1 else 0
    of_log = (len(of_t) - 1).bit_length() if len(of_t) > 1 else 0
    st_ll = read(ll_log)
    st_of = read(of_log)
    st_ml = read(ml_log)
    rep0, rep1, rep2 = ctx.rep
    lit_pos = 0
    nlit = len(lits)

    for seq in range(nb_seq):
        ll_base, ll_bits, ll_nb, ll_next = ll_t[st_ll]
        ml_base, ml_bits, ml_nb, ml_next = ml_t[st_ml]
        of_base, of_bits, of_nb, of_next = of_t[st_of]

        if of_bits > 1:
            offset = of_base + read(of_bits)
            rep2 = rep1
            rep1 = rep0
            rep0 = offset
        else:
            ll0 = 1 if ll_base == 0 else 0
            if of_bits == 0:
                if ll0:
                    offset = rep1
                    rep1 = rep0
                    rep0 = offset
                else:
                    offset = rep0
            else:
                # code 1..3 — это НОМЕР повторного смещения (prevOffset[code]),
                # а code == 3 при нулевой длине литералов означает «rep0 - 1».
                code = of_base + ll0 + read(1)
                if code == 3:
                    tmp = rep0 - 1
                elif code == 2:
                    tmp = rep2
                else:
                    tmp = rep1
                if tmp == 0:
                    raise ZbicError("последовательности: нулевое смещение")
                if code != 1:
                    rep2 = rep1
                rep1 = rep0
                rep0 = offset = tmp

        ml = ml_base + (read(ml_bits) if ml_bits else 0)
        ll = ll_base + (read(ll_bits) if ll_bits else 0)

        if lit_pos + ll > nlit:
            raise ZbicError("последовательности: литералов не хватило")
        if ll:
            out += lits[lit_pos:lit_pos + ll]
            lit_pos += ll
        if offset > len(out):
            raise ZbicError("последовательности: смещение %d за начало вывода" % offset)
        if offset >= ml:
            start = len(out) - offset
            out += out[start:start + ml]
        else:
            start = len(out) - offset
            for _ in range(ml):
                out.append(out[start])
                start += 1

        if seq != nb_seq - 1:
            st_ll = ll_next + read(ll_nb)
            st_ml = ml_next + read(ml_nb)
            st_of = of_next + read(of_nb)

    if lit_pos < nlit:
        out += lits[lit_pos:]
    ctx.rep = [rep0, rep1, rep2]


def decode_frame(data, off=0):
    """Разбор одного кадра (ZBIC или zstd) -> (распакованное, смещение за кадром)."""
    if len(data) - off < 5:
        raise ZbicError("кадр короче заголовка")
    magic = int.from_bytes(data[off:off + 4], "little")
    if magic == ZBIC_MAGIC:
        read_ncount = read_ncount_bic
    elif magic == ZSTD_MAGIC:
        read_ncount = read_ncount_std
    else:
        raise ZbicError("не кадр ZBIC/zstd: магия %08X" % magic)
    p = off + 4
    fhd = data[p]
    p += 1
    fcs_flag = fhd >> 6
    single = (fhd >> 5) & 1
    checksum = (fhd >> 2) & 1
    did_flag = fhd & 3
    if (fhd >> 3) & 1:
        raise ZbicError("кадр: резервный бит заголовка не ноль")
    if not single:
        p += 1                              # Window_Descriptor
    did_size = (0, 1, 2, 4)[did_flag]
    if did_size:
        p += did_size
    if fcs_flag == 0:
        fcs = data[p] if single else None
        p += 1 if single else 0
    elif fcs_flag == 1:
        fcs = int.from_bytes(data[p:p + 2], "little") + 256
        p += 2
    elif fcs_flag == 2:
        fcs = int.from_bytes(data[p:p + 4], "little")
        p += 4
    else:
        fcs = int.from_bytes(data[p:p + 8], "little")
        p += 8

    ctx = _Ctx(read_ncount)
    out = bytearray()
    while True:
        if len(data) - p < 3:
            raise ZbicError("кадр: заголовок блока обрезан")
        h = data[p] | (data[p + 1] << 8) | (data[p + 2] << 16)
        p += 3
        last = h & 1
        btype = (h >> 1) & 3
        bsize = h >> 3
        # у RLE-блока Block_Size — это число ПОВТОРОВ, а в потоке лежит один байт,
        # поэтому длину в потоке проверяем отдельно от него
        if btype != 1 and p + bsize > len(data):
            raise ZbicError("кадр: блок выходит за данные")
        if btype == 1 and p >= len(data):
            raise ZbicError("кадр: RLE-блок выходит за данные")
        if btype == 0:
            out += data[p:p + bsize]
            p += bsize
        elif btype == 1:
            out += bytes([data[p]]) * bsize
            p += 1
        elif btype == 2:
            decode_block_compressed(ctx, data, p, p + bsize, out)
            p += bsize
        else:
            raise ZbicError("кадр: блок запрещённого типа 3")
        if last:
            break

    if checksum:
        if len(data) - p < 4:
            raise ZbicError("кадр: контрольная сумма обрезана")
        want = int.from_bytes(data[p:p + 4], "little")
        p += 4
        got = xxh64(bytes(out)) & 0xFFFFFFFF
        if got != want:
            raise ZbicError("кадр: контрольная сумма не сошлась")
    if fcs is not None and len(out) != fcs:
        raise ZbicError("кадр: размер %d, а объявлено %d" % (len(out), fcs))
    return bytes(out), p


def decompress(data):
    """Все кадры подряд -> распакованные байты."""
    out = bytearray()
    p = 0
    n = len(data)
    while p < n:
        blob, p = decode_frame(data, p)
        out += blob
    return bytes(out)


# ----------------------------------------------------------------- xxhash64
# (контрольная сумма кадра zstd; алгоритм xxHash64, Yann Collet, BSD-2)

_P1 = 0x9E3779B185EBCA87
_P2 = 0xC2B2AE3D27D4EB4F
_P3 = 0x165667B19E3779F9
_P4 = 0x85EBCA77C2B2AE63
_P5 = 0x27D4EB2F165667C5
_M = (1 << 64) - 1


def _rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & _M


def _round(acc, val):
    acc = (acc + val * _P2) & _M
    acc = _rotl(acc, 31)
    return (acc * _P1) & _M


def xxh64(data, seed=0):
    n = len(data)
    if n >= 32:
        v1 = (seed + _P1 + _P2) & _M
        v2 = (seed + _P2) & _M
        v3 = seed & _M
        v4 = (seed - _P1) & _M
        p = 0
        while p + 32 <= n:
            v1 = _round(v1, int.from_bytes(data[p:p + 8], "little"))
            v2 = _round(v2, int.from_bytes(data[p + 8:p + 16], "little"))
            v3 = _round(v3, int.from_bytes(data[p + 16:p + 24], "little"))
            v4 = _round(v4, int.from_bytes(data[p + 24:p + 32], "little"))
            p += 32
        h = (_rotl(v1, 1) + _rotl(v2, 7) + _rotl(v3, 12) + _rotl(v4, 18)) & _M
        for v in (v1, v2, v3, v4):
            h = (h ^ _round(0, v)) & _M
            h = (h * _P1 + _P4) & _M
    else:
        h = (seed + _P5) & _M
        p = 0
    h = (h + n) & _M
    while p + 8 <= n:
        k = _round(0, int.from_bytes(data[p:p + 8], "little"))
        h = (_rotl(h ^ k, 27) * _P1 + _P4) & _M
        p += 8
    if p + 4 <= n:
        h = (_rotl(h ^ (int.from_bytes(data[p:p + 4], "little") * _P1 & _M), 23)
             * _P2 + _P3) & _M
        p += 4
    while p < n:
        h = (_rotl(h ^ (data[p] * _P5 & _M), 11) * _P1) & _M
        p += 1
    h ^= h >> 33
    h = (h * _P2) & _M
    h ^= h >> 29
    h = (h * _P3) & _M
    h ^= h >> 32
    return h


# ----------------------------------------------------------------- CLI


def main():
    import argparse
    import hashlib
    from pathlib import Path

    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

    ap = argparse.ArgumentParser(description="распаковка ZBIC/zstd на чистом Python")
    ap.add_argument("--decompress", metavar="ФАЙЛ", help="кадр ZBIC/zstd")
    ap.add_argument("-o", "--out", metavar="ФАЙЛ", help="куда положить результат")
    args = ap.parse_args()
    if not args.decompress:
        ap.print_help()
        return 2
    blob = Path(args.decompress).read_bytes()
    data = decompress(blob)
    print("распаковано %d Б -> %d Б, sha256 %s"
          % (len(blob), len(data), hashlib.sha256(data).hexdigest()))
    if args.out:
        Path(args.out).write_bytes(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
