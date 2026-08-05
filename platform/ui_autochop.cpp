/*************************************************************
 * platform/ui_autochop.cpp
 * Auto-Chop and Auto-Split subsystem implementation.
 *************************************************************/
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>
#include "ui_autochop.h"
#include "ui_internal.h"
#include "img_format.h"
#include "img_io.h"
#include "compat.h"
#include "palette_math.h"
#include "img_util.h"
#include <vector>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <string>

#include "anipoint.h"
#include "ui_timeline.h"

struct AutoChopTargetRef {
    IMG *img;
    int idx;
};

extern struct SDL_Color g_palette[256];

static long long EstimateZcomBitsForRect(const IMG *img,
                                         int rx, int ry, int rw, int rh,
                                         int bpp)
{
    if (!img || !img->data_p || rw <= 0 || rh <= 0 || bpp <= 0) return 0;
    long long uncompressed = (long long)rw * (long long)rh * (long long)bpp;
    if (rw <= 10) return uncompressed;
    int stride = ((int)img->w + 3) & ~3;
    const unsigned char *pixels = (const unsigned char *)img->data_p;
    std::vector<int> zlc((size_t)rh);
    std::vector<int> ztc((size_t)rh);
    long long lead_left[4] = {0, 0, 0, 0};
    long long trail_left[4] = {0, 0, 0, 0};
    for (int y = 0; y < rh; y++) {
        const unsigned char *row = pixels + (ry + y) * stride + rx;
        int lead = 0;
        while (lead < rw && row[lead] == 0) lead++;
        int trail = 0;
        if (lead < rw) { int x = rw - 1; while (x >= lead && row[x] == 0) { trail++; x--; } }
        zlc[(size_t)y] = lead;
        ztc[(size_t)y] = trail;
        for (int k = 0; k < 4; k++) {
            int factor = 1 << k;
            int q = lead / factor; if (q > 15) q = 15;
            lead_left[k] += lead - q * factor;
            q = trail / factor; if (q > 15) q = 15;
            trail_left[k] += trail - q * factor;
        }
    }
    int lm = 0, tm = 0;
    for (int k = 1; k < 4; k++) {
        if (lead_left[k] < lead_left[lm]) lm = k;
        if (trail_left[k] < trail_left[tm]) tm = k;
    }
    int lm_mult = 1 << lm;
    int tm_mult = 1 << tm;
    long long bits = (long long)rh * 8LL;
    for (int y = 0; y < rh; y++) {
        int qzl = zlc[(size_t)y] / lm_mult; if (qzl > 15) qzl = 15;
        int qzt = ztc[(size_t)y] / tm_mult; if (qzt > 15) qzt = 15;
        int visible = rw - qzl * lm_mult - qzt * tm_mult;
        if (visible < 0) visible = 0;
        bits += (long long)visible * (long long)bpp;
    }
    return bits < uncompressed ? bits : uncompressed;
}

static IMG *AutoChopPrimaryTarget(int *out_idx = NULL)
{
    int marked = CountMarkedImages();
    if (g_doc->ilselected >= 0) {
        IMG *selected = get_img(g_doc->ilselected);
        if (selected && (marked == 0 || (selected->flags & 1))) {
            if (out_idx) *out_idx = g_doc->ilselected;
            return selected;
        }
    }
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (marked == 0 || (img->flags & 1)) {
            if (out_idx) *out_idx = idx;
            return img;
        }
    }
    if (out_idx) *out_idx = -1;
    return NULL;
}

void AutoChopSetThreeBandSize(void)
{
    int idx = -1;
    IMG *img = AutoChopPrimaryTarget(&idx);
    if (!img || img->w == 0 || img->h == 0) return;
    g_chop_mode = AutoChopMode_ManualGrid;
    g_chop_w = (int)img->w;
    g_chop_h = ((int)img->h + 2) / 3;
    if (g_chop_h < 1) g_chop_h = 1;
    if (idx >= 0) g_doc->ilselected = idx;
}

void OpenAutoChopDialog(void)
{
    AutoChopSetThreeBandSize();
    g_show_auto_chop = true;
}

static void AutoChopPreviewClear(AutoChopPreview *p)
{
    if (!p) return;
    p->pieces.clear();
    p->target_count = 0;
    p->raw_cells = 0;
    p->empty_cells = 0;
    p->bpp = 0;
    p->src_uncomp_bits = 0;
    p->src_zcom_bits = 0;
    p->split_uncomp_bits = 0;
    p->split_zcom_bits = 0;
    p->best_split_valid = false;
    p->best_split_vertical = false;
    p->best_split_pos = 0;
}

