// rand and srand for tests/lua-float-math-tests.sh, in place of the host's:
// newlib's generator, which the firmware links, a 64-bit linear
// congruential one that answers its top 31 bits. Seeds 31 to 36 pin every
// answer to one draw, so that a test reaches the draw it names (see
// tests/lua-float-math-test.lua); any other seed lets the generator run.

#include <stdlib.h>

static unsigned long long next = 1;
static int pinned = -1;

static const int pins[] = {
    RAND_MAX - 1, // 31: rounded to 1 in float
    64,           // 32: a small draw a wide range must tell apart
    1789569707,   // 33: random(6) is 6 in exact arithmetic, 5 through a float
    1789569664,   // 34: random(6) is 5, 6 if a product rounds up in float
    2147483392,   // 35: random(-2147483392, 255) is 0, 1 if it rounds
    RAND_MAX,     // 36: the top draw, which random(-2147483392, 255) takes to 255
};

void srand(unsigned int seed)
{
    next = seed;
    pinned = seed >= 31 && seed <= 36 ? pins[seed - 31] : -1;
}

int rand(void)
{
    if (pinned >= 0)
        return pinned;
    next = next * 6364136223846793005ULL + 1;
    return (int)((next >> 32) & RAND_MAX);
}
