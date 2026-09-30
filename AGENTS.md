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
- Patches to the Lua sources apply once each, keyed on text they add
  (`lua_patch` in `CMakeLists.txt`), so an existing `libraries/lua-5.1.5` picks up
  a new one on the next build.
- `bash tests/lua-number-tests.sh` tests `source/lua-number.c` on the host; it
  needs only `cc`.
- `bash tests/host-tests.sh` compares `source/tpbot.c` with the Lua it
  replaced, `tests/tpbot-reference.lua`, and runs `source/lua-script.lua` over
  a stand-in board; it needs `cc` and a firmware build's `libraries/lua-5.1.5`.
