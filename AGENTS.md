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
