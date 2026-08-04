/*************************************************************
 * test/sprite_sheet_import_test.cpp
 *
 * Regression coverage for sprite-sheet auto detection. The generated sheet
 * intentionally includes grid lines, text-like label marks, and disconnected
 * hands/feet so the detector has to cluster islands into whole sprites.
 *************************************************************/
#include "img_io.h"
#include "palette_math.h"
#include "stb_image.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

void undo_push(void) {}
bool doc_undo_push(void) { return true; }

struct Rgb {
    unsigned char r, g, b;
};

static void fill_rect(std::vector<Rgb> &img, int w, int h,
                      int x0, int y0, int x1, int y1, Rgb c)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= w) x1 = w - 1;
    if (y1 >= h) y1 = h - 1;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            img[(size_t)y * w + x] = c;
        }
    }
}

static bool write_ppm(const char *path, const std::vector<Rgb> &img, int w, int h)
{
    FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    bool ok = std::fwrite(img.data(), sizeof(Rgb), img.size(), f) == img.size();
    std::fclose(f);
    return ok;
}

static unsigned short pack555(int r5, int g5, int b5)
{
    return (unsigned short)(((r5 & 0x1F) << 10) |
                            ((g5 & 0x1F) << 5) |
                             (b5 & 0x1F));
}

static void write_pal_word(unsigned char *dst, int idx, unsigned short w)
{
    dst[idx * 2 + 0] = (unsigned char)(w & 0xFF);
    dst[idx * 2 + 1] = (unsigned char)(w >> 8);
}

static bool check_png_export_uses_image_palette(void)
{
    document_init();

    PAL *gray = AllocPal();
    PAL *red = AllocPal();
    IMG *img = AllocImg();
    if (!gray || !red || !img) {
        std::fprintf(stderr, "FAIL: could not allocate export test document\n");
        return false;
    }

    gray->numc = 2;
    gray->data_p = std::calloc(2, 2);
    red->numc = 2;
    red->data_p = std::calloc(2, 2);
    if (!gray->data_p || !red->data_p) {
        std::fprintf(stderr, "FAIL: could not allocate export test palettes\n");
        return false;
    }

    write_pal_word((unsigned char *)gray->data_p, 1, pack555(15, 15, 15));
    write_pal_word((unsigned char *)red->data_p, 1, pack555(31, 0, 0));

    img->w = 1;
    img->h = 1;
    img->palnum = 1;
    img->data_p = std::calloc(1, 4);
    if (!img->data_p) {
        std::fprintf(stderr, "FAIL: could not allocate export test pixels\n");
        return false;
    }
    ((unsigned char *)img->data_p)[0] = 1;
    g_doc->ilselected = 0;

    const char *path = "png_palette_export_test.png";
    std::remove(path);
    ExportPng(path);

    int w = 0, h = 0, channels = 0;
    unsigned char *rgba = stbi_load(path, &w, &h, &channels, 4);
    std::remove(path);
    if (!rgba) {
        std::fprintf(stderr, "FAIL: exported PNG could not be decoded\n");
        return false;
    }

    bool ok = (w == 1 && h == 1 &&
               rgba[0] > 240 && rgba[1] < 16 && rgba[2] < 16 && rgba[3] == 255);
    if (!ok) {
        std::fprintf(stderr,
                     "FAIL: PNG export used wrong palette/color: %dx%d RGBA=(%u,%u,%u,%u)\n",
                     w, h, rgba[0], rgba[1], rgba[2], rgba[3]);
    }
    stbi_image_free(rgba);
    document_clear_contents(g_doc);
    return ok;
}

/* An imported palette must declare the depth its color count actually needs.
   Stamping every import 8bpp made the TBL/IRW/LOAD2 exports pack wider than
   the art required. */
