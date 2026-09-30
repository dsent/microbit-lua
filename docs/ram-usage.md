# RAM usage and how to lower it

Numbers are for the nRF52833 micro:bit v2 build in this repo. Static sizes come
from `build/MICROBIT.map`; Lua-heap figures are on-device `LUA_MEM_DEBUG`
readings (see *Method*).

## Part 1 — where the RAM goes

Total RAM is 128 KB (`0x20000000`–`0x20020000`).

### Fixed / static

| region | size | notes |
|---|---:|---|
| SoftDevice (BLE blob) | ~8 KB | `0x20000000`–`0x20002040`, outside the app linker region |
| `.data` | 0.7 KB | initialized C/C++ data, copied from flash |
| `.bss` | 8.1 KB | zero-initialized C/C++ data |
| `.stack` | 8 KB | one shared execution stack for all fibers (raised from 2 KB after stack-guard panics); measured usage in *Stack* below |
| heap | ~103 KB | everything else; grows up to the stack floor |

The embedded Lua script is **not** in RAM: `source/lua-script.lua` is
`objcopy`'d into the read-only `.lua_script` flash section and read in place via
`__lua_meta.start` (`source/main.cpp`). The cost is the parsed `Proto`, not the
text.

### Stack

This port pages every fiber through one shared execution stack at the top of RAM,
`[stack_limit(), fiber_initial_stack_base())` (`__StackLimit`..`__StackTop`,
8 KB, set by `__StackSize` in `source/nrf52833*.ld.patch`). The context switch
copies only the used range (`CortexContextSwitch.s`), so the shared region holds
the running fiber's live frames; inactive fibers sit in heap buffers sized by
`verify_stack_size`.

`source/stack-probe.c` measures the high-water mark: it paints the free part of
the region with a pattern at boot, and `stack_probe_peak()` finds the deepest
address later overwritten. `microbit.stackUsage()` returns the peak in bytes,
`microbit.stackReset()` re-paints, and `main()` prints
`STACK <tag>: current=… peak=… region=…` at boot checkpoints (needs
`DMESG_SERIAL_DEBUG`).

On-device (region = 8192 B):

