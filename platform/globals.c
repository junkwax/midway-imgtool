/*************************************************************
 * platform/globals.c
 *
 * Data definitions that used to be loose globals (originally in
 * IT/itimg.asm BSS) are now fields of `Document`. The tab/document
 * backing store lives in document.cpp; this file keeps app-wide C globals.
 *************************************************************/
#include "compat.h"
#include <SDL.h>

/* ---- App-wide (not per-document) ----
 * DOS DTA (Disk Transfer Area) — 43 bytes for findfile/findnext.
 * Process-wide scratch, not a document field. */
unsigned char dta[43] = {0};
