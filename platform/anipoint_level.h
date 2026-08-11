/*************************************************************
 * platform/anipoint_level.h
 * Derive sensible animation points for frames that never got one.
 *
 * Imported and freshly created sprites land with anix/aniy = 0,0, which puts
 * the anchor in the top-left corner of the pixel rectangle. Played back, a run
 * of such frames jitters: each frame's art sits at a different height inside
 * its own box, and with the anchor pinned to the corner that difference goes
 * straight to the screen. Frames that were authored properly do not have this
 * problem, because their anipoint tracks a physical feature.
 *
 * The fix needs a reference for where the ground is, and the split is really
 * shipped-vs-added: STANCE frames come with the game, so their footing is
 * ground truth, while the frames being fixed are ones added afterwards that
 * have to match it. That is why the reference is the stance specifically and
 * not, say, whichever ground line the most frames happen to agree on -- a
 * majority of added frames being wrong together does not make them right.
 * So: find the STANCE frame, measure its ground line, and drop every unset
 * same-palette frame onto it.
 *
 * Palette is the grouping key rather than sprite size. In real libraries the
 * character's frames all share one palette while effects carry their own
 * (BOSS8.IMG: 11 frames on palette 3 for the boss, 12 on palette 4 for a spark
 * effect), whereas sizes vary frame to frame -- no two sprites in BOSS8 share
 * one. An effect has no feet and no business standing on the ground line, so it
 * is centred on its own art instead.
 *
 * Only frames whose anipoint is still 0,0 are touched. A frame that was
 * authored deliberately is left exactly as it is, even when it disagrees with
 * the stance: BOSS8's six BGHAMMERTOP frames agree with each other on a ground
 * line 15px below BGSTANCE1's, and "correct" there is a judgement call for a
 * human, not a bulk edit.
 *************************************************************/
#ifndef ANIPOINT_LEVEL_H
#define ANIPOINT_LEVEL_H

#include "img_format.h"   /* IMG */

#include <vector>

/* Row index of the lowest non-transparent pixel, or -1 when the sprite is
   entirely transparent (or has no pixel data). */
int anipoint_level_content_bottom(const IMG *img);

/* Centre of the non-transparent art, ignoring transparent padding. Returns
   false and leaves the outputs alone for an empty sprite. */
bool anipoint_level_content_center(const IMG *img, int *out_cx, int *out_cy);

/* An anipoint of exactly 0,0 is the "never set" sentinel: it is what import
   and sprite creation leave behind. Art genuinely anchored at its own top-left
   corner is vanishingly rare, and is the price of not needing a separate
   has-anipoint flag the IMG format does not carry. */
bool anipoint_level_is_unset(const IMG *img);

/* True when the 16-byte sprite name contains "STANCE", case-insensitively. */
bool anipoint_level_name_is_stance(const char *name);

/* Ground line of a frame: the row its art rests on, measured from the anipoint
   rather than from the frame origin, so it is comparable across frames of
   different heights. Returns false for an empty sprite. */
bool anipoint_level_ground_line(const IMG *img, int *out_ground);

enum class AnipointLevelAction {
    Skip,          /* left alone; see AnipointLevelPlanEntry::reason */
    GroundAlign,   /* same palette as the stance: stand it on the ground line */
    Center,        /* different palette: centre on its own art */
};

struct AnipointLevelPlanEntry {
    int  index      = -1;      /* position in the input list */
    AnipointLevelAction action = AnipointLevelAction::Skip;
    int  cur_anix   = 0;
    int  cur_aniy   = 0;
    int  new_anix   = 0;
    int  new_aniy   = 0;
    int  cur_ground = 0;       /* GroundAlign only: where it stands now */
    int  new_ground = 0;       /* GroundAlign only: where it will stand */
    const char *reason = "";   /* static string, safe to store */
};

struct AnipointLevelPlan {
    bool has_reference     = false;
    int  reference_index   = -1;
    int  reference_pal     = -1;
    int  ground_line       = 0;
    std::vector<AnipointLevelPlanEntry> entries;  /* one per changed frame */
    int  ground_count      = 0;
    int  center_count      = 0;
    int  skipped_anchored  = 0;  /* already had an anipoint */
    int  skipped_empty     = 0;  /* nothing opaque to measure */
};

/* Work out what should change. Nothing is written -- the caller applies the
   plan, so the same computation can drive a preview. */
AnipointLevelPlan AnipointLevelBuildPlan(const std::vector<IMG *> &imgs);

#endif /* ANIPOINT_LEVEL_H */
