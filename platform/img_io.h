/*************************************************************
 * platform/img_io.h
 * File I/O declarations: IMG load/save, TGA/LBM/PNG import/export.
 *************************************************************/
#ifndef IMG_IO_H
#define IMG_IO_H

#include "img_format.h"
#include <vector>

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

void LoadImgFile(void);
void SaveImgFile(void);
void WriteAnilstFromMarked(const char* filepath);
void WriteTblFromMarked(const char* filepath, unsigned int base_address, bool mk3_format, bool include_pal);
void BuildTgaFromMarked(const char* filepath);
void SaveTga(void);
void SaveLbm(void);
void LoadTga(void);
void LoadLbm(void);
void ImportPng(const char *path);
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

#endif /* IMG_IO_H */
