/*************************************************************
 * platform/img_io.h
 * File I/O declarations: IMG load/save, TGA/LBM/PNG import/export.
 *************************************************************/
#ifndef IMG_IO_H
#define IMG_IO_H

#include "img_format.h"
#include <vector>
#include <string>

struct BulkRestoreMatch {
    IMG* child;
    IMG* parent;
    bool selected;
    /* OOB diagnostics, populated by ComputeBulkRestoreCoverage:
     *   covered_pixels = pixels of child that fall inside parent rect after
     *                    anipoint-relative dx/dy shift
     *   total_pixels   = child->w * child->h
     * Coverage < 100% means the copy will be partial: Pairs mode zero-fills
     * the uncovered region (potentially destroying hand-tuned detail), Diff
     * mode leaves it untouched. Either way, dx/dy may indicate an anipoint
     * mismatch the user should review before clicking Start. */
    int covered_pixels;
    int total_pixels;
};

/* Fills covered_pixels / total_pixels for every match in the vector.
 * Cheap (just rect-clip math, no pixel walk) so it's safe to call in
 * the preview/test step. */
void ComputeBulkRestoreCoverage(std::vector<BulkRestoreMatch>& matches);

extern int  g_img_tex_idx;
extern char g_restore_msg[128];
extern float g_restore_msg_timer;
extern bool g_verbose;
extern int g_load2_ppp;
extern bool g_load2_limit_scales_to_3;
extern std::vector<std::string> g_log_lines;

void verbose_log(const char *fmt, ...);

void LoadImgFile(void);
void SaveImgFile(void);
/* Flatten per-sprite overlay layers onto their images just for the file write,
   then restore the pre-flatten pixels so layers stay editable. Implemented in
   imgui_overlay.cpp; called by SaveImgFile around the pixel write. */
void FlattenLayersForSave(void);
void RestoreLayersAfterSave(void);
/* Read an IMG's frame names from its header records only (no pixel load). */
void ProbeImgFrameNames(const char *path, std::vector<std::string> &out);
/* Full-document undo snapshot (captures all images' pixels/geometry/order).
   Implemented in imgui_overlay.cpp; used by geometry-changing IO ops. */
bool doc_undo_push(void);
void WriteAnilstFromMarked(const char* filepath);
void WriteTblFromMarked(const char* filepath, unsigned int base_address, bool mk3_format, bool include_pal, bool pad_4bit, bool align_16bit, bool dual_bank, int bank);
void WriteIrwFromMarked(const char* filepath, unsigned int base_address, int bpp, bool align_16bit);
void BuildTgaFromMarked(const char* filepath);
bool BuildImageExportRgba(const IMG *img, std::vector<unsigned char> &rgba,
                          int *w_out, int *h_out);
void SaveTga(const char *filepath);
void SaveLbm(const char *filepath);
void LoadTga(const char *filepath);
void LoadLbm(const char *filepath);
void ImportPng(const char *path);
/* Index a PNG against an existing palette instead of building a new one.
   `palnum` selects the palette; pass -1 to resolve it from the current
   selection. Resolve it once and pass it explicitly when importing several
   files in a row, so every file lands in the same palette regardless of what
   the earlier imports did to the selection. Returns true if an image was
   added. */
bool ImportPngMatch(const char *path, int palnum = -1);
/* The palette ImportPngMatch(-1) would use: the selected image's palette, then
   the selected palette, then palette 0. Returns -1 when the document has no
   usable palette at all. */
int  ResolveImportMatchPalette(void);

enum SpriteSheetDetectMode {
    SpriteSheetDetect_Auto = 0,
    SpriteSheetDetect_Islands = 1,
};

struct SpriteSheetImportOptions {
    int  detect_mode;
    int  background_threshold;
    int  min_pixels;
    int  padding;
    bool crop;
    char name_prefix[12];
};

struct SpriteSheetDebugFrame {
    int x0, y0, x1, y1; /* inclusive source bounds */
    int pixels;
    int islands;
    int row;
    int frame;
};

struct SpriteSheetDebugReport {
    int sheet_w;
    int sheet_h;
    int raw_islands;
    int accepted_frames;
    int line_rows;
    int line_cols;
    std::vector<SpriteSheetDebugFrame> frames;
};

int ImportSpriteSheetMatch(const char *path, const SpriteSheetImportOptions *options);
int AnalyzeSpriteSheet(const char *path, const SpriteSheetImportOptions *options,
                       SpriteSheetDebugReport *report);
