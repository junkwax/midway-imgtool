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
void WriteAnilstFromMarked(const char* filepath);
void WriteTblFromMarked(const char* filepath, unsigned int base_address, bool mk3_format, bool include_pal, bool pad_4bit, bool align_16bit, bool dual_bank, int bank);
void WriteIrwFromMarked(const char* filepath, unsigned int base_address, int bpp, bool align_16bit);
void BuildTgaFromMarked(const char* filepath);
void SaveTga(const char *filepath);
void SaveLbm(const char *filepath);
void LoadTga(const char *filepath);
void LoadLbm(const char *filepath);
void ImportPng(const char *path);
void ImportPngMatch(const char *path);
void ExportPng(const char *path);
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

int  ChopMarkedImages(int grid_w, int grid_h, bool trim);

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

/* Align anipoints of all marked images to the anipoint of the image at
 * reference_idx. Conceptually: pick one frame as the "anchor frame", all
 * other marked frames are shifted (via anipoint) so they share that anchor.
 * Returns count of edited images. */
int  AlignAnipointsToMarked(int reference_idx);

#endif /* IMG_IO_H */
