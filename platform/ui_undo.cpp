/*************************************************************
 * platform/ui_undo.cpp
 * Undo/Redo subsystem implementation.
 *************************************************************/
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>
#include "ui_undo.h"
#include "ui_internal.h"
#include "img_format.h"
#include "img_io.h"
#include "compat.h"
#include <vector>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#include "ui_timeline.h"
#include "ui_canvas.h"
#include "ui_palette.h"

static void DoDocUndo(void);
static void DoPixelUndo(void);
static void DoLegacyUndo(void);
static void DoDocRedo(void);
static void DoPixelRedo(void);
static void DoLegacyRedo(void);
static void undo_apply(int idx);

extern struct SDL_Color g_palette[256];


struct DocSnapshot {
    unsigned int seq;
    Document     doc;
    int          hitbox_x, hitbox_y, hitbox_w, hitbox_h;
    int          sel_color;
    bool         palette_selection[256];
};

static std::vector<DocSnapshot *> g_doc_hist;

static std::vector<DocSnapshot *> g_doc_redo;

static const size_t kDocHistMax = 16;

static size_t ImgPixelBytes(unsigned short w, unsigned short h)
{
    return (size_t)(((unsigned int)w + 3u) & ~3u) * (size_t)h;
}

static bool CloneBytes(const void *src, size_t n, void **out)
{
    if (!out) return false;
    *out = NULL;
    if (!src || n == 0) return true;
    void *dst = malloc(n);
    if (!dst) return false;
    memcpy(dst, src, n);
    *out = dst;
    return true;
}

static void FreeImgChainForSnapshot(void *head)
{
    IMG *img = (IMG *)head;
    while (img) {
        IMG *next = (IMG *)img->nxt_p;
        FreeImg(img);
        img = next;
    }
}

static void FreePalChainForSnapshot(void *head)
{
    PAL *pal = (PAL *)head;
    while (pal) {
        PAL *next = (PAL *)pal->nxt_p;
        FreePal(pal);
        pal = next;
    }
}

static bool CloneImgChainForSnapshot(const IMG *src, void **out)
{
    if (!out) return false;
    *out = NULL;
    IMG *head = NULL;
    IMG *tail = NULL;

    while (src) {
        IMG *dst = (IMG *)calloc(1, sizeof(IMG));
        if (!dst) {
            FreeImgChainForSnapshot(head);
            return false;
        }

        *dst = *src;
        dst->nxt_p = NULL;
        dst->data_p = NULL;
        dst->pttbl_p = NULL;
        dst->opaltbl_p = NULL;
        dst->baseline_p = NULL;
        dst->temp = NULL;
        dst->layer_p = NULL;

        if (!CloneBytes(src->data_p, ImgPixelBytes(src->w, src->h), &dst->data_p) ||
            !CloneBytes(src->pttbl_p, 40, &dst->pttbl_p) ||
            !CloneBytes(src->opaltbl_p, 16, &dst->opaltbl_p) ||
            !CloneBytes(src->layer_p, LayerBlockBytesFromHeader(src->layer_p),
                        &dst->layer_p)) {
            FreeImg(dst);
            FreeImgChainForSnapshot(head);
            return false;
        }

        unsigned short bw = src->baseline_w ? src->baseline_w : src->w;
        unsigned short bh = src->baseline_h ? src->baseline_h : src->h;
        if (!CloneBytes(src->baseline_p, ImgPixelBytes(bw, bh), &dst->baseline_p)) {
            FreeImg(dst);
            FreeImgChainForSnapshot(head);
            return false;
        }

        if (tail) tail->nxt_p = dst;
        else head = dst;
        tail = dst;
        src = (const IMG *)src->nxt_p;
    }

    *out = head;
    return true;
}

static bool ClonePalChainForSnapshot(const PAL *src, void **out)
{
    if (!out) return false;
    *out = NULL;
    PAL *head = NULL;
    PAL *tail = NULL;

    while (src) {
        PAL *dst = (PAL *)calloc(1, sizeof(PAL));
        if (!dst) {
            FreePalChainForSnapshot(head);
            return false;
        }

        *dst = *src;
        dst->nxt_p = NULL;
        dst->data_p = NULL;
        dst->temp = NULL;

        if (!CloneBytes(src->data_p, (size_t)src->numc * 2u, &dst->data_p)) {
            FreePal(dst);
            FreePalChainForSnapshot(head);
            return false;
        }

        if (tail) tail->nxt_p = dst;
        else head = dst;
        tail = dst;
        src = (const PAL *)src->nxt_p;
    }

    *out = head;
    return true;
}

