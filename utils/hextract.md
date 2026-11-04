# hextract

Intel HEX file inspector and payload manipulation tool for micro:bit v2 firmware.
Lua 5.1.5 compatible.

## Intel HEX Format

The Intel HEX format is a text-based representation of binary data for
programming flash memory. Each line is a record with the structure:

```
:LLAAAATT[DD...]CC
```

| Field | Length | Description |
|-------|--------|-------------|
| `:`   | 1 char | Start code |
| `LL`  | 2 hex  | Byte count (number of data bytes) |
| `AAAA`| 4 hex  | 16-bit address within the current segment |
| `TT`  | 2 hex  | Record type |
| `DD`  | 2*LL   | Data bytes |
| `CC`  | 2 hex  | Checksum (two's complement of sum of all preceding bytes) |

### Record Types

| Type | Name | Description |
|------|------|-------------|
| `00` | Data | Data bytes at the current address |
| `01` | EOF | End of file (must be the last record) |
| `02` | Extended Segment Address | Sets the 16-bit segment base; physical address = `(segment << 4) + AAAA` |
| `03` | Start Segment Address | Entry point (CS:IP) for 80x86 processors |
| `04` | Extended Linear Address | Sets the upper 16 bits; physical address = `(upper << 16) + AAAA` |
| `05` | Start Linear Address | Entry point (32-bit EIP) |

### Addressing

The 16-bit `AAAA` field is relative to a base set by type `02` or `04` records.
Type `02` uses segment addressing (base = segment << 4), giving a 20-bit
address space. Type `04` uses linear addressing (base = upper << 16), giving a
32-bit address space. Once a type `04` record appears, it takes precedence over
type `02`.

### Checksum

The checksum byte is the two's complement of the sum of all bytes in the record
(excluding the leading `:` and the checksum itself):

```
CC = (-(LL + AH + AL + TT + DD[0] + ... + DD[n])) & 0xFF
```

## Firmware Memory Layout (nRF52833)

As defined by the two linker scripts:
 - `libraries/codal-microbit-v2/ld/nrf52833.ld`
 - `libraries/codal-microbit-v2/ld/nrf52833-softdevice.ld` (used when
   BLE support is included/enabled)

We patch these ld scripts in our build to add a `.lua_meta` block and a
`.lua_script` section after the `.data` LMA in FLASH.

### Lua Metadata Block

Located immediately after the `.data` LMA in FLASH, 20 bytes (5 little-endian 32-bit words):

| Offset | Field | Description |
|--------|-------|-------------|
| 0x00 | Magic | `0x4C554131` ("LUA1") |
| 0x04 | Start | Physical address of the Lua script in FLASH |
| 0x08 | End | Physical address of the end of the Lua script |
| 0x0C | Size | Size of the Lua script in bytes |
| 0x10 | Space | Total FLASH space available for the script (from Start to end of FLASH region) |

**Invariant**: `Size == End - Start`. The `embed` command maintains
this invariant and `extract` reports a warning when it's broken.

The `.lua_meta` block and `.lua_script` section are placed as the last
pieces in FLASH, so that all remaining space is available for embedding
larger Lua scripts later. The `hextract` tool auto-detects the metadata
location by searching for the `LUA1` magic value at 4-byte aligned
offsets, so it works with any linker layout without hardcoded addresses.

## Architecture

### Parsing

`parse_hex_file()` reads the entire HEX file and groups contiguous data records
into **blocks**. Each block tracks:
- `start_addr`: physical address of the first byte
- `bytes`: array of byte values
- `length`: number of bytes in the block
- `use_linear`: whether the block uses type 04 (linear) or type 02 (segment) addressing

Contiguous records with the same addressing mode are merged into a single block.
A gap in addresses or a change in addressing mode starts a new block.

### Block Operations

- `read_bytes(blocks, addr, len)`: reads bytes from the block containing `addr`.
  Returns zero-filled array if address is not found.
- `write_bytes(blocks, addr, data)`: writes data into the block containing `addr`.
  Fails if the write would exceed block boundaries.
- `replace_block_range(blocks, addr, old_len, new_data)`: replaces a range within
  a block with new data of a different length. The block is resized accordingly.
  If `old_len` exceeds the available space in the block (e.g., metadata claims a
  larger range than physically stored due to omitted trailing zeros), it is
  clamped to the actual available bytes.

### Serialization

`serialize_blocks()` writes blocks back to Intel HEX format:
- Type 02 (segment) or Type 04 (linear) records are emitted when the base changes
- Data records use 16-byte chunks
- Output uses LF line endings
- Ends with `:00000001FF` (EOF record)

## Usage

```bash
lua hextract <command> [args]
```

### Commands

**`structure <file>`** - Inspect the structure of an Intel HEX file.

Output:
- Data regions with physical address boundaries and sizes
- LUA metadata block analysis with sanity checks (auto-detected location)
- Script preview (ASCII-printable characters)
- Summary statistics (record counts, data bytes, address range, gaps)
- Checksum validation summary

**`extract <file> [<output>]`** - Extract the embedded Lua script from firmware.

- If `<output>` is `-` or omitted, writes to stdout
- Reads script from `Start` to `End` address per metadata
- Warns if `Size` field differs from `End - Start`

**`embed <firmware.hex> <script> [<output.hex>] [--overwrite|--in-place]`** - Embed a Lua script into firmware.

- Replaces the existing script payload at the address specified by metadata
- Updates metadata (`End`, `Size`) to reflect the new script
- Resizes the containing block to match the new script length
- If `<output.hex>` is omitted and `--overwrite` is not set, prompts for confirmation
- If `<output.hex>` is omitted and `--overwrite` is set, writes back to the input file
- Rejects scripts larger than `Space`, the space the metadata reports

## Tests

Run the test suite with:

```bash
bash tests/hextract-tests.sh
```

The suite covers:
- Structure parsing and metadata detection
- Extract to file and stdout
- Embed idempotency (same data twice produces identical output)
- Embed smaller/larger scripts with round-trip verification
- Rejection of oversized scripts
- Checksum validity after embed
- Sequential embeds (shrink -> grow -> shrink)
- Round-trip integrity (embed then extract matches source)
- `--overwrite` flag behavior
- Output file isolation (original preserved when writing to different file)
- LF line endings in output
- Metadata start address stability after embed
