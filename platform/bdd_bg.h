/*************************************************************
 * platform/bdd_bg.h
 *
 * Read-only BDD/BDB stage loader for the World View reference
 * background. Ported from midway-bddtool's `bdd_core` (parsing)
 * and its `render` command (compositing).
 *
 * Deliberately read-only: imgtool loads a stage to look at it
 * behind an animation, never to edit it. There is no save path
 * here — bddtool owns authoring.
 *
 * The pair matters. A .BDD holds the pixels (images + palettes);
 * a .BDB holds every placement. Neither alone draws a stage, so
 * BddBgLoad() takes either one and resolves its sibling by stem.
 *
 * What this can and cannot show
 * -----------------------------
 * Objects composite at their raw (depth, sy) world coordinates,
 * which is exactly what the files say. It is NOT the game camera:
 * real MK2 parallax lives in BGND.ASM (`<stage>_scroll` rates,
 * per-plane `.word x,y` offsets, `dlists_<stage>` draw order, and
 * a floor layer that is not in the BDD at all). Without that
 * source the honest result is a correctly-assembled, correctly-
 * coloured stage that the user positions by hand.
 *
 * Because the modules are packed as stacked bands rather than
 * overlaid planes, the whole-world composite is a tall atlas.
 * Modules are therefore exposed individually: each is one
 * parallax plane, and BddBgPlayfieldModule() picks the one the
 * fighters stand on so a freshly loaded stage lands somewhere
 * useful instead of on an empty band.
 *************************************************************/
#ifndef BDD_BG_H
#define BDD_BG_H

#include <SDL.h>
#include <string>
#include <vector>

/* One BDB module line: a rectangular region of the world that the
   original toolchain packaged as a single assembly module, which
   at runtime is one parallax plane. */
struct BddBgModule {
    std::string name;
    int x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    int object_count = 0;      /* objects whose rect fits inside this one */
    int playfield_objects = 0; /* of those, ones on DMA layer 0x40/0x41 */

    /* Bounds of what the module's objects actually cover, which is
       routinely smaller than the declared rect — DPUL5 in DEDPOOL claims
       418 rows and paints about 350. Aligning art to the playfield floor
       needs the painted edge; the declared one leaves a dead band. */
    bool has_content = false;
    int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
};

struct BddBackground {
    bool loaded = false;
    std::string bdd_path;
    std::string bdb_path;
    std::string stage_name;
    int world_w = 0;
    int world_h = 0;

    /* The composited stage, cropped to its non-transparent bounds:
       a declared world is routinely far larger than the art in it
       (DEDPOOL declares 4000x3000 and fills 2364x2077), and the
       empty margin is texture we would upload for nothing.
       content_x/content_y are the world coordinates of the
       buffer's top-left pixel, so world space is recoverable. */
    int content_x = 0, content_y = 0;
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;  /* row-major RGBA8, w*h*4 */

    std::vector<BddBgModule> modules;
    int object_count = 0;
    int image_count = 0;
    int palette_count = 0;
    int skipped_objects = 0;   /* placements naming a missing image/palette */

    std::string error;         /* set when loaded == false */

    SDL_Texture *tex = nullptr;  /* built lazily by BddBgTexture() */
};

/* Load the stage `path` belongs to. `path` may be the .BDD or the
   .BDB; the sibling is resolved by stem in the same directory,
   trying the case of the given path first. Any previous contents
   of `out` are freed. Returns false and fills out->error on
   failure, including when the sibling is missing. */
bool BddBgLoad(const char *path, BddBackground *out);

/* Release pixels and texture. Safe on an already-empty struct. */
void BddBgFree(BddBackground *bg);

/* The composited stage as a renderer texture, created on first use
   and cached. Returns NULL when nothing is loaded or the upload
   fails. Owned by `bg` — freed by BddBgFree(), never by callers. */
SDL_Texture *BddBgTexture(BddBackground *bg);

/* Index into bg->modules of the plane the fighters stand on: the
   module holding the most DMA layer 0x40/0x41 objects, which is
   the 1.0x-scroll playfield plane in every shipped stage. Falls
   back to the module with the most objects, then -1 when the file
   declares none. */
int BddBgPlayfieldModule(const BddBackground *bg);

#endif
