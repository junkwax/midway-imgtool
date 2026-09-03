/*************************************************************
 * platform/digitize_matte.h
 *
 * Pull a digitized actor off the backdrop they were shot against.
 *
 * Midway's character art started as video: an actor on a flat backdrop,
 * frame-grabbed, then keyed. Everything downstream — the material masks, the
 * ramp fit in palette_ramp.h, the remap — assumes it is looking at the actor's
 * own colors. It usually isn't. A frame that has been keyed and flattened to
 * PNG carries a rim of pixels that are part actor and part backdrop, and those
 * pixels are the single biggest reason a naive import looks wrong: they are a
 * continuum of tints no two of which are alike, a quantizer faithfully spends
 * palette slots describing them, and against an arena background they read as
 * a halo the original art does not have.
 *
 * So the key here does two things, not one. It decides how much of each pixel
 * was backdrop (alpha), and then it takes the backdrop back out of the pixels
 * where the answer was "some of it" — solving C = a*F + (1-a)*B for F instead
 * of leaving the blend in place. A pixel that is half actor and half white
 * comes back as the actor's color at full strength, and the halo stops being
 * something the palette has to pay for.
 *
 * The DMA hardware has exactly one transparent index, so the soft alpha this
 * produces is a working value, not a deliverable: MatteBinarize cuts it to a
 * 1-bit mask once the color work is done.
 *
 * Order matters. Key and decontaminate at the source resolution, downscale
 * with MatteDownscale (which weights color by alpha, so the backdrop cannot
 * creep back in through the filter), and only then segment and quantize.
 * Downscaling after indexing re-indexes a blur; downscaling without alpha
 * weighting reintroduces the halo that was just removed.
 *
 * Pure buffers and plain ints throughout — no g_doc, no UI globals, no undo.
 *************************************************************/
#ifndef PLATFORM_DIGITIZE_MATTE_H
#define PLATFORM_DIGITIZE_MATTE_H

/* How to separate actor from backdrop.
 *
 * The key is a distance threshold in 8-bit RGB, not a hue test, so it works
 * for the blue cyclorama the arcade art was shot against and equally for a
 * frame someone has already cut out and flattened onto white. */
struct MatteParams {
    /* The backdrop color. For flattened art this is whatever it was flattened
       onto — usually white or black. */
    unsigned char key_r, key_g, key_b;

    /* Pixels within this RGB distance of the key are pure backdrop. */
    int  key_tolerance;

    /* Width of the ramp above key_tolerance over which a pixel goes from pure
       backdrop to pure actor. 0 gives a hard key with no partial pixels (and
       so nothing for decontamination to do). */
    int  key_softness;

    /* Undo the backdrop blend in partial pixels. Correct when the source was
       flattened onto the key color, which is the usual case for art exported
       as PNG. Turn it off when the source carries real alpha whose RGB is
       already the actor's own color. */
    bool decontaminate;

    /* Pull a saturated key's hue back out of the actor. Only meaningful when
       the key has a dominant channel (a blue or green screen); a white, black,
       or gray key leaves this a no-op however it is set. */
    bool despill;
    int  despill_strength;   /* 0..100 */

    /* Where MatteBinarize cuts soft alpha to the one transparent index. */
    int  alpha_threshold;    /* 0..255 */

    /* Erode the binary mask by this many pixels. One step trims the last rank
       of half-covered edge pixels, which the original art does not have. */
    int  shrink;
};

/* Key on white, medium softness, decontamination on, no despill: the settings
   for a frame someone cut out by hand and saved as a PNG. */
MatteParams MatteParamsDefault(void);

/* What the key did, for the UI to report. */
struct MatteStats {
    int keyed;        /* pixels the key called pure backdrop */
    int partial;      /* pixels it placed between the two extremes */
    int opaque;       /* pixels it called pure actor */
    int decontam;     /* partial pixels whose color decontamination changed */
    int despilled;    /* pixels the despill pass pulled toward neutral */
};

/* Key `rgba` (w*h*4, straight alpha) against the backdrop in `p`.
 *
 * `out_rgb` receives w*h*3 decontaminated actor colors and `out_alpha` w*h
 * coverage values. Any alpha already on the source is honored: the key's own
 * answer is multiplied by it, so a pixel the file already called transparent
 * stays transparent. Fully transparent output pixels get a color of 0.
 *
 * `stats` may be NULL. */
void MatteExtract(const unsigned char *rgba, int w, int h,
                  const MatteParams &p,
                  unsigned char *out_rgb,
                  unsigned char *out_alpha,
                  MatteStats *stats);

/* Cut soft alpha to the 1-bit mask the DMA can actually store: 1 where alpha
   is at or above `threshold`, then eroded `shrink` times with a 4-neighbour
   kernel (outside the image counts as transparent). Returns the number of
   pixels left set. `out_mask` is w*h bytes of 0 or 1. */
int MatteBinarize(const unsigned char *alpha, int w, int h,
                  int threshold, int shrink, unsigned char *out_mask);

/* Area-average `rgb`/`alpha` (sw x sh) down to dw x dh, weighting color by
   coverage so transparent pixels contribute none of their color to the
   result. Alpha is averaged by area alone. Destination pixels no source
   coverage reaches come back fully transparent and black.
 *
 * This is the resize that belongs between the key and the quantizer: the
 * source frames are several times the size the sprite ships at, and averaging
 * color without alpha weights would blend the backdrop straight back into the
 * edge that MatteExtract just cleaned. */
void MatteDownscale(const unsigned char *rgb, const unsigned char *alpha,
                    int sw, int sh,
                    unsigned char *out_rgb, unsigned char *out_alpha,
                    int dw, int dh);

#endif /* PLATFORM_DIGITIZE_MATTE_H */