static bool check_import_png_sets_palette_bpp(void)
{
    document_init();

    /* Five distinct opaque colors plus transparency. Median-cut stops
       splitting once every bucket holds one color, so the palette lands at
       5 colors + index 0 = 6 entries, which needs 3 bits. */
    const int w = 10, h = 2;
    unsigned char rgba[10 * 2 * 4] = {0};
    const unsigned char colors[5][3] = {
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {255, 0, 255}
    };
    for (int x = 0; x < w; x++) {
        const unsigned char *c = colors[x % 5];
        for (int y = 0; y < h; y++) {
            unsigned char *p = rgba + ((size_t)y * w + x) * 4;
            p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = 255;
        }
    }

    const char *path = "png_bpp_import_test.png";
    std::remove(path);
    if (!WriteRgbaPng(path, w, h, rgba)) {
        std::fprintf(stderr, "FAIL: could not write the bpp import fixture\n");
        return false;
    }
    ImportPng(path);
    std::remove(path);

    PAL *pal = get_pal(g_doc->palcnt > 0 ? (int)g_doc->palcnt - 1 : -1);
    if (!pal) {
        std::fprintf(stderr, "FAIL: ImportPng created no palette\n");
        return false;
    }

    int want = PaletteBppForColorCount((int)pal->numc);
    bool ok = ((int)pal->bitspix == want);
    if (!ok) {
        std::fprintf(stderr,
                     "FAIL: imported palette has %u colors but BITSPIX %u (expected %d)\n",
                     pal->numc, pal->bitspix, want);
    }
    /* Guard the intent, not just the internal consistency: a handful of
       colors must not come back as a full 8bpp palette. */
    if (pal->numc > 16 || pal->bitspix > 4) {
        std::fprintf(stderr,
                     "FAIL: 5-color PNG quantized to %u colors at %u bpp\n",
                     pal->numc, pal->bitspix);
        ok = false;
    }

    document_clear_contents(g_doc);
    return ok;
}

static void draw_sprite(std::vector<Rgb> &img, int w, int h, int x, int y)
{
    (void)h;
    Rgb blue = {16, 32, 112};
    Rgb brown = {128, 88, 36};
    Rgb skin = {224, 188, 144};
    Rgb black = {8, 8, 8};
    fill_rect(img, w, h, x + 8, y + 4, x + 27, y + 25, blue);
    fill_rect(img, w, h, x + 10, y + 26, x + 30, y + 40, brown);
    fill_rect(img, w, h, x + 4, y + 2, x + 16, y + 9, skin);
    fill_rect(img, w, h, x + 35, y + 11, x + 40, y + 16, skin); /* separated hand */
    fill_rect(img, w, h, x + 0, y + 39, x + 8, y + 43, black);  /* separated foot */
}

static void draw_label_noise(std::vector<Rgb> &img, int w, int h)
{
    Rgb black = {0, 0, 0};
    fill_rect(img, w, h, 8, 8, 10, 28, black);
    fill_rect(img, w, h, 20, 8, 22, 28, black);
    fill_rect(img, w, h, 11, 17, 19, 19, black);
    fill_rect(img, w, h, 32, 8, 34, 28, black);
    fill_rect(img, w, h, 30, 8, 38, 10, black);
    fill_rect(img, w, h, 30, 26, 38, 28, black);
}

int main(void)
{
    const int w = 180;
    const int h = 90;
    std::vector<Rgb> img((size_t)w * h, {255, 255, 255});
    Rgb grid = {210, 210, 210};
    fill_rect(img, w, h, 0, 44, w - 1, 45, grid);
    fill_rect(img, w, h, 58, 0, 59, h - 1, grid);
    fill_rect(img, w, h, 118, 0, 119, h - 1, grid);
    draw_label_noise(img, w, h);
    draw_sprite(img, w, h, 68, 4);
    draw_sprite(img, w, h, 128, 4);

    const char *path = "sprite_sheet_import_test.ppm";
    if (!write_ppm(path, img, w, h)) {
        std::fprintf(stderr, "FAIL: could not write %s\n", path);
        return 1;
    }

    SpriteSheetImportOptions opts = {};
    opts.detect_mode = SpriteSheetDetect_Auto;
    opts.background_threshold = 245;
    opts.min_pixels = 120;
    opts.padding = 2;
    opts.crop = true;
    std::strncpy(opts.name_prefix, "FRAME", sizeof(opts.name_prefix) - 1);

    SpriteSheetDebugReport report;
    int frames = AnalyzeSpriteSheet(path, &opts, &report);
    std::remove(path);

    if (frames != 2) {
        std::fprintf(stderr, "FAIL: expected 2 frames, got %d (raw islands=%d)\n",
                     frames, report.raw_islands);
        return 1;
    }
    for (const SpriteSheetDebugFrame &f : report.frames) {
        if (f.islands < 2) {
            std::fprintf(stderr, "FAIL: frame was not clustered from disconnected parts "
                         "(box=%d,%d..%d,%d islands=%d pixels=%d)\n",
                         f.x0, f.y0, f.x1, f.y1, f.islands, f.pixels);
            return 1;
        }
        if (f.x0 < 55) {
            std::fprintf(stderr, "FAIL: label noise was imported as part of a sprite\n");
            return 1;
        }
    }

    if (!check_png_export_uses_image_palette())
        return 1;

    if (!check_import_png_sets_palette_bpp())
        return 1;

    std::printf("PASS: sprite sheet detection grouped disconnected parts and ignored labels\n");
    std::printf("PASS: PNG export uses the selected image palette\n");
    std::printf("PASS: PNG import derives palette BPP from its color count\n");
    return 0;
}
