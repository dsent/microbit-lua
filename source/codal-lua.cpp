// -*- mode: c++; indent-tabs-mode: nil; -*-
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "codal-lua.h"
#include "MicroBit.h"
#include "Event.h"
#include "I2C.h"

extern MicroBit uBit;

// Bind the Lua i2c API to CODAL's pre-wired EXTERNAL edge-connector bus
// (MICROBIT_PIN_EXT_SDA=P1_00 / MICROBIT_PIN_EXT_SCL=P0_26), where the TPBot
// lives.
I2C &i2c = uBit.i2c;

#define LUA_MICROBIT_FUNCTIONS						\
    X(reset,      { uBit.reset();					\
                    return 0;						\
                  })							\
    X(sleep,      { uint32_t ms = (uint32_t)luaL_checkinteger(L, 1);	\
                    uBit.sleep(ms);					\
                    return 0;						\
                  })							\
    X(seedRandom, { uint32_t seed = (uint32_t)luaL_optinteger(L, 1, 0);	\
                    if(seed) { uBit.seedRandom(seed); }			\
                    else { uBit.seedRandom(); }				\
                    return 0;						\
		  })							\
    X(random,     { int max = (int)luaL_checkinteger(L, 1);		\
                    lua_pushinteger(L, (lua_Integer)uBit.random(max));	\
                    return 1;						\
                  })							\
    X(systemTime, { lua_pushinteger(L, (lua_Integer)uBit.systemTime());	\
                    return 1;						\
                  })							\
    X(serialNumber, { lua_pushinteger(L, microbit_serial_number());	\
                    return 1;						\
                  })							\
    X(friendlyName, { lua_pushstring(L, microbit_friendly_name());	\
                    return 1;						\
                  })							\
    X(panic,      { int statusCode = (int)luaL_checkinteger(L, 1);      \
                    microbit_panic(statusCode);				\
                    return 0;						\
                  })

// create a Lua image table from C ImageData and place it on the stack
void lua_createimage(lua_State *L, ImageData *ptr) {
  int size = ptr->width * ptr->height;
  lua_createtable(L, 0, 3);
  lua_pushliteral(L, "width");
  lua_pushinteger(L, ptr->width);
  lua_settable(L, -3);
  lua_pushliteral(L, "height");
  lua_pushinteger(L, ptr->height);
  lua_settable(L, -3);
  lua_pushliteral(L, "data");
  lua_createtable(L, size, 0);
  for(int i = 0; i < size; i++) {
    lua_pushinteger(L, ptr->data[i]);
    lua_rawseti(L, -2, i + 1);
  }
  lua_settable(L, -3);
}

// check and return a Lua function argument as a C++ Image object
Image luaL_checkimage(lua_State *L, int narg) {
  Image r;
  int width;
  int height;
  uint8_t value;
  luaL_checktype(L, narg, LUA_TTABLE);
  lua_getfield(L, narg, "width");
  width = (int)lua_tointeger(L, -1);
  lua_getfield(L, narg, "height");
  height = (int)lua_tointeger(L, -1);
  lua_pop(L, 2);
  r = Image(width, height);
  lua_getfield(L, narg, "data");
  for(int y = 0; y < height; y++) {
    for(int x = 0; x < width; x++) {
      lua_pushinteger(L, 1 + x + width * y);
      lua_gettable(L, -2);
      value = (uint8_t)lua_tointeger(L, -1);
      lua_pop(L, 1);
      if(r.setPixelValue(x, y, value) != DEVICE_OK) {
        luaL_error(L, "image error");
      }
    }
  }
  lua_pop(L, 1);
  return r;
}