/* Shared with the Sprite panel's ROM readout: both have to answer "what depth
   will LOAD2 pack this at" the same way, or the two numbers disagree. */
int Load2BppForImage(const IMG *img)
{
    PAL *pal = img ? get_pal((int)img->palnum) : NULL;
    if (g_load2_ppp > 0 && g_load2_ppp <= 8) {
        int bpp = g_load2_ppp;
        if (pal && pal->numc > (unsigned short)(1u << g_load2_ppp) &&
            pal->bitspix > 0 && pal->bitspix <= 8)
            bpp = (int)pal->bitspix;
        return bpp;
    }
    if (pal) {
        if (pal->bitspix > 0 && pal->bitspix <= 8) return (int)pal->bitspix;
        int colors = pal->numc > 0 ? (int)pal->numc : 1;
        int bpp = 1;
        while (bpp < 8 && (1 << bpp) < colors) bpp++;
        return bpp;
    }
    return 8;
}

bool BuildAutoChopPreviewForImage(const IMG *img, AutoChopPreview *out)
{
    if (!out) return false;
    AutoChopPreviewClear(out);
    if (!img || !img->data_p || img->w == 0 || img->h == 0 ||
        g_chop_w <= 0 || g_chop_h <= 0)
        return false;

    out->target_count = 1;
    out->bpp = Load2BppForImage(img);
    out->src_uncomp_bits = (long long)img->w * (long long)img->h * out->bpp;
    out->src_zcom_bits = EstimateZcomBitsForRect(img, 0, 0, img->w, img->h,
                                                 out->bpp);

    int rows = ((int)img->h + g_chop_h - 1) / g_chop_h;
    int cols = ((int)img->w + g_chop_w - 1) / g_chop_w;
    int stride = ((int)img->w + 3) & ~3;
    const unsigned char *src = (const unsigned char *)img->data_p;
    int piece_no = 0;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int cell_x = c * g_chop_w;
            int cell_y = r * g_chop_h;
            int cell_w = g_chop_w;
            int cell_h = g_chop_h;
            if (cell_x + cell_w > (int)img->w) cell_w = (int)img->w - cell_x;
            if (cell_y + cell_h > (int)img->h) cell_h = (int)img->h - cell_y;
            out->raw_cells++;

            int min_x = cell_w, min_y = cell_h;
            int max_x = -1, max_y = -1;
            int opaque = 0;
            for (int y = 0; y < cell_h; y++) {
                for (int x = 0; x < cell_w; x++) {
                    if (src[(cell_y + y) * stride + (cell_x + x)] != 0) {
                        if (x < min_x) min_x = x;
                        if (x > max_x) max_x = x;
                        if (y < min_y) min_y = y;
                        if (y > max_y) max_y = y;
                        opaque++;
                    }
                }
            }

            if (max_x < min_x) {
                out->empty_cells++;
                continue;
            }

            if (!g_chop_trim) {
                min_x = 0; min_y = 0;
                max_x = cell_w - 1; max_y = cell_h - 1;
            }

            AutoChopPiecePreview piece = {};
            piece.cell_x = cell_x;
            piece.cell_y = cell_y;
            piece.cell_w = cell_w;
            piece.cell_h = cell_h;
            piece.out_x = cell_x + min_x;
            piece.out_y = cell_y + min_y;
            piece.out_w = max_x - min_x + 1;
            piece.out_h = max_y - min_y + 1;
            piece.piece_no = piece_no++;
            piece.opaque_pixels = opaque;
            piece.uncomp_bits = (long long)piece.out_w *
                                (long long)piece.out_h *
                                (long long)out->bpp;
            piece.zcom_bits = EstimateZcomBitsForRect(img, piece.out_x,
                                                      piece.out_y,
                                                      piece.out_w,
                                                      piece.out_h,
                                                      out->bpp);
            out->split_uncomp_bits += piece.uncomp_bits;
            out->split_zcom_bits += piece.zcom_bits;
            out->pieces.push_back(piece);
        }
    }

    return true;
}

