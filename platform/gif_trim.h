/*************************************************************
 * platform/gif_trim.h
 * Border-trim analysis for imported RGBA animation frames.
 *
 * Split out of img_io.cpp's GIF importer because the original trim tested
 * alpha alone, which quietly made the "Trim Border" option a no-op on most
 * real files. stb_image only marks a pixel transparent when the GIF declares
 * a transparent index; an animation exported from video or a screen capture
 * declares none, so every pixel comes back at alpha 255, the content box is
 * the whole frame, and nothing is ever cropped.
 *
 * Alpha is still the preferred signal when the file carries it. A flat
 * background color is the fallback when it doesn't — with a tolerance, because
 * GIF quantization dithers even a "solid" background and an exact match finds
 * nothing on exactly the files this is meant to handle.
 *
 * Pure buffer math: no UI globals, no document state.
 *************************************************************/
#pragma once

enum GifTrimBasis {
    GifTrim_None = 0,   /* nothing to trim, or no usable background color */
    GifTrim_Alpha,      /* the file declares transparency; trim against it */
    GifTrim_Background  /* fully opaque; trim against the shared border color */
};

/* Most common color around the outer ring of every frame, as 8-bit RGB.
 * The ring rather than the whole image: a sprite that happens to be mostly one
 * color would otherwise nominate its own body as the background.
 * Returns false when there is no opaque border pixel to sample. */
bool GifBorderBackgroundColor(const unsigned char *rgba, int w, int h,
                              int frame_count, unsigned char out_rgb[3]);

/* Union content box across every frame, so a trim can never break the frames'
 * registration with each other. `tolerance` is per-channel 0..255 slack and
 * applies only on the background path.
 *
 * On GifTrim_None the outputs are untouched. `out_bg` (optional) receives the
 * background color that was used, which callers report so a no-op trim reads
 * as "nothing to remove" rather than "the option is broken". */
GifTrimBasis GifFramesContentBBox(const unsigned char *rgba, int w, int h,
                                  int frame_count, int tolerance,
                                  unsigned char out_bg[3],
                                  int *out_x0, int *out_y0,
                                  int *out_x1, int *out_y1);
