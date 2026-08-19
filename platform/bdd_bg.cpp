/*************************************************************
 * platform/bdd_bg.cpp
 *
 * BDD/BDB parsing and stage compositing for the World View
 * reference background. See bdd_bg.h for the format notes and
 * for what this deliberately does not attempt.
 *************************************************************/
#include "bdd_bg.h"

/* Declared here rather than by including ui_internal.h: the renderer is the
   only thing this module needs from the UI, and pulling that header in would
   drag imgui into every target that links the loader. */
extern SDL_Renderer *g_imgui_renderer;

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* Ceilings copied from bddtool. They exist to stop a corrupt header
   from sizing an allocation, not because the data ever approaches
   them — the largest shipped stage runs a few hundred objects. */
enum {
    kBddMaxImages   = 4096,
    kBddMaxObjects  = 16384,
    kBddMaxModules  = 256,
    kBddMaxPalettes = 256,
    kBddMaxDim      = 4096,
    /* A stage wider than this would exceed what renderers reliably
       accept as a single texture, and nothing legitimate comes
       close (the biggest real world is 4000x3000 declared). */
    kBddMaxCanvas   = 8192
};

struct BddBgImage {
    int idx = 0;
    int w = 0, h = 0;
    std::vector<unsigned char> pix;
};

struct BddBgPalette {
    int count = 0;
    unsigned char rgb[256][3];
};

struct BddBgObject {
    int wx = 0;     /* DMA control word; bit 4 = hflip, bit 5 = vflip */
    int depth = 0;  /* world X */
    int sy = 0;     /* world Y */
    int ii = 0;     /* image index, matches BddBgImage::idx */
    int fl = 0;     /* palette index, in BDD file order */
};

/* ---- small helpers ------------------------------------------------ */

static void BddSetError(BddBackground *bg, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bg->error = buf;
}

/* A BDD interleaves ASCII headers with raw pixel blocks, so it must be
   read as binary and its text lines pulled off a byte at a time —
   fgets() on a text stream would mangle the pixels that follow. */
static bool BddReadLine(FILE *f, char *out, size_t outsz)
{
    if (!out || outsz == 0) return false;
    out[0] = '\0';
    int c;
    while ((c = fgetc(f)) != EOF && (c == '\r' || c == '\n')) {}
    if (c == EOF) return false;

    size_t n = 0;
    out[n++] = (char)c;
    while ((c = fgetc(f)) != EOF && c != '\n') {
        if (c == '\r') continue;
        if (n + 1 < outsz) out[n++] = (char)c;
    }
    out[n] = '\0';
    return true;
}

/* 5-bit channels replicated into 8, rather than shifted left by 3:
   plain <<3 never reaches 255, so a stage's whites come out dingy
   and a screenshot cannot be colour-matched against the real thing. */
static void BddRgb555ToRgb(unsigned short c, unsigned char *out)
{
    int r = (c >> 10) & 31;
    int g = (c >> 5) & 31;
    int b = c & 31;
    out[0] = (unsigned char)((r << 3) | (r >> 2));
    out[1] = (unsigned char)((g << 3) | (g >> 2));
    out[2] = (unsigned char)((b << 3) | (b >> 2));
}

static std::string BddStripExtension(const std::string &path)
{
    size_t dot = path.find_last_of('.');
    size_t sep = path.find_last_of("/\\");
    if (dot == std::string::npos) return path;
    if (sep != std::string::npos && dot < sep) return path;
    return path.substr(0, dot);
}

