#!/bin/bash
# Хост-тесты nxredirect: та же логика, что исполняется в модуле, прогоняется на ПК.
# Запускается из WSL Ubuntu (нужен только g++, devkitPro не требуется).
#
#   run_hosttest.sh <папка exefs игры> <путь к собранному subsdkN> [поломка]
#   NXR_GAME_EXEFS=<папка> run_hosttest.sh - <subsdkN> [поломка]
#
# Два теста:
#   path_test    — построение пути мода из TitleID и подмена префикса (модули игры
#                  не нужны, гоняется всегда);
#   symfind_test — обход модулей и поиск символов на НАСТОЯЩИХ модулях игры.
#
# ВАЖНО. Если папка модулей игры не задана или в ней нет модулей, symfind_test
# КРАСНЕЕТ (код 2), а не пропускается молча: гейт, который ничего не проверил, не
# должен выглядеть пройденным.
#
# Makefile/g++ спокойно переживают пробелы, но чтобы не зависеть от кавычек в
# цепочке powershell -> wsl -> bash, исходники и входные файлы копируются во
# временную папку без пробелов, и тесты запускаются уже там.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
GAME_EXEFS="${1:--}"
MODULE="$2"
BREAK="${3:-none}"

if [ "$GAME_EXEFS" = "-" ]; then
    GAME_EXEFS="${NXR_GAME_EXEFS:-}"
fi

WORK="${NXR_HOSTTEST_DIR:-/tmp/nxredirect-hosttest}"
case "$WORK" in
    *\ *) echo "Путь сборки теста не должен содержать пробелов: $WORK"; exit 2;;
esac

mkdir -p "$WORK"
# Модули игры — в СВОЮ свежую папку на каждый прогон. Прежде папка
# была общей ($WORK/exefs), а копировались только найденные subsdkN: после прогона
# на игре с subsdk0..3 (SiN) прогон на TotK (только subsdk0) молча тестировал смесь
# двух игр.
EXEFS_DIR="$(mktemp -d "$WORK/exefs.XXXXXX")"
# Свою папку этого прогона (путь — из mktemp выше, не маска) убираем
# на выходе: прежде они копились в $WORK навсегда. NXR_KEEP_TEMP=1 — оставить.
if [ "${NXR_KEEP_TEMP:-0}" = "0" ] || [ -z "${NXR_KEEP_TEMP:-}" ]; then
    trap 'rm -rf -- "$EXEFS_DIR"' EXIT
else
    trap 'echo "NXR_KEEP_TEMP: модули игры оставлены: $EXEFS_DIR"' EXIT
fi
cp -f "$HERE/symfind_test.cpp" "$HERE/path_test.cpp" "$WORK/"
cp -f "$HERE/../src/nxr_symfind.hpp" "$HERE/../src/nxr_path.hpp" "$WORK/"

