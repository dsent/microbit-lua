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

extern MicroBit uBit;

// Bind the Lua i2c API to CODAL's pre-wired EXTERNAL edge-connector bus
// (MICROBIT_PIN_EXT_SDA=P1_00 / MICROBIT_PIN_EXT_SCL=P0_26), where the TPBot
// lives.
I2C &i2c = uBit.i2c;

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
    F(stackUsage, { lua_pushinteger(L, (lua_Integer)stack_probe_peak());	\
                    return 1;						\
                  })							\
    F(stackReset, { stack_probe_paint();				\
                    return 0;						\
                  })							\
    F(panic,      { int statusCode = (int)luaL_checkinteger(L, 1);      \
                    microbit_panic(statusCode);				\
                    return 0;						\
                  })

// create a Lua image table from C ImageData and place it on the stack
void lua_createimage(lua_State *L, ImageData *ptr) {
  size_t size = ptr->width * ptr->height;
  lua_createtable(L, 0, 3);
  lua_pushliteral(L, "width");
  lua_pushinteger(L, ptr->width);
  lua_settable(L, -3);
  lua_pushliteral(L, "height");
  lua_pushinteger(L, ptr->height);
  lua_settable(L, -3);
  lua_pushliteral(L, "data");
  lua_pushlstring(L, (const char *)(ptr->data), size);
  lua_settable(L, -3);
}

