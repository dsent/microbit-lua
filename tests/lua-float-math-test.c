// How far Lua's single-precision math (source/lua-float-math.h) lands from
// the double functions' results rounded to float. For each function the
// header maps, it samples a range of inputs, out to where the result
// overflows, and reports the largest difference in units in the last place
// (ulp); infinity counts as one ulp past the largest finite float. It fails
// when one exceeds the bound given as the first argument, or when the two
// disagree on a NaN. luai_nummod and luai_numpow, the VM's % and ^, are
// measured as luaconf.h writes them, with source/luaconf-nummod.patch: %
// is measured in ulp of its larger operand or result, as near a result of
// zero any rounding of the product shows as a large distance in ulp.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
static long bound;

// Distance in ulp between two floats of any sign, in 64 bits, as it can
// pass 2^31; capped where a 32-bit long ends
static long ulps(float a, float b) {
  int32_t ia, ib;
  int64_t d;
  memcpy(&ia, &a, 4);
  memcpy(&ib, &b, 4);
  if (ia < 0) ia = (int32_t)(0x80000000u - (uint32_t)ia);
  if (ib < 0) ib = (int32_t)(0x80000000u - (uint32_t)ib);
  d = (int64_t)ia - (int64_t)ib;
  if (d < 0) d = -d;
  return d > 0x7fffffff ? 0x7fffffffL : (long)d;
}

typedef struct {
  const char *name;
  long worst;
  double worst_x, worst_y;
  int mismatch;
} result;

static void begin(result *r, const char *name) {
  memset(r, 0, sizeof *r);
  r->name = name;
}

// The size of one ulp at the magnitude of v
static double ulp_of(float v) {
  v = fabsf(v);
  return (double)nextafterf(v, INFINITY) - (double)v;
}

static void take(result *r, float got, double want_d, double x, double y,
                 int by_operand) {
  float want = (float)want_d;
  long d;
  if (isnan(got) || isnan(want)) {
    if (!(isnan(got) && isnan(want))) r->mismatch++;
    return;
  }
  if (by_operand && (isinf(got) || isinf(want))) {
    if (got != want) r->mismatch++;
    return;
  }
  if (by_operand) {
    float m = fmaxf(fmaxf(fabsf((float)x), fabsf(got)), fabsf(want));
    d = (long)ceil(fabs((double)got - (double)want) / ulp_of(m));
  } else {
    d = ulps(got, want);
  }
  if (d > r->worst) {
    r->worst = d;
    r->worst_x = x;
    r->worst_y = y;
  }
}

// A float's bits, for a printf without float support (newlib's nano)
static unsigned long bits(double v) {
  float f = (float)v;
  uint32_t u;
  memcpy(&u, &f, 4);
  return (unsigned long)u;
}

static void end(result *r) {
  checks++;
  if (r->worst > bound || r->mismatch) failures++;
  printf("%s %-8s max %ld ulp at (%.9g, %.9g) [%08lx %08lx]",
         r->worst > bound || r->mismatch ? "FAIL" : "ok  ", r->name, r->worst,
         r->worst_x, r->worst_y, bits(r->worst_x), bits(r->worst_y));
  if (r->mismatch)
    printf(", %d NaN or infinity mismatches", r->mismatch);
  printf("\n");
}

#define STEPS 100000

// The value at step i of STEPS from lo to hi, as a float
static float at(double lo, double hi, int i) {
  return (float)(lo + (hi - lo) * i / (STEPS - 1));
}

// The value at step i of STEPS from lo to hi, both above 0, in equal
// ratios, so that every power of ten gets its share
static float at_ratio(double lo, double hi, int i) {
  return (float)(lo * pow(hi / lo, (double)i / (STEPS - 1)));
}

typedef struct {
  const char *name;
  float (*f)(float);
  double (*d)(double);
  double lo, hi;
  int by_ratio;
} unary;

static void one(const unary *u) {
  result r;
  int i;
  begin(&r, u->name);
  for (i = 0; i < STEPS; i++) {
    float x = u->by_ratio ? at_ratio(u->lo, u->hi, i) : at(u->lo, u->hi, i);
    take(&r, u->f(x), u->d(x), x, 0, 0);
  }
  end(&r);
}

// Every float from lo to hi, where a function nears overflow and a grid
// steps over the inputs that matter
static void every(const unary *u) {
  result r;
  float x;
  begin(&r, u->name);
  for (x = (float)u->lo; x <= (float)u->hi; x = nextafterf(x, INFINITY))
    take(&r, u->f(x), u->d(x), x, 0, 0);
  end(&r);
}

