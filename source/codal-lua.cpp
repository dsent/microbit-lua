// -*- mode: c++; indent-tabs-mode: nil; -*-
#include <stdio.h>
#include <string.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "codal-lua.h"
#include "MicroBit.h"
#include "Event.h"
#include "I2C.h"
#include "neopixel.h"
#include "stack-probe.h"
#include "tpbot.h"
#include "lua-events.h"
#include "lua-modules.h"
#include "board-alloc.h"
#include "radio-link.h"
#include "link-words.h"

extern MicroBit uBit;

// Bind the Lua i2c API to CODAL's pre-wired EXTERNAL edge-connector bus
// (MICROBIT_PIN_EXT_SDA=P1_00 / MICROBIT_PIN_EXT_SCL=P0_26), where the TPBot
// lives.
I2C &i2c = uBit.i2c;

// The version build.py derives from git: the commit, "-drift" when the tree
// differed from it, "unknown" outside git. It sits in the image after a fixed
// mark, so a tool reading a hex file can find which firmware the file holds.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "unknown"
#endif
#define FIRMWARE_VERSION_MARK "microbit-lua firmware "
extern "C" __attribute__((used))
const char firmware_version_mark[] = FIRMWARE_VERSION_MARK FIRMWARE_VERSION;

#define LUA_MICROBIT_FUNCTIONS						\
    F(reset,      { uBit.reset();					\
                    return 0;						\
                  })							\
    F(sleep,      { uint32_t ms = (uint32_t)luaL_checkinteger(L, 1);	\
                    lua_events_sleep(ms);				\
                    return 0;						\
                  })							\
    F(seedRandom, { uint32_t seed = (uint32_t)luaL_optinteger(L, 1, 0);	\
                    if(seed) { uBit.seedRandom(seed); }			\
                    else { uBit.seedRandom(); }				\
                    return 0;						\
		  })							\
    F(random,     { int max = (int)luaL_checkinteger(L, 1);		\
                    lua_pushinteger(L, (lua_Integer)uBit.random(max));	\
                    return 1;						\
                  })							\
    F(systemTime, { lua_pushinteger(L, (lua_Integer)uBit.systemTime());	\
                    return 1;						\
                  })							\
    F(serialNumber, { char buf[12];					\
                    snprintf(buf, sizeof buf, "%lu",			\
                             (unsigned long)microbit_serial_number());	\
                    lua_pushstring(L, buf);				\
                    return 1;						\
                  })							\
    F(friendlyName, { lua_pushstring(L, microbit_friendly_name());	\
                    return 1;						\
                  })							\
    F(version,    { lua_pushstring(L, firmware_version_mark		\
                                   + sizeof FIRMWARE_VERSION_MARK - 1);	\
                    return 1;						\
                  })							\
    F(stackUsage, { lua_pushinteger(L, (lua_Integer)stack_probe_peak());	\
                    return 1;						\
                  })							\
    F(stackReset, { stack_probe_paint();				\
                    return 0;						\
                  })							\
    F(stackCurrent, { lua_pushinteger(L, (lua_Integer)stack_probe_current()); \
                    return 1;						\
                  })							\
    F(eventsDropped, { return lua_events_dropped(L); })		\
    F(eventFallback, { return lua_events_fallback(L); })		\
    F(eventLine,  { return lua_events_line(L); })			\
    F(eventRepl,  { return lua_events_repl(L); })			\
    F(panic,      { int statusCode = (int)luaL_checkinteger(L, 1);      \
                    microbit_panic(statusCode);				\
                    return 0;						\
                  })

// create a Lua image table from pixels and place it on the stack
static void lua_createimage(lua_State *L, int width, int height,
                            const uint8_t *data) {
  int size = width * height;
  lua_createtable(L, 0, 3);
  lua_pushliteral(L, "width");
  lua_pushinteger(L, width);
  lua_settable(L, -3);
  lua_pushliteral(L, "height");
  lua_pushinteger(L, height);
  lua_settable(L, -3);
  lua_pushliteral(L, "data");
  lua_pushlstring(L, (const char *)data, size);
  lua_settable(L, -3);
}

// The display's image as a Lua table. The screenshot is copied out and
// let go first: making the table can raise an error, which would jump
// past a C++ owner's destructor.
static int push_screenshot(lua_State *L) {
  uint8_t pixels[5 * 5];
  int width = 0, height = 0;
  {
    Image shot = uBit.display.screenShot();
    if (shot.getWidth() * shot.getHeight() <= (int)sizeof pixels) {
      width = shot.getWidth();
      height = shot.getHeight();
      memcpy(pixels, shot.getBitmap(), width * height);
    }
  }
  lua_createimage(L, width, height, pixels);
  return 1;
}

// An image table's pixels, read into a buffer on Lua's heap left on the
// stack, and its width and height: all the Lua is done here, before any
// C++ Image is made of them (see push_codal_text). data is a string, a
// byte a pixel row by row, or a table of numbers, one a pixel; pixels it
// does not reach stay off. Its size and the kind of its data are checked
// before the buffer is asked for: a buffer the heap cannot give stops the
// board with panic 020, where a check is an argument error.
#define IMAGE_PIXELS_MAX 4096
static const uint8_t *luaL_checkimage(lua_State *L, int narg,
                                      int *width, int *height) {
  uint8_t *pixels;
  size_t size, count, i;
  int kind;
  luaL_checktype(L, narg, LUA_TTABLE);
  lua_getfield(L, narg, "width");
  *width = (int)lua_tointeger(L, -1);
  lua_getfield(L, narg, "height");
  *height = (int)lua_tointeger(L, -1);
  lua_pop(L, 2);
  luaL_argcheck(L, 0 <= *width && 0 <= *height, narg,
                "an image's width and height are 0 or more");
  luaL_argcheck(L, *width <= IMAGE_PIXELS_MAX && *height <= IMAGE_PIXELS_MAX
                && *width * *height <= IMAGE_PIXELS_MAX, narg,
                "an image has at most 4096 pixels, width times height");
  count = (size_t)*width * *height;
  lua_getfield(L, narg, "data");
  kind = lua_type(L, -1);
  luaL_argcheck(L, kind == LUA_TSTRING || kind == LUA_TTABLE, narg,
                "an image's data is a string, or a list of numbers, one "
                "for each pixel");
  pixels = (uint8_t *)lua_newuserdata(L, count);
  memset(pixels, 0, count);
  if (kind == LUA_TSTRING) {
    const char *data = lua_tolstring(L, -2, &size);
    memcpy(pixels, data, size < count ? size : count);
  } else {
    for (i = 0; i < count; i++) {
      lua_pushinteger(L, (lua_Integer)i + 1);
      lua_gettable(L, -3);
      pixels[i] = (uint8_t)lua_tointeger(L, -1);
      lua_pop(L, 1);
    }
  }
  lua_remove(L, -2);
  return pixels;
}

// The most a read may ask for at once, serial or i2c: a buffer the heap
// cannot give stops the board with panic 020, where this is an argument
// error.
#define READ_MAX 4096
#define READ_RANGE "from 0 to 4096"

// read(n) of a port: up to n bytes, read into a buffer on Lua's heap and
// returned as a string. CODAL's read(n) puts n bytes on the C stack, below
// any check (source/lua-cstack.c).
template <typename Read>
static int read_into_lua(lua_State *L, Read read) {
  int size = luaL_checkint(L, 1);
  luaL_argcheck(L, 0 <= size && size <= READ_MAX, 1, READ_RANGE);
  uint8_t *buffer = (uint8_t *)lua_newuserdata(L, size);
  int got = read(buffer, size);
  lua_pushlstring(L, (const char *)buffer, got > 0 ? got : 0);
  return 1;
}

