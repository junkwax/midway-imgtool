/*************************************************************
 * platform/ui_canvas.h
 * Canvas/World-View rendering helpers split from imgui_overlay.cpp.
 *
 * Phase C of the overlay split. This first slice owns the single-sprite
 * World View canvas branch; the larger marked-tab World View panel remains
 * in the overlay until its own dependency closure is small enough to move.
 *************************************************************/
#pragma once

#include <SDL.h>
#include <imgui.h>
#include <string>
#include <utility>
#include <vector>
#include "document.h"    /* Document */
#include "img_format.h"  /* IMG */
#include "ui_timeline.h" /* kDefaultTimelineHold — shared with the Image timeline */

/* Slot layout. The special lanes sit AFTER the marked rows, so changing
   kWorldMarkedSourceTabs moves them -- and a .WAX stores rows by slot index.
   A project written under the old count would hand its dummy-body row to a
   marked-row slot and lose the lane entirely, so the writer records
   `slot.source_tabs` and the loaders remap the specials off it.
   kWorldMarkedLegacySourceTabs is what files written before that key existed
   used, and is the fallback when it is absent. */
enum { kWorldMarkedLegacySourceTabs = 10 };

enum {
    kWorldMarkedSourceTabs = 20,                  /* marked IMG rows */
    kWorldDummyDecapSlot = kWorldMarkedSourceTabs,/* optional dummy body */
    kWorldAsmSlot,                                /* ASM-driven player lane */
    kWorldAsmOpponentSlot,                        /* ASM-driven opponent lane */
    kWorldEmbeddedSeqScrSlot,                     /* embedded WIMP sequence/script lane */
    kWorldMarkedMaxTabs                           /* source rows + dummy + ASM + embedded lanes */
};

/* Ceiling on a "every tick" World View PNG export, so a lane with a huge
   visible-until value can't spray tens of thousands of files. */
enum { kWorldPngSequenceMaxFrames = 600 };

/* Ticks a blood frame holds when a spray is first imported. MKBLOOD.ASM runs
   its sprays at an ani speed of 5, but that is the speed the finished effect
   plays at, not the granularity you author it at: imported at 5 the row lands
   on every fifth tick and can no longer be lined up with the frame that caused
   it. Import at 1 -- one tick per blood frame, the finest the timeline has --
   and set the speed afterwards with the row's Ticks/frame. */
enum { kWorldBloodTicksPerFrame = 1 };

/* MK2 runs its game logic once per video field on the TMS34010 hardware, about
   54.7 times a second. A SEQSCR "tick" is exactly one of those, so a hold of 2
   is 2/54.7s on the real machine. Previewing at anything else makes timing
   decisions that will not hold up in game, which is why every tick-driven
   preview defaults to this rate rather than a round number. */
/* Measured from MAME's mk2 driver: refresh_attoseconds =
   18,279,250,000,000,000, i.e. 54.7068 Hz / 18.279 ms a tick.  One game
   tick is one display refresh - a lane at sleep 4 holds each row for
   exactly 4 emulator frames.  (Not 60 Hz / 16 ms; MK2's own docs had that
   wrong, and they have since been corrected.) */
constexpr float kMk2TickHz = 54.7068f;

struct WorldViewState {
    bool enabled = false;
    int w = 400;        /* arcade playfield width */
    int h = 254;        /* arcade playfield height */
    int origin_x = 200; /* anchor target inside world */
    int origin_y = 20;  /* anchor target inside world (top-anchored) */
    bool onion = false; /* faintly draw prev frame underneath */
    bool show_borders = true;
    bool show_anipoint = true;

    /* ---- Reference figure ----
       An optional standing-fighter outline drawn at the same anchor, so
       "will this effect land on the victim?" is a look instead of a
       calculation. It is a proportioned silhouette rather than real art —
       imgtool ships no fighter sprite — which is why its footprint and
       placement are adjustable: line it up once against a known-good stance
       frame and it stays put.

       ref_dx/ref_dy place the figure's feet centre relative to the anchor,
       in world pixels. ref_mirror flips it so both facings are checkable at
       once alongside the canvas mirror preview.

       ref_dy defaults to h - origin_y so the figure stands on the playfield
       floor rather than hanging off the top: the anchor is near the world's
       top edge (origin_y = 20), so feet-at-anchor would put all but the shins
       above y=0. "Feet to Floor" in the config popup restores this after the
       origin moves. */
    /* ---- Game placement ----
       The stock anchor (200, 20) sits near the top of the playfield, so a
       loaded animation hangs from the ceiling instead of standing where the
       game would draw it. With game_placement on, the anchor's Y is derived
       from the first frame so that frame's feet land on floor_y; every later
       frame then offsets from there by its own dX/dY, which is exactly how a
       sequence reads on the real screen. Turn it off to go back to the raw
       anchor for anipoint alignment work. */
    /* Mark BGBIGFIST1A/1B/1C/1D and get one reassembled frame, not four
       separate ones: pieces of a chopped drawing collapse into a single
       multi-piece lane entry, each drawn at its own anipoint. */
    bool group_subframes = true;

    bool game_placement = true;
    int floor_y = 254;  /* playfield bottom; where a standing fighter's feet sit */

    bool show_reference = false;
    bool ref_mirror = false;
    int ref_w = 62;     /* MK2 standing fighter footprint, roughly */
    int ref_h = 110;
    int ref_dx = 0;
    int ref_dy = 234;   /* = h(254) - origin_y(20) */

    /* ---- Reference background ----
       An optional MK2 stage (BDD/BDB pair) composited behind the
       playfield, so an animation can be judged against the art it
       will actually play over instead of flat black. Strictly a
       backdrop: nothing here edits the stage, and none of it
       reaches a saved IMG.

       bg_x/bg_y are the world coordinates drawn at the playfield's
       top-left corner — i.e. a camera position, moved by the popup
       or by snapping to a module. The stage is far larger than the
       400x254 playfield, so it is always a window onto the art.

       Real parallax is not reconstructable from BDD/BDB alone (the
       rates, per-plane offsets and draw order live in BGND.ASM), so
       placement is the user's to set; bg_module records which plane
       they snapped to purely so the popup can show it. */
    bool bg_enabled = false;
    int bg_x = 0;
    int bg_y = 0;
    int bg_alpha = 255;
    int bg_module = -1;
};

/* Two-sprite anipoint staging workspace.  The selected target is adjusted
   directly, so its corrected anchor persists in the IMG on save. */
struct AnipointLinkState {
    bool enabled = false;
    int reference_doc_idx = -1;
    int reference_img_idx = -1;
    int target_doc_idx = -1;
    int target_img_idx = -1;
    float zoom = 1.0f;
    int target_offset_x = 0; /* preview-only game/local placement offset */
    int target_offset_y = 0;
    int reference_alpha = 145;
    bool dragging = false;
    ImVec2 drag_start = ImVec2(0, 0);
};

struct WorldCanvasLayout {
    float scale = 1.0f;
    float width = 0.0f;
    float height = 0.0f;
    ImVec2 pos = ImVec2(0, 0);
    float origin_x = 0.0f;
    float origin_y = 0.0f;
};

/* Draw the reference figure into a World View canvas laid out by
   ComputeWorldCanvasLayout(). No-op when state.show_reference is false. */
void WorldDrawReferenceFigure(ImDrawList *dl, const WorldCanvasLayout &layout,
                              const WorldViewState &state);

/* Draw the loaded stage behind everything else in a World View canvas laid out
   by ComputeWorldCanvasLayout(), clipped to that canvas. No-op when
   state.bg_enabled is false or no stage is loaded. */
void WorldDrawReferenceBackground(ImDrawList *dl, const WorldCanvasLayout &layout,
                                  const WorldViewState &state);

