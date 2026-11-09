# Translating ModKitNX · Перевод ModKitNX · Переклад ModKitNX

[English](#english) · [Русский](#русский) · [Українська](#українська)

## English

All interface text lives in plain JSON files, one folder per language:

```
resources/i18n/
├── languages.json      ← the list of languages (the only place to register one)
├── en-US/              ← English, the main language: every key is here
│   ├── app.json        ← screens
│   ├── core.json       ← install/remove plan, warnings and errors
│   ├── hints.json      ← button hints at the bottom (OK, Back…)
│   ├── update.json     ← the app update window
│   ├── theme.json      ← the theme choice in Settings
│   └── sdkscan.json    ← “Find sdk in other games”
├── ru/
└── uk/
```

A key missing in your language falls back to English, so a half-done translation still works.

**Fix or improve a translation.** Open the file of your language and change the text on the right of `:`.
Do not change the keys (the left side), keep `{0}`, `{1}`… as they are (they are replaced by paths, versions,
numbers; you may move them inside the sentence), and keep technical names as is: `sdk`, `nnSdk`, `ZBIC`,
`main.npdm`, `exefs`, `romfs`, `subsdk9`, folder paths.

**Add a new language.**
1. Copy the folder `resources/i18n/en-US` to `resources/i18n/<code>` — for example `de`, `pl`, `pt-BR`. The folder
   name is the language code.
2. Translate the values in every file of the folder: `app.json`, `core.json`, `hints.json`, `update.json`,
   `theme.json`, `sdkscan.json`.
3. Add one line to `resources/i18n/languages.json`: the code, the language's own name, its name in English and “Choose a language” in it:
   `{ "code": "de", "name": "Deutsch", "english": "German", "choose": "Sprache wählen" }`. No code changes are needed — the language
   appears on the first-start screen and in Settings.

The console's system font shows Latin and Cyrillic; for other scripts, check the screens before sending.

**Check.** Run `py -3 tools/i18n_check.py` (Python 3, nothing to install; on Linux/macOS `python3`). It checks that every
language has all keys of `en-US`, no extra keys, the same `{0}`-placeholders, non-empty values, UTF-8 without BOM
and LF line endings. `OK` and exit code 0 — ready. `py -3 tools/i18n_check.py --self-test` proves the checker itself
catches mistakes.

**Send.** Fork the repository, commit only your language folder and the line in `languages.json`, open a pull request.
Files: UTF-8 without BOM, LF line endings.

## Русский

Все тексты интерфейса — обычные JSON-файлы, по папке на язык:

```
resources/i18n/
├── languages.json      ← список языков (единственное место, где язык регистрируется)
├── en-US/              ← английский, основной язык: здесь есть все ключи
│   ├── app.json        ← экраны
│   ├── core.json       ← план установки и снятия, предупреждения и ошибки
│   ├── hints.json      ← подсказки кнопок внизу (OK, Назад…)
│   ├── update.json     ← окно обновления приложения
│   ├── theme.json      ← выбор темы в «Настройках»
│   └── sdkscan.json    ← «Найти sdk в других играх»
├── ru/
└── uk/
```

Если ключа нет в вашем языке, берётся английский — недоделанный перевод всё равно работает.

**Исправить или улучшить перевод.** Откройте файл своего языка и поменяйте текст справа от `:`. Ключи (слева) не
меняйте, `{0}`, `{1}`… оставьте (на их место подставляются пути, версии, числа; переставлять внутри фразы можно),
технические имена — как есть: `sdk`, `nnSdk`, `ZBIC`, `main.npdm`, `exefs`, `romfs`, `subsdk9`, пути к папкам.

**Добавить новый язык.**
1. Скопируйте папку `resources/i18n/en-US` в `resources/i18n/<код>` — например `de`, `pl`, `pt-BR`. Имя папки и есть
   код языка.
2. Переведите значения во всех файлах папки: `app.json`, `core.json`, `hints.json`, `update.json`, `theme.json`,
   `sdkscan.json`.
3. Добавьте одну строку в `resources/i18n/languages.json`: код, самоназвание языка, его имя по-английски и «Выберите язык» на нём:
   `{ "code": "de", "name": "Deutsch", "english": "German", "choose": "Sprache wählen" }`. Код править не нужно — язык появится на экране
   первого запуска и в «Настройках».

Системный шрифт консоли показывает латиницу и кириллицу; для других письменностей проверьте экраны до отправки.

**Проверить.** `py -3 tools/i18n_check.py` (Python 3, ставить ничего не нужно; в Linux/macOS — `python3`). Проверяет,
что в каждом языке есть все ключи `en-US`, нет лишних, плейсхолдеры `{0}` те же, значения не пустые, файлы — UTF-8 без
BOM и с LF. `OK` и код выхода 0 — готово. `py -3 tools/i18n_check.py --self-test` — негативная проба: сама проверка
ловит ошибки.

**Прислать.** Сделайте форк репозитория, закоммитьте только папку своего языка и строку в `languages.json`, откройте
pull request. Файлы — UTF-8 без BOM, окончания строк LF.

## Українська

Усі тексти інтерфейсу — звичайні JSON-файли, по теці на мову:

```
resources/i18n/
├── languages.json      ← список мов (єдине місце, де мову реєструють)
├── en-US/              ← англійська, основна мова: тут є всі ключі
│   ├── app.json        ← екрани
│   ├── core.json       ← план встановлення і видалення, попередження та помилки
│   ├── hints.json      ← підказки кнопок унизу (OK, Назад…)
│   ├── update.json     ← вікно оновлення застосунку
│   ├── theme.json      ← вибір теми в «Налаштуваннях»
│   └── sdkscan.json    ← «Знайти sdk в інших іграх»
├── ru/
└── uk/
```

Якщо ключа немає у вашій мові, береться англійський — недороблений переклад однаково працює.

**Виправити або покращити переклад.** Відкрийте файл своєї мови і змініть текст праворуч від `:`. Ключі (ліворуч) не
змінюйте, `{0}`, `{1}`… залиште (на їхнє місце підставляються шляхи, версії, числа; переставляти всередині фрази
можна), технічні назви — як є: `sdk`, `nnSdk`, `ZBIC`, `main.npdm`, `exefs`, `romfs`, `subsdk9`, шляхи до тек.

**Додати нову мову.**
1. Скопіюйте теку `resources/i18n/en-US` у `resources/i18n/<код>` — наприклад `de`, `pl`, `pt-BR`. Назва теки і є
   кодом мови.
2. Перекладіть значення в усіх файлах теки: `app.json`, `core.json`, `hints.json`, `update.json`, `theme.json`,
   `sdkscan.json`.
3. Додайте один рядок у `resources/i18n/languages.json`: код, власну назву мови, її назву англійською і «Оберіть мову» нею:
   `{ "code": "de", "name": "Deutsch", "english": "German", "choose": "Sprache wählen" }`. Код змінювати не треба — мова з'явиться на
   екрані першого запуску і в «Налаштуваннях».

Системний шрифт консолі показує латиницю і кирилицю; для інших писемностей перевірте екрани перед надсиланням.

**Перевірити.** `py -3 tools/i18n_check.py` (Python 3, нічого встановлювати не треба; у Linux/macOS — `python3`).
Перевіряє, що в кожній мові є всі ключі `en-US`, немає зайвих, плейсхолдери `{0}` ті самі, значення не порожні, файли —
UTF-8 без BOM і з LF. `OK` і код виходу 0 — готово. `py -3 tools/i18n_check.py --self-test` — негативна проба: сама
перевірка ловить помилки.

**Надіслати.** Зробіть форк репозиторію, закомітьте лише теку своєї мови і рядок у `languages.json`, відкрийте
pull request. Файли — UTF-8 без BOM, закінчення рядків LF.
