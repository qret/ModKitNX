# Modules in romfs/modules — where they come from

These files are prebuilt **nxredirect** exefs modules (based on exlaunch), licensed **GPL-2.0**.
They are shipped as separate files inside the application's romfs (aggregation, not linking):
the application copies the chosen variant to `atmosphere/contents/<TitleID>/exefs/subsdkN` on the console.

Source code of the modules: `nxredirect/` in this repository — https://github.com/qret/ModKitNX/tree/main/nxredirect (license and build instructions: `nxredirect/LICENSE`, `nxredirect/README.md`)

| variant | file | size | sha256 |
|---|---|---|---|
| autofix | autofix/subsdk | 57733 | `14c6031505ced14bdc806dbbd3baf3fdc55a7d7c2bac2a478a9b27b89ef5d564` |
| autofix-earlyfs | autofix-earlyfs/subsdk | 58314 | `fa4e99b38484598feb3c0db64b250854d40c97fd3b21132b399564a718900601` |
| plain | plain/subsdk | 27774 | `a74dc0958a2d8b879e63169b6b305c839fa8ac5a796f3eeb5657e9662fba6a71` |
| diag | diag/subsdk | 60664 | `186b7ff2fc60674a9072feaaa3f4f2201bf96d7025ef84846f97ad95d7832d57` |
| diag-gfx | diag-gfx/subsdk | 100711 | `2b9d98b9b80bca97348d8d7ede05a6ee563ca8155fe081d9c056caf231e263eb` |
| plain-gfx | plain-gfx/subsdk | 90911 | `ab5545ef9cea3ba44943aa0549501743419c0d9ee041555a102dff53a89733f8` |
| autofix-diag | autofix-diag/subsdk | 89331 | `80e77a72b908f180deeaa2c25472037223112da59bf1a9aac7c2eed08c4d7ba7` |
| autofix-diag-gfx | autofix-diag-gfx/subsdk | 99229 | `8c72dd6d955cf371cc8616b19886d2c2fdaf4c85c1894cdfa372f59cc98db782` |
| autofix-diag-earlyfs | autofix-diag-earlyfs/subsdk | 89986 | `b2027ea304f9ae5784d2f6963ea023febedfaece94c0e9aa93cacd9d26aa3ba4` |

Each file can be rebuilt from the sources and compared byte for byte (WSL or Linux with devkitPro):
`bash nxredirect/build-romfs-modules.sh <build folder> --check romfs/modules`; sizes and sha256 are also in `manifest.json`.
This file and `manifest.json` are generated together with the modules; do not edit by hand.
