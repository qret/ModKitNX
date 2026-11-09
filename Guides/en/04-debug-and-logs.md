# If a game or a mod does not work: debug build and logs

[← Guide](README.md) · [На русском](../ru/04-debug-and-logs.md)

1. On the game's page tick **“Debug build”** (right above **Install**) and press **Install**. The regular module is
   replaced cleanly, without leftovers.
2. Start the game once (until it freezes, crashes or shows a black screen), then open ModKitNX again.
3. On the game's page open **“Debug report”**: the app reads the report and says in plain words what went wrong — for
   example, the mod folder is not where it should be, or the first failed graphics call. Reports are kept in
   `sdmc:/switch/ModKitNX/logs/<TitleID>/` (the last 5 starts).
4. To ask for help, press **“Collect logs to send”**. The app puts the game's reports, the latest Atmosphère crash
   report for this game, a short console passport (firmware and Atmosphère versions) and the install receipt into
   `sdmc:/switch/ModKitNX/logs/<Game> v<version>/` and packs them into a `.zip` next to it. There are no keys and no
   game files inside.
5. Send the `.zip` to the **4IFIR group on Telegram — [t.me/kf4fr](https://t.me/kf4fr)**, or
   [open an issue](https://github.com/qret/ModKitNX/issues/new/choose) on GitHub and attach it.

When everything works, untick **“Debug build”** and press **Install** again — the regular module goes back in place.
