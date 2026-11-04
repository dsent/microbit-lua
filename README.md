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
and pinned in `codal.json`.

You can update them with `./build.py --update`; it should fail loudly
if there are any pending changes in your checkouts and/or git pulling
is not a fast-forward.

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

Every fiber runs in that one 8 KB region, and nothing guards its bottom: a
call that goes past it writes over the top of the heap, and the board stops
with a 045 panic. Lua's own limit of 200 nested C calls does not keep it off:
a recursion through a metamethod takes 140 to 220 bytes a level here, through
`pcall` 430. So Lua checks the C stack before each call it makes from C and
at each level of the parser, and with more than 5,888 bytes in use it stops
with "C stack overflow", an error a `pcall` catches. The limit is the region's
size less a margin (`source/lua-cstack.c`).

A Lua function calling a Lua function uses no C stack: it takes a call frame
on the heap, in an array that doubles as the calls deepen. Measured from a
fresh boot, 500 nested calls run and the 501st panics the board with 020,
out of memory, as the array doubles to 1024 frames; with 8 KB of data on the
heap the edge is 428. So `LUAI_MAXCALLS` is 200 (`source/luaconf-maxcalls.patch`),
and a recursion without a base case ends with "stack overflow" and the prompt
back: at about 115 levels the first time after a boot, when the list of 128
frames would double, and at about 190 after that, since Lua then keeps the
list at 200 frames.


## Numbers as text

The firmware leaves printf's float support out to save flash, so the C
library writes nothing for a float. `tostring`, `..` and `print` write
numbers with `lua_float2str` (`source/lua-number2str.c`), in 7 significant
digits. `string.format`'s `%e`, `%f` and `%g` go to `source/lua-number.c`
through `source/lstrlib-number-text.patch`, and follow the format as C does:
`string.format("%.2f", 2.5)` gives `2.50`. Past 14 significant digits they
write zeros, and the `#` flag is ignored.

`tests/lua-number-tests.sh` builds `source/lua-number.c` with the host's C
compiler and checks it against the host's printf.


## TPBot

`tpbot` and `tpbot2` are in C, in `source/tpbot.c`.

- `tpbot` holds what the TPBot Classic and the TPBot Edu both do:
  `set_car_light`, `set_motors_speed` and `get_distance`. Each command sends
  both robots' frames, and each robot ignores the other's, so one program
  drives either robot. `require("tpbot")` also sets the globals
  `robot_move(left, right, seconds)` and `robot_info()`.
- `tpbot2` holds everything the Edu does and sends the Edu's frames only:
  the three above, `run_distance(mm)` and `turn(degrees)`.
  `require("tpbot2")` sets `robot_move` and `robot_info` for the Edu, plus
  `straight(squares)`, in squares of 145 mm, and `turn(hours)`, in hours of
  a clock face. With `tpbot` alone they are not there, and Lua says
  `attempt to call global 'straight' (a nil value)`.

`robot_move` stops the robot after the seconds given, from 0 to 3600, and
refuses a call without them. `get_distance()` answers in mm in both
modules, the unit `run_distance` takes, or `nil` when nothing is within
about 4 m to echo back.

A distance under 10 mm and a turn under 5 degrees, either way, do nothing,
as 0 does: a TPBot Edu sent 1 or 2 mm, or 1 degree, drives or spins on and
never stops. So `straight(0.06)` and `turn(0.1)` do nothing. A distance or
an angle of 65536 or more is refused. A robot command waits about 5 s for a
switched-off TPBot Edu that holds the bus (`NRF52I2C_TIMEOUT10US` in
`codal.json`), not 14, before it says the robot does not answer.

## Host tests

`bash tests/host-tests.sh` builds this firmware's Lua for the host and runs:

- every TPBot command over a stand-in bus, checking the bytes each writes,
  what it returns and the words of each error;
- `source/lua-cstack.c`'s limit at two stack sizes.

It needs `cc`, `patch`, and the Lua tarball a firmware build leaves in
`libraries/`, which it patches as the build does.

# Where

The project's home is at
[github.com/nagydani/microbit-lua](https://github.com/nagydani/microbit-lua).