static bool BuildAutoSplitPieceForRect(const IMG *img, int rx, int ry,
                                       int rw, int rh, int piece_no, int bpp,
                                       AutoChopPiecePreview *piece)
{
    if (!img || !img->data_p || !piece || rw <= 0 || rh <= 0) return false;

    int stride = ((int)img->w + 3) & ~3;
    const unsigned char *src = (const unsigned char *)img->data_p;
    int min_x = rw, min_y = rh;
    int max_x = -1, max_y = -1;
    int opaque = 0;

    for (int y = 0; y < rh; y++) {
        for (int x = 0; x < rw; x++) {
            if (src[(ry + y) * stride + (rx + x)] != 0) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
                opaque++;
            }
        }
    }

    if (max_x < min_x) return false;

    if (!g_chop_trim) {
        min_x = 0; min_y = 0;
        max_x = rw - 1; max_y = rh - 1;
    }

    *piece = {};
    piece->cell_x = rx;
    piece->cell_y = ry;
    piece->cell_w = rw;
    piece->cell_h = rh;
    piece->out_x = rx + min_x;
    piece->out_y = ry + min_y;
    piece->out_w = max_x - min_x + 1;
    piece->out_h = max_y - min_y + 1;
    piece->piece_no = piece_no;
    piece->opaque_pixels = opaque;
    piece->uncomp_bits = (long long)piece->out_w *
                         (long long)piece->out_h *
                         (long long)bpp;
    piece->zcom_bits = EstimateZcomBitsForRect(img, piece->out_x,
                                               piece->out_y,
                                               piece->out_w,
                                               piece->out_h,
                                               bpp);
    return true;
}

static bool BuildAutoSplitPreviewForImageAt(const IMG *img, bool vertical,
                                            int split_pos,
                                            AutoChopPreview *out)
{
    if (!out) return false;
    AutoChopPreviewClear(out);
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return false;

    int w = (int)img->w;
    int h = (int)img->h;
    if (vertical) {
        if (split_pos <= k_auto_split_min_side ||
            w - split_pos <= k_auto_split_min_side)
            return false;
    } else {
        if (split_pos <= k_auto_split_min_side ||
            h - split_pos <= k_auto_split_min_side)
            return false;
    }

    out->target_count = 1;
    out->raw_cells = 2;
    out->bpp = Load2BppForImage(img);
    out->src_uncomp_bits = (long long)w * (long long)h * out->bpp;
    out->src_zcom_bits = EstimateZcomBitsForRect(img, 0, 0, w, h, out->bpp);
    out->best_split_valid = true;
    out->best_split_vertical = vertical;
    out->best_split_pos = split_pos;

    AutoChopPiecePreview piece = {};
    if (vertical) {
        if (BuildAutoSplitPieceForRect(img, 0, 0, split_pos, h, 0,
                                       out->bpp, &piece)) {
            out->split_uncomp_bits += piece.uncomp_bits;
            out->split_zcom_bits += piece.zcom_bits;
            out->pieces.push_back(piece);
        } else {
            out->empty_cells++;
        }
        if (BuildAutoSplitPieceForRect(img, split_pos, 0, w - split_pos, h, 1,
                                       out->bpp, &piece)) {
            out->split_uncomp_bits += piece.uncomp_bits;
            out->split_zcom_bits += piece.zcom_bits;
            out->pieces.push_back(piece);
        } else {
            out->empty_cells++;
        }
    } else {
        if (BuildAutoSplitPieceForRect(img, 0, 0, w, split_pos, 0,
                                       out->bpp, &piece)) {
            out->split_uncomp_bits += piece.uncomp_bits;
            out->split_zcom_bits += piece.zcom_bits;
            out->pieces.push_back(piece);
        } else {
            out->empty_cells++;
        }
        if (BuildAutoSplitPieceForRect(img, 0, split_pos, w, h - split_pos, 1,
                                       out->bpp, &piece)) {
            out->split_uncomp_bits += piece.uncomp_bits;
            out->split_zcom_bits += piece.zcom_bits;
            out->pieces.push_back(piece);
        } else {
            out->empty_cells++;
        }
    }

    return out->pieces.size() == 2;
}

