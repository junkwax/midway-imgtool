/*************************************************************
 * platform/globals.c
 *
 * Data definitions originally in IT/itimg.asm BSS section.
 * Moved to C so the ASM files can be removed from the build.
 *************************************************************/
#include "compat.h"
#include <SDL.h>

/* ---- Image list ---- */
void         *img_p       = NULL;
unsigned int  imgcnt      = 0;
int           ilselected  = -1;
unsigned int  imgdataw    = 0;
unsigned int  il1stprt    = 0;
int           ilpalloaded = -1;

/* ---- Second image list (dual-list switching) ---- */
void         *img2_p      = NULL;
unsigned int  img2cnt     = 0;
int           il2selected = -1;
unsigned int  il21stprt   = 0;

/* ---- Palette list ---- */
void         *pal_p       = NULL;
unsigned int  palcnt      = 0;
int           plselected  = -1;

/* ---- Counts ---- */
unsigned int  seqcnt      = 0;
unsigned int  scrcnt      = 0;
unsigned int  damcnt      = 0;
unsigned int  fileversion = 0;

/* ---- Sequence/script memory ---- */
void         *scrseqmem_p = NULL;
unsigned int  scrseqbytes = 0;

/* ---- File I/O paths (DOS-era 8.3 convention) ---- */
char          fpath_s[64]    = {0};
char          fname_s[13]    = {0};
char          fnametmp_s[13] = {0};

/* DOS DTA (Disk Transfer Area) — 43 bytes for findfile/findnext */
unsigned char dta[43]        = {0};