/* Position the loaded stage so module `module_idx` is centred in the playfield
   with its bottom edge on state.floor_y. No-op on an out-of-range index. */
struct BddBackground;
void WorldBgSnapToModule(WorldViewState &state, const BddBackground &bg,
                         int module_idx);

struct WorldMarkedPanelLayout {
    ImVec2 pos = ImVec2(0, 0);
    float width = 0.0f;
    float height = 0.0f;
};

struct CanvasTransform2D {
    ImVec2 img_pos = ImVec2(0, 0);
    float sx = 1.0f;
    float sy = 1.0f;
    float cx_img = 0.0f;
    float cy_img = 0.0f;
    float ca = 1.0f;
    float sa = 0.0f;
};



struct CanvasTransformHandleOverlay {
    TransformHandle hover = TransformHandle::None;
    bool rotate_hover = false;
    bool chain_hover = false;
    ImVec2 center = ImVec2(0, 0);
    ImVec2 chain_min = ImVec2(0, 0);
    ImVec2 chain_max = ImVec2(0, 0);
};

struct CanvasTransformDragStart {
    TransformHandle handle = TransformHandle::None;
    ImVec2 mouse = ImVec2(0, 0);
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    float angle_deg = 0.0f;
    float ref_aspect = 1.0f;
};

struct CanvasTransformDragResult {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    float angle_deg = 0.0f;
};

struct CanvasContentBounds {
    bool valid = false;
    int min_x = 0;
    int min_y = 0;
    int max_x = 0;
    int max_y = 0;
};

struct CanvasPasteSnapResult {
    int x = 0;
    int y = 0;
    bool hit_x = false;
    bool hit_y = false;
    int guide_x = 0;
    int guide_y = 0;
};

struct CanvasPasteDragResult {
    int x = 0;
    int y = 0;
    bool hit_x = false;
    bool hit_y = false;
    int guide_x = 0;
    int guide_y = 0;
};

struct CanvasPasteHint {
    const char *text = "";
    ImU32 color = IM_COL32(255, 255, 0, 255);
};

struct CanvasPasteControlsLayout {
    ImVec2 min = ImVec2(0, 0);
    ImVec2 max = ImVec2(0, 0);
    ImVec2 blend_label_pos = ImVec2(0, 0);
    ImVec2 blend_control_pos = ImVec2(0, 0);
    ImVec2 opacity_label_pos = ImVec2(0, 0);
    ImVec2 opacity_control_pos = ImVec2(0, 0);
    ImVec2 smooth_control_pos = ImVec2(0, 0);
    float item_width = 0.0f;
    bool blocks_mouse = false;
};

struct CanvasPasteHitTest {
    ImVec2 bounds_min = ImVec2(0, 0);
    ImVec2 bounds_max = ImVec2(0, 0);
    bool hovering = false;
    bool over_sprite = false;
};

struct CanvasPastePreviewCell {
    int target_x = 0;
    int target_y = 0;
    ImVec2 quad[4] = {};
};

struct CanvasPasteGeometry {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    float angle_deg = 0.0f;
    CanvasTransform2D transform;
    ImVec2 corners[4] = {};
    CanvasPasteHitTest hit;
};

float ZoomFitScaleForAvailable(const ImVec2 &avail);
float ZoomDisplayScaleForAvailable(const ImVec2 &avail);
void ZoomClampPanForScale(const ImVec2 &avail, float scale);
void ZoomClampPanForAvailable(const ImVec2 &avail);
void ZoomPanBy(const ImVec2 &avail, float dx, float dy);
void ZoomImageRectForAvailable(const ImVec2 &avail, const ImVec2 &origin,
                               ImVec2 *pos, ImVec2 *size, float *scale_out);
float ZoomNextLevel(float current, int dir);
void ResetZoomToFit(void);
void ResetZoomToHalfFit(const ImVec2 &avail);
void ResetZoomForSelection(const ImVec2 &avail);
void QueueZoomStep(int dir);
void QueueZoomFit(void);
bool ApplyZoomScale(float old_scale, float new_scale,
                    const ImVec2 &anchor, const ImVec2 &old_pos,
                    const ImVec2 &old_size, const ImVec2 &avail);
bool CanvasPointInRect(ImVec2 p, ImVec2 mn, ImVec2 mx);
CanvasTransform2D CanvasMakeTransform(ImVec2 img_pos, float sx, float sy,
                                      int rect_x, int rect_y,
                                      int rect_w, int rect_h,
                                      float angle_deg);
ImVec2 CanvasTransformPointScreen(const CanvasTransform2D &xf,
                                  float image_x, float image_y);
ImVec2 CanvasTransformPointImage(const CanvasTransform2D &xf,
                                 float image_x, float image_y);
void CanvasTransformRectCorners(const CanvasTransform2D &xf,
                                int rect_x, int rect_y,
                                int rect_w, int rect_h,
                                ImVec2 corners[4]);
void CanvasQuadBounds(const ImVec2 corners[4], ImVec2 *out_min,
                      ImVec2 *out_max);
void DrawCanvasPasteBorder(ImDrawList *dl, const ImVec2 corners[4],
                           bool transform_active, bool hovering);
void DrawCanvasPasteSnapGuides(ImDrawList *dl, ImVec2 img_pos,
                               float sx, float sy,
                               int tex_w, int tex_h,
                               bool dragging,
                               bool hit_x, int guide_x,
                               bool hit_y, int guide_y);
void DrawCanvasPasteHint(ImDrawList *dl, ImVec2 img_pos,
                         const char *hint, ImU32 color);
CanvasPasteHint CanvasPasteHintForState(bool transform_active,
                                        TransformHandle handle,
                                        bool paste_dragging);
CanvasPasteControlsLayout CanvasPasteControlsLayoutFor(ImVec2 canvas_origin,
                                                       ImVec2 mouse);
CanvasPasteHitTest CanvasPasteHitTestFor(const ImVec2 corners[4],
                                         ImVec2 img_pos,
                                         ImVec2 img_sz,
                                         ImVec2 mouse);
CanvasPastePreviewCell CanvasPastePreviewCellForPixel(
    const CanvasTransform2D &xf,
    int paste_x, int paste_y,
    int paste_w, int paste_h,
    int clip_w, int clip_h,
    int pixel_x, int pixel_y);
CanvasPasteGeometry CanvasPasteGeometryForState(
    ImVec2 img_pos,
    float sx, float sy,
    ImVec2 img_sz,
    ImVec2 mouse,
    bool transform_active,
    int transform_x, int transform_y,
    int transform_w, int transform_h,
    float transform_angle_deg,
    int paste_x, int paste_y,
    int paste_w, int paste_h);
CanvasTransformHandleOverlay DrawCanvasTransformHandles(
    ImDrawList *dl, const ImVec2 corners[4], ImVec2 mouse,
    TransformHandle active_handle, bool aspect_locked);
void CanvasResizeTransformRect(TransformHandle handle,
                               int drag_x, int drag_y,
                               int drag_w, int drag_h,
                               float ref_aspect,
                               int dx, int dy,
                               bool lock_aspect,
                               int *out_x, int *out_y,
                               int *out_w, int *out_h);
float CanvasRotateTransformAngle(float start_angle_deg,
                                 ImVec2 drag_mouse,
                                 ImVec2 mouse,
                                 ImVec2 center,
                                 bool snap_15_deg);
CanvasTransformDragResult CanvasResolveTransformDrag(
    const CanvasTransformDragStart &start,
    ImVec2 mouse,
    float sx, float sy,
    ImVec2 center,
    bool aspect_locked,
    bool shift_down);
