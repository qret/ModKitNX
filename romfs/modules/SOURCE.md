# Modules in romfs/modules — where they come from

These files are prebuilt **nxredirect** exefs modules (based on exlaunch), licensed **GPL-2.0**.
They are shipped as separate files inside the application's romfs (aggregation, not linking):
the application copies the chosen variant to `atmosphere/contents/<TitleID>/exefs/subsdkN` on the console.

Source code of the modules: `nxredirect/` in this repository — https://github.com/qret/ModKitNX/tree/main/nxredirect (license and build instructions: `nxredirect/LICENSE`, `nxredirect/README.md`)

| variant | file | size | sha256 |
|---|---|---|---|
| autofix | autofix/subsdk | 55116 | `4b0256c65e530c5532ec4cfaf462ccf1830372f16c12c5522f71dc4098b23f8a` |
| autofix-earlyfs | autofix-earlyfs/subsdk | 55745 | `5ffca1737ecda9f6f271af39df855daa1416934ed9a37cb833e4873cfe2b3c05` |
| plain | plain/subsdk | 27708 | `46a071a2fd269a03ef1f4a1d5a064bc5c2979750c7c544cf1934c08befb02040` |
| diag | diag/subsdk | 60595 | `b11c8448c5144f2d64926fb4912c8468ff2747c331716873e8840b266e77219d` |
| diag-gfx | diag-gfx/subsdk | 100703 | `34959e376ff8566b96dc5c5f56d238e5d0b764feb3083160eb1e0d4ed9b91acc` |
| plain-gfx | plain-gfx/subsdk | 91128 | `bc3ee846c15b48f7613dbd9e07b1445f8a792486f4bfcd72987055e08cf5ddd9` |
| autofix-diag | autofix-diag/subsdk | 86727 | `203873f2478840e1f7b283459a5c5abc50029030f69ba60af6a0a047ce3f9d1a` |
| autofix-diag-gfx | autofix-diag-gfx/subsdk | 94494 | `9e71d94c91e47459ce5a49c7f670c714628404c5ce28d0364c0b891db7931e93` |
| autofix-diag-earlyfs | autofix-diag-earlyfs/subsdk | 87309 | `a929d81a96de288463f690c467ed3a560d1a86e3f892a7228c14bb1fa9fc8a06` |

Each file can be rebuilt from the sources and compared byte for byte (WSL or Linux with devkitPro):
`bash nxredirect/build-romfs-modules.sh <build folder> --check romfs/modules`; sizes and sha256 are also in `manifest.json`.
This file and `manifest.json` are generated together with the modules; do not edit by hand.
