/*************************************************************
 * test/bdd_bg_test.cpp
 *
 * Coverage for the BDD/BDB reference-background loader.
 *
 * The parser walks a file that interleaves ASCII headers with raw
 * binary pixel blocks, so a single miscounted byte desynchronises
 * everything after it and the stage still "loads" — just wrong.
 * These tests build known-good pairs on disk and assert the exact
 * pixels that come back, plus the refusals that keep a corrupt or
 * half-present stage from reaching the canvas.
 *************************************************************/
#include "bdd_bg.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* Symbols the app defines elsewhere. The loader only touches the
   renderer in BddBgTexture(), which these tests never call. */
SDL_Renderer *g_imgui_renderer = NULL;

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

static std::string g_tmp_prefix;

static std::string TmpPath(const char *stem, const char *ext)
{
    return g_tmp_prefix + stem + ext;
}

static void WriteFile(const std::string &path, const std::vector<unsigned char> &bytes)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); exit(2); }
    if (!bytes.empty()) fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
}

static void Append(std::vector<unsigned char> &v, const char *s)
{
    while (*s) v.push_back((unsigned char)*s++);
}

static unsigned short Pack555(int r5, int g5, int b5)
{
    return (unsigned short)(((r5 & 0x1F) << 10) | ((g5 & 0x1F) << 5) | (b5 & 0x1F));
}

static void AppendU16LE(std::vector<unsigned char> &v, unsigned short c)
{
    v.push_back((unsigned char)(c & 0xFF));
    v.push_back((unsigned char)(c >> 8));
}

/* A 2x2 image, indices 0..3, and a palette whose entry N is a distinct
   colour. Index 0 must stay transparent no matter what the palette says. */
static void BuildSimplePair(const char *stem, std::string *bdd_out,
                            std::string *bdb_out)
{
    std::vector<unsigned char> bdd;
    Append(bdd, "1\n");
    Append(bdd, "1 2 2 0\n");
    const unsigned char pix[4] = { 0, 1, 2, 3 };   /* TL transparent */
    for (int i = 0; i < 4; i++) bdd.push_back(pix[i]);
    Append(bdd, "TESTPAL 4\n");
    AppendU16LE(bdd, Pack555(31, 31, 31));  /* 0: opaque white, never drawn */
    AppendU16LE(bdd, Pack555(31, 0, 0));    /* 1: red */
    AppendU16LE(bdd, Pack555(0, 31, 0));    /* 2: green */
    AppendU16LE(bdd, Pack555(0, 0, 31));    /* 3: blue */

    std::vector<unsigned char> bdb;
    /* name w h max_depth modules pals objects */
    Append(bdb, "TESTSTAGE 64 64 255 1 1 1\n");
    Append(bdb, "TMOD 0 63 0 63\n");
    /* wx depth sy ii fl — layer 0x40 is the playfield plane */
    Append(bdb, "4000 10 20 1 0\n");

    *bdd_out = TmpPath(stem, ".BDD");
    *bdb_out = TmpPath(stem, ".BDB");
    WriteFile(*bdd_out, bdd);
    WriteFile(*bdb_out, bdb);
}

static const unsigned char *PixAt(const BddBackground &bg, int x, int y)
{
    return &bg.rgba[((size_t)y * bg.w + x) * 4];
}

