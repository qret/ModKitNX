# ModKitNX

**English** · [Русский](#modkitnx--по-русски)

Play games made for a newer firmware on yours — right on the Switch. Plus mods without LayeredFS.

Your console runs firmware 19.0.1, and a new game wants 22 — it does not start: "Could not start the software", or a
black screen. **ModKitNX** prepares such a game for *your* firmware right on the console: unpacks ZBIC, swaps the
game's sdk, adds a module that fills in missing functions and grants the needed permissions. One button — and one
button back: the installed game itself is never touched.

The same module loads mods — translations, graphics — without LayeredFS, so big games with heavy mods stop freezing.

<table>
<tr><td width="50%"><img src="screenshots/readme/01-language.png" alt="Language at first start"></td><td width="50%"><img src="screenshots/readme/02-games.png" alt="Game list"></td></tr>
<tr><td width="50%"><img src="screenshots/readme/03-game-sdk.png" alt="Game page: sdk verdict"></td><td width="50%"><img src="screenshots/readme/04-module-fixes.png" alt="What the module fixes"></td></tr>
<tr><td width="50%"><img src="screenshots/readme/05-plan.png" alt="Install plan"></td><td width="50%"><img src="screenshots/readme/06-result.png" alt="Result"></td></tr>
</table>

<sub>Rendered on a PC with sample data; light and dark themes.</sub>

## Installing

1. Download `ModKitNX.nro` from **[the latest release](../../releases/latest)** and copy it to
   `sdmc:/switch/ModKitNX/`.
2. Launch any game **while holding R** — the homebrew menu opens with full memory; start ModKitNX there.
3. For a game newer than your firmware, the app needs an sdk that fits: put one into `sdmc:/switch/ModKitNX/sdk/`
   or press **Find sdk in other games** — [details](Guides/en/02-sdk.md).

### 📖 [Guide](Guides/en/README.md)

How it works and real cases, sdk files, mods, debug build and logs, console keys, building.
[На русском](Guides/ru/README.md).

## Thanks

To **Луня (Lunya) — [Langegen](https://github.com/Langegen)** and
**[TorrentShopNX](https://github.com/Langegen/TorrentShopNX)** — the game list, titles and icons, the game's sdk
version and the interface are based on it. To **Duckbill**, the author of DBI. Heartfelt thanks to the **4IFIR
community** — it keeps me motivated, and it is the best: **[Telegram](https://t.me/kf4fr)** ·
**[GitHub](https://github.com/rashevskyv/4ifir-checker/releases)**. Full list — [`THANKS.md`](THANKS.md).

<a href="https://t.me/kf4fr"><img src=".github/images/telegram-kf4fr.png" width="120" alt="QR: t.me/kf4fr"></a>

**License:** GPL-3.0-or-later ([`LICENSE`](LICENSE)); the game module in `nxredirect/` — GPL-2.0.
**Disclaimer:** use at your own risk; do not go online with CFW or modified games — a ban is possible. Not affiliated
with Nintendo.

---

# ModKitNX — по-русски

[English](#modkitnx) · **Русский**

Запускайте игры, рассчитанные на прошивку новее, на своей — прямо на Switch. Плюс моды без LayeredFS.

На консоли прошивка 19.0.1, а новая игра требует 22-ю — и не запускается: «Не удалось запустить программу» или чёрный
экран. **ModKitNX** прямо на консоли готовит такую игру к *вашей* прошивке: снимает сжатие ZBIC, подменяет sdk игры,
добавляет модуль, который восполняет недостающие функции, и даёт игре нужные права. Одна кнопка — и одна кнопка
назад: сама установленная игра не меняется.

Тот же модуль подключает моды — переводы, графику — без LayeredFS, и большие игры с тяжёлыми модами перестают зависать.

Снимки экрана — [выше](#modkitnx).

## Установка

1. Скачайте `ModKitNX.nro` из **[последнего релиза](../../releases/latest)** и положите в `sdmc:/switch/ModKitNX/`.
2. Запустите любую игру, **удерживая R**, — откроется меню homebrew с полной памятью; там запустите ModKitNX.
3. Для игры новее прошивки нужна подходящая sdk: положите её в `sdmc:/switch/ModKitNX/sdk/` или нажмите
   **«Найти sdk в других играх»** — [подробнее](Guides/ru/02-sdk.md).

### 📖 [Руководство](Guides/ru/README.md)

Как это работает и настоящие случаи, файлы sdk, моды, отладка и логи, ключи консоли, сборка.
[In English](Guides/en/README.md).

## Благодарности

**Луне — [Langegen](https://github.com/Langegen)** и **[TorrentShopNX](https://github.com/Langegen/TorrentShopNX)**:
список игр, названия и иконки, версия sdk игры и интерфейс сделаны по нему. **Duckbill**, автору DBI. От всей души
спасибо **сообществу 4IFIR** — оно мотивирует, и оно лучшее: **[Telegram](https://t.me/kf4fr)** ·
**[GitHub](https://github.com/rashevskyv/4ifir-checker/releases)**. Полный список — [`THANKS.md`](THANKS.md).

**Лицензия:** GPL-3.0-or-later ([`LICENSE`](LICENSE)); модуль для игр в `nxredirect/` — GPL-2.0.
**Отказ от ответственности:** на свой риск; не выходите в сеть с CFW и изменёнными играми — возможен бан. Проект не
связан с Nintendo.
