/*************************************************************
 * platform/globals.c
 *
 * Data definitions that used to be loose globals (originally in
 * IT/itimg.asm BSS) are now fields of `Document`. This file owns
 * the single-document storage and the `g_doc` pointer that the rest
 * of the codebase reads/writes through.
 *
 * When tabs land (Phase 3), the single `g_doc_storage` here is
 * replaced with a std::deque<Document> in a .cpp file, and `g_doc`
 * is re-pointed on tab switch. The rest of the code never has to
 * change — every callsite already goes through g_doc->X.
 *************************************************************/
#include "compat.h"
#include <SDL.h>
#include <string.h>
#include "document.h"

/* Single-document storage. Replaced by a tabs container in Phase 3.
   Zero-initialized at file scope; document_init() resets sentinel fields
   (ilselected = -1, file_bufscr = 0xFF, etc.) at startup. */
static Document g_doc_storage;

Document *g_doc = &g_doc_storage;

void document_init(void)
{
    /* g_doc already points at g_doc_storage; this is a hook for
       Phase 3 when initialization becomes "push first tab". */
    memset(&g_doc_storage, 0, sizeof(g_doc_storage));
    g_doc_storage.file_bufscr[0] = 0xFF;
    g_doc_storage.file_bufscr[1] = 0xFF;
    g_doc_storage.file_bufscr[2] = 0xFF;
    g_doc_storage.file_bufscr[3] = 0xFF;
    g_doc_storage.ilselected = -1;
    g_doc_storage.il2selected = -1;
    g_doc_storage.plselected = -1;
    g_doc_storage.ilpalloaded = -1;
}

/* ---- App-wide (not per-document) ----
 * DOS DTA (Disk Transfer Area) — 43 bytes for findfile/findnext.
 * Process-wide scratch, not a document field. */
unsigned char dta[43] = {0};