static bool BddFileExists(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* The pair is conventionally NAME.BDD + NAME.BDB, but the case of the
   extension is not consistent across the dumps, so try the obvious
   spellings before giving up. */
static bool BddResolveSibling(const std::string &path, const char *upper,
                              const char *lower, std::string *out)
{
    std::string stem = BddStripExtension(path);
    const char *tries[2] = { upper, lower };
    for (int i = 0; i < 2; i++) {
        std::string candidate = stem + tries[i];
        if (BddFileExists(candidate)) {
            *out = candidate;
            return true;
        }
    }
    return false;
}

/* ---- BDB (placement) ---------------------------------------------- */

static bool BddLoadBdb(const char *path, BddBackground *bg,
                       std::vector<BddBgObject> *objects)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        BddSetError(bg, "cannot open BDB: %s", path);
        return false;
    }

    char ln[512];
    if (!BddReadLine(f, ln, sizeof(ln))) {
        fclose(f);
        BddSetError(bg, "empty BDB: %s", path);
        return false;
    }

    char name[64] = {0};
    int world_w = 0, world_h = 0, max_depth = 255;
    int modules = 0, palettes = 0, object_count = -1;
    int fields = sscanf(ln, "%63s %d %d %d %d %d %d", name, &world_w, &world_h,
                        &max_depth, &modules, &palettes, &object_count);
    if (fields < 3) {
        fclose(f);
        BddSetError(bg, "unreadable BDB header: %s", ln);
        return false;
    }
    bg->stage_name = name;
    bg->world_w = world_w;
    bg->world_h = world_h;
    if (modules < 0 || modules > kBddMaxModules) modules = 0;

    for (int m = 0; m < modules; m++) {
        if (!BddReadLine(f, ln, sizeof(ln))) break;
        BddBgModule mod;
        char mname[64] = {0};
        if (sscanf(ln, "%63s %d %d %d %d", mname, &mod.x1, &mod.x2,
                   &mod.y1, &mod.y2) < 5)
            continue;
        mod.name = mname;
        bg->modules.push_back(mod);
    }

    /* A count of -1 (older files omit the field) means "read to EOF". */
    int limit = (object_count >= 0 && object_count <= kBddMaxObjects)
              ? object_count : kBddMaxObjects;
    for (int i = 0; i < limit; i++) {
        if (!BddReadLine(f, ln, sizeof(ln))) break;
        char a[32], b[32], c[32], d[32], e[32];
        if (sscanf(ln, "%31s %31s %31s %31s %31s", a, b, c, d, e) < 5)
            continue;
        BddBgObject obj;
        obj.wx    = (int)strtol(a, NULL, 16);
        obj.depth = atoi(b);
        obj.sy    = atoi(c);
        obj.ii    = (int)strtol(d, NULL, 16);
        obj.fl    = atoi(e);
        objects->push_back(obj);
    }

    fclose(f);
    if (objects->empty()) {
        BddSetError(bg, "BDB lists no placements: %s", path);
        return false;
    }
    return true;
}

/* ---- BDD (pixels) -------------------------------------------------- */

static bool BddLoadBdd(const char *path, BddBackground *bg,
                       std::vector<BddBgImage> *images,
                       std::vector<BddBgPalette> *palettes)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        BddSetError(bg, "cannot open BDD: %s", path);
        return false;
    }

    char ln[512];
    if (!BddReadLine(f, ln, sizeof(ln))) {
        fclose(f);
        BddSetError(bg, "empty BDD: %s", path);
        return false;
    }

    int expected = atoi(ln);
    if (expected < 0 || expected > kBddMaxImages) {
        fclose(f);
        BddSetError(bg, "invalid BDD image count (%d): %s", expected, path);
        return false;
    }

    for (int i = 0; i < expected; i++) {
        if (!BddReadLine(f, ln, sizeof(ln))) break;
        unsigned int idx = 0;
        int w = 0, h = 0, dma = 0;
        if (sscanf(ln, "%x %d %d %d", &idx, &w, &h, &dma) < 3)
            break;
        if (w <= 0 || h <= 0 || w > kBddMaxDim || h > kBddMaxDim) break;

        BddBgImage img;
        img.idx = (int)idx;
        img.w = w;
        img.h = h;
        img.pix.resize((size_t)w * (size_t)h);
        if (fread(img.pix.data(), 1, img.pix.size(), f) != img.pix.size())
            break;
        images->push_back(img);
    }

    /* Palettes run to EOF: the count is not restated after the images. */
    while (palettes->size() < (size_t)kBddMaxPalettes) {
        if (!BddReadLine(f, ln, sizeof(ln))) break;
        char pname[64] = {0};
        int count = 0;
        if (sscanf(ln, "%63s %d", pname, &count) < 2) break;
        if (count <= 0 || count > 256) break;

        std::vector<unsigned char> raw((size_t)count * 2);
        if (fread(raw.data(), 1, raw.size(), f) != raw.size()) break;

        BddBgPalette pal;
        memset(pal.rgb, 0, sizeof(pal.rgb));
        pal.count = count;
        for (int i = 0; i < count; i++) {
            unsigned short c = (unsigned short)(raw[(size_t)i * 2] |
                                                (raw[(size_t)i * 2 + 1] << 8));
            BddRgb555ToRgb(c, pal.rgb[i]);
        }
        palettes->push_back(pal);
    }

    fclose(f);
    if (images->empty()) {
        BddSetError(bg, "BDD holds no images: %s", path);
        return false;
    }
    if (palettes->empty()) {
        BddSetError(bg, "BDD holds no palettes: %s", path);
        return false;
    }
    return true;
}

/* ---- compositing --------------------------------------------------- */

static const BddBgImage *BddFindImage(const std::vector<BddBgImage> &images, int idx)
{
    for (size_t i = 0; i < images.size(); i++)
        if (images[i].idx == idx) return &images[i];
    return NULL;
}

