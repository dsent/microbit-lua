// Lua's math in single precision. With LUA_NUMBER_IS_FLOAT, Lua's numbers
// are floats, but lmathlib, and lvm and lcode through luai_numpow and
// luai_nummod, call the double functions. The nRF52833's FPU does single
// precision only, so each of those ran in software, and libm carried both
// families. This maps each call to its float form. A result can differ from
// the double one rounded to float by up to 3 ulp, as newlib's sinhf and
// coshf do near where they overflow; at the top of the float range a result
// can be the largest finite float where the double one rounded to infinity
// (tests/lua-float-math-tests.sh measures it). % takes fmodf's exact
// remainder (source/luaconf-nummod.patch), and math.random scales its draw
// in 64-bit integers (source/lmathlib-random.patch). math.rad and math.deg
// still multiply and divide by a double constant.
// Constant folding in lcode calls the same functions as the VM, so compiled
// and run-time results agree, but for the sign of a zero, which Lua 5.1's
// constant table does not keep apart.
//
// CMakeLists.txt forces this header into Lua's own sources only, after
// codal_extra_definitions.h, so CODAL's and the firmware's own calls keep
// their doubles: double sqrt and atan2 stay in the image for CODAL's
// compass and accelerometer and for compass-track.
#ifndef LUA_FLOAT_MATH_H
#define LUA_FLOAT_MATH_H

#if defined(LUA_NUMBER_IS_FLOAT) && LUA_NUMBER_IS_FLOAT

#include <math.h>

#define fabs(x)     fabsf(x)
#define sin(x)      sinf(x)
#define sinh(x)     sinhf(x)
#define cos(x)      cosf(x)
#define cosh(x)     coshf(x)
#define tan(x)      tanf(x)
#define tanh(x)     tanhf(x)
#define asin(x)     asinf(x)
#define acos(x)     acosf(x)
#define atan(x)     atanf(x)
#define atan2(y, x) atan2f(y, x)
#define ceil(x)     ceilf(x)
#define floor(x)    floorf(x)
#define fmod(x, y)  fmodf(x, y)
#define sqrt(x)     sqrtf(x)
#define pow(x, y)   powf(x, y)
#define log(x)      logf(x)
#define log10(x)    log10f(x)
#define exp(x)      expf(x)
#define frexp(x, e) frexpf(x, e)
#define ldexp(x, e) ldexpf(x, e)
// modf stays double: math.modf passes a double for the integer part.

#endif

#endif
