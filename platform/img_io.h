/*************************************************************
 * platform/img_io.h
 * File I/O declarations: IMG load/save, TGA/LBM/PNG import/export.
 *************************************************************/
#ifndef IMG_IO_H
#define IMG_IO_H

#include "img_format.h"

extern int  g_img_tex_idx;
extern char g_restore_msg[128];
extern float g_restore_msg_timer;

void LoadImgFile(void);
void SaveImgFile(void);
void WriteAnilstFromMarked(const char* filepath);
void BuildTgaFromMarked(const char* filepath);
void SaveTga(void);
void SaveLbm(void);
void LoadTga(void);
void LoadLbm(void);
void ImportPng(const char *path);
void ExportPng(const char *path);
int  RestoreMarkedFromSource(void);

#endif /* IMG_IO_H */
