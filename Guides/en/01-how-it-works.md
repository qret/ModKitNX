# How it works

[← Guide](README.md) · [На русском](../ru/01-how-it-works.md)

## What it does

- **Unpacks ZBIC.** Games built for firmware 22 pack their main file in a new format that older Atmosphère cannot
  open. ModKitNX unpacks it once. On firmware 22+ or Atmosphère 1.12+ the system reads ZBIC itself, and the app leaves
  it alone.
- **Swaps the game's sdk** — the set of system libraries inside the game — for one that fits your firmware. Dump it
  from your own console, or press **Find sdk in other games**: the app takes a suitable one from the games you have
  installed. Nothing is downloaded.
- **Adds a small module to the game** that fills in the functions the older sdk lacks or has only as empty stubs.
  One such stub gave Well Dweller a black screen.
- **Grants the game the permissions** the module needs.

One button does it all, and you see the plan first. One button undoes it all: the installed game itself is never
touched — everything lives in its own folder and is removed cleanly.

**Bonus — mods without LayeredFS.** The same module loads translations, graphics and textures directly. LayeredFS
keeps a list of every file of the game in Atmosphère's small memory, and big games with heavy mods don't fit — they
freeze or crash. With ModKitNX they work, on any Atmosphère and any CFW build.

## Who it is for

- You stay on a lower firmware than your games need and don't want to update.
- Your heavy mods or translations freeze through LayeredFS.
- You want a fix that is not tied to one Atmosphère version or one particular CFW build.

## Also

- Each game's page shows what the game needs on your firmware and which sdk fits.
- A debug build and **Collect logs to send** — for when something does not start.
- English, Russian and Ukrainian; a light or dark theme that follows the console; updates itself from GitHub releases.
- Open source, GPL-3.0-or-later.

Built on the work of **Луня (Lunya) — [Langegen](https://github.com/Langegen)**, author of
[TorrentShopNX](https://github.com/Langegen/TorrentShopNX) — see [Thanks](#thanks).

## Before → after: real cases

Real cases, checked on a console (firmware 19.0.1, Atmosphère 1.11.1). Everything ModKitNX makes lies in the game's
folder `sdmc:/atmosphere/contents/<TitleID>/`.

- **SiN Reloaded** (needs firmware 22.5.0, console on 19.0.1, Atmosphère 1.11.1).
  *Before:* "Could not start the software". *After:* the game works.
  *How:* the game's main file is packed like a zip archive, in the new ZBIC format that older Atmosphère cannot open —
  ModKitNX unpacks it once. And the set of system tools inside the game (the sdk) is made for firmware 22 — ModKitNX
  gives the game the set that fits your firmware.
- **Well Dweller** (the same firmware).
  *Before:* after the same two steps — a black screen. *After:* the game works.
  *How:* the older sdk lacks some functions or has them only as empty stubs. Here the game asks the graphics system
  for the size of its window buffer and gets nothing back. The module ModKitNX adds to the game puts a working
  version in place of the stub. What the module fixes for a game is shown on the game's page.
- **Russian translation of Trails in the Sky 2nd Chapter.**
  *Before:* the game froze at startup; the translation was shipped with a modified Atmosphère (`package3`) that only
  worked on the UltraNX CFW build. *After:* the translation works for everyone, on any Atmosphère and any build.
  *How:* LayeredFS, the usual way to load a mod, keeps a list of all the game's files in Atmosphère's small memory, and
  this game has tens of thousands of them. The module needs no list: when the game opens a file, it hands over the
  mod's file with the same name.
- **Tears of the Kingdom + the UltraCam graphics mod.**
  *Before:* the game crashed with the mod — on firmware 20 and newer system modules get even less memory.
  *After:* the game works with the mod. *How:* the same as Trails — the module loads the mod's files without LayeredFS.

To let the module read the SD card, ModKitNX adds just the permissions it needs to the game's "passport"
(`main.npdm`), and nothing more.
