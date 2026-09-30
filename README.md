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
drives is handled when the move is over. A radio link is answered all the
same (see Radio link).

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
`answered`, `notSent`, `typed` and `microbit.sleep` from when it opens, so the
REPL over a link answers too; a line that takes away another global it uses can
stop what the REPL shows, and the global can be put back from the prompt. The
firmware arms the port itself after an `on_event` that fails on the port's
event, and has what already waits there read.


## Radio link

`listen(name)` at the prompt waits for the board named `name` to call, then
serves it a REPL of its own over the radio. `connect(name, timeout)` on that
board calls, and from then on what is typed there goes over the link a line at a
time, and what comes back is printed. Typing shows as at the board's own
prompt: Backspace takes back a character of the line not yet sent, and Enter
ends the line on the screen. A line typed while `connect` still calls goes over
the link once it opens. The link itself is `microbit.radio`'s `connect`,
`listen`, `answered`, `tx` and `rx`, in `source/radio-link.c`; `notSent` gives
the words the REPL says when a line did not go, and `typed` what typing makes of
the line and shows.

A frame carries its kind, the link's number and a piece's number. The link's
number is one byte, drawn at random by the board that calls. HELLO and WELCOME
carry both boards' names, the link's version, 5, and the call's number, two
bytes the calling board draws for each `connect` and sends again with every
HELLO of that call. A board whose firmware has an older link is not answered,
and does not answer, so `connect` says `Connection timed out.` A board takes the
pieces and answers that carry its link's number. A HELLO that repeats the open
link's call, with its link's number, its caller and its call's number, is
answered again, and the link keeps what it took; any other call for the board
opens the link anew.

`tx(message)` sends the message in pieces of 25 bytes, each sent again every 30
ms, 8 times at most, until the other board takes it, and returns true once every
piece is taken: the message has arrived, and a line in it may not have run yet.
A board takes a piece whatever its Lua is doing: the fiber that carries the
radio's event answers the piece and keeps it in C before it hands the event on,
and while another Lua call runs, the event only waits for it. A line sent while
the robot drives is taken at once, waits in the board's inbox, and runs when the
move is over, after the lines before it; the prompt comes back once it has run.
The inbox holds 8 pieces, 218 B of the board's heap made when the first link
opens. A piece that comes again, because the answer to it was lost, is answered
again and dropped. Pieces are numbered in 16 bits: a new piece is taken for a
repeat only after 65,535 pieces in a row are lost to a board that took none,
each a line typed or an answer given.

A piece that finds the inbox full is not taken, and neither is one sent to a
board that is gone. The board that called then says `The other micro:bit did not
answer, so it may not have got:` and the line, and drops what was typed after
it, what reached the port while it tried included: that is shown, and the line
it begins is dropped up to its line ending. It says so when that held more than
a line ending, and tells the person to check whether the line ran before typing
it again, since the other board may have taken the line and only its answers
been lost. When the other board showed `>>` after the last line it took, it
says the other board may be waiting for the rest of a statement, and tells the
person to press the reset button on the board that called and connect again
before that check: the new call starts the other board's session over, with
its unfinished statement gone. The board that called prints what the other
board says as it comes, and between the lines it sends, so its own inbox does
not fill while a paste goes out.

The first piece of each message is marked, and `rx()` returns with a piece
whether it starts a message. The link's REPL is sent whole lines, one to a
message, so a piece that starts a message drops what follows the last line
ending: a line whose end never came. A line said to be lost never runs in part,
the next one runs as it was sent, and the lines of a statement still open at
`>>` stay.

While a board serves a link, the radio's events do not reach `on_event`:
`listen()` takes what comes without them, and the link's traffic does not push a
button press out of the line of events waiting while the robot drives.

Four cases the link does not cover. A line said to be lost has arrived when
every answer to its last piece was lost on the way: it runs, and typed again, it
runs twice. A command that runs Lua without ever waiting lets no fiber run, so
the pieces sent meanwhile wait in the radio's own queue of 4; a copy of one
taken after the command may finish a line the sending board has already said was
lost. An answer the board that called does not take is cut short, with nothing
to say so, and what the other board says next follows it on the same line: when
the board that called is out of reach, or busy while more than 8 pieces of the
answer come, in a handler of its own or sending a line that is not taken. And
once a board has opened a link, its fiber takes every datagram that comes, so a
program there that reads the radio itself with `recv` gets none.


## TPBot

`tpbot` and the robot globals `robot_info`, `robot_move`, `turn` and
`straight` are in C, in `source/tpbot.c`, for the TPBot Edu and the TPBot
Classic at once.


## Host tests

`bash tests/host-tests.sh` builds this firmware's Lua for the host and runs:

- every TPBot command beside the Lua it replaced (`tests/tpbot-reference.lua`),
  comparing the bytes each writes to the bus, what it returns and the words of
  each error;
- `source/lua-script.lua` over a stand-in board, with events through
  `source/lua-events.c`: typing at the REPL, buttons pressed while Lua sleeps,
  the globals the REPL uses taken away, and a radio link through
  `source/radio-link.c`, both ends of it, with lines sent while the robot
  drives;
- `source/radio-link.c` built without Lua's headers, calling nothing of
  Lua's;
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
