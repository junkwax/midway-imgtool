/*************************************************************
 * platform/anipoint_edit.cpp
 * Sequence-propagating anipoint editing declared in anipoint_edit.h.
 *************************************************************/
#include "anipoint_edit.h"
#include "anipoint.h"     /* name parsing + predicates + secondary mutators */
#include "img_util.h"     /* img_name_string, signed_to_img_word */
#include "ui_internal.h"  /* mark_dirty */
#include "ui_timeline.h"  /* InvalidateThumb */
#include "img_io.h"       /* doc_undo_push, g_img_tex_idx */
#include "document.h"     /* g_doc */
#include "subframe_align.h" /* subframe_align_from_parent */

#include <imgui.h>
#include <string>
#include <vector>

/* Undo coalescing flag: true while a drag's edits collapse into one undo step. */
static bool g_sequence_anipoint_undo_active = false;

static std::string anipoint_sequence_parent_name(const IMG *img)
{
    std::string name = trim_sprite_name(img_name_string(img));
    std::string parent = InferSubframeParentName(name.c_str());
    return parent.empty() ? name : parent;
}

static bool same_anipoint_sequence(const IMG *src, const IMG *candidate)
{
    if (!src || !candidate) return false;
    std::string src_parent = anipoint_sequence_parent_name(src);
    std::string candidate_parent = anipoint_sequence_parent_name(candidate);
    if (src_parent.empty() || candidate_parent.empty()) return false;

    std::string src_stem, candidate_stem;
    bool src_numbered = strip_trailing_sequence_digits(src_parent, &src_stem);
    bool candidate_numbered =
        strip_trailing_sequence_digits(candidate_parent, &candidate_stem);

    if (src_numbered)
        return candidate_numbered && ascii_iequals(src_stem, candidate_stem);
    return ascii_iequals(src_parent, candidate_parent);
}

bool begin_sequence_anipoint_edit(void)
{
    if (g_sequence_anipoint_undo_active) {
        mark_dirty();
        return true;
    }
    if (!doc_undo_push()) return false;
    g_sequence_anipoint_undo_active = true;
    return true;
}

void finish_sequence_anipoint_edit_if_idle(void)
{
    if (g_sequence_anipoint_undo_active &&
        !ImGui::IsAnyItemActive() &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        g_sequence_anipoint_undo_active = false;
}

static int apply_anipoint_delta_to_sequence(IMG *src, int src_idx,
                                            int dx1, int dy1,
                                            int dx2, int dy2,
                                            bool affect_primary,
                                            bool affect_secondary)
{
    if (!src || (!affect_primary && !affect_secondary)) return 0;
    int changed = 0;
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (idx == src_idx || !same_anipoint_sequence(src, img)) continue;

        bool touched = false;
        if (affect_primary && (dx1 != 0 || dy1 != 0)) {
            img->anix = signed_to_img_word((int)(short)img->anix + dx1);
            img->aniy = signed_to_img_word((int)(short)img->aniy + dy1);
            touched = true;
        }
        if (affect_secondary && (dx2 != 0 || dy2 != 0) &&
            secondary_anipoint_in_use(img)) {
            img->anix2 = signed_to_img_word((int)(short)img->anix2 + dx2);
            img->aniy2 = signed_to_img_word((int)(short)img->aniy2 + dy2);
            touched = true;
        }
        if (touched) {
            InvalidateThumb(idx);
            changed++;
        }
    }
    return changed;
}

int shift_subframe_anipoints(Document *doc, const IMG *parent, int dx, int dy)
{
    if (!doc || !parent || (dx == 0 && dy == 0)) return 0;

    std::string parent_name = trim_sprite_name(img_name_string(parent));
    if (parent_name.empty()) return 0;

    int moved = 0;
    int idx = 0;
    for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (img == parent) continue;
        std::string owner = InferSubframeParentName(img_name_string(img).c_str());
        if (owner.empty() || !ascii_iequals(owner, parent_name)) continue;

        img->anix = signed_to_img_word((int)(short)img->anix + dx);
        img->aniy = signed_to_img_word((int)(short)img->aniy + dy);
        /* Thumbnails are cached per active document only. */
        if (doc == g_doc) InvalidateThumb(idx);
        moved++;
    }
    return moved;
}

