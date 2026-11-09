#!/usr/bin/env bash
# build-romfs-modules.sh — сборка всех вариантов модуля nxredirect, которые лежат в romfs/modules/
# приложения ModKitNX, из этих исходников. Только сборка: ни карты, ни консоли, ни файлов игр.
#
#   bash build-romfs-modules.sh <папка-выхода> [вариант ...]
#   bash build-romfs-modules.sh <папка-выхода> --check <romfs/modules>   сверить sha256 с описью manifest.json
#
# Нужно: Linux или WSL, devkitPro с devkitA64 (DEVKITPRO, по умолчанию /opt/devkitpro), make, rsync, sha256sum.
# Каждый вариант собирается в СВОЕЙ свежей временной папке (mktemp -d): make сравнивает время файлов, а не
# флаги, и сборка с другим -D в той же папке молча отдала бы прежний бинарник.
#
# Воспроизводимость. Код и данные модуля от папки сборки не зависят, но GNU build-id (заголовок NSO,
# ModuleId, и 20 байт в .rodata) считается по ELF целиком, вместе с путями отладочной информации.
# Поставленные модули собраны в /tmp/nxr-build-subsdk9<суффикс>; чтобы build-id совпал, пути временной
# папки подменяются на эти через -ffile-prefix-map (C/C++) и --debug-prefix-map (ассемблер). С ними
# результат совпадает с romfs/modules побайтно (проверено на всех вариантах).
# Временные папки не удаляются (скрипт ничего не удаляет) — путь печатается.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-}"
[ -n "$OUT" ] || { echo "использование: bash build-romfs-modules.sh <папка-выхода> [вариант ...] [--check <romfs/modules>]" >&2; exit 2; }
shift
CHECK=""
WANT=()
while [ $# -gt 0 ]; do
  case "$1" in
    --check) CHECK="${2:-}"; [ -n "$CHECK" ] || { echo "--check: нужна папка romfs/modules" >&2; exit 2; }; shift 2 ;;
    *) WANT+=("$1"); shift ;;
  esac
done

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
export PATH="$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH"
[ -x "$DEVKITA64/bin/aarch64-none-elf-gcc" ] || { echo "нет devkitA64 в $DEVKITA64 (задайте DEVKITPRO)" >&2; exit 3; }
for t in make rsync sha256sum; do command -v "$t" >/dev/null || { echo "нет программы $t" >&2; exit 3; }; done
[ -f "$HERE/exlaunch/Makefile" ] && [ -f "$HERE/src/main.cpp" ] || { echo "нет exlaunch/Makefile или src/main.cpp рядом со скриптом" >&2; exit 2; }

BASE_C_FLAGS="-Wno-error=non-c-typedef-for-linkage"
# ARCH берётся из misc/mk/common.mk: ASFLAGS переопределяется целиком, -g и ARCH нужно повторить.
ARCH="$(grep -m1 -E '^ARCH[[:space:]]*:=' "$HERE/exlaunch/misc/mk/common.mk" | sed -E 's/^ARCH[[:space:]]*:=[[:space:]]*//')"

# вариант | флаги (через запятую; порядок как у nxr_build.py: DIAG, NO_AUTOFIX, EARLY_FS, DIAG_GFX) | суффикс папки
VARIANTS="
autofix||-autofix
autofix-earlyfs|-DNXR_EARLY_FS_HOOK|-autofix-earlyfs
plain|-DNXR_NO_AUTOFIX|
diag|-DNXR_DIAG,-DNXR_NO_AUTOFIX|-diag
diag-gfx|-DNXR_DIAG,-DNXR_NO_AUTOFIX,-DNXR_DIAG_GFX|-diag-gfxdiag
plain-gfx|-DNXR_NO_AUTOFIX,-DNXR_DIAG_GFX|-gfxdiag
autofix-diag|-DNXR_DIAG|-diag-autofix
autofix-diag-gfx|-DNXR_DIAG,-DNXR_DIAG_GFX|-diag-autofix-gfxdiag
autofix-diag-earlyfs|-DNXR_DIAG,-DNXR_EARLY_FS_HOOK|-diag-autofix-earlyfs
"

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
n=0; bad=0
while IFS='|' read -r id flags suffix; do
  [ -n "$id" ] || continue
  if [ ${#WANT[@]} -gt 0 ] && ! printf '%s\n' "${WANT[@]}" | grep -qx "$id"; then continue; fi
  n=$((n + 1))
  work="$(mktemp -d "${TMPDIR:-/tmp}/nxr-romfs-$id.XXXXXX")"
  orig="/tmp/nxr-build-subsdk9$suffix"
  rsync -a --exclude build --exclude deploy --exclude .git "$HERE/exlaunch/" "$work/exlaunch/"
  rsync -a --exclude __pycache__ "$HERE/src/" "$work/src/"
  cflags="$BASE_C_FLAGS ${flags//,/ } -ffile-prefix-map=$work=$orig"
  if ! (cd "$work/exlaunch" && make MODULE_NAME=subsdk9 "C_FLAGS=$cflags" \
          "ASFLAGS=-g $ARCH -Wa,--debug-prefix-map=$work=$orig" >"$work/make.log" 2>&1); then
    echo "[FAIL] $id: сборка не удалась, лог: $work/make.log" >&2; tail -20 "$work/make.log" >&2; bad=$((bad + 1)); continue
  fi
  mkdir -p "$OUT/$id"
  cp "$work/exlaunch/deploy/subsdk9" "$OUT/$id/subsdk"
  sha="$(sha256sum "$OUT/$id/subsdk" | cut -c1-64)"
  line="$id  $(stat -c %s "$OUT/$id/subsdk") B  $sha"
  if [ -n "$CHECK" ]; then
    want="$(grep -A4 "\"id\": *\"$id\"" "$CHECK/manifest.json" | grep -m1 '"sha256"' | sed -E 's/.*"sha256": *"([0-9a-f]+)".*/\1/' || true)"
    if [ "$sha" = "$want" ]; then line="$line  = manifest"; else line="$line  != manifest (${want:-нет в описи})"; bad=$((bad + 1)); fi
  fi
  echo "$line   (временная папка: $work)"
done <<< "$VARIANTS"

[ "$n" -gt 0 ] || { echo "ни одного варианта не выбрано" >&2; exit 2; }
if [ "$bad" -gt 0 ]; then echo "[FAIL] вариантов с ошибкой или расхождением: $bad из $n" >&2; exit 1; fi
echo "[OK] собрано вариантов: $n -> $OUT"
