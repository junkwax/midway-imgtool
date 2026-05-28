/*************************************************************
 * test/sprite_sheet_import_test.cpp
 *
 * Regression coverage for sprite-sheet auto detection. The generated sheet
 * intentionally includes grid lines, text-like label marks, and disconnected
 * hands/feet so the detector has to cluster islands into whole sprites.
 *************************************************************/
#include "img_io.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

void undo_push(void) {}

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

    std::printf("PASS: sprite sheet detection grouped disconnected parts and ignored labels\n");
    return 0;
}
