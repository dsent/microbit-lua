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

### Firmware version

The firmware knows which build it is. `microbit.version()` returns it,
and the REPL's greeting prints it:

```
micro:bit
Lua 5.1 REPL
firmware 86c8e16
```

`build.py` derives it from git at build time: the commit `git describe
--always --dirty=-drift` names, `-drift` marking a tracked file that
differs from that commit, or `unknown` outside this repository's git.
`./build.py --firmware-version` prints it without building. A
`FIRMWARE_VERSION` in the environment, when not empty, is taken as
given; it may hold letters, digits and `. _ + / -`. A container needs
one when git cannot see the checkout from inside it, as for a
submodule mounted on its own:

```
podman run --tty --rm --volume "$(pwd)":/workspace --workdir /workspace \
  -e FIRMWARE_VERSION="$(./build.py --firmware-version)" microbit -c ./build.py
```

The version also sits in the image after the text
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


## Events

The firmware calls the global `on_event(source, value, timestamp)` for each
event: a button, the serial port, the radio. One Lua call runs at a time: an
event that comes while the script, a handler or a command is still running
waits in a line of 16, and is handled once the call is over, in the order the
events came. The serial port's event the REPL waits for is kept apart and
never lost. When the line is full an event is dropped;
`microbit.eventsDropped()` says how many have been since boot.


## TPBot

`tpbot` and the robot globals `robot_info`, `robot_move`, `turn` and
`straight` are in C, in `source/tpbot.c`, for the TPBot Edu and the TPBot
Classic at once. `bash tests/tpbot-host-tests.sh` builds this firmware's Lua for
the host and runs every command beside the Lua it replaced
(`tests/tpbot-reference.lua`), comparing the bytes each writes to the bus, what
it returns and the words of each error. It needs `cc` and the patched Lua a
firmware build leaves in `libraries/`.


## Numbers as text

The firmware leaves printf's float support out to save flash, so
`source/lua-number.c` turns numbers into text: `tostring`, `..`,
`table.concat`, and `string.format`'s `%e`, `%f` and `%g`. Two patches,
`source/luaconf-number-text.patch` and `source/lstrlib-number-text.patch`,
route Lua's own calls there. A whole number that fits in 32 bits shows in
full; any other number shows 7 significant digits.

`tests/lua-number-tests.sh` builds it with the host's C compiler and checks it
against the host's printf.


# Where

The project's home is at
[github.com/nagydani/microbit-lua](https://github.com/nagydani/microbit-lua).
