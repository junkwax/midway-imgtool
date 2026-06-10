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

WorldViewState &WorldView(void);
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
void WorldCollectMarkedFrames(Document *doc, std::vector<int> &out);
WorldMarkedLane WorldBuildDummyDecapLane(WorldMarkedSequenceState &state,
                                         int active_doc_idx);
bool WorldAppendMarkedSourceLane(WorldMarkedSequenceState &state, int doc_idx,
                                 std::vector<WorldMarkedLane> &lanes);
bool WorldUpdateMarkedLanePlayback(WorldMarkedSequenceState &state,
                                   std::vector<WorldMarkedLane> &lanes,
                                   float delta_time);
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
