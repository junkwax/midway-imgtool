/*************************************************************
 * platform/img_format.h
 * IMG/PAL data structures, allocators, and palette helpers.
 * Extracted from imgui_overlay.cpp.
 *************************************************************/
#ifndef IMG_FORMAT_H
#define IMG_FORMAT_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* Structure definitions matching wmpstruc.inc */
#pragma pack(push, 2)
struct IMG {
    void          *nxt_p;
    char           n_s[16];
    unsigned short flags;
    unsigned short anix;
    unsigned short aniy;
    unsigned short w;
    unsigned short h;
    unsigned short palnum;
    void          *data_p;
    void          *pttbl_p;     /* point table pointer or NULL */
    unsigned short anix2;
    unsigned short aniy2;
    unsigned short aniz2;
    unsigned short opals;
    void          *temp;        /* temp for offset when saving */
    /* Debug-only: disk file fields (populated during load, used during save) */
    unsigned int   file_oset;   /* offset in file of this image */
    unsigned int   file_data;   /* IMAGE_disk.data (+32) — unused by runtime
                                   but part of the on-disk record; real files
                                   carry non-zero values here */
    unsigned short file_lib;    /* library handle index */
    unsigned short file_frm;    /* frame number for anim — 0xFFFF in real
                                   game files; 0 in DOS-imgtool-saved ones */
    unsigned short file_pttblnum; /* point table index or 0xFFFF */
    /* Verbatim 16-byte name field from disk. Real game files use bytes after
     * the null terminator as scratch (e.g. "JCSTANCE1\0vda  \0") — those
     * trailing bytes ride along through the WIMP toolchain. LOAD2 hashes the
     * raw 16 bytes for sprite-allocation purposes, so zero-padding here
     * (which the original DOS imgtool DID — it just strcpy'd to a stale
     * buffer that often held leftover bytes) shifts the IRW layout. */
    unsigned char  file_name_raw[16];
    /* Pristine pixel snapshot taken at load time. Same shape as data_p
     * (stride * h bytes, where stride = (w+3)&~3). Used by the diff-mode
     * bulk-restore so we can propagate ONLY the user's edits to children
     * instead of overwriting hand-tuned per-piece pixels. NULL until load
     * fills it; freed by FreeImg. */
    void          *baseline_p;
    /* Source filename for UI hierarchy grouping */
    char           src_filename[16];
};

struct PAL {
    void          *nxt_p;
    char           n_s[10];
    unsigned char  flags;
    unsigned char  bitspix;   /* bits per pixel — 8 for 256-color */
    unsigned short numc;      /* number of colors */
    unsigned short pad;
    void          *data_p;    /* pointer to packed 15-bit RGB words (2 bytes each, little-endian: XRRRRRGG GGGBBBBB) */
    void          *temp;      /* temp for offset when saving */
    /* Debug-only: disk file fields (populated during load, used during save) */
    unsigned char  file_name_raw[10]; /* verbatim 10-byte name from disk */
    unsigned short file_data;  /* PALETTE_disk.data (+18) */
    unsigned short file_lib;   /* PALETTE_disk.lib (+20) */
    unsigned char  file_colind;/* PALETTE_disk.colind (+22) */
    unsigned char  file_cmap;  /* PALETTE_disk.cmap (+23) */
    unsigned short file_spare; /* PALETTE_disk.spare (+24) */
};

/* Pack/unpack helpers for the 15-bit RGB palette word stored in PAL.DATA_p.
   Format: little-endian word = XRRRRRGG GGGBBBBB, 5 bits per channel. */
static inline void pal_word_to_rgb8(const unsigned char *src, unsigned char *r, unsigned char *g, unsigned char *b)
{
    unsigned short w = (unsigned short)(src[0] | (src[1] << 8));
    unsigned char r5 = (unsigned char)((w >> 10) & 0x1F);
    unsigned char g5 = (unsigned char)((w >>  5) & 0x1F);
    unsigned char b5 = (unsigned char)( w        & 0x1F);
    *r = (unsigned char)((r5 << 3) | (r5 >> 2));
    *g = (unsigned char)((g5 << 3) | (g5 >> 2));
    *b = (unsigned char)((b5 << 3) | (b5 >> 2));
}
static inline void rgb8_to_pal_word(unsigned char r, unsigned char g, unsigned char b, unsigned char *dst)
{
    unsigned short r5 = (unsigned short)(r >> 3);
    unsigned short g5 = (unsigned short)(g >> 3);
    unsigned short b5 = (unsigned short)(b >> 3);
    unsigned short w  = (unsigned short)((r5 << 10) | (g5 << 5) | b5);
    dst[0] = (unsigned char)(w & 0xFF);
    dst[1] = (unsigned char)((w >> 8) & 0xFF);
}
#pragma pack(pop)

