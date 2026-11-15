// ModKitNX core — форвардер (ярлык на домашнем экране): сборка трёх NCA, PFS0, romfs, NACP и cnmt без libnx.
// SPDX-License-Identifier: GPL-3.0-or-later
// Based on sphaira by ITotalJustice (GPL-3.0): sphaira/source/owo.cpp
//
// Форвардер — установленное приложение из трёх NCA (Р-43):
//   Program — exefs: загрузчик hbl (nx-hbloader в варианте sphaira, ISC; исходники — hbl/) и его main.npdm;
//             romfs: /nextArgv и /nextNroPath — что hbl запустит (наш .nro);
//   Control — romfs: control.nacp и icon_AmericanEnglish.dat (имя и иконка домашнего экрана);
//   Meta    — exefs-подобный PFS0 с cnmt: «эти два — одно приложение».
// Разделы без шифрования; заголовок NCA (0xC00) шифруется ключом заголовка консоли — его выводит spl, ядро ключа
// не видит: шифрует платформа (HeaderCryptFn). Порядок и раскладка — как у sphaira owo.cpp и проверенного на консоли
// порта forwarder.c (сверка побайтно — tests/host/forwarder_tests.cpp и docs/CHANGELOG.md).
// Здесь только байты; запись через ncm и регистрация через ns — platform/forwarder.
#pragma once

#include "mk/core.hpp"

#include <functional>
#include <optional>

