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
};

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

#endif /* IMG_IO_H */
