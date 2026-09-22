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
firmware payload is evaluated. The payload is `source/lua-script.lua`
with one robot library put in place of its `--@ROBOT_LIBRARY@` line:
`source/tpbot.lua` for the TPBot Classic, `source/tpbot2.lua` for the
TPBot Edu. The two boards speak different I2C formats, and the Edu
board alone drives a distance and turns an angle, so
`tpbot.run_distance`, `tpbot.turn` and the `straight` and `turn`
helpers built on them are in the Edu library only. Everything else —
the REPL, the serial session, the radio link and the buttons — is
shared. This payload can be replaced by the `hextract` script without
recompiling the firmware image.

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

### Choosing the robot

`./build.py --robot edu` builds for the TPBot Edu and `./build.py
--robot classic` for the TPBot Classic; `edu` is what you get when you
say nothing. Run it twice to have both firmwares:

```shell
./build.py --robot edu
./build.py --robot classic
```

Each build writes `MICROBIT.hex`, and keeps a copy of it as
`MICROBIT-edu.hex` or `MICROBIT-classic.hex` along with the matching
`.buildinfo`. `MICROBIT.hex` is whichever robot you built last, so
flash the board from the named copy when both are around.

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


# Where

The project's home is at
[github.com/nagydani/microbit-lua](https://github.com/nagydani/microbit-lua).
