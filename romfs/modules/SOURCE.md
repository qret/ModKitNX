# Modules in romfs/modules — where they come from

These files are prebuilt **nxredirect** exefs modules (based on exlaunch), licensed **GPL-2.0**.
They are shipped as separate files inside the application's romfs (aggregation, not linking):
the application copies the chosen variant to `atmosphere/contents/<TitleID>/exefs/subsdkN` on the console.

Source code of the modules: `nxredirect/` in this repository — https://github.com/qret/ModKitNX/tree/main/nxredirect (license and build instructions: `nxredirect/LICENSE`, `nxredirect/README.md`)

| variant | file | size | sha256 |
|---|---|---|---|
| autofix | autofix/subsdk | 58099 | `ca3c5fefd1ea6cfa006a80112c5058a6dbbf66f830e5d6f81d0954bfb9dbbdf2` |
| autofix-earlyfs | autofix-earlyfs/subsdk | 58688 | `d9b2e757ed45438880552f3917c6f317cbee1ce5f3b65fb15b94e570c7f78a61` |
| plain | plain/subsdk | 27774 | `de415ac5f430020bcb92ecad6401db242cc6448738054a1daa8563784d78fc4b` |
| diag | diag/subsdk | 60664 | `1a93157c45ad4d952068a2ca46e89b3bd48ce4efb920749cdc4919338ddd1d44` |
| diag-gfx | diag-gfx/subsdk | 100711 | `896f7cdc27269f7fa2c6f1ed1822dd7b2d148d9427c329efa1199ce500eaf1c7` |
| plain-gfx | plain-gfx/subsdk | 90911 | `a2dd301ab15ad687027316d1439126ec06a85855b0e250171fe1ffa8156a7596` |
| autofix-diag | autofix-diag/subsdk | 89718 | `83ad7ea94f7efc953cd7ffbe19685186181035740207fa21046bcf06eea47995` |
| autofix-diag-gfx | autofix-diag-gfx/subsdk | 99692 | `79251e82c89bbb3c818dc4752093c02017b7fd332a7119c01c43acf5ccc9fd4a` |
| autofix-diag-earlyfs | autofix-diag-earlyfs/subsdk | 90341 | `eaedabb539566e065b5781e7b745be7f000e4108ae36990b2cc2ba4362000bbf` |

Each file can be rebuilt from the sources and compared byte for byte (WSL or Linux with devkitPro):
`bash nxredirect/build-romfs-modules.sh <build folder> --check romfs/modules`; sizes and sha256 are also in `manifest.json`.
This file and `manifest.json` are generated together with the modules; do not edit by hand.
