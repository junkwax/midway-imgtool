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

enum {
    kWorldMarkedSourceTabs = 4,
    kWorldDummyDecapSlot = 4,      /* optional dummy body */
    kWorldAsmSlot = 5,             /* ASM-driven player lane */
    kWorldAsmOpponentSlot = 6,     /* ASM-driven opponent lane (fatalities) */
    kWorldMarkedMaxTabs = 7        /* 4 tabs + dummy + 2 ASM lanes */
};

struct WorldViewState {
    bool enabled = false;
    int w = 400;        /* arcade playfield width */
    int h = 254;        /* arcade playfield height */
    int origin_x = 200; /* anchor target inside world */
    int origin_y = 20;  /* anchor target inside world (top-anchored) */
    bool onion = false; /* faintly draw prev frame underneath */
};

struct WorldCanvasLayout {
    float scale = 1.0f;
    float width = 0.0f;
    float height = 0.0f;
    ImVec2 pos = ImVec2(0, 0);
    float origin_x = 0.0f;
    float origin_y = 0.0f;
};

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

enum class TransformHandle {
    None,
    Move,
    Rotate,
    TL, T, TR,
    L,      R,
    BL, B, BR
};

struct CanvasTransformHandleOverlay {
    TransformHandle hover = TransformHandle::None;
    bool rotate_hover = false;
    bool chain_hover = false;
    ImVec2 center = ImVec2(0, 0);
    ImVec2 chain_min = ImVec2(0, 0);
    ImVec2 chain_max = ImVec2(0, 0);
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
CanvasTransformHandleOverlay DrawCanvasTransformHandles(
    ImDrawList *dl, const ImVec2 corners[4], ImVec2 mouse,
    TransformHandle active_handle, bool aspect_locked);
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

struct WorldMarkedSequenceState {
    bool marked_play = false;
    float fps = 12.0f;
    float timer = 0.0f;
    int frame = 0;
    bool paused = false;
    bool mirror_active = false;
    bool mirror_other = false;
    bool mirror_extra[5] = {false, false, false, false, false};
    bool dummy_decap_body = false;
    bool dummy_decap_reset = true;
    bool dummy_decap_manual = false;
    int dummy_decap_doc_idx = -1;
    std::string dummy_decap_prefix;
    int drag_slot = -1;
    int drag_frame = -1;
    ImVec2 drag_mouse = ImVec2(0, 0);
    int drag_dx = 0;
    int drag_dy = 0;
    bool drag_mirror = false;
    bool show_asm = false;
    std::string generated_asm;
    bool hold_end[kWorldMarkedMaxTabs] = {false, false, false, false, true, false, false};
    std::vector<int> frame_delays[kWorldMarkedMaxTabs];
    std::vector<int> local_dx[kWorldMarkedMaxTabs];
    std::vector<int> local_dy[kWorldMarkedMaxTabs];
    std::vector<int> visible_from[kWorldMarkedMaxTabs];
    std::vector<int> frame_mirror[kWorldMarkedMaxTabs]; /* per-frame flip (ASM ani_flip) */
    std::vector<int> sequence_frames[kWorldMarkedMaxTabs];
    std::vector<int> default_frames[kWorldMarkedMaxTabs];
    Document *sequence_doc[kWorldMarkedMaxTabs] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    int sequence_doc_idx[kWorldMarkedMaxTabs] = {-1, -1, -1, -1, -1, -1, -1};
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
    IMG *img = nullptr;
    bool dummy_decap = false;
    std::string label;
    std::string asm_label_part;
};

struct WorldAsmLaneFrame {
    const std::vector<int> *piece_img = nullptr;
    const std::vector<Document*> *piece_doc = nullptr;
    int dx = 0;
    int dy = 0;
    bool mirror = false;
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
    bool lane_rect_valid[kWorldMarkedMaxTabs] = {false, false, false, false, false, false, false};
    bool lane_mirror_x[kWorldMarkedMaxTabs] = {false, false, false, false, false, false, false};
    ImVec2 lane_rect_min[kWorldMarkedMaxTabs] = {};
    ImVec2 lane_rect_max[kWorldMarkedMaxTabs] = {};
};

struct WorldMarkedSceneResult {
    WorldCanvasLayout layout;
    WorldMarkedPanelLayout panel_layout;
    WorldMarkedLaneRenderInfo render_info;
};

WorldViewState &WorldView(void);
WorldCanvasLayout ComputeWorldCanvasLayout(ImVec2 avail, ImVec2 img_pos,
                                           int world_w, int world_h,
                                           int world_origin_x,
                                           int world_origin_y);
WorldMarkedPanelLayout ComputeWorldMarkedPanelLayout(ImVec2 avail,
                                                     ImVec2 img_pos,
                                                     int lane_count);
WorldMarkedSequenceState &WorldMarkedState(void);
bool *WorldMarkedMirrorFlag(WorldMarkedSequenceState &state, int slot);

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
WorldMarkedLane WorldBuildDummyDecapLane(WorldMarkedSequenceState &state,
                                         int active_doc_idx);
bool WorldAppendMarkedDocumentLanes(WorldMarkedSequenceState &state,
                                    int active_doc_idx,
                                    std::vector<WorldMarkedLane> &lanes,
                                    bool *dummy_decap_missing);
bool WorldAppendMarkedSourceLane(WorldMarkedSequenceState &state, int doc_idx,
                                 std::vector<WorldMarkedLane> &lanes);
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
                                          const std::vector<WorldMarkedAsmLaneInput> &asm_lanes);
