# sdk files

[← Guide](README.md) · [На русском](../ru/02-sdk.md)

Needed for games newer than your firmware. Dump the system library (`nnSdk`) from **your own** console and put the
files into `sdmc:/switch/ModKitNX/sdk/` (subfolders are fine). The app also looks into `sdmc:/switch/DBI/dumps/SDK/`.
Files are recognised by their content, not by name, and the version is shown next to each one.

No suitable sdk file? Every game carries its own sdk inside. On the game's page press **“Find sdk in other games”**:
the app looks through your installed games, offers the sdk that suits your firmware and saves a copy to
`sdmc:/switch/ModKitNX/sdk/`, signed with the game it came from. Nothing is downloaded.