bool BuildBestAutoSplitPreviewForImage(const IMG *img, bool vertical,
                                              AutoChopPreview *out)
{
    if (!out) return false;
    AutoChopPreviewClear(out);
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return false;

    int dim = vertical ? (int)img->w : (int)img->h;
    int first = k_auto_split_min_side + 1;
    int last = dim - (k_auto_split_min_side + 1);
    if (first > last) return false;

    bool have_best = false;
    AutoChopPreview best;
    for (int split = first; split <= last; split++) {
        AutoChopPreview trial;
        if (!BuildAutoSplitPreviewForImageAt(img, vertical, split, &trial))
            continue;
        if (!have_best ||
            trial.split_zcom_bits < best.split_zcom_bits ||
            (trial.split_zcom_bits == best.split_zcom_bits &&
             std::abs(split - dim / 2) < std::abs(best.best_split_pos - dim / 2))) {
            best = trial;
            have_best = true;
        }
    }

    if (!have_best) return false;
    *out = best;
    return true;
}

bool SelectedImageWillAutoChop(void)
{
    if (g_doc->ilselected < 0) return false;
    int marked = CountMarkedImages();
    if (marked == 0) return true;
    IMG *img = get_img(g_doc->ilselected);
    return img && (img->flags & 1);
}

void BuildAutoChopTargetSummary(AutoChopPreview *out)
{
    if (!out) return;
    AutoChopPreviewClear(out);
    int marked = CountMarkedImages();
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        bool target = marked > 0 ? ((img->flags & 1) != 0)
                                 : (idx == g_doc->ilselected);
        if (!target) continue;

        AutoChopPreview one;
        if (!BuildAutoChopPreviewForImage(img, &one)) continue;
        out->target_count += one.target_count;
        out->raw_cells += one.raw_cells;
        out->empty_cells += one.empty_cells;
        out->src_uncomp_bits += one.src_uncomp_bits;
        out->src_zcom_bits += one.src_zcom_bits;
        out->split_uncomp_bits += one.split_uncomp_bits;
        out->split_zcom_bits += one.split_zcom_bits;
        if (out->bpp == 0) out->bpp = one.bpp;
        else if (out->bpp != one.bpp) out->bpp = -1;
        out->pieces.insert(out->pieces.end(), one.pieces.begin(), one.pieces.end());
    }
}

static void CollectAutoChopTargets(std::vector<AutoChopTargetRef> &targets)
{
    targets.clear();
    int marked = CountMarkedImages();
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        bool target = marked > 0 ? ((img->flags & 1) != 0)
                                 : (idx == g_doc->ilselected);
        if (target) targets.push_back({img, idx});
    }
}

static void AutoSplitTargetSummaryClear(AutoSplitTargetSummary *s)
{
    if (!s) return;
    AutoChopPreviewClear(&s->selected_preview);
    s->target_count = 0;
    s->split_count = 0;
    s->skipped_count = 0;
    s->bpp = 0;
    s->src_zcom_bits = 0;
    s->split_zcom_bits = 0;
}

void BuildAutoSplitTargetSummary(bool vertical,
                                        AutoSplitTargetSummary *summary)
{
    if (!summary) return;
    AutoSplitTargetSummaryClear(summary);

    std::vector<AutoChopTargetRef> targets;
    CollectAutoChopTargets(targets);
    summary->target_count = (int)targets.size();

    for (const AutoChopTargetRef &target : targets) {
        AutoChopPreview preview;
        if (!BuildBestAutoSplitPreviewForImage(target.img, vertical, &preview)) {
            summary->skipped_count++;
            continue;
        }

        if (target.idx == g_doc->ilselected)
            summary->selected_preview = preview;
        summary->split_count++;
        summary->src_zcom_bits += preview.src_zcom_bits;
        summary->split_zcom_bits += preview.split_zcom_bits;
        if (summary->bpp == 0) summary->bpp = preview.bpp;
        else if (summary->bpp != preview.bpp) summary->bpp = -1;
    }
}

static void UnlinkAllocatedImage(IMG *img)
{
    if (!img) return;
    IMG *prev = NULL;
    IMG *cur = (IMG *)g_doc->img_p;
    while (cur && cur != img) {
        prev = cur;
        cur = (IMG *)cur->nxt_p;
    }
    if (cur == img) {
        if (prev) prev->nxt_p = cur->nxt_p;
        else g_doc->img_p = cur->nxt_p;
        if (g_doc->imgcnt > 0) g_doc->imgcnt--;
    }
    FreeImg(img);
}

static bool AutoChopNameEndsWithDigit(const char *name)
{
    size_t n = 0;
    while (n < 15 && name && name[n] != '\0') n++;
    while (n > 0 && name[n - 1] == ' ') n--;
    return n > 0 && std::isdigit((unsigned char)name[n - 1]);
}