// see https://rneacy.dev/mbv2/ubit/display/
#define LUA_DISPLAY_FUNCTIONS						\
    X(getWidth,   { lua_pushinteger(L, uBit.display.getWidth());	\
                    return 1;						\
                  })							\
    X(getHeight,  { lua_pushinteger(L, uBit.display.getHeight());	\
                    return 1;						\
                  })							\
    X(setBrightness, { int b = luaL_checkint(L, 1);			\
                    int r = uBit.display.setBrightness(b);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(getBrightness, { int r = uBit.display.getBrightness();		\
                    lua_pushinteger(L, r);				\
                    return 1;						\
                  })							\
    X(enable,     { uBit.display.enable();				\
                    return 0;						\
                  })							\
    X(disable,     { uBit.display.disable();				\
                    return 0;						\
                  })							\
    X(screenShot, { ImageData *ptr =					\
                      uBit.display.screenShot().leakData();		\
                    lua_createimage(L, ptr);				\
                    ptr->decr();					\
                    return 1;						\
                  })							\
    X(setDisplayMode, { DisplayMode mode = 				\
                      static_cast<DisplayMode>(luaL_checkinteger(L, 1));\
                    uBit.display.setDisplayMode(mode);			\
                    return 0;						\
                  })							\
    X(getDisplayMode, { DisplayMode mode =				\
                      uBit.display.getDisplayMode();			\
                    lua_pushinteger(L, static_cast<lua_Integer>(mode));	\
                    return 1;						\
                  })							\
    X(clear,      { uBit.display.clear();				\
                    return 0;						\
                  })							\
    X(readLightLevel, { int r = uBit.display.readLightLevel();		\
                    lua_pushinteger(L, r);				\
                    return 1;						\
                  })							\
    X(setSleep,   { NRF52LEDMatrix display = uBit.display;		\
                    luaL_checkany(L, 1);				\
                    display.setSleep(lua_toboolean(L, 1) != 0);		\
                    return 0;						\
                  })							\
    X(stopAnimation, { uBit.display.stopAnimation();			\
                    return 0;						\
                  })							\
    X(printAsync, { const char *s = luaL_checkstring(L, 1);		\
                    int delay = luaL_optint(L, 2,			\
                      DISPLAY_DEFAULT_PRINT_SPEED);			\
                    int r = uBit.display.printAsync(s, delay);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(print,      { const char *s = luaL_checkstring(L, 1);		\
                    int delay = luaL_optint(L, 2,			\
                      DISPLAY_DEFAULT_PRINT_SPEED);			\
                    int r = uBit.display.print(s, delay);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(scrollAsync, { const char *s = luaL_checkstring(L, 1);		\
                    int delay = luaL_optint(L, 2,			\
                      DISPLAY_DEFAULT_SCROLL_SPEED);			\
                    int r = uBit.display.scrollAsync(s, delay);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(scroll,     { const char *s = luaL_checkstring(L, 1);		\
                    int delay = luaL_optint(L, 2,			\
                      DISPLAY_DEFAULT_SCROLL_SPEED);			\
                    int r = uBit.display.scroll(s, delay);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(animateAsync, { Image image = luaL_checkimage(L, 1);		\
                    int delay = luaL_checkint(L, 2);			\
                    int stride = luaL_checkint(L, 3);			\
                    int startingPosition =				\
                      luaL_optint(L, 4, DISPLAY_ANIMATE_DEFAULT_POS);	\
                    int autoClear =					\
                      luaL_optint(L, 5, DISPLAY_DEFAULT_AUTOCLEAR);	\
                    int r = uBit.display.animateAsync(image,		\
                                                      delay,		\
                                                      stride,		\
                                                      startingPosition,	\
                                                      autoClear);	\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(animate,    { Image image = luaL_checkimage(L, 1);		\
                    int delay = luaL_checkint(L, 2);			\
                    int stride = luaL_checkint(L, 3);			\
                    int startingPosition =				\
                      luaL_optint(L, 4, DISPLAY_ANIMATE_DEFAULT_POS);	\
                    int autoClear =					\
                      luaL_optint(L, 5, DISPLAY_DEFAULT_AUTOCLEAR);	\
                    int r = uBit.display.animate(image,			\
                                                 delay,			\
                                                 stride,		\
                                                 startingPosition,	\
                                                 autoClear);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(setPixelValue, {							\
                    uint16_t x = (uint16_t)luaL_checkint(L, 1);		\
                    uint16_t y = (uint16_t)luaL_checkint(L, 2);		\
                    uint8_t value = (uint8_t)luaL_checkint(L, 3);	\
                    int r =						\
                      uBit.display.image.setPixelValue(x, y, value);	\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(getPixelValue, {							\
                    uint16_t x = (uint16_t)luaL_checkint(L, 1);		\
                    uint16_t y = (uint16_t)luaL_checkint(L, 2);		\
                    int r = uBit.display.image.getPixelValue(x, y);	\
                    if(r != DEVICE_INVALID_PARAMETER) {			\
                      lua_pushinteger(L, r);				\
                      return 1;						\
                    }							\
                    return 0;						\
                  })

// I failed counting X expansions in LUA_DISPLAY_FUNCTIONS automatically
#define LUA_DISPLAY_COUNT 21

// see https://rneacy.dev/mbv2/ubit/accelerometer/
#define LUA_ACCELEROMETER_FUNCTIONS					\
    X(setPeriod,  { int period = luaL_checkint(L, 1);			\
                    int r = uBit.accelerometer.setPeriod(period);	\
                    lua_pushboolean(L, r == MICROBIT_OK);		\
                    return 1;						\
                  })							\
    X(getPeriod,  { int r = uBit.accelerometer.getPeriod();		\
                    lua_pushinteger(L, r);				\
                    return 1;						\
                  })							\
    X(setRange,   { int range = luaL_checkint(L, 1);			\
                    int r = uBit.accelerometer.setRange(range);		\
                    lua_pushboolean(L, r == MICROBIT_OK);		\
                    return 1;						\
                  })							\
    X(getRange,   { int r = uBit.accelerometer.getRange();		\
                    lua_pushinteger(L, r);				\
                    return 1;						\
                  })							\
    X(configure,  { int r = uBit.accelerometer.configure();		\
                    lua_pushboolean(L, r == MICROBIT_OK);		\
                    return 1;						\
                  })							\
    X(requestUpdate, { int r = uBit.accelerometer.requestUpdate();	\
                    lua_pushboolean(L, r == MICROBIT_OK);		\
                    return 1;						\
                  })							\
    X(getSample,  { Sample3D sample = uBit.accelerometer.getSample();	\
                    lua_pushinteger(L, sample.x);			\
                    lua_pushinteger(L, sample.y);			\
                    lua_pushinteger(L, sample.z);			\
                    return 3;						\
                  })							\
    X(getGesture, { uint16_t r = uBit.accelerometer.getGesture();	\
                    lua_pushinteger(L, r);				\
                    return 1;						\
                  })

#define LUA_ACCELEROMETER_COUNT 6

// see https://rneacy.dev/mbv2/ubit/compass/
#define LUA_COMPASS_FUNCTIONS						\
    X(heading,     { int r = uBit.compass.heading();			\
                     if(r == DEVICE_CALIBRATION_IN_PROGRESS) {		\
                       lua_pushnil(L);					\
                     } else {						\
                       lua_pushinteger(L, r);				\
                     }							\
                     return 1;						\
                   })							\
    X(getFieldStrength, { int r = uBit.compass.getFieldStrength();	\
                     lua_pushinteger(L, r);				\
                     return 1;						\
                   })							\
    X(calibrate,   { int r = uBit.compass.calibrate();			\
                     lua_pushboolean(L, r == MICROBIT_OK);		\
                     return 1;						\
                   })							\
    X(setCalibration, { CompassCalibration cc = CompassCalibration();	\
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
    X(getCalibration, { CompassCalibration cc =				\
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
    X(isCalibrated, { int r = uBit.compass.isCalibrated();		\
                     lua_pushboolean(L, r);				\
                     return 1;						\
                   })							\
    X(isCalibrating, { int r = uBit.compass.isCalibrating();		\
                     lua_pushboolean(L, r);				\
                     return 1;						\
                   })							\
    X(clearCalibration, { uBit.compass.clearCalibration();		\
                     return 0;						\
                   })							\
    X(configure,   { int r = uBit.compass.configure();			\
                     lua_pushboolean(L, r == MICROBIT_OK);		\
                     return 1;						\
                   })							\
    X(setPeriod,   { int period = luaL_checkint(L, 1);			\
                     int r = uBit.compass.setPeriod(period);		\
                     lua_pushboolean(L, r == MICROBIT_OK);		\
                     return 1;						\
                   })							\
    X(getPeriod,   { int r = uBit.compass.getPeriod();			\
                     lua_pushinteger(L, r);				\
                     return 1;						\
                   })							\
    X(requestUpdate, { int r = uBit.compass.requestUpdate();		\
                     lua_pushboolean(L, r == MICROBIT_OK);		\
                     return 1;						\
                   })							\
    X(update,      { int r = uBit.compass.update();			\
                     lua_pushboolean(L, r == MICROBIT_OK);		\
                     return 1;						\
                   })							\
    X(getSample,  { Sample3D sample = uBit.compass.getSample();		\
                    lua_pushinteger(L, sample.x);			\
                    lua_pushinteger(L, sample.y);			\
                    lua_pushinteger(L, sample.z);			\
                    return 3;						\
                  })

#define LUA_COMPASS_COUNT 14

#define LUA_AUDIO_FUNCTIONS						\
    X(getAudioPin, { lua_pushlightuserdata(L,				\
                      &uBit.audio.virtualOutputPin);			\
                    return 1;						\
                  })							\
    X(setVolume,  { int volume = luaL_checkint(L, 1);			\
                    int r = uBit.audio.setVolume(volume);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(getVolume,  { lua_pushinteger(L, uBit.audio.getVolume());		\
                    return 1;						\
                  })							\
    X(express,    { const char *expression = luaL_checkstring(L, 1);	\
                    uBit.audio.soundExpressions.playAsync(expression);	\
                    return 0;						\
                  })

#define LUA_AUDIO_COUNT 4

Pin *luaL_checkPin(lua_State *L, int narg) {
  luaL_checktype(L, narg, LUA_TLIGHTUSERDATA);
  return (Pin *)lua_touserdata(L, narg);
}

#define LUA_IO_FUNCTIONS						\
    X(setDigitalValue, { 						\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int value = luaL_checkint(L, 2);			\
                    int r = pin->setDigitalValue(value);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(getDigitalValue, {						\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->getDigitalValue();			\
                    if(r == 0 || r == 1) {				\
                      lua_pushinteger(L, r);				\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    X(setAnalogValue, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int value = luaL_checkint(L, 2);			\
                    int r = pin->setAnalogValue(value);			\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(setServoValue, {							\
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
    X(getAnalogValue, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->getAnalogValue();			\
                    if(r >= 0 || r <= 1024) {				\
                      lua_pushinteger(L, r);				\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    X(isInput,    { Pin *pin = luaL_checkPin(L, 1);			\
                    lua_pushboolean(L, pin->isInput() == 1);		\
                    return 1;						\
                  })							\
    X(isOutput,   { Pin *pin = luaL_checkPin(L, 1);			\
                    lua_pushboolean(L, pin->isOutput() == 1);		\
                    return 1;						\
                  })							\
    X(isDigital,  { Pin *pin = luaL_checkPin(L, 1);			\
                    lua_pushboolean(L, pin->isDigital() == 1);		\
                    return 1;						\
                  })							\
    X(isAnalog,   { Pin *pin = luaL_checkPin(L, 1);			\
                    lua_pushboolean(L, pin->isAnalog() == 1);		\
                    return 1;						\
                  })							\
    X(isTouched,  { Pin *pin = luaL_checkPin(L, 1);			\
                    lua_pushboolean(L, pin->isTouched() == 1);		\
                    return 1;						\
                  })							\
    X(setServoPulseUs, {						\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    uint32_t pulseWidth =				\
                      (uint32_t)luaL_checkinteger(L, 2); 		\
                    int r = pin->setServoPulseUs(pulseWidth);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(setAnalogPeriod, {						\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int period = luaL_checkint(L, 2);			\
                    int r = pin->setAnalogPeriod(period);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(setAnalogPeriodUs, {						\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    uint32_t period =					\
                      (uint32_t)luaL_checkinteger(L, 2);		\
                    int r = pin->setAnalogPeriodUs(period);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(getAnalogPeriodUs, {						\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    uint32_t r = pin->getAnalogPeriodUs();		\
                    if((int)r != DEVICE_NOT_SUPPORTED) {		\
                      lua_pushinteger(L, r);				\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    X(getAnalogPeriod, {						\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->getAnalogPeriod();			\
                    if(r != DEVICE_NOT_SUPPORTED) {			\
                      lua_pushinteger(L, r);				\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    X(setPullUp,  { Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->setPull((PullMode)PullUp);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(setPullDown, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->setPull((PullMode)PullDown);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(setPullNone, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->setPull((PullMode)PullNone);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(drainPin,   { Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->drainPin();				\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(pulseUs,    { Pin *pin = luaL_checkPin(L, 1);			\
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
    X(getPulseUs, { Pin *pin = luaL_checkPin(L, 1);			\
                    int timeout = luaL_checkint(L, 2);			\
                    int r = pin->getPulseUs(timeout);			\
                    if(r != DEVICE_CANCELLED) {				\
                      lua_pushinteger(L, r);				\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    X(eventOnEdge, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->eventOn(DEVICE_PIN_EVENT_ON_EDGE);	\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(eventOnPulse, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->eventOn(DEVICE_PIN_EVENT_ON_PULSE);	\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(eventOnTouch, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->eventOn(DEVICE_PIN_EVENT_ON_TOUCH);	\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(eventNone,  { Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->eventOn(DEVICE_PIN_EVENT_NONE);	\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(isActive,   { Pin *pin = luaL_checkPin(L, 1);			\
                    int r = pin->isActive();				\
                    lua_pushboolean(L, r == 1);				\
                    return 1;						\
                  })							\
    X(setPolarity, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    int polarity = luaL_checkint(L, 2);			\
                    pin->setPolarity(polarity);				\
                    return 0;						\
                  })							\
    X(getPolarity, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    lua_pushinteger(L, pin->getPolarity());		\
                    return 1;						\
                  })							\
    X(setActiveHi, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    pin->setActiveHi();					\
                    return 0;						\
                  })							\
    X(setActiveLo, {							\
                    Pin *pin = luaL_checkPin(L, 1);			\
                    pin->setActiveLo();					\
                    return 0;						\
                  })							\
    X(disconnect, { Pin *pin = luaL_checkPin(L, 1);			\
                    pin->disconnect();					\
                    return 0;						\
                  })							\
    X(getPin,     { int pin = luaL_checkint(L, 1);			\
                    lua_pushlightuserdata(L, &uBit.io.pin[pin]);	\
                    return 1;						\
                  })

#define LUA_IO_COUNT 32

void lua_pushManagedString(lua_State *L, ManagedString s) {
  lua_pushlstring(L, s.toCharArray(), s.length());
}

ManagedString luaL_checkManagedString(lua_State *L, int narg) {
  size_t length;
  const char *str = luaL_checklstring(L, narg, &length);
  return ManagedString(str, length);
}

#define LUA_SERIAL_FUNCTIONS						\
    X(send,       { ManagedString s = luaL_checkManagedString(L, 1);	\
                    int r = uBit.serial.send(s, SYNC_SLEEP);		\
                    if(r != DEVICE_SERIAL_IN_USE &&			\
                       r != DEVICE_INVALID_PARAMETER) {			\
                      lua_pushinteger(L, r);				\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    X(sendAsync,  { ManagedString s = luaL_checkManagedString(L, 1);	\
                    int r = uBit.serial.send(s, ASYNC);			\
                    if(r != DEVICE_SERIAL_IN_USE &&			\
                       r != DEVICE_INVALID_PARAMETER) {			\
                      lua_pushinteger(L, r);				\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    X(getByte,    { int r = uBit.serial.getChar(SYNC_SLEEP);		\
                    lua_pushinteger(L, r);				\
                    return 1;						\
                  })							\
    X(getByteAsync, {							\
                    int r = uBit.serial.getChar(ASYNC);			\
                    if(r != DEVICE_NO_DATA) {				\
                      lua_pushinteger(L, r);				\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    X(getChar,    { char r = (char)uBit.serial.getChar(SYNC_SLEEP);	\
                    lua_pushlstring(L, &r, 1);				\
                    return 1;						\
                  })							\
    X(getCharAsync, {							\
                    int r = uBit.serial.getChar(ASYNC);			\
                    if(r != DEVICE_NO_DATA) {				\
                      char c = (char)r;					\
                      lua_pushlstring(L, &c, 1);			\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    X(read,       { int size = luaL_checkint(L, 1);			\
                    lua_pushManagedString(L,				\
                      uBit.serial.read(size, SYNC_SLEEP));		\
                    return 1;						\
                  })							\
    X(readAsync,  { int size = luaL_checkint(L, 1);			\
                    lua_pushManagedString(L,				\
                      uBit.serial.read(size, ASYNC));			\
                    return 1;						\
                  })							\
    X(readUntil,  { ManagedString delimiters =				\
                      luaL_checkManagedString(L, 1);			\
                    lua_pushManagedString(L,				\
                      uBit.serial.readUntil(delimiters, SYNC_SLEEP));	\
                    return 1;						\
                  })							\
    X(setBaud,    { int baudrate = luaL_checkint(L, 1);			\
                    int r = uBit.serial.setBaud(baudrate);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(redirect,   { Pin *tx = luaL_checkPin(L, 1);			\
                    Pin *rx = luaL_checkPin(L, 2);			\
                    int r = uBit.serial.redirect(*tx, *rx);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(eventAfter, { uBit.serial.eventAfter(luaL_checkint(L, 1),		\
                                           SYNC_SLEEP);			\
                    return 0;						\
                  })							\
    X(eventAfterAsync, {						\
                    uBit.serial.eventAfter(luaL_checkint(L, 1),		\
                                           ASYNC);			\
                    return 0;						\
                  })							\
    X(eventOn,    { uBit.serial.eventOn(luaL_checkManagedString(L, 1),	\
                                           SYNC_SLEEP);			\
                    return 0;						\
                  })							\
    X(eventOnAsync, {							\
                    uBit.serial.eventOn(luaL_checkManagedString(L, 1),	\
                                           ASYNC);			\
                    return 0;						\
                  })							\
    X(isReadable, { int r = uBit.serial.isReadable();			\
                    if(r == 0 || r == 1) {				\
                      lua_pushboolean(L, r == 1);			\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    X(isWriteable, {							\
                    lua_pushboolean(L, uBit.serial.isWriteable() == 1);	\
                    return 1;						\
                  })							\
    X(setRxBufferSize, {						\
                    uint8_t size = luaL_checkint(L, 1);			\
                    int r = uBit.serial.setRxBufferSize(size);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(setTxBufferSize, {						\
                    uint8_t size = luaL_checkint(L, 1);			\
                    int r = uBit.serial.setTxBufferSize(size);		\
                    lua_pushboolean(L, r == DEVICE_OK);			\
                    return 1;						\
                  })							\
    X(getRxBufferSize, {						\
                    lua_pushinteger(L, uBit.serial.getRxBufferSize());	\
                    return 1;						\
                  })							\
    X(getTxBufferSize, {						\
                    lua_pushinteger(L, uBit.serial.getTxBufferSize());	\
                    return 1;						\
                  })							\
    X(clearRxBuffer, {							\
                    lua_pushboolean(L,					\
                      uBit.serial.clearRxBuffer() == DEVICE_OK);	\
                    return 1;						\
                  })							\
    X(clearTxBuffer, {							\
                    lua_pushboolean(L,					\
                      uBit.serial.clearTxBuffer() == DEVICE_OK);	\
                    return 1;						\
                  })							\
    X(rxBufferedSize, {							\
                    lua_pushinteger(L, uBit.serial.rxBufferedSize());	\
                    return 1;						\
                  })							\
    X(txBufferedSize, {							\
                    lua_pushinteger(L,uBit.serial.txBufferedSize());	\
                    return 1;						\
                  })							\
    X(rxInUse,    { lua_pushboolean(L,					\
                      uBit.serial.rxInUse() != 0);			\
                    return 1;						\
                  })							\
    X(txInUse,    { lua_pushboolean(L,					\
                      uBit.serial.txInUse() != 0);			\
                    return 1;						\
                  })

#define LUA_SERIAL_COUNT 25

#if CONFIG_ENABLED(DEVICE_BLE)
#include "MicroBitUARTService.h"

extern MicroBitUARTService *uart;

#define LUA_BLE_FUNCTIONS						\
    X(send,       { if(!uart) { lua_pushnil(L); return 1; }		\
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
    X(sendAsync,  { if(!uart) { lua_pushnil(L); return 1; }		\
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
    X(getChar,    { if(!uart) { lua_pushnil(L); return 1; }		\
                    char r = (char)uart->getc(SYNC_SLEEP);		\
                    lua_pushlstring(L, &r, 1);				\
                    return 1;						\
                  })							\
    X(getCharAsync, {							\
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
    X(read,       { if(!uart) { lua_pushnil(L); return 1; }		\
                    int size = luaL_checkint(L, 1);			\
                    lua_pushManagedString(L,				\
                      uart->read(size, SYNC_SLEEP));			\
                    return 1;						\
                  })							\
    X(readAsync,  { if(!uart) { lua_pushnil(L); return 1; }		\
                    int size = luaL_checkint(L, 1);			\
                    lua_pushManagedString(L,				\
                      uart->read(size, ASYNC));				\
                    return 1;						\
                  })							\
    X(readUntil,  { if(!uart) { lua_pushnil(L); return 1; }		\
                    ManagedString delimiters =				\
                      luaL_checkManagedString(L, 1);			\
                    lua_pushManagedString(L,				\
                      uart->readUntil(delimiters, SYNC_SLEEP));		\
                    return 1;						\
                  })							\
    X(eventOn,    { if(!uart) { return 0; }				\
                    uart->eventOn(luaL_checkManagedString(L, 1),	\
                                           SYNC_SLEEP);			\
                    return 0;						\
                  })							\
    X(eventOnAsync, {							\
                    if(!uart) { return 0; }				\
                    uart->eventOn(luaL_checkManagedString(L, 1),	\
                                           ASYNC);			\
                    return 0;						\
                  })							\
    X(eventAfter, { if(!uart) { return 0; }				\
                    uart->eventAfter(luaL_checkint(L, 1),		\
                                           SYNC_SLEEP);			\
                    return 0;						\
                  })							\
    X(eventAfterAsync, {						\
                    if(!uart) { return 0; }				\
                    uart->eventAfter(luaL_checkint(L, 1),		\
                                           ASYNC);			\
                    return 0;						\
                  })							\
    X(isReadable, { if(!uart) { lua_pushnil(L); return 1; }		\
                    int r = uart->isReadable();				\
                    if(r == 0 || r == 1) {				\
                      lua_pushboolean(L, r == 1);			\
                    } else {						\
                      lua_pushnil(L);					\
                    }							\
                    return 1;						\
                  })							\
    X(rxBufferedSize, {						\
                    if(!uart) { lua_pushnil(L); return 1; }		\
                    lua_pushinteger(L, uart->rxBufferedSize());		\
                    return 1;						\
                  })							\
    X(txBufferedSize, {						\
                    if(!uart) { lua_pushnil(L); return 1; }		\
                    lua_pushinteger(L, uart->txBufferedSize());		\
                    return 1;						\
                  })

#define LUA_BLE_COUNT 14
#endif // CONFIG_ENABLED(DEVICE_BLE)

#define LUA_I2C_FUNCTIONS						\
    X(read,       { int address = luaL_checkint(L, 1);			\
                    int length = luaL_checkint(L, 2);			\
                    char data[length];					\
                    if(i2c.read(address, data, length) == MICROBIT_OK){	\
                      lua_pushlstring(L, data, length);			\
                      return 1;						\
                    } else {						\
                      return luaL_error(L, "i2c read error");		\
                    }							\
                  })							\
    X(write,      { int address = luaL_checkint(L, 1);			\
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

#define RADIO_HELLO    1
#define RADIO_WELCOME  2
#define RADIO_DATA     3
#define RADIO_ACK      4

#define RADIO_TRIES    8
#define RADIO_WAIT     30

/* How long connect waits when nobody says otherwise */
#define RADIO_TIMEOUT  5000

static uint8_t radio_link = 0;
static char radio_peer[RADIO_NAME + 1];
static uint8_t radio_out = 0;   /* number of the last sent */
static uint8_t radio_in = 0;    /* number of the last taken */

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
    if (n > 0 && n <= (int)sizeof(radio_held)) {
        memcpy(radio_held, p.getBytes(), n);
        radio_held_len = n;
    }
    return false;
}

/* Both ends start a link the same way */
static void radio_open(uint8_t link, const char *peer)
{
    radio_link = link;
    radio_held_len = 0;
    radio_out = 0;
    radio_in = 0;
    memcpy(radio_peer, peer, RADIO_NAME);
    radio_peer[RADIO_NAME] = 0;
}

/* Call once, then wait a moment for the answer */
static bool radio_called(uint8_t link, const char *hello)
{
    uint64_t until;
    radio_put(RADIO_HELLO, link, 0, hello, RADIO_NAME * 2);
    until = uBit.systemTime() + RADIO_WAIT;
    while (uBit.systemTime() < until) {
        if (radio_take(RADIO_WELCOME, link, NULL, NULL,
                       NULL, NULL)) return true;
        uBit.sleep(1);
    }
    return false;
}

/* connect(friendlyName, timeout_ms) -> boolean */
static int radio_connect(lua_State *L)
{
    const char *them = luaL_checkstring(L, 1);
    int timeout = luaL_optint(L, 2, RADIO_TIMEOUT);
    char hello[RADIO_NAME * 2];
    uint8_t link = (uint8_t)(uBit.random(255) + 1);
    uint64_t deadline = uBit.systemTime() + timeout;

    memcpy(hello, them, RADIO_NAME);
    memcpy(hello + RADIO_NAME, microbit_friendly_name(),
           RADIO_NAME);

    while (uBit.systemTime() < deadline) {
        if (radio_called(link, hello)) {
            radio_open(link, them);
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

/* A HELLO addressed to this board: answer it and take the
 * link it names. Any link it replaces is dropped — the other
 * end has started over, which is how a reset board finds its
 * way back. */
static bool radio_called_us(void)
{
    const char *us = microbit_friendly_name();
    uint8_t body[RADIO_BODY];
    uint8_t link;
    int len;

    if (!radio_take(RADIO_HELLO, 0, &link, NULL, body, &len)
        || len != RADIO_NAME * 2
        || memcmp(body, us, RADIO_NAME) != 0) return false;
    radio_open(link, (char *)body + RADIO_NAME);
    radio_put(RADIO_WELCOME, link, 0, NULL, 0);
    return true;
}

/* listen() -> friendlyName of whoever connected */
static int radio_listen(lua_State *L)
{
    while (!radio_called_us()) uBit.sleep(1);
    lua_pushstring(L, radio_peer);
    return 1;
}

/* answered() -> friendlyName if somebody has just called
 * again, or nil */
static int radio_answered(lua_State *L)
{
    if (!radio_called_us()) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, radio_peer);
    return 1;
}


/* One piece, repeated until the far end answers it */
static bool radio_one(const char *body, int len)
{
    radio_out++;
    for (int n = 0; n < RADIO_TRIES; n++) {
        uint64_t until;
        radio_put(RADIO_DATA, radio_link, radio_out, body, len);
        until = uBit.systemTime() + RADIO_WAIT;
        while (uBit.systemTime() < until) {
            uint8_t num;
            if (radio_take(RADIO_ACK, radio_link, NULL,
                           &num, NULL, NULL)
                && num == radio_out) return true;
            uBit.sleep(1);
        }
    }
    return false;
}

/* tx(message) -> boolean
 * The message goes piece by piece, each taken before the
 * next one leaves. */
static int radio_tx(lua_State *L)
{
    size_t len;
    const char *msg = luaL_checklstring(L, 1, &len);
    size_t sent = 0;

    if (radio_link == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    do {
        int piece = (int)(len - sent);
        if (piece > RADIO_BODY) piece = RADIO_BODY;
        if (!radio_one(msg + sent, piece)) {
            lua_pushboolean(L, 0);
            return 1;
        }
        sent += piece;
    } while (sent < len);

    lua_pushboolean(L, 1);
    return 1;
}

/* rx() -> string or nil
 * One piece, acknowledged. A piece that arrives twice is
 * acknowledged again and dropped: the far end did not hear
 * the first answer. */
static int radio_rx(lua_State *L)
{
    uint8_t body[RADIO_BODY];
    uint8_t num;
    int len;

    if (radio_link == 0
        || !radio_take(RADIO_DATA, radio_link, NULL,
                       &num, body, &len))
    {
        lua_pushnil(L);
        return 1;
    }
    radio_put(RADIO_ACK, radio_link, num, NULL, 0);
    if (num == radio_in) {
        lua_pushnil(L);
        return 1;
    }
    radio_in = num;
    lua_pushlstring(L, (const char *)body, len);
    return 1;
}

#define LUA_RADIO_FUNCTIONS						\
    X(setTransmitPower, {						\
                    int power = luaL_checkint(L, 1);			\
                    int r = uBit.radio.setTransmitPower(power);		\
                    lua_pushboolean(L, r == MICROBIT_OK);		\
                    return 1;						\
                  })							\
    X(setFrequencyBand, {						\
                    int band = luaL_checkint(L, 1);			\
                    int r = uBit.radio.setFrequencyBand(band);		\
                    lua_pushboolean(L, r == MICROBIT_OK);		\
                    return 1;						\
                  })							\
    X(enable,     { int r = uBit.radio.enable();			\
                    lua_pushboolean(L, r == MICROBIT_OK);		\
                    return 1;						\
                  })							\
    X(disable,    { int r = uBit.radio.disable();			\
                    lua_pushboolean(L, r == MICROBIT_OK);		\
                    return 1;						\
                  })							\
    X(setGroup,   {							\
                    uint8_t group = (uint8_t)luaL_checkint(L, 1);	\
                    int r = uBit.radio.setGroup(group);			\
                    lua_pushboolean(L, r == MICROBIT_OK);		\
                    return 1;						\
                  })							\
    X(dataReady,  { int r = uBit.radio.dataReady();			\
                    lua_pushinteger(L, r);					\
                    return 1;						\
                  })							\
    X(recv,       { PacketBuffer r = uBit.radio.datagram.recv();	\
                    if(r == PacketBuffer::EmptyPacket) {		\
                      lua_pushnil(L);					\
                    } else {						\
                      lua_pushlstring(L,				\
                        (const char*)r.getBytes(), r.length());		\
                    }						 	\
                    return 1;						\
                  })							\
    X(send,       { size_t len;						\
                    const char *buffer = luaL_checklstring(L, 1, &len);	\
                    int r = uBit.radio.datagram.send(			\
                      PacketBuffer((uint8_t*)buffer, len));		\
                    lua_pushboolean(L, r == MICROBIT_OK);		\
                    return 1;						\
                  })							\
    X(connect,    { return radio_connect(L); })				\
    X(listen,     { return radio_listen(L); })				\
    X(tx,         { return radio_tx(L); })				\
    X(rx,         { return radio_rx(L); })				\
    X(answered,   { return radio_answered(L); })

#define LUA_RADIO_COUNT 13

#define LUA_CODAL_CONSTANTS \
    X(MICROBIT_ID_LOGO) \
    X(DEVICE_ID_BUTTON_A) \
    X(DEVICE_ID_BUTTON_B) \
    X(DEVICE_ID_BUTTON_AB) \
    X(DEVICE_ID_SERIAL) \
    X(DEVICE_ID_ACCELEROMETER) \
    X(DEVICE_ID_COMPASS) \
    X(DEVICE_ID_GESTURE) \
    X(DEVICE_ID_RADIO) \
    X(DEVICE_ID_RADIO_DATA_READY) \
    X(ACCELEROMETER_EVT_DATA_UPDATE) \
    X(ACCELEROMETER_EVT_TILT_UP) \
    X(ACCELEROMETER_EVT_TILT_DOWN) \
    X(ACCELEROMETER_EVT_TILT_LEFT) \
    X(ACCELEROMETER_EVT_TILT_RIGHT) \
    X(ACCELEROMETER_EVT_FACE_UP) \
    X(ACCELEROMETER_EVT_FACE_DOWN) \
    X(ACCELEROMETER_EVT_FREEFALL) \
    X(ACCELEROMETER_EVT_3G) \
    X(ACCELEROMETER_EVT_6G) \
    X(ACCELEROMETER_EVT_8G) \
    X(ACCELEROMETER_EVT_SHAKE) \
    X(COMPASS_EVT_DATA_UPDATE) \
    X(COMPASS_EVT_CONFIG_NEEDED) \
    X(COMPASS_EVT_CALIBRATE) \
    X(COMPASS_EVT_CALIBRATION_NEEDED) \
    X(DEVICE_BUTTON_EVT_DOWN) \
    X(DEVICE_BUTTON_EVT_UP) \
    X(DEVICE_BUTTON_EVT_CLICK) \
    X(DEVICE_BUTTON_EVT_LONG_CLICK) \
    X(DEVICE_BUTTON_EVT_HOLD) \
    X(DEVICE_BUTTON_EVT_DOUBLE_CLICK) \
    X(MICROBIT_RADIO_EVT_DATAGRAM) \
    X(CODAL_SERIAL_EVT_HEAD_MATCH)

#if CONFIG_ENABLED(DEVICE_BLE)
#define LUA_BLE_CONSTANTS \
    X(MICROBIT_ID_BLE) \
    X(MICROBIT_ID_BLE_UART) \
    X(MICROBIT_BLE_EVT_CONNECTED) \
    X(MICROBIT_BLE_EVT_DISCONNECTED) \
    X(MICROBIT_UART_S_EVT_DELIM_MATCH) \
    X(MICROBIT_UART_S_EVT_HEAD_MATCH) \
    X(MICROBIT_UART_S_EVT_RX_FULL)
#endif

#define X(name, body) static int l_##name(lua_State *L) body
LUA_MICROBIT_FUNCTIONS
LUA_DISPLAY_FUNCTIONS
LUA_ACCELEROMETER_FUNCTIONS
LUA_AUDIO_FUNCTIONS
LUA_IO_FUNCTIONS
LUA_SERIAL_FUNCTIONS
#undef X

#if CONFIG_ENABLED(DEVICE_BLE)
#define X(name, body) static int l_ble_uart_##name(lua_State *L) body
LUA_BLE_FUNCTIONS
#undef X
#endif

#define X(name, body) static int l_compass_##name(lua_State *L) body
LUA_COMPASS_FUNCTIONS
#undef X

#define X(name, body) static int l_radio_##name(lua_State *L) body
LUA_RADIO_FUNCTIONS
#undef X

#define X(name, body) static int l_i2c_##name(lua_State *L) body
LUA_I2C_FUNCTIONS
#undef X

#define X(name, body) {#name, l_##name},
static const luaL_Reg l_microbit[] = {
    LUA_MICROBIT_FUNCTIONS
    {NULL, NULL}
};
static const luaL_Reg l_display[] = {
    LUA_DISPLAY_FUNCTIONS
    {NULL, NULL}
};
static const luaL_Reg l_accelerometer[] = {
    LUA_ACCELEROMETER_FUNCTIONS
    {NULL, NULL}
};
static const luaL_Reg l_audio[] = {
    LUA_AUDIO_FUNCTIONS
    {NULL, NULL}
};
static const luaL_Reg l_io[] = {
    LUA_IO_FUNCTIONS
    {NULL, NULL}
};
static const luaL_Reg l_serial[] = {
    LUA_SERIAL_FUNCTIONS
    {NULL, NULL}
};
#if CONFIG_ENABLED(DEVICE_BLE)
#define X(name, body) {#name, l_ble_uart_##name},
static const luaL_Reg l_ble_uart[] = {
    LUA_BLE_FUNCTIONS
    {NULL, NULL}
};
#undef X
#endif
#undef X

#define X(name, body) {#name, l_compass_##name},
static const luaL_Reg l_compass[] = {
    LUA_COMPASS_FUNCTIONS
    {NULL, NULL}
};
#undef X

#define X(name, body) {#name, l_radio_##name},
static const luaL_Reg l_radio[] = {
    LUA_RADIO_FUNCTIONS
    {NULL, NULL}
};
#undef X

#define X(name, body) {#name, l_i2c_##name},
static const luaL_Reg l_i2c[] = {
    LUA_I2C_FUNCTIONS
    {NULL, NULL}
};
#undef X

void register_lua_api(lua_State *L) {
  luaL_register(L, "microbit", l_microbit);
  lua_createtable(L, 0, LUA_DISPLAY_COUNT);
  luaL_register(L, NULL, l_display);
  lua_setfield(L, -2, "display");
  lua_createtable(L, 0, LUA_ACCELEROMETER_COUNT);
  luaL_register(L, NULL, l_accelerometer);
  lua_setfield(L, -2, "accelerometer");
  lua_createtable(L,0, LUA_COMPASS_COUNT);
  luaL_register(L, NULL, l_compass);
  lua_setfield(L, -2, "compass");
  lua_createtable(L, 0, LUA_AUDIO_COUNT);
  luaL_register(L, NULL, l_audio);
  lua_setfield(L, -2, "audio");
  lua_createtable(L, 0, LUA_IO_COUNT);
  luaL_register(L, NULL, l_io);
  lua_setfield(L, -2, "io");
  lua_createtable(L, 0, LUA_SERIAL_COUNT);
  luaL_register(L, NULL, l_serial);
  lua_setfield(L, -2, "serial");
  lua_createtable(L, 0, LUA_RADIO_COUNT);
  luaL_register(L, NULL, l_radio);
  lua_setfield(L, -2, "radio");
  lua_createtable(L, 0, LUA_I2C_COUNT);
  luaL_register(L, NULL, l_i2c);
  lua_setfield(L, -2, "i2c");
#if CONFIG_ENABLED(DEVICE_BLE)
  lua_createtable(L, 0, 1);                 // microbit.ble
  lua_createtable(L, 0, LUA_BLE_COUNT);
  luaL_register(L, NULL, l_ble_uart);
  lua_setfield(L, -2, "uart");
  lua_setfield(L, -2, "ble");
#endif

  // TODO: avoid repeated reallocations
  // CODAL event constants — use the exact C macro names so that reading
  // CODAL source/docs is sufficient to write Lua code (no extra translation).
  // Capture the absolute stack index of the microbit table because leftover
  // entries from luaopen_* (base, table, string, math) sit below it — using
  // a relative index like -2 would target the wrong table.
  int microbit_idx = lua_gettop(L);
#define X(n) lua_pushinteger(L, n); lua_setfield(L, microbit_idx, #n);
  LUA_CODAL_CONSTANTS
#undef X
#if CONFIG_ENABLED(DEVICE_BLE)
#define X(n) lua_pushinteger(L, n); lua_setfield(L, microbit_idx, #n);
  LUA_BLE_CONSTANTS
#undef X
#endif
}

static lua_State *lua_state;

// Runs in a dedicated fiber (spawned by on_codal_event).  Has a full
// fiber stack so lua_pcall won't overflow the idle or interrupt stacks.
static void lua_event_handler_fiber(void *arg) {
  codal::Event e = *(codal::Event *)arg;
  delete (codal::Event *)arg;

  lua_getglobal(lua_state, "on_event");
  if (!lua_isfunction(lua_state, -1)) {
    lua_pop(lua_state, 1);
    return;
  }
  lua_pushinteger(lua_state, e.source);
  lua_pushinteger(lua_state, e.value);
  lua_pushinteger(lua_state, e.timestamp);
  if (lua_pcall(lua_state, 3, 0, 0) != LUA_OK) {
    const char *err = lua_tostring(lua_state, -1);
    if (err) {
      uBit.display.scroll(err);
    }
    lua_pop(lua_state, 1);
  }
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
static void on_codal_event(codal::Event e, void *arg) {
  (void)arg;
  if (e.source == DEVICE_ID_SCHEDULER || e.source == DEVICE_ID_COMPONENT ||
      e.source == DEVICE_ID_NOTIFY || e.source == DEVICE_ID_NOTIFY_ONE)
    return;
  codal::Event *copy = new codal::Event(e);
  create_fiber(lua_event_handler_fiber, copy);
}

void register_lua_event_listener(lua_State *L) {
  lua_state = L;
  uBit.messageBus.listen(DEVICE_ID_ANY, DEVICE_EVT_ANY,
                         on_codal_event, NULL,
                         MESSAGE_BUS_LISTENER_IMMEDIATE);
}