static void TestLoadAndComposite(void)
{
    std::string bdd_path, bdb_path;
    BuildSimplePair("simple", &bdd_path, &bdb_path);

    BddBackground bg;
    CHECK(BddBgLoad(bdd_path.c_str(), &bg));
    if (!bg.loaded) {
        std::fprintf(stderr, "  error was: %s\n", bg.error.c_str());
        return;
    }

    CHECK(bg.stage_name == "TESTSTAGE");
    CHECK(bg.world_w == 64 && bg.world_h == 64);
    CHECK(bg.object_count == 1);
    CHECK(bg.image_count == 1);
    CHECK(bg.palette_count == 1);
    CHECK(bg.skipped_objects == 0);

    /* Cropped to the art, not the declared 64x64 world. */
    CHECK(bg.w == 2 && bg.h == 2);
    CHECK(bg.content_x == 10 && bg.content_y == 20);

    /* Index 0 stays transparent even though palette entry 0 is opaque white. */
    CHECK(PixAt(bg, 0, 0)[3] == 0);

    /* 5-bit channels replicate into 8, so full-scale reaches 255 rather
       than the 248 a plain <<3 would give. */
    const unsigned char *red = PixAt(bg, 1, 0);
    CHECK(red[0] == 255 && red[1] == 0 && red[2] == 0 && red[3] == 255);
    const unsigned char *green = PixAt(bg, 0, 1);
    CHECK(green[0] == 0 && green[1] == 255 && green[2] == 0 && green[3] == 255);
    const unsigned char *blue = PixAt(bg, 1, 1);
    CHECK(blue[0] == 0 && blue[1] == 0 && blue[2] == 255 && blue[3] == 255);

    /* The module tally is what the plane picker ranks on. */
    CHECK(bg.modules.size() == 1);
    if (bg.modules.size() == 1) {
        const BddBgModule &mod = bg.modules[0];
        CHECK(mod.name == "TMOD");
        CHECK(mod.object_count == 1);
        CHECK(mod.playfield_objects == 1);
        /* Painted bounds, not the declared 0..63 rect: the snap aligns to
           these so the art meets the floor line with no dead band. */
        CHECK(mod.has_content);
        CHECK(mod.cx1 == 10 && mod.cy1 == 20);
        CHECK(mod.cx2 == 11 && mod.cy2 == 21);
    }
    CHECK(BddBgPlayfieldModule(&bg) == 0);

    BddBgFree(&bg);
    CHECK(!bg.loaded);
    CHECK(bg.rgba.empty());
    CHECK(bg.modules.empty());
}

/* Passing the .BDB must find the .BDD and produce the same stage: the
   pair is symmetric and the user may pick either half in the dialog. */
static void TestLoadFromBdbSide(void)
{
    std::string bdd_path, bdb_path;
    BuildSimplePair("either", &bdd_path, &bdb_path);

    BddBackground bg;
    CHECK(BddBgLoad(bdb_path.c_str(), &bg));
    CHECK(bg.loaded);
    CHECK(bg.w == 2 && bg.h == 2);
    CHECK(bg.bdd_path == bdd_path);
    CHECK(bg.bdb_path == bdb_path);
    BddBgFree(&bg);
}

/* Flip bits live in the DMA control word, and getting them backwards
   mirrors a whole stage subtly enough to miss by eye. */
static void TestFlipBits(void)
{
    std::vector<unsigned char> bdd;
    Append(bdd, "1\n");
    Append(bdd, "1 2 1 0\n");
    bdd.push_back(1);   /* left = red */
    bdd.push_back(2);   /* right = green */
    Append(bdd, "P 3\n");
    AppendU16LE(bdd, Pack555(0, 0, 0));
    AppendU16LE(bdd, Pack555(31, 0, 0));
    AppendU16LE(bdd, Pack555(0, 31, 0));

    std::vector<unsigned char> bdb;
    Append(bdb, "FLIP 32 32 255 0 1 1\n");
    Append(bdb, "4010 0 0 1 0\n");   /* wx bit 4 set = horizontal flip */

    std::string bdd_path = TmpPath("flip", ".BDD");
    std::string bdb_path = TmpPath("flip", ".BDB");
    WriteFile(bdd_path, bdd);
    WriteFile(bdb_path, bdb);

    BddBackground bg;
    CHECK(BddBgLoad(bdd_path.c_str(), &bg));
    if (bg.loaded) {
        CHECK(bg.w == 2 && bg.h == 1);
        /* Flipped: green now on the left, red on the right. */
        CHECK(PixAt(bg, 0, 0)[1] == 255);
        CHECK(PixAt(bg, 1, 0)[0] == 255);
    }
    BddBgFree(&bg);
}

