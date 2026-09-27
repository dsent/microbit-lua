// -*- mode: c; indent-tabs-mode: nil; -*-
//
// Numbers as text, without printf's float support.
//
// The firmware links newlib-nano without float printf to save flash, so
// sprintf writes nothing for %e, %f and %g, and Lua turned 2.5 into "".
// luaconf.h sends lua_number2str here, and string.format sends its %e, %f
// and %g items here.
//
// The digits come from double arithmetic, which carries 14 significant
// digits, as Lua's own %.14g does: any further ones print as 0, and in rare
// cases the 14th rounds the other way from printf. A halfway case rounds to
// even, as printf does.

#include <math.h>
#include <string.h>

#define MAX_DIGITS 14

// 10^n; exact up to 10^22.
static double ten(int n) {
    double p = 1;
    while (n-- > 0)
        p *= 10;
    return p;
}

// x * 10^n
static double scale(double x, int n) {
    return n >= 0 ? x * ten(n) : x / ten(-n);
}

// The decimal exponent of x > 0: 10^e <= x < 10^(e+1).
static int exponent(double x) {
    int e = (int)floor(log10(x));
    if (scale(x, -e) >= 10)
        e++;
    else if (scale(x, -e) < 1)
        e--;
    return e;
}

// m rounded to an integer, halfway to even.
static unsigned long long round_even(double m) {
    double f = floor(m);
    unsigned long long r = (unsigned long long)f;
    double rest = m - f;
    if (rest > 0.5 || (rest == 0.5 && (r & 1)))
        r++;
    return r;
}

// x > 0 rounded to d significant digits (1 to MAX_DIGITS): the digits as
// an integer, and in *e the decimal exponent of the first one.
static unsigned long long round_digits(double x, int d, int *e) {
    int k = exponent(x);
    unsigned long long r = round_even(scale(x, d - 1 - k));
    if (r >= (unsigned long long)ten(d)) {  // 9.96 became 10.0
        r /= 10;
        k++;
    }
    *e = k;
    return r;
}

// Writes the d digits of r, then z zeros; returns the end.
static char *put_digits(char *s, unsigned long long r, int d, int z) {
    char *end = s + d;
    for (char *p = end; p > s; r /= 10)
        *--p = (char)('0' + r % 10);
    while (z-- > 0)
        *end++ = '0';
    return end;
}

// x >= 0 with p decimals; returns the end.
static char *fixed(char *s, double x, int p, int alt) {
    // x * 10^p rounded, as the d digits of r and then z zeros
    unsigned long long r = 0;
    int d = 1, z = 0, len = 1;
    if (x > 0) {
        int e = exponent(x);
        int want = e + 1 + p;
        if (want > 0) {
            d = want < MAX_DIGITS ? want : MAX_DIGITS;
            r = round_digits(x, d, &e);
            len = e + 1 + p;
            z = len - d;
        } else {
            r = round_even(scale(x, p));
        }
    }
    int whole = len - p;
    if (whole > 0) {
        char *end = put_digits(s, r, d, z);
        if (p == 0 && !alt)
            return end;
        memmove(s + whole + 1, s + whole, (size_t)p);
        s[whole] = '.';
        return end + 1;
    }
    *s++ = '0';
    *s++ = '.';
    while (whole++ < 0)
        *s++ = '0';
    return put_digits(s, r, d, z);
}

// The mantissa of x >= 0 in scientific notation with p decimals, and in *e
// its exponent; returns the end.
static char *mantissa(char *s, double x, int p, int alt, int *e) {
    unsigned long long r = 0;
    int d = p + 1, z = 0;
    *e = 0;
    if (x > 0) {
        if (d > MAX_DIGITS) {
            z = d - MAX_DIGITS;
            d = MAX_DIGITS;
        }
        r = round_digits(x, d, e);
    }
    char *end = put_digits(s + 1, r, d, z);
    s[0] = s[1];
    if (p == 0 && !alt)
        return end - 1;
    s[1] = '.';
    return end;
}