namespace mk::forwarder {

// ---------------- TitleID ----------------
// Формула sphaira (owo.cpp install_forwader_internal): args пусто → args = nro_path, иначе nro_path + ' ' + args;
// sha256(nro_path + args), первые 8 байт как u64 (little-endian) & 0x00FFFFFFFFFFF000 | 0x05….
// Вид пути — как его передаёт sphaira: «/switch/…/X.nro» (список homebrew строится от "/switch", без «sdmc:»), —
// тогда ярлык, созданный в sphaira на тот же .nro, получает тот же TitleID и заменяется, а не задваивается.
std::string next_argv(const std::string& nro_path, const std::string& args);
uint64_t title_id(const std::string& nro_path, const std::string& args, IHash& hash);

constexpr uint64_t kPrefixMask = 0xFF00000000000000ULL;
constexpr uint64_t kPrefix = 0x0500000000000000ULL;
constexpr uint64_t kHbmenuForwarder = 0x0104444444444000ULL;   // ярлык Homebrew Menu (hbmenu)
// Форвардеры схемы sphaira (наш тоже): старший байт 0x05. Настоящие приложения — 0x01….
inline bool is_forwarder_id(uint64_t id) { return (id & kPrefixMask) == kPrefix; }
// Не игра — в список игр приложения не берётся: ярлык hbmenu и все форвардеры 0x05….
inline bool hidden_from_games(uint64_t id) { return id == kHbmenuForwarder || is_forwarder_id(id); }

// ---------------- NPDM ----------------
// Правка main.npdm hbl, как sphaira patch_npdm со значениями по умолчанию: адресное пространство 36 бит (тип 1),
// три ядра (приоритеты 28..59, ядра 0..2), имя "Application", TitleID в ACI0 и в диапазон ACID. force_debug —
// Atmosphère ≥ 1.8.0 (HOS 19): вместо прежнего флага отладки — бит ForceDebug (BIT 19 описателя флагов отладки),
// иначе hbl не отобразит код NRO. Ошибка NPDM_BAD — не NPDM или нет описателя флагов ядра.
uint32_t kernel_flags_three_cores();   // описатель флагов ядра без младших бит шаблона (как npdm_kernel_flags)
void patch_npdm(Bytes& npdm, uint64_t tid, bool force_debug);

// ---------------- NACP ----------------
constexpr size_t kNacpSize = 0x4000;
struct NacpPatch {
    std::string name;      // все 16 языков
    std::string author;
    std::string version;   // display_version
    std::string error_code_category;   // до 8 символов (у sphaira — "sphaira")
    uint64_t tid = 0;
};
// NACP без исходного (нет .nro): нули, «не оценено» во всех рейтингах (0xFF).
Bytes default_nacp();
// NACP из ассетов NRO (ASET после кода): тот, что записал nacptool, со всеми его полями. Нет файла, не NRO или нет
// NACP — nullopt.
std::optional<Bytes> nacp_from_nro(IFileSystem& fs, const std::string& path);
// Поля, которые делают NACP форвардерским, — как sphaira patch_nacp (имя/автор, учётная запись не спрашивается,
// снимки и видео разрешены, лого Nintendo, без сохранений, TitleID в presence/save owner/local communication…),
// плюс два поля по опыту forwarder.c на консоли: поддержан только American English (иконка одна —
// icon_AmericanEnglish.dat; при «поддержаны все» домашний экран ждал icon_<язык>.dat вечно) и формат таблицы
// названий 0 (несжатая — пишем её сами). Ошибка NACP_BAD — размер не 0x4000.
void patch_nacp(Bytes& nacp, const NacpPatch& p);

// Смещения полей NACP (ns ApplicationControlProperty). Платформа сверяет их с NacpStruct libnx (static_assert).
namespace nacp_off {
constexpr size_t kLangEntry = 0x300, kName = 0x200, kAuthor = 0x100;
constexpr size_t kStartupUserAccount = 0x3025, kUserAccountSwitchLock = 0x3026, kAocRegistrationType = 0x3027;
constexpr size_t kSupportedLanguageFlag = 0x302C, kScreenshot = 0x3034, kVideoCapture = 0x3035;
constexpr size_t kDataLossConfirmation = 0x3036, kPlayLogPolicy = 0x3037, kPresenceGroupId = 0x3038;
constexpr size_t kRatingAge = 0x3040, kDisplayVersion = 0x3060, kAocBaseId = 0x3070, kSaveDataOwnerId = 0x3078;
constexpr size_t kUserAccountSaveDataSize = 0x3080, kUserAccountSaveDataJournalSize = 0x3088;
constexpr size_t kDeviceSaveDataSize = 0x3090, kDeviceSaveDataJournalSize = 0x3098;
constexpr size_t kErrorCodeCategory = 0x30A8, kLocalCommunicationId = 0x30B0;
constexpr size_t kLogoType = 0x30F0, kLogoHandling = 0x30F1, kPseudoDeviceIdSeed = 0x30F8;
constexpr size_t kUserAccountSaveDataSizeMax = 0x3148, kUserAccountSaveDataJournalSizeMax = 0x3150;
constexpr size_t kDeviceSaveDataSizeMax = 0x3158, kDeviceSaveDataJournalSizeMax = 0x3160;
constexpr size_t kPlayLogQueryCapability = 0x3210, kRequiredNetworkLicense = 0x3213, kTitlesDataFormat = 0x3215;
}  // namespace nacp_off

// ---------------- сборка NCA ----------------
// Шифрование заголовка NCA на месте: 0xC00 байт, шесть секторов AES-128-XTS по 0x200 с «Nintendo-tweak» (номер
// сектора big-endian), ключом заголовка консоли. Бросает Error при отказе.
using HeaderCryptFn = std::function<void(uint8_t* header, size_t size)>;

// Типы содержимого ncm (NcmContentType) — для таблицы cnmt и базы ncm.
enum : uint8_t { kNcmMeta = 0, kNcmProgram = 1, kNcmControl = 3 };
constexpr uint8_t kStorageSdCard = 5;            // NcmStorageId_SdCard
constexpr uint8_t kMetaTypeApplication = 0x80;   // NcmContentMetaType_Application
constexpr uint8_t kInstallTypeFull = 0;          // NcmContentInstallType_Full

struct Input {
    uint64_t tid = 0;
    Bytes hbl_main;          // exefs/main (NSO) загрузчика
    Bytes npdm;              // main.npdm, уже исправленный patch_npdm
    Bytes nacp;              // control.nacp, уже исправленный patch_nacp (0x4000)
    Bytes icon;              // JPEG 256×256
    std::string nro_path;    // /nextNroPath
    std::string argv;        // /nextArgv (next_argv)
    uint8_t storage_id = kStorageSdCard;
};

struct Nca {
    Bytes data;              // готовый NCA (заголовок зашифрован HeaderCryptFn)
    Digest hash{};           // sha256 всего NCA; первые 16 байт — content id
    uint8_t ncm_type = 0;
};

struct Output {
    Nca program, control, meta;
    Bytes cnmt;              // содержимое Application_<TID>.cnmt (в Meta NCA)
    // Запись для ncmContentMetaDatabaseSet: NcmContentMetaHeader + NcmApplicationMetaExtendedHeader +
    // NcmContentInfo[3] (meta, program, control) — побайтно как NcmContentMetaData у sphaira.
    Bytes meta_db_record;
};

// Ошибки: FWD_BAD_INPUT (пустые части, иконка больше 0x20000, NACP не 0x4000).
Output build(const Input& in, IHash& hash, const HeaderCryptFn& crypt);

// Имя файла cnmt в Meta NCA: "Application_%016llX.cnmt".
std::string cnmt_name(uint64_t tid);

}  // namespace mk::forwarder
