#!/bin/bash
# Хост-тест диагностики графики: сборка, прогон и НЕГАТИВНЫЕ ПРОБЫ.
#
#   run_gfxtest.sh            — настоящая логика (обязана быть ЗЕЛЁНОЙ)
#   run_gfxtest.sh <поломка>  — намеренно испорченная копия (обязана быть КРАСНОЙ)
#   run_gfxtest.sh --all      — настоящая плюс все поломки, общий вердикт
#
# Ломается ТОЛЬКО копия во временной папке; файлы проекта не трогаются.
# Запускать из WSL Ubuntu; нужен только g++, devkitPro не требуется.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src"
WORK="${NXR_GFXTEST_DIR:-/tmp/nxredirect-gfxtest}"

BREAKS="no-fail-mark ptr-not-failure no-null-check no-crlf skip-all report-only-found vi-init-as-result errno-not-failure no-budget-check window-ignored armed-ignored ring-overflow early-partial-line early-no-drop-count pending-overflow pending-unready-drain seterror-sig-ignored seterror-success-logged seterror-not-in-budget"

prepare() {
    rm_free_dir="$1"
    mkdir -p "$rm_free_dir"
    cp -f "$HERE/gfx_test.cpp" "$rm_free_dir/"
    cp -f "$SRC/nxr_gfx.hpp" "$rm_free_dir/"
}