void CanvasDragDeltaPixels(ImVec2 drag_start_mouse, ImVec2 mouse,
                           float sx, float sy,
                           int *dx, int *dy);
CanvasContentBounds CanvasFindOpaqueBounds(const IMG *img);
CanvasPasteSnapResult CanvasSnapPasteToContent(int x, int y, int w, int h,
                                               int target_w, int target_h,
                                               const CanvasContentBounds &bounds,
                                               float view_sx, float view_sy);
CanvasPasteSnapResult CanvasPasteCenterGuide(int x, int y, int w, int h,
                                             int target_w, int target_h,
                                             bool hit_x, bool hit_y,
                                             int guide_x, int guide_y);
void CanvasClampPasteRect(int canvas_w, int canvas_h,
                          int rect_w, int rect_h,
                          int *x, int *y);
CanvasPasteDragResult CanvasResolvePasteDrag(ImVec2 drag_start_mouse,
                                             ImVec2 mouse,
                                             float sx, float sy,
                                             int start_x, int start_y,
                                             int rect_w, int rect_h,
                                             int canvas_w, int canvas_h,
                                             int target_w, int target_h,
                                             bool snap_to_content,
                                             const CanvasContentBounds &bounds,
                                             bool show_center_guides);
void CanvasRotateButtonRects(ImVec2 img_pos, ImVec2 img_sz,
                             ImVec2 canvas_pos, ImVec2 canvas_sz,
                             ImVec2 mins[2], ImVec2 maxs[2]);
void DrawCanvasRotateButtons(ImDrawList *dl, const ImVec2 mins[2],
                             const ImVec2 maxs[2], int hover_idx);
void DrawCanvasCheckerboard(ImDrawList *dl, ImVec2 img_pos, ImVec2 img_sz,
                            float scale);
void DrawCanvasPixelGrid(ImDrawList *dl, ImVec2 img_pos, ImVec2 img_sz,
                         int tex_w, int tex_h, float scale);
void DrawCanvasZoomIndicator(bool zoom_fit, float zoom);
void DrawCanvasDmaCompressionOverlay(ImDrawList *dl, IMG *img,
                                     ImVec2 img_pos, float sx, float sy);
void DrawCanvasColorIsolationOverlay(ImDrawList *dl, IMG *img,
                                     const bool kept[256],
                                     ImVec2 img_pos, float sx, float sy);
void DrawCanvasPixelHoverHighlight(ImDrawList *dl, ImVec2 mouse,
                                   ImVec2 img_pos, ImVec2 img_sz,
                                   float sx, float sy,
                                   bool suppress);
void DrawCanvasPencilCursor(ImDrawList *dl, ImVec2 img_pos,
                            float sx, float sy,
                            int pixel_x, int pixel_y,
                            int brush, ImU32 color);
void DrawCanvasCloneStampAids(ImDrawList *dl, ImVec2 img_pos,
                              float sx, float sy,
                              int source_x, int source_y,
                              bool show_dest_brush,
                              int dest_x, int dest_y,
                              int brush);
void DrawCanvasLassoPath(ImDrawList *dl, ImVec2 img_pos,
                         float sx, float sy,
                         const std::vector<std::pair<int, int>> &points);
void DrawCanvasSelectionOverlay(ImDrawList *dl, ImVec2 img_pos,
                                float sx, float sy,
                                int x1, int y1, int x2, int y2,
                                bool is_mask, int mask_w,
                                const std::vector<bool> *pixel_mask);
void DrawCanvasAnipointCrosshair(ImDrawList *dl, ImVec2 p, ImU32 col,
                                 float len, float thick);
bool CanvasAnipointHitTest(const IMG *img, ImVec2 img_pos,
                           float sx, float sy, ImVec2 mouse,
                           bool *primary_hover, bool *secondary_hover);
void DrawCanvasAnipointOverlay(ImDrawList *dl, const IMG *img,
                               const IMG *prev_img,
                               ImVec2 img_pos, float sx, float sy,
                               ImVec2 mouse,
                               bool *primary_hover,
                               bool *secondary_hover);
/* ---- Mirror (h-flip) preview ----
 * The canvas has only ever drawn the unflipped sprite, so an anipoint that
 * sits well outside its own art looked plausible here and landed hundreds of
 * pixels away in game the moment the sprite faced the other way. These draw
 * the placement the engine would produce h-flipped, mirrored about the
 * anipoint exactly the way ganiof/ani2 do.
 *
 * `flip_left_offset_px` is where the mirrored sprite's left edge sits relative
 * to the unflipped one, in sprite pixels: anix - anix_eff(flipped). */
int CanvasFlipPreviewOffsetPx(const IMG *img);

/* Draw the mirrored placement of `img_texture` as a tinted ghost, plus the
   fixed anipoint line between the two placements. `img_pos`/`img_sz` are the
   unflipped sprite's screen rect. */
void DrawCanvasFlipPreview(ImDrawList *dl, const IMG *img,
                           SDL_Texture *img_texture,
                           ImVec2 img_pos, ImVec2 img_sz,
                           float sx, float sy, bool ghost);

void DrawCanvasHitboxOverlay(ImDrawList *dl, ImVec2 img_pos,
                             float sx, float sy,
                             int x, int y, int w, int h,
                             ImVec2 mouse, bool hovering[4]);
void DrawCanvasStrikeBoxOverlay(ImDrawList *dl, ImVec2 img_pos,
                                float sx, float sy,
                                int x, int y, int w, int h,
                                const char *label,
                                ImVec2 mouse, bool enable_hover,
                                bool hovering[4]);
void CanvasResizeRectFromCorner(int corner, int mouse_x, int mouse_y,
                                int *x, int *y, int *w, int *h);

struct WorldMarkedSplitLane {
    int slot = -1;
    int doc_idx = -1;
};

struct WorldMarkedSequenceState {
    WorldMarkedSequenceState()
    {
        for (int i = 0; i < kWorldMarkedMaxTabs; i++) {
            sequence_doc_uid[i] = 0;
            lane_visible[i] = true;
            lane_order[i] = -1;
            auto_step[i] = 3;
            auto_life[i] = 32;
            auto_vx[i] = 0;
            auto_vy[i] = 1;
            auto_y[i] = 200;
            chain_count[i] = 3;
            chain_gap[i] = 0; /* 0 = unset; auto-fills from the sprite's height on first use */
            chain_delay[i] = 0;
            chain_vy[i] = 1;
            chain_pingpong[i] = false;
            pingpong_delay[i] = 0;
            stop_tick[i] = 0;
            subframe_swap_tick[i] = 0;
            subframe_waterline_y[i] = 0;
            subframe_fine_source[i] = -1;
        }
        hold_end[kWorldDummyDecapSlot] = true;
    }