/* True when `img` is a direct subframe of `parent_name`. */
static bool is_subframe_of(const IMG *img, const std::string &parent_name)
{
    std::string owner = InferSubframeParentName(img_name_string(img).c_str());
    return !owner.empty() && ascii_iequals(owner, parent_name);
}

int count_subframes(Document *doc, const IMG *parent)
{
    if (!doc || !parent) return 0;
    std::string parent_name = trim_sprite_name(img_name_string(parent));
    if (parent_name.empty()) return 0;
    int n = 0;
    for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p)
        if (img != parent && is_subframe_of(img, parent_name)) n++;
    return n;
}

IMG *find_subframe_parent(Document *doc, const IMG *child)
{
    if (!doc || !child) return NULL;
    std::string owner = InferSubframeParentName(img_name_string(child).c_str());
    if (owner.empty()) return NULL;
    for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p) {
        if (img == child) continue;
        if (ascii_iequals(trim_sprite_name(img_name_string(img)), owner))
            return img;
    }
    return NULL;
}

void collect_subframe_indices(Document *doc, const IMG *parent,
                              std::vector<int> *out)
{
    if (!out) return;
    out->clear();
    if (!doc || !parent) return;
    std::string parent_name = trim_sprite_name(img_name_string(parent));
    if (parent_name.empty()) return;

    int idx = 0;
    for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (img == parent || !is_subframe_of(img, parent_name)) continue;
        if (!img->data_p || img->w == 0 || img->h == 0) continue;
        out->push_back(idx);
    }
}

/* IMG bitmaps carry a 4-byte-aligned stride, same as everywhere else. */
static StampBuf img_stamp_view(const IMG *img)
{
    StampBuf b;
    b.pixels = (const unsigned char *)img->data_p;
    b.w = (int)img->w;
    b.h = (int)img->h;
    b.stride = ((int)img->w + 3) & ~3;
    return b;
}

SubframeRecalcReport recalc_subframe_anipoints_from_parent(Document *doc,
                                                           IMG *parent)
{
    SubframeRecalcReport rep;
    if (!doc || !parent || !parent->data_p || parent->w == 0 || parent->h == 0)
        return rep;

    std::string parent_name = trim_sprite_name(img_name_string(parent));
    if (parent_name.empty()) return rep;

    StampBuf pbuf = img_stamp_view(parent);
    int parent_ax = (int)(short)parent->anix;
    int parent_ay = (int)(short)parent->aniy;

    /* Collect the work first so undo is pushed only when there is some. */
    struct Fix { IMG *img; int idx; int ax; int ay; bool exact; };
    std::vector<Fix> fixes;
    int idx = 0;
    for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (img == parent || !is_subframe_of(img, parent_name)) continue;
        rep.considered++;
        if (!img->data_p || img->w == 0 || img->h == 0) { rep.unplaced++; continue; }

        SubframeAlign a =
            subframe_align_from_parent(pbuf, parent_ax, parent_ay,
                                       img_stamp_view(img),
                                       (int)(short)img->anix,
                                       (int)(short)img->aniy);
        if (!a.valid) { rep.unplaced++; continue; }
        if (a.exact) rep.exact++; else rep.approximate++;

        if (a.new_anix == (int)(short)img->anix &&
            a.new_aniy == (int)(short)img->aniy)
            continue;
        Fix f; f.img = img; f.idx = idx; f.ax = a.new_anix; f.ay = a.new_aniy;
        f.exact = a.exact;
        fixes.push_back(f);
    }

    if (fixes.empty()) return rep;
    if (doc == g_doc && !doc_undo_push()) return rep;

    for (size_t i = 0; i < fixes.size(); i++) {
        fixes[i].img->anix = signed_to_img_word(fixes[i].ax);
        fixes[i].img->aniy = signed_to_img_word(fixes[i].ay);
        if (doc == g_doc) InvalidateThumb(fixes[i].idx);
        rep.changed++;
    }
    doc->dirty = true;
    if (doc == g_doc) g_img_tex_idx = -2;
    return rep;
}

