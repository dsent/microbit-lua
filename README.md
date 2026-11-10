[![CI](https://github.com/nagydani/microbit-lua/actions/workflows/ci.yaml/badge.svg)](https://github.com/nagydani/microbit-lua/actions/workflows/ci.yaml)

# Microbit Lua

Lua compiled for the [micro:bit](https://microbit.org/) SBC.


# How

The
[microbit-v2-samples](https://github.com/lancaster-university/microbit-v2-samples)
repo has been forked, stripped, and modified to download Lua v5.1.5
and compile it as the main payload of the firmware. It builds upon
[CODAL](https://tech.microbit.org/software/runtime/) and exposes its
API to the Lua runtime.

When the board is powered on then the Lua VM is initialized and the
firmware payload (the `source/lua-script.lua` file) is evaluated. This
payload can be replaced by the `hextract` script without recompiling
the firmware image.

The hextract tool requires a known layout of the firmware. For that we
need to edit the linker script, but that lives in the
`codal-microbit-v2` repo. To avoid having to patch CODAL itself, we
introduce a phase in our build script that applies the
`source/nrf52833-softdevice.ld.patch` patch to CODAL's linker script.

## Building

The simplest and most reliable way to build it is by using the
included `Dockerfile`:

1) Build the container image:
   `podman build --platform linux/amd64 -t microbit -f Dockerfile .`

2) Build the firmware (from the project root):
   `podman run --tty --rm --volume "$(pwd)":/workspace --workdir /workspace microbit -c ./build.py`

3) Alternatively, you can start a shell and work inside the container:
   `podman run --tty --rm --interactive --volume "$(pwd)":/workspace --workdir /workspace microbit`

The current directory will be shared with the container under
`/workspace`.

### Dependencies

The build automatically downloads the dependencies into the
`libraries/` directory when first invoked. The dependencies are listed
in `codal.json`, some at a fixed commit and some following a branch.

You can update them with `./build.py --update`; it should fail loudly
if there are any pending changes in your checkouts and/or git pulling
is not a fast-forward.

### Firmware version

`microbit.version()` returns the version the firmware was built as, and
the REPL's greeting prints it on its third line, `firmware 86c8e16`.
`build.py` takes it from `git describe --always --dirty=-drift`: the
commit, or the latest annotated tag and the commits since it, such as
`v1.0-3-g86c8e16`; a lightweight tag does not count. `-drift` marks a
tracked file that differs from the commit, or an untracked file in
`source/`, which the build compiles too. Outside git the version is
`unknown`. `./build.py --firmware-version` prints it without building.

The version names this repository's tree only. The CODAL libraries in
`libraries/` are whatever was fetched for the branches `codal.json`
names, so two builds of one version can hold different CODAL.

A `FIRMWARE_VERSION` in the environment, when not empty, is taken as
given, for a container where git cannot see the checkout, or refuses it
as another user's:

```
podman run --tty --rm --volume "$(pwd)":/workspace --workdir /workspace \
  -e FIRMWARE_VERSION="$(./build.py --firmware-version)" microbit -c ./build.py
```

Only letters, digits and `. _ + / : -` can go into the version; any
other character stops the build. The image holds it after the text
`microbit-lua firmware `, ended by a zero byte, so a tool can read it
out of a `.hex` file.

## Flashing

The Microbit board exposes a pendrive-like interface. Mount it like
any other pendrive and just copy the `MICROBIT.hex` file to it. While
flashing, the orange led next to the USB connector will be blinking
fast. A few seconds later the firmware is automatically started.

To avoid manually mounting the drive, you can use this on an typical
Linux:

```shell
udisksctl mount -b $(lsblk -o NAME,LABEL | awk '$2=="MICROBIT"{print "/dev/"$1}') && \
  cp MICROBIT.hex /run/media/${USER}/MICROBIT/
```


## Updating the Lua payload

The `utils/hextract` command line tool can be used to extract and
embed the Lua payload in a .hex file. It's a Lua script written by
LLMs, compatible with Lua 5.1.

Some technical details are documented in
[utils/hextract.md](utils/hextract.md).


## Serial connection

When the microbit is attached with a USB cable, then this should open
a serial console:

```
screen /dev/ttyACM0 115200
```

If `/dev/ttyACM0` is not present, then look at `dmesg --follow` while
you plug in your board.


## Debugging

The `codal.json` file has two variables:

`DMESG_SERIAL_DEBUG`: writes the `DMESG` output to the serial
port. You can also add `DMESG("hello");` lines to your own C++ code.

`CODAL_DEBUG`:
 - 1: general debug info
 - 2: heap allocation info


## Stack usage

The firmware always tracks the high-water mark of the shared fiber stack. From
the REPL:

```lua
microbit.stackUsage()   -- peak stack bytes used since boot (or the last reset)
microbit.stackReset()   -- restart the measurement
```

With `DMESG_SERIAL_DEBUG` enabled, `main()` also prints
`STACK <tag>: current=… peak=… region=…` at boot. On the current script the
deepest user is the Lua parser (~4 KB) parsing the embedded chunk; see
`docs/ram-usage.md` for the breakdown and sizing guidance.

### Deep programs

A program that recurses too deep stops with a Lua error, which `pcall` can
catch, instead of crashing the board:

- **"C stack overflow"** when less than 2,304 bytes of the 8 KB stack are left
  (more than 5,888 in use). Lua checks before each call it makes from C, each
  coroutine it resumes and each level of the parser (`source/lua-cstack.c`).
  Nothing else stops a call that runs past the bottom: it overwrites the heap,
  and the board panics with 045. Lua's own limit of 200 nested C calls is too
  high for this stack.
- **"stack overflow"** after about 120 nested Lua calls (`LUAI_MAXCALLS` is
  128, `source/luaconf-maxcalls.patch`). These calls use the heap, not the C
  stack, and with stock Lua's limit of 20,000 the board ran out of heap (panic
  020) at 501. A tail call, `return f()`, is not counted.

Not covered: pattern matching with hundreds of `?`, `*`, `+` or `-` items, an
assignment to 150 names, `string.dump` of deeply nested functions, loading
precompiled bytecode, and, from a fresh boot, a runaway recursion through a
function with 32 or more locals, which runs out of heap before the call limit.


## Numbers as text

The firmware leaves printf's float support out to save flash, so the C
library writes nothing for a float. `tostring`, `..` and `print` write
numbers with `lua_float2str` (`source/lua-number2str.c`), in up to 7
significant digits. `string.format`'s `%f` goes to `source/lua-format.c`
through `source/lstrlib-number-text.patch` and rounds as C does, for a
number whose magnitude is below 4e9, to up to 9 decimals:
`string.format("%.2f", 2.5)` gives `2.50`. `%e`, `%E`, `%g`, `%G` and any
other `%f` write the magnitude as `print` does, in lowercase. The sign, the
width and the `-`, `+` and space flags follow C; the `0` and `#` flags are
ignored.

`tests/lua-format-tests.sh` checks it with the host's C compiler.


## TPBot

`require("tpbot")` drives a TPBot Classic and `require("tpbot2")` a TPBot
Edu (`source/tpbot.c`). Each command sends its robot's I2C frame and
returns.

- `tpbot`: `set_car_light(r, g, b)`, `set_motors_speed(left, right)`,
  `info()` and `get_distance()`.
- `tpbot2`: `set_car_light(r, g, b)`, `set_motors_speed(left, right)`,
  `speed_control(left, right)`, `set_speed(mm_per_s)`, `run_distance(mm)`,
  `turn(degrees)`, `pivot(degrees)`, `done()`, `set_servo(servo, value)`,
  `adjust_motor_speed()`, `reset_motor_adjust()`, `info()` and
  `get_distance()`.

- A fraction is dropped. `set_motors_speed` takes speeds from -255 to 255,
  and runs the motors until the next motor command; light values go from 0
  to 255.
- `run_distance` and `turn` return once the frame is sent, and the Edu
  drives or turns on; a distance or a turn sent before it is done cuts it
  short. A negative distance drives backward, and a positive angle turns
  left. A distance or an angle of 65536 or more is refused, and one under 1
  does nothing. An Edu sent 1 or 2 mm drives on, and sent 1 degree spins on,
  without stopping by itself, until `set_motors_speed(0, 0)` stops it; a
  turn under 5 degrees happens only some of the time.
- `pivot` turns about one wheel, the left one for a positive angle and the
  right one for a negative angle: the other wheel is given twice the angle a
  `turn` gives each, as pxt-TPBot's left and right steering do. An angle of
  32768 or more is refused, and one under 1 does nothing.
- `speed_control` runs the wheels at speeds in mm/s by the Edu's own speed
  control, a negative speed backward; pxt-TPBot sends 200 to 500 mm/s, or 0.
  `set_speed` sets the speed of the Edu's distances and turns in mm/s, and
  refuses a negative one. A speed of 65536 or more is refused.
- `set_servo` sets the servo at S1 to S4, numbered 1 to 4, to a value of 0
  to 255. pxt-TPBot sends 0 to 180: a 180 degree servo's angle, half a 360
  degree servo's angle, or a continuous servo's speed, with 90 for still.
- `adjust_motor_speed` starts the Edu's calibration of its motors,
  pxt-TPBot's "Adjust motor speed". In three runs the Edu drove forward and
  back a few times, or once only forward, and then its lights turned green;
  meanwhile it answered nothing on the bus. Left alone, it answered again
  20 s after the command; read from during the calibration, as `done()`
  reads, it answered nothing until it was switched off and on.
  `reset_motor_adjust` sends pxt-TPBot's "Reset motor adjust".
- `done()` is true once the Edu's last distance, turn or pivot is over, or a
  motor command has stopped it. It reads from the robot, and a TPBot Classic
  that is read from holds the bus until it is switched off and on, so a
  program that may meet one asks `info()` first.
- `info()` returns a table: `connected` is true when a robot answers, and
  then `robot` is `"TPBot Classic"` or `"TPBot Edu"`.
- `get_distance()` answers in mm, or `nil` when nothing within about 4 m
  echoes back.
- The line sensors are pins P13, the left one, and P14, read through
  `microbit.io`. pxt-TPBot sets them to no pull, as `setPullNone(getPin(13))`
  does, and reads 0 over black and 1 over white, as
  `getDigitalValue(getPin(13))` does.
- A switched-off Edu holds the bus. A command then waits about 5 s
  (`NRF52I2C_TIMEOUT10US` in `codal.json`; CODAL's default is 14 s) and fails
  with "The robot does not answer"; `info` waits as long and answers
  `connected` false. `get_distance` does not use the bus.

`tests/tpbot-host-tests.sh` checks every command over a stand-in bus: the
bytes each writes, what it returns and the words of each error.


# Where

The project's home is at
[github.com/nagydani/microbit-lua](https://github.com/nagydani/microbit-lua).