// check and return a Lua function argument as a C++ Image object. data is
// a string, a byte a pixel row by row, or a list of numbers, one a pixel;
// pixels it does not reach stay off. Every Lua call that can raise an
// error comes before the Image is made: an error jumps past the Image's
// destructor, and what it holds on the heap is never given back. So a
// binding reads its other arguments first, too.
#define IMAGE_PIXELS_MAX 4096
Image luaL_checkimage(lua_State *L, int narg) {
  int width, height, kind;
  luaL_checktype(L, narg, LUA_TTABLE);
  lua_getfield(L, narg, "width");
  width = (int)lua_tointeger(L, -1);
  lua_getfield(L, narg, "height");
  height = (int)lua_tointeger(L, -1);
  lua_getfield(L, narg, "data");
  kind = lua_type(L, -1);
  // a bigger image than the heap can give stops the board with panic 020
  luaL_argcheck(L, width * height <= IMAGE_PIXELS_MAX, narg,
                "an image has at most 4096 pixels, width times height");
  luaL_argcheck(L, kind == LUA_TSTRING || kind == LUA_TTABLE, narg,
                "an image's data is a string, or a list of numbers, one "
                "for each pixel");
  Image r(width, height);           // empty when a size is below 0
  uint8_t *pixels = r.getBitmap();
  size_t count = (size_t)r.getWidth() * r.getHeight();
  if (kind == LUA_TSTRING) {
    size_t size;
    const char *data = lua_tolstring(L, -1, &size);
    memcpy(pixels, data, size < count ? size : count);
  } else {
    for (size_t i = 0; i < count; i++) {
      lua_rawgeti(L, -1, (int)i + 1);
      pixels[i] = (uint8_t)lua_tointeger(L, -1);
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 3);
  return r;
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
    F(screenShot, { ImageData *ptr =					\
                      uBit.display.screenShot().leakData();		\
                    lua_createimage(L, ptr);				\
                    ptr->decr();					\
                    return 1;						\
                  })							\
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
    F(readLightLevel, { int r = uBit.display.readLightLevel();		\
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
    F(print,      { const char *s = luaL_checkstring(L, 1);		\
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
    F(scroll,     { const char *s = luaL_checkstring(L, 1);		\
                    int delay = luaL_optint(L, 2,			\
                      DISPLAY_DEFAULT_SCROLL_SPEED);			\
                    int r = uBit.display.scroll(s, delay);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(animateAsync, { int delay = luaL_checkint(L, 2);			\
                    int stride = luaL_checkint(L, 3);			\
                    int startingPosition =				\
                      luaL_optint(L, 4, DISPLAY_ANIMATE_DEFAULT_POS);	\
                    int autoClear =					\
                      luaL_optint(L, 5, DISPLAY_DEFAULT_AUTOCLEAR);	\
                    Image image = luaL_checkimage(L, 1);		\
                    int r = uBit.display.animateAsync(image,		\
                                                      delay,		\
                                                      stride,		\
                                                      startingPosition,	\
                                                      autoClear);	\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(animate,    { int delay = luaL_checkint(L, 2);			\
                    int stride = luaL_checkint(L, 3);			\
                    int startingPosition =				\
                      luaL_optint(L, 4, DISPLAY_ANIMATE_DEFAULT_POS);	\
                    int autoClear =					\
                      luaL_optint(L, 5, DISPLAY_DEFAULT_AUTOCLEAR);	\
                    Image image = luaL_checkimage(L, 1);		\
                    int r = uBit.display.animate(image,			\
                                                 delay,			\
                                                 stride,		\
                                                 startingPosition,	\
                                                 autoClear);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(show,       { int x = luaL_optint(L, 2, 0);			\
                    int y = luaL_optint(L, 3, 0);			\
                    int alpha = luaL_optint(L, 4, 0);			\
                    int delay = luaL_optint(L, 5, 0);			\
                    Image image = luaL_checkimage(L, 1);		\
                    int r = uBit.display.print(				\
                      image, x, y, alpha, delay);			\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(showAsync,  { int x = luaL_optint(L, 2, 0);			\
                    int y = luaL_optint(L, 3, 0);			\
                    int alpha = luaL_optint(L, 4, 0);			\
                    int delay = luaL_optint(L, 5,			\
                      DISPLAY_DEFAULT_PRINT_SPEED);			\
                    Image image = luaL_checkimage(L, 1);		\
                    int r = uBit.display.printAsync(			\
                      image, x, y, alpha, delay);			\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(pan,        { int delay = luaL_optint(L, 2,			\
                      DISPLAY_DEFAULT_SCROLL_SPEED);			\
                    int stride = luaL_optint(L, 3,			\
                      DISPLAY_DEFAULT_SCROLL_STRIDE);			\
                    Image image = luaL_checkimage(L, 1);		\
                    int r = uBit.display.scroll(image, delay, stride);	\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(panAsync,   { int delay = luaL_optint(L, 2,			\
                      DISPLAY_DEFAULT_SCROLL_SPEED);			\
                    int stride = luaL_optint(L, 3,			\
                      DISPLAY_DEFAULT_SCROLL_STRIDE);			\
                    Image image = luaL_checkimage(L, 1);		\
                    int r = uBit.display.scrollAsync(image, delay,	\
                              stride);					\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(rotateRight,{ uBit.display.rotateTo(				\
                      MICROBIT_DISPLAY_ROTATION_90);			\
                    return 0;						\
                  })							\
    F(rotateLeft, { uBit.display.rotateTo(				\
                      MICROBIT_DISPLAY_ROTATION_270);			\
                    return 0;						\
                  })							\
    F(rotateUp,   { uBit.display.rotateTo(				\
                      MICROBIT_DISPLAY_ROTATION_0);			\
                    return 0;						\
                  })							\
    F(rotateDown, { uBit.display.rotateTo(				\
                      MICROBIT_DISPLAY_ROTATION_180);			\
                    return 0;						\
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
    F(getPulseUs, { Pin *pin = luaL_checkPin(L, 1);			\
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
                    luaL_argcheck(L, 0 <= pin && pin < uBit.io.pins, 1,	\
                                  "from 0 to 32");			\
                    lua_pushlightuserdata(L, &uBit.io.pin[pin]);	\
                    return 1;						\
                  })

void lua_pushManagedString(lua_State *L, ManagedString s) {
  lua_pushlstring(L, s.toCharArray(), s.length());
}

ManagedString luaL_checkManagedString(lua_State *L, int narg) {
  size_t length;
  const char *str = luaL_checklstring(L, narg, &length);
  return ManagedString(str, length);
}

#define LUA_SERIAL_FUNCTIONS						\
    F(send,       { ManagedString s = luaL_checkManagedString(L, 1);	\
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
    F(getByte,    { int r = uBit.serial.getChar(SYNC_SLEEP);		\
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
    F(getChar,    { char r = (char)uBit.serial.getChar(SYNC_SLEEP);	\
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
    F(read,       { int size = luaL_checkint(L, 1);			\
                    luaL_argcheck(L, 0 <= size && size <= 256, 1,	\
                                  "0 to 256 bytes");			\
                    lua_pushManagedString(L,				\
                      uBit.serial.read(size, SYNC_SLEEP));		\
                    return 1;						\
                  })							\
    F(readAsync,  { int size = luaL_checkint(L, 1);			\
                    luaL_argcheck(L, 0 <= size && size <= 256, 1,	\
                                  "0 to 256 bytes");			\
                    lua_pushManagedString(L,				\
                      uBit.serial.read(size, ASYNC));			\
                    return 1;						\
                  })							\
    F(readUntil,  { ManagedString delimiters =				\
                      luaL_checkManagedString(L, 1);			\
                    lua_pushManagedString(L,				\
                      uBit.serial.readUntil(delimiters, SYNC_SLEEP));	\
                    return 1;						\
                  })							\
    F(setBaud,    { int baudrate = luaL_checkint(L, 1);			\
                    int r = uBit.serial.setBaud(baudrate);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(redirect,   { Pin *tx = luaL_checkPin(L, 1);			\
                    Pin *rx = luaL_checkPin(L, 2);			\
                    int r = uBit.serial.redirect(*tx, *rx);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    F(eventAfter, { uBit.serial.eventAfter(luaL_checkint(L, 1),		\
                                           SYNC_SLEEP);			\
                    return 0;						\
                  })							\
    F(eventAfterAsync, {						\
                    uBit.serial.eventAfter(luaL_checkint(L, 1),		\
                                           ASYNC);			\
                    return 0;						\
                  })							\
    F(eventOn,    { uBit.serial.eventOn(luaL_checkManagedString(L, 1),	\
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
                    int size = luaL_checkint(L, 1);			\
                    luaL_argcheck(L, 0 <= size && size <= 256, 1,	\
                                  "0 to 256 bytes");			\
                    lua_pushManagedString(L,				\
                      uart->read(size, SYNC_SLEEP));			\
                    return 1;						\
                  })							\
    F(readAsync,  { if(!uart) { lua_pushnil(L); return 1; }		\
                    int size = luaL_checkint(L, 1);			\
                    luaL_argcheck(L, 0 <= size && size <= 256, 1,	\
                                  "0 to 256 bytes");			\
                    lua_pushManagedString(L,				\
                      uart->read(size, ASYNC));				\
                    return 1;						\
                  })							\
    F(readUntil,  { if(!uart) { lua_pushnil(L); return 1; }		\
                    ManagedString delimiters =				\
                      luaL_checkManagedString(L, 1);			\
                    lua_pushManagedString(L,				\
                      uart->readUntil(delimiters, SYNC_SLEEP));		\
                    return 1;						\
                  })							\
    F(eventOn,    { if(!uart) { return 0; }				\
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
                    luaL_argcheck(L, 0 <= length && length <= 256, 2,	\
                                  "0 to 256 bytes");			\
                    char data[length];					\
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
  if (i2c.write(32, data, length) != MICROBIT_OK)
    return luaL_error(L, "%s", tpbot_no_answer);
  return 0;
}

static void robot_read(lua_State *L, uint8_t *data, int length) {
  if (i2c.read(32, data, length) != MICROBIT_OK)
    luaL_error(L, "%s", tpbot_no_answer);
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

// The bus hardware reads the frame by DMA, which cannot read flash: a
// frame there, a string literal, would go out as whatever the DMA read
// while the write reported success. That is how a TPBot Classic's stop
// once left a wheel turning. So each frame is copied to the stack first.
extern "C" int tpbot_i2c_write(int address, const char *data, size_t length) {
  char frame[TPBOT_FRAME_MAX];
  if (length > sizeof frame)
    return -1;
  memcpy(frame, data, length);
  return i2c.write(address, frame, length) == MICROBIT_OK ? 0 : -1;
}

extern "C" int tpbot_i2c_read(int address, char *data, size_t length) {
  return i2c.read(address, (uint8_t *)data, length) == MICROBIT_OK ? 0 : -1;
}

extern "C" void tpbot_sleep(uint32_t ms) {
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

int32_t nezha2_motor_travel(lua_State *L) {
  uint8_t p[4];
  nezha2_send(L, 0, 70, 0, 245, 0);
  uBit.sleep(4);
  robot_read(L, p, 4);
  return (int32_t) p[0] |
         (int32_t) p[1] << 8 |
         (int32_t) p[2] << 16 |
         (int32_t) p[3] << 24;
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
    F(motor_travel, {							\
                    lua_pushnumber(L,					\
                      0.1 * (lua_Number)nezha2_motor_travel(L));	\
                    return 1;						\
                  })							\
    F(motor_position, {							\
                    int p = nezha2_motor_travel(L) % 3600;		\
                    lua_pushnumber(L,					\
                      0.1 * (lua_Number)(p < 0 ? p + 3600 : p));	\
                    return 1;						\
                  })							\
    F(motor_speed, {							\
                    uint8_t s[2];					\
                    nezha2_send(L, 0, 71, 0, 245, 0);			\
                    uBit.sleep(3);					\
                    robot_read(L, s, 2);				\
                    lua_pushnumber(L, (s[1] * 256 + s[0]) * 0.0926f);	\
                    return 1;						\
                  })


/*
 * A link over radio datagrams.
 *
 * Datagrams are 32 bytes and anyone on the group hears them
 * all, so a frame says what it is and which link it belongs
 * to. The link number is drawn by the side that calls
 * connect; two pairs on one group draw different numbers and
 * ignore each other's traffic. A name is always the five
 * letters of a micro:bit's friendly name, so HELLO carries
 * both ends' names in a fixed ten bytes.
 */

#define RADIO_HEAD     3
#define RADIO_NAME     5
/* The radio is documented to carry 32 bytes and the driver
 * reports as many sent, but past 29 the tail arrives zeroed:
 * measured between two boards, 28 and 29 come through whole,
 * 30 loses its last byte and 31 and 32 lose two. So a frame
 * is 29, and the driver's own limit is never reached. */
#define RADIO_FRAME    29
#define RADIO_BODY     (RADIO_FRAME - RADIO_HEAD)

/* Kinds of frame. Other software on the same group starts its
 * packets with small numbers too (MakeCode's packet types run
 * from 0 up), so these sit where nothing else puts a first
 * byte, and a stray packet is not read as one of ours. */
#define RADIO_HELLO    0xA1
#define RADIO_WELCOME  0xA2
#define RADIO_DATA     0xA3
#define RADIO_ACK      0xA4
#define RADIO_BYE      0xA5
#define RADIO_BEACON   0xA6

#define RADIO_TRIES    8
#define RADIO_WAIT     30

/* How long connect waits when nobody says otherwise */
#define RADIO_TIMEOUT  5000

/* A listening board sends a beacon every one to two seconds,
 * the gap drawn afresh each time, so that two boards do not
 * keep sending together. A calling board that hears none for
 * three of the longest gaps takes the link for lost; while it
 * calls, a radio event comes this often, so that it notices
 * the silence even when nothing else arrives. */
#define RADIO_BEACON_GAP 1000
#define RADIO_SILENCE  (3 * 2 * RADIO_BEACON_GAP)
#define RADIO_TICK     500
#define RADIO_EVT_TICK (MICROBIT_RADIO_EVT_DATAGRAM + 1)

static uint8_t radio_link = 0;
static char radio_peer[RADIO_NAME + 1];
static uint8_t radio_out = 0;   /* number of the last sent */
static uint8_t radio_in = 0;    /* number of the last taken */
static bool radio_calling = false;      /* this end called */
static uint64_t radio_heard = 0;        /* the far end's last beacon */
static uint64_t radio_beacon_due = 0;

/* kind, link, number, then body */
static int radio_put(uint8_t kind, uint8_t link, uint8_t num,
                     const char *body, int len)
{
    uint8_t f[RADIO_FRAME];
    if (len > RADIO_BODY) len = RADIO_BODY;
    f[0] = kind;
    f[1] = link;
    f[2] = num;
    if (len > 0) memcpy(f + RADIO_HEAD, body, len);
    return uBit.radio.datagram.send(
        PacketBuffer(f, len + RADIO_HEAD));
}

/* A frame taken off the air that the caller did not want.
 * Reading a datagram removes it, so one asked for DATA while
 * an ACK is due would throw the ACK away and leave the
 * sender waiting out its tries. It waits here instead, until
 * somebody wants it or another unwanted one takes its
 * place. */
static uint8_t radio_held[RADIO_FRAME];
static int radio_held_len = 0;

/* What the far end sends while a tx here waits for its answer.
 * rx cannot take it then, since events wait for the running
 * call, and the far end's own tx, an echo for one, would wait
 * for this end's answer until one of the two gave up. So tx
 * answers it and keeps it for rx: up to ten pieces, the echo
 * of the longest read the port holds. A piece that finds no
 * room is left unanswered and comes again. */
static uint8_t radio_early[10 * RADIO_BODY];
static int radio_early_len = 0;

/* Is this frame the one being waited for, and if so, what
 * does it carry? Anything longer than a frame is not ours. */
static bool radio_wanted(uint8_t *b, int n, uint8_t kind,
                         uint8_t link, uint8_t *from,
                         uint8_t *num, uint8_t *body,
                         int *len)
{
    if (n < RADIO_HEAD || n > RADIO_FRAME) return false;
    if (b[0] != kind) return false;
    if (link != 0 && b[1] != link) return false;
    if (from) *from = b[1];
    if (num) *num = b[2];
    if (body) memcpy(body, b + RADIO_HEAD, n - RADIO_HEAD);
    if (len) *len = n - RADIO_HEAD;
    return true;
}

/* The next frame of this kind for this link, or nothing */
static bool radio_take(uint8_t kind, uint8_t link,
                       uint8_t *from, uint8_t *num,
                       uint8_t *body, int *len)
{
    if (radio_held_len > 0
        && radio_wanted(radio_held, radio_held_len, kind,
                        link, from, num, body, len)) {
        radio_held_len = 0;
        return true;
    }
    PacketBuffer p = uBit.radio.datagram.recv();
    if (p == PacketBuffer::EmptyPacket) return false;
    int n = p.length();
    if (radio_wanted(p.getBytes(), n, kind, link, from,
                     num, body, len)) return true;
    /* Beacons come every second or two and are never held; a
     * calling board notes the one from the board it called, as
     * long as that board still waits for this one */
    if (n > 0 && p.getBytes()[0] == RADIO_BEACON) {
        const uint8_t *b = p.getBytes() + RADIO_HEAD;
        if (radio_calling && n == RADIO_HEAD + 2 * RADIO_NAME
            && memcmp(b, radio_peer, RADIO_NAME) == 0
            && memcmp(b + RADIO_NAME, microbit_friendly_name(),
                      RADIO_NAME) == 0)
            radio_heard = uBit.systemTime();
        return false;
    }
    if (n > 0 && n <= (int)sizeof(radio_held)) {
        memcpy(radio_held, p.getBytes(), n);
        radio_held_len = n;
    }
    return false;
}

/* The link is over at this end, and the ticks a call brings
 * stop with it */
static void radio_drop()
{
    system_timer_cancel_event(DEVICE_ID_RADIO, RADIO_EVT_TICK);
    radio_link = 0;
    radio_calling = false;
}

/* Both ends start a link the same way, from none */
static void radio_open(uint8_t link, const char *peer)
{
    radio_drop();
    radio_link = link;
    radio_held_len = 0;
    radio_early_len = 0;
    radio_out = 0;
    radio_in = 0;
    memcpy(radio_peer, peer, RADIO_NAME);
    radio_peer[RADIO_NAME] = 0;
}

/* The answer to a HELLO, if this is it. A WELCOME names both
 * ends, the one it goes to and the one it comes from, so a
 * frame that only happens to share a kind and a link number
 * with it, from some other board on the air, is not taken for
 * one. hello is the call: the board called, then this one. */
static bool radio_welcomed(uint8_t link, const char *hello)
{
    uint8_t body[RADIO_BODY];
    int len;

    if (!radio_take(RADIO_WELCOME, link, NULL, NULL, body, &len))
        return false;
    return len == RADIO_NAME * 2
        && memcmp(body, hello + RADIO_NAME, RADIO_NAME) == 0
        && memcmp(body + RADIO_NAME, hello, RADIO_NAME) == 0;
}

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

/* Call once, then wait a moment for the answer */
static bool radio_called(uint8_t link, const char *hello)
{
    uint64_t until;
    radio_put(RADIO_HELLO, link, 0, hello, RADIO_NAME * 2);
    until = uBit.systemTime() + RADIO_WAIT;
    while (uBit.systemTime() < until) {
        if (radio_welcomed(link, hello)) return true;
        uBit.sleep(1);
    }
    return false;
}

/* A HELLO addressed to this board, from the board it waits
 * for, or from any if it waits for none: answer it and take
 * the link it names. A call from anyone else is left
 * unanswered, so the caller does not think it got through.
 * Any link the call replaces is dropped: the other end has
 * started over, which is how a reset board finds its way
 * back. */
static bool radio_called_us(const char *from)
{
    const char *us = microbit_friendly_name();
    uint8_t body[RADIO_BODY];
    char welcome[RADIO_NAME * 2];
    uint8_t link;
    int len;

    if (!radio_take(RADIO_HELLO, 0, &link, NULL, body, &len)
        || len != RADIO_NAME * 2
        || memcmp(body, us, RADIO_NAME) != 0) return false;
    if (from && memcmp(body + RADIO_NAME, from, RADIO_NAME) != 0)
        return false;
    radio_open(link, (char *)body + RADIO_NAME);
    memcpy(welcome, body + RADIO_NAME, RADIO_NAME);
    memcpy(welcome + RADIO_NAME, us, RADIO_NAME);
    radio_put(RADIO_WELCOME, link, 0, welcome, RADIO_NAME * 2);
    return true;
}

/* A DATA frame of this link, answered: its length, or -1 when
 * none has come, or when it came before, since the far end did
 * not hear the first answer; then it is answered again and
 * dropped. */
static int radio_data(uint8_t *body)
{
    uint8_t num;
    int len;

    if (!radio_take(RADIO_DATA, radio_link, NULL, &num, body, &len))
        return -1;
    radio_put(RADIO_ACK, radio_link, num, NULL, 0);
    if (num == radio_in) return -1;
    radio_in = num;
    return len;
}

/* One frame, repeated until the far end answers it; what the
 * far end sends meanwhile is answered and kept */
static bool radio_one(uint8_t kind, const char *body, int len)
{
    radio_out++;
    for (int n = 0; n < RADIO_TRIES; n++) {
        uint64_t until;
        radio_put(kind, radio_link, radio_out, body, len);
        until = uBit.systemTime() + RADIO_WAIT;
        while (uBit.systemTime() < until) {
            uint8_t num;
            int got;
            if (radio_take(RADIO_ACK, radio_link, NULL,
                           &num, NULL, NULL)
                && num == radio_out) return true;
            if (radio_early_len + RADIO_BODY <= (int)sizeof(radio_early)
                && (got = radio_data(radio_early + radio_early_len)) > 0)
                radio_early_len += got;
            uBit.sleep(1);
        }
    }
    return false;
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
    F(recv,       { PacketBuffer r = uBit.radio.datagram.recv();	\
                    if(r == PacketBuffer::EmptyPacket) {		\
                      lua_pushnil(L);					\
                    } else {						\
                      lua_pushlstring(L,				\
                        (const char*)r.getBytes(), r.length());		\
                    }						 	\
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
    F(connect,    { const char *them = radio_name(L, 1);		\
                    int timeout = luaL_optint(L, 2, RADIO_TIMEOUT);	\
                    char hello[RADIO_NAME * 2];				\
                    uint8_t link = (uint8_t)(uBit.random(255) + 1);	\
                    uint64_t deadline = uBit.systemTime() + timeout;	\
                    memcpy(hello, them, RADIO_NAME);			\
                    memcpy(hello + RADIO_NAME,				\
                      microbit_friendly_name(), RADIO_NAME);		\
                    while (uBit.systemTime() < deadline) {		\
                      if (radio_called(link, hello)) {			\
                        radio_open(link, them);				\
                        radio_calling = true;				\
                        radio_heard = uBit.systemTime();		\
                        system_timer_event_every(RADIO_TICK,		\
                          DEVICE_ID_RADIO, RADIO_EVT_TICK);		\
                        lua_pushboolean(L, 1);				\
                        return 1;					\
                      }							\
                    }							\
                    lua_pushboolean(L, 0);				\
                    return 1;						\
                  })							\
/* tx(message) -> boolean
 * The message goes piece by piece, each taken before the
 * next one leaves. */							\
    F(tx,         { size_t len;						\
                    const char *msg = luaL_checklstring(L, 1, &len);	\
                    size_t sent = 0;					\
                    if (radio_link == 0) {				\
                      lua_pushboolean(L, 0);				\
                      return 1;						\
                    }							\
                    do {						\
                      int piece = (int)(len - sent);			\
                      if (piece > RADIO_BODY) piece = RADIO_BODY;	\
                      if (!radio_one(RADIO_DATA, msg + sent, piece)) {		\
                        lua_pushboolean(L, 0);				\
                        return 1;					\
                      }							\
                      sent += piece;					\
                    } while (sent < len);				\
                    lua_pushboolean(L, 1);				\
                    return 1;						\
                  })							\
/* rx() -> string, nil, or false once the link is over: the
 * far end has closed it, or, at the calling end, has sent no
 * beacon for too long. All a tx took while it waited, or one
 * piece, acknowledged. */						\
    F(rx,         { uint8_t body[RADIO_BODY];				\
                    uint8_t num;					\
                    int len;						\
                    if (radio_link == 0) {				\
                      lua_pushnil(L);					\
                      return 1;						\
                    }							\
                    if (radio_early_len > 0) {				\
                      lua_pushlstring(L, (const char *)radio_early,	\
                                      radio_early_len);		\
                      radio_early_len = 0;				\
                      return 1;						\
                    }							\
                    bool bye = radio_take(RADIO_BYE, radio_link, NULL,	\
                                          &num, NULL, NULL);		\
                    if (bye) {						\
                      radio_put(RADIO_ACK, radio_link, num, NULL, 0);	\
                    }							\
                    if (bye || (radio_calling && uBit.systemTime()	\
                                - radio_heard > RADIO_SILENCE)) {	\
                      radio_drop();					\
                      lua_pushboolean(L, 0);				\
                      return 1;						\
                    }							\
                    len = radio_data(body);				\
                    if (len < 0) {					\
                      lua_pushnil(L);					\
                      return 1;						\
                    }							\
                    lua_pushlstring(L, (const char *)body, len);	\
                    return 1;						\
                  })							\
/* close() -- the far end is told that the link is over, and
 * this end drops it */							\
    F(close,      { if (radio_link != 0) {				\
                      radio_one(RADIO_BYE, NULL, 0);			\
                      radio_drop();					\
                    }							\
                    return 0;						\
                  })							\
/* beacon([name]) -- once one is due, a listening board says
 * who it is and, with a name, which board it waits for */		\
    F(beacon,     { const char *awaited = radio_opt_name(L, 1);		\
                    char b[RADIO_NAME * 2];				\
                    if (uBit.systemTime() < radio_beacon_due) return 0;	\
                    memcpy(b, microbit_friendly_name(), RADIO_NAME);	\
                    if (awaited) memcpy(b + RADIO_NAME, awaited, RADIO_NAME); \
                    radio_put(RADIO_BEACON, 0, 0, b,			\
                      awaited ? RADIO_NAME * 2 : RADIO_NAME);		\
                    radio_beacon_due = uBit.systemTime()		\
                      + RADIO_BEACON_GAP + uBit.random(RADIO_BEACON_GAP); \
                    return 0;						\
                  })							\
/* scan() -> {friendlyName = boolean}: the boards whose
 * beacons are heard within three of the longest gaps, each
 * true if it waits for this board, false if for another */		\
    F(scan,       { const char *us = microbit_friendly_name();		\
                    uint8_t body[RADIO_BODY];				\
                    int len;						\
                    uint64_t until = uBit.systemTime() + RADIO_SILENCE;	\
                    uBit.radio.enable();				\
                    /* Whatever was queued before the scan is old: those
                     * boards may have stopped listening since */	\
                    while (!(uBit.radio.datagram.recv()			\
                             == PacketBuffer::EmptyPacket)) {		\
                    }							\
                    lua_newtable(L);					\
                    while (uBit.systemTime() < until) {			\
                      if (!radio_take(RADIO_BEACON, 0, NULL, NULL, body, \
                                      &len) || len < RADIO_NAME) {	\
                        uBit.sleep(1);					\
                        continue;					\
                      }							\
                      lua_pushlstring(L, (const char *)body, RADIO_NAME); \
                      lua_pushboolean(L, len < RADIO_NAME * 2		\
                        || memcmp(body + RADIO_NAME, us, RADIO_NAME) == 0); \
                      lua_rawset(L, -3);				\
                    }							\
                    return 1;						\
                  })							\
/* answered([name]) -> friendlyName if somebody, or with a
 * name that board, has just called again, or nil */		\
    F(answered,   { const char *from = radio_opt_name(L, 1);		\
                    if (radio_called_us(from)) {			\
                      lua_pushstring(L, radio_peer);			\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })

#define LUA_RADIO_COUNT 15

// PlanetX gesture sensor EF05020: a PAJ7620 at I2C address 0x73.
#define PAJ7620 0xE6

// The chip's settings for bank 0 and then bank 1, in runs of registers
// that follow each other: the number of bytes in a run, its first
// register, then the values for that register and the ones after it, as
// the chip moves on to the next by itself. The run at 0xEE ends on 0xEF,
// the bank register, and so switches to bank 1; the last run switches
// back. The settings are those Elecfreaks send.
static const uint8_t paj7620_setup[] = {
  10, 0x32, 0x29, 0x01, 0x00, 0x01, 0x00, 0x07, 0x17, 0x06, 0x12,
  5, 0x3F, 0x00, 0x02, 0xFF, 0x01,
  14, 0x46, 0x2D, 0x0F, 0x3C, 0x00, 0x1E, 0x00, 0x20, 0x00, 0x1A, 0x14,
  0x00, 0x10, 0x00,
  16, 0x5C, 0x02, 0x00, 0x10, 0x3F, 0x27, 0x28, 0x00, 0x03, 0xF7, 0x03,
  0xD9, 0x03, 0x01, 0xC8, 0x40,
  10, 0x6D, 0x04, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0xF0, 0x00,
  45, 0x80, 0x42, 0x44, 0x04, 0x20, 0x20, 0x00, 0x10, 0x00, 0x05, 0x18,
  0x10, 0x01, 0x37, 0x00, 0xF0, 0x81, 0x06, 0x06, 0x1E, 0x0D, 0x0A, 0x0A,
  0x0C, 0x05, 0x0A, 0x41, 0x14, 0x0A, 0x3F, 0x33, 0xAE, 0xF9, 0x48, 0x13,
  0x10, 0x08, 0x30, 0x19, 0x10, 0x08, 0x24, 0x04, 0x1E, 0x1E,
  8, 0xCC, 0x19, 0x0B, 0x13, 0x64, 0x21, 0x0F, 0x88,
  11, 0xE0, 0x01, 0x04, 0x41, 0xD6, 0x00, 0x0C, 0x0A, 0x00, 0x00, 0x00,
  3, 0xEE, 0x07, 0x01,
  32, 0x00, 0x1E, 0x1E, 0x0F, 0x10, 0x02, 0x00, 0xB0, 0x04, 0x0D, 0x0E,
  0x9C, 0x04, 0x05, 0x0F, 0x02, 0x12, 0x02, 0x02, 0x00, 0x01, 0x05, 0x07,
  0x05, 0x07, 0x01, 0x04, 0x05, 0x0C, 0x2A, 0x01, 0x00,
  4, 0x21, 0x00, 0x00, 0x00,
  6, 0x25, 0x01, 0x00, 0x39, 0x7F, 0x08,
  12, 0x30, 0x03, 0x00, 0x1A, 0x1A, 0x07, 0x07, 0x01, 0xFF, 0x36, 0x07,
  0x00,
  12, 0x3E, 0xFF, 0x00, 0x77, 0x40, 0x00, 0x30, 0xA0, 0x5C, 0x00, 0x00,
  0x58,
  12, 0x4A, 0x1E, 0x1E, 0x00, 0x00, 0xA0, 0x80, 0x00, 0x00, 0x00, 0x00,
  0x00,
  2, 0x57, 0x80,
  10, 0x59, 0x10, 0x08, 0x94, 0xE8, 0x08, 0x3D, 0x99, 0x45, 0x40,
  14, 0x63, 0x2D, 0x02, 0x96, 0x00, 0x97, 0x01, 0xCD, 0x01, 0xB0, 0x04,
  0x2C, 0x01, 0x32,
  8, 0x71, 0x00, 0x01, 0x35, 0x00, 0x33, 0x31, 0x01,
  4, 0x7C, 0x84, 0x03, 0x01,
  2, 0xEF, 0x00,
  0
};

// Asleep, the chip leaves the first access unanswered, wakes on it and
// takes settings 400 us later. The I2C hardware takes what it sends by
// DMA, which reaches RAM only, so the settings go over from a copy.
// Gesture flags raised before the setup mean nothing and are read away.
static bool paj7620_init() {
  uint8_t bank0[] = { 0xEF, 0x00 }, flags[2], setup[sizeof paj7620_setup];
  i2c.write(PAJ7620, bank0, 2);
  target_wait_us(400);
  memcpy(setup, paj7620_setup, sizeof setup);
  for (uint8_t *run = setup; *run; run += *run + 1)
    if (i2c.write(PAJ7620, run + 1, *run) != MICROBIT_OK)
      return false;
  return i2c.readRegister(PAJ7620, 0x43, flags, 2) == MICROBIT_OK;
}

// The gesture flags raised since the last read, which clears them, or -1
// without a sensor: bits 0-7 of register 0x43, then bit 0 of 0x44, for
// right, left, up, down, forward, backward, clockwise, counter-clockwise
// and wave, as Elecfreaks name them, seen facing the window with the
// socket to the left. A sensor not answering, at first or plugged in
// again, is set up anew and has no flags yet.
static int gesture_flags() {
  static bool ready = false;
  uint8_t flags[2];
  if (ready && i2c.readRegister(PAJ7620, 0x43, flags, 2) == MICROBIT_OK)
    return flags[0] | (flags[1] & 1) << 8;
  ready = paj7620_init();
  return ready ? 0 : -1;
}

// PlanetX color sensor EF05006 comes in two kinds, set up and read much
// as Elecfreaks do. The newer, at I2C address 0x43, takes an integration
// time at 0x81 and power at 0x80, and gives red, green, blue and clear
// light from 0xA0 on. The older, an APDS9960 at 0x39, takes an
// integration time at 0x81, gain at 0x8F and power at 0x80, and gives
// clear, red, green and blue light from 0x94 on, read together and the
// clear moved last. Its gain is a quarter of theirs, as with theirs a
// white brick comes near the most it can count. Each value is two bytes,
// low byte first as in memory here.
#define COLOR_NEW 0x86
#define COLOR_OLD 0x72

// The kind of sensor found, or 0 before one is.
static uint8_t color_sensor;

static bool color_write(uint8_t sensor, uint8_t reg, uint8_t value) {
  uint8_t data[] = { reg, value };
  return i2c.write(sensor, data, 2) == MICROBIT_OK;
}

// Red, green, blue and clear light from the sensor found.
static bool color_read(uint16_t light[4]) {
  if (color_sensor == COLOR_NEW)
    return i2c.readRegister(COLOR_NEW, 0xA0, (uint8_t *)light, 8)
      == MICROBIT_OK;
  if (i2c.readRegister(COLOR_OLD, 0x94, (uint8_t *)light, 8)
      != MICROBIT_OK)
    return false;
  uint16_t clear = light[0];
  memmove(light, light + 1, 3 * sizeof *light);
  light[3] = clear;
  return true;
}

// The kind that answers, set up and given the 150 ms Elecfreaks wait for
// a first measurement, or 0 without a sensor.
static uint8_t color_init() {
  uint8_t sensor;
  if (color_write(COLOR_NEW, 0x81, 0xCA)) {
    color_write(COLOR_NEW, 0x80, 0x17);
    sensor = COLOR_NEW;
  } else if (color_write(COLOR_OLD, 0x81, 0xFC)) {
    color_write(COLOR_OLD, 0x8F, 0x02);
    color_write(COLOR_OLD, 0x80, 0x03);
    sensor = COLOR_OLD;
  } else {
    return 0;
  }
  uBit.sleep(150);
  return sensor;
}

// The light of the four channels, or an error without a sensor. One not
// answering, at first or plugged in again, is looked for anew.
static void color_light(lua_State *L, uint16_t light[4]) {
  if (color_sensor && color_read(light))
    return;
  color_sensor = color_init();
  if (!color_sensor || !color_read(light))
    luaL_error(L, "no color sensor");
}

// Hue in degrees, saturation and value of the light, its channels taken
// relative to what the sensor found gives for a white LEGO brick 1 to
// 2 cm away, so that such a brick has value 1.
static void color_hsv(lua_State *L, float hsv[3]) {
  static const float new_white[] = { 4443, 10312, 11104 },
    old_white[] = { 414, 792, 948 };
  uint16_t light[4];
  float c[3], max, min, d;
  color_light(L, light);
  const float *white = color_sensor == COLOR_NEW ? new_white : old_white;
  for (int i = 0; i < 3; i++)
    c[i] = light[i] / white[i];
  max = fmaxf(c[0], fmaxf(c[1], c[2]));
  min = fminf(c[0], fminf(c[1], c[2]));
  d = max - min;
  hsv[0] = d == 0 ? 0
    : max == c[0] ? fmodf(60 * (c[1] - c[2]) / d + 360, 360)
    : max == c[1] ? 60 * (c[2] - c[0]) / d + 120
    : 60 * (c[0] - c[1]) / d + 240;
  hsv[1] = d == 0 ? 0 : d / max;
  hsv[2] = max;
}

// The color numbered as bits of blue, red and green: 1 blue, 2 red,
// 3 magenta, 4 green, 5 cyan, 6 yellow, 7 white, or 0 when too dark.
// The hues dividing them are those LEGO bricks were seen to sit between
// by each kind of sensor; the older sees blue nearer to cyan.
static int color_number(const float hsv[3]) {
  static const uint16_t new_bound[] = { 25, 80, 180, 222, 270, 345 },
    old_bound[] = { 16, 80, 180, 207, 270, 345 };
  static const uint8_t number[] = { 2, 6, 4, 5, 1, 3, 2 };
  const uint16_t *bound = color_sensor == COLOR_NEW ? new_bound : old_bound;
  int i = 0;
  if (hsv[2] < 0.05f)
    return 0;
  if (hsv[1] < 0.25f)
    return 7;
  while (i < 6 && hsv[0] >= bound[i])
    i++;
  return number[i];
}

const int digitalRJ[] = { 8, 12, 14, 16 };

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
                  })							\
/* gesture() -> 1 right, 2 left, 3 up, 4 down, 5 forward, 6 backward,	\
   7 clockwise, 8 counter-clockwise, 9 wave, or 0. A gesture counts	\
   only if its flag is the only one raised, as with Elecfreaks, but a	\
   wave also with those of the swipes it is made of. */			\
   F(gesture,     { int flags = gesture_flags();			\
                    if (flags < 0)					\
                      return luaL_error(L, "no gesture sensor");	\
                    if (flags > 0x100)					\
                      flags = 0x100;					\
                    flags = flags == 0 || flags & (flags - 1) ? 0	\
                      : __builtin_ctz(flags) + 1;			\
                    lua_pushinteger(L, flags);				\
                    return 1;						\
                  })							\
/* colorRGB() -> red, green, blue and clear light as measured */	\
   F(colorRGB,    { uint16_t light[4];					\
                    color_light(L, light);				\
                    for (int i = 0; i < 4; i++)				\
                      lua_pushinteger(L, light[i]);			\
                    return 4;						\
                  })							\
/* colorHSV() -> hue in degrees, saturation, value 1 for white */	\
   F(colorHSV,    { float hsv[3];					\
                    color_hsv(L, hsv);				\
                    for (int i = 0; i < 3; i++)				\
                      lua_pushnumber(L, hsv[i]);			\
                    return 3;						\
                  })							\
/* color() -> 1 blue, 2 red, 3 magenta, 4 green, 5 cyan, 6 yellow,	\
   7 white, or 0 */							\
   F(color,       { float hsv[3];					\
                    color_hsv(L, hsv);				\
                    lua_pushinteger(L, color_number(hsv));		\
                    return 1;						\
                  })

#define LUA_PLANETX_COUNT 7

#define LUA_CODAL_CONSTANTS \
    C(MICROBIT_ID_LOGO) \
    C(DEVICE_ID_BUTTON_A) \
    C(DEVICE_ID_BUTTON_B) \
    C(DEVICE_ID_BUTTON_AB) \
    C(DEVICE_ID_DISPLAY) \
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
    C(DISPLAY_EVT_ANIMATION_COMPLETE) \
    C(DISPLAY_EVT_FREE) \
    C(LED_MATRIX_EVT_LIGHT_SENSE) \
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

#define F(name, body) static int l_nezha2_##name(lua_State *L) body
LUA_NEZHA2_FUNCTIONS
#undef F

// One entry per public name of an API namespace: a method (func set) or an
// event constant (func NULL, value set). Kept in flash; the namespace tables
// reference these arrays and materialise entries on first access.
typedef struct {
  const char *name;
  lua_CFunction func;
  lua_Integer value;
} LuaApi;

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

static const LuaApi l_tpbot2[] = {
    TPBOT2_FUNCTIONS
    {NULL, NULL, 0}
};
#undef X

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

// Resolve a missing field on an API namespace table. Upvalue 1 is the
// namespace's LuaApi array. The first matching entry is materialised (a C
// closure for a method, an integer for a constant) and cached in the table, so
// only names that are actually used ever allocate.
static int l_lazy_index(lua_State *L) {
  const char *key = (lua_type(L, 2) == LUA_TSTRING) ? lua_tostring(L, 2) : NULL;
  if (key != NULL) {
    const LuaApi *e = (const LuaApi *)lua_touserdata(L, lua_upvalueindex(1));
    for (; e->name != NULL; e++) {
      if (strcmp(e->name, key) == 0) {
        if (e->func != NULL)
          lua_pushcfunction(L, e->func);
        else
          lua_pushinteger(L, e->value);
        lua_pushvalue(L, 2);    // key
        lua_pushvalue(L, -2);   // value
        lua_rawset(L, 1);       // table[key] = value (cache it)
        return 1;
      }
    }
  }
  return 0;
}

// Give the table at absolute index idx the lazy __index over api.
static void lua_set_lazy_index(lua_State *L, int idx, const LuaApi *api) {
  lua_newtable(L);
  lua_pushlightuserdata(L, (void *)api);
  lua_pushcclosure(L, l_lazy_index, 1);
  lua_setfield(L, -2, "__index");
  lua_setmetatable(L, idx);
}


// module(), from lua-5.1.5/src/loadlib.c

static void setfenv (lua_State *L) {
  lua_Debug ar;
  if (lua_getstack(L, 1, &ar) == 0 ||
      lua_getinfo(L, "f", &ar) == 0 ||  /* get calling function */
      lua_iscfunction(L, -1))
    luaL_error(L, LUA_QL("module") " not called from a Lua function");
  lua_pushvalue(L, -2);
  lua_setfenv(L, -2);
  lua_pop(L, 1);
}


static void dooptions (lua_State *L, int n) {
  int i;
  for (i = 2; i <= n; i++) {
    lua_pushvalue(L, i);  /* get option (a function) */
    lua_pushvalue(L, -2);  /* module */
    lua_call(L, 1, 0);
  }
}


static void modinit (lua_State *L, const char *modname) {
  const char *dot;
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "_M");  /* module._M = module */
  lua_pushstring(L, modname);
  lua_setfield(L, -2, "_NAME");
  dot = strrchr(modname, '.');  /* look for last dot in module name */
  if (dot == NULL) dot = modname;
  else dot++;
  /* set _PACKAGE as package name (full module name minus last part) */
  lua_pushlstring(L, modname, dot - modname);
  lua_setfield(L, -2, "_PACKAGE");
}


/* push the module's table, creating it (and _LOADED[modname]) if needed */
static void push_module (lua_State *L, const char *modname) {
  lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
  lua_getfield(L, -1, modname);  /* get _LOADED[modname] */
  if (!lua_istable(L, -1)) {  /* not found? */
    lua_pop(L, 1);  /* remove previous result */
    /* try global variable (and create one if it does not exist) */
    if (luaL_findtable(L, LUA_GLOBALSINDEX, modname, 1) != NULL)
      luaL_error(L, "name conflict for module " LUA_QS, modname);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, modname);  /* _LOADED[modname] = new table */
  }
  lua_remove(L, -2);  /* remove _LOADED */
  /* check whether table already has a _NAME field */
  lua_getfield(L, -1, "_NAME");
  if (!lua_isnil(L, -1))  /* is table an initialized module? */
    lua_pop(L, 1);
  else {  /* no; initialize it */
    lua_pop(L, 1);
    modinit(L, modname);
  }
}


static int ll_module (lua_State *L) {
  const char *modname = luaL_checkstring(L, 1);
  int n = lua_gettop(L);  /* number of arguments */
  push_module(L, modname);
  lua_pushvalue(L, -1);
  setfenv(L);
  dooptions(L, n);
  return 0;
}


// Modules
//
// Nothing is loaded before the embedded script runs. require(name)
// makes a module from the list below the first time it is asked for:
// its table is made as module() makes it, so microbit.audio lands in
// microbit.audio, and gets the lazy __index over the module's API.
// A parent made on the way (microbit, for microbit.audio) is a plain
// table; requiring it later gives that same table its API.

// A module require() can make: its name, its API, and what else its first
// require does, if anything.
typedef struct {
  const char *name;
  const LuaApi *api;
  void (*open)(lua_State *L);
} LuaModule;

static const LuaModule lua_modules[] = {
  {"microbit",               l_microbit, NULL},
  {"microbit.display",       l_display, NULL},
  {"microbit.accelerometer", l_accelerometer, NULL},
  {"microbit.compass",       l_compass, NULL},
  {"microbit.audio",         l_audio, NULL},
  {"microbit.io",            l_io, NULL},
  {"microbit.serial",        l_serial, NULL},
  {"microbit.i2c",           l_i2c, NULL},
  {"microbit.radio",         l_radio, NULL},
#if CONFIG_ENABLED(DEVICE_BLE)
  {"microbit.ble.uart",      l_ble_uart, NULL},
#endif
  {"planetx",                l_planetx, NULL},
  {"tpbot",                  l_tpbot, tpbot_register_globals},
  {"tpbot2",                 l_tpbot2, tpbot2_register_globals},
  {"nezha2",                 l_nezha2, NULL},
  {NULL, NULL, NULL}
};

static int l_require(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
  lua_getfield(L, -1, name);
  if (lua_toboolean(L, -1)) return 1;
  const LuaModule *m = lua_modules;
  while (m->name != NULL && strcmp(m->name, name) != 0) m++;
  if (m->name == NULL)
    return luaL_error(L, "module " LUA_QS " not found", name);
  push_module(L, name);
  lua_set_lazy_index(L, lua_gettop(L), m->api);
  if (m->open != NULL)
    m->open(L);
  return 1;
}


// package, module and require: all there is before the script runs.
// package.loaded is the registry's _LOADED, which module() relies on.
void register_lua_modules(lua_State *L) {
  lua_newtable(L);                                    // package
  lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
  lua_setfield(L, -2, "loaded");
  lua_setglobal(L, "package");
  lua_register(L, "module", ll_module);
  lua_register(L, "require", l_require);
}

// What source/lua-events.c needs of the board
extern "C" void lua_events_mistake(const char *message) {
  uBit.serial.send(ManagedString("\r\nRuntime error: ") + message + "\r\n");
  uBit.display.scrollAsync(message);
}

extern "C" uint32_t lua_events_now(void) {
  return (uint32_t)uBit.systemTime();
}

extern "C" void lua_events_pause(uint32_t ms) {
  uBit.sleep(ms);
}

// Runs in a dedicated fiber (spawned by on_codal_event).  Has a full
// fiber stack so lua_pcall won't overflow the idle or interrupt stacks.
// The event goes to Lua now, or once the call running there lets it in
// (source/lua-events.c).
static void lua_event_handler_fiber(void *arg) {
  codal::Event *event = (codal::Event *)arg;
  LuaEvent e = { event->source, event->value, (uint32_t)event->timestamp };
  delete event;
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
// Filtering removes ~99% of the per-tick/per-char fiber traffic. So are the
// serial port's data-received event, raised at an idle tick after any
// character, and its RX-full event, raised for every character lost: no
// script reads them, and while a Lua call runs they would fill the line of
// events that wait for it. So would the accelerometer's samples, about
// fifty a second once the script requires it for gestures, which come to
// Lua as gesture events of their own.
static void on_codal_event(codal::Event e, void *arg) {
  (void)arg;
  if (e.source == DEVICE_ID_SCHEDULER || e.source == DEVICE_ID_COMPONENT ||
      e.source == DEVICE_ID_NOTIFY || e.source == DEVICE_ID_NOTIFY_ONE)
    return;
  if (e.source == DEVICE_ID_SERIAL
      && (e.value == CODAL_SERIAL_EVT_DATA_RECEIVED
          || e.value == CODAL_SERIAL_EVT_RX_FULL))
    return;
  if (e.source == DEVICE_ID_ACCELEROMETER
      && e.value == ACCELEROMETER_EVT_DATA_UPDATE)
    return;
  codal::Event *copy = new codal::Event(e);
  create_fiber(lua_event_handler_fiber, copy);
}

void register_lua_event_listener(lua_State *L) {
  lua_events_open(L, DEVICE_ID_SERIAL, CODAL_SERIAL_EVT_HEAD_MATCH);
  uBit.messageBus.listen(DEVICE_ID_ANY, DEVICE_EVT_ANY,
                         on_codal_event, NULL,
                         MESSAGE_BUS_LISTENER_IMMEDIATE);
}