static void mark_selected_anipoint_changed(void)
{
    InvalidateThumb(g_doc ? g_doc->ilselected : -1);
    g_img_tex_idx = -2;
    mark_dirty();
}

bool set_primary_anipoint_with_sequence(IMG *img, int new_ax, int new_ay)
{
    if (!img) return false;
    int old_ax = (int)(short)img->anix;
    int old_ay = (int)(short)img->aniy;
    int dx = new_ax - old_ax;
    int dy = new_ay - old_ay;
    if (dx == 0 && dy == 0) return false;
    if (!begin_sequence_anipoint_edit()) return false;

    img->anix = signed_to_img_word(new_ax);
    img->aniy = signed_to_img_word(new_ay);
    apply_anipoint_delta_to_sequence(img, g_doc->ilselected,
                                     dx, dy, 0, 0, true, false);
    InvalidateThumb(g_doc->ilselected);
    g_img_tex_idx = -2;
    mark_dirty();
    return true;
}

bool set_primary_anipoint_local(IMG *img, int new_ax, int new_ay)
{
    if (!img) return false;
    unsigned short packed_ax = signed_to_img_word(new_ax);
    unsigned short packed_ay = signed_to_img_word(new_ay);
    if (img->anix == packed_ax && img->aniy == packed_ay) return false;
    if (!begin_sequence_anipoint_edit()) return false;

    img->anix = packed_ax;
    img->aniy = packed_ay;
    mark_selected_anipoint_changed();
    return true;
}

bool set_secondary_anipoint_with_sequence(IMG *img, int new_ax2, int new_ay2)
{
    if (!img) return false;
    bool was_active = secondary_anipoint_in_use(img);
    int old_ax2 = was_active ? (int)(short)img->anix2 : 0;
    int old_ay2 = was_active ? (int)(short)img->aniy2 : 0;
    int dx = new_ax2 - old_ax2;
    int dy = new_ay2 - old_ay2;
    if (was_active && dx == 0 && dy == 0) return false;
    if (!begin_sequence_anipoint_edit()) return false;
    if (!was_active)
        activate_secondary_anipoint(img);

    img->anix2 = signed_to_img_word(new_ax2);
    img->aniy2 = signed_to_img_word(new_ay2);
    if (dx != 0 || dy != 0) {
        apply_anipoint_delta_to_sequence(img, g_doc->ilselected,
                                         0, 0, dx, dy, false, true);
    }
    InvalidateThumb(g_doc->ilselected);
    g_img_tex_idx = -2;
    mark_dirty();
    return true;
}

bool set_secondary_anipoint_local(IMG *img, int new_ax2, int new_ay2)
{
    if (!img) return false;
    bool was_active = secondary_anipoint_in_use(img);
    unsigned short packed_ax2 = signed_to_img_word(new_ax2);
    unsigned short packed_ay2 = signed_to_img_word(new_ay2);
    if (was_active && img->anix2 == packed_ax2 && img->aniy2 == packed_ay2)
        return false;
    if (!begin_sequence_anipoint_edit()) return false;
    if (!was_active)
        activate_secondary_anipoint(img);

    img->anix2 = packed_ax2;
    img->aniy2 = packed_ay2;
    mark_selected_anipoint_changed();
    return true;
}

bool clear_secondary_anipoint_local(IMG *img)
{
    if (!img) return false;
    if (img->anix2 == (unsigned short)-1 &&
        img->aniy2 == (unsigned short)-1 &&
        img->aniz2 == (unsigned short)-1)
        return false;
    if (!begin_sequence_anipoint_edit()) return false;

    clear_secondary_anipoint(img);
    mark_selected_anipoint_changed();
    return true;
}

bool set_secondary_anipoint_z_local(IMG *img, int new_az2)
{
    if (!img) return false;
    if (new_az2 == -1)
        return clear_secondary_anipoint_local(img);

    bool was_active = secondary_anipoint_in_use(img);
    unsigned short packed_az2 = signed_to_img_word(new_az2);
    if (was_active && img->aniz2 == packed_az2) return false;
    if (!begin_sequence_anipoint_edit()) return false;
    if (!was_active)
        activate_secondary_anipoint(img);

    img->aniz2 = packed_az2;
    mark_selected_anipoint_changed();
    return true;
}