static void FreeDocSnapshot(DocSnapshot *snap)
{
    if (!snap) return;
    FreeImgChainForSnapshot(snap->doc.img_p);
    FreeImgChainForSnapshot(snap->doc.img2_p);
    FreePalChainForSnapshot(snap->doc.pal_p);
    free(snap->doc.scrseqmem_p);
    free(snap->doc.damtbl_p);
    free(snap);
}

static DocSnapshot *CaptureDocSnapshot(unsigned int seq)
{
    if (!g_doc) return NULL;
    DocSnapshot *snap = (DocSnapshot *)calloc(1, sizeof(DocSnapshot));
    if (!snap) return NULL;

    snap->seq = seq;
    snap->doc = *g_doc;
    snap->doc.img_p = NULL;
    snap->doc.img2_p = NULL;
    snap->doc.pal_p = NULL;
    snap->doc.scrseqmem_p = NULL;
    snap->doc.damtbl_p = NULL;

    if (!CloneImgChainForSnapshot((const IMG *)g_doc->img_p, &snap->doc.img_p) ||
        !CloneImgChainForSnapshot((const IMG *)g_doc->img2_p, &snap->doc.img2_p) ||
        !ClonePalChainForSnapshot((const PAL *)g_doc->pal_p, &snap->doc.pal_p) ||
        !CloneBytes(g_doc->scrseqmem_p, g_doc->scrseqbytes, &snap->doc.scrseqmem_p) ||
        !CloneBytes(g_doc->damtbl_p, g_doc->damtblbytes, &snap->doc.damtbl_p)) {
        FreeDocSnapshot(snap);
        return NULL;
    }

    snap->hitbox_x = g_hitbox_x;
    snap->hitbox_y = g_hitbox_y;
    snap->hitbox_w = g_hitbox_w;
    snap->hitbox_h = g_hitbox_h;
    snap->sel_color = g_sel_color;
    memcpy(snap->palette_selection, g_palette_selection,
           sizeof(snap->palette_selection));
    return snap;
}

void ClearDocumentHistoryStacks(void)
{
    for (DocSnapshot *snap : g_doc_hist)
        FreeDocSnapshot(snap);
    g_doc_hist.clear();
    ClearDocumentRedoStack();
}

void ClearDocumentRedoStack(void)
{
    for (DocSnapshot *snap : g_doc_redo)
        FreeDocSnapshot(snap);
    g_doc_redo.clear();
}

static bool RestoreDocSnapshot(DocSnapshot *snap)
{
    if (!snap || !g_doc) return false;

    FreeImgChainForSnapshot(g_doc->img_p);
    FreeImgChainForSnapshot(g_doc->img2_p);
    FreePalChainForSnapshot(g_doc->pal_p);
    free(g_doc->scrseqmem_p);
    free(g_doc->damtbl_p);

    Document restored = snap->doc;
    snap->doc.img_p = NULL;
    snap->doc.img2_p = NULL;
    snap->doc.pal_p = NULL;
    snap->doc.scrseqmem_p = NULL;
    snap->doc.damtbl_p = NULL;
    *g_doc = restored;

    if (g_doc->ilselected >= (int)g_doc->imgcnt)
        g_doc->ilselected = g_doc->imgcnt ? (int)g_doc->imgcnt - 1 : -1;
    if (g_doc->plselected >= (int)g_doc->palcnt)
        g_doc->plselected = g_doc->palcnt ? (int)g_doc->palcnt - 1 : -1;

    g_hitbox_x = snap->hitbox_x;
    g_hitbox_y = snap->hitbox_y;
    g_hitbox_w = snap->hitbox_w;
    g_hitbox_h = snap->hitbox_h;
    g_sel_color = snap->sel_color;
    if (g_sel_color < 0) g_sel_color = 0;
    if (g_sel_color > 255) g_sel_color = 255;
    memcpy(g_palette_selection, snap->palette_selection,
           sizeof(g_palette_selection));

    ApplyPalette(g_doc->plselected);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    ClearTimelineThumbCache();
    g_img_tex_idx = -2;
    g_zoom_reset = true;
    g_palette_nav = false;
    InvalidatePaletteSync();
    mark_dirty();
    return true;
}

