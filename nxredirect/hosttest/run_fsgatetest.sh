#!/bin/bash
# Хост-тест ЗАТВОРА ФАЙЛОВОЙ СИСТЕМЫ: сборка, прогон и НЕГАТИВНЫЕ ПРОБЫ.
# Здесь же — хост-тест ОТЧЁТОВ отладочного модуля (logs_test.cpp,
# src/nxr_logs.hpp: путь по TitleID, номер, ротация «последние 5»): отчёты
# пишутся сразу после открытия затвора, и прогон у них общий.
#
#   run_fsgatetest.sh            — настоящая логика (обязана быть ЗЕЛЁНОЙ)
#   run_fsgatetest.sh <поломка>  — намеренно испорченная копия (обязана быть КРАСНОЙ)
#   run_fsgatetest.sh --all      — настоящая плюс все поломки, общий вердикт
#
# Ломается ТОЛЬКО копия во временной папке; файлы проекта не трогаются.
# Ничего не удаляется: каждая проба работает в своей подпапке.
# Запускать из WSL Ubuntu; нужен только g++, devkitPro не требуется.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src"
WORK="${NXR_FSGATETEST_DIR:-/tmp/nxredirect-fsgatetest}"

BREAKS="gate-always-open no-wait-for-setallocator gate-reopen no-drop-count ring-overflow no-terminator open-flag-only no-retry-gate-opener gate-mount-no-cas logs-keep-all logs-reuse-number logs-keep-six logs-no-mkdir logs-lower-tid skip-all"

prepare() {
    d="$1"
    mkdir -p "$d"
    cp -f "$HERE/fsgate_test.cpp" "$HERE/logs_test.cpp" "$d/"
    cp -f "$SRC/nxr_fsgate.hpp" "$SRC/nxr_logs.hpp" "$SRC/nxr_path.hpp" "$d/"
}

apply_break() {
    d="$1"; b="$2"
    case "$b" in
        none) ;;
        gate-always-open)
            # Затвора нет: модуль работает с nn::fs с первой же секунды — ровно
            # то, из-за чего Well Dweller падал с Result 0x327602.
            sed -i 's/    if (gate.IsOpen()) {/    if (true) {/' "$d/nxr_fsgate.hpp"
            ;;
        no-wait-for-setallocator)
            # Затвор открывается по первому OpenFile даже у игры, которая ЗОВЁТ
            # SetAllocator: её вызов мог ничего не выделить, и флаг взвели бы мы.
            sed -i 's/    if (gameCallsSetAllocator) {/    if (false) {/' "$d/nxr_fsgate.hpp"
            ;;
        gate-reopen)
            # Затвор «открывается» каждый раз заново: отложенная работа пошла бы
            # по второму кругу, а причина в отчёте оказалась бы неверной.
            sed -i 's/        if (!m_Reason.compare_exchange_strong(expected, static_cast<int>(reason),/        if (false \&\& !m_Reason.compare_exchange_strong(expected, static_cast<int>(reason),/' \
                "$d/nxr_fsgate.hpp"
            ;;
        no-drop-count)
            # Непоместившиеся ранние события перестают считаться: отчёт
            # умалчивает о потере, а это и есть нечестная строка.
            sed -i 's/^            ++m_Dropped;/            ;/' "$d/nxr_fsgate.hpp"
            ;;
        ring-overflow)
            # Проверка размера снята — запись уходит за край массива в .bss.
            sed -i 's/        if (m_Count >= N) {/        if (false) {/' "$d/nxr_fsgate.hpp"
            ;;
        no-terminator)
            # Обрезанный путь не завершается нулём: в отчёт попадёт мусор из
            # соседней записи (а на консоли — из чужой памяти).
            sed -i "s/^        it.m_Path\[n\] = '\\\\0';/        ;/" "$d/nxr_fsgate.hpp"
            ;;
        open-flag-only)
            # Причина записана, а сам затвор не открыт: модуль замолчал бы
            # навсегда, и мод перестал бы работать.
            sed -i 's/        m_Open.store(true, std::memory_order_release);/        ;/' \
                "$d/nxr_fsgate.hpp"
            ;;
        no-retry-gate-opener)
            # Как было раньше: вызов, открывший затвор, не повторяется с SD —
            # первый файл чужого мода (ini UltraCam у TotK) терялся.
            sed -i 's/    return action == OpenFileAction::PassThroughThenOpenGate \&\& originalFailed;/    (void)action; (void)originalFailed; return false;/' "$d/nxr_fsgate.hpp"
            ;;
        gate-mount-no-cas)
            # Как было раньше: OpenFsGate монтирует SD без compare-exchange —
            # при параллельном EnsureSdMounted MountSdCard зовётся дважды (0x7802).
            sed -i 's/^        if (TryClaim()) {/        m_State.store(static_cast<int>(SdState::InProgress), std::memory_order_release); if (true) {/' \
                "$d/nxr_fsgate.hpp"
            ;;
        logs-keep-all)
            # Ротации нет: старые отчёты не удаляются, папка растёт без края.
            sed -i 's/^            return n < m_Top\[m_TopCount - 1\];/            (void)n; return false;/' "$d/nxr_logs.hpp"
            grep -q '(void)n; return false;' "$d/nxr_logs.hpp" || { echo "поломка logs-keep-all не легла"; return 2; }
            ;;
        logs-reuse-number)
            # Новый отчёт получает номер последнего: перезапись вместо нового файла.
            sed -i 's/return m_Max >= MaxNumber ? MaxNumber : m_Max + 1;/return m_Max >= MaxNumber ? MaxNumber : m_Max;/' "$d/nxr_logs.hpp"
            grep -q ': m_Max;' "$d/nxr_logs.hpp" || { echo "поломка logs-reuse-number не легла"; return 2; }
            ;;
        logs-keep-six)
            # Хранится 6, а не 5 — расхождение с правилом «последние 5».
            sed -i 's/^    constexpr int           Keep        = 5;/    constexpr int           Keep        = 6;/' "$d/nxr_logs.hpp"
            grep -q 'Keep        = 6;' "$d/nxr_logs.hpp" || { echo "поломка logs-keep-six не легла"; return 2; }
            ;;
        logs-no-mkdir)
            # Папки не создаются: на чистой карте отчёта не будет вовсе.
            sed -i 's/^        r.dirsMade = ForEachDirLink(L, \[&\](const char\* p) { fs.MakeDir(p); });/        r.dirsMade = 0;/' "$d/nxr_logs.hpp"
            grep -q 'r.dirsMade = 0;' "$d/nxr_logs.hpp" || { echo "поломка logs-no-mkdir не легла"; return 2; }
            ;;
        logs-lower-tid)
            # TitleID строчными: папка не совпадёт с тем, что ищет приложение.
            sed -i "s/Ch(static_cast<char>(nib < 10 ? ('0' + nib) : ('A' + nib - 10)));/Ch(static_cast<char>(nib < 10 ? ('0' + nib) : ('a' + nib - 10)));/" "$d/nxr_logs.hpp"
            grep -q "('a' + nib - 10)" "$d/nxr_logs.hpp" || { echo "поломка logs-lower-tid не легла"; return 2; }
            ;;
        skip-all)
            # Проверки не выполняются вовсе — обязан сработать «затвор нуля».
            sed -i 's/^    TestGameWithSetAllocator();/    \/\/TestGameWithSetAllocator();/;
                    s/^    TestGameWithoutSetAllocator();/    \/\/TestGameWithoutSetAllocator();/;
                    s/^    TestGateOpenerRetry();/    \/\/TestGateOpenerRetry();/;
                    s/^    TestEarlyLog();/    \/\/TestEarlyLog();/;
                    s/^    TestGateOnce();/    \/\/TestGateOnce();/;
                    s/^    TestSdMountOnce();/    \/\/TestSdMountOnce();/' "$d/fsgate_test.cpp"
            sed -i 's/^    Test\([A-Za-z]*\)();/    \/\/Test\1();/' "$d/logs_test.cpp"
            ;;
        *)
            echo "неизвестная поломка: $b"; return 2;;
    esac
    return 0
}

