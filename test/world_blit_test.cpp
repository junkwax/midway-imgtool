/*************************************************************
 * test/world_blit_test.cpp
 *
 * Coverage for the CPU sprite compositor behind the World View PNG export:
 * WorldBlitSpriteRgba (palette lookup, channel order, mirroring, clipping,
 * alpha blending) and the WriteRgbaPng round-trip it feeds.
 *
 * These are the pieces a wrong byte order or off-by-one would corrupt
 * silently — the exported file would simply look wrong rather than fail.
 *************************************************************/
#include "world_render.h"
#include "img_io.h"
#include "stb_image.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

/* Symbols the app defines elsewhere; the blitter never dereferences the
   renderer, and the VGA palette is only a fallback we don't exercise here. */
SDL_Renderer *g_imgui_renderer = NULL;
extern "C" { SDL_Color g_palette[256]; }
void undo_push(void) {}
bool doc_undo_push(void) { return true; }

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

static unsigned short pack555(int r5, int g5, int b5)
{
    return (unsigned short)(((r5 & 0x1F) << 10) | ((g5 & 0x1F) << 5) | (b5 & 0x1F));
}

static const int kBufW = 8;
static const int kBufH = 8;

struct Px { unsigned char r, g, b, a; };

static Px At(const std::vector<unsigned char> &buf, int x, int y)
{
    const unsigned char *p = buf.data() + ((size_t)y * kBufW + x) * 4;
    Px v = { p[0], p[1], p[2], p[3] };
    return v;
}

static bool IsRed(Px p)   { return p.r > 240 && p.g < 16 && p.b < 16 && p.a == 255; }
static bool IsBlue(Px p)  { return p.b > 240 && p.r < 16 && p.g < 16 && p.a == 255; }
static bool IsClear(Px p) { return p.a == 0 && p.r == 0 && p.g == 0 && p.b == 0; }