bool doc_undo_push(void)   /* non-static: also used by img_io.cpp crop ops */
{
    DocSnapshot *snap = CaptureDocSnapshot(++g_undo_seq);
    mark_dirty();
    if (!snap) return false;

    if (g_doc_hist.size() >= kDocHistMax) {
        FreeDocSnapshot(g_doc_hist.front());
        g_doc_hist.erase(g_doc_hist.begin());
    }
    g_doc_hist.push_back(snap);
    ClearDocumentRedoStack();
    for (auto &r : g_pixel_redo) pixel_hist_free(&r);
    g_pixel_redo.clear();
    if (g_undo_idx < g_undo_count - 1)
        g_undo_count = g_undo_idx + 1;
    return true;
}

void ResetPerDocumentUiState(bool clear_pixel_clipboard)
{
    g_undo_count = 0;
    g_undo_idx = -1;
    g_undo_seq = 0;
    ClearPixelHistoryStacks();
    ClearDocumentHistoryStacks();
    ClearTimelineThumbCache();
    TimelineClearFrames();
    g_timeline_built_for_imgcnt = 0;
    g_is_playing = false;
    g_play_timer = 0.0f;
    g_timeline_play_dir = 1;
    ClearTimelineCompositeSelection();

    if (clear_pixel_clipboard) ClearPixelClipboard();
    g_grid_sel.active = false;
    g_grid_sel.dragging = false;
    g_lasso_points.clear();
    g_active_tool = ActiveTool::None;
    g_pasted.active = false;
    g_pasted.dragging = false;
    g_xform.active = false;
    g_xform.handle = TransformHandle::None;
    g_content_nudge_img = -1;
    g_palette_nav = false;
    g_clone_source_set = false;
    g_clone_offset_set = false;
    g_remap_target_color = -1;
    ResetPaletteUiState();
    g_snap_bbox.valid = false;
    memset(g_palette_selection, 0, sizeof(g_palette_selection));
    g_palette_baseline_nc = 0;
    reset_palette_adjust_sliders();
    g_img_tex_idx = -2;
    g_zoom_reset = true;
    InvalidatePaletteSync();
}

void DoUndo(void)
{
    unsigned int pixel_seq = !g_pixel_hist.empty() ? g_pixel_hist.back().seq : 0;
    unsigned int doc_seq = !g_doc_hist.empty() ? g_doc_hist.back()->seq : 0;
    unsigned int legacy_seq = (g_undo_idx > 0) ? g_undo[g_undo_idx].seq : 0;

    if (doc_seq && doc_seq >= pixel_seq && doc_seq >= legacy_seq)
        DoDocUndo();
    else if (pixel_seq && pixel_seq >= legacy_seq)
        DoPixelUndo();
    else
        DoLegacyUndo();
}

void DoRedo(void)
{
    const unsigned int none = 0xFFFFFFFFu;
    unsigned int pixel_seq = !g_pixel_redo.empty() ? g_pixel_redo.back().seq : none;
    unsigned int doc_seq = !g_doc_redo.empty() ? g_doc_redo.back()->seq : none;
    unsigned int legacy_seq = (g_undo_idx < g_undo_count - 1) ? g_undo[g_undo_idx + 1].seq : none;

    if (doc_seq <= pixel_seq && doc_seq <= legacy_seq)
        DoDocRedo();
    else if (pixel_seq <= legacy_seq)
        DoPixelRedo();
    else
        DoLegacyRedo();
}



static void DoPixelUndo(void)
{
    if (!g_pixel_hist.empty()) {
        PixelHist e = g_pixel_hist.back();
        g_pixel_hist.pop_back();
        PixelHist r = {};
        bool have_redo = pixel_hist_capture_img(e.img_idx, &r, e.full_state);
        if (have_redo) r.seq = e.seq;
        if (pixel_hist_restore(&e)) {
            if (have_redo) g_pixel_redo.push_back(r);
            g_palette_nav = false;
        } else {
            if (have_redo) pixel_hist_free(&r);
            g_pixel_hist.push_back(e);
            return;
        }
        pixel_hist_free(&e);
    }
}

static void DoDocUndo(void)
{
    if (g_doc_hist.empty()) return;
    DocSnapshot *snap = g_doc_hist.back();
    g_doc_hist.pop_back();

    DocSnapshot *redo = CaptureDocSnapshot(snap->seq);
    if (RestoreDocSnapshot(snap)) {
        if (redo) g_doc_redo.push_back(redo);
    } else {
        if (redo) FreeDocSnapshot(redo);
        g_doc_hist.push_back(snap);
        return;
    }
    FreeDocSnapshot(snap);
}

