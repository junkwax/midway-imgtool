/*************************************************************
 * platform/stamp_erase.h
 *
 * Find a copied sprite inside another frame and subtract it.
 *
 * Midway's effect art is layered by hand: UMK3FIRE.IMG holds a skeleton that
 * appears verbatim inside fourteen flame frames, with the flame painted over
 * the top of it. Pulling the skeleton back out is a search problem, not a
 * geometry problem — the frames are different sizes, the anipoints disagree,
 * and the only thing that reliably identifies the object is its own pixels.
 *
 * So the match is driven by content, and it has to survive the object being
 * mostly repainted: measured against the real UMK3FIRE art, only 21-35% of the
 * skeleton's pixels come through the flame byte-identical. Pixels sampled from
 * all over the object each vote for the placement they would imply, the
 * best-supported placements are scored in full, and the winner is walked to its
 * exact position. Erasing then clears only pixels whose index is identical to
 * the stamp's, which is what leaves the flame behind — wherever it covers the
 * skeleton the indices differ and the pixel stays.
 *
 * Everything here works on raw indexed buffers and plain ints — no g_doc, no
 * UI globals, no undo — so it is unit-testable on its own.
 *************************************************************/
#ifndef PLATFORM_STAMP_ERASE_H
#define PLATFORM_STAMP_ERASE_H

/* Non-owning view of an indexed 8-bit bitmap. Index 0 is transparent. */
struct StampBuf {
    const unsigned char *pixels;
    int w, h, stride;
};

/* One evaluated placement of the stamp over the target. */
struct StampMatch {
    bool valid;       /* a placement was found and scored */
    int  dx, dy;      /* stamp pixel (0,0) sits at target (dx, dy) */
    int  matched;     /* opaque stamp pixels whose index equals the target's */
    int  covered;     /* opaque stamp pixels that land inside the target */
    int  total;       /* opaque pixels in the stamp (denominator for the score) */
    int  candidates;  /* placements scored — diagnostics for the UI */

    StampMatch()
        : valid(false), dx(0), dy(0), matched(0), covered(0), total(0),
          candidates(0) {}
};

/* Fraction of the stamp's opaque pixels found at their expected index, 0..1.
   0 when the stamp is empty or the match is invalid. */
float StampMatchScore(const StampMatch &m);

/* Opaque (index != 0) pixel count. */
int StampOpaqueCount(const StampBuf &stamp);

/* Score the one placement at (dx, dy). Always returns valid=true for a
   non-empty stamp, even when nothing matches — the caller decides what score
   is good enough. */
StampMatch StampScoreAt(const StampBuf &stamp, const StampBuf &target,
                        int dx, int dy);

/* Best placement within `radius` pixels of (dx0, dy0), searched exhaustively.
   A negative radius searches every placement with any overlap at all, which is
   thorough but O(w*h) placements — prefer StampSearchAuto for that. */
StampMatch StampSearchWindow(const StampBuf &stamp, const StampBuf &target,
                             int dx0, int dy0, int radius);

/* Content-driven search over the whole target, with no alignment hint.
 *
 * Samples pixels from across the stamp, preferring colors the target rarely
 * uses, and has each vote for the placement it would imply. Votes are weighted
 * by how unusual the color is, and no single color may cast many, so a large
 * flat region cannot outvote the rest of the object. The best-supported
 * placements are then scored in full and the winner walked to its local
 * optimum. Measured at ~1.3 ms per frame against real UMK3FIRE sprites, and
 * agreeing with an exhaustive search on 24 of its 26 frames (the two it misses
 * are frames the object is not in).
 *
 * `max_candidates` caps how many placements get fully scored (<= 0 uses a
 * sensible default). Returns valid=false when the stamp is empty or holds no
 * index the target has at all — i.e. the object is definitely not here. */
StampMatch StampSearchAuto(const StampBuf &stamp, const StampBuf &target,
                           int max_candidates);

/* Clear (set to index 0) every target pixel that the stamp covers with the
   same index at placement (dx, dy). Pixels where the two disagree — the flame
   painted over the object — are left alone, as are pixels the stamp leaves
   transparent. Returns how many pixels changed. */
int StampEraseAt(unsigned char *target, int target_w, int target_h,
                 int target_stride, const StampBuf &stamp, int dx, int dy);

#endif /* PLATFORM_STAMP_ERASE_H */
