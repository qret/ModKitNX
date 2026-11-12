# Modules in romfs/modules — where they come from

These files are prebuilt **nxredirect** exefs modules (based on exlaunch), licensed **GPL-2.0**.
They are shipped as separate files inside the application's romfs (aggregation, not linking):
the application copies the chosen variant to `atmosphere/contents/<TitleID>/exefs/subsdkN` on the console.

Source code of the modules: `nxredirect/` in this repository — https://github.com/qret/ModKitNX/tree/main/nxredirect (license and build instructions: `nxredirect/LICENSE`, `nxredirect/README.md`)

| variant | file | size | sha256 |
|---|---|---|---|
| autofix | autofix/subsdk | 58323 | `9234d1f7de996ba37536cb552f6d5f5f693fa79acced9935891032e9ebd1c71e` |
| autofix-earlyfs | autofix-earlyfs/subsdk | 58906 | `6b4750a21ced15ba2eb49d6ca6cfac78b74ce70deaf9675d549d2292f22d18da` |
| plain | plain/subsdk | 27774 | `3edefcfb8e4786e4245cb916d37a71d85d71d2ba955340db1894599940bfd082` |
| diag | diag/subsdk | 60664 | `143015a80328827daff30f742dec7efefa76a942d9def8423bfbd679f96e9b01` |
| diag-gfx | diag-gfx/subsdk | 100975 | `67be3328f483520ec2c91847a6609831c3c35556f0539cb2b81d6364e4028920` |
| plain-gfx | plain-gfx/subsdk | 91037 | `3c02e53343394e2fc52b934ae342667e6dfe13a28d9925c7031a9492dbbb1428` |
| autofix-diag | autofix-diag/subsdk | 89925 | `274249523328026ad5dc275814129b9fd2361b593a0c12058a919a558eb426e2` |
| autofix-diag-gfx | autofix-diag-gfx/subsdk | 100550 | `086917f554f27d0f07ae7924dbec774d015bfc7622176cb762980eec6ac64290` |
| autofix-diag-earlyfs | autofix-diag-earlyfs/subsdk | 90555 | `6c22b952429abbcd10b55921f5dd379983f6fcb781e606a9e594b11ba1a40816` |

Each file can be rebuilt from the sources and compared byte for byte (WSL or Linux with devkitPro):
`bash nxredirect/build-romfs-modules.sh <build folder> --check romfs/modules`; sizes and sha256 are also in `manifest.json`.
This file and `manifest.json` are generated together with the modules; do not edit by hand.