static void DoLegacyUndo(void)
{
    if (g_undo_idx > 0) {
        g_undo_idx--;
        undo_apply(g_undo_idx);
    }
}

static void DoPixelRedo(void)
{
    if (!g_pixel_redo.empty()) {
        PixelHist e = g_pixel_redo.back();
        g_pixel_redo.pop_back();
        PixelHist u = {};
        bool have_undo = pixel_hist_capture_img(e.img_idx, &u, e.full_state);
        if (have_undo) u.seq = e.seq;
        if (pixel_hist_restore(&e)) {
            if (have_undo) g_pixel_hist.push_back(u);
            g_palette_nav = false;
        } else {
            if (have_undo) pixel_hist_free(&u);
            g_pixel_redo.push_back(e);
            return;
        }
        pixel_hist_free(&e);
    }
}

static void DoDocRedo(void)
{
    if (g_doc_redo.empty()) return;
    DocSnapshot *snap = g_doc_redo.back();
    g_doc_redo.pop_back();

    DocSnapshot *undo = CaptureDocSnapshot(snap->seq);
    if (RestoreDocSnapshot(snap)) {
        if (undo) g_doc_hist.push_back(undo);
    } else {
        if (undo) FreeDocSnapshot(undo);
        g_doc_redo.push_back(snap);
        return;
    }
    FreeDocSnapshot(snap);
}

static void DoLegacyRedo(void)
{
    if (g_undo_idx < g_undo_count - 1) {
        g_undo_idx++;
        undo_apply(g_undo_idx);
    }
}



void undo_push(void)
{
    mark_dirty();
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img) return;

    if (g_undo_idx >= 0) {
        EditSnapshot *last = &g_undo[g_undo_idx];
        if (last->image_idx == g_doc->ilselected &&
            last->anix == img->anix && last->aniy == img->aniy &&
            last->anix2 == img->anix2 && last->aniy2 == img->aniy2 &&
            last->aniz2 == img->aniz2 &&
            last->hitbox_x == g_hitbox_x && last->hitbox_y == g_hitbox_y &&
            last->hitbox_w == g_hitbox_w && last->hitbox_h == g_hitbox_h)
            return;
    }

    if (g_undo_idx < UNDO_STACK_SIZE - 1) {
        g_undo_idx++;
    } else {
        for (int i = 0; i < UNDO_STACK_SIZE - 1; i++)
            g_undo[i] = g_undo[i + 1];
    }

    EditSnapshot *s = &g_undo[g_undo_idx];
    s->seq = ++g_undo_seq;
    s->image_idx = g_doc->ilselected;
    s->anix  = img->anix;  s->aniy  = img->aniy;
    s->anix2 = img->anix2; s->aniy2 = img->aniy2; s->aniz2 = img->aniz2;
    s->w = img->w; s->h = img->h;
    s->palnum = img->palnum; s->flags = img->flags;
    s->hitbox_x = g_hitbox_x; s->hitbox_y = g_hitbox_y;
    s->hitbox_w = g_hitbox_w; s->hitbox_h = g_hitbox_h;
    g_undo_count = g_undo_idx + 1;
    for (auto &r : g_pixel_redo) pixel_hist_free(&r);
    g_pixel_redo.clear();
    ClearDocumentRedoStack();
}

static void undo_apply(int idx)
{
    if (idx < 0 || idx >= g_undo_count) return;
    EditSnapshot *s = &g_undo[idx];
    IMG *img = get_img(s->image_idx);
    if (!img) return;
    img->anix  = s->anix;  img->aniy  = s->aniy;
    img->anix2 = s->anix2; img->aniy2 = s->aniy2; img->aniz2 = s->aniz2;
    img->w = s->w; img->h = s->h;
    img->palnum = s->palnum; img->flags = s->flags;
    g_hitbox_x = s->hitbox_x; g_hitbox_y = s->hitbox_y;
    g_hitbox_w = s->hitbox_w; g_hitbox_h = s->hitbox_h;
}

bool CanUndo(void)
{
    return !g_pixel_hist.empty() || !g_doc_hist.empty() || g_undo_idx > 0;
}

bool CanRedo(void)
{
    return !g_pixel_redo.empty() || !g_doc_redo.empty() || g_undo_idx < g_undo_count - 1;
}
