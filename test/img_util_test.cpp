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

    /* next_sequential_img_name walks a numbered file series. */
    CHECK(next_sequential_img_name("CAGE4.IMG") == "CAGE5.IMG");
    CHECK(next_sequential_img_name("CAGE9.IMG") == "CAGE10.IMG");
    CHECK(next_sequential_img_name("CAGE09.IMG") == "CAGE10.IMG");   /* padding kept */
    CHECK(next_sequential_img_name("CAGE007.IMG") == "CAGE008.IMG");
    CHECK(next_sequential_img_name("CAGE.IMG") == "CAGE2.IMG");      /* unnumbered = #1 */
    CHECK(next_sequential_img_name("cage4.img") == "cage5.img");     /* case untouched */
    CHECK(next_sequential_img_name("CAGE4") == "CAGE5.IMG");         /* default ext */
    CHECK(next_sequential_img_name("CAGE4.IMG", 3) == "CAGE7.IMG");  /* step */
    CHECK(next_sequential_img_name("ABCDEFG9.IMG") == "ABCDEF10.IMG"); /* 8.3 stem */
    CHECK(next_sequential_img_name("C:\\ART\\V1.2\\CAGE4.IMG") == "C:\\ART\\V1.2\\CAGE5.IMG");

    if (g_fails == 0) {
        std::printf("PASS: img_util helpers behave as specified\n");
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d img_util check(s)\n", g_fails);
    return 1;
}