static char *put_exponent(char *s, int e, char mark) {
    *s++ = mark;
    *s++ = e < 0 ? '-' : '+';
    if (e < 0)
        e = -e;
    if (e >= 100)
        *s++ = (char)('0' + e / 100);
    *s++ = (char)('0' + e / 10 % 10);
    *s++ = (char)('0' + e % 10);
    return s;
}

// Drops the trailing zeros of the decimals, and a point left bare.
static char *strip(char *s, char *end) {
    if (!memchr(s, '.', (size_t)(end - s)))
        return end;
    while (end[-1] == '0')
        end--;
    if (end[-1] == '.')
        end--;
    return end;
}

// x >= 0 as conversion c (e, E, f, g or G); returns the end.
static char *body(char *s, double x, char c, int p, int alt) {
    int upper = c == 'E' || c == 'G';
    if (isnan(x) || isinf(x)) {
        const char *word = isnan(x) ? (upper ? "NAN" : "nan")
                                    : (upper ? "INF" : "inf");
        memcpy(s, word, 3);
        return s + 3;
    }
    char mark = upper ? 'E' : 'e';
    int e = 0;
    if (c == 'f')
        return fixed(s, x, p, alt);
    if (c == 'e' || c == 'E') {
        char *end = mantissa(s, x, p, alt, &e);
        return put_exponent(end, e, mark);
    }
    // g: the shorter of the two, by the exponent after rounding
    if (p == 0)
        p = 1;
    if (x > 0)
        round_digits(x, p < MAX_DIGITS ? p : MAX_DIGITS, &e);
    if (e < -4 || e >= p) {
        char *end = mantissa(s, x, p - 1, alt, &e);
        return put_exponent(alt ? end : strip(s, end), e, mark);
    }
    char *end = fixed(s, x, p - 1 - e, alt);
    return alt ? end : strip(s, end);
}

// sprintf(s, form, x) for one %e, %E, %f, %g or %G item with its flags,
// width and precision, as string.format hands them over.
int luai_numformat(char *s, const char *form, double x) {
    int left = 0, plus = 0, space = 0, alt = 0, zero = 0;
    int width = 0, p = 6;
    const char *f = form + 1;
    for (;; f++) {
        if (*f == '-')
            left = 1;
        else if (*f == '+')
            plus = 1;
        else if (*f == ' ')
            space = 1;
        else if (*f == '#')
            alt = 1;
        else if (*f == '0')
            zero = 1;
        else
            break;
    }
    while (*f >= '0' && *f <= '9')
        width = width * 10 + (*f++ - '0');
    if (*f == '.') {
        p = 0;
        while (*++f >= '0' && *f <= '9')
            p = p * 10 + (*f - '0');
    }
    char *end = s;
    if (signbit(x))
        *end++ = '-';
    else if (plus)
        *end++ = '+';
    else if (space)
        *end++ = ' ';
    int sign = (int)(end - s);
    end = body(end, fabs(x), *f, p, alt);
    int len = (int)(end - s);
    if (len < width) {
        int pad = width - len;
        if (left) {
            memset(end, ' ', (size_t)pad);
        } else if (zero && isfinite(x)) {
            memmove(s + sign + pad, s + sign, (size_t)(len - sign));
            memset(s + sign, '0', (size_t)pad);
        } else {
            memmove(s + pad, s, (size_t)len);
            memset(s, ' ', (size_t)pad);
        }
        len = width;
    }
    s[len] = '\0';
    return len;
}

// A number as tostring and .. show it: a whole number that fits in 32 bits
// in full, any other with 7 significant digits.
int luai_number2str(char *s, double x) {
    if (x == floor(x) && fabs(x) < 2147483648.0)
        return luai_numformat(s, "%.0f", x);
    return luai_numformat(s, "%.7g", x);
}
