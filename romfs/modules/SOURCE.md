# Modules in romfs/modules — where they come from

These files are prebuilt **nxredirect** exefs modules (based on exlaunch), licensed **GPL-2.0**.
They are shipped as separate files inside the application's romfs (aggregation, not linking):
the application copies the chosen variant to `atmosphere/contents/<TitleID>/exefs/subsdkN` on the console.

Source code of the modules: `nxredirect/` in this repository — https://github.com/qret/ModKitNX/tree/main/nxredirect (license and build instructions: `nxredirect/LICENSE`, `nxredirect/README.md`)

| variant | file | size | sha256 |
|---|---|---|---|
| autofix | autofix/subsdk | 55116 | `54847f38686218fcf5713f49562325a3f23b7412173177c98f605113d58674f1` |
| autofix-earlyfs | autofix-earlyfs/subsdk | 55697 | `16af9d82cac2255348262a5d04317c84226413f4c6b7c1dfee545720824a3a3b` |
| plain | plain/subsdk | 27716 | `eb91ab709898c0b4837334cc3477e470b058999ca3da792e011028556641e8c7` |
| diag | diag/subsdk | 60652 | `2b8f64bae5cc6bef3ec24f9e1b8c84836e4b51a771e2a9ea45b3cb2d4859a9bd` |
| diag-gfx | diag-gfx/subsdk | 100676 | `493f2fd8ed1331d12b2da8654051ba66f6def5b07dc28ca8b275b6ec450a3f02` |
| plain-gfx | plain-gfx/subsdk | 91162 | `126b7736f943a256eb374805fef87e0c2b2c80326dbc75d6529eab48e6b41a55` |
| autofix-diag | autofix-diag/subsdk | 86788 | `ae4669bb8ef6d2c0da3b5ee02b08f3b519f2efac64cf01ffc6ba3f688272027e` |
| autofix-diag-gfx | autofix-diag-gfx/subsdk | 94422 | `624106993017d8c208bf57c9f19063b09f2f7d9ebbc96436dfcdd21357ba4608` |
| autofix-diag-earlyfs | autofix-diag-earlyfs/subsdk | 87352 | `8d0f7220a6017295b7886821ad60c029ecf217effb342f15e2d3d1b66eaac035` |

Each file can be rebuilt from the sources and compared byte for byte (WSL or Linux with devkitPro):
`bash nxredirect/build-romfs-modules.sh <build folder> --check romfs/modules`; sizes and sha256 are also in `manifest.json`.
This file and `manifest.json` are generated together with the modules; do not edit by hand.
