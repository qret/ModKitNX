#!/usr/bin/env python3
# ModKitNX — сборка шрифта интерфейса resources/font/font.ttf из Noto Sans Regular (SIL OFL 1.1).
#
# Зачем: borealis на Switch берёт romfs:/font/font.ttf как ОСНОВНОЙ шрифт всего текста (switch_font.cpp,
# USER_FONT_PATH → FONT_CHINESE_SIMPLIFIED = шрифт по умолчанию). Без него кириллица рисуется китайским системным
# шрифтом с широкими глифами («Д о б р о   п о ж а л о в а т ь»). Шрифт Nintendo
# (как у TorrentShopNX и в resources/ borealis) не кладём: он не наш, репозиторий публичный.
#
# Что делает (воспроизводимо, без сети):
#   1. проверяет sha256 архива релиза NotoSans-v2.015 (notofonts/latin-greek-cyrillic);
#   2. берёт NotoSans/unhinted/ttf/NotoSans-Regular.ttf и OFL.txt;
#   3. подмножество: Latin + Latin-1 + Latin Extended-A, кириллица (рус., укр. ї є і ґ), типографика и знаки UI;
#      без GSUB/GPOS/хинтинга (nanovg их не читает);
#   4. масштаб ×1.06 (unitsPerEm 1000 → 943) и вертикальные метрики как у системного шрифта Switch
#      (ascender : descender = 0.88 : 0.12): высота и ширина строк близки к раскладкам borealis, рассчитанным
#      на шрифт Nintendo (у Noto заглавные ниже: 0.714 em против 0.788 em, текст уже на ~6 %).
#
# Запуск:  py -3 tools/make_font.py --zip <путь к NotoSans-v2.015.zip>
#   архив: https://github.com/notofonts/latin-greek-cyrillic/releases/download/NotoSans-v2.015/NotoSans-v2.015.zip
#   нужен fonttools:  py -3 -m pip install --user fonttools
import argparse
import hashlib
import io
import os
import sys
import zipfile

ZIP_SHA256 = "0c34df072a3fa7efbb7cbf34950e1f971a4447cffe365d3a359e2d4089b958f5"
TTF_IN_ZIP = "NotoSans/unhinted/ttf/NotoSans-Regular.ttf"
OFL_IN_ZIP = "OFL.txt"

UNICODES = [
    (0x0020, 0x007E),   # ASCII
    (0x00A0, 0x00FF),   # Latin-1: « » · × è é ® ° …
    (0x0100, 0x017F),   # Latin Extended-A: названия игр (ł ő š …)
    (0x0400, 0x045F),   # кириллица: русский, украинский (Є І Ї є і ї), белорусский
    (0x0490, 0x0491),   # Ґ ґ
    (0x2010, 0x2027),   # ‐ – — ‘ ’ “ ” • … и др.
    (0x2030, 0x203A),   # ‰ ‹ ›
    (0x20AC, 0x20AC),   # €
    (0x2116, 0x2116),   # №
    (0x2122, 0x2122),   # ™
    # ← ↑ → ↓ (U+2190–2193): в Noto Sans их нет — рисуются здесь (add_arrows)
    (0x2212, 0x2212),   # −
    (0x2260, 0x2260),   # ≠
    (0x2264, 0x2265),   # ≤ ≥
    (0x25CB, 0x25CB),   # ○
    (0x25CF, 0x25CF),   # ●
]
SCALE_UPM = 943          # 1000 / 1.06
ASCENT, DESCENT = 830, -113   # 0.88 : 0.12 от 943 — как hhea системного шрифта Switch (901 : 123 из 1024)


def add_arrows(font) -> None:
    """Стрелки ← ↑ → ↓ (в интерфейсе: «Payloads → Lockpick_RCM»): в Noto Sans Regular их нет, а запасной шрифт на
    консоли — китайский системный (широкий глиф). Простая стрелка: древко толщиной как у «−», ось — на его высоте."""
    from fontTools.pens.ttGlyphPen import TTGlyphPen

    cy, half, head = 352, 38, 230                      # ось и толщина «−» (317..388), полувысота наконечника
    right = [(60, cy + half), (560, cy + half), (560, cy + head), (800, cy),
             (560, cy - head), (560, cy - half), (60, cy - half)]   # по часовой (TrueType, y вверх)
    adv_h, adv_v, cx, y0 = 860, 600, 300, -10

    def shapes():
        yield 0x2192, "arrowright", adv_h, right
        yield 0x2190, "arrowleft", adv_h, [(adv_h - x, y) for x, y in reversed(right)]
        up = [(cx - (y - cy), y0 + (x - 60)) for x, y in right]        # поворот на +90° — обход сохраняется
        yield 0x2191, "arrowup", adv_v, up
        yield 0x2193, "arrowdown", adv_v, [(cx + (y - cy), y0 + 740 - (x - 60)) for x, y in right]

    order = list(font.getGlyphOrder())
    for code, name, adv, pts in shapes():
        pen = TTGlyphPen(None)
        pen.moveTo(pts[0])
        for pt in pts[1:]:
            pen.lineTo(pt)
        pen.closePath()
        glyph = pen.glyph()
        glyph.recalcBounds(font["glyf"])
        font["glyf"][name] = glyph
        font["hmtx"][name] = (adv, glyph.xMin)
        if name not in order:
            order.append(name)
        for t in font["cmap"].tables:
            if t.isUnicode():
                t.cmap[code] = name
    font.setGlyphOrder(order)
    font["glyf"].glyphOrder = order


def main() -> int:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--zip", required=True, help="NotoSans-v2.015.zip")
    ap.add_argument("--out", default=os.path.join(root, "resources", "font"))
    a = ap.parse_args()

    data = open(a.zip, "rb").read()
    got = hashlib.sha256(data).hexdigest()
    if got != ZIP_SHA256:
        print(f"FAIL sha256 архива {got} != {ZIP_SHA256}")
        return 1
    try:
        from fontTools import subset
        from fontTools.ttLib import TTFont
    except ImportError:
        print("FAIL нет fonttools: py -3 -m pip install --user fonttools")
        return 1

    with zipfile.ZipFile(io.BytesIO(data)) as z:
        ttf = z.read(TTF_IN_ZIP)
        ofl = z.read(OFL_IN_ZIP)

    font = TTFont(io.BytesIO(ttf))
    opts = subset.Options()
    opts.layout_features = []
    opts.drop_tables += ["GSUB", "GPOS", "GDEF", "DSIG"]
    opts.hinting = False
    opts.glyph_names = False
    opts.notdef_outline = True
    opts.name_IDs = ["*"]
    opts.name_languages = ["*"]
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=[u for lo, hi in UNICODES for u in range(lo, hi + 1)])
    sub.subset(font)

    add_arrows(font)
    font["head"].unitsPerEm = SCALE_UPM
    hhea = font["hhea"]
    hhea.ascent, hhea.descent, hhea.lineGap = ASCENT, DESCENT, 0
    os2 = font["OS/2"]
    os2.sTypoAscender, os2.sTypoDescender, os2.sTypoLineGap = ASCENT, DESCENT, 0

    os.makedirs(a.out, exist_ok=True)
    out_ttf = os.path.join(a.out, "font.ttf")
    font.save(out_ttf)
    with open(os.path.join(a.out, "OFL.txt"), "wb") as f:
        f.write(ofl.replace(b"\r\n", b"\n"))
    h = hashlib.sha256(open(out_ttf, "rb").read()).hexdigest()
    print(f"ok   {out_ttf}: {os.path.getsize(out_ttf)} байт, глифов {len(font.getGlyphOrder())}, sha256 {h}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
