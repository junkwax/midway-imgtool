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

/* ---- Sequence/script memory ----
 * Raw byte blob of all SEQSCR records and their ENTRY arrays, captured
 * verbatim from the source file so save can write it back unchanged.
 * Lives in [oset + imgcnt*IMAGE_disk + (palcnt-NUMDEFPAL)*PALETTE_disk ..)
 * on disk — NOT at the start of the file. */
void         *scrseqmem_p = NULL;
unsigned int  scrseqbytes = 0;

/* ---- LIB_HDR fields preserved verbatim from load ----
 * The original DOS imgtool clobbers bufscr to -1 and zeroes spare1/2/3 on
 * save. But real game-asset pipelines populate bufscr with script-buffer
 * indices that LOAD2 reads to compute IRW layout. Clobbering bufscr causes
 * LOAD2 to emit a slightly different IRW size, shifting the start of every
 * subsequent character's sprites in the bank — producing visible "Cage
 * sprites overflow into Baraka" artifacts. Preserve verbatim. */
unsigned char file_bufscr[4]    = { 0xFF, 0xFF, 0xFF, 0xFF };
unsigned short file_spare1      = 0;
unsigned short file_spare2      = 0;
unsigned short file_spare3      = 0;

/* ---- File I/O paths (DOS-era 8.3 convention) ---- */
char          fpath_s[64]    = {0};
char          fname_s[13]    = {0};
char          fnametmp_s[13] = {0};

/* DOS DTA (Disk Transfer Area) — 43 bytes for findfile/findnext */
unsigned char dta[43]        = {0};
