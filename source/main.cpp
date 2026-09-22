// -*- mode: c++; indent-tabs-mode: nil; -*-
#include <cstdint>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

// metadata descriptor of the lua-script block in the firmware
struct LuaMeta {
    uint32_t magic;
    uint32_t start;
    uint32_t end;
    uint32_t size;
    uint32_t space;
};

#define LUA_META_MAGIC 0x4C554131

// defined by the linker script
extern const LuaMeta __lua_meta;

#include "MicroBit.h"
#if CONFIG_ENABLED(DEVICE_BLE)
#include "MicroBitBLEManager.h"
#include "MicroBitUARTService.h"
#endif
#include "codal-lua.h"

MicroBit uBit;

#if CONFIG_ENABLED(DEVICE_BLE)
// UART service (Nordic UART over BLE)
MicroBitUARTService *uart;

void onBLEConnected(MicroBitEvent)
{
    uBit.display.print("C");  // Connected
}

void onBLEDisconnected(MicroBitEvent)
{
    uBit.display.print("D");  // Disconnected
}
#endif

static int lua_panic_handler(lua_State *L) {
    (void)L;
    while (true) {
        uBit.display.scroll("Lua panic");
        uBit.sleep(1000);
    }
}

#if CONFIG_ENABLED(DEVICE_BLE)
void setup_ble_uart_service() {
    // Create UART service over BLE. The Lua REPL owns all RX/TX handling:
    // it arms a per-char head-match (eventAfter) and parses length-prefixed
    // frames from the RX stream.
    uart = new MicroBitUARTService(*uBit.ble, 128, 64);

    // Register event handlers
    uBit.messageBus.listen(
        MICROBIT_ID_BLE,
        MICROBIT_BLE_EVT_CONNECTED,
        onBLEConnected
    );

    uBit.messageBus.listen(
        MICROBIT_ID_BLE,
        MICROBIT_BLE_EVT_DISCONNECTED,
        onBLEDisconnected
    );
}
#endif

int main() {
    uBit.init();

    // Enlarge the serial RX ring (default 20) so pasted lines don't overflow
    // before the REPL drain fiber catches up. 254 is the uint8_t API maximum.
    uBit.serial.setRxBufferSize(254);

#if CONFIG_ENABLED(DEVICE_BLE)
    setup_ble_uart_service();
#endif

    DMESG("main speaking");

    if ((__lua_meta.end - __lua_meta.start != __lua_meta.size)
        || __lua_meta.magic != LUA_META_MAGIC)
    {
        while (true)
        {
            uBit.display.scroll("LuaMeta block problem, aborting");
            uBit.sleep(1000);
        }
    }

    /* Create Lua state using CODAL heap */
    lua_State *L = luaL_newstate();
    if (!L) {
        lua_panic_handler(NULL);
    }

    lua_atpanic(L, lua_panic_handler);

    /* Load standard libraries */
    luaopen_base(L);
    luaopen_table(L);
    luaopen_string(L);
    luaopen_math(L);

    register_lua_api(L);
    // Register the MessageBus listener BEFORE running the script so that
    // events queued during / after script execution are never missed.
    register_lua_event_listener(L);

    if (luaL_loadbuffer(L, (const char*)__lua_meta.start,
                        __lua_meta.size, "embedded") == LUA_OK)
    {
        if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK)
        {
            const char *err = lua_tostring(L, -1);
            uBit.display.scroll("Lua error!");
            if (err) {
                uBit.display.scroll(err);
            }
        }
    }
    else
    {
        const char* err = lua_tostring(L, -1);
        uBit.display.scroll("Compile error: ");
        if (err) {
            uBit.display.scroll(err);
        }
    }

    // Don't lua_close(L) — the Lua state must stay alive for the event
    // listener callback (on_codal_event) to call lua_pcall later.
    // The event handler fibers are children of the idle fiber, so the
    // scheduler keeps them alive without consuming a user fiber slot.
    release_fiber();
}
