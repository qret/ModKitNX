// ModKitNX core — переносимый AES-128 (ECB, CTR) без libnx.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Зачем в ядре: хост-тесты разбора NCA (синтетический NCA шифруется и расшифровывается тем же кодом) и
// запасной программный путь. На консоли быстрый CTR даёт платформа (libnx aes128Ctr* или spl ComputeCtr),
// ядро получает его через mk::nca::CtrFn. Ключи ядро не хранит и никуда не пишет.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mk::aes {

class Aes128 {
public:
    explicit Aes128(const uint8_t key[16]);
    ~Aes128();
    Aes128(const Aes128&) = delete;
    Aes128& operator=(const Aes128&) = delete;
    void encrypt_block(const uint8_t in[16], uint8_t out[16]) const;
    void decrypt_block(const uint8_t in[16], uint8_t out[16]) const;

private:
    uint8_t rk_[176];
};

// n кратно 16; in и out могут совпадать.
void ecb_encrypt(const uint8_t key[16], const uint8_t* in, uint8_t* out, size_t n);
void ecb_decrypt(const uint8_t key[16], const uint8_t* in, uint8_t* out, size_t n);
// CTR: iv — счётчик первого блока (big-endian 128 бит, как у Switch); n любое; in и out могут совпадать.
void ctr_crypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t n);
// AES-128-XTS с «Nintendo-tweak» (номер сектора big-endian, как заголовок NCA): key — 32 байта (ключ данных + ключ
// tweak), секторы по sector_size начиная с first_sector; n кратно 16, на месте. Хост-тесты форвардера (на консоли
// заголовок шифрует libnx aes128Xts*).
void xts_nintendo(const uint8_t key[32], uint64_t first_sector, size_t sector_size, uint8_t* data, size_t n,
                  bool encrypt);

}  // namespace mk::aes