/* Tally each module's objects so the plane picker has something to rank.
   First-fit containment, matching bddtool: a module is a packaging
   rectangle and an object belongs to the first one that encloses it. */
static void BddTallyModules(BddBackground *bg,
                            const std::vector<BddBgObject> &objects,
                            const std::vector<BddBgImage> &images)
{
    for (size_t o = 0; o < objects.size(); o++) {
        const BddBgObject &obj = objects[o];
        const BddBgImage *img = BddFindImage(images, obj.ii);
        if (!img) continue;
        int x2 = obj.depth + img->w - 1;
        int y2 = obj.sy + img->h - 1;
        for (size_t m = 0; m < bg->modules.size(); m++) {
            BddBgModule &mod = bg->modules[m];
            if (obj.depth < mod.x1 || obj.sy < mod.y1 ||
                x2 > mod.x2 || y2 > mod.y2)
                continue;
            mod.object_count++;
            int layer = (obj.wx >> 8) & 0xFF;
            if (layer == 0x40 || layer == 0x41) mod.playfield_objects++;
            if (!mod.has_content) {
                mod.has_content = true;
                mod.cx1 = obj.depth; mod.cy1 = obj.sy;
                mod.cx2 = x2;        mod.cy2 = y2;
            } else {
                if (obj.depth < mod.cx1) mod.cx1 = obj.depth;
                if (obj.sy < mod.cy1)    mod.cy1 = obj.sy;
                if (x2 > mod.cx2)        mod.cx2 = x2;
                if (y2 > mod.cy2)        mod.cy2 = y2;
            }
            break;
        }
    }
}

static bool BddComposite(BddBackground *bg,
                         const std::vector<BddBgObject> &objects,
                         const std::vector<BddBgImage> &images,
                         const std::vector<BddBgPalette> &palettes)
{
    /* Size the canvas from the art, not the declared world: the header
       is routinely generous and an overhanging object is legal. */
    int minx = 0, miny = 0, maxx = 0, maxy = 0;
    bool any = false;
    for (size_t i = 0; i < objects.size(); i++) {
        const BddBgObject &obj = objects[i];
        const BddBgImage *img = BddFindImage(images, obj.ii);
        if (!img) continue;
        if (!any) {
            minx = obj.depth; miny = obj.sy;
            maxx = obj.depth + img->w; maxy = obj.sy + img->h;
            any = true;
            continue;
        }
        if (obj.depth < minx) minx = obj.depth;
        if (obj.sy < miny) miny = obj.sy;
        if (obj.depth + img->w > maxx) maxx = obj.depth + img->w;
        if (obj.sy + img->h > maxy) maxy = obj.sy + img->h;
    }
    if (!any) {
        BddSetError(bg, "no placement names an image the BDD contains");
        return false;
    }

    int w = maxx - minx;
    int h = maxy - miny;
    if (w <= 0 || h <= 0 || w > kBddMaxCanvas || h > kBddMaxCanvas) {
        BddSetError(bg, "stage spans %dx%d, beyond the %d-pixel limit",
                    w, h, (int)kBddMaxCanvas);
        return false;
    }

    bg->content_x = minx;
    bg->content_y = miny;
    bg->w = w;
    bg->h = h;
    bg->rgba.assign((size_t)w * (size_t)h * 4, 0);

    /* BDB file order is the draw order within a stage, so a later
       placement legitimately paints over an earlier one. */
    for (size_t i = 0; i < objects.size(); i++) {
        const BddBgObject &obj = objects[i];
        const BddBgImage *img = BddFindImage(images, obj.ii);
        if (!img || img->pix.empty()) { bg->skipped_objects++; continue; }
        if (obj.fl < 0 || obj.fl >= (int)palettes.size()) {
            bg->skipped_objects++;
            continue;
        }
        const BddBgPalette &pal = palettes[(size_t)obj.fl];
        bool hfl = (obj.wx & 0x10) != 0;
        bool vfl = (obj.wx & 0x20) != 0;

        for (int yy = 0; yy < img->h; yy++) {
            int dy = obj.sy + yy - miny;
            if (dy < 0 || dy >= h) continue;
            int sy = vfl ? (img->h - 1 - yy) : yy;
            const unsigned char *row = &img->pix[(size_t)sy * img->w];
            unsigned char *dstrow = &bg->rgba[((size_t)dy * w) * 4];
            for (int xx = 0; xx < img->w; xx++) {
                int dx = obj.depth + xx - minx;
                if (dx < 0 || dx >= w) continue;
                int sx = hfl ? (img->w - 1 - xx) : xx;
                unsigned char v = row[sx];
                if (v == 0) continue;             /* index 0 is transparent */
                if (v >= pal.count) continue;
                unsigned char *dst = dstrow + (size_t)dx * 4;
                dst[0] = pal.rgb[v][0];
                dst[1] = pal.rgb[v][1];
                dst[2] = pal.rgb[v][2];
                dst[3] = 255;
            }
        }
    }
    return true;
}