    /* On by default. Marked rows are what World View is for, and the toggle
       was an extra step between opening it and seeing the lane tools -- with
       nothing marked the lane draw returns early anyway, so defaulting it on
       costs nothing and skips the ritual. */
    bool marked_play = true;
    /* There is deliberately no adjustable tick rate here. It used to be a
       `float fps` on this struct with a 1..60 slider, and every lane authored
       with that slider moved shipped mistimed: the export had to scale the
       hold by kMk2TickHz/fps to recover game ticks, and rounded when the
       preview rate was not a whole divisor of 54.7. The rate is hardware --
       read kMk2TickHz. Ticks/frame is the only speed knob. */
    /* Ticks each frame holds when nothing else authored a timing for it. The
       tick rate is hardware and does not move; this is the number that does,
       and it is the same number you write into the ASM. See
       kDefaultTimelineHold for why it is 4 rather than 1 — the Image timeline
       shares the constant so the two previews cannot disagree. */
    int default_hold = kDefaultTimelineHold;
    /* slot_hold is the row's ACTUAL hold -- what its T/f box shows and what a
       grown row extends at -- whether or not it is pinned. slot_hold_custom
       only answers "may the global overwrite this row", and defaults to false
       so the global genuinely reaches the whole scene.

       Keeping these two separate matters: slot_hold used to be read only for
       pinned rows, so an unpinned blood row sitting at its imported 1 tick
       reported the scene's 4 and looked as though it had already inherited
       when nothing had retimed it. Zero means never set, and only then does
       default_hold stand in. */
    int slot_hold[kWorldMarkedMaxTabs] = {};
    bool slot_hold_custom[kWorldMarkedMaxTabs] = {};
    float timer = 0.0f;
    int frame = 0;
    /* The tick by which every visible lane has finished, recomputed each
       frame by WorldUpdateMarkedLanePlayback. Only meaningful when nothing in
       the scene loops; the transport reads it to know that Play should start
       over rather than resume against an ended scene. Preview state, not
       saved. */
    int preview_end_tick = 0;
    /* Starts paused. World View opening straight into a running animation
       meant every layout read, every anipoint check and every drag started
       against a moving target; Play is one click when you actually want
       motion. Saved projects restore whatever they were saved with. */
    bool paused = true;
    bool mirror_active = false;
    bool mirror_other = false;
    bool mirror_extra[kWorldMarkedMaxTabs - 2] = {};
    bool dummy_decap_body = false;
    bool dummy_decap_reset = true;
    bool dummy_decap_manual = false;
    int dummy_decap_doc_idx = -1;
    std::string dummy_decap_prefix;
    int active_slot = -1;         /* slot controlled by Left/Right + hard-anipoint tools */
    int drag_slot = -1;
    int drag_frame = -1;
    bool drag_dual = false;        /* dragging the dual (second) instance */
    ImVec2 drag_mouse = ImVec2(0, 0);
    int drag_dx = 0;
    int drag_dy = 0;
    bool drag_mirror_x = false;
    bool drag_mirror_y = false;
    std::vector<int> drag_all_dx;
    std::vector<int> drag_all_dy;
    /* Anchor-link mode: drag from a feature on one sprite to its matching
       feature on another. The release point's sprite moves by its anipoint. */
    bool anchor_link_mode = false;
    bool anchor_link_active = false;
    IMG *anchor_link_source_img = NULL;
    ImVec2 anchor_link_source = ImVec2(0, 0);
    bool show_asm = false;
    bool draw_sprite_borders = true;
    bool show_boundary_overlay = true;
    std::string generated_asm;
    bool embedded_active = false;
    bool embedded_is_script = false;
    bool embedded_show_companions = false;
    int embedded_record_index = -1;
    int embedded_doc_idx = -1;
    std::string embedded_name;
    std::vector<std::string> embedded_frame_labels;
    std::vector<int> embedded_targets;
    std::vector<WorldMarkedSplitLane> split_lanes;
    bool lane_visible[kWorldMarkedMaxTabs] = {};
    /* Row order, as a rank per slot. Lanes are rebuilt from scratch every
       frame out of tab order plus the split list, so a user-chosen order
       cannot live in the vector -- it has to hang off the slot. -1 means "not
       placed yet"; WorldMarkedApplyLaneOrder hands those the next rank in
       build order, which is why an untouched session keeps exactly the order
       it always had. */
    int lane_order[kWorldMarkedMaxTabs];
    bool hold_end[kWorldMarkedMaxTabs] = {};
    std::vector<int> frame_delays[kWorldMarkedMaxTabs];
    std::vector<int> local_dx[kWorldMarkedMaxTabs];
    std::vector<int> local_dy[kWorldMarkedMaxTabs];
    std::vector<int> visible_from[kWorldMarkedMaxTabs];
    std::vector<int> visible_until[kWorldMarkedMaxTabs]; /* 0 = no hide cutoff */
    std::vector<int> motion_dx[kWorldMarkedMaxTabs];     /* visual pixels per tick; +X moves right */
    std::vector<int> motion_dy[kWorldMarkedMaxTabs];     /* visual pixels per tick; +Y moves down */
    std::vector<int> motion_cap_x[kWorldMarkedMaxTabs];  /* visual motion distance before stopping; 0 = unlimited */
    std::vector<int> motion_cap_y[kWorldMarkedMaxTabs];
    int lane_base_dx[kWorldMarkedMaxTabs] = {}; /* game-style lane spawn offset */
    int lane_base_dy[kWorldMarkedMaxTabs] = {};
    /* Rigid row: dragging ANY frame in the world moves the whole run by that
       delta, instead of just the frame under the cursor. A blood spray is one
       travelling effect, not a set of independently posed frames -- placing it
       a frame at a time means thirteen chances to leave one behind. Ctrl-drag
       does the same on any row; this makes it the default for rows that are
       only ever moved as a unit. */
    bool lane_rigid[kWorldMarkedMaxTabs] = {};
    std::vector<int> frame_mirror[kWorldMarkedMaxTabs]; /* per-frame flip bits: X=ani_flip, Y=ani_flip_v */
    std::vector<int> frame_z[kWorldMarkedMaxTabs];      /* per-entry draw priority; higher draws on top */
    std::vector<int> dual_on[kWorldMarkedMaxTabs];      /* per-entry second sprite instance enabled */
    std::vector<int> dual_dx[kWorldMarkedMaxTabs];      /* second instance local anipoint X delta */
    std::vector<int> dual_dy[kWorldMarkedMaxTabs];      /* second instance local anipoint Y delta */
    std::vector<int> dual_z[kWorldMarkedMaxTabs];       /* second instance draw priority */
    std::vector<std::vector<int>> entry_pieces[kWorldMarkedMaxTabs]; /* empty = single image (sequence_frames[fi]); non-empty = composite of these IMG indices, drawn together as one frame */
    /* Owning doc TAB INDEX per entry (not a Document*): document tabs live in
       a container that can reshuffle or replace its backing storage on
       reorder/close, so a Document* cached here across frames can dangle.
       -1 means "use this row's own sequence_doc_uid[slot]". Rows default every
       entry to -1; a frame dragged in from another row's document stores
       that document's current tab index so mixed-source rows resolve
       correctly, re-derived via document_get() fresh every frame.

       An index cannot dangle — document_get() bounds-checks it — but it does
       shift when a lower tab closes, so a mixed-source entry can end up
       naming the wrong file. Unlike the row binding above that is a
       misdraw rather than a crash, which is why this is still an index. */
    std::vector<int> frame_doc[kWorldMarkedMaxTabs];
    std::vector<int> sequence_frames[kWorldMarkedMaxTabs];
    std::vector<int> default_frames[kWorldMarkedMaxTabs];
    /* Owning document per row, held as a Document::uid — NOT a Document* and
       NOT a tab index, both of which go wrong the moment tabs change.

       Tabs live in a std::deque that erases on close and is move-assigned
       wholesale on reorder, so a cached Document* dangles: reading it after a
       close returns recycled heap, and walking the IMG list off it faults.
       That is the 0xC0000005 in doc_get_img this field exists to prevent.
       A tab index survives the free but not the shift — closing a lower tab
       silently rebinds every row above it to its neighbour's file.

       The uid survives both. 0 means "no document". Resolve through
       WorldMarkedRowDoc()/WorldMarkedRowDocIndex() at the point of use rather
       than caching the result across frames, and assign through
       WorldMarkedSetRowDoc()/WorldMarkedClearRowDoc(). */
    unsigned int sequence_doc_uid[kWorldMarkedMaxTabs] = {};
    int pingpong_delay[kWorldMarkedMaxTabs] = {}; /* pause before a generated reverse pass */
    int stop_tick[kWorldMarkedMaxTabs] = {};      /* 0 = run normally; >0 freezes preview at this tick */
    int auto_step[kWorldMarkedMaxTabs] = {};
    int auto_life[kWorldMarkedMaxTabs] = {};
    int auto_vx[kWorldMarkedMaxTabs] = {};
    int auto_vy[kWorldMarkedMaxTabs] = {};
    int auto_y[kWorldMarkedMaxTabs] = {};
    int chain_count[kWorldMarkedMaxTabs] = {};
    int chain_gap[kWorldMarkedMaxTabs] = {};
    int chain_delay[kWorldMarkedMaxTabs] = {};
    int chain_vy[kWorldMarkedMaxTabs] = {};
    bool chain_pingpong[kWorldMarkedMaxTabs] = {};
    int subframe_swap_tick[kWorldMarkedMaxTabs] = {};
    int subframe_waterline_y[kWorldMarkedMaxTabs] = {};   /* absolute world Y; 0 = unset */
    int subframe_fine_source[kWorldMarkedMaxTabs] = {};   /* IMG index used to look up fine subframes; -1 = unset */
};