int DebugSpriteSheetImport(const char *path, const char *output_dir,
                           const SpriteSheetImportOptions *options,
                           SpriteSheetDebugReport *report);

enum GifBlendMode {
    GifBlend_Normal = 0,
    GifBlend_Dissolve,
    GifBlend_Darken,
    GifBlend_Multiply,
    GifBlend_ColorBurn,
    GifBlend_LinearBurn,
    GifBlend_Lighten,
    GifBlend_Screen,
    GifBlend_ColorDodge,
    GifBlend_Overlay,
    GifBlend_SoftLight,
    GifBlend_HardLight,
    GifBlend_Difference,
    GifBlend_Exclusion,
    GifBlend_Count
};
const char *GifBlendModeName(int mode);
void ImportGif(const char *path, int blend_mode, int opacity_percent, bool import_all_frames,
               bool trim_transparent_border);
void ExportPng(const char *path);
/* Write a row-major RGBA8 buffer straight to a PNG. Used by callers that build
   their own composite (World View export) rather than exporting one IMG. */
bool WriteRgbaPng(const char *path, int w, int h, const unsigned char *rgba);
bool ExportAnimatedGif(const char *path, const std::vector<int> &frames,
                       const std::vector<int> &holds, float fps,
                       bool loop, bool pingpong, bool align_anipoints);
void ExportPalette(const char *path, bool adobe_act);
void ImportPalette(const char *path);
int  RestoreMarkedFromSource(void);
int  RestoreMarkedFromSourceForce(void);
int  ExecuteBulkRestorePairs(const std::vector<BulkRestoreMatch>& matches);
int  ExecuteBulkRestoreDiff (const std::vector<BulkRestoreMatch>& matches);
/* Reconstruct mode: parent is treated as ground truth. For each child
 * pixel, if it differs from the parent (after anipoint-relative dx/dy
 * shift), copy the parent's pixel into the child. Useful for restoring
 * censored/blacked-out regions in shipping art where the master sprite
 * still carries the original detail. Out-of-overlap child pixels are
 * untouched (unlike Pairs mode's zero-fill). */
int  ExecuteBulkRestoreReconstruct(const std::vector<BulkRestoreMatch>& matches);

int  ChopMarkedImages(int grid_w, int grid_h, bool trim); /* marked sprites, or selected sprite if none are marked */

/* Edge defringe for all marked images.
 * For each pixel within `radius` pixels of a transparent boundary (8-connected),
 * if its 8-neighborhood contains both transparent and opaque pixels, replace
 * it with the average palette index of its non-transparent neighbors. This
 * kills the 1-2px halo of bluescreen/greenscreen spill that survives chroma
 * removal on digitized actor sprites. Returns count of edited pixels. */
int  DefringeMarkedImages(int radius);

/* Crop each marked image to its non-transparent bounding box. Anipoints are
 * adjusted so the visual rendering stays identical. Returns count of edited
 * images. */
int  CropMarkedImagesToContent(void);
int  CropSelectedImageToContent(void);

/* Align anipoints of all marked images to the anipoint of the image at
 * reference_idx. Conceptually: pick one frame as the "anchor frame", all
 * other marked frames are shifted (via anipoint) so they share that anchor.
 * Returns count of edited images. */
int  AlignAnipointsToMarked(int reference_idx);

/* Mirror marked images' anipoint X coordinates for reverse-facing sprites.
 * Uses the same horizontal anchor convention as World View mirroring:
 * mirrored_x = image_width - x. Y/Z values are left unchanged. */
int  MirrorMarkedAnipointsToReverse(void);

/* Recolor every sprite except source_idx so its opaque pixels use only
 * palette indices that are present in the source sprite. Each target pixel is
 * converted through its current palette RGB, matched to the nearest source
 * sprite color, then the target is assigned to the source palette. Returns
 * the number of images that would change and optionally reports byte-level
 * pixel writes through pixels_changed_out. */
int  PreviewMatchAllSpritesToSourceColors(int source_idx, int *pixels_changed_out);

/* Apply the operation described above. Returns the number of images changed
 * and optionally reports byte-level pixel writes through pixels_changed_out. */
int  MatchAllSpritesToSourceColors(int source_idx, int *pixels_changed_out);

#endif /* IMG_IO_H */
