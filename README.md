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
and the REPL's greeting prints it, after the empty line the board starts
every reset with:

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


## Stack and heap

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

CODAL stops the board with panic 020 whenever its heap cannot give what is
asked of it (`DEVICE_PANIC_HEAP_FULL` in the target): Lua's own allocations
included, a fiber for an event, the event line. So on the board a program
that fills the heap ends in 020, not in Lua's "not enough memory", and the
prompt comes back only after a reset. What the firmware does when an
allocation fails, the error at the prompt, an event counted as dropped, the
port's event kept, is what the host tests check, with an allocator that can
refuse.

Lua loads only text: `loadstring` and `load` refuse precompiled code, as
`string.dump` makes it, whose nesting nothing checks against the C stack.


## Modules

Before a script runs there are only `package`, `module` and `require`. A
program requires what it uses: `require("microbit")` makes the global
`microbit` and returns it; `require("microbit.display")` makes
`microbit.display` and returns it (making a plain `microbit` table on the
way if there is none yet; `require("microbit")` then gives that same table
its functions); `require("tpbot")` makes the global `tpbot` and sets the
robot commands `robot_info`, `robot_move`, `turn` and `straight`. The other
modules are `microbit.accelerometer`, `microbit.compass`, `microbit.audio`,
`microbit.io`, `microbit.serial`, `microbit.i2c`, `microbit.radio`,
`planetx`, `tpbot2` and `nezha2`. Requiring a module sends nothing to a
robot. The firmware's own script requires `microbit` and all its
namespaces above, and `tpbot`, so the prompt has them all.

A build with `DEVICE_BLE` on has `microbit.ble.uart` as well. Its `read(n)`,
like `microbit.serial.read(n)`, returns every byte read, a zero byte
included; CODAL's own `read(n)` for the BLE UART stops at the first zero.
Both take from 0 to 4096 bytes at a time.


## Events

The firmware calls the global `on_event(source, value, timestamp)` for each
event: a button, the serial port, the radio. It reads `on_event` straight
from the globals, whatever metatable a program gives `_G`. A line typed at
the REPL runs in an environment of its own, so it sets the global with
`_G.on_event = f`. When `on_event` cannot be called, the event goes to the
handler the script gave `microbit.eventFallback()`, its own, so the REPL
keeps answering; with no such handler, as in a program that is the board's
whole script and sets no `on_event`, the event goes nowhere. Events that are
only noise to a program never reach it: the serial port saying it has data,
or that it is full, and a scroll that has ended.

One Lua call runs at a time. An event that comes while one runs waits in a
line, in the order the events came, once the program has an `on_event`: the
firmware looks for one at every wait that lets other fibers run
(`microbit.sleep`, `robot_move`, a scroll, a serial or radio wait), so a
program that sets `on_event` and then drives keeps the presses made during
the move. The line holds 16 events unless `microbit.eventLine(n)` says
otherwise (0 drops every event that would have to wait); it is made the
first time an event has to wait for a program that has an `on_event` or a
fallback, and a program with neither pays nothing for it. The running call
handles the events that were waiting whenever it enters `microbit.sleep`,
and again every 10 ms while it sleeps: a program that sets `on_event` and
then loops with `microbit.sleep` gets each event while it sleeps. A sleep
ends on time however many events come, but lasts as long as the handlers it
runs. The running call is the script at boot, or a command at the REPL; a
handler's own `microbit.sleep` handles nothing, so one handler always ends
before the next starts, and so does a program's own `on_event` when it gets
the serial port's event. The firmware's REPL marks the command it runs for
that event with `microbit.eventRepl()`, so a command typed through an
`on_event` that passes the port's event on to the REPL's still handles
events at its sleeps. A sleep that is already more than 4,352 bytes deep in
the C stack handles none, so the handlers it would run keep 1.5 KB before
the limit; their events wait for the call to return. A call from C into Lua,
a coroutine's resume, or a level of the parser, that finds more than 5,888
bytes of the 8 KB stack in use stops with "C stack overflow", and a pattern
that would match deeper stops with "pattern too complex"
(`microbit.stackCurrent()` says how much is in use now,
`microbit.stackUsage()` the most since boot). These limits are the stack's
size less fixed margins (`source/lua-cstack.h`), so they follow
`__StackSize`, down to 4 KB: the linker refuses a smaller one. What still
waits when the call is over is handled then. When the line is full the
oldest event in it is dropped, as is an event for which an allocation fails,
for the line or for a fiber to carry it (see Stack and heap for what the
board does then); `microbit.eventsDropped()` says how many have been since
boot.

