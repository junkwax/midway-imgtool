/*****************************************************************************
 * platform/document.h
 *
 * Document — one open IMG and its associated editor state. Per-tab state
 * lives here; app-wide state (file dialog, modal flags, recent files,
 * preferences) stays in imgui_overlay.cpp as file-static globals.
 *
 * The active document is reached through `g_doc` (pointer). Tab switch =
 * `g_doc = &tabs[active]`. The backing container is std::deque so element
 * addresses are stable across push_back — this matters because several
 * inline helpers (AllocImg, AllocPal) take the address of fields like
 * &g_doc->img_p.
 *
 * Field provenance: most fields originated in IT/itimg.asm BSS and were
 * mirrored into globals.c during the SDL port. Names preserved verbatim so
 * the rename across ~550 callsites is a pure mechanical change.
 *****************************************************************************/
#ifndef PLATFORM_DOCUMENT_H
#define PLATFORM_DOCUMENT_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IMG and PAL forward decls — defined in img_format.h. We don't include
   img_format.h here because img_format.h pulls in Document via the
   AllocImg/get_img helpers. */
struct IMG;
struct PAL;

typedef struct Document {
    /* ---- Image list ---- */
    void         *img_p;          /* head of IMG linked list */
    unsigned int  imgcnt;
    int           ilselected;     /* selected index in image list, -1 = none */
    unsigned int  imgdataw;
    unsigned int  il1stprt;       /* first visible row in image list */
    int           ilpalloaded;

    /* ---- Second image list (dual-list switching, DOS legacy) ---- */
    void         *img2_p;
    unsigned int  img2cnt;
    int           il2selected;
    unsigned int  il21stprt;

    /* ---- Palette list ---- */
    void         *pal_p;          /* head of PAL linked list */
    unsigned int  palcnt;
    int           plselected;

    /* ---- Counts / version ---- */
    unsigned int  seqcnt;
    unsigned int  scrcnt;
    unsigned int  damcnt;
    unsigned int  fileversion;

    /* ---- Sequence/script memory ----
     * Raw byte blob of all SEQSCR records and their ENTRY arrays, captured
     * verbatim from the source file so save can write it back unchanged. */
    void         *scrseqmem_p;
    unsigned int  scrseqbytes;

    /* ---- LIB_HDR fields preserved verbatim from load ----
     * Real game-asset pipelines populate bufscr with script-buffer indices
     * that LOAD2 reads to compute IRW layout. Clobbering bufscr causes
     * LOAD2 to emit a slightly different IRW size, shifting subsequent
     * characters' sprites in the bank. Preserve verbatim. */
    unsigned char file_bufscr[4];
    unsigned short file_spare1;
    unsigned short file_spare2;
    unsigned short file_spare3;

    /* ---- File I/O paths (DOS-era 8.3 convention) ---- */
    char          fpath_s[64];
    char          fname_s[13];
    char          fnametmp_s[13];
} Document;

/* The currently-active document. Points at one element of the tabs
   container (or at a single-document fallback before tabs exist).
   Never NULL after document_init() runs. */
extern Document *g_doc;

/* Initialize the tab system with one empty document. Must be called once
   during startup before any IMG load. */
void document_init(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_DOCUMENT_H */