| consumer | C stack |
|---|---:|
| parsing the embedded script (`luaL_loadbuffer`) | ~4.1–4.2 KB |
| REPL/event chain (`on_event` → session → `submit` → `loadstring` → `pcall`) | ~2.5 KB |
| Lua-to-Lua recursion | ~0 (handled by `luaV_execute`'s `newframe`; grows the Lua stack, not the C stack) |

The two C-stack consumers are **additive in the REPL** — a submitted chunk is
parsed inside the event chain — so a large user chunk can reach roughly
`2.5 KB + parser depth`. Size for that sum plus margin for newlib `printf`/`%g`
(dtoa) and nested IRQ/SoftDevice frames: 8 KB is a comfortable field value,
7 KB is a plausible trim, and going lower needs the worst-case chunk measured.
The guard panics (`DEVICE_STACK_OVERFLOW`) on overrun, and reducing `__StackSize`
grows the heap, since heap end = `stack_limit()`. Lua's C stack limits
(`source/lua-cstack.h`) are the region's size, read at run time, less fixed
margins: 5,888, 6,400 and 4,352 bytes at 8 KB, and 2 KB lower each at 6 KB.
The linker patches refuse a `__StackSize` below 4 KB, where the margins no
longer fit; the Compy's own script needs about 7 KB to be read at all.

### Lua heap (the tunable part)

On-device `LUA_MEM_DEBUG` deltas between the boot markers, measured on the
build with S1 (lazy tables) + S2 (debug strip) + S6 (32-bit float numbers),
before `require` and the TPBot commands in C (`source/tpbot.c`):

| item | size | marker delta |
|---|---:|---|
| Lua state (empty) | 2,143 B | `state − boot` |
| base/table/string/math libraries | 7,777 B | `stdlib − state` |
| `microbit.*` API namespace tables | 1,742 B | `api − stdlib` |
| embedded script `Proto` + API names (with debug info) | 19,656 B | `loaded − api` |
| ↳ debug info, freed by S2 | 7,740 B | `stripped − loaded` |
| ↳ stripped `Proto` + API names | 11,916 B | `stripped − api` |
| names materialised by the script + runtime state | 1,659 B | `ran − stripped` |

Steady state (`ran`) was 25,237 B.

Host estimates of the builds since, on a 32-bit host model of the board's
Lua (float numbers, 4-byte pointers; its empty state is 2,143 B, as on the
board): each build's registration reproduced, its API stubbed, its script
loaded, stripped and run as `main()` does, then collected. Static RAM
(`.bss` + `.data`) is from each build's link.

| build | Lua heap after boot | `.bss` + `.data` |
|---|---:|---:|
| daa2f77, before `require` (TPBot commands in Lua) | 29,080 B | 14,704 B |
| upstream 1733ccd (`require`, TPBot in C for tpbot2) | 26,191 B | 14,712 B |
| this firmware | 26,033 B | 14,736 B |

What moved it, measured the same way:

- the TPBot commands in C instead of Lua: 3,961 B less heap
  (`tests/host-tests.sh`'s own figure, built 32-bit);
- no `_M`, `_NAME` and `_PACKAGE` in module tables: 532 B less, on
  upstream's script;
- `microbit.accelerometer`, `.compass`, `.io` and `.i2c`, required by the
  firmware's script so the prompt has them: 1,051 B of heap;
- the dispatcher's state: 18 B of `.bss`; its line of 16 events is 144 B
  of the board's heap, made the first time an event has to wait for a
  program with somewhere to send it;
- the radio's inbox of 2 pieces: 4 B of `.bss`, and 68 B of the board's
  heap when the first link opens.
 The same markers on the double build (S1+S2,
no S6) ended at 30,837 B, so S6 saves a further 5,600 B; against the original
eager + debug build (`ran` 48,249 B) the three changes save 23,012 B.

With S1 the API tables hold only the subtables and metatables at boot (1,742 B
versus 16,575 B when eagerly registered, both measured with `double`). Names the
script actually uses are interned when its chunk is loaded and materialised as
it runs, which is why `loaded − api` is larger here than in the eager-build
measurement below.

#### `microbit.*` table breakdown (if eagerly registered, `double`)

LP32 sizes (identical to the ARM target): `TValue`=16, `Node`=32, `CClosure`=40,
`TString`=16, `Table`=36. (Under S6/`float` they are `TValue`=8, `Node`=20,
`CClosure`=28, `UpVal`=20; `TString` and `Table` are unchanged.)

| item | count | bytes |
|---|---:|---:|
| hash nodes | 215 | ~6,880 |
| C closures (one per registered function) | 138 | ~5,520 |
| interned names (115 functions + 41 constants + 11 fields) | 167 | ~4,000 |
| table headers | 11 | ~396 |
| **total (measured)** | | **16,575 B** |

Per-table node counts: `microbit` 64, `display` 32, `serial` 32, `io` 32,
`compass` 16, `ble.uart` 16, `accelerometer` 8, `radio` 8, `audio` 4, `i2c` 2,
`ble` 1. (`microbit` has 58 fields — 8 functions + 9 subtables + 41 constants —
which forces a 64-node table.) This whole cost is what S1 removes at boot: after
S1 + S6, `api − stdlib` = 1,742 B.

#### Embedded script `Proto` breakdown

With the stripped bytecode dump as a cross-check (39 protos, 1015 instructions,
215 numeric constants, 184 string constants):

| portion | size |
|---|---:|
| loaded `Proto` with debug info (`loaded − api`, current/S6) | 19,656 B |
| debug info (`lineinfo`, `locvars`, upvalue names), freed by S2 | 7,740 B |
| stripped `Proto` + API names (`stripped − api`, current/S6) | 11,916 B |
| ↳ raw `code` arrays (dump) | 4,060 B |

On the eager `double` build (before S1, API names already interned) the same
`loaded`/`stripped` deltas were 19,640 B / 11,900 B. S1 moves ~1.7 KB of API
name strings from registration into chunk load, while S6 halves the 215 numeric
constants (16→8 B each, −1,720 B); the two roughly cancel.

The text parser always generates the debug info; S2 frees it after load.
Combined with S1 and S6, the steady-state heap (`ran`) fell from 48,249 B to
25,237 B.

## Part 2 — strategies to lower RAM

### S1. Lazy API namespace dispatch — done

Implemented in `source/codal-lua.cpp`. Methods and constants are merged into one
flash-resident `LuaApi` table per namespace (generated by the `F`/`C`
X-macros), and each namespace is an empty table with an `__index` resolver
(`l_lazy_index`), made by `require` (`register_lua_modules` requires the
`microbit.*` namespaces and `tpbot` before the script runs). A module's table
holds no `_M`, `_NAME` or `_PACKAGE`. On first access the matching entry is
materialised — a C closure for a method, an integer for a constant — and
cached in the table; untouched names never allocate. On device the boot cost
of the API tables (`api − stdlib`) fell from 16,575 B to 2,186 B and steady
state (`ran`) by 11,493 B.

Trade-offs:

- names appear in `pairs()` / `rawget()` only once touched;
- a miss costs a short linear scan of the namespace's table;
- touching every name (unusual) returns to the eager footprint.

### S2. Strip debug info from the embedded chunk — done

Implemented in `source/lua-strip-debug.c` and called from `source/main.cpp`
right after `luaL_loadbuffer` succeeds. `lua_strip_debug()` walks the loaded
`Proto` recursively and frees `lineinfo`, `locvars`, and `upvalues` with
`luaM_freearray`, zeroing the counts: line numbers, local variable names and
upvalue names. The `debug` library is not opened, so what a program loses is
in its errors, which have no line and name no local or upvalue: `local t
t.x = 1` gives `program: attempt to index a nil value`, where unstripped it
gives `program:1: attempt to index local 't' (a nil value)`, and an upvalue
shows as `'?'`; globals and fields keep their names. The chunk is loaded as
`=program`, and its errors name it with no line: from the VM
(`source/ldebug-no-line.patch`), and from `error`, `assert` or a function's
bad argument, `program: boom` (`source/lauxlib-no-line.patch`). REPL chunks
compiled later keep their debug info, their lines and their names.

### S4. Resolve constants through `__index` — done

The 41 constants are entries in the same lazy `LuaApi` tables as the methods, so
they are materialised on first access too (and cached, so O(1) thereafter).

### S5. Flash-resident strings — todo, ~3–5 KB

Strings are the easy permanent class: they are **leaf objects** (no outgoing
references), so the collector only needs to *not write* their header — no child
marking, no cycles. The blocker is not the GC but **identity**: table and global
lookups compare string pointers (`luaH_getstr`, `ltable.c:455`), so a static
`"print"` must be the single canonical object for its content.

Feasible mechanism:

- declare static `TString`s in `.rodata` with the bytes inline at
  `getstr(ts) = (ts)+1` and a **precomputed `hash`** (flash is read-only; the
  pool can't be chained into `strt.hash`, which needs `next` writes);
- look the pool up **first** in `luaS_newlstr` (`lstring.c:75`) and return the
  static object, so no heap duplicate is ever interned and pointer equality
  keeps working unchanged;
- skip read-only objects in `markobject`/`markvalue` and in the
  `reallymarkobject` call inside `luaC_barrierf` (`is_readonly` is a plain
  address test — flash is `< 0x10000000`); they are never linked into `rootgc`,
  so sweep and `luaS_resize` never see them.

Exclude Lua keywords: the lexer writes `tsv.reserved` (`llex.c:70`).

The residual is only the strings actually referenced — the script's chunk
constants (~109 unique) plus standard-library names — roughly **3–5 KB**; S1
already deferred the rest. API/stdlib names are known in C; the chunk constants
need a build-time generator (host Lua to enumerate them), which is the bulk of
the work. This supersedes the earlier "external data pointer" idea: a whole
static `TString` (header + inline bytes) is simpler and saves more.

### S6. 32-bit float Lua numbers — done

`LUA_NUMBER_IS_FLOAT` (codal.json) selects `float` instead of `double`, via a
small in-place patch of Lua's `luaconf.h` (`source/luaconf-float.patch`, applied
idempotently from CMake; to become a commit in the Lua fork). It halves `TValue`
(16→8) and shrinks `Node` (32→20), `CClosure` (40→28), `UpVal` (32→20) and Lua
stack slots; the number formatting/parsing macros move to `"%.9g"`/`strtof`.
The Cortex-M4 single-precision FPU also makes arithmetic faster. On device the
steady-state heap fell by a further 5,600 B (`ran` 30,837→25,237 B).

Caveat: a 24-bit mantissa makes integers above 2^24 approximate:
`microbit.systemTime()` past ~4 h 39 m, and large literals. Event timestamps
are microseconds, the low 32 bits of CODAL's, handed to Lua as a signed
integer: exact only up to 16.8 s, off by up to 64 µs after that, negative
from 35.8 min on, and they wrap every 71.6 min.
`microbit.serialNumber()` now returns an exact decimal **string** to keep the
32-bit device ID lossless (API change). `LUAI_USER_ALIGNMENT_T` and the string
layout are untouched.

### S7. Flash-resident Protos — todo, ~4–10 KB

Compile `lua-script.lua` to bytecode (`luac -s`) and embed that in the
`.lua_script` flash section instead of the text, loaded by a loader of its
own (`f_parser` refuses binary chunks: `source/ldo-text-only.patch`), then
point `Proto` fields at it. This is also the prerequisite for a build-time
compiled payload generally.

The dump cannot be aliased: `luaU_undump` deserializes it with `luaF_newproto`,
`luaM_newvector` and `luaS_newlstr`, and the on-disk stream is not the in-memory
layout. So a build-time generator must emit the in-memory tree as C data with
link-time-resolved pointers, matching the target (`sizeof(size_t)`,
`LUA_NUMBER`, endianness).

| | static in flash | RAM | requirements |
|---|---|---:|---|
| L1 | `code` | ~4.0 KB | generator; `LoadCode` points `f->code` at flash; `luaF_freeproto` skips freeing it |
| L2 | `code` + `k` | ~7.3 KB | L1 plus S5 (string `k` entries need link-time `TString*` addresses) |
| L3 | whole `Proto` tree (headers, `code`, `k`, `p`) | ~10.4 KB | L2 plus permanent-`Proto` GC handling |

`code` is inert (no GC references), so L1 needs no collector changes. L2 makes
`k` a flash `TValue[]`: numeric entries are inert, but string entries must be
canonical static `TString`s (S5), else pointer-equality lookups break. L3 also
puts the headers in flash, so the collector must not write their `marked` /
`gclist` and `luaF_freeproto` must not free them; unlike the non-leaf closure
case, the `Proto` graph is an acyclic tree (`p` are children; `k`, `source` are
leaves), so no visited set is needed — marking can recurse directly.
`upvalues`/`lineinfo`/`locvars` are already gone via S2. L3 also removes the
boot-time parse of the embedded chunk (the parser stays for REPL `loadstring`).

Blocker/tooling: the bytecode header encodes `sizeof(size_t)`; a native 64-bit
host `luac` emits 8 and is rejected by `LoadHeader` on the 32-bit ARM target.
Build needs a 32-bit `luac` (`gcc -m32`, i.e. `gcc-multilib` in the Docker/CI
image) or a cross-build under qemu. This generator is the dominant cost; the
loader and GC changes are small by comparison. It also changes the `hextract
embed` workflow to require compatible bytecode.

### Not worth the complexity

- **General permanent GC objects (non-leaf: closures, protos, tables).** They
  have children, and since flash cannot hold `marked`, marking them needs a
  separate visited set to break cycles, plus gray-list and write-barrier
  changes — core-collector surgery where a bug is a use-after-free. Only ~1 KB
  of used API `CClosure`s is reachable this way (S1 already deferred the rest).
- **Static standard-library tables/closures (~7.8 KB).** Built at runtime by
  `luaopen_*`; making them static means rewriting those libraries and making
  mutable non-leaf tables permanent. Not worth it.
- **Frozen Lua tables.** Don't work: tables are mutable through the API and are
  non-leaf.
- **C `.data`/`.bss` audit.** Mostly genuine runtime state; broad search for a
  small, uncertain yield.

### Method and caveats

- Static sizes: `build/MICROBIT.map` and `build/nrf52833-patched.ld`.
- Namespace sizes: the merged `LuaApi` tables in `source/codal-lua.cpp`,
  generated by the `F`/`C` X-macros; the eager breakdown uses LP32 struct sizes
  (identical to the ARM target).
- Lua-heap sizes are on-device `LUA_MEM_DEBUG` readings on the micro:bit v2,
  computed as deltas of the boot markers (last bullet). The per-table node/name
  breakdown remains an analytical cross-check.
- On-device instrumentation is built into `source/main.cpp`: define
  `LUA_MEM_DEBUG` (e.g. `"LUA_MEM_DEBUG": 1` in `codal.json` config) to emit a
  `LUA_MEM <tag>: lua=<bytes>` line via `DMESG` at boot, after `luaL_newstate`,
  after the standard libraries, after the API registration, after loading the
  script, and after running it; the deltas isolate each component. Output also
  needs `"DMESG_SERIAL_DEBUG": 1`. If `CODAL_DEBUG >= 2` as well, each tag also
  calls `device_heap_print()` (CODAL allocator `mb_total_used`/`mb_total_free`).
  Config changes require `./build.py --clean`.
- Number type: S6 is selected by `LUA_NUMBER_IS_FLOAT`; `source/luaconf-float.patch`
  is applied in place from CMake with a content guard, so it is a no-op once
  `libraries/` is patched. `LUAI_USER_ALIGNMENT_T` (`double`) is deliberately
  left unchanged, so string/userdata alignment does not depend on the number
  type.
