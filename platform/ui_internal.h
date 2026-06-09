/*************************************************************
 * platform/ui_internal.h
 * Shared internal declarations for the ImGui overlay.
 *
 * Phase B of the overlay split (refactoring_plan.md): the overlay's file-scope
 * state is being moved out of the imgui_overlay.cpp monolith into ui_state.cpp
 * and declared here as `extern`, so UI code split into sibling translation
 * units (Phase C: ui_canvas, ui_palette, ...) can share it.
 *
 * This header is internal to the overlay implementation; it is NOT part of the
 * public imgui_overlay.h C API. Migration is incremental — globals are added
 * here one cohesive group at a time, building green after each.
 *************************************************************/
#pragma once
#include <SDL.h>
#include "document.h"   /* g_doc, Document::dirty */

/* ---- Dirty marking ----
   Single entry point for flagging the active document unsaved. Shared so any
   module (not just the overlay) can mark edits; the palette-usage cache it
   invalidates still lives in the overlay. Use this instead of touching
   g_doc->dirty directly so future side-effects live in one place. */
void InvalidatePaletteUsage(void);
inline void mark_dirty(void) { g_doc->dirty = true; InvalidatePaletteUsage(); }

/* ---- SDL state ---- */
extern SDL_Window   *g_imgui_window;
extern SDL_Renderer *g_imgui_renderer;
extern SDL_Texture  *g_canvas_texture;   /* VGA plane tex — init compat, not displayed */
extern bool          g_icon_font_loaded; /* glyph icons vs short text-label fallback */

/* Per-image render texture — rebuilt when selected image or palette changes.
   (g_img_tex_idx is defined in globals.c, not here.) */
extern SDL_Texture  *g_img_texture;
extern int           g_img_tex_w;
extern int           g_img_tex_h;

/* ---- Toolbar icon glyphs ----
   Material Symbols Sharp codepoints (UTF-8). Codepoints are stable across the
   Material Symbols family — see https://fonts.google.com/icons. When
   g_icon_font_loaded is false the UI falls back to the *_TXT strings. */
#define ICON_OPEN     "\xEE\x8B\x88"     /* U+E2C8 folder_open */
#define ICON_FOLDER   "\xEE\x8B\x87"     /* U+E2C7 folder */
#define ICON_IMAGE    "\xEE\x8F\xB4"     /* U+E3F4 image */
#define ICON_VIS      "\xEE\xA3\xB4"     /* U+E8F4 visibility */
#define ICON_SAVE     "\xEE\x85\xA1"     /* U+E161 save */
#define ICON_MARK     "\xEE\xA0\xB4"     /* U+E834 check_box — reads as 'this sprite is checked/marked' */
#define ICON_MARK_ALL "\xEE\x85\xA2"     /* U+E162 select_all */
#define ICON_CLEAR    "\xEE\xA0\xB5"     /* U+E835 check_box_outline_blank — paired visually with ICON_MARK */
#define ICON_POINTS   "\xEE\x86\xB3"     /* U+E1B3 gps_fixed — concentric registration target */
#define ICON_HITBOX   "\xEE\x87\xA6"     /* U+E1E6 activity_zone */
#define ICON_MARQUEE  "\xEE\xBD\x92"     /* U+EF52 highlight_alt — dashed-rect marquee */
#define ICON_UNDO     "\xEE\x85\xA6"     /* U+E166 undo */
#define ICON_REDO     "\xEE\x85\x9A"     /* U+E15A redo */
#define ICON_RESIZE   "\xEE\xA1\x9B"     /* U+E85B aspect_ratio */
#define ICON_ZOOM_IN  "\xEE\xA3\xBF"     /* U+E8FF zoom_in */
#define ICON_ZOOM_OUT "\xEE\xA4\x80"     /* U+E900 zoom_out */
#define ICON_LOCK     "\xEE\xA2\x97"     /* U+E897 lock */
#define ICON_UNLOCK   "\xEE\xA2\x98"     /* U+E898 lock_open */
#define ICON_SUBFRAME "\xEE\x97\x9A"     /* U+E5DA subdirectory_arrow_right */

#define ICON_OPEN_TXT     "Op"
#define ICON_FOLDER_TXT   "D "
#define ICON_IMAGE_TXT    "I "
#define ICON_VIS_TXT      "V "
#define ICON_SAVE_TXT     "Sv"
#define ICON_MARK_TXT     "Mk"
#define ICON_MARK_ALL_TXT "MA"
#define ICON_CLEAR_TXT    "CM"
#define ICON_POINTS_TXT   "Pt"
#define ICON_HITBOX_TXT   "Hb"
#define ICON_MARQUEE_TXT  "[]"
#define ICON_UNDO_TXT     "Uz"
#define ICON_REDO_TXT     "Ry"
#define ICON_RESIZE_TXT   "Sz"
#define ICON_ZOOM_IN_TXT  "Z+"
#define ICON_ZOOM_OUT_TXT "Z-"
#define ICON_LOCK_TXT     "Lk"
#define ICON_UNLOCK_TXT   "Un"
#define ICON_SUBFRAME_TXT "|-"

/* ---- Zoom / Pan ---- */
extern float g_zoom;
extern float g_pan_x;
extern float g_pan_y;
extern bool  g_zoom_fit;
extern bool  g_zoom_reset;
extern float g_zoom_wheel_accum;
extern int   g_zoom_pending_steps;
extern bool  g_zoom_pending_fit;
extern unsigned char *g_pixel_undo;
extern int            g_pixel_undo_img;  /* -2 = never built */

/* ---- Undo system (geometry / metadata snapshots) ---- */
#define UNDO_STACK_SIZE 32
struct EditSnapshot {
    unsigned int   seq;
    int            image_idx;
    unsigned short anix, aniy;
    unsigned short anix2, aniy2, aniz2;
    unsigned short w, h;
    unsigned short palnum;
    unsigned short flags;
    int            hitbox_x, hitbox_y, hitbox_w, hitbox_h;
};
extern EditSnapshot g_undo[UNDO_STACK_SIZE];
extern int          g_undo_idx;
extern int          g_undo_count;
