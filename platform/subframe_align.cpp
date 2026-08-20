/*************************************************************
 * platform/subframe_align.cpp
 * Subframe anipoint re-derivation declared in subframe_align.h.
 *************************************************************/
#include "subframe_align.h"

float subframe_align_score(const SubframeAlign &a)
{
    if (!a.valid || a.total <= 0) return 0.0f;
    return (float)a.matched / (float)a.total;
}

void subframe_claimed_offset(int parent_anix, int parent_aniy,
                             int child_anix, int child_aniy,
                             int *out_x, int *out_y)
{
    if (out_x) *out_x = parent_anix - child_anix;
    if (out_y) *out_y = parent_aniy - child_aniy;
}

static SubframeAlign from_match(const StampMatch &m,
                                int parent_anix, int parent_aniy)
{
    SubframeAlign a;
    if (!m.valid) return a;
    a.valid    = true;
    a.off_x    = m.dx;
    a.off_y    = m.dy;
    a.new_anix = parent_anix - m.dx;
    a.new_aniy = parent_aniy - m.dy;
    a.matched  = m.matched;
    a.total    = m.total;
    a.exact    = m.total > 0 && m.matched == m.total;
    return a;
}

SubframeAlign subframe_align_from_parent(const StampBuf &parent,
                                         int parent_anix, int parent_aniy,
                                         const StampBuf &child,
                                         int child_anix, int child_aniy)
{
    SubframeAlign none;
    if (!parent.pixels || !child.pixels) return none;
    if (parent.w <= 0 || parent.h <= 0 || child.w <= 0 || child.h <= 0)
        return none;
    if (StampOpaqueCount(child) <= 0) return none;

    /* Score what the file already claims first. Art that was never disturbed
       returns its own offset for the cost of one comparison, so running this
       over a whole library is cheap and idempotent. */
    int claim_x = 0, claim_y = 0;
    subframe_claimed_offset(parent_anix, parent_aniy, child_anix, child_aniy,
                            &claim_x, &claim_y);
    StampMatch claimed = StampScoreAt(child, parent, claim_x, claim_y);
    if (claimed.valid && claimed.total > 0 && claimed.matched == claimed.total)
        return from_match(claimed, parent_anix, parent_aniy);

    /* A chopped piece always fits wholly inside its parent, so that box is the
       whole search space -- (pw-cw+1) * (ph-ch+1) placements, not the full
       overlap sweep. */
    if (child.w <= parent.w && child.h <= parent.h) {
        StampMatch best = claimed;
        for (int dy = 0; dy + child.h <= parent.h; dy++) {
            for (int dx = 0; dx + child.w <= parent.w; dx++) {
                StampMatch m = StampScoreAt(child, parent, dx, dy);
                if (!m.valid) continue;
                if (!best.valid || m.matched > best.matched) {
                    best = m;
                    if (m.total > 0 && m.matched == m.total)
                        return from_match(best, parent_anix, parent_aniy);
                }
            }
        }
        return from_match(best, parent_anix, parent_aniy);
    }

    /* Bigger than its parent in some axis: not a plain chop, so fall back to
       the content-driven search, which can place a piece that overhangs. */
    StampMatch auto_match = StampSearchAuto(child, parent, 0);
    if (!auto_match.valid) return from_match(claimed, parent_anix, parent_aniy);
    if (claimed.valid && claimed.matched >= auto_match.matched)
        return from_match(claimed, parent_anix, parent_aniy);
    return from_match(auto_match, parent_anix, parent_aniy);
}