typedef struct {
  const char *name;
  float (*f)(float, float);
  double (*d)(double, double);
  double xlo, xhi, ylo, yhi;
  int by_operand;
} binary;

#define GRID 300

static void two(const binary *b) {
  result r;
  int i, j;
  begin(&r, b->name);
  for (i = 0; i < GRID; i++)
    for (j = 0; j < GRID; j++) {
      float x = (float)(b->xlo + (b->xhi - b->xlo) * i / (GRID - 1));
      float y = (float)(b->ylo + (b->yhi - b->ylo) * j / (GRID - 1));
      take(&r, b->f(x, y), b->d(x, y), x, y, b->by_operand);
    }
  end(&r);
}

// luai_nummod with float numbers: after the header, the floor in float
// and the product and difference in double (source/luaconf-nummod.patch);
// before it, the quotient in float and the rest in double, through floor
static float mod_f(float a, float b) {
  return (float)(a - floorf(a / b) * (double)b);
}
static double mod_d(double a, double b) {
  float q = (float)a / (float)b;
  return a - floor(q) * b;
}

// ^ with a whole exponent, as in 2^10
static float powi_f(float x, float y) { return powf(x, roundf(y)); }
static double powi_d(double x, double y) { return pow(x, round(y)); }

static float ldexp_f(float x, float e) { return ldexpf(x, (int)e); }
static double ldexp_d(double x, double e) { return ldexp(x, (int)e); }

static float frexp_f(float x) { int e; return frexpf(x, &e); }
static double frexp_d(double x) { int e; return frexp(x, &e); }

int main(int argc, char **argv) {
  static const unary unaries[] = {
    { "fabs",  fabsf,  fabs,  -1e6, 1e6, 0 },
    { "sin",   sinf,   sin,   -100, 100, 0 },
    { "sinh",  sinhf,  sinh,  -90, 90, 0 },
    { "cos",   cosf,   cos,   -100, 100, 0 },
    { "cosh",  coshf,  cosh,  -90, 90, 0 },
    { "tan",   tanf,   tan,   -100, 100, 0 },
    { "tanh",  tanhf,  tanh,  -12, 12, 0 },
    { "asin",  asinf,  asin,  -1, 1, 0 },
    { "acos",  acosf,  acos,  -1, 1, 0 },
    { "atan",  atanf,  atan,  -1000, 1000, 0 },
    { "ceil",  ceilf,  ceil,  -1e4, 1e4, 0 },
    { "floor", floorf, floor, -1e4, 1e4, 0 },
    { "sqrt",  sqrtf,  sqrt,  1e-30, 3e38, 1 },
    { "log",   logf,   log,   1e-30, 3e38, 1 },
    { "log ~1", logf,  log,   0.01, 10, 0 },
    { "log10", log10f, log10, 1e-30, 3e38, 1 },
    { "log10 ~1", log10f, log10, 0.01, 10, 0 },
    { "exp",   expf,   exp,   -104, 90, 0 },
    { "frexp", frexp_f, frexp_d, -1e6, 1e6, 0 },
  };
  static const unary tops[] = {
    { "sinh top", sinhf, sinh, 88, 89.5, 0 },
    { "cosh top", coshf, cosh, 88, 89.5, 0 },
    { "exp top",  expf,  exp,  87.5, 89, 0 },
  };
  static const binary binaries[] = {
    { "atan2", atan2f, atan2, -100, 100, -100, 100, 0 },
    { "fmod",  fmodf,  fmod,  -1000, 1000, 0.5, 50, 0 },
    { "pow",   powf,   pow,   0.01, 100, -20, 20, 0 },
    { "pow big", powf, pow,   1e9, 1e10, 3.5, 4.5, 0 },
    { "%",     mod_f,  mod_d, -1000, 1000, 0.5, 50, 1 },
    { "^",     powf,   pow,   0.01, 10, -6, 6, 0 },
    { "^ whole", powi_f, powi_d, -10, 10, -6, 6, 0 },
    { "ldexp", ldexp_f, ldexp_d, -1000, 1000, -20, 20, 0 },
  };
  size_t i;
  bound = argc > 1 ? atol(argv[1]) : 1;
  for (i = 0; i < sizeof unaries / sizeof unaries[0]; i++)
    one(&unaries[i]);
  for (i = 0; i < sizeof tops / sizeof tops[0]; i++)
    every(&tops[i]);
  for (i = 0; i < sizeof binaries / sizeof binaries[0]; i++)
    two(&binaries[i]);
  printf("%d functions within %ld ulp, %d failed\n", checks, bound, failures);
  return failures != 0;
}