struct WorldMarkedLane {
    Document *doc = nullptr;
    int doc_idx = -1;
    int delay_slot = 0;
    std::vector<int> frames;
    std::vector<std::vector<int>> frame_pieces;
    /* For ASM lanes whose pieces span multiple IMG files, the owning doc per
       frame / per piece. Empty for ordinary single-doc lanes. */
    std::vector<Document*> frame_docs;
    std::vector<std::vector<Document*>> frame_piece_docs;
    std::vector<std::string> frame_labels;
    int frame_pos = 0;
    int tick = 0;
    IMG *img = nullptr;
    bool dummy_decap = false;
    std::string label;
    std::string asm_label_part;
};

struct WorldAsmLaneFrame {
    const std::vector<int> *piece_img = nullptr;
    const std::vector<unsigned int> *piece_doc_uid = nullptr;
    int dx = 0;
    int dy = 0;
    bool mirror = false;
    bool mirror_v = false;
};

struct WorldMarkedAsmLaneInput {
    bool enabled = false;
    int slot_id = 0;
    Document *doc = nullptr;
    int doc_idx = -1;
    const char *name = nullptr;
    std::vector<WorldAsmLaneFrame> frames;
};

struct WorldMarkedLaneThumbClick {
    bool clicked = false;
    int doc_idx = -1;
    int img_idx = -1;
};

struct WorldMarkedPanelAction {
    bool copied_asm = false;
    int copied_lane_count = 0;
    bool request_save_asm = false;
    bool request_load_asm = false;
    bool request_save_project = false;
    bool request_load_project = false;
    /* Same file, added to the open scene instead of replacing it. */
    bool request_append_project = false;
    bool request_save_png = false;
    bool request_save_png_seq = false;
    bool request_load_bg = false;
    bool dummy_assigned = false;
    bool dummy_assign_failed = false;
};

struct WorldMarkedPanelResult {
    WorldMarkedPanelAction header;
    WorldMarkedLaneThumbClick thumb_click;
    bool copied_popup_asm = false;
};

struct WorldMarkedTabsResult {
    bool drew = false;
    WorldMarkedPanelResult panel;
};

struct WorldMarkedLaneRenderInfo {
    bool lane_rect_valid[kWorldMarkedMaxTabs] = {};
    bool lane_mirror_x[kWorldMarkedMaxTabs] = {};
    bool lane_mirror_y[kWorldMarkedMaxTabs] = {};
    ImVec2 lane_rect_min[kWorldMarkedMaxTabs] = {};
    ImVec2 lane_rect_max[kWorldMarkedMaxTabs] = {};
    bool lane_bad_y_anchor[kWorldMarkedMaxTabs] = {};
    /* Screen rect of the dual (second) sprite instance, when drawn. */
    bool dual_rect_valid[kWorldMarkedMaxTabs] = {};
    ImVec2 dual_rect_min[kWorldMarkedMaxTabs] = {};
    ImVec2 dual_rect_max[kWorldMarkedMaxTabs] = {};
    bool dual_bad_y_anchor[kWorldMarkedMaxTabs] = {};
};

struct WorldMarkedSceneResult {
    WorldCanvasLayout layout;
    WorldMarkedPanelLayout panel_layout;
    WorldMarkedLaneRenderInfo render_info;
};

WorldViewState &WorldView(void);
struct BddBackground;
BddBackground &WorldBackground(void);
AnipointLinkState &AnipointLink(void);
WorldCanvasLayout ComputeWorldCanvasLayout(ImVec2 avail, ImVec2 img_pos,
                                           int world_w, int world_h,
                                           int world_origin_x,
                                           int world_origin_y);
/* `hidden_lane_count` of the `lane_count` rows are collapsed to a single line
   (see WorldDrawMarkedLaneControls), so they ask for a fraction of the
   height a drawn row does. */
WorldMarkedPanelLayout ComputeWorldMarkedPanelLayout(ImVec2 avail,
                                                     ImVec2 img_pos,
                                                     int lane_count,
                                                     int hidden_lane_count = 0);
WorldMarkedSequenceState &WorldMarkedState(void);
bool *WorldMarkedMirrorFlag(WorldMarkedSequenceState &state, int slot);

/* Row -> document binding. Always resolve through these rather than caching a
   Document* or tab index across frames: see sequence_doc_uid for why both go
   wrong when tabs are closed or reordered. Both return "nothing" (NULL / -1)
   for an unbound row or one whose document has since been closed. */
Document *WorldMarkedRowDoc(const WorldMarkedSequenceState &state, int slot);
int WorldMarkedRowDocIndex(const WorldMarkedSequenceState &state, int slot);
/* Bind `slot` to the document at tab index `doc_idx`; an out-of-range index
   clears the binding. */
void WorldMarkedSetRowDoc(WorldMarkedSequenceState &state, int slot, int doc_idx);
void WorldMarkedClearRowDoc(WorldMarkedSequenceState &state, int slot);

/* Pure string helpers used by the marked World View panel. */
bool WorldDecapBodyFrameNo(const std::string &name, int *frame_no, std::string *prefix);
bool WorldDecapBodyPieceInfo(const std::string &name, int *frame_no,
                             std::string *prefix, int *kind);
bool WorldDecapPrefixFromName(const std::string &name, std::string *prefix);
int WorldDummyDecapFrameCount(void);
int WorldDummyDecapFrameNo(int index);
void WorldResetDummyDecapDelays(WorldMarkedSequenceState &state, int frame_count);
bool WorldAssignSelectedDummyDecap(WorldMarkedSequenceState &state,
                                   IMG *selected_img,
                                   int active_doc_idx);
void WorldCollectMarkedFrames(Document *doc, std::vector<int> &out);
/* Marked sprites grouped by chopped-piece family: out_frames gets one entry per
   group (its first piece), out_pieces gets every piece in that group. */
void WorldCollectMarkedFrameGroups(Document *doc, std::vector<int> &out_frames,
                                   std::vector<std::vector<int>> &out_pieces);
WorldMarkedLane WorldBuildDummyDecapLane(WorldMarkedSequenceState &state,
                                         int active_doc_idx);
bool WorldAppendMarkedDocumentLanes(WorldMarkedSequenceState &state,
                                    int active_doc_idx,
                                    std::vector<WorldMarkedLane> &lanes,
                                    bool *dummy_decap_missing);
bool WorldAppendMarkedSourceLane(WorldMarkedSequenceState &state, int doc_idx,
                                 std::vector<WorldMarkedLane> &lanes,
                                 bool used_source_slots[kWorldMarkedSourceTabs]);
