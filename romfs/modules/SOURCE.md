# Modules in romfs/modules — where they come from

These files are prebuilt **nxredirect** exefs modules (based on exlaunch), licensed **GPL-2.0**.
They are shipped as separate files inside the application's romfs (aggregation, not linking):
the application copies the chosen variant to `atmosphere/contents/<TitleID>/exefs/subsdkN` on the console.

Source code of the modules: `nxredirect/` in this repository — https://github.com/qret/ModKitNX/tree/main/nxredirect (license and build instructions: `nxredirect/LICENSE`, `nxredirect/README.md`)

| variant | file | size | sha256 |
|---|---|---|---|
| autofix | autofix/subsdk | 58315 | `87805c5c75713c899233fba2a112aab604e1d1c22cf8a23d1303517e51af8fe8` |
| autofix-earlyfs | autofix-earlyfs/subsdk | 58911 | `2b5b28dca6cca99d77ed57312d19bb92eba172474018c9d8fc639676f27e1023` |
| plain | plain/subsdk | 27708 | `2cdd904aa4c0e3f7429265980618ae689006983fdd9d79f6b8c8eefa8a3988e3` |
| diag | diag/subsdk | 60595 | `b2d8598d9203bd9bbf98cfa3743fd4eae0048c34c03af36dd1f6bbfaf19fc08b` |
| diag-gfx | diag-gfx/subsdk | 100703 | `cb02c7d172f5a96d2d7ad5d36b3754fd267de5a3fed755d38effd6d4adc703ef` |
| plain-gfx | plain-gfx/subsdk | 91136 | `7fdc6974b59a4152e56f8b377e201b34de791e586f71fa832df82763ca25e8ce` |
| autofix-diag | autofix-diag/subsdk | 89876 | `e8ee27e2b7468c9d1e28c207d4d92ffd024aa82e08c2777dd659a0d41cd7f155` |
| autofix-diag-gfx | autofix-diag-gfx/subsdk | 99340 | `30fd5d4213d1f50e37677a78ab74321d7d694dda0d11e527a906d1ebafad8ee5` |
| autofix-diag-earlyfs | autofix-diag-earlyfs/subsdk | 90513 | `f3bd380a4df5f29049de2bdaef4c9d5356a5ea0d6bbf6c55b93a7d77cf9ab85a` |

Each file can be rebuilt from the sources and compared byte for byte (WSL or Linux with devkitPro):
`bash nxredirect/build-romfs-modules.sh <build folder> --check romfs/modules`; sizes and sha256 are also in `manifest.json`.
This file and `manifest.json` are generated together with the modules; do not edit by hand.
