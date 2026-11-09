# Ресурсы: откуда взято

Картинки и иконки — из **TorrentShopNX by Langegen (Луня)**, GPL-3.0, <https://github.com/Langegen/TorrentShopNX>,
коммит `744e7430604ff1f541026d0e2675a3ecbda97bb7` (клон 08.11.2026).
Остальные ресурсы (XML экранов, i18n) — наши, GPL-3.0-or-later; раскладка строки списка игр сделана по образцу
TorrentShopNX (пометка в шапке `xml/game_cell.xml`).

| Файл у нас | Путь в оригинале | sha256 | Что это |
|---|---|---|---|
| ~~`img/torrentshopnx-icon-reference.jpg`~~ — перенесена из `resources/` во внутренние документы проекта (не публикуются) | `resources/icon.jpg` | `771de74c…5d8ed6f` | иконка TorrentShopNX как образец стиля; приложением НЕ используется, в `.nro` не входит |
| `img/cover_blank.png` | `resources/img/cover_blank.png` | `e76c7825…b60966` | заглушка, если у игры нет иконки |

`icon.jpg` — СВОЯ иконка ModKitNX: иконка TorrentShopNX несёт надпись «TSNX» и логотип
Nintendo Switch — чужое имя и торговая марка, поэтому как иконка приложения не годится; оригинал сохранён как образец во внутренних документах (не в `resources/`).

## Шрифт интерфейса

`font/font.ttf` — подмножество **Noto Sans Regular** v2.015 (The Noto Project Authors, **SIL OFL 1.1**, лицензия —
`font/OFL.txt` рядом), архив релиза <https://github.com/notofonts/latin-greek-cyrillic/releases/tag/NotoSans-v2.015>
(`NotoSans-v2.015.zip`, sha256 `0c34df07…89b958f5`), файл `NotoSans/unhinted/ttf/NotoSans-Regular.ttf`.
Изменения (fonttools): подмножество — латиница, Latin-1, Latin Extended-A, кириллица с украинскими буквами,
типографика; без GSUB/GPOS и хинтинга; масштаб ×1.06 (unitsPerEm 943) и вертикальные метрики 0.88 : 0.12 — под
раскладки borealis; стрелки ← ↑ → ↓ (в Noto Sans их нет) дорисованы простыми контурами. Результат: sha256
`cc85633e…54a7cae8`.
Шрифт Nintendo (как у TorrentShopNX и в ресурсах borealis) не используется: он не наш, раздавать его нельзя.