bool WorldAppendAsmLane(WorldMarkedSequenceState &state, const char *name,
                        const std::vector<WorldAsmLaneFrame> &frames,
                        Document *doc, int doc_idx, int slot_id,
                        std::vector<WorldMarkedLane> &lanes);
WorldMarkedTabsResult WorldDrawMarkedTabs(WorldMarkedSequenceState &state,
                                          const WorldViewState &world,
                                          ImVec2 avail,
                                          ImVec2 img_pos,
                                          float delta_time,
                                          int active_doc_idx,
                                          IMG *selected_img,
                                          const std::vector<WorldMarkedAsmLaneInput> &asm_lanes,
                                          bool draw_panel);
bool WorldUpdateMarkedLanePlayback(WorldMarkedSequenceState &state,
                                   std::vector<WorldMarkedLane> &lanes,
                                   float delta_time);
ImU32 WorldMarkedLaneOutlineColor(int slot);
void WorldDrawMarkedLaneSprites(ImDrawList *dl, WorldMarkedSequenceState &state,
                                const std::vector<WorldMarkedLane> &lanes,
                                const WorldCanvasLayout &layout,
                                WorldMarkedLaneRenderInfo &render_info);
/* Same scene as WorldDrawMarkedLaneSprites, composited 1:1 into a world-pixel
   RGBA buffer (no borders/overlays) for file export. `use_lane_alpha` keeps the
   per-lane translucency used on screen; false composites every lane opaque.
   Returns how many sprite instances landed inside the world rect. */
int WorldComposeMarkedSceneRgba(WorldMarkedSequenceState &state,
                                const std::vector<WorldMarkedLane> &lanes,
                                int world_w, int world_h,
                                int origin_x, int origin_y,
                                bool use_lane_alpha,
                                std::vector<unsigned char> &out);
/* Write the World View composite at the current tick to `path`. */
bool ExportWorldViewPng(const char *path, bool crop_to_content, bool use_lane_alpha);
/* Write one PNG per tick as <base>_0000.PNG ... Returns frames written and,
   via out_total_ticks, how many ticks the sequence spans. */
int ExportWorldViewPngSequence(const char *path_base, bool crop_to_content,
                               bool use_lane_alpha, int *out_total_ticks);
WorldMarkedSceneResult WorldDrawMarkedScene(WorldMarkedSequenceState &state,
                                            const WorldViewState &world,
                                            const std::vector<WorldMarkedLane> &lanes,
                                            ImVec2 avail,
                                            ImVec2 img_pos);
void WorldDrawMarkedLaneTags(ImDrawList *dl,
                             const std::vector<WorldMarkedLane> &lanes,
                             const WorldMarkedLaneRenderInfo &render_info,
                             ImVec2 world_pos);
void WorldDrawMarkedLaneStatus(ImDrawList *dl, WorldMarkedSequenceState &state,
                               const std::vector<WorldMarkedLane> &lanes,
                               const WorldMarkedLaneRenderInfo &render_info,
                               const WorldCanvasLayout &layout,
                               ImVec2 world_pos, float world_width);
WorldMarkedPanelAction WorldDrawMarkedPanelHeader(WorldMarkedSequenceState &state,
                                                  const std::vector<WorldMarkedLane> &lanes,
                                                  bool dummy_decap_missing,
                                                  IMG *selected_img,
                                                  int active_doc_idx);
WorldMarkedPanelResult WorldDrawMarkedPanel(WorldMarkedSequenceState &state,
                                            std::vector<WorldMarkedLane> &lanes,
                                            const WorldMarkedPanelLayout &layout,
                                            bool dummy_decap_missing,
                                            IMG *selected_img,
                                            int active_doc_idx);
/* Returns true when the row was removed during the call, in which case the
   lane is stale and nothing else may be drawn from it this frame. */
bool WorldDrawMarkedLaneControls(WorldMarkedSequenceState &state,
                                 WorldMarkedLane &lane,
                                 const std::vector<WorldMarkedLane> &lanes,
                                 int display_slot);
/* `lanes` is needed for the frame right-click menu, which can spawn a new row
   and therefore has to know which slots are taken. */
WorldMarkedLaneThumbClick WorldDrawMarkedLaneThumbnails(WorldMarkedSequenceState &state,
                                                        WorldMarkedLane &lane,
                                                        const std::vector<WorldMarkedLane> &lanes);

/* ---- Blood lanes -------------------------------------------------------
   MK2 does not animate blood inside the move that caused it: MKREACT.ASM
   calls create_blood_proc with a blood_procs[] index and a packed [y,x]
   offset from the victim's origin, and the spray runs as its own process
   from that point. A blood row here is the authoring twin of that -- its own
   row, its own art, scheduled to start at one entry's tick and anchored on
   that entry's anipoint, which is the same offset the engine takes (and in
   the same facing space, since multi_adjust_xy mirrors it for a flipped
   object).

   The art is whatever is open: any numbered run of sprites whose names read
   as blood (BLOOD/SPILL/SPURT/DRIP/SPRAY/SPLAT/GUTS/GORE) in any tab. */
struct WorldBloodRun {
    int doc_idx = -1;
    std::string doc_name;
    std::string stem;            /* SPILL, from SPILL1..SPILL13 */
    std::vector<int> frames;     /* image indices, in image-list order */
};
void WorldCollectBloodRuns(std::vector<WorldBloodRun> &out);

/* An IMG on disk whose FILE name reads as blood -- BLOOD.IMG, MK1BLOOD.IMG.
   The sprites inside one are not required to say "blood" themselves:
   MKBLOOD.TBL's runs are GUSHER/GOOBER/BIGBLD/BPARTS/STAB at least as often
   as SPILL/SPURT, so matching sprite names alone finds barely half of what
   BLOOD.IMG holds. The file name is what qualifies its whole contents. */
struct WorldBloodFile {
    std::string path;      /* full path */
    std::string name;      /* file name only */
    bool open = false;     /* already an open tab */
};
/* Scan the folders an IMG could plausibly live in -- every open tab's, the
   active document's, $IMGDIR, and their sibling data/ dirs -- for those files.
   Cached; the scan re-runs when the open tabs change or `force_rescan`. */
void WorldFindBloodImgFiles(std::vector<WorldBloodFile> &out, bool force_rescan);
/* Open every discovered blood IMG that is not already a tab, then put the tab
   that was active back. Returns how many were opened. */
int WorldOpenBloodImgFiles(void);
/* Add a row playing `run` once, starting at the tick `src_entry` of `src_lane`
   begins and sitting on that entry's anipoint. */
bool WorldMarkedCreateBloodLane(WorldMarkedSequenceState &state,
                                const std::vector<WorldMarkedLane> &lanes,
                                const WorldMarkedLane &src_lane,
                                int src_entry,
                                const WorldBloodRun &run,
                                std::string *out_msg);
/* Show the Subframes/Tick/Swap strip above a row's thumbnails. Off by
   default; toggled from the app's View menu. Defined in ui_canvas.cpp. */
extern bool g_world_show_subframe_tool;
std::string WorldBuildMarkedAsm(WorldMarkedSequenceState &state,
                                const std::vector<WorldMarkedLane> &lanes);
bool WorldDrawMarkedAsmPopup(WorldMarkedSequenceState &state);
/* `canvas_min`/`canvas_max` bound where a grab may start. That is the whole
   canvas rect, not the playfield: a sprite anchored past the world edge still
   draws out there and has to stay grabbable. */
void WorldHandleMarkedLaneDrag(ImDrawList *dl, WorldMarkedSequenceState &state,
                               const std::vector<WorldMarkedLane> &lanes,
                               const WorldMarkedLaneRenderInfo &render_info,
                               const WorldCanvasLayout &world_layout,
                               const WorldMarkedPanelLayout &panel_layout,
                               ImVec2 canvas_min, ImVec2 canvas_max);