/* Disk file-format structures matching wmpstruc.inc */
#pragma pack(push, 2)
struct LIB_HDR {
    unsigned short imgcnt;    /* +0  number of images */
    unsigned short palcnt;    /* +2  number of palettes */
    unsigned int   oset;      /* +4  offset to IMAGE/PALETTE records */
    unsigned short version;   /* +8  WIMP version (0x500+) */
    unsigned short seqcnt;    /* +10 sequence count */
    unsigned short scrcnt;    /* +12 script count */
    unsigned short damcnt;    /* +14 damage table count */
    unsigned short temp;      /* +16 0xABCD if valid */
    unsigned char  bufscr[4]; /* +18 buffer script indices */
    unsigned short spare1;    /* +22 */
    unsigned short spare2;    /* +24 */
    unsigned short spare3;    /* +26 */
};

struct IMAGE_disk {
    char           n_s[16];   /* +0  image name */
    unsigned short flags;     /* +16 */
    unsigned short anix;      /* +18 */
    unsigned short aniy;      /* +20 */
    unsigned short w;         /* +22 */
    unsigned short h;         /* +24 */
    unsigned short palnum;    /* +26 */
    unsigned int   oset;      /* +28 offset to pixel data */
    unsigned int   data;      /* +32 (unused in file, placeholder) */
    unsigned short lib;       /* +36 library index */
    unsigned short anix2;     /* +38 */
    unsigned short aniy2;     /* +40 */
    unsigned short aniz2;     /* +42 */
    unsigned short frm;       /* +44 frame number */
    unsigned short opals;     /* +46 */
    unsigned short pttblnum;  /* +48 point table index or 0xFFFF */
};

struct PALETTE_disk {
    char           n_s[10];   /* +0  palette name */
    unsigned char  flags;     /* +10 */
    unsigned char  bitspix;   /* +11 */
    unsigned short numc;      /* +12 number of colors */
    unsigned int   oset;      /* +14 offset to color data */
    unsigned short data;      /* +18 (unused in file) */
    unsigned short lib;       /* +20 */
    unsigned char  colind;    /* +22 */
    unsigned char  cmap;      /* +23 */
    unsigned short spare;     /* +24 */
};
#pragma pack(pop)

#define NUMDEFPAL 3

/* Document state — fields live on Document (see document.h) and are
   reached through g_doc->X. The names below are the new accessor pattern;
   the rest of the codebase already uses them. */
#include "document.h"

extern "C" {
extern char exe_dir[];
}

/* Allocator wrappers — switch from ASM pool to C heap. */
static inline IMG *AllocImg(void)
{
    IMG *img = (IMG *)calloc(1, sizeof(IMG));
    if (!img) return NULL;
    IMG **pp = (IMG **)&g_doc->img_p;
    while (*pp) pp = (IMG **)&(*pp)->nxt_p;
    *pp = img;
    g_doc->imgcnt++;
    return img;
}

static inline PAL *AllocPal(void)
{
    PAL *pal = (PAL *)calloc(1, sizeof(PAL));
    if (!pal) return NULL;
    PAL **pp = (PAL **)&g_doc->pal_p;
    while (*pp) pp = (PAL **)&(*pp)->nxt_p;
    *pp = pal;
    g_doc->palcnt++;
    return pal;
}

static inline void FreeImg(IMG *img)
{
    if (!img) return;
    free(img->data_p);
    free(img->pttbl_p);
    free(img->baseline_p);
    free(img);
}

static inline void FreePal(PAL *pal)
{
    if (!pal) return;
    free(pal->data_p);
    free(pal);
}

static inline void *PoolAlloc(size_t n) { return calloc(1, n); }
#define PoolFree  free
#define PoolDup(p, sz) memcpy(malloc(sz), (p), (sz))

static inline IMG *get_img(int idx)
{
    if (idx < 0) return NULL;
    IMG *img = (IMG *)g_doc->img_p;
    for (int i = 0; i < idx && img; i++) img = (IMG *)img->nxt_p;
    return img;
}

static inline PAL *get_pal(int idx)
{
    if (idx < 0) return NULL;
    PAL *pal = (PAL *)g_doc->pal_p;
    for (int i = 0; i < idx && pal; i++) pal = (PAL *)pal->nxt_p;
    return pal;
}

static inline int count_imgs(void)
{
    int n = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) n++;
    return n;
}

static inline int count_pals(void)
{
    int n = 0;
    for (PAL *pal = (PAL *)g_doc->pal_p; pal; pal = (PAL *)pal->nxt_p) n++;
    return n;
}

/* Allocate a 40-byte PTTBL and attach it to the given image index. */
static inline void AddPointTable(int img_idx)
{
    IMG *img = (img_idx >= 0) ? get_img(img_idx) : NULL;
    if (!img || img->pttbl_p) return;
    img->pttbl_p = calloc(1, 40);
}

#endif /* IMG_FORMAT_H */
