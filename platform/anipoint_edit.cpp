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

#include <imgui.h>
#include <string>

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