/* ---- public API ---------------------------------------------------- */

void BddBgFree(BddBackground *bg)
{
    if (!bg) return;
    if (bg->tex) {
        SDL_DestroyTexture(bg->tex);
        bg->tex = NULL;
    }
    bg->loaded = false;
    bg->bdd_path.clear();
    bg->bdb_path.clear();
    bg->stage_name.clear();
    bg->world_w = bg->world_h = 0;
    bg->content_x = bg->content_y = 0;
    bg->w = bg->h = 0;
    bg->rgba.clear();
    bg->rgba.shrink_to_fit();
    bg->modules.clear();
    bg->object_count = bg->image_count = bg->palette_count = 0;
    bg->skipped_objects = 0;
    bg->error.clear();
}

bool BddBgLoad(const char *path, BddBackground *out)
{
    if (!out) return false;
    BddBgFree(out);
    if (!path || !path[0]) {
        BddSetError(out, "no path given");
        return false;
    }

    std::string given = path;
    std::string bdd_path, bdb_path;

    /* Accept either half of the pair and find the other one. */
    std::string ext;
    size_t dot = given.find_last_of('.');
    if (dot != std::string::npos) ext = given.substr(dot);
    bool given_is_bdb = (ext.size() == 4 &&
                         (ext[1] == 'b' || ext[1] == 'B') &&
                         (ext[2] == 'd' || ext[2] == 'D') &&
                         (ext[3] == 'b' || ext[3] == 'B'));

    if (given_is_bdb) {
        bdb_path = given;
        if (!BddResolveSibling(given, ".BDD", ".bdd", &bdd_path)) {
            BddSetError(out, "no matching .BDD next to %s — the BDB holds "
                             "placements only, the BDD holds the pixels",
                        given.c_str());
            return false;
        }
    } else {
        bdd_path = given;
        if (!BddResolveSibling(given, ".BDB", ".bdb", &bdb_path)) {
            BddSetError(out, "no matching .BDB next to %s — the BDD holds "
                             "pixels only, the BDB holds every placement",
                        given.c_str());
            return false;
        }
    }

    std::vector<BddBgObject> objects;
    if (!BddLoadBdb(bdb_path.c_str(), out, &objects)) return false;

    std::vector<BddBgImage> images;
    std::vector<BddBgPalette> palettes;
    if (!BddLoadBdd(bdd_path.c_str(), out, &images, &palettes)) return false;

    BddTallyModules(out, objects, images);
    if (!BddComposite(out, objects, images, palettes)) return false;

    out->bdd_path = bdd_path;
    out->bdb_path = bdb_path;
    out->object_count = (int)objects.size();
    out->image_count = (int)images.size();
    out->palette_count = (int)palettes.size();
    out->loaded = true;
    out->error.clear();
    return true;
}

SDL_Texture *BddBgTexture(BddBackground *bg)
{
    if (!bg || !bg->loaded || bg->w <= 0 || bg->h <= 0) return NULL;
    if (bg->tex) return bg->tex;
    if (!g_imgui_renderer || bg->rgba.empty()) return NULL;

    SDL_Texture *tex = SDL_CreateTexture(g_imgui_renderer,
                                         SDL_PIXELFORMAT_ABGR8888,
                                         SDL_TEXTUREACCESS_STATIC,
                                         bg->w, bg->h);
    if (!tex) return NULL;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
    if (SDL_UpdateTexture(tex, NULL, bg->rgba.data(), bg->w * 4) != 0) {
        SDL_DestroyTexture(tex);
        return NULL;
    }
    bg->tex = tex;
    return bg->tex;
}

int BddBgPlayfieldModule(const BddBackground *bg)
{
    if (!bg || bg->modules.empty()) return -1;

    int best = -1, best_playfield = 0;
    for (size_t i = 0; i < bg->modules.size(); i++) {
        if (bg->modules[i].playfield_objects > best_playfield) {
            best_playfield = bg->modules[i].playfield_objects;
            best = (int)i;
        }
    }
    if (best >= 0) return best;

    /* No 1.0x-scroll objects anywhere (a background-only stage):
       the busiest module is still the best thing to look at. */
    int most = 0;
    for (size_t i = 0; i < bg->modules.size(); i++) {
        if (bg->modules[i].object_count > most) {
            most = bg->modules[i].object_count;
            best = (int)i;
        }
    }
    return best;
}