static size_t AutoChopNameLen15(const char *name)
{
    size_t n = 0;
    while (n < 15 && name && name[n] != '\0') n++;
    return n;
}

static std::string AutoChopShortenParentNameForSuffix(const char *name,
                                                      size_t suffix_len)
{
    if (suffix_len >= 15) suffix_len = 14;
    std::string base(name ? name : "", AutoChopNameLen15(name));
    if (base.empty()) base = "SPRITE";

    while (base.size() + suffix_len > 15 && !base.empty()) {
        size_t digit_start = base.size();
        while (digit_start > 0 &&
               base[digit_start - 1] >= '0' &&
               base[digit_start - 1] <= '9') {
            digit_start--;
        }

        size_t remove_pos = std::string::npos;
        if (digit_start < base.size() && digit_start > 0) {
            size_t before_digits = digit_start - 1;
            if ((base[before_digits] >= 'A' && base[before_digits] <= 'Z') ||
                (base[before_digits] >= 'a' && base[before_digits] <= 'z')) {
                remove_pos = before_digits;
            }
        }

        if (remove_pos == std::string::npos)
            remove_pos = base.size() - 1;
        base.erase(remove_pos, 1);
    }

    if (base.empty()) base = "SPRITE";
    if (base.size() > 15 - suffix_len)
        base.resize(15 - suffix_len);
    return base;
}

static void AutoChopSubframeSuffix(const char *parent_name, int piece_no,
                                   char *buf, size_t buf_sz)
{
    if (!buf || buf_sz == 0) return;
    if (piece_no < 0) piece_no = 0;

    if (AutoChopNameEndsWithDigit(parent_name)) {
        if (piece_no < 26)
            snprintf(buf, buf_sz, "%c", (char)('A' + piece_no));
        else
            snprintf(buf, buf_sz, "_%02d", piece_no + 1);
    } else {
        snprintf(buf, buf_sz, "%d", piece_no + 1);
    }
}

static void AutoChopSubframeName(const char *parent_name, int piece_no,
                                 char out[16])
{
    if (!out) return;
    char suffix[8];
    AutoChopSubframeSuffix(parent_name, piece_no, suffix, sizeof(suffix));
    std::string base =
        AutoChopShortenParentNameForSuffix(parent_name, strlen(suffix));
    base += suffix;

    char desired[16];
    snprintf(desired, sizeof(desired), "%.15s", base.c_str());
    MakeDerivedImageName(desired, "", out);
}

static bool CreateAutoSplitPiece(IMG *master,
                                 const AutoChopPiecePreview &piece,
                                 const char *child_name)
{
    if (!master || !master->data_p || piece.out_w <= 0 || piece.out_h <= 0)
        return false;

    IMG *child = AllocImg();
    if (!child) return false;

    child->w = (unsigned short)piece.out_w;
    child->h = (unsigned short)piece.out_h;
    child->palnum = master->palnum;
    child->flags = 0;
    child->opals = master->opals;
    if (master->opaltbl_p) {
        child->opaltbl_p = malloc(16);
        if (!child->opaltbl_p) {
            UnlinkAllocatedImage(child);
            return false;
        }
        memcpy(child->opaltbl_p, master->opaltbl_p, 16);
    }
    child->anix = signed_to_img_word((int)(short)master->anix - piece.out_x);
    child->aniy = signed_to_img_word((int)(short)master->aniy - piece.out_y);
    clear_secondary_anipoint(child);

    int src_stride = ((int)master->w + 3) & ~3;
    int dst_stride = (piece.out_w + 3) & ~3;
    child->data_p = PoolAlloc((size_t)dst_stride * (size_t)piece.out_h);
    if (!child->data_p) {
        UnlinkAllocatedImage(child);
        return false;
    }

    const unsigned char *src = (const unsigned char *)master->data_p;
    unsigned char *dst = (unsigned char *)child->data_p;
    for (int y = 0; y < piece.out_h; y++) {
        memcpy(dst + y * dst_stride,
               src + (piece.out_y + y) * src_stride + piece.out_x,
               piece.out_w);
    }

    strncpy(child->src_filename, master->src_filename,
            sizeof(child->src_filename) - 1);
    child->src_filename[sizeof(child->src_filename) - 1] = '\0';
    strncpy(child->n_s, child_name && child_name[0] ? child_name : "SUBFRAME",
            sizeof(child->n_s) - 1);
    child->n_s[sizeof(child->n_s) - 1] = '\0';
    return true;
}

