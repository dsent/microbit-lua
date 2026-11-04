// -*- mode: c; indent-tabs-mode: nil; -*-
//
// Host test for source/lua-number.c: string.format's %e, %f and %g items
// for chosen numbers, against the host's printf. Run by lua-number-tests.sh.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int luai_numformat(char *s, const char *form, double n);

static int failures = 0;
static int checks = 0;

// Lua's numbers here are floats; string.format hands them over as double.
static const float values[] = {
    0,     -0.0f,  1,       -7,     12,     100,   0.5f,     1.5f,
    2.5f,  -2.5f,  0.125f,  0.1f,   0.25f,  -0.75f, 1.0f / 3, 2.0f / 3,
    3.14159f, 9.95f, 99.5f, 0.05f,  1e-5f,  123456, 16777216, 1e10f,
    3.4e38f, 1e-38f, INFINITY, -INFINITY, NAN,
};

// The shapes lessons use, then the flags and conversions C defines.
static const char *forms[] = {
    "%.1f", "%.2f", "%5.1f", "%f",  "%.0f", "%.3f", "%-8.2f", "%05.1f",
    "%+.1f", "% .2f", "%e",  "%.2e", "%E",  "%g",   "%.3g",   "%G",
    "%.10f",
};

// Past 14 significant digits the formatter writes zeros where printf
// writes the binary value's digits: such items are left out.
static int within_digits(const char *form, double x) {
    const char *f = form + 1;
    while (strchr("-+ #0123456789", *f))
        f++;
    int p = 6;
    if (*f == '.')
        for (p = atoi(++f); *f >= '0' && *f <= '9';)
            f++;
    if (*f == 'f' && x != 0 && isfinite(x))
        return floor(log10(fabs(x))) + 1 + p <= 14;
    return *f == 'f' || p <= 13;
}

static void compare(const char *form, double x) {
    char got[512], want[512];
    luai_numformat(got, form, x);
    snprintf(want, sizeof want, form, x);
    checks++;
    if (strcmp(got, want) != 0) {
        failures++;
        printf("FAIL %s of %.9g: got \"%s\", want \"%s\"\n", form, x, got,
               want);
    }
}

// Nine significant digits read back to the same float.
static void round_trip(float x) {
    char text[64];
    luai_numformat(text, "%.9g", x);
    checks++;
    if (strtof(text, NULL) != x) {
        failures++;
        printf("FAIL %.9g wrote \"%s\", which reads back as %.9g\n", x, text,
               strtof(text, NULL));
    }
}

int main(void) {
    size_t nforms = sizeof forms / sizeof forms[0];
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++)
        for (size_t j = 0; j < nforms; j++)
            if (within_digits(forms[j], values[i]))
                compare(forms[j], values[i]);
    round_trip(0.1f);
    round_trip(1.0f / 3);
    round_trip(3.14159f);
    round_trip(1e-5f);
    round_trip(16777215);
    round_trip(3.4e38f);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