int main(void)
{
    document_init();

    PAL *pal = AllocPal();
    IMG *img = AllocImg();
    if (!pal || !img) {
        std::fprintf(stderr, "FAIL: could not allocate test document\n");
        return 1;
    }

    /* Index 0 transparent, 1 red, 2 blue. */
    pal->numc = 3;
    pal->data_p = std::calloc(3, 2);
    if (!pal->data_p) { std::fprintf(stderr, "FAIL: palette alloc\n"); return 1; }
    unsigned char *pd = (unsigned char *)pal->data_p;
    unsigned short red = pack555(31, 0, 0), blue = pack555(0, 0, 31);
    pd[2] = (unsigned char)(red & 0xFF);  pd[3] = (unsigned char)(red >> 8);
    pd[4] = (unsigned char)(blue & 0xFF); pd[5] = (unsigned char)(blue >> 8);

    /* 3x2 sprite, stride padded to 4:
         row0:  1 0 2
         row1:  2 1 0   */
    img->w = 3;
    img->h = 2;
    img->palnum = 0;
    img->data_p = std::calloc(4 * 2, 1);
    if (!img->data_p) { std::fprintf(stderr, "FAIL: pixel alloc\n"); return 1; }
    unsigned char *px = (unsigned char *)img->data_p;
    px[0] = 1; px[1] = 0; px[2] = 2;
    px[4] = 2; px[5] = 1; px[6] = 0;

    std::vector<unsigned char> buf((size_t)kBufW * kBufH * 4, 0);

    /* ---- Straight blit: palette colors land in RGBA order ---- */
    int written = WorldBlitSpriteRgba(g_doc, img, 255, false, false, 2, 1,
                                      buf.data(), kBufW, kBufH);
    CHECK(written == 4);                      /* 6 pixels, 2 transparent */
    CHECK(IsRed(At(buf, 2, 1)));
    CHECK(IsClear(At(buf, 3, 1)));            /* index 0 never written */
    CHECK(IsBlue(At(buf, 4, 1)));
    CHECK(IsBlue(At(buf, 2, 2)));
    CHECK(IsRed(At(buf, 3, 2)));
    CHECK(IsClear(At(buf, 4, 2)));
    CHECK(IsClear(At(buf, 1, 1)));            /* nothing outside the rect */

    /* ---- Mirrored X reverses each row ---- */
    std::fill(buf.begin(), buf.end(), 0);
    WorldBlitSpriteRgba(g_doc, img, 255, true, false, 0, 4,
                        buf.data(), kBufW, kBufH);
    CHECK(IsBlue(At(buf, 0, 4)));
    CHECK(IsClear(At(buf, 1, 4)));
    CHECK(IsRed(At(buf, 2, 4)));

    /* ---- Mirrored Y swaps the rows ---- */
    std::fill(buf.begin(), buf.end(), 0);
    WorldBlitSpriteRgba(g_doc, img, 255, false, true, 0, 0,
                        buf.data(), kBufW, kBufH);
    CHECK(IsBlue(At(buf, 0, 0)));             /* row1 drawn first */
    CHECK(IsRed(At(buf, 1, 0)));
    CHECK(IsRed(At(buf, 0, 1)));

    /* ---- Clipping: partly off the top-left, and fully outside ---- */
    std::fill(buf.begin(), buf.end(), 0);
    written = WorldBlitSpriteRgba(g_doc, img, 255, false, false, -1, -1,
                                  buf.data(), kBufW, kBufH);
    /* Source column 0 and row 0 are cut off, leaving row1's (1,1)=red and
       (2,1)=transparent — so exactly one pixel lands, at the buffer origin. */
    CHECK(written == 1);
    CHECK(IsRed(At(buf, 0, 0)));
    CHECK(IsClear(At(buf, 1, 0)));
    CHECK(WorldBlitSpriteRgba(g_doc, img, 255, false, false, -50, 0,
                              buf.data(), kBufW, kBufH) == 0);
    CHECK(WorldBlitSpriteRgba(g_doc, img, 255, false, false, kBufW + 4, 0,
                              buf.data(), kBufW, kBufH) == 0);
    CHECK(WorldBlitSpriteRgba(g_doc, img, 255, false, false, 0, kBufH,
                              buf.data(), kBufW, kBufH) == 0);

    /* ---- Zoom: each source pixel becomes a zoom x zoom block ----
       The World View's per-row magnification goes through the export too, so
       a PNG of a zoomed scene is the scene that was on screen. */
    std::fill(buf.begin(), buf.end(), 0);
    written = WorldBlitSpriteRgba(g_doc, img, 255, false, false, 0, 0,
                                  buf.data(), kBufW, kBufH, 2);
    CHECK(written == 16);                     /* 4 opaque source pixels x 4 */
    CHECK(IsRed(At(buf, 0, 0)));              /* source (0,0) fills 2x2 */
    CHECK(IsRed(At(buf, 1, 0)));
    CHECK(IsRed(At(buf, 0, 1)));
    CHECK(IsRed(At(buf, 1, 1)));
    CHECK(IsClear(At(buf, 2, 0)));            /* index 0 stays a 2x2 hole */
    CHECK(IsClear(At(buf, 3, 1)));
    CHECK(IsBlue(At(buf, 4, 0)));             /* source (2,0) */
    CHECK(IsBlue(At(buf, 5, 1)));
    CHECK(IsBlue(At(buf, 0, 2)));             /* source (0,1), second row */
    CHECK(IsRed(At(buf, 3, 3)));              /* source (1,1) */
    CHECK(IsClear(At(buf, 0, 4)));            /* nothing past 2*h rows */

    /* Mirroring reads the source reversed, not the blown-up block. */
    std::fill(buf.begin(), buf.end(), 0);
    WorldBlitSpriteRgba(g_doc, img, 255, true, false, 0, 0,
                        buf.data(), kBufW, kBufH, 2);
    CHECK(IsBlue(At(buf, 0, 0)));             /* row0 reversed: 2 0 1 */
    CHECK(IsBlue(At(buf, 1, 1)));
    CHECK(IsClear(At(buf, 2, 0)));
    CHECK(IsRed(At(buf, 4, 0)));
    CHECK(IsRed(At(buf, 5, 1)));

    /* A zoomed sprite clips against the buffer at its ZOOMED extent: only the
       left half of source column 0 fits, four rows deep. */
    std::fill(buf.begin(), buf.end(), 0);
    CHECK(WorldBlitSpriteRgba(g_doc, img, 255, false, false, kBufW - 1, 0,
                              buf.data(), kBufW, kBufH, 2) == 4);
    CHECK(IsRed(At(buf, kBufW - 1, 0)));
    CHECK(IsRed(At(buf, kBufW - 1, 1)));
    CHECK(IsBlue(At(buf, kBufW - 1, 2)));
    CHECK(IsBlue(At(buf, kBufW - 1, 3)));
    CHECK(IsClear(At(buf, kBufW - 1, 4)));

    /* Zoom 0 and negative zoom are treated as 1 rather than dividing by 0. */
    std::fill(buf.begin(), buf.end(), 0);
    CHECK(WorldBlitSpriteRgba(g_doc, img, 255, false, false, 0, 0,
                              buf.data(), kBufW, kBufH, 0) == 4);
    CHECK(IsClear(At(buf, 1, 0)));
    std::fill(buf.begin(), buf.end(), 0);
    CHECK(WorldBlitSpriteRgba(g_doc, img, 255, false, false, 0, 0,
                              buf.data(), kBufW, kBufH, -3) == 4);

    /* ---- Painter's order: a later opaque sprite replaces an earlier one ---- */
    std::fill(buf.begin(), buf.end(), 0);
    WorldBlitSpriteRgba(g_doc, img, 255, false, false, 0, 0,
                        buf.data(), kBufW, kBufH);          /* red at 0,0 */
    WorldBlitSpriteRgba(g_doc, img, 255, true, false, 0, 0,
                        buf.data(), kBufW, kBufH);          /* blue at 0,0 */
    CHECK(IsBlue(At(buf, 0, 0)));

    /* ---- Translucent lane over an opaque one blends toward it ---- */
    std::fill(buf.begin(), buf.end(), 0);
    WorldBlitSpriteRgba(g_doc, img, 255, false, false, 0, 0,
                        buf.data(), kBufW, kBufH);          /* red base */
    WorldBlitSpriteRgba(g_doc, img, 128, true, false, 0, 0,
                        buf.data(), kBufW, kBufH);          /* half blue */
    Px mixed = At(buf, 0, 0);
    CHECK(mixed.a == 255);
    CHECK(mixed.r > 100 && mixed.r < 140);
    CHECK(mixed.b > 100 && mixed.b < 140);

    /* A translucent sprite onto empty space keeps its own alpha. */
    std::fill(buf.begin(), buf.end(), 0);
    WorldBlitSpriteRgba(g_doc, img, 128, false, false, 0, 0,
                        buf.data(), kBufW, kBufH);
    CHECK(At(buf, 0, 0).a == 128);
    CHECK(At(buf, 0, 0).r > 240);

    /* ---- WriteRgbaPng round-trips the buffer byte for byte ---- */
    std::fill(buf.begin(), buf.end(), 0);
    WorldBlitSpriteRgba(g_doc, img, 255, false, false, 2, 3,
                        buf.data(), kBufW, kBufH);
    const char *path = "world_blit_roundtrip_test.png";
    std::remove(path);
    CHECK(WriteRgbaPng(path, kBufW, kBufH, buf.data()));

    int rw = 0, rh = 0, channels = 0;
    unsigned char *back = stbi_load(path, &rw, &rh, &channels, 4);
    std::remove(path);
    CHECK(back != NULL);
    if (back) {
        CHECK(rw == kBufW && rh == kBufH);
        CHECK(std::memcmp(back, buf.data(), buf.size()) == 0);
        stbi_image_free(back);
    }

    /* Degenerate inputs are rejected rather than crashing. */
    CHECK(WorldBlitSpriteRgba(NULL, img, 255, false, false, 0, 0,
                              buf.data(), kBufW, kBufH) == 0);
    CHECK(WorldBlitSpriteRgba(g_doc, NULL, 255, false, false, 0, 0,
                              buf.data(), kBufW, kBufH) == 0);
    CHECK(WorldBlitSpriteRgba(g_doc, img, 255, false, false, 0, 0,
                              NULL, kBufW, kBufH) == 0);
    CHECK(!WriteRgbaPng(NULL, kBufW, kBufH, buf.data()));

    document_clear_contents(g_doc);

    if (g_fails == 0) {
        std::printf("PASS: world sprite compositor and PNG writer behave as specified\n");
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d world blit check(s)\n", g_fails);
    return 1;
}