std::string WorldMarkedAsmToken(const std::string &raw, const char *fallback);
std::string WorldMarkedAsmLabelPart(const char *raw, int slot);
int ClampWorldMarkedAniptDelta(int value);
int ClampWorldMarkedVisibleFrom(int value);
int ClampWorldMarkedVisibleUntil(int value);
int ClampWorldMarkedZ(int value);
int ClampWorldMarkedMotion(int value);
int ClampWorldMarkedMotionCap(int value);
void WorldMarkedRestart(WorldMarkedSequenceState &state);
void StepWorldMarkedSequence(WorldMarkedSequenceState &state, int delta);
void WorldMarkedSetTick(WorldMarkedSequenceState &state, int tick);
/* True when an embedded World View sequence/script table is loaded and showing
   its entries (so Up/Down can step through its frames instead of the image list). */
bool WorldEmbeddedSeqScrActive(const WorldMarkedSequenceState &state);
/* Leave the loaded WIMP sequence/script table and return to the ordinary
   marked-row World View.  The source SEQSCR data is left untouched. */
void WorldExitEmbeddedSeqScr(WorldMarkedSequenceState &state);
/* Attach one more sprite to an existing World View frame.  Every piece is
   placed from its own IMG anipoint, so the resulting composite stays tied to
   the frame and is preserved by the World View ASM/project export. */
bool WorldMarkedAttachSpriteToFrame(WorldMarkedSequenceState &state,
                                    int slot, int frame_idx,
                                    Document *doc, int doc_idx,
                                    int sprite_idx);
/* Advance the loaded embedded sequence/script by `delta` entries (wrapping) and
   sync the editor selection to the target sprite. */
void StepWorldEmbeddedSeqScrEntry(WorldMarkedSequenceState &state, int delta);
void EnsureWorldMarkedFrameDelays(WorldMarkedSequenceState &state, int slot, int frame_count);
/* Push `ticks` into every row that is still following the global hold, and
   restart the tick clock so the new timing shows immediately. Rows pinned with
   their own T/f are skipped unless `include_custom`, the explicit "make
   everything match" action behind the All button. The dummy-decap slot is
   always left alone: its holds are a canned effect with their own reset
   button, not user timing.

   Both setters also re-lay a SCHEDULED row's Show@/Hide@ windows at the new
   hold. Blood sprays are scheduled runs (visible_from = base + i*hold), and
   rewriting frame_delays without the windows left them playing at their old
   cadence -- the row appeared to ignore the speed control completely. Windows
   that are not a uniform run at the old hold (Build Chain output, hand-typed
   Show@/Hide@) are left untouched rather than guessed at. */
void WorldMarkedApplyUniformHold(WorldMarkedSequenceState &state, int ticks,
                                 bool include_custom = false);
/* Set one row's hold. custom=true pins it against the global; custom=false
   hands it back so the global owns it again. */
void WorldMarkedSetSlotHold(WorldMarkedSequenceState &state, int slot,
                            int ticks, bool custom);
/* The hold this row actually runs at -- pinned or not. Falls back to the
   global only for a row whose hold was never set. */
int WorldMarkedSlotHold(const WorldMarkedSequenceState &state, int slot);

/* ---- Merging a second project into the open scene ----------------------
   Load replaces the workspace; these two let another .WAX be laid on top of
   it instead. The copy is field-by-field on purpose: the origin, tick clock,
   global hold and ASM lanes belong to the scene already open. */
int WorldMarkedFirstFreeSourceSlot(const WorldMarkedSequenceState &state);
bool WorldMarkedCopySlotFrom(WorldMarkedSequenceState &state, int dst_slot,
                             WorldMarkedSequenceState &src, int src_slot,
                             int doc_idx);

/* ---- Baking World View placement back into the IMG ----------------------
   A marked lane positions a frame at (origin - (anipoint + local dX/dY)), and
   those deltas are preview state: they live in WorldMarkedSequenceState, not
   in the file, so a drag in World View looks right and then saves as nothing.
   Baking folds the delta into the sprite's own anipoint — the drawn position
   does not move, but it now survives a save. */

/* Locate the World View entry that draws this sprite. */
bool WorldMarkedFindEntryForImage(WorldMarkedSequenceState &state,
                                  int doc_idx, int img_idx,
                                  int *out_slot, int *out_entry);

/* Fold local dX/dY into anix/aniy for one entry, or for the whole slot when
   `entry` is negative, and zero the deltas. Returns the number of sprites
   changed; `out_conflicts` counts entries skipped because the same IMG was
   already baked from an earlier entry with its own offset. */
int WorldMarkedBakeEntryOffsets(WorldMarkedSequenceState &state,
                                int slot, int entry, int *out_conflicts);
int WorldMarkedTickForFrame(WorldMarkedSequenceState &state, int slot,
                            int frame_count, int frame_idx);
int WorldMarkedSequenceTicks(WorldMarkedSequenceState &state, int slot, int frame_count);
int WorldMarkedFrameForTick(WorldMarkedSequenceState &state, int slot,
                            int frame_count, int tick, bool hold_final);
void WorldMarkedClearSequenceState(WorldMarkedSequenceState &state, int slot);
void WorldMarkedSyncSequenceOverride(WorldMarkedSequenceState &state, int slot,
                                     Document *doc, int doc_idx,
                                     std::vector<int> &frames,
                                     std::vector<std::vector<int>> &frame_pieces,
                                     std::vector<std::string> &frame_labels);
void WorldMarkedResetSequenceToDefaults(WorldMarkedSequenceState &state, int slot);
bool WorldMarkedSplitLaneAtFrame(WorldMarkedSequenceState &state,
                                 const WorldMarkedLane &lane,
                                 const std::vector<WorldMarkedLane> &lanes,
                                 int frame_idx);
void WorldMarkedClearSplitLanes(WorldMarkedSequenceState &state);
bool WorldMarkedReverseSlot(WorldMarkedSequenceState &state, WorldMarkedLane &lane);
bool WorldMarkedDeleteSplitSlot(WorldMarkedSequenceState &state, int slot);

/* ---- Row order and removal ---------------------------------------------
   Apply the stored order once per build, after the last lane has been
   appended, so the panel, the canvas draw order (equal Z falls back to row
   order) and the ASM export all agree on what "row 1" means. */
void WorldMarkedApplyLaneOrder(WorldMarkedSequenceState &state,
                               std::vector<WorldMarkedLane> &lanes);
/* Swap the displayed row at `display_index` with the neighbour `dir` away
   (-1 up, +1 down). False at the ends, or for an unrecognised row. */
bool WorldMarkedMoveLane(WorldMarkedSequenceState &state,
                         const std::vector<WorldMarkedLane> &lanes,
                         int display_index, int dir);
/* What removing this row would actually do, in words, for the confirmation. */
std::string WorldMarkedRemoveLaneDescription(const WorldMarkedSequenceState &state,
                                             const WorldMarkedLane &lane);
/* Remove a row at its source: split rows are dropped, the dummy body, ASM and
   embedded lanes are switched off, and a base marked row goes away by
   unmarking the frames in its document that created it. `out_msg` receives a
   sentence describing what happened. */
bool WorldMarkedRemoveLane(WorldMarkedSequenceState &state,
                           const WorldMarkedLane &lane,
                           std::string *out_msg);

