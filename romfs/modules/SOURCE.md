# Modules in romfs/modules — where they come from

These files are prebuilt **nxredirect** exefs modules (based on exlaunch), licensed **GPL-2.0**.
They are shipped as separate files inside the application's romfs (aggregation, not linking):
the application copies the chosen variant to `atmosphere/contents/<TitleID>/exefs/subsdkN` on the console.

Source code of the modules: `nxredirect/` in this repository — https://github.com/qret/ModKitNX/tree/main/nxredirect (license and build instructions: `nxredirect/LICENSE`, `nxredirect/README.md`)

| variant | file | size | sha256 |
|---|---|---|---|
| autofix | autofix/subsdk | 57321 | `c1b82fcca90394f6c63e0da5b7357c1eb07e08ba82596da94f0282da143f1fb3` |
| autofix-earlyfs | autofix-earlyfs/subsdk | 57883 | `d409de8978cc00b66c9cbaeb645f36719d94ac4548b1e91f3a32f9e3b75a9060` |
| plain | plain/subsdk | 27708 | `ef1cfa36b0d5bb1665fbb5024c5513577b8747cf42c48210d48fc3c037dccb31` |
| diag | diag/subsdk | 60595 | `2c1fe62253365ae23872b960a5c6698f364cfcd45af8c4240322d47f26acb453` |
| diag-gfx | diag-gfx/subsdk | 100703 | `2f060293e0aed3a633e846e7677b5c8b76276d5e32a4b3386ac6dcbd446d961a` |
| plain-gfx | plain-gfx/subsdk | 91136 | `92ef864d20ad4f709ce4c529e1e889e29458bd70949d9b679ac898685c3c9924` |
| autofix-diag | autofix-diag/subsdk | 88972 | `939090b27b1f95a0df5918608ea76b0f1d6700a385cedce6f80078ae46ef0526` |
| autofix-diag-gfx | autofix-diag-gfx/subsdk | 99657 | `4fa034fef604f9ae2cc664627e93143e5dbdcc8e80d7c28a85046c0bf936a98d` |
| autofix-diag-earlyfs | autofix-diag-earlyfs/subsdk | 89546 | `1f94bbb1474dc381cca50b49814e4ac12d3b4c9fad1dfb6e7e3cf2439d99fa4c` |

Each file can be rebuilt from the sources and compared byte for byte (WSL or Linux with devkitPro):
`bash nxredirect/build-romfs-modules.sh <build folder> --check romfs/modules`; sizes and sha256 are also in `manifest.json`.
This file and `manifest.json` are generated together with the modules; do not edit by hand.
