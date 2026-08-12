/*************************************************************
 * platform/anipoint_level.cpp
 * Implementation of the stance-referenced anipoint leveller.
 * See anipoint_level.h for why the reference is the stance frame.
 *************************************************************/
#include "anipoint_level.h"

#include <cstring>

/* Rows are padded out to a 4-byte boundary, same as everywhere else. */
static int level_stride(int w)
{
    return (w + 3) & ~3;
}

static const unsigned char *level_pixels(const IMG *img)
{
    if (!img || !img->data_p) return nullptr;
    if (img->w == 0 || img->h == 0) return nullptr;
    return (const unsigned char *)img->data_p;
}

/* Sign-extend an anipoint word. The format stores these as unsigned shorts but
   they are signed offsets -- an anchor above the frame origin is normal. */
static int level_signed(unsigned short v)
{
    return (int)(short)v;
}

int anipoint_level_content_bottom(const IMG *img)
{
    const unsigned char *px = level_pixels(img);
    if (!px) return -1;

    const int w = (int)img->w;
    const int h = (int)img->h;
    const int stride = level_stride(w);

    for (int y = h - 1; y >= 0; y--) {
        const unsigned char *row = px + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; x++) {
            if (row[x] != 0) return y;   /* index 0 is transparent */
        }
    }
    return -1;
}

bool anipoint_level_content_center(const IMG *img, int *out_cx, int *out_cy)
{
    const unsigned char *px = level_pixels(img);
    if (!px) return false;

    const int w = (int)img->w;
    const int h = (int)img->h;
    const int stride = level_stride(w);

    int min_x = w, min_y = h, max_x = -1, max_y = -1;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = px + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; x++) {
            if (row[x] == 0) continue;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }
    if (max_x < 0) return false;

    /* Round up, not down. Checked against all twelve hand-authored BGSPARK
       frames in BOSS8: every one has anix == w/2 and aniy == h/2, which for
       art spanning 0..w-1 is (min+max+1)/2. Truncating instead lands a pixel
       left and up on every even dimension. */
    if (out_cx) *out_cx = (min_x + max_x + 1) / 2;
    if (out_cy) *out_cy = (min_y + max_y + 1) / 2;
    return true;
}

bool anipoint_level_is_unset(const IMG *img)
{
    if (!img) return false;
    return img->anix == 0 && img->aniy == 0;
}

bool anipoint_level_name_is_stance(const char *name)
{
    if (!name) return false;

    /* The name field is 16 bytes and real game files reuse the bytes past the
       terminator as scratch ("JCSTANCE1\0vda  \0"), so stop at the NUL rather
       than scanning the whole field -- otherwise trailing garbage could match. */
    char upper[17];
    size_t n = 0;
    while (n < 16 && name[n] != '\0') {
        char c = name[n];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        upper[n] = c;
        n++;
    }
    upper[n] = '\0';
    return std::strstr(upper, "STANCE") != nullptr;
}

bool anipoint_level_ground_line(const IMG *img, int *out_ground)
{
    int bottom = anipoint_level_content_bottom(img);
    if (bottom < 0) return false;
    if (out_ground) *out_ground = bottom - level_signed(img->aniy);
    return true;
}

AnipointLevelPlan AnipointLevelBuildPlan(const std::vector<IMG *> &imgs)
{
    AnipointLevelPlan plan;

    /* First stance frame in list order wins. Libraries that carry several
       (STANCE1..STANCE8) authored them against one ground line, so the choice
       between them does not matter; taking the first keeps the result stable
       regardless of what happens to be selected. */
    for (size_t i = 0; i < imgs.size(); i++) {
        const IMG *img = imgs[i];
        if (!img || !anipoint_level_name_is_stance(img->n_s)) continue;

        int ground = 0;
        if (!anipoint_level_ground_line(img, &ground)) continue;  /* empty; keep looking */

        plan.has_reference   = true;
        plan.reference_index = (int)i;
        plan.reference_pal   = (int)img->palnum;
        plan.ground_line     = ground;
        break;
    }
    if (!plan.has_reference) return plan;

    for (size_t i = 0; i < imgs.size(); i++) {
        IMG *img = imgs[i];
        if (!img) continue;
        if ((int)i == plan.reference_index) continue;   /* never move the reference */

        if (!anipoint_level_is_unset(img)) {
            plan.skipped_anchored++;
            continue;
        }

        int cx = 0, cy = 0;
        if (!anipoint_level_content_center(img, &cx, &cy)) {
            plan.skipped_empty++;
            continue;
        }

        AnipointLevelPlanEntry e;
        e.index    = (int)i;
        e.cur_anix = level_signed(img->anix);
        e.cur_aniy = level_signed(img->aniy);

        if ((int)img->palnum == plan.reference_pal) {
            /* Same character: stand it on the stance's ground line.

               Y only. X is deliberately left alone, because it is not
               derivable from the pixels. Measured against the hand-authored
               BOSS8 frames, the best geometric guess (horizontal centroid of
               the bottom rows -- the feet) still misses by 12.6px on average,
               and the BGFATHAM run is off by a near-constant 22px across all
               seven frames: that constant is the character travelling across
               the screen during the fatality. anix encodes authored world
               motion, so any value computed from a bounding box overwrites
               animation with geometry. Frame centre is worse still (19.8px).

               Leaving it means an unset frame keeps anix = 0 and still needs a
               human, which is honest -- the Y fix alone stops the vertical
               bobbing, and the caller reports that X was not touched. */
            int bottom = anipoint_level_content_bottom(img);
            e.action     = AnipointLevelAction::GroundAlign;
            e.new_anix   = e.cur_anix;
            e.new_aniy   = bottom - plan.ground_line;
            e.cur_ground = bottom - e.cur_aniy;
            e.new_ground = plan.ground_line;
            e.reason     = "same palette as stance: Y aligned to ground line, X left for authoring";
            plan.ground_count++;
        } else {
            /* Different palette: an effect, not the character. It has no feet,
               so standing it on the ground line would be meaningless -- centre
               it on its own art so it lands where it is placed. */
            e.action   = AnipointLevelAction::Center;
            e.new_anix = cx;
            e.new_aniy = cy;
            e.reason   = "different palette: centred on its own art";
            plan.center_count++;
        }

        if (e.new_anix == e.cur_anix && e.new_aniy == e.cur_aniy) {
            /* Already exactly right; do not report a no-op as a change. */
            if (e.action == AnipointLevelAction::GroundAlign) plan.ground_count--;
            else plan.center_count--;
            continue;
        }
        plan.entries.push_back(e);
    }

    return plan;
}