# Третий аргумент — НАМЕРЕННАЯ ПОЛОМКА логики (негативные пробы самого теста).
# Ломается только КОПИЯ заголовка во временной папке; исходник не трогается.
# Каждая поломка обязана сделать тест красным — иначе тест «всегда зелёный».
# Несколько поломок сразу — через «+»: так видно, что ослабленный
# тест на поломке зелёный, а настоящий — красный (needs-dt-hash+weak-hash-check).
SYMFIND_DEFS=""
for ONE_BREAK in ${BREAK//+/ }; do
case "$ONE_BREAK" in
    none) ;;
    needs-dt-hash)
        # Поиск символов начинает ТРЕБОВАТЬ DT_HASH, как
        # GetSymbolByName шаблона exlaunch (причина №3). На игре без таблицы
        # (WD, SiN) краснеет уже блок [1]; на TotK, где таблица у всех модулей,
        # его ловит только блок [6] — та же игра с вырезанным DT_HASH.
        sed -i 's/^        if (t.strtab == 0 || t.symtab == 0 || t.strsz == 0) {/        if (t.strtab == 0 || t.symtab == 0 || t.strsz == 0 || !t.hasHash) {/' \
            "$WORK/nxr_symfind.hpp"
        grep -q '|| !t.hasHash) {' "$WORK/nxr_symfind.hpp" || { echo "поломка needs-dt-hash не легла"; exit 2; }
        ;;
    weak-hash-check)
        # ОСЛАБЛЕННЫЙ тест (не поломка логики): блок [6] не собирается. Нужен
        # только в паре с needs-dt-hash — показать, что ловит именно блок [6].
        SYMFIND_DEFS="$SYMFIND_DEFS -DNXR_SYMFIND_WEAK_HASH_CHECK"
        ;;
    mod0-limit)
        # Как было до исправления №5: MOD0 ищется только рядом с началом модуля.
        sed -i 's/maxMod0Offset = 0x4000000/maxMod0Offset = 0x1000/' "$WORK/nxr_symfind.hpp"
        ;;
    no-mapped-check)
        # Проверка отображённости памяти отключена: разбор уйдёт в пустоту.
        sed -i 's#^\(.*inline std::uint64_t ReadableSize(Query& query, std::uint64_t addr) {\)#\1 return 0x100000000ull;#' \
            "$WORK/nxr_symfind.hpp"
        ;;
    wrong-perm)
        # Область кода ищется по неверным правам — не найдётся ни один модуль.
        sed -i 's/info.perm == PermRx/info.perm == PermW/' "$WORK/nxr_symfind.hpp"
        ;;
    fixed-prefix)
        # Как было в прежнем проекте: подменяется только "rom:/" (исправление №6).
        sed -i 's#gamePath\[n\] != .:. && gamePath\[n\] != ./.#gamePath[n] != 0x3a \&\& gamePath[n] != 0x2f \&\& n < 3#' \
            "$WORK/nxr_path.hpp"
        ;;
    lower-hex)
        # TitleID в пути строчными буквами: Atmosphère такую папку не найдёт.
        sed -i "s/('A' + nib - 10)/('a' + nib - 10)/" "$WORK/nxr_path.hpp"
        ;;
    no-self-guard)
        # Снят self-guard: подменяются пути к самой SD (наш файл мода, отчёт, Atmosphère).
        sed -i 's/^            if (same) {/            if (false) {/' "$WORK/nxr_path.hpp"
        ;;
    no-mount-blacklist)
        # Снят чёрный список точек монтирования — подменялись бы сейвы и кеш.
        sed -i 's/^        if (IsBlockedMount(gamePath, n)) {/        if (false) {/' \
            "$WORK/nxr_path.hpp"
        ;;
    import-ignores-shndx)
        # Проверка «символ не определён в этом модуле» снята: импортом начинает
        # считаться и ОПРЕДЕЛЁННЫЙ символ. Тогда модуль решил бы, что игра зовёт
        # nn::fs::SetAllocator, у любой игры (символ есть в sdk) — и затвор ФС
        # у игр без этого вызова не открылся бы никогда.
        sed -i 's/            if (sym.st_shndx != SHN_UNDEF_) {/            if (false) {/' \
            "$WORK/nxr_symfind.hpp"
        ;;
    no-mode-check)
        # Снята проверка режима — подменялось бы и открытие на запись.
        sed -i 's/^        return mode == OpenModeRead;/        (void)mode; return true;/' \
            "$WORK/nxr_path.hpp"
        ;;
    shared-mount-name)
        # Как было раньше: SD монтируется под ОБЩИМ именем "sd". Чужой мод
        # (UltraCam для TotK) занимал его первым, MountSdCard отдавал 0x7802, и
        # подмена выключалась на всю сессию.
        sed -i 's/^#define NXR_SD_MOUNT "nxrsd"/#define NXR_SD_MOUNT "sd"/' "$WORK/nxr_path.hpp"
        ;;
    rtld-last-wins)
        # Правило rtld перевёрнуто: выигрывает ПОСЛЕДНИЙ определивший модуль
        # (sdk), а не первый. Тогда модуль решил бы, что его экспорт
        # NvRmMemQueryHandleParams проиграл, — а по коду rtld он выигрывает.
        sed -i 's#                return true;   /\* rtld: выигрывает ПЕРВЫЙ найденный \*/#                return false;#' "$WORK/nxr_symfind.hpp"
        ;;
    import-slot-any-symbol)
        # Слот импорта ищется без сравнения имени: первая же релокация на
        # неопределённый символ. Ответ «куда разрешился импорт» стал бы ложным.
        sed -i 's/^                if (NameEquals(strings + sym.st_name, name)) {\r\?$/                if (true) {/' "$WORK/nxr_symfind.hpp"
        ;;
    content-always)
        # Как в рабочей сборке TotK с ручным ключом NXR_ALLOW_CONTENT_MOUNT:
        # "content" подменяется ВСЕГДА, без узнавания через MountRom. Тогда
        # подменялся бы и nn::fs::MountContent чужого NCA.
        sed -i 's/^    constexpr bool ContentMountForced = false;/    constexpr bool ContentMountForced = true;/' \
            "$WORK/nxr_path.hpp"
        ;;
    rom-mount-ignored)
        # Реестр romfs не читается: узнанное через MountRom имя ничего не даёт —
        # ровно состояние «ini UltraCam не доходит».
        sed -i 's/^            (romMounts == nullptr || !romMounts->Contains(gamePath, n))) {/            true) {/' \
            "$WORK/nxr_path.hpp"
        ;;
    rom-mount-takes-system)
        # Реестр принимает и системные имена: «MountRom("save")» сделал бы сейв
        # подменяемым. Снята проверка жёсткого списка в RomMounts::ValidLength.
        sed -i 's/^            if (IsBlockedMount(name, len)) {/            if (false) {/' \
            "$WORK/nxr_path.hpp"
        ;;
    unmount-keeps)
        # Unmount не вычёркивает имя: после него под "content" мог бы оказаться
        # MountContent, а подмена продолжалась бы.
        sed -i 's/^                s.ver.compare_exchange_strong(v, ((v \& ~StateMask) + GenStep) | StateEmpty,/                if (false) s.ver.compare_exchange_strong(v, ((v \& ~StateMask) + GenStep) | StateEmpty,/' \
            "$WORK/nxr_path.hpp"
        ;;
    *) echo "неизвестная поломка: $ONE_BREAK"; exit 2;;