bool WorldUpdateMarkedLanePlayback(WorldMarkedSequenceState &state,
                                   std::vector<WorldMarkedLane> &lanes,
                                   float delta_time);
ImU32 WorldMarkedLaneOutlineColor(int slot);
void WorldDrawMarkedLaneSprites(ImDrawList *dl, WorldMarkedSequenceState &state,
                                const std::vector<WorldMarkedLane> &lanes,
                                const WorldCanvasLayout &layout,
                                WorldMarkedLaneRenderInfo &render_info);
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
void WorldDrawMarkedLaneControls(WorldMarkedSequenceState &state,
                                 WorldMarkedLane &lane,
                                 int display_slot);
WorldMarkedLaneThumbClick WorldDrawMarkedLaneThumbnails(WorldMarkedSequenceState &state,
                                                        WorldMarkedLane &lane);
std::string WorldBuildMarkedAsm(WorldMarkedSequenceState &state,
                                const std::vector<WorldMarkedLane> &lanes);
bool WorldDrawMarkedAsmPopup(WorldMarkedSequenceState &state);
void WorldHandleMarkedLaneDrag(ImDrawList *dl, WorldMarkedSequenceState &state,
                               const std::vector<WorldMarkedLane> &lanes,
                               const WorldMarkedLaneRenderInfo &render_info,
                               const WorldCanvasLayout &world_layout,
                               const WorldMarkedPanelLayout &panel_layout);
std::string WorldMarkedAsmToken(const std::string &raw, const char *fallback);
std::string WorldMarkedAsmLabelPart(const char *raw, int slot);
int ClampWorldMarkedAniptDelta(int value);
int ClampWorldMarkedVisibleFrom(int value);
void WorldMarkedRestart(WorldMarkedSequenceState &state);
void StepWorldMarkedSequence(WorldMarkedSequenceState &state, int delta);
void EnsureWorldMarkedFrameDelays(WorldMarkedSequenceState &state, int slot, int frame_count);
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
void WorldMarkedDuplicateSequenceEntry(WorldMarkedSequenceState &state, int slot, int frame_idx);
void WorldMarkedMoveSequenceEntry(WorldMarkedSequenceState &state, int slot, int frame_idx, int dir);
void WorldMarkedDeleteSequenceEntry(WorldMarkedSequenceState &state, int slot, int frame_idx);
void WorldMarkedBuildSingleFrameLane(Document *doc, const std::vector<int> &frames,
                                     std::vector<std::vector<int>> &frame_pieces,
                                     std::vector<std::string> &frame_labels);
void WorldRefreshMarkedLaneAfterSequenceEdit(WorldMarkedSequenceState &state,
                                             WorldMarkedLane &lane,
                                             int &edit_frame);

/* Draw the single-sprite World View canvas into the current ImGui window.
   Returns true when it consumed/reserved the canvas area. */
bool DrawWorldViewSingleSprite(ImVec2 avail, ImVec2 img_pos, ImGuiIO &io,
                               IMG *img, SDL_Texture *img_texture,
                               int image_idx, int image_count,
                               int world_w, int world_h,
                               int world_origin_x, int world_origin_y,
                               bool onion_enabled, bool mirror_active);

/* Destroy module-owned transient/cached canvas textures. */
void ClearCanvasUiTextures(void);