// see https://rneacy.dev/mbv2/ubit/display/
#define LUA_DISPLAY_FUNCTIONS						\
    F(getWidth,   { lua_pushinteger(L, uBit.display.getWidth());	\
                    return 1;						\
                  })							\
    F(getHeight,  { lua_pushinteger(L, uBit.display.getHeight());	\
                    return 1;						\
                  })							\
    F(setBrightness, { int b = luaL_checkint(L, 1);			\
                    int r = uBit.display.setBrightness(b);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(getBrightness, { int r = uBit.display.getBrightness();		\
                    lua_pushinteger(L, r);				\
                    return 1;						\
                  })							\
    F(enable,     { uBit.display.enable();				\
                    return 0;						\
                  })							\
    F(disable,     { uBit.display.disable();				\
                    return 0;						\
                  })							\
    F(screenShot, { return push_screenshot(L); })			\
    F(setDisplayMode, { DisplayMode mode = 				\
                      static_cast<DisplayMode>(luaL_checkinteger(L, 1));\
                    uBit.display.setDisplayMode(mode);			\
                    return 0;						\
                  })							\
    F(getDisplayMode, { DisplayMode mode =				\
                      uBit.display.getDisplayMode();			\
                    lua_pushinteger(L, static_cast<lua_Integer>(mode));	\
                    return 1;						\
                  })							\
    F(clear,      { uBit.display.clear();				\
                    return 0;						\
                  })							\
    F(readLightLevel, { lua_events_before_wait();			\
                    /* the first reading sleeps while the LEDs measure */ \
                    int r = uBit.display.readLightLevel();		\
                    lua_pushinteger(L, r);				\
                    return 1;						\
                  })							\
    F(setSleep,   { luaL_checkany(L, 1);				\
                    uBit.display.NRF52LEDMatrix::setSleep(		\
                      lua_toboolean(L, 1) != 0);			\
                    return 0;						\
                  })							\
    F(stopAnimation, { uBit.display.stopAnimation();			\
                    return 0;						\
                  })							\
    F(printAsync, { const char *s = luaL_checkstring(L, 1);		\
                    int delay = luaL_optint(L, 2,			\
                      DISPLAY_DEFAULT_PRINT_SPEED);			\
                    int r = uBit.display.printAsync(s, delay);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(print,      { lua_events_before_wait(); const char *s = luaL_checkstring(L, 1);		\
                    int delay = luaL_optint(L, 2,			\
                      DISPLAY_DEFAULT_PRINT_SPEED);			\
                    int r = uBit.display.print(s, delay);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(scrollAsync, { const char *s = luaL_checkstring(L, 1);		\
                    int delay = luaL_optint(L, 2,			\
                      DISPLAY_DEFAULT_SCROLL_SPEED);			\
                    int r = uBit.display.scrollAsync(s, delay);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(scroll,     { lua_events_before_wait(); const char *s = luaL_checkstring(L, 1);		\
                    int delay = luaL_optint(L, 2,			\
                      DISPLAY_DEFAULT_SCROLL_SPEED);			\
                    int r = uBit.display.scroll(s, delay);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(animateAsync, { int width;					\
                    int height;						\
                    int delay = luaL_checkint(L, 2);			\
                    int stride = luaL_checkint(L, 3);			\
                    int startingPosition =				\
                      luaL_optint(L, 4, DISPLAY_ANIMATE_DEFAULT_POS);	\
                    int autoClear =					\
                      luaL_optint(L, 5, DISPLAY_DEFAULT_AUTOCLEAR);	\
                    const uint8_t *pixels =				\
                      luaL_checkimage(L, 1, &width, &height);		\
                    int r = uBit.display.animateAsync(			\
                                                      Image(width, height, pixels),\
                                                      delay,		\
                                                      stride,		\
                                                      startingPosition,	\
                                                      autoClear);	\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(animate,    { lua_events_before_wait(); int width;		\
                    int height;						\
                    int delay = luaL_checkint(L, 2);			\
                    int stride = luaL_checkint(L, 3);			\
                    int startingPosition =				\
                      luaL_optint(L, 4, DISPLAY_ANIMATE_DEFAULT_POS);	\
                    int autoClear =					\
                      luaL_optint(L, 5, DISPLAY_DEFAULT_AUTOCLEAR);	\
                    const uint8_t *pixels =				\
                      luaL_checkimage(L, 1, &width, &height);		\
                    int r = uBit.display.animate(				\
                                                 Image(width, height, pixels),\
                                                 delay,			\
                                                 stride,		\
                                                 startingPosition,	\
                                                 autoClear);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(show,       { lua_events_before_wait(); int width;		\
                    int height;						\
                    const uint8_t *pixels =				\
                      luaL_checkimage(L, 1, &width, &height);		\
                    int r = uBit.display.print(Image(width, height, pixels));\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(setPixelValue, {							\
                    uint16_t x = (uint16_t)luaL_checkint(L, 1);		\
                    uint16_t y = (uint16_t)luaL_checkint(L, 2);		\
                    uint8_t value = (uint8_t)luaL_checkint(L, 3);	\
                    int r =						\
                      uBit.display.image.setPixelValue(x, y, value);	\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(getPixelValue, {							\
                    uint16_t x = (uint16_t)luaL_checkint(L, 1);		\
                    uint16_t y = (uint16_t)luaL_checkint(L, 2);		\
                    int r = uBit.display.image.getPixelValue(x, y);	\
                    if(r != DEVICE_INVALID_PARAMETER) {			\
                      lua_pushinteger(L, r);				\
                      return 1;						\
                    }							\
                    return 0;						\
                  })

// see https://rneacy.dev/mbv2/ubit/accelerometer/
#define LUA_ACCELEROMETER_FUNCTIONS					\
    F(setPeriod,  { int period = luaL_checkint(L, 1);			\
                    int r = uBit.accelerometer.setPeriod(period);	\
                    lua_pushboolean(L, r == MICROBIT_OK);		\
                    return 1;						\
                  })							\
    F(getPeriod,  { int r = uBit.accelerometer.getPeriod();		\
                    lua_pushinteger(L, r);				\
                    return 1;						\
                  })							\
    F(setRange,   { int range = luaL_checkint(L, 1);			\
                    int r = uBit.accelerometer.setRange(range);		\
                    lua_pushboolean(L, r == MICROBIT_OK);		\
                    return 1;						\
                  })							\
    F(getRange,   { int r = uBit.accelerometer.getRange();		\
                    lua_pushinteger(L, r);				\
                    return 1;						\
                  })							\
    F(configure,  { int r = uBit.accelerometer.configure();		\
                    lua_pushboolean(L, r == MICROBIT_OK);		\
                    return 1;						\
                  })							\
    F(requestUpdate, { int r = uBit.accelerometer.requestUpdate();	\
                    lua_pushboolean(L, r == MICROBIT_OK);		\
                    return 1;						\
                  })							\
    F(getSample,  { Sample3D sample = uBit.accelerometer.getSample();	\
                    lua_pushinteger(L, sample.x);			\
                    lua_pushinteger(L, sample.y);			\
                    lua_pushinteger(L, sample.z);			\
                    return 3;						\
                  })							\
    F(getGesture, { uint16_t r = uBit.accelerometer.getGesture();	\
                    lua_pushinteger(L, r);				\
                    return 1;						\
                  })

// see https://rneacy.dev/mbv2/ubit/compass/
#define LUA_COMPASS_FUNCTIONS						\
    F(heading,     { int r = uBit.compass.heading();			\
                     if(r == DEVICE_CALIBRATION_IN_PROGRESS) {		\
                       lua_pushnil(L);					\
                     } else {						\
                       lua_pushinteger(L, r);				\
                     }							\
                     return 1;						\
                   })							\
    F(getFieldStrength, { int r = uBit.compass.getFieldStrength();	\
                     lua_pushinteger(L, r);				\
                     return 1;						\
                   })							\
    F(calibrate,   { int r = uBit.compass.calibrate();			\
                     lua_pushboolean(L, r == MICROBIT_OK);		\
                     return 1;						\
                   })							\
    F(setCalibration, { CompassCalibration cc = CompassCalibration();	\
                     cc.centre.x = luaL_optint(L, 1, 0);		\
                     cc.centre.y = luaL_optint(L, 2, 0);		\
                     cc.centre.z = luaL_optint(L, 3, 0);		\
                     cc.scale.x = luaL_optint(L, 4, 1024);		\
                     cc.scale.y = luaL_optint(L, 5, 1024);		\
                     cc.scale.z = luaL_optint(L, 6, 1024);		\
                     cc.radius = luaL_optint(L, 7, 0);			\
                     uBit.compass.setCalibration(cc);			\
                     return 0;						\
                   })							\
    F(getCalibration, { CompassCalibration cc =				\
                       uBit.compass.getCalibration();			\
                     lua_pushinteger(L, cc.centre.x);			\
                     lua_pushinteger(L, cc.centre.y);			\
                     lua_pushinteger(L, cc.centre.z);			\
                     lua_pushinteger(L, cc.scale.x);			\
                     lua_pushinteger(L, cc.scale.y);			\
                     lua_pushinteger(L, cc.scale.z);			\
                     lua_pushinteger(L, cc.radius);			\
                     return 7;						\
                   })							\
    F(isCalibrated, { int r = uBit.compass.isCalibrated();		\
                     lua_pushboolean(L, r);				\
                     return 1;						\
                   })							\
    F(isCalibrating, { int r = uBit.compass.isCalibrating();		\
                     lua_pushboolean(L, r);				\
                     return 1;						\
                   })							\
    F(clearCalibration, { uBit.compass.clearCalibration();		\
                     return 0;						\
                   })							\
    F(configure,   { int r = uBit.compass.configure();			\
                     lua_pushboolean(L, r == MICROBIT_OK);		\
                     return 1;						\
                   })							\
    F(setPeriod,   { int period = luaL_checkint(L, 1);			\
                     int r = uBit.compass.setPeriod(period);		\
                     lua_pushboolean(L, r == MICROBIT_OK);		\
                     return 1;						\
                   })							\
    F(getPeriod,   { int r = uBit.compass.getPeriod();			\
                     lua_pushinteger(L, r);				\
                     return 1;						\
                   })							\
    F(requestUpdate, { int r = uBit.compass.requestUpdate();		\
                     lua_pushboolean(L, r == MICROBIT_OK);		\
                     return 1;						\
                   })							\
    F(update,      { int r = uBit.compass.update();			\
                     lua_pushboolean(L, r == MICROBIT_OK);		\
                     return 1;						\
                   })							\
    F(getSample,  { Sample3D sample = uBit.compass.getSample();		\
                    lua_pushinteger(L, sample.x);			\
                    lua_pushinteger(L, sample.y);			\
                    lua_pushinteger(L, sample.z);			\
                    return 3;						\
                  })

#define LUA_AUDIO_FUNCTIONS						\
    F(getAudioPin, { lua_pushlightuserdata(L,				\
                      &uBit.audio.virtualOutputPin);			\
                    return 1;						\
                  })							\
    F(setVolume,  { int volume = luaL_checkint(L, 1);			\
                    int r = uBit.audio.setVolume(volume);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(getVolume,  { lua_pushinteger(L, uBit.audio.getVolume());		\
                    return 1;						\
                  })							\
    F(express,    { const char *expression = luaL_checkstring(L, 1);	\
                    uBit.audio.soundExpressions.playAsync(expression);	\
                    return 0;						\
                  })

Pin *luaL_checkPin(lua_State *L, int narg) {
  luaL_checktype(L, narg, LUA_TLIGHTUSERDATA);
  return (Pin *)lua_touserdata(L, narg);
}

#define LUA_IO_FUNCTIONS						\
    F(setDigitalValue, { 						\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int value = luaL_checkint(L, 2);			\
                    int r = pin->setDigitalValue(value);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(getDigitalValue, {						\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->getDigitalValue();			\
                    if(r == 0 || r == 1) {				\
                      lua_pushinteger(L, r);				\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    F(setAnalogValue, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int value = luaL_checkint(L, 2);			\
                    int r = pin->setAnalogValue(value);			\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(setServoValue, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int value = luaL_checkint(L, 2);			\
                    int range = luaL_optint(L, 3,			\
                      DEVICE_PIN_DEFAULT_SERVO_RANGE);			\
                    int center = luaL_optint(L, 4,			\
                      DEVICE_PIN_DEFAULT_SERVO_CENTER);			\
                    int r = pin->setServoValue(value, range, center);	\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(getAnalogValue, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->getAnalogValue();			\
                    if(r >= 0 && r <= 1024) {				\
                      lua_pushinteger(L, r);				\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    F(isInput,    { Pin *pin = luaL_checkPin(L, 1);			\
                    lua_pushboolean(L, pin->isInput() == 1);		\
                    return 1;						\
                  })							\
    F(isOutput,   { Pin *pin = luaL_checkPin(L, 1);			\
                    lua_pushboolean(L, pin->isOutput() == 1);		\
                    return 1;						\
                  })							\
    F(isDigital,  { Pin *pin = luaL_checkPin(L, 1);			\
                    lua_pushboolean(L, pin->isDigital() == 1);		\
                    return 1;						\
                  })							\
    F(isAnalog,   { Pin *pin = luaL_checkPin(L, 1);			\
                    lua_pushboolean(L, pin->isAnalog() == 1);		\
                    return 1;						\
                  })							\
    F(isTouched,  { Pin *pin = luaL_checkPin(L, 1);			\
                    lua_pushboolean(L, pin->isTouched() == 1);		\
                    return 1;						\
                  })							\
    F(setServoPulseUs, {						\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    uint32_t pulseWidth =				\
                      (uint32_t)luaL_checkinteger(L, 2); 		\
                    int r = pin->setServoPulseUs(pulseWidth);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(setAnalogPeriod, {						\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int period = luaL_checkint(L, 2);			\
                    int r = pin->setAnalogPeriod(period);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(setAnalogPeriodUs, {						\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    uint32_t period =					\
                      (uint32_t)luaL_checkinteger(L, 2);		\
                    int r = pin->setAnalogPeriodUs(period);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(getAnalogPeriodUs, {						\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    uint32_t r = pin->getAnalogPeriodUs();		\
                    if((int)r != DEVICE_NOT_SUPPORTED) {		\
                      lua_pushinteger(L, r);				\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    F(getAnalogPeriod, {						\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->getAnalogPeriod();			\
                    if(r != DEVICE_NOT_SUPPORTED) {			\
                      lua_pushinteger(L, r);				\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    F(setPullUp,  { Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->setPull((PullMode)PullUp);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(setPullDown, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->setPull((PullMode)PullDown);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(setPullNone, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->setPull((PullMode)PullNone);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(drainPin,   { Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->drainPin();				\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(pulseUs,    { Pin *pin = luaL_checkPin(L, 1);			\
                    int value = luaL_checkint(L, 2);			\
                    uint64_t width_us = luaL_checklong(L, 3);		\
                    int r = pin->setDigitalValue(value);		\
                    uint64_t start = system_timer_current_time_us();	\
                    if( r == DEVICE_OK ) {				\
                      while(system_timer_current_time_us() - start <	\
                            width_us) { /* busy wait */ }		\
                      lua_pushboolean(L,				\
                        pin->setDigitalValue(1 - value) == DEVICE_OK);	\
                    } else {						\
                      lua_pushboolean(L, 0);				\
                    }							\
                    return 1;						\
                  })							\
    F(getPulseUs, { lua_events_before_wait(); Pin *pin = luaL_checkPin(L, 1);			\
                    int timeout = luaL_checkint(L, 2);			\
                    int r = pin->getPulseUs(timeout);			\
                    if(r != DEVICE_CANCELLED) {				\
                      lua_pushinteger(L, r);				\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    F(eventOnEdge, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->eventOn(DEVICE_PIN_EVENT_ON_EDGE);	\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(eventOnPulse, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->eventOn(DEVICE_PIN_EVENT_ON_PULSE);	\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(eventOnTouch, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->eventOn(DEVICE_PIN_EVENT_ON_TOUCH);	\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(eventNone,  { Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->eventOn(DEVICE_PIN_EVENT_NONE);	\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(isActive,   { Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->isActive();				\
                    lua_pushboolean(L, r == 1);				\
                    return 1;						\
                  })							\
    F(setPolarity, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int polarity = luaL_checkint(L, 2);			\
                    pin->setPolarity(polarity);				\
                    return 0;						\
                  })							\
    F(getPolarity, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    lua_pushinteger(L, pin->getPolarity());		\
                    return 1;						\
                  })							\
    F(setActiveHi, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    pin->setActiveHi();					\
                    return 0;						\
                  })							\
    F(setActiveLo, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    pin->setActiveLo();					\
                    return 0;						\
                  })							\
    F(disconnect, { Pin *pin = luaL_checkPin(L, 1);			\
                    pin->disconnect();					\
                    return 0;						\
                  })							\
    F(getPin,     { int pin = luaL_checkint(L, 1);			\
                    /* 30 and 31 are the USB serial pins, to redirect	\
                       the port back to the console */			\
                    luaL_argcheck(L, 0 <= pin && pin < uBit.io.pins, 1,	\
                                  "from 0 to 32");			\
                    lua_pushlightuserdata(L, &uBit.io.pin[pin]);	\
                    return 1;						\
                  })

// Text CODAL makes, pushed onto Lua's stack. A Lua error raised while a
// C++ object is alive jumps past its destructor, and what the object holds
// on the heap is never given back. Pushing text can raise one: the string
// it makes can start a step of the collector, which runs a program's __gc
// finalizers (newproxy), unprotected, and one may fail, or find the C
// stack full. So the text is pushed in a protected call, and an error
// raised only once its owner is gone.
static int push_text_ref = LUA_NOREF;

static int push_text(lua_State *L) {
  const ManagedString *text = (const ManagedString *)lua_touserdata(L, 1);
  lua_pushlstring(L, text->toCharArray(), text->length());
  return 1;
}

// What make() returns, pushed
template <typename Make>
static int push_codal_text(lua_State *L, Make make) {
  int status;
  {
    ManagedString text = make();
    lua_rawgeti(L, LUA_REGISTRYINDEX, push_text_ref);
    lua_pushlightuserdata(L, &text);
    status = lua_pcall(L, 1, 1, 0);
  }
  return status == 0 ? 1 : lua_error(L);
}

ManagedString luaL_checkManagedString(lua_State *L, int narg) {
  size_t length;
  const char *str = luaL_checklstring(L, narg, &length);
  return ManagedString(str, length);
}

#define LUA_SERIAL_FUNCTIONS						\
    F(send,       { lua_events_before_wait(); ManagedString s = luaL_checkManagedString(L, 1);	\
                    int r = uBit.serial.send(s, SYNC_SLEEP);		\
                    if(r != DEVICE_SERIAL_IN_USE &&			\
                       r != DEVICE_INVALID_PARAMETER) {			\
                      lua_pushinteger(L, r);				\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    F(sendAsync,  { ManagedString s = luaL_checkManagedString(L, 1);	\
                    int r = uBit.serial.send(s, ASYNC);			\
                    if(r != DEVICE_SERIAL_IN_USE &&			\
                       r != DEVICE_INVALID_PARAMETER) {			\
                      lua_pushinteger(L, r);				\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    F(getByte,    { lua_events_before_wait(); int r = uBit.serial.getChar(SYNC_SLEEP);		\
                    lua_pushinteger(L, r);				\
                    return 1;						\
                  })							\
    F(getByteAsync, {							\
                    int r = uBit.serial.getChar(ASYNC);			\
                    if(r != DEVICE_NO_DATA) {				\
                      lua_pushinteger(L, r);				\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    F(getChar,    { lua_events_before_wait(); char r = (char)uBit.serial.getChar(SYNC_SLEEP);	\
                    lua_pushlstring(L, &r, 1);				\
                    return 1;						\
                  })							\
    F(getCharAsync, {							\
                    int r = uBit.serial.getChar(ASYNC);			\
                    if(r != DEVICE_NO_DATA) {				\
                      char c = (char)r;					\
                      lua_pushlstring(L, &c, 1);			\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    F(read,       { lua_events_before_wait();				\
                    return read_into_lua(L, [](uint8_t *b, int n) {	\
                      return uBit.serial.read(b, n, SYNC_SLEEP); });	\
                  })							\
    F(readAsync,  { return read_into_lua(L, [](uint8_t *b, int n) {	\
                      return uBit.serial.read(b, n, ASYNC); });		\
                  })							\
    F(readUntil,  { lua_events_before_wait();				\
                    size_t n;						\
                    const char *d = luaL_checklstring(L, 1, &n);	\
                    return push_codal_text(L, [&]() {			\
                      return uBit.serial.readUntil(ManagedString(d, n),	\
                                                   SYNC_SLEEP); });	\
                  })							\
    F(setBaud,    { int baudrate = luaL_checkint(L, 1);			\
                    int r = uBit.serial.setBaud(baudrate);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(redirect,   { Pin *tx = luaL_checkPin(L, 1);			\
                    Pin *rx = luaL_checkPin(L, 2);			\
                    int r = uBit.serial.redirect(*tx, *rx);		\
                    if (r == DEVICE_OK)					\
                      port_is_console = tx == &uBit.io.usbTx		\
                                        && rx == &uBit.io.usbRx;	\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(eventAfter, { lua_events_before_wait(); uBit.serial.eventAfter(luaL_checkint(L, 1),		\
                                           SYNC_SLEEP);			\
                    return 0;						\
                  })							\
    F(eventAfterAsync, {						\
                    uBit.serial.eventAfter(luaL_checkint(L, 1),		\
                                           ASYNC);			\
                    return 0;						\
                  })							\
    F(eventOn,    { lua_events_before_wait(); uBit.serial.eventOn(luaL_checkManagedString(L, 1),	\
                                           SYNC_SLEEP);			\
                    return 0;						\
                  })							\
    F(eventOnAsync, {							\
                    uBit.serial.eventOn(luaL_checkManagedString(L, 1),	\
                                           ASYNC);			\
                    return 0;						\
                  })							\
    F(isReadable, { int r = uBit.serial.isReadable();			\
                    if(r == 0 || r == 1) {				\
                      lua_pushboolean(L, r == 1);			\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    F(isWriteable, {							\
                    lua_pushboolean(L, uBit.serial.isWriteable() == 1);	\
                    return 1;						\
                  })							\
    F(setRxBufferSize, {						\
                    uint8_t size = luaL_checkint(L, 1);			\
                    int r = uBit.serial.setRxBufferSize(size);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(setTxBufferSize, {						\
                    uint8_t size = luaL_checkint(L, 1);			\
                    int r = uBit.serial.setTxBufferSize(size);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(getRxBufferSize, {						\
                    lua_pushinteger(L, uBit.serial.getRxBufferSize());	\
                    return 1;						\
                  })							\
    F(getTxBufferSize, {						\
                    lua_pushinteger(L, uBit.serial.getTxBufferSize());	\
                    return 1;						\
                  })							\
    F(clearRxBuffer, {							\
                    lua_pushboolean(L,					\
                      uBit.serial.clearRxBuffer() == DEVICE_OK);	\
                    return 1;						\
                  })							\
    F(clearTxBuffer, {							\
                    lua_pushboolean(L,					\
                      uBit.serial.clearTxBuffer() == DEVICE_OK);	\
                    return 1;						\
                  })							\
    F(rxBufferedSize, {							\
                    lua_pushinteger(L, uBit.serial.rxBufferedSize());	\
                    return 1;						\
                  })							\
    F(txBufferedSize, {							\
                    lua_pushinteger(L,uBit.serial.txBufferedSize());	\
                    return 1;						\
                  })							\
    F(rxInUse,    { lua_pushboolean(L,					\
                      uBit.serial.rxInUse() != 0);			\
                    return 1;						\
                  })							\
    F(txInUse,    { lua_pushboolean(L,					\
                      uBit.serial.txInUse() != 0);			\
                    return 1;						\
                  })

#if CONFIG_ENABLED(DEVICE_BLE)
#include "MicroBitUARTService.h"

extern MicroBitUARTService *uart;

#define LUA_BLE_FUNCTIONS						\
    F(send,       { if(!uart) { lua_pushnil(L); return 1; }		\
                    lua_events_before_wait();				\
                    ManagedString s = luaL_checkManagedString(L, 1);	\
                    int r = uart->send(s, SYNC_SLEEP);			\
                    if(r != DEVICE_SERIAL_IN_USE &&			\
                       r != DEVICE_INVALID_PARAMETER) {			\
                      lua_pushinteger(L, r);				\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    F(sendAsync,  { if(!uart) { lua_pushnil(L); return 1; }		\
                    ManagedString s = luaL_checkManagedString(L, 1);	\
                    int r = uart->send(s, ASYNC);			\
                    if(r != DEVICE_SERIAL_IN_USE &&			\
                       r != DEVICE_INVALID_PARAMETER) {			\
                      lua_pushinteger(L, r);				\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    F(getChar,    { if(!uart) { lua_pushnil(L); return 1; }		\
                    lua_events_before_wait();				\
                    char r = (char)uart->getc(SYNC_SLEEP);		\
                    lua_pushlstring(L, &r, 1);				\
                    return 1;						\
                  })							\
    F(getCharAsync, {							\
                    if(!uart) { lua_pushnil(L); return 1; }		\
                    int r = uart->getc(ASYNC);				\
                    if(r != MICROBIT_NO_DATA) {				\
                      char c = (char)r;					\
                      lua_pushlstring(L, &c, 1);			\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    F(read,       { if(!uart) { lua_pushnil(L); return 1; }		\
                    lua_events_before_wait();				\
                    return read_into_lua(L, [](uint8_t *b, int n) {	\
                      return uart->read(b, n, SYNC_SLEEP); });		\
                  })							\
    F(readAsync,  { if(!uart) { lua_pushnil(L); return 1; }		\
                    return read_into_lua(L, [](uint8_t *b, int n) {	\
                      return uart->read(b, n, ASYNC); });		\
                  })							\
    F(readUntil,  { if(!uart) { lua_pushnil(L); return 1; }		\
                    lua_events_before_wait();				\
                    size_t n;						\
                    const char *d = luaL_checklstring(L, 1, &n);	\
                    return push_codal_text(L, [&]() {			\
                      return uart->readUntil(ManagedString(d, n),	\
                                             SYNC_SLEEP); });		\
                  })							\
    F(eventOn,    { if(!uart) { return 0; }				\
                    lua_events_before_wait();				\
                    uart->eventOn(luaL_checkManagedString(L, 1),	\
                                           SYNC_SLEEP);			\
                    return 0;						\
                  })							\
    F(eventOnAsync, {							\
                    if(!uart) { return 0; }				\
                    uart->eventOn(luaL_checkManagedString(L, 1),	\
                                           ASYNC);			\
                    return 0;						\
                  })							\
    F(eventAfter, { if(!uart) { return 0; }				\
                    lua_events_before_wait();				\
                    uart->eventAfter(luaL_checkint(L, 1),		\
                                           SYNC_SLEEP);			\
                    return 0;						\
                  })							\
    F(eventAfterAsync, {						\
                    if(!uart) { return 0; }				\
                    uart->eventAfter(luaL_checkint(L, 1),		\
                                           ASYNC);			\
                    return 0;						\
                  })							\
    F(isReadable, { if(!uart) { lua_pushnil(L); return 1; }		\
                    int r = uart->isReadable();				\
                    if(r == 0 || r == 1) {				\
                      lua_pushboolean(L, r == 1);			\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    F(rxBufferedSize, {						\
                    if(!uart) { lua_pushnil(L); return 1; }		\
                    lua_pushinteger(L, uart->rxBufferedSize());		\
                    return 1;						\
                  })							\
    F(txBufferedSize, {						\
                    if(!uart) { lua_pushnil(L); return 1; }		\
                    lua_pushinteger(L, uart->txBufferedSize());		\
                    return 1;						\
                  })

#endif // CONFIG_ENABLED(DEVICE_BLE)

#define LUA_I2C_FUNCTIONS						\
    F(read,       { int address = luaL_checkint(L, 1);			\
                    int length = luaL_checkint(L, 2);			\
                    luaL_argcheck(L, 0 <= length && length <= READ_MAX,	\
                                  2, READ_RANGE);			\
                    /* on Lua's heap: the C stack has no room to spare */ \
                    char *data = (char *)lua_newuserdata(L, length);	\
                    if(i2c.read(address, data, length) == MICROBIT_OK){	\
                      lua_pushlstring(L, data, length);			\
                      return 1;						\
                    } else {						\
                      return luaL_error(L, "i2c read error");		\
                    }							\
                  })							\
    F(write,      { int address = luaL_checkint(L, 1);			\
                    size_t length;					\
                    char *data =					\
                      (char *)luaL_checklstring(L, 2, &length);		\
                    if(i2c.write(address, data, length)			\
                      == MICROBIT_OK){					\
                      return 0;						\
                    } else {						\
                      return luaL_error(L, "i2c write error");		\
                    }							\
                  })

#define LUA_I2C_COUNT 2


// Robot boards take their commands as bytes over I2C at address 32.

static uint8_t robot_byte(lua_State *L, int narg, int value) {
  luaL_argcheck(L, 0 <= value && value <= 255, narg, "out of range");
  return (uint8_t)value;
}

// A robot that does not answer is told in the words tpbot uses.
static int robot_send(lua_State *L, uint8_t *data, int length) {
  if (i2c.write(32, data, length) != MICROBIT_OK) {
    lua_pushstring(L, tpbot_no_answer);
    return lua_error(L);
  }
  return 0;
}

static void robot_read(lua_State *L, uint8_t *data, int length) {
  if (i2c.read(32, data, length) != MICROBIT_OK) {
    lua_pushstring(L, tpbot_no_answer);
    lua_error(L);
  }
}

// Width in microseconds of the next high pulse on pin, -1 on timeout.
// Polled: getPulseUs floods the Lua event handler with PulseIn ticks.
static int pulse_width(Pin &pin, uint32_t timeout) {
  uint64_t start = system_timer_current_time_us();
  while (pin.getDigitalValue() == 0)
    if (system_timer_current_time_us() - start > timeout) return -1;
  uint64_t rise = system_timer_current_time_us();
  while (pin.getDigitalValue() == 1)
    if (system_timer_current_time_us() - start > timeout) return -1;
  return system_timer_current_time_us() - rise;
}

// The longest a sonar's echo lasts, with nothing in front of it.
#define ECHO_LONGEST_US 60000

// What source/tpbot.c needs of the board. The bus and the sleep are the
// ones microbit.i2c.write and microbit.sleep use.

extern "C" int tpbot_i2c_write(int address, const char *data, size_t length) {
  return i2c.write(address, (char *)data, length) == MICROBIT_OK ? 0 : -1;
}

extern "C" void tpbot_sleep(uint32_t ms) {
  lua_events_before_wait();
  uBit.sleep(ms);
}

// The sonar, trigger on P16 and echo on P15. An echo still high from the
// reading before is let end first, so that its tail is not timed as this
// one: the echo timed is the first to rise after the trigger.
extern "C" int tpbot_echo_us(void) {
  Pin &trigger = uBit.io.pin[16];
  Pin &echo = uBit.io.pin[15];
  uint64_t start = system_timer_current_time_us();
  while (echo.getDigitalValue() == 1)
    if (system_timer_current_time_us() - start > ECHO_LONGEST_US) return -1;
  trigger.setDigitalValue(1);
  system_timer_wait_us(10);
  trigger.setDigitalValue(0);
  return pulse_width(echo, 25000);
}

// TPBot 2 (TPBot Edu) frames a command as 255, 249, its code, the number
// of parameters, then the parameters. Its sonar is the TPBot Classic one.
static void tpbot2_header(uint8_t *data, int code, int count) {
  data[0] = 255;
  data[1] = 249;
  data[2] = code;
  data[3] = count;
}

#define LUA_TPBOT2_FUNCTIONS						\
    F(set_car_light, {							\
                    uint8_t data[7];					\
                    tpbot2_header(data, 48, 3);				\
                    data[4] = robot_byte(L, 1, luaL_checkint(L, 1));	\
                    data[5] = robot_byte(L, 2, luaL_checkint(L, 2));	\
                    data[6] = robot_byte(L, 3, luaL_checkint(L, 3));	\
                    return robot_send(L, data, 7);			\
                  })							\
    F(set_motors_speed, {						\
                    int left = luaL_checkint(L, 1);			\
                    int right = luaL_checkint(L, 2);			\
                    uint8_t data[7];					\
                    tpbot2_header(data, 16, 3);				\
                    data[4] = robot_byte(L, 1, abs(left));		\
                    data[5] = robot_byte(L, 2, abs(right));		\
                    data[6] = (left < 0) + 2 * (right < 0);		\
                    return robot_send(L, data, 7);			\
                  })							\
    F(run_distance, {							\
                    int mm = luaL_checkint(L, 1);			\
                    if (mm == 0)					\
                      return 0;						\
                    int d = abs(mm);					\
                    uint8_t data[7];					\
                    tpbot2_header(data, 65, 3);				\
                    data[4] = robot_byte(L, 1, d / 256);		\
                    data[5] = d % 256;					\
                    data[6] = mm < 0 ? 3 : 0;				\
                    return robot_send(L, data, 7);			\
                  })							\
    F(turn, {								\
                    int deg = luaL_checkint(L, 1);			\
                    if (deg == 0)					\
                      return 0;						\
                    int d = abs(deg);					\
                    uint8_t data[9];					\
                    tpbot2_header(data, 66, 5);				\
                    data[4] = robot_byte(L, 1, d / 256);		\
                    data[5] = d % 256;					\
                    data[6] = data[4];					\
                    data[7] = data[5];					\
                    data[8] = deg < 0 ? 2 : 1;				\
                    return robot_send(L, data, 9);			\
                  })

// Nezha 2 takes 8-byte commands: 255, 249, the motor (argument 1), then
// five bytes.
static int nezha2_send(lua_State *L, int b3, int b4, int b5, int b6, int b7) {
  uint8_t data[8];
  data[0] = 255;
  data[1] = 249;
  data[2] = robot_byte(L, 1, luaL_checkint(L, 1));
  data[3] = b3;
  data[4] = b4;
  data[5] = b5;
  data[6] = b6;
  data[7] = b7;
  return robot_send(L, data, 8);
}

#define LUA_NEZHA2_FUNCTIONS						\
    F(motor_turn, {							\
                    int amount = luaL_checkint(L, 2);			\
                    int a = abs(amount);				\
                    return nezha2_send(L, amount < 0 ? 2 : 1, 112,	\
                      robot_byte(L, 2, a / 256),			\
                      robot_byte(L, 3, luaL_checkint(L, 3)), a % 256);	\
                  })							\
    F(motor_goto, {							\
                    int mode = robot_byte(L, 2, luaL_checkint(L, 2));	\
                    int a = (luaL_checkint(L, 3) % 360 + 360) % 360;	\
                    return nezha2_send(L, 0, 93, a / 256, mode, a % 256);\
                  })							\
    F(motor_reset, {							\
                    return nezha2_send(L, 0, 29, 0, 245, 0);		\
                  })							\
    F(motor_spin, {							\
                    int speed = luaL_checkint(L, 2);			\
                    return nezha2_send(L, speed < 0 ? 2 : 1, 96,	\
                      robot_byte(L, 2, abs(speed)), 245, 0);		\
                  })							\
    F(motor_position, { lua_events_before_wait();							\
                    uint8_t p[4];					\
                    nezha2_send(L, 0, 70, 0, 245, 0);			\
                    uBit.sleep(4);					\
                    robot_read(L, p, 4);				\
                    uint32_t v = p[0] | p[1] << 8 | p[2] << 16 |	\
                      (uint32_t)p[3] << 24;				\
                    lua_pushnumber(L, (v % 3600) * 0.1f);		\
                    return 1;						\
                  })							\
    F(motor_speed, { lua_events_before_wait();							\
                    uint8_t s[2];					\
                    nezha2_send(L, 0, 71, 0, 245, 0);			\
                    uBit.sleep(3);					\
                    robot_read(L, s, 2);				\
                    lua_pushnumber(L, (s[1] * 256 + s[0]) * 0.0926f);	\
                    return 1;						\
                  })


/*
 * A link over radio datagrams, in source/radio-link.c, over this
 * board's radio and clock.
 */

/* How long connect waits when nobody says otherwise */
#define RADIO_TIMEOUT  5000

static void radio_air_send(RadioLink *, const uint8_t *frame, int len)
{
    uBit.radio.datagram.send((uint8_t *)frame, len);
}

static int radio_air_recv(RadioLink *, uint8_t frame[RADIO_AIR_MAX])
{
    int n = uBit.radio.datagram.recv(frame, RADIO_AIR_MAX);
    return n < 0 ? -1 : n;
}

static uint32_t radio_air_now(RadioLink *)
{
    return (uint32_t)uBit.systemTime();
}

static void radio_air_pause(RadioLink *, uint32_t ms)
{
    uBit.sleep(ms);
}

static const RadioAir radio_air = {
    radio_air_send, radio_air_recv, radio_air_now, radio_air_pause
};

static RadioLink radio_link = {
    &radio_air, 0, 0, 0, false, 0, {0}, {0}, 0, NULL
};

/* A board's friendly name from a Lua argument. Names are five
 * letters, and the frames carry exactly five, so anything else
 * is a mistake in the call, said at once. */
static const char *radio_name(lua_State *L, int arg)
{
    size_t len;
    const char *name = luaL_checklstring(L, arg, &len);
    luaL_argcheck(L, len == RADIO_NAME, arg, "a board name is five letters");
    return name;
}

/* The same, where the name may be left out */
static const char *radio_opt_name(lua_State *L, int arg)
{
    return lua_isnoneornil(L, arg) ? NULL : radio_name(L, arg);
}

#define LUA_RADIO_FUNCTIONS						\
    F(setTransmitPower, {						\
                    int power = luaL_checkint(L, 1);			\
                    int r = uBit.radio.setTransmitPower(power);		\
                    lua_pushboolean(L, r == MICROBIT_OK);		\
                    return 1;						\
                  })							\
    F(setFrequencyBand, {						\
                    int band = luaL_checkint(L, 1);			\
                    int r = uBit.radio.setFrequencyBand(band);		\
                    lua_pushboolean(L, r == MICROBIT_OK);		\
                    return 1;						\
                  })							\
    F(enable,     { int r = uBit.radio.enable();			\
                    lua_pushboolean(L, r == MICROBIT_OK);		\
                    return 1;						\
                  })							\
    F(disable,    { int r = uBit.radio.disable();			\
                    lua_pushboolean(L, r == MICROBIT_OK);		\
                    return 1;						\
                  })							\
    F(setGroup,   {							\
                    uint8_t group = (uint8_t)luaL_checkint(L, 1);	\
                    int r = uBit.radio.setGroup(group);			\
                    lua_pushboolean(L, r == MICROBIT_OK);		\
                    return 1;						\
                  })							\
    F(dataReady,  { int r = uBit.radio.dataReady();			\
                    lua_pushinteger(L, r);					\
                    return 1;						\
                  })							\
    F(recv,       { /* copied out before the push, which can raise */	\
                    uint8_t packet[MICROBIT_RADIO_MAX_PACKET_SIZE];	\
                    int n = -1;						\
                    {							\
                      PacketBuffer r = uBit.radio.datagram.recv();	\
                      if (!(r == PacketBuffer::EmptyPacket)		\
                          && r.length() <= (int)sizeof packet) {	\
                        n = r.length();					\
                        memcpy(packet, r.getBytes(), n);		\
                      }							\
                    }							\
                    if (n < 0)						\
                      lua_pushnil(L);					\
                    else						\
                      lua_pushlstring(L, (const char *)packet, n);	\
                    return 1;						\
                  })							\
    F(send,       { size_t len;						\
                    const char *buffer = luaL_checklstring(L, 1, &len);	\
                    int r = uBit.radio.datagram.send(			\
                      PacketBuffer((uint8_t*)buffer, len));		\
                    lua_pushboolean(L, r == MICROBIT_OK);		\
                    return 1;						\
                  })							\
/* connect(friendlyName, timeout_ms) -> boolean */			\
    F(connect,    { lua_events_before_wait(); const char *them = radio_name(L, 1);		\
                    int timeout = luaL_optint(L, 2, RADIO_TIMEOUT);	\
                    uint8_t link = (uint8_t)(uBit.random(255) + 1);	\
                    uint16_t call = (uint16_t)uBit.random(0x10000);	\
                    lua_pushboolean(L, radio_link_call(&radio_link, them,	\
                      microbit_friendly_name(), link, call,		\
                      timeout > 0 ? (uint32_t)timeout : 0));		\
                    return 1;						\
                  })							\
/* listen([name]) -> friendlyName of whoever connected; with a
 * name, only that board is answered */				\
    F(listen,     { lua_events_before_wait(); const char *from = radio_opt_name(L, 1);		\
                    while (!radio_link_called(&radio_link,		\
                             microbit_friendly_name(), from))		\
                      uBit.sleep(1);					\
                    lua_pushstring(L, radio_link.peer);			\
                    return 1;						\
                  })							\
/* tx(message) -> boolean
 * The message goes piece by piece, each taken before the
 * next one leaves: true once the other board has them all,
 * which it may still have to run. */					\
    F(tx,         { lua_events_before_wait(); size_t len;						\
                    const char *msg = luaL_checklstring(L, 1, &len);	\
                    lua_pushboolean(L, radio_link_tx(&radio_link, msg, len));	\
                    return 1;						\
                  })							\
/* rx() -> string and whether it starts a message, or nil
 * One piece, the oldest the other board sent. Of a message
 * whose end was lost on the way, nothing more comes: the
 * next piece starts another. */					\
    F(rx,         { uint8_t body[RADIO_BODY];				\
                    int len;						\
                    bool starts;					\
                    if (!radio_link_rx(&radio_link, body, &len, &starts)) {	\
                      lua_pushnil(L);					\
                      return 1;						\
                    }							\
                    lua_pushlstring(L, (const char *)body, len);	\
                    lua_pushboolean(L, starts);				\
                    return 2;						\
                  })							\
/* answered([name]) -> friendlyName if somebody, or with a
 * name that board, has just made a new call, or nil; a call
 * repeated for the open link is none */		\
    F(answered,   { const char *from = radio_opt_name(L, 1);		\
                    if (radio_link_called(&radio_link,			\
                          microbit_friendly_name(), from)) {		\
                      lua_pushstring(L, radio_link.peer);		\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
/* notSent(line, typedAfter) -> string
 * What the REPL says when a line over the link did not go */	\
    F(notSent,    { return link_words_not_sent(L); })		\
/* typed(sofar, text) -> line, shown
 * Typing over the link, Backspace applied, and what it shows */	\
    F(typed,      { return link_words_typed(L); })

#define LUA_RADIO_COUNT 15

static const int digitalRJ[] = { 8, 12, 14, 16 };

#define LUA_PLANETX_FUNCTIONS						\
   F(getDigitalPin, { int port = luaL_checkint(L, 1);			\
                    luaL_argcheck(L, 1 <= port && port <= 4, 1,	\
                                  "RJ port 1 to 4");			\
                    int pin = digitalRJ[port - 1];			\
                    lua_pushlightuserdata(L, &uBit.io.pin[pin]);	\
                    return 1;						\
                  })							\
   F(trimpot,     { Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->getAnalogValue();			\
                    if(r >= 0 && r <= 1024) {				\
                      lua_pushnumber(L, (lua_Number)r * 9.765625e-4);	\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
   F(neopixel_send, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    size_t length;					\
                    const unsigned char *str =	(const unsigned char*)	\
                      luaL_checklstring(L, 2, &length);			\
                    codal::neopixel_send_buffer(*pin, str, length);	\
                    return 0;						\
                  })

#define LUA_PLANETX_COUNT 3

#define LUA_CODAL_CONSTANTS \
    C(MICROBIT_ID_LOGO) \
    C(DEVICE_ID_BUTTON_A) \
    C(DEVICE_ID_BUTTON_B) \
    C(DEVICE_ID_BUTTON_AB) \
    C(DEVICE_ID_SERIAL) \
    C(DEVICE_ID_ACCELEROMETER) \
    C(DEVICE_ID_COMPASS) \
    C(DEVICE_ID_GESTURE) \
    C(DEVICE_ID_RADIO) \
    C(DEVICE_ID_RADIO_DATA_READY) \
    C(ACCELEROMETER_EVT_DATA_UPDATE) \
    C(ACCELEROMETER_EVT_NONE) \
    C(ACCELEROMETER_EVT_TILT_UP) \
    C(ACCELEROMETER_EVT_TILT_DOWN) \
    C(ACCELEROMETER_EVT_TILT_LEFT) \
    C(ACCELEROMETER_EVT_TILT_RIGHT) \
    C(ACCELEROMETER_EVT_FACE_UP) \
    C(ACCELEROMETER_EVT_FACE_DOWN) \
    C(ACCELEROMETER_EVT_FREEFALL) \
    C(ACCELEROMETER_EVT_2G) \
    C(ACCELEROMETER_EVT_3G) \
    C(ACCELEROMETER_EVT_6G) \
    C(ACCELEROMETER_EVT_8G) \
    C(ACCELEROMETER_EVT_SHAKE) \
    C(COMPASS_EVT_DATA_UPDATE) \
    C(COMPASS_EVT_CONFIG_NEEDED) \
    C(COMPASS_EVT_CALIBRATE) \
    C(COMPASS_EVT_CALIBRATION_NEEDED) \
    C(DEVICE_BUTTON_EVT_DOWN) \
    C(DEVICE_BUTTON_EVT_UP) \
    C(DEVICE_BUTTON_EVT_CLICK) \
    C(DEVICE_BUTTON_EVT_LONG_CLICK) \
    C(DEVICE_BUTTON_EVT_HOLD) \
    C(DEVICE_BUTTON_EVT_DOUBLE_CLICK) \
    C(MICROBIT_RADIO_EVT_DATAGRAM) \
    C(CODAL_SERIAL_EVT_HEAD_MATCH)

#if CONFIG_ENABLED(DEVICE_BLE)
#define LUA_BLE_CONSTANTS \
    C(MICROBIT_ID_BLE) \
    C(MICROBIT_ID_BLE_UART) \
    C(MICROBIT_BLE_EVT_CONNECTED) \
    C(MICROBIT_BLE_EVT_DISCONNECTED) \
    C(MICROBIT_UART_S_EVT_DELIM_MATCH) \
    C(MICROBIT_UART_S_EVT_HEAD_MATCH) \
    C(MICROBIT_UART_S_EVT_RX_FULL)
#endif

#define F(name, body) static int l_##name(lua_State *L) body
LUA_MICROBIT_FUNCTIONS
LUA_DISPLAY_FUNCTIONS
LUA_ACCELEROMETER_FUNCTIONS
LUA_AUDIO_FUNCTIONS
LUA_IO_FUNCTIONS
LUA_SERIAL_FUNCTIONS
LUA_PLANETX_FUNCTIONS
#undef F

#if CONFIG_ENABLED(DEVICE_BLE)
#define F(name, body) static int l_ble_uart_##name(lua_State *L) body
LUA_BLE_FUNCTIONS
#undef F
#endif

#define F(name, body) static int l_compass_##name(lua_State *L) body
LUA_COMPASS_FUNCTIONS
#undef F

#define F(name, body) static int l_radio_##name(lua_State *L) body
LUA_RADIO_FUNCTIONS
#undef F

#define F(name, body) static int l_i2c_##name(lua_State *L) body
LUA_I2C_FUNCTIONS
#undef F

#define F(name, body) static int l_tpbot2_##name(lua_State *L) body
LUA_TPBOT2_FUNCTIONS
#undef F

#define F(name, body) static int l_nezha2_##name(lua_State *L) body
LUA_NEZHA2_FUNCTIONS
#undef F

#define F(name, body) {#name, l_##name, 0},
#define C(n)          {#n, NULL, (lua_Integer)(n)},
static const LuaApi l_microbit[] = {
    LUA_MICROBIT_FUNCTIONS
    LUA_CODAL_CONSTANTS
#if CONFIG_ENABLED(DEVICE_BLE)
    LUA_BLE_CONSTANTS
#endif
    {NULL, NULL, 0}
};
#undef C
static const LuaApi l_display[] = {
    LUA_DISPLAY_FUNCTIONS
    {NULL, NULL, 0}
};
static const LuaApi l_accelerometer[] = {
    LUA_ACCELEROMETER_FUNCTIONS
    {NULL, NULL, 0}
};
static const LuaApi l_audio[] = {
    LUA_AUDIO_FUNCTIONS
    {NULL, NULL, 0}
};
static const LuaApi l_io[] = {
    LUA_IO_FUNCTIONS
    {NULL, NULL, 0}
};
static const LuaApi l_serial[] = {
    LUA_SERIAL_FUNCTIONS
    {NULL, NULL, 0}
};
static const LuaApi l_planetx[] = {
    LUA_PLANETX_FUNCTIONS
    {NULL, NULL, 0}
};
#undef F

#if CONFIG_ENABLED(DEVICE_BLE)
#define F(name, body) {#name, l_ble_uart_##name, 0},
static const LuaApi l_ble_uart[] = {
    LUA_BLE_FUNCTIONS
    {NULL, NULL, 0}
};
#undef F
#endif

#define F(name, body) {#name, l_compass_##name, 0},
static const LuaApi l_compass[] = {
    LUA_COMPASS_FUNCTIONS
    {NULL, NULL, 0}
};
#undef F

#define F(name, body) {#name, l_radio_##name, 0},
static const LuaApi l_radio[] = {
    LUA_RADIO_FUNCTIONS
    {NULL, NULL, 0}
};
#undef F

#define F(name, body) {#name, l_i2c_##name, 0},
static const LuaApi l_i2c[] = {
    LUA_I2C_FUNCTIONS
    {NULL, NULL, 0}
};
#undef F

// The TPBot commands, from source/tpbot.c
#define X(name, function) {#name, function, 0},
static const LuaApi l_tpbot[] = {
    TPBOT_FUNCTIONS
    {NULL, NULL, 0}
};
#undef X

#define F(name, body) {#name, l_tpbot2_##name, 0},
static const LuaApi l_tpbot2[] = {
    LUA_TPBOT2_FUNCTIONS
    {"get_distance", tpbot_get_distance, 0},
    {NULL, NULL, 0}
};
#undef F

#define F(name, body) {#name, l_nezha2_##name, 0},
static const LuaApi l_nezha2[] = {
    LUA_NEZHA2_FUNCTIONS
    {"TURNS", NULL, 1},
    {"DEGREES", NULL, 2},
    {"SECONDS", NULL, 3},
    {"SHORTESTARC", NULL, 1},
    {"CLOCKWISE", NULL, 2},
    {"COUNTERCLOCKWISE", NULL, 3},
    {NULL, NULL, 0}
};
#undef F

// The modules require() makes (source/lua-modules.c). tpbot's first
// require also sets the robot commands that are globals (source/tpbot.c).
static const LuaModule lua_modules[] = {
  {"microbit",               l_microbit,      NULL},
  {"microbit.display",       l_display,       NULL},
  {"microbit.accelerometer", l_accelerometer, NULL},
  {"microbit.compass",       l_compass,       NULL},
  {"microbit.audio",         l_audio,         NULL},
  {"microbit.io",            l_io,            NULL},
  {"microbit.serial",        l_serial,        NULL},
  {"microbit.i2c",           l_i2c,           NULL},
  {"microbit.radio",         l_radio,         NULL},
#if CONFIG_ENABLED(DEVICE_BLE)
  {"microbit.ble.uart",      l_ble_uart,      NULL},
#endif
  {"planetx",                l_planetx,       NULL},
  {"tpbot",                  l_tpbot,         tpbot_register_globals},
  {"tpbot2",                 l_tpbot2,        NULL},
  {"nezha2",                 l_nezha2,        NULL},
  {NULL, NULL, NULL}
};

void register_lua_modules(lua_State *L) {
  lua_pushcfunction(L, push_text);
  push_text_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  lua_modules_open(L, lua_modules);
}

// The board's heap, for what the firmware makes when first needed
extern "C" void *board_alloc(size_t size) {
  return malloc(size);
}

extern "C" void board_free(void *p) {
  free(p);
}

// What source/lua-events.c needs of the board
extern "C" void lua_events_show_error(const char *message, bool wait) {
  if (wait)
    uBit.display.scroll(message);
  else
    uBit.display.scrollAsync(message);
}

// Whether the serial port is the USB one, the Compy's console: a program
// may redirect it to pins of its own, to drive another device
bool port_is_console = true;

void port_mistake(const char *what, const char *message) {
  static const char NEWLINE[] = "\r\n";
  uBit.serial.send((uint8_t *)NEWLINE, sizeof NEWLINE - 1, SYNC_SLEEP);
  uBit.serial.send((uint8_t *)what, strlen(what), SYNC_SLEEP);
  uBit.serial.send((uint8_t *)message, strlen(message), SYNC_SLEEP);
  uBit.serial.send((uint8_t *)NEWLINE, sizeof NEWLINE - 1, SYNC_SLEEP);
}

// A handler's mistake goes to the console, and never into the stream of a
// device a program has put on the port
extern "C" void lua_events_port_error(const char *what,
                                      const char *message) {
  if (port_is_console)
    port_mistake(what, message);
}

extern "C" bool lua_events_arm_port(void) {
  uBit.serial.eventAfter(1, ASYNC);
  return uBit.serial.isReadable() == 1;
}

extern "C" uint32_t lua_events_stack_used(void) {
  return stack_probe_current();
}

extern "C" uint32_t lua_events_stack_region(void) {
  return stack_probe_region();
}

extern "C" uint32_t lua_events_now(void) {
  return (uint32_t)uBit.systemTime();
}

extern "C" void lua_events_pause(uint32_t ms) {
  uBit.sleep(ms);
}

// Runs in a dedicated fiber (spawned by on_codal_event).  Has a full
// fiber stack so lua_pcall won't overflow the idle or interrupt stacks.
static void lua_event_handler_fiber(void *arg) {
  codal::Event *event = (codal::Event *)arg;
  LuaEvent e = { event->source, event->value, (uint32_t)event->timestamp };
  delete event;
  // the far end of a link is answered whatever Lua is doing
  if (e.source == DEVICE_ID_RADIO && e.value == MICROBIT_RADIO_EVT_DATAGRAM
      && !radio_link_heard(&radio_link))
    return;
  lua_event_arrived(e);
}

// Called by the MessageBus for every event matching DEVICE_ID_ANY /
// DEVICE_EVT_ANY.
//
// Registered with the IMMEDIATE flags, so events are dispatched straight
// from the posting context (fiber or IRQ). This is safe now that:
//  - the periodic housekeeping ticks are filtered out below, so dispatch
//    only runs for genuine user-relevant events (a few per second), and
//  - the heap allocations here (`new Event`, `new Fiber`) go through the
//    BASEPRI allocator critical section, so a heap search can no longer
//    mask the SoftDevice's priority-0 radio IRQs past their arming
//    deadline.
//
// The DEFAULT/QUEUE_IF_BUSY flags were tried instead (events dispatched
// via the MessageBus event queue in idle context), but serial head-match
// events then never reached the Lua handler, so IMMEDIATE is required.
//
// We also filter out the scheduler's internal channels: the periodic
// housekeeping ticks (DEVICE_SCHEDULER_EVT_TICK and
// DEVICE_COMPONENT_EVT_SYSTEM_TICK, both fired every
// SCHEDULER_TICK_PERIOD_US = 6ms), and the wait/notify channels
// (DEVICE_ID_NOTIFY / DEVICE_ID_NOTIFY_ONE, e.g. CODAL_SERIAL_EVT_TX_EMPTY
// fired from the UARTE IRQ for every transmitted char). These are pure
// internal noise — no Lua script ever handles them — but every one would
// otherwise spawn a Lua fiber (two heap allocations plus a lua_pcall).
// Filtering removes ~99% of the per-tick/per-char fiber traffic.
//
// The scheduler's tick is also the time to try again with the port's
// event, when it found no fiber while Lua was free: the port is armed for
// one event at a time, so nothing else would bring the REPL what was typed.
static void hand_on(codal::Event e) {
  codal::Event *copy = new codal::Event(e);
  if (create_fiber(lua_event_handler_fiber, copy) == NULL) {
    LuaEvent missed = { e.source, e.value, (uint32_t)e.timestamp };
    delete copy;
    // the port is armed for one event at a time: this one is not lost
    lua_events_missed(missed);
  }
}

static void on_codal_event(codal::Event e, void *arg) {
  (void)arg;
  if (e.source == DEVICE_ID_SCHEDULER && e.value == DEVICE_SCHEDULER_EVT_TICK
      && lua_events_take_stranded_port()) {
    hand_on(codal::Event(DEVICE_ID_SERIAL, CODAL_SERIAL_EVT_HEAD_MATCH,
                         codal::CREATE_ONLY));
    return;
  }
  if (e.source == DEVICE_ID_SCHEDULER || e.source == DEVICE_ID_COMPONENT ||
      e.source == DEVICE_ID_NOTIFY || e.source == DEVICE_ID_NOTIFY_ONE)
    return;
  if (lua_event_is_noise(e.source, e.value))
    return;
  hand_on(e);
}

// Events that only fill the waiting line: the port saying it has data
// after every character, or that it is full, and a scroll that has ended.
// Nothing in the firmware's script reads them.
static const LuaEventId lua_noise_events[] = {
  { DEVICE_ID_SERIAL, CODAL_SERIAL_EVT_DATA_RECEIVED },
  { DEVICE_ID_SERIAL, CODAL_SERIAL_EVT_RX_FULL },
  { DEVICE_ID_DISPLAY, DISPLAY_EVT_ANIMATION_COMPLETE },
};

// The port's event that the REPL waits for, and the noise
static const LuaEventsConfig lua_events_config = {
  { DEVICE_ID_SERIAL, CODAL_SERIAL_EVT_HEAD_MATCH },
  lua_noise_events,
  sizeof lua_noise_events / sizeof lua_noise_events[0]
};

void register_lua_event_listener(lua_State *L) {
  lua_events_open(L, &lua_events_config);
  uBit.messageBus.listen(DEVICE_ID_ANY, DEVICE_EVT_ANY,
                         on_codal_event, NULL,
                         MESSAGE_BUS_LISTENER_IMMEDIATE);
}