apply_break() {
    d="$1"; b="$2"
    case "$b" in
        none) ;;
        no-fail-mark)
            # Метка неуспеха совпадает с обычной: первую сломавшуюся функцию
            # в отчёте больше не найти ни глазом, ни поиском.
            sed -i 's/constexpr char MarkFail\[\] = "!! ";/constexpr char MarkFail[] = "   ";/' "$d/nxr_gfx.hpp"
            ;;
        ptr-not-failure)
            # Нулевой указатель (eglCreateContext вернул 0) считается успехом.
            sed -i 's/case RetKind::Pointer: return e.ret == 0;/case RetKind::Pointer: return false;/' "$d/nxr_gfx.hpp"
            ;;
        no-null-check)
            # Хук ставится даже по нулевому адресу — на консоли это падение.
            sed -i 's/if (addr != 0 \&\& installed < budgetLeft) {/if (true) {/' "$d/nxr_gfx.hpp"
            ;;
        no-budget-check)
            # Бюджет трамплинов exlaunch не проверяется — ровно то, из-за чего
            # ШАГ-7 уронил игру: пул кончился, и exlaunch аварийно завершил её.
            sed -i 's/if (addr != 0 \&\& installed < budgetLeft) {/if (addr != 0) {/' "$d/nxr_gfx.hpp"
            ;;
        window-ignored)
            # Перехватчик пишет всегда, а не только внутри окна и в своём
            # потоке — это и есть «широкий перехват», который нельзя.
            sed -i 's/return w.m_Armed != 0 \&\& tls != 0 \&\& w.m_Owner == tls;/return true;/' "$d/nxr_gfx.hpp"
            ;;
        armed-ignored)
            # Признак готовности не смотрим: перехватчик может сработать по
            # ещё не готовым данным (то, чего требовалось не допустить).
            sed -i 's/return w.m_Armed != 0 \&\& tls != 0 \&\& w.m_Owner == tls;/return tls != 0 \&\& w.m_Owner == tls;/' "$d/nxr_gfx.hpp"
            ;;
        ring-overflow)
            # Буфер пишется без проверки размера — выход за массив в памяти
            # чужого потока. Тест обязан это увидеть.
            sed -i 's/if (ring.m_Count >= LockRingSize) {/if (false) {/' "$d/nxr_gfx.hpp"
            ;;
        early-partial-line)
            # Ранний буфер пишет, «сколько влезет»: в отчёт попадёт обрывок строки,
            # а запись уйдёт за край массива.
            sed -i 's/        if (t.m_Len > Size || n > static_cast<std::size_t>(Size - t.m_Len)) {/        if (false) {/' "$d/nxr_gfx.hpp"
            ;;
        early-no-drop-count)
            # Не поместившиеся ранние строки не считаются — отчёт умалчивает о потере.
            sed -i 's/^            ++t.m_Dropped;/            ;/' "$d/nxr_gfx.hpp"
            ;;
        pending-overflow)
            # Слот выдаётся без проверки размера — запись за массив в .bss.
            sed -i 's/        if (i >= static_cast<std::uint32_t>(N)) {/        if (false) {/' "$d/nxr_gfx.hpp"
            ;;
        pending-unready-drain)
            # Сливается всё взятое, без флага готовности: в отчёт уходит
            # недописанная другим потоком запись.
            sed -i 's/               __atomic_load_n(\&r.m_Ready\[r.m_Drained\], __ATOMIC_ACQUIRE) != 0) {/               r.m_Drained < __atomic_load_n(\&r.m_Reserved, __ATOMIC_ACQUIRE)) {/' "$d/nxr_gfx.hpp"
            ;;
        seterror-sig-ignored)
            # Подпись не сверяется: inline-хук встал бы посреди чужой версии GL.
            sed -i 's/            if (code\[i\] != kSetErrorTargets\[index\].sig\[i\]) {/            if (false) {/' "$d/nxr_gfx.hpp"
            ;;
        seterror-success-logged)
            # Пишется и EGL_SUCCESS: буфер забьётся сбросами ошибки до первой
            # настоящей строки.
            sed -i 's/        return code != EglSuccess;/        (void)code; return true;/' "$d/nxr_gfx.hpp"
            ;;
        seterror-not-in-budget)
            # Пробы SetError не учтены в счёте трамплинов (inline-хук тоже берёт
            # трамплин) — пул снова может кончиться, как в ШАГЕ-7.
            sed -i 's/                               + static_cast<int>(S_Count) + HooksOutsideGfx;/                               + HooksOutsideGfx;/' "$d/nxr_gfx.hpp"
            ;;
        no-crlf)
            # Перевод строки без CR: блокнот Windows покажет всё одной строкой.
            sed -i "s/            Put('\\\\r');//" "$d/nxr_gfx.hpp"
            ;;
        skip-all)
            # Проверки не выполняются вовсе — обязан сработать «затвор нуля».
            sed -i 's/^    TestFormat();/    \/\/TestFormat();/;
                    s/^    TestFailureFlag();/    \/\/TestFailureFlag();/;
                    s/^    TestInstall();/    \/\/TestInstall();/;
                    s/^    TestWriteOnEachEvent();/    \/\/TestWriteOnEachEvent();/;
                    s/^    TestTargetTable();/    \/\/TestTargetTable();/;
                    s/^    TestRetKinds();/    \/\/TestRetKinds();/;
                    s/^    TestHookBudget();/    \/\/TestHookBudget();/;
                    s/^    TestLockWindow();/    \/\/TestLockWindow();/;
                    s/^    TestEarlyText();/    \/\/TestEarlyText();/;
                    s/^    TestPendingRing();/    \/\/TestPendingRing();/;
                    s/^    TestSetError();/    \/\/TestSetError();/' "$d/gfx_test.cpp"
            ;;
        vi-init-as-result)
            # Прежняя ошибка: nn::vi::Initialize объявлена возвращающей Result,
            # хотя не возвращает ничего. Мусор из w0 печатается как «ОШИБКА» —
            # ложный след первой же строкой отчёта.
            sed -i 's/^          RetKind::Void },/          RetKind::Result },/' "$d/nxr_gfx.hpp"
            ;;
        errno-not-failure)
            # Код ошибки блокировки (EDEADLK) перестаёт считаться неуспехом:
            # ради него и затевается проба ШАГ-7, а в отчёте его было бы не найти.
            sed -i 's/case RetKind::Errno:   return e.ret != 0;/case RetKind::Errno:   return false;/' "$d/nxr_gfx.hpp"
            ;;
        report-only-found)
            # В отчёт попадают только НАЙДЕННЫЕ цели: про ненайденный символ
            # читатель отчёта никогда не узнает, а это и есть половина диагноза.
            sed -i 's/^            report(i, addr, done);/            if (addr != 0) report(i, addr, done);/' "$d/nxr_gfx.hpp"
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
    if ! (cd "$d" && g++ -std=c++20 -O2 -Wall -o gfx_test gfx_test.cpp -I. >build.log 2>&1); then
        # Поломка, которая не компилируется, тоже считается «красной»: логика
        # изменена и тест её не пропустил.
        echo "        (сборка не удалась — считаем красным)"
        tail -3 "$d/build.log"
        return 1
    fi
    (cd "$d" && ./gfx_test >run.log 2>&1)
    rc=$?
    tail -3 "$d/run.log" | sed 's/^/        /'
    return $rc
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
        if run_one "$b"; then
            echo "  ЗЕЛЕНО — ПЛОХО: поломка не поймана, проверка пустая"
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
