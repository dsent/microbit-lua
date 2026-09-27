// -*- mode: c; indent-tabs-mode: nil; -*-
//
// Host test for source/lua-number.c: what tostring shows for chosen
// numbers, and string.format's %e, %f and %g items against the host's
// printf for floats across the whole range. Run by lua-number-tests.sh.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int luai_number2str(char *s, double n);
int luai_numformat(char *s, const char *form, double n);

static int failures = 0;
static int checks = 0;

static void expect_text(float x, const char *want) {
    char got[64];
    luai_number2str(got, x);
    checks++;
    if (strcmp(got, want) != 0) {
        failures++;
        printf("FAIL tostring(%.9g): got \"%s\", want \"%s\"\n", x, got, want);
    }
}

// Significant digits printf needs for x in this item: past 14 the
// formatter prints zeros where printf prints the binary value's digits.
static int digits_needed(double x, char c, int p) {
    if (c == 'e' || c == 'E')
        return p + 1;
    if (c == 'g' || c == 'G')
        return p == 0 ? 1 : p;
    if (x == 0 || !isfinite(x))
        return 1;
    return (int)floor(log10(fabs(x))) + 1 + p;
}

static void compare(const char *form, double x) {
    const char *f = form + 1;
    while (strchr("-+ #0123456789", *f))
        f++;
    int p = 6;
    if (*f == '.') {
        p = 0;
        while (*++f >= '0' && *f <= '9')
            p = p * 10 + (*f - '0');
    }
    if (digits_needed(x, *f, p) > 14)
        return;
    char got[512], want[512];
    luai_numformat(got, form, x);
    snprintf(want, sizeof want, form, x);
    // glibc's %#g drops the zeros when rounding reaches the next power
    // of ten (999999.5 gives "1.e+06"); C wants "1.00000e+06"
    if (strchr(form, '#') && (*f == 'g' || *f == 'G')) {
        int n = 0;
        for (const char *c = want; *c && *c != 'e' && *c != 'E'; c++)
            n += *c >= '0' && *c <= '9';
        if (n < (p == 0 ? 1 : p))
            return;
    }
    checks++;
    if (strcmp(got, want) != 0) {
        failures++;
        if (failures <= 20)
            printf("FAIL %s of %.9g: got \"%s\", want \"%s\"\n", form, x, got,
                   want);
    }
}

static const char *forms[] = {
    "%g",     "%e",    "%f",      "%.0f",    "%.1f",     "%.2f",
    "%.3f",   "%.6f",  "%.0e",    "%.3e",    "%.7g",     "%.1g",
    "%.0g",   "%.9g",  "%.14e",   "%G",      "%E",       "%10.3f",
    "%-10.3f", "%+.2f", "% .2g",  "%#.0f",   "%#g",      "%#.3g",
    "%#.0e",  "%08.2f", "%-8.2e", "%+012.4e", "%5g",     "%.15g",
    "%3.1f",  "%020.10f",
};

static uint32_t state = 2463534242u;

static uint32_t next(void) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

int main(void) {
    expect_text(0, "0");
    expect_text(-0.0f, "-0");
    expect_text(1, "1");
    expect_text(-7, "-7");
    expect_text(12, "12");
    expect_text(2.5f, "2.5");
    expect_text(-2.5f, "-2.5");
    expect_text(0.1f, "0.1");
    expect_text(-0.5f, "-0.5");
    expect_text(1.0f / 3, "0.3333333");
    expect_text(3.14159265f, "3.141593");
    expect_text(0.1f * 3, "0.3");
    expect_text(6.6f, "6.6");
    expect_text(1e-7f, "1e-07");
    expect_text(16777216, "16777216");
    expect_text(123456789, "123456792");
    expect_text(2147483520.0f, "2147483520");
    expect_text(2147483648.0f, "2.147484e+09");
    expect_text(1e10f, "1e+10");
    expect_text(3.4e38f, "3.4e+38");
    expect_text(1e-45f, "1.401298e-45");
    expect_text(INFINITY, "inf");
    expect_text(-INFINITY, "-inf");
    expect_text(NAN, "nan");

    static const double chosen[] = {
        0,     -0.0,   0.5,    1.5,    2.5,   -2.5,  0.125, 0.375, 9.5,
        99.5,  999999.5, 1e-5, 0.05,   0.001, 1e-4, 9.9999, 99.999, 1e6,
        1e7,   123456, 1234567, 12345678, 1e15, 1e38, 1e-38, INFINITY,
        -INFINITY, NAN,
    };
    size_t nforms = sizeof forms / sizeof forms[0];
    for (size_t i = 0; i < sizeof chosen / sizeof chosen[0]; i++)
        for (size_t j = 0; j < nforms; j++)
            compare(forms[j], (float)chosen[i]);
    for (int i = 0; i < 200000; i++) {
        uint32_t bits = next();
        float x;
        memcpy(&x, &bits, sizeof x);
        if (!isfinite(x))
            continue;
        compare(forms[i % nforms], x);
    }
    // everyday numbers: up to 8 digits, with the point anywhere
    for (int i = 0; i < 200000; i++) {
        double x = next() % 100000000;
        for (int k = next() % 12; k > 0; k--)
            x /= 10;
        compare(forms[i % nforms], (float)(i % 2 ? -x : x));
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