int ApplyBestAutoSplitToTargets(bool vertical)
{
    std::vector<AutoChopTargetRef> targets;
    CollectAutoChopTargets(targets);
    if (targets.empty()) return 0;

    struct PendingSplit {
        IMG *img;
        int idx;
        AutoChopPreview preview;
    };
    std::vector<PendingSplit> pending;
    for (const AutoChopTargetRef &target : targets) {
        AutoChopPreview preview;
        if (BuildBestAutoSplitPreviewForImage(target.img, vertical, &preview))
            pending.push_back({target.img, target.idx, preview});
    }
    if (pending.empty()) return 0;

    if (!doc_undo_push()) return 0;

    int created = 0;
    int first_created_idx = -1;
    for (const PendingSplit &plan : pending) {
        int local_created = 0;
        for (int i = 0; i < (int)plan.preview.pieces.size(); i++) {
            char child_name[16];
            AutoChopSubframeName(plan.img->n_s, i, child_name);
            if (CreateAutoSplitPiece(plan.img, plan.preview.pieces[(size_t)i], child_name)) {
                local_created++;
                created++;
                if (first_created_idx < 0)
                    first_created_idx = (int)g_doc->imgcnt - 1;
            }
        }
        if (local_created > 0) {
            plan.img->flags &= ~1;
            InvalidateThumb(plan.idx);
        }
    }

    if (created > 0) {
        if (first_created_idx >= 0) g_doc->ilselected = first_created_idx;
        g_img_tex_idx = -2;
        g_zoom_reset = true;
        mark_dirty();
    }
    return created;
}

static void AutoChopPieceLabel(const AutoChopPiecePreview &piece,
                               char *buf, size_t buf_sz)
{
    if (!buf || buf_sz == 0) return;
    if (piece.piece_no < 26)
        snprintf(buf, buf_sz, "%c", 'A' + piece.piece_no);
    else
        snprintf(buf, buf_sz, "%02d", piece.piece_no + 1);
}

void DrawAutoChopPreviewRects(ImDrawList *dl,
                                     const AutoChopPreview &preview,
                                     ImVec2 img_pos, float sx, float sy,
                                     bool foreground)
{
    if (!dl || preview.pieces.empty()) return;

    for (const AutoChopPiecePreview &piece : preview.pieces) {
        ImVec2 cell_min(img_pos.x + piece.cell_x * sx,
                        img_pos.y + piece.cell_y * sy);
        ImVec2 cell_max(img_pos.x + (piece.cell_x + piece.cell_w) * sx,
                        img_pos.y + (piece.cell_y + piece.cell_h) * sy);
        ImVec2 out_min(img_pos.x + piece.out_x * sx,
                       img_pos.y + piece.out_y * sy);
        ImVec2 out_max(img_pos.x + (piece.out_x + piece.out_w) * sx,
                       img_pos.y + (piece.out_y + piece.out_h) * sy);

        if (!foreground) {
            ImU32 fill = g_chop_trim ? IM_COL32(30, 190, 115, 58)
                                     : IM_COL32(255, 185, 45, 44);
            dl->AddRectFilled(out_min, out_max, fill);
            continue;
        }

        if (g_chop_trim) {
            dl->AddRect(cell_min, cell_max, IM_COL32(255, 210, 80, 120),
                        0.0f, 0, 1.0f);
        }
        ImU32 halo = IM_COL32(0, 0, 0, 210);
        ImU32 col = g_chop_trim ? IM_COL32(55, 240, 150, 245)
                                : IM_COL32(255, 190, 65, 245);
        dl->AddRect(ImVec2(out_min.x - 1.0f, out_min.y - 1.0f),
                    ImVec2(out_max.x + 1.0f, out_max.y + 1.0f),
                    halo, 0.0f, 0, 3.0f);
        dl->AddRect(out_min, out_max, col, 0.0f, 0, 1.5f);

        if ((out_max.x - out_min.x) >= 14.0f &&
            (out_max.y - out_min.y) >= 12.0f) {
            char label[8];
            AutoChopPieceLabel(piece, label, sizeof(label));
            ImVec2 text_pos(out_min.x + 3.0f, out_min.y + 2.0f);
            dl->AddText(ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f),
                        halo, label);
            dl->AddText(text_pos, col, label);
        }
    }
}