/* Later placements paint over earlier ones — BDB file order is the
   stage's own draw order and reversing it hides foreground art. */
static void TestDrawOrder(void)
{
    std::vector<unsigned char> bdd;
    Append(bdd, "1\n");
    Append(bdd, "1 1 1 0\n");
    bdd.push_back(1);
    Append(bdd, "A 2\n");
    AppendU16LE(bdd, Pack555(0, 0, 0));
    AppendU16LE(bdd, Pack555(31, 0, 0));   /* pal 0 index 1 = red */
    Append(bdd, "B 2\n");
    AppendU16LE(bdd, Pack555(0, 0, 0));
    AppendU16LE(bdd, Pack555(0, 31, 0));   /* pal 1 index 1 = green */

    std::vector<unsigned char> bdb;
    Append(bdb, "ORDER 32 32 255 0 2 2\n");
    Append(bdb, "4000 5 5 1 0\n");   /* red first */
    Append(bdb, "4000 5 5 1 1\n");   /* green second, same spot */

    std::string bdd_path = TmpPath("order", ".BDD");
    std::string bdb_path = TmpPath("order", ".BDB");
    WriteFile(bdd_path, bdd);
    WriteFile(bdb_path, bdb);

    BddBackground bg;
    CHECK(BddBgLoad(bdd_path.c_str(), &bg));
    if (bg.loaded) {
        CHECK(bg.w == 1 && bg.h == 1);
        CHECK(PixAt(bg, 0, 0)[1] == 255);   /* green won */
        CHECK(PixAt(bg, 0, 0)[0] == 0);
    }
    BddBgFree(&bg);
}

/* A placement naming an image or palette the BDD lacks is counted and
   skipped rather than dropping the whole stage: real files carry the
   odd stale reference and the rest is still worth looking at. */
static void TestSkipsUnresolvedPlacements(void)
{
    std::vector<unsigned char> bdd;
    Append(bdd, "1\n");
    Append(bdd, "1 1 1 0\n");
    bdd.push_back(1);
    Append(bdd, "P 2\n");
    AppendU16LE(bdd, Pack555(0, 0, 0));
    AppendU16LE(bdd, Pack555(31, 0, 0));

    std::vector<unsigned char> bdb;
    Append(bdb, "PARTIAL 32 32 255 0 1 3\n");
    Append(bdb, "4000 0 0 1 0\n");   /* fine */
    Append(bdb, "4000 8 0 9 0\n");   /* image 9 does not exist */
    Append(bdb, "4000 0 8 1 7\n");   /* palette 7 does not exist */

    std::string bdd_path = TmpPath("partial", ".BDD");
    std::string bdb_path = TmpPath("partial", ".BDB");
    WriteFile(bdd_path, bdd);
    WriteFile(bdb_path, bdb);

    BddBackground bg;
    CHECK(BddBgLoad(bdd_path.c_str(), &bg));
    if (bg.loaded) {
        CHECK(bg.object_count == 3);
        CHECK(bg.skipped_objects == 2);   /* the bad image and the bad palette */
        /* Only placements that resolve to an image size the canvas, so the
           canvas spans the two that do: y 0 and y 8, one pixel tall each. */
        CHECK(bg.w == 1 && bg.h == 9);
        CHECK(PixAt(bg, 0, 0)[0] == 255);
        CHECK(PixAt(bg, 0, 8)[3] == 0);   /* the bad-palette one drew nothing */
    }
    BddBgFree(&bg);
}

/* Half a pair draws nothing, so the refusal has to be explicit and
   name which half is missing. */
static void TestMissingSiblingFails(void)
{
    std::vector<unsigned char> bdd;
    Append(bdd, "1\n");
    Append(bdd, "1 1 1 0\n");
    bdd.push_back(1);
    Append(bdd, "P 2\n");
    AppendU16LE(bdd, Pack555(0, 0, 0));
    AppendU16LE(bdd, Pack555(31, 0, 0));

    std::string bdd_path = TmpPath("lonely", ".BDD");
    WriteFile(bdd_path, bdd);
    remove(TmpPath("lonely", ".BDB").c_str());

    BddBackground bg;
    CHECK(!BddBgLoad(bdd_path.c_str(), &bg));
    CHECK(!bg.loaded);
    CHECK(!bg.error.empty());
    CHECK(bg.error.find(".BDB") != std::string::npos);
    BddBgFree(&bg);
}

