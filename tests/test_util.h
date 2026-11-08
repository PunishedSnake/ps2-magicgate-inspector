/* SPDX-License-Identifier: MIT */
#ifndef MCI_TEST_UTIL_H
#define MCI_TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>

static int g_failures;
static int g_checks;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_EQ_U(a, b) do { \
    unsigned long long _a = (unsigned long long)(a); \
    unsigned long long _b = (unsigned long long)(b); \
    g_checks++; \
    if (_a != _b) { \
        g_failures++; \
        fprintf(stderr, "%s:%d: %s == %s failed: 0x%llx != 0x%llx\n", \
                __FILE__, __LINE__, #a, #b, _a, _b); \
    } \
} while (0)

#define CHECK_EQ_I(a, b) do { \
    long long _a = (long long)(a); \
    long long _b = (long long)(b); \
    g_checks++; \
    if (_a != _b) { \
        g_failures++; \
        fprintf(stderr, "%s:%d: %s == %s failed: %lld != %lld\n", \
                __FILE__, __LINE__, #a, #b, _a, _b); \
    } \
} while (0)

#define TEST_MAIN_END(name) do { \
    if (g_failures) { \
        fprintf(stderr, "%s: %d/%d checks FAILED\n", name, g_failures, g_checks); \
        return 1; \
    } \
    printf("%s: %d checks PASS\n", name, g_checks); \
    return 0; \
} while (0)

/* Deterministic xorshift32 so failures reproduce exactly. */
static unsigned int g_rng = 0x5900D2EBu;
static unsigned char rng_byte(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (unsigned char)(g_rng & 0xFFu);
}

#endif
