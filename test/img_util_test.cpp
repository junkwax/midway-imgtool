/*************************************************************
 * test/img_util_test.cpp
 *
 * Unit coverage for the small pure IMG helpers in platform/img_util.cpp.
 *************************************************************/
#include "img_util.h"

#include <cstdio>
#include <cstring>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

int main(void)
{
    /* signed_to_img_word clamps to the signed 16-bit range and packs it. */
    CHECK(signed_to_img_word(0) == 0);
    CHECK(signed_to_img_word(100) == 100);
    CHECK(signed_to_img_word(32767) == 32767);
    CHECK(signed_to_img_word(40000) == 32767);                 /* clamp high */
    CHECK(signed_to_img_word(-1) == (unsigned short)-1);       /* 0xFFFF */
    CHECK(signed_to_img_word(-32768) == (unsigned short)0x8000);
    CHECK(signed_to_img_word(-40000) == (unsigned short)0x8000); /* clamp low */

    /* img_name_string reads a NUL-terminated name out of n_s. */
    {
        IMG img {};
        std::strncpy(img.n_s, "WALK", sizeof(img.n_s));
        CHECK(img_name_string(&img) == "WALK");
    }
    /* A name filling n_s with no terminator reads exactly sizeof(n_s) chars. */
    {
        IMG img {};
        std::memset(img.n_s, 'A', sizeof(img.n_s));
        CHECK(img_name_string(&img).size() == sizeof(img.n_s));
    }
    /* Null IMG -> empty string. */
    CHECK(img_name_string(nullptr) == "");

    if (g_fails == 0) {
        std::printf("PASS: img_util helpers behave as specified\n");
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d img_util check(s)\n", g_fails);
    return 1;
}