run_one() {
    b="$1"
    d="$WORK/$b"
    prepare "$d" || return 2
    apply_break "$d" "$b" || return 2
    if ! (cd "$d" && g++ -std=c++20 -O2 -Wall -pthread -o fsgate_test fsgate_test.cpp -I. >build.log 2>&1 &&
          g++ -std=c++20 -O2 -Wall -Wextra -o logs_test logs_test.cpp -I. >>build.log 2>&1); then
        # Поломка, которая не компилируется, тоже считается «красной»: логика
        # изменена и тест её не пропустил.
        echo "        (сборка не удалась — считаем красным)"
        tail -3 "$d/build.log"
        return 1
    fi
    (cd "$d" && ./fsgate_test >run.log 2>&1)
    rc=$?
    tail -3 "$d/run.log" | sed 's/^/        /'
    (cd "$d" && ./logs_test >logs.log 2>&1)
    rc2=$?
    tail -2 "$d/logs.log" | sed 's/^/        /'
    [ $rc -ne 0 ] && return $rc
    return $rc2
}

if [ "${1:---all}" = "--all" ]; then
    fails=0
    echo "=== настоящая логика (обязана быть ЗЕЛЁНОЙ) ==="
    if run_one none; then
        echo "  ЗЕЛЕНО — верно"
    else
        echo "  КРАСНО — ПЛОХО: настоящая логика не проходит свой же тест"
        fails=$((fails + 1))
    fi
    for b in $BREAKS; do
        echo "=== негативная проба '$b' (обязана быть КРАСНОЙ) ==="
        run_one "$b"
        rc=$?
        if [ $rc -eq 0 ]; then
            echo "  ЗЕЛЕНО — ПЛОХО: поломка не поймана, проверка пустая"
            fails=$((fails + 1))
        elif [ $rc -eq 2 ]; then
            # Код 2 — поломка НЕ ПРИМЕНИЛАСЬ (sed не нашёл строку)
            # или проба неизвестна: тест не прогонялся, «красным» это не считается.
            echo "  ПОЛОМКА НЕ ПРИМЕНИЛАСЬ — ПЛОХО: проба ничего не проверила"
            fails=$((fails + 1))
        else
            echo "  КРАСНО — верно, поломка поймана"
        fi
    done
    echo
    if [ "$fails" -eq 0 ]; then
        echo "ИТОГ: тест зелёный, все $(echo $BREAKS | wc -w) негативных проб поймано"
        exit 0
    fi
    echo "ИТОГ: неудач $fails"
    exit 1
fi

run_one "${1:-none}"
exit $?
