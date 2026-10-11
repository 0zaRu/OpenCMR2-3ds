// FTOI (game/FloatToInt.h) in a CMR2_QIFIST unit against the x87 FISTP that
// MSVC6 /QIfist emits: 16-, 32- and 64-bit stores under the game's control
// word 0x027f, for halves, negatives, large values, out of range and NaN.
#define CMR2_QIFIST
#include "FloatToInt.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <vector>

static unsigned short SetControlWord(unsigned short cw)
{
    unsigned short old;
    __asm__ volatile("fnstcw %0" : "=m"(old));
    __asm__ volatile("fldcw %0" : : "m"(cw));
    return old;
}

static int64_t Fistp64(double x)
{
    int64_t r;
    __asm__ volatile("fistpll %0" : "=m"(r) : "t"(x) : "st");
    return r;
}

static int32_t Fistp32(double x)
{
    int32_t r;
    __asm__ volatile("fistpl %0" : "=m"(r) : "t"(x) : "st");
    return r;
}

int main()
{
    unsigned short saved = SetControlWord(0x027f);
    std::vector<double> values = {0.0, -0.0, 0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 0.49999999999999994, 1e9 + 0.5,
                                  2147483647.0, 2147483647.4, 2147483647.5, 2147483648.0, -2147483648.0,
                                  -2147483648.5, -2147483649.0, 4294967295.0, 4294967296.0, 9.2e18, -9.3e18, 1e300,
                                  -1e300, std::numeric_limits<double>::infinity(),
                                  -std::numeric_limits<double>::infinity(), std::nan("")};
    std::mt19937_64 rng(1);
    std::uniform_real_distribution<double> wide(-3e9, 3e9), narrow(-70000.0, 70000.0);
    for (int i = 0; i < 200000; i++) {
        values.push_back(wide(rng));
        values.push_back(std::round(narrow(rng) * 2) / 2);  // exact halves
        values.push_back((float)narrow(rng) * 65536.0f);   // a float operand, as the game converts
    }
    int failures = 0;
    for (double x : values) {
        struct {
            const char *what;
            long long got, want;
        } checks[] = {
            {"int", FTOI(int, x), Fistp32(x)},
            {"__int64", FTOI(__int64, x), Fistp64(x)},
            {"(int)__int64", (int)FTOI(__int64, x), (int)Fistp64(x)},
            {"unsigned", FTOI(unsigned int, x), (unsigned int)Fistp64(x)},
            {"short", FTOI(short, x), (short)Fistp32(x)},
        };
        for (auto &c : checks) {
            if (c.got != c.want && failures++ < 20)
                fprintf(stderr, "FTOI(%s, %.17g) = %lld, FISTP %lld\n", c.what, x, c.got, c.want);
        }
    }
    SetControlWord(saved);
    if (failures) {
        fprintf(stderr, "%d mismatches\n", failures);
        return 1;
    }
    printf("FTOI matches x87 FISTP for %zu values\n", values.size());
    return 0;
}