esac
done

CXX="${CXX:-g++}"
CXXFLAGS="-std=c++17 -O1 -Wall -Wextra"

echo "=== path_test (пути; модули игры не нужны) ==="
# -pthread: проба реестра romfs из нескольких потоков.
$CXX $CXXFLAGS -pthread -o "$WORK/path_test" "$WORK/path_test.cpp"
"$WORK/path_test"

echo
echo "=== symfind_test (поиск символов в настоящих модулях игры) ==="
if [ -z "$GAME_EXEFS" ] || [ -z "$MODULE" ]; then
    echo "КРАСНО: не заданы модули игры и/или собранный модуль."
    echo "  использование: run_hosttest.sh <папка exefs игры> <subsdkN> [поломка]"
    echo "  или:           NXR_GAME_EXEFS=<папка> run_hosttest.sh - <subsdkN>"
    exit 2
fi
for f in rtld main sdk; do
    if [ ! -f "$GAME_EXEFS/$f" ]; then
        echo "КРАСНО: нет обязательного модуля игры: $GAME_EXEFS/$f"
        exit 2
    fi
done
# Копируем то, что есть: rtld/main/sdk обязательны, subsdk0..9 — какие найдутся.
for f in rtld main sdk; do cp -f "$GAME_EXEFS/$f" "$EXEFS_DIR/$f"; done
for n in 0 1 2 3 4 5 6 7 8 9; do
    [ -f "$GAME_EXEFS/subsdk$n" ] && cp -f "$GAME_EXEFS/subsdk$n" "$EXEFS_DIR/subsdk$n"
done
cp -f "$MODULE" "$WORK/module.nso"

$CXX $CXXFLAGS $SYMFIND_DEFS -o "$WORK/symfind_test" "$WORK/symfind_test.cpp"
"$WORK/symfind_test" "$EXEFS_DIR" "$WORK/module.nso"