The serial port's event the REPL waits for is kept apart and never lost,
even when no fiber can be made to carry it: the running call takes it, or
with Lua free, the scheduler's next tick hands it on again. It goes first
when the running call is over, and never at a sleep, so the lines sent to
the REPL run one after another. The port holds 254 characters that wait to
be read, what is typed while the script at boot runs included, and the REPL
reads them when it starts.

`robot_move`'s own wait handles nothing: a button pressed while the robot
drives is handled when the move is over, and so is a key that came over a
radio link, if it is still there (see Radio link).

A mistake in a handler goes to the serial port as `Runtime error: ` and the
message, unless a program has redirected the port to other pins
(`microbit.serial.redirect(microbit.io.getPin(30), microbit.io.getPin(31))`
puts it back on USB), and scrolls by on the display, if the display is free,
while the program goes on. A script that stops on a mistake at boot sends it
to the port the same way, then shows `Lua error!` and the message on the
display, once, before anything else happens; the board then handles events
with the `on_event` the script set before it stopped, if any. A script that
does not compile sends `Compile error: ` and the message to the port, and
scrolls them by.

Lua's own `print` writes to stdout, which goes nowhere on this board; the
firmware's script puts its own in its place, writing to the serial port.

Whatever goes wrong on the way from a line to its result, short of CODAL's heap
running out (see Stack and heap), the prompt comes back and the port is armed.
The REPL keeps `loadstring`, `setfenv`, `pcall`, `string.gsub`, `string.sub`,
`string.find`, `string.match`, `string.format`, `table.concat`,
`microbit.serial.eventAfterAsync`, `microbit.serial.getCharAsync` and
`microbit.eventRepl` of its own for that, and a radio link's `tx`, `rx`,
`answered` and `microbit.sleep` from when it opens, so the REPL over a link
answers too; a line that takes away another global it uses can
stop what the REPL shows, and the global can be put back from the prompt. The
firmware arms the port itself after an `on_event` that fails on the port's
event, and has what already waits there read.


## Radio link

`listen(name)` at the prompt waits for the board named `name` to call, or for
any board when no name is given, then serves the one that called a REPL of its
own over the radio. `connect(name, timeout)` on that board calls, and from then
on every key typed there goes over the link as it comes, and what comes back
goes to the port as it is: the serving board echoes, erases and enters, with
the same code as its own console, so the calling board's port shows what the
serving board's own would. Backspace, Enter and Ctrl+C work over the link as at
the console. Ctrl+D on the calling board hands its port back to the console,
which says `<name> disconnected.`; the serving board keeps listening, so the
link can be made again. Ctrl+C or Ctrl+D on the serving board's own port ends
`listen()`, which closes the link. `listen(name)` typed over the link lets the
caller go and waits for the board now named. The link itself is
`microbit.radio`'s `connect`, `answered`, `tx`, `rx`, `close`, `beacon` and
`scan`, in `source/codal-lua.cpp`, as nagydani/microbit-lua has them.

A frame carries its kind, the link's number and a piece's number, in three
bytes, then up to 26 bytes of body. The link's number is one byte, drawn at
random by the board that calls; a board takes the pieces and answers that carry
its link's number, and a board that calls again, reset or not, is taken as it
comes. `tx(message)` sends the message in pieces, each sent again every 30 ms,
8 times at most, until the other board takes it, and returns false when none of
the 8 was answered in time, and stops there. A board takes a piece only from
Lua: the serving board in its `listen()` loop, every 5 ms, and the calling board
when the radio's event brings it, after the call that is running; while `tx`
waits for an answer a board takes nothing. So what the calling board sends in
one read goes whole only up to one frame, 26 characters: with more, the serving
board's echo of the first frame meets the second frame half way, each board's
`tx` waits out its 8 tries for an answer the other cannot give, a quarter of a
second, and then each takes the frame it held meanwhile; a read of two frames,
up to 52 characters, arrives whole after that pause, and of a longer one the
third frame on is never sent, since `tx` stopped at the second. A terminal that
sends a line in one write, as the Compy's does, meets this on a line over 26
characters. The same two-sided wait meets a key typed on the calling board while
the serving board prints over the link. nagydani/microbit-lua has none of this:
there the radio's event takes the echo while `tx` waits, in a fiber of its own;
here events wait behind the running call. A `tx` that answered the frames it
meets while it waits would end it, and is proposed upstream
(compy-microbit-radio-tx-answers-data). A key typed while the serving board runs a command, driving
the robot for instance, is not answered in time, and the calling board gives it
up after a quarter of a second without a word; the serving board may still find
it in its radio's queue of 4 datagrams when the command ends, and type it then,
or may not, when the queue was full. So what is typed meanwhile may run late,
or not at all, and nothing says which: wait for the echo before typing on. A
piece sent to a board that is gone is lost the same way.

