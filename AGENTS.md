# Agent notes for opencode

## Environment
- Passwordless sudo is granted for package management only:
  `/usr/bin/apt-get`, `/usr/bin/apt`, `/usr/bin/dpkg`. Other sudo commands need
  a password. Use it to install build dependencies globally, e.g.
  `sudo apt-get install -y --no-install-recommends <pkgs>`.
  (Do not pass env vars through sudo; `DEBIAN_FRONTEND=...` is rejected.)
- Installed build deps: cmake, gcc-arm-none-eabi + binutils + newlib/libstdc++,
  git, make, patch, diffutils, less, python3, lua5.1-5.4.

## Build
- Use `python3 build.py --clean` as the entry point (not cmake directly)
- Outputs `MICROBIT.hex` and `MICROBIT.bin` in the project root (`build/` holds the ELF + map)
- Dependencies are pinned via `"branches"` in `codal.json`: each key is the repo URL to
  clone from (may be a fork) and each value the ref (SHA or branch). Entries are matched
  to `target.json` dependencies by repo NAME — same semantics in the CMake fresh-clone
  path (`find_dependency_override`) and `build.py --update`. Update those SHAs to bump.
- `build.py --clean` only wipes `build/`; `libraries/` is reused as-is. Run
  `./build.py --update` after changing pins (the build warns if a 40-hex pin differs).
- Patches to the Lua sources live in `source/` and are listed in order with
  `lua_patch(FILE PATCH)` in `CMakeLists.txt`, before `lua_patch_apply()`. Every
  configure applies them to fresh sources from the tarball, under `build/`, and
  stops on a patch that does not apply. `libraries/` keeps only the tarball; a
  `libraries/lua-5.1.5` left by older builds is unused and can be deleted.
- `bash tests/lua-number-tests.sh` tests `source/lua-number.c` on the host; it
  needs only `cc`.
- `bash tests/cstack-host-tests.sh` tests `source/lua-cstack.c`'s limit; it
  needs `cc` and the Lua tarball a firmware build leaves in `libraries/`.
- `bash tests/host-tests.sh` checks the frames `source/tpbot.c` writes over
  a stand-in bus, and runs the C stack test above; it needs `cc`, `patch`
  and the Lua tarball a firmware build leaves in `libraries/`.
- The build itself needs no host Lua. `tests/hextract-tests.sh` does: `utils/hextract`
  pins its interpreter with `#!/usr/bin/env lua5.1`, and the harness runs the script
  directly, so only the `lua5.1` package is required (no PATH or alternatives setup).
