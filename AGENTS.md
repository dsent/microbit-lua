# Agent notes for opencode

## Build
- Use `python3 build.py --clean` as the entry point (not cmake directly)
- Outputs `MICROBIT.hex` and `MICROBIT.bin` in the project root (`build/` holds the ELF + map)
- Dependencies are pinned via `"branches"` in `codal.json`: each key is the repo URL to
  clone from (may be a fork) and each value the ref (SHA or branch). Entries are matched
  to `target.json` dependencies by repo NAME — same semantics in the CMake fresh-clone
  path (`find_dependency_override`) and `build.py --update`. Update those SHAs to bump.
- `build.py --clean` only wipes `build/`; `libraries/` is reused as-is. Run
  `./build.py --update` after changing pins (the build warns if a 40-hex pin differs).
- Patches to the Lua sources are listed with `lua_patch` in `CMakeLists.txt` and
  applied once each by `source/lua-patch.cmake`: an existing
  `libraries/lua-5.1.5` takes a patch added after a file's last one on the next
  build. Anything else, a patch inserted before others of its file, dropped or
  edited, stops the build and says to delete `libraries/lua-5.1.5`.
- `bash tests/lua-number-tests.sh` tests `source/lua-number.c` on the host; it
  needs only `cc`.
- `bash tests/host-tests.sh` compares `source/tpbot.c` with the Lua it
  replaced, `tests/tpbot-reference.lua`, and runs `source/lua-script.lua` over
  a stand-in board; it needs `cc`, `patch`, `cmake` and the Lua tarball a
  firmware build leaves in `libraries/`.