/* ---- Promote a row into the document's own sequence block --------------
   A row is preview state; a SEQSCR record is the file. An ENTRY holds four
   things -- target sprite, ticks, dX, dY -- and that is the whole overlap.
   Everything else a row carries (per-entry flips, Z, motion, show/hide
   ticks, the dual copy, composite pieces beyond the primary) has nowhere to
   go in the IMG, so it is counted and named in `out_msg` rather than
   silently dropped.

   `seq_name` names the record (empty falls back to a name derived from the
   row's first sprite). `target_doc_idx` is the open tab to write into, -1 for
   the active one; the destination is made active for the write and restored
   after, and any frame living in another file is imported into it on the way.

   Entries are written back-to-front, because that is how a non-script SEQSCR
   record is stored -- see the std::reverse in the body. */
bool WorldMarkedPromoteLaneToSequence(WorldMarkedSequenceState &state,
                                      const WorldMarkedLane &lane,
                                      const char *seq_name, int target_doc_idx,
                                      std::string *out_msg);
void WorldMarkedDuplicateSequenceEntry(WorldMarkedSequenceState &state, int slot, int frame_idx);
void WorldMarkedMoveSequenceEntry(WorldMarkedSequenceState &state, int slot, int frame_idx, int dir);
void WorldMarkedDeleteSequenceEntry(WorldMarkedSequenceState &state, int slot, int frame_idx);
bool WorldMarkedMoveEntryBetweenSlots(WorldMarkedSequenceState &state,
                                      int src_slot, int src_frame_idx,
                                      int dst_slot, int dst_frame_idx);
Document *WorldMarkedResolveEntryDoc(Document *row_doc, int doc_idx_override);
std::vector<Document*> WorldMarkedResolveFrameDocs(Document *row_doc,
                                                   const std::vector<int> &doc_idx_overrides);
void WorldMarkedBuildSingleFrameLane(Document *doc, const std::vector<int> &frames,
                                     std::vector<std::vector<int>> &frame_pieces,
                                     std::vector<std::string> &frame_labels,
                                     const std::vector<std::vector<int>> *piece_overrides = NULL,
                                     const std::vector<int> *doc_idx_overrides = NULL);
void WorldRefreshMarkedLaneAfterSequenceEdit(WorldMarkedSequenceState &state,
                                             WorldMarkedLane &lane,
                                             int &edit_frame);
std::string WorldBuildSeqScrAsmExport(int record_index);

/* Draw the single-sprite World View canvas into the current ImGui window.
   Returns true when it consumed/reserved the canvas area. */
bool DrawWorldViewSingleSprite(ImVec2 avail, ImVec2 img_pos, ImGuiIO &io,
                               IMG *img, SDL_Texture *img_texture,
                               int image_idx, int image_count,
                               int world_w, int world_h,
                               int world_origin_x, int world_origin_y,
                               bool onion_enabled, bool mirror_active,
                               bool show_borders, bool show_anipoint);
bool DrawAnipointLinkCanvas(ImVec2 avail, ImVec2 img_pos, ImGuiIO &io);

/* ---- Sequence/Script frame library --------------------------------------
   MK2 splits one character's sprites across numbered sibling IMGs (CAGE1.IMG
   .. CAGE10.IMG). The Anim workspace browses that whole set as one library so
   a sequence can be built without hunting through tabs. Subframe pieces are
   left out: they are parts of a frame, not frames. */
struct SeqScrFrameRef {
    int doc_idx = -1;
    int img_idx = -1;
    int file_slot = -1;    /* index into SeqScrFrameLibrary::files */
    int subframes = 0;     /* child pieces this parent owns */
    std::string name;
};

struct SeqScrFrameLibrary {
    std::string stem;                    /* "CAGE" */
    std::vector<std::string> files;      /* "CAGE1.IMG" .. in numeric order */
    std::vector<int> file_docs;          /* doc tab index per file, -1 = closed */
    std::vector<SeqScrFrameRef> frames;  /* parents only, file then list order */
    int open_files = 0;
};

const SeqScrFrameLibrary &SeqScrFrameLib(void);
/* Rebuild the library for the active document's numbered sibling set. When
   `open_missing` is set, siblings that are not open yet are loaded as document
   tabs (the active tab is restored afterwards). */
void SeqScrRebuildFrameLibrary(bool open_missing);
/* Draw the frame browser (file dropdown, frame list, mark/add buttons) into
   the current ImGui window, using `avail_h` for the list height budget. */
void DrawSeqScrFrameBrowser(float avail_h);
/* Frame the Anim workspace's corner box should show instead of the entry under
   the playhead. False when the browser has no selection. */
bool SeqScrBrowserPreviewFrame(int *doc_idx, int *img_idx);
/* True while the frame browser owns Up/Down/Space, i.e. it was the last list
   the user clicked in and it has rows to walk. Mirrors g_palette_nav. */
bool SeqScrFrameNavActive(void);
/* Move the browser highlight by `delta` rows within the current file/filter
   view, previewing whatever it lands on. */
void SeqScrStepFrameSelection(int delta);
/* Toggle the highlighted frame's mark bit. */
void SeqScrToggleSelectedMark(void);
/* How many entries in the loaded record point at a sprite outside the IMG that
   owns it. Those live in the preview/ASM export only — a SEQSCR entry is an
   index into its own file's image list and cannot name a foreign sprite. */
int SeqScrForeignEntryCount(const WorldMarkedSequenceState &state);

/* World-view anchor Y that stands `img` on WorldView().floor_y, given that a
   sprite's top edge is drawn at anchor_y - anipoint_effective(aniy, h).
   Returns the current origin_y unchanged when the sprite is unusable. */
int WorldGamePlacementOriginY(const IMG *img);
/* Re-derive the shared anchor from the loaded record's first drawable frame,
   so the animation plays where the game would draw it. No-op when
   game_placement is off or nothing is loaded. */
void WorldApplyGamePlacement(const WorldMarkedSequenceState &state);

/* Draw the Sequence/Script ("Anim") workspace into the current ImGui window:
   the loaded SEQSCR record animating in a world-sized viewport, a corner
   sprite inspector for the entry under the playhead, and the record list plus
   its entry table underneath. Returns true when it consumed the canvas area. */
bool DrawSeqScrWorkspace(ImVec2 avail, ImVec2 img_pos, ImGuiIO &io);

/* Destroy module-owned transient/cached canvas textures. */
void ClearCanvasUiTextures(void);

/* Draw the main edit canvas window. */
void DrawCanvasWindow(float canvas_x, float canvas_y, float canvas_w, float canvas_h);

/* Clipboard, Paste, and Layer Operations */
void ClearPixelClipboard(void);
bool BuildClipboardPaletteMap(const PAL *target_pal, unsigned char map[256]);
void copy_image(bool cut);
void PasteClipboardAsNewImage(void);
void CutSelectionToNewImage(void);
void CopySelectionToNewImage(void);
bool paste_preview_rgba(unsigned char src_ci, unsigned char dst_ci,
                        const PAL *target_pal,
                        const unsigned char pal_map[256],
                        bool remap_palette, int x, int y,
                        int *r, int *g, int *b, int *a);
void flip_clipboard_horizontal(void);
void flip_clipboard_vertical(void);
void flatten_img_layer(IMG *img);
void delete_img_layer(IMG *img);
void flip_layer_horizontal(SpriteLayer *L);
void flip_layer_vertical(SpriteLayer *L);
void drop_paste_to_layer(void);
void apply_pasted_region(void);
void paste_image(void);
bool clipboard_secondary_anipoint_in_use(void);

/* Free Transform */
void xform_begin(void);
void xform_cancel(void);
void xform_commit(void);

/* Selections */
void select_all(void);
void deselect_all(void);
void invert_selection(void);
bool selection_contains_pixel(IMG *img, int x, int y);
void selection_begin_add_drag(int sw, int sh, bool add);
void selection_finish_add_drag(int sw, int sh);