A listening board sends a beacon every one to two seconds with its name and the
name of the board it waits for. `microbit.radio.scan()` listens for three of the
longest gaps and answers a table of the boards heard, each `true` when it waits
for this board. A calling board that hears no beacon for itself for that long
takes the link for lost, and `rx()` answers `false`, as it does once the other
board has closed the link with a BYE frame; the console then says
`<name> disconnected.`

The link has no cryptographic protection; a board that hears a link's number
can send on it.


## TPBot

`tpbot` and the robot globals `robot_info`, `robot_move`, `turn` and
`straight` are in C, in `source/tpbot.c`, for the TPBot Edu and the TPBot
Classic at once.

`straight(l)` drives `l` squares of 145 mm. `tpbot.get_distance()` and
`tpbot2.get_distance()` answer in mm, the unit `run_distance` takes, or `nil`
when nothing is within about 4 m to echo back.

A distance under 10 mm and a turn under 5 degrees, either way, do nothing, as 0
does: a TPBot Edu sent 1 or 2 mm, or 1 degree, drives or spins on and never
stops. This holds for `tpbot.run_distance` and `tpbot.turn`, for `tpbot2`'s,
and so for `straight(l)` under about 0.069 (10/145) and `turn(h)` within about
1/6 of an hour of 12 o'clock, either side; float rounding moves both edges a
little. A distance or an angle of 65536 or more, not a number and infinity
included, is refused.


## Host tests

`bash tests/host-tests.sh` builds this firmware's Lua for the host and runs:

- every TPBot command beside the Lua it replaced (`tests/tpbot-reference.lua`),
  comparing the bytes each writes to the bus, what it returns and the words of
  each error;
- `source/lua-script.lua` over a stand-in board, with events through
  `source/lua-events.c`: typing at the REPL, buttons pressed while Lua sleeps,
  the globals the REPL uses taken away, and a radio link whose other board the
  test plays, serving it and calling it;
- `source/lua-cstack.c`'s limits at four stack sizes, which they follow, and
  at sizes too small for them, where none wraps round;
- `tests/wait-audit.sh`: every binding that waits looks for `on_event` first;
- `tests/lua-patch-test.cmake`: the build's Lua patches go into a fresh Lua
  as plain `patch` puts them, change nothing a second time and finish a file
  patched in part; a file changed otherwise, or holding a patch no longer
  listed or edited since, stops the build naming the way back, and a patch
  that no longer fits Lua, a missing one, or no `patch` tool is named; a file
  of the unpacked Lua that no patch touches, changed or gone, stops the build
  too, and so does a Lua tarball in `libraries/` that is not lua.org's, by its
  SHA256, at every configure.

It needs `cc`, `patch`, `cmake`, and the Lua tarball a firmware build leaves
in `libraries/`, which it patches as the build does.


## Numbers as text

The firmware leaves printf's float support out to save flash, so
`source/lua-number.c` turns numbers into text: `tostring`, `..`,
`table.concat`, and `string.format`'s `%e`, `%f` and `%g`. Two patches,
`source/luaconf-number-text.patch` and `source/lstrlib-number-text.patch`,
route Lua's own calls there. As `tostring` and `..` write it, a whole number
that fits in 32 bits shows in full, and any other number with 7 significant
digits. `string.format` follows its format: `%f` gives 6 decimals unless the
format says otherwise, and a precision past 14 significant digits is only
approximate.

`tests/lua-number-tests.sh` builds it with the host's C compiler and checks it
against the host's printf.


# Where

The project's home is at
[github.com/nagydani/microbit-lua](https://github.com/nagydani/microbit-lua).