static void TestMissingFileFails(void)
{
    BddBackground bg;
    CHECK(!BddBgLoad(TmpPath("does_not_exist", ".BDD").c_str(), &bg));
    CHECK(!bg.loaded);
    CHECK(!bg.error.empty());

    CHECK(!BddBgLoad(NULL, &bg));
    CHECK(!bg.loaded);
    BddBgFree(&bg);
}

/* No 0x40/0x41 objects anywhere: the picker must still return the
   busiest module rather than -1, or a background-only stage opens
   pointing at nothing. */
static void TestPlayfieldModuleFallback(void)
{
    std::vector<unsigned char> bdd;
    Append(bdd, "1\n");
    Append(bdd, "1 1 1 0\n");
    bdd.push_back(1);
    Append(bdd, "P 2\n");
    AppendU16LE(bdd, Pack555(0, 0, 0));
    AppendU16LE(bdd, Pack555(31, 0, 0));

    std::vector<unsigned char> bdb;
    Append(bdb, "SKYONLY 64 64 255 2 1 3\n");
    Append(bdb, "FAR 0 9 0 9\n");
    Append(bdb, "NEAR 20 29 0 9\n");
    Append(bdb, "3200 0 0 1 0\n");    /* layer 0x32, far plane */
    Append(bdb, "3200 20 0 1 0\n");   /* two objects land in NEAR */
    Append(bdb, "3200 21 0 1 0\n");

    std::string bdd_path = TmpPath("skyonly", ".BDD");
    std::string bdb_path = TmpPath("skyonly", ".BDB");
    WriteFile(bdd_path, bdd);
    WriteFile(bdb_path, bdb);

    BddBackground bg;
    CHECK(BddBgLoad(bdd_path.c_str(), &bg));
    if (bg.loaded) {
        CHECK(bg.modules.size() == 2);
        CHECK(BddBgPlayfieldModule(&bg) == 1);   /* NEAR, the busier one */
    }
    BddBgFree(&bg);

    BddBackground empty;
    CHECK(BddBgPlayfieldModule(&empty) == -1);
    CHECK(BddBgPlayfieldModule(NULL) == -1);
}

/* Loading over a live background must not leak or leave stale fields. */
static void TestReloadReplaces(void)
{
    std::string a_bdd, a_bdb;
    BuildSimplePair("reload", &a_bdd, &a_bdb);

    BddBackground bg;
    CHECK(BddBgLoad(a_bdd.c_str(), &bg));
    CHECK(bg.w == 2 && bg.h == 2);

    /* A failed load clears what was there rather than half-updating it. */
    CHECK(!BddBgLoad(TmpPath("nope", ".BDD").c_str(), &bg));
    CHECK(!bg.loaded);
    CHECK(bg.w == 0 && bg.h == 0);
    CHECK(bg.modules.empty());
    CHECK(bg.stage_name.empty());
    BddBgFree(&bg);
}

int main(int argc, char **argv)
{
    /* Write scratch files next to the binary unless told otherwise, so a
       sandboxed CI run does not depend on a writable system temp. */
    g_tmp_prefix = (argc > 1) ? argv[1] : "bdd_bg_test_";

    TestLoadAndComposite();
    TestLoadFromBdbSide();
    TestFlipBits();
    TestDrawOrder();
    TestSkipsUnresolvedPlacements();
    TestMissingSiblingFails();
    TestMissingFileFails();
    TestPlayfieldModuleFallback();
    TestReloadReplaces();

    if (g_fails) {
        std::fprintf(stderr, "bdd_bg_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("bdd_bg_test: all checks passed\n");
    return 0;
}
