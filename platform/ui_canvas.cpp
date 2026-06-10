/*************************************************************
 * platform/ui_canvas.cpp
 * Canvas/World-View helpers declared in ui_canvas.h.
 *************************************************************/
#include "ui_canvas.h"

#include "anipoint_edit.h"  /* set_primary_anipoint_with_sequence */
#include "img_format.h"     /* get_img */
#include "img_util.h"       /* img_name_string */
#include "shim_vid.h"       /* g_palette */
#include "ui_internal.h"    /* g_imgui_renderer */
#include "ui_timeline.h"    /* ClampTimelineHold */
#include "world_render.h"   /* doc_get_img */

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>

static SDL_Texture *s_world_onion_tex = NULL;
static int s_world_onion_tex_w = 0;
static int s_world_onion_tex_h = 0;
static int s_world_onion_idx = -1;

WorldViewState &WorldView(void)
{
    static WorldViewState state;
    return state;
}

WorldCanvasLayout ComputeWorldCanvasLayout(ImVec2 avail, ImVec2 img_pos,
                                           int world_w, int world_h,
                                           int world_origin_x,
                                           int world_origin_y)
{
    WorldCanvasLayout layout;
    float fit_x = avail.x / (float)world_w;
    float fit_y = avail.y / (float)world_h;
    layout.scale = (fit_x < fit_y) ? fit_x : fit_y;
    if (layout.scale < 1.0f) layout.scale = 1.0f;
    layout.scale = (float)(int)layout.scale;
    if (layout.scale < 1.0f) layout.scale = 1.0f;

    layout.width = (float)world_w * layout.scale;
    layout.height = (float)world_h * layout.scale;
    layout.pos = ImVec2(img_pos.x + (avail.x - layout.width) * 0.5f,
                        img_pos.y + (avail.y - layout.height) * 0.5f);
    layout.origin_x = layout.pos.x + world_origin_x * layout.scale;
    layout.origin_y = layout.pos.y + world_origin_y * layout.scale;
    return layout;
}

WorldMarkedSequenceState &WorldMarkedState(void)
{
    static WorldMarkedSequenceState state;
    return state;
}

bool *WorldMarkedMirrorFlag(WorldMarkedSequenceState &state, int slot)
{
    if (slot == 0) return &state.mirror_active;
    if (slot == 1) return &state.mirror_other;
    if (slot >= 2 && slot < kWorldMarkedMaxTabs) return &state.mirror_extra[slot - 2];
    return nullptr;
}

static const int kWorldDummyDecapOrder[] = {
    1, 2, 3,
    4, 3, 4, 3, 4, 3,
    4, 5, 6, 7
};

static const int kWorldDummyDecapDefaultDelays[] = {
    48, 6, 6,
    10, 10, 10, 10, 10, 10,
    6, 6, 6, 6
};

static bool WorldReadDecapFrameNo(const std::string &upper, size_t pos,
                                  int *frame_no, size_t *end_pos)
{
    if (pos >= upper.size() || !std::isdigit((unsigned char)upper[pos])) return false;

    int val = 0;
    size_t p = pos;
    while (p < upper.size() && std::isdigit((unsigned char)upper[p])) {
        val = val * 10 + (upper[p] - '0');
        p++;
    }
    if (val < 1 || val > 7) return false;
    if (frame_no) *frame_no = val;
    if (end_pos) *end_pos = p;
    return true;
}

static std::string WorldUpperName(const std::string &name)
{
    std::string upper;
    upper.reserve(name.size());
    for (char c : name)
        upper.push_back((char)std::toupper((unsigned char)c));
    return upper;
}

bool WorldDecapBodyFrameNo(const std::string &name, int *frame_no, std::string *prefix)
{
    std::string upper = WorldUpperName(name);
    if (upper.find("DECAPHEAD") != std::string::npos ||
        upper.find("DECAPLEG") != std::string::npos ||
        upper.find("DECAPTORSO") != std::string::npos)
        return false;

    size_t pos = upper.rfind("DECAP");
    if (pos == std::string::npos) return false;
    size_t p = pos + 5;
    int val = 0;
    if (!WorldReadDecapFrameNo(upper, p, &val, &p)) return false;
    if (p != upper.size()) return false;

    if (frame_no) *frame_no = val;
    if (prefix) *prefix = upper.substr(0, pos);
    return true;
}

bool WorldDecapBodyPieceInfo(const std::string &name, int *frame_no,
                             std::string *prefix, int *kind)
{
    std::string upper = WorldUpperName(name);
    size_t pos = upper.rfind("DECAPLEG");
    int piece_kind = 0; /* leg before torso */
    size_t token_len = 8;
    if (pos == std::string::npos) {
        pos = upper.rfind("DECAPTORSO");
        piece_kind = 1;
        token_len = 10;
    }
    if (pos == std::string::npos) return false;

    size_t p = pos + token_len;
    int val = 0;
    if (!WorldReadDecapFrameNo(upper, p, &val, &p)) return false;

    if (frame_no) *frame_no = val;
    if (prefix) *prefix = upper.substr(0, pos);
    if (kind) *kind = piece_kind;
    return true;
}

bool WorldDecapPrefixFromName(const std::string &name, std::string *prefix)
{
    int frame_no = 0;
    if (WorldDecapBodyFrameNo(name, &frame_no, prefix)) return true;
    int kind = 0;
    if (WorldDecapBodyPieceInfo(name, &frame_no, prefix, &kind)) return true;
    return false;
}

int WorldDummyDecapFrameCount(void)
{
    return (int)(sizeof(kWorldDummyDecapOrder) / sizeof(kWorldDummyDecapOrder[0]));
}

int WorldDummyDecapFrameNo(int index)
{
    int n = WorldDummyDecapFrameCount();
    if (index < 0 || index >= n) return 1;
    return kWorldDummyDecapOrder[index];
}

void WorldResetDummyDecapDelays(WorldMarkedSequenceState &state, int frame_count)
{
    if (frame_count < 0) frame_count = 0;
    std::vector<int> &delays = state.frame_delays[kWorldDummyDecapSlot];
    delays.assign((size_t)frame_count, 1);
    state.local_dx[kWorldDummyDecapSlot].assign((size_t)frame_count, 0);
    state.local_dy[kWorldDummyDecapSlot].assign((size_t)frame_count, 0);
    state.visible_from[kWorldDummyDecapSlot].assign((size_t)frame_count, 0);
    int n = (int)(sizeof(kWorldDummyDecapDefaultDelays) /
                  sizeof(kWorldDummyDecapDefaultDelays[0]));
    if (frame_count < n) n = frame_count;
    for (int i = 0; i < n; i++)
        delays[i] = ClampTimelineHold(kWorldDummyDecapDefaultDelays[i]);
    state.dummy_decap_reset = false;
}

void WorldCollectMarkedFrames(Document *doc, std::vector<int> &out)
{
    out.clear();
    if (!doc) return;
    int idx = 0;
    for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if ((img->flags & 1) && img->data_p && img->w > 0 && img->h > 0)
            out.push_back(idx);
    }
}

struct WorldDecapCandidate {
    Document *doc;
    int doc_idx;
    std::string prefix;
    int frame_idx[8];
    std::vector<int> pieces[8];
    bool has_leg[8];
    bool has_torso[8];
    int wrapper_count;
    int piece_frame_count;
};

static bool WorldDecapCandidateComplete(const WorldDecapCandidate &cand)
{
    if (cand.wrapper_count >= 7) return true;
    for (int frame_no = 1; frame_no <= 7; frame_no++) {
        if (!cand.has_leg[frame_no] || !cand.has_torso[frame_no])
            return false;
    }
    return true;
}

static int WorldDecapCandidateScore(const WorldDecapCandidate &cand)
{
    int piece_frames = 0;
    for (int frame_no = 1; frame_no <= 7; frame_no++)
        if (cand.has_leg[frame_no] && cand.has_torso[frame_no])
            piece_frames++;
    return cand.wrapper_count * 10 + piece_frames;
}

WorldMarkedLane WorldBuildDummyDecapLane(WorldMarkedSequenceState &state,
                                         int active_doc_idx)
{
    std::vector<WorldDecapCandidate> candidates;
    for (int doc_idx = 0; doc_idx < document_tab_count(); doc_idx++) {
        Document *doc = document_get(doc_idx);
        if (!doc) continue;
        for (unsigned int img_idx = 0; img_idx < doc->imgcnt; img_idx++) {
            IMG *img = doc_get_img(doc, (int)img_idx);
            int frame_no = 0;
            std::string prefix;
            int piece_kind = -1;
            bool is_wrapper = WorldDecapBodyFrameNo(img_name_string(img),
                                                    &frame_no, &prefix);
            bool is_piece = !is_wrapper &&
                WorldDecapBodyPieceInfo(img_name_string(img),
                                        &frame_no, &prefix, &piece_kind);
            if (!is_wrapper && !is_piece)
                continue;

            int cand_idx = -1;
            for (int i = 0; i < (int)candidates.size(); i++) {
                if (candidates[i].doc_idx == doc_idx &&
                    candidates[i].prefix == prefix) {
                    cand_idx = i;
                    break;
                }
            }
            if (cand_idx < 0) {
                WorldDecapCandidate cand = {};
                cand.doc = doc;
                cand.doc_idx = doc_idx;
                cand.prefix = prefix;
                for (int i = 0; i < 8; i++) cand.frame_idx[i] = -1;
                for (int i = 0; i < 8; i++) {
                    cand.has_leg[i] = false;
                    cand.has_torso[i] = false;
                }
                cand.wrapper_count = 0;
                cand.piece_frame_count = 0;
                candidates.push_back(cand);
                cand_idx = (int)candidates.size() - 1;
            }
            WorldDecapCandidate &cand = candidates[cand_idx];
            if (is_wrapper && cand.frame_idx[frame_no] < 0) {
                candidates[cand_idx].frame_idx[frame_no] = (int)img_idx;
                candidates[cand_idx].wrapper_count++;
            } else if (is_piece) {
                bool had_frame = !cand.pieces[frame_no].empty();
                cand.pieces[frame_no].push_back((int)img_idx);
                if (piece_kind == 0) cand.has_leg[frame_no] = true;
                else cand.has_torso[frame_no] = true;
                if (!had_frame) cand.piece_frame_count++;
            }
        }
    }

    int best = -1;
    for (int i = 0; i < (int)candidates.size(); i++) {
        if (!WorldDecapCandidateComplete(candidates[i])) continue;
        if (state.dummy_decap_manual) {
            if (candidates[i].doc_idx != state.dummy_decap_doc_idx ||
                candidates[i].prefix != state.dummy_decap_prefix)
                continue;
            best = i;
            break;
        }
        if (best < 0)
            best = i;
        else if (candidates[i].doc_idx == active_doc_idx &&
                 candidates[best].doc_idx != active_doc_idx)
            best = i;
        else if (candidates[i].doc_idx == candidates[best].doc_idx &&
                 WorldDecapCandidateScore(candidates[i]) >
                 WorldDecapCandidateScore(candidates[best]))
            best = i;
    }

    WorldMarkedLane lane = {};
    lane.delay_slot = kWorldDummyDecapSlot;
    lane.frame_pos = 0;
    lane.img = NULL;
    lane.dummy_decap = true;
    lane.label = "Dummy Decap Body";
    lane.asm_label_part = "dummy_decap_body";
    if (best < 0) {
        lane.doc = NULL;
        lane.doc_idx = -1;
        return lane;
    }

    WorldDecapCandidate &cand = candidates[best];
    lane.doc = cand.doc;
    lane.doc_idx = cand.doc_idx;
    lane.asm_label_part = "dummy_decap_body_" +
                          WorldMarkedAsmLabelPart(cand.prefix.c_str(),
                                                  kWorldDummyDecapSlot);
    char label_buf[96];
    snprintf(label_buf, sizeof(label_buf), "Dummy Decap Body [%s]",
             cand.prefix.c_str());
    lane.label = label_buf;

    int n = WorldDummyDecapFrameCount();
    lane.frames.reserve((size_t)n);
    lane.frame_pieces.reserve((size_t)n);
    lane.frame_labels.reserve((size_t)n);
    for (int i = 0; i < n; i++) {
        int frame_no = WorldDummyDecapFrameNo(i);
        char frame_label[96];
        snprintf(frame_label, sizeof(frame_label), "%sDECAP%d",
                 cand.prefix.c_str(), frame_no);
        lane.frame_labels.push_back(frame_label);

        std::vector<int> pieces;
        if (cand.frame_idx[frame_no] >= 0) {
            pieces.push_back(cand.frame_idx[frame_no]);
        } else {
            pieces = cand.pieces[frame_no];
            std::sort(pieces.begin(), pieces.end(), [&](int a, int b) {
                IMG *ia = doc_get_img(cand.doc, a);
                IMG *ib = doc_get_img(cand.doc, b);
                int fa = 0, fb = 0, ka = 0, kb = 0;
                std::string pa, pb;
                WorldDecapBodyPieceInfo(img_name_string(ia), &fa, &pa, &ka);
                WorldDecapBodyPieceInfo(img_name_string(ib), &fb, &pb, &kb);
                if (ka != kb) return ka < kb;
                return img_name_string(ia) < img_name_string(ib);
            });
        }
        int representative = pieces.empty() ? -1 : pieces[0];
        lane.frames.push_back(representative);
        lane.frame_pieces.push_back(pieces);
    }

    if (state.dummy_decap_reset ||
        state.dummy_decap_doc_idx != cand.doc_idx ||
        state.dummy_decap_prefix != cand.prefix ||
        (int)state.frame_delays[kWorldDummyDecapSlot].size() !=
        (int)lane.frames.size()) {
        state.dummy_decap_doc_idx = cand.doc_idx;
        state.dummy_decap_prefix = cand.prefix;
        WorldResetDummyDecapDelays(state, (int)lane.frames.size());
    }
    EnsureWorldMarkedFrameDelays(state, kWorldDummyDecapSlot,
                                 (int)lane.frames.size());
    return lane;
}

bool WorldAppendMarkedSourceLane(WorldMarkedSequenceState &state, int doc_idx,
                                 std::vector<WorldMarkedLane> &lanes)
{
    if ((int)lanes.size() >= kWorldMarkedSourceTabs) return false;
    Document *doc = document_get(doc_idx);
    if (!doc) return false;

    WorldMarkedLane lane = {};
    lane.doc = doc;
    lane.doc_idx = doc_idx;
    lane.delay_slot = (int)lanes.size();
    lane.frame_pos = 0;
    lane.img = NULL;
    lane.dummy_decap = false;
    WorldCollectMarkedFrames(doc, lane.frames);
    if (lane.frames.empty()) return false;

    WorldMarkedBuildSingleFrameLane(doc, lane.frames,
                                    lane.frame_pieces, lane.frame_labels);
    WorldMarkedSyncSequenceOverride(state, lane.delay_slot, doc, doc_idx,
                                    lane.frames, lane.frame_pieces,
                                    lane.frame_labels);
    lanes.push_back(lane);
    return true;
}

bool WorldUpdateMarkedLanePlayback(WorldMarkedSequenceState &state,
                                   std::vector<WorldMarkedLane> &lanes,
                                   float delta_time)
{
    if (state.fps < 1.0f) state.fps = 1.0f;
    if (state.fps > 60.0f) state.fps = 60.0f;
    if (!state.paused)
        state.timer += delta_time;
    float step = 1.0f / state.fps;
    while (state.timer >= step) {
        state.timer -= step;
        state.frame++;
    }

    bool have_image = false;
    for (int slot = 0; slot < (int)lanes.size(); slot++) {
        WorldMarkedLane &lane = lanes[slot];
        int n = (int)lane.frames.size();
        if (n <= 0) continue;
        lane.frame_pos = WorldMarkedFrameForTick(state, lane.delay_slot, n,
                                                 state.frame,
                                                 state.hold_end[lane.delay_slot]);
        Document *fdoc = (lane.frame_pos < (int)lane.frame_docs.size() &&
                          lane.frame_docs[lane.frame_pos])
                       ? lane.frame_docs[lane.frame_pos] : lane.doc;
        lane.img = doc_get_img(fdoc, lane.frames[lane.frame_pos]);
        if (lane.img) have_image = true;
    }
    return have_image;
}

std::string WorldMarkedAsmToken(const std::string &raw, const char *fallback)
{
    std::string out;
    out.reserve(raw.size() + 8);
    for (char c : raw) {
        unsigned char uc = (unsigned char)c;
        if (std::isalnum(uc) || c == '_' || c == '+' || c == '-' || c == '*' ||
            c == '(' || c == ')')
            out.push_back(c);
    }
    if (out.empty() && fallback) out = fallback;
    if (!out.empty() && std::isdigit((unsigned char)out[0]))
        out.insert(out.begin(), '_');
    return out;
}

std::string WorldMarkedAsmLabelPart(const char *raw, int slot)
{
    std::string out;
    if (raw) {
        for (size_t i = 0; raw[i] && i < 64; i++) {
            char c = raw[i];
            if (c == '.') break;
            unsigned char uc = (unsigned char)c;
            if (std::isalnum(uc))
                out.push_back((char)std::tolower(uc));
            else if (c == '_')
                out.push_back('_');
        }
    }
    if (out.empty()) {
        char fallback[24];
        snprintf(fallback, sizeof(fallback), "tab%d", slot + 1);
        out = fallback;
    }
    if (std::isdigit((unsigned char)out[0]))
        out.insert(out.begin(), '_');
    return out;
}

int ClampWorldMarkedAniptDelta(int value)
{
    if (value < -32768) return -32768;
    if (value >  32767) return  32767;
    return value;
}

int ClampWorldMarkedVisibleFrom(int value)
{
    if (value < 0) return 0;
    if (value > 99999) return 99999;
    return value;
}

void WorldMarkedRestart(WorldMarkedSequenceState &state)
{
    state.timer = 0.0f;
    state.frame = 0;
}

void StepWorldMarkedSequence(WorldMarkedSequenceState &state, int delta)
{
    state.paused = true;
    state.timer = 0.0f;
    state.frame += delta;
    if (state.frame < 0)
        state.frame = 0;
}

void EnsureWorldMarkedFrameDelays(WorldMarkedSequenceState &state, int slot, int frame_count)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    if (frame_count < 0) frame_count = 0;

    std::vector<int> &delays = state.frame_delays[slot];
    if ((int)delays.size() < frame_count)
        delays.resize((size_t)frame_count, 1);
    else if ((int)delays.size() > frame_count)
        delays.resize((size_t)frame_count);
    for (int &delay : delays)
        delay = ClampTimelineHold(delay);

    std::vector<int> &local_dx = state.local_dx[slot];
    std::vector<int> &local_dy = state.local_dy[slot];
    std::vector<int> &visible_from = state.visible_from[slot];
    if ((int)local_dx.size() < frame_count)
        local_dx.resize((size_t)frame_count, 0);
    else if ((int)local_dx.size() > frame_count)
        local_dx.resize((size_t)frame_count);
    if ((int)local_dy.size() < frame_count)
        local_dy.resize((size_t)frame_count, 0);
    else if ((int)local_dy.size() > frame_count)
        local_dy.resize((size_t)frame_count);
    if ((int)visible_from.size() < frame_count)
        visible_from.resize((size_t)frame_count, 0);
    else if ((int)visible_from.size() > frame_count)
        visible_from.resize((size_t)frame_count);
    for (int &dx : local_dx)
        dx = ClampWorldMarkedAniptDelta(dx);
    for (int &dy : local_dy)
        dy = ClampWorldMarkedAniptDelta(dy);
    for (int &show_tick : visible_from)
        show_tick = ClampWorldMarkedVisibleFrom(show_tick);

    std::vector<int> &fmir = state.frame_mirror[slot];
    if ((int)fmir.size() < frame_count)
        fmir.resize((size_t)frame_count, 0);
    else if ((int)fmir.size() > frame_count)
        fmir.resize((size_t)frame_count);
}

int WorldMarkedTickForFrame(WorldMarkedSequenceState &state, int slot,
                            int frame_count, int frame_idx)
{
    EnsureWorldMarkedFrameDelays(state, slot, frame_count);
    if (slot < 0 || slot >= kWorldMarkedMaxTabs || frame_count <= 0) return 0;
    if (frame_idx < 0) frame_idx = 0;
    if (frame_idx >= frame_count) frame_idx = frame_count - 1;
    int tick = 0;
    for (int i = 0; i < frame_idx; i++)
        tick += ClampTimelineHold(state.frame_delays[slot][i]);
    return tick;
}

int WorldMarkedSequenceTicks(WorldMarkedSequenceState &state, int slot, int frame_count)
{
    EnsureWorldMarkedFrameDelays(state, slot, frame_count);
    if (slot < 0 || slot >= kWorldMarkedMaxTabs || frame_count <= 0) return 1;
    int ticks = 0;
    for (int i = 0; i < frame_count; i++)
        ticks += ClampTimelineHold(state.frame_delays[slot][i]);
    return ticks > 0 ? ticks : 1;
}

int WorldMarkedFrameForTick(WorldMarkedSequenceState &state, int slot,
                            int frame_count, int tick, bool hold_final)
{
    EnsureWorldMarkedFrameDelays(state, slot, frame_count);
    if (slot < 0 || slot >= kWorldMarkedMaxTabs || frame_count <= 0) return 0;
    int cycle_ticks = WorldMarkedSequenceTicks(state, slot, frame_count);
    if (hold_final && tick >= cycle_ticks) return frame_count - 1;

    int t = tick % cycle_ticks;
    if (t < 0) t += cycle_ticks;
    for (int i = 0; i < frame_count; i++) {
        int delay = ClampTimelineHold(state.frame_delays[slot][i]);
        if (t < delay) return i;
        t -= delay;
    }
    return frame_count - 1;
}

void WorldMarkedClearSequenceState(WorldMarkedSequenceState &state, int slot)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    state.frame_delays[slot].clear();
    state.local_dx[slot].clear();
    state.local_dy[slot].clear();
    state.visible_from[slot].clear();
    state.frame_mirror[slot].clear();
}

void WorldMarkedBuildSingleFrameLane(Document *doc, const std::vector<int> &frames,
                                     std::vector<std::vector<int>> &frame_pieces,
                                     std::vector<std::string> &frame_labels)
{
    frame_pieces.clear();
    frame_labels.clear();
    frame_pieces.reserve(frames.size());
    frame_labels.reserve(frames.size());
    for (int idx : frames) {
        frame_pieces.push_back(std::vector<int>(1, idx));
        frame_labels.push_back(img_name_string(doc_get_img(doc, idx)));
    }
}

void WorldMarkedSyncSequenceOverride(WorldMarkedSequenceState &state, int slot,
                                     Document *doc, int doc_idx,
                                     std::vector<int> &frames,
                                     std::vector<std::vector<int>> &frame_pieces,
                                     std::vector<std::string> &frame_labels)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs || !doc)
        return;

    const std::vector<int> defaults = frames;
    bool doc_changed = state.sequence_doc[slot] != doc ||
                       state.sequence_doc_idx[slot] != doc_idx;
    bool have_seq = !state.sequence_frames[slot].empty();

    /* Does the persisted sequence still reference only frames the doc has? */
    bool stale_entry = false;
    if (!doc_changed && have_seq) {
        for (int idx : state.sequence_frames[slot]) {
            if (!doc_get_img(doc, idx)) { stale_entry = true; break; }
        }
    }
    bool defaults_changed = state.default_frames[slot] != defaults;

    if (doc_changed || !have_seq) {
        /* A different sprite/tab now occupies this slot (or there is nothing
           built yet): seed the sequence straight from the marked frames. */
        state.sequence_doc[slot] = doc;
        state.sequence_doc_idx[slot] = doc_idx;
        state.default_frames[slot] = defaults;
        state.sequence_frames[slot] = defaults;
        WorldMarkedClearSequenceState(state, slot);
        EnsureWorldMarkedFrameDelays(state, slot, (int)defaults.size());
    } else if (defaults_changed || stale_entry) {
        /* Same sprite, but the marked SET changed (the user marked another
           frame/sprite) or a referenced frame was deleted. Reconcile in place
           so the hand-built sequence survives: keep every still-marked entry
           in its current position with its per-entry edits, drop entries whose
           frame is gone or was unmarked, and append only the newly marked
           frames at the end. */
        EnsureWorldMarkedFrameDelays(state, slot,
            (int)state.sequence_frames[slot].size());

        std::vector<int> prev_defaults = state.default_frames[slot];
        std::vector<int> old_seq   = state.sequence_frames[slot];
        std::vector<int> old_delay = state.frame_delays[slot];
        std::vector<int> old_dx    = state.local_dx[slot];
        std::vector<int> old_dy    = state.local_dy[slot];
        std::vector<int> old_vis   = state.visible_from[slot];
        std::vector<int> old_mir   = state.frame_mirror[slot];

        std::vector<int> new_seq, new_delay, new_dx, new_dy, new_vis, new_mir;
        new_seq.reserve(old_seq.size() + defaults.size());
        for (size_t i = 0; i < old_seq.size(); i++) {
            int idx = old_seq[i];
            if (!doc_get_img(doc, idx)) continue;   /* frame deleted from doc */
            if (std::find(defaults.begin(), defaults.end(), idx) == defaults.end())
                continue;                            /* sprite was unmarked */
            new_seq.push_back(idx);
            new_delay.push_back(old_delay[i]);
            new_dx.push_back(old_dx[i]);
            new_dy.push_back(old_dy[i]);
            new_vis.push_back(old_vis[i]);
            new_mir.push_back(old_mir[i]);
        }
        for (int idx : defaults) {
            /* Only frames newly added to the marked set get appended; frames
               the user deliberately removed from the sequence (still marked)
               stay removed. */
            if (std::find(prev_defaults.begin(), prev_defaults.end(), idx) != prev_defaults.end())
                continue;
            if (std::find(new_seq.begin(), new_seq.end(), idx) != new_seq.end())
                continue;
            new_seq.push_back(idx);
            new_delay.push_back(1);
            new_dx.push_back(0);
            new_dy.push_back(0);
            new_vis.push_back(0);
            new_mir.push_back(0);
        }

        state.default_frames[slot] = defaults;
        state.sequence_frames[slot] = new_seq;
        state.frame_delays[slot]    = new_delay;
        state.local_dx[slot]        = new_dx;
        state.local_dy[slot]        = new_dy;
        state.visible_from[slot]    = new_vis;
        state.frame_mirror[slot]    = new_mir;
        EnsureWorldMarkedFrameDelays(state, slot, (int)new_seq.size());
    }

    frames = state.sequence_frames[slot];
    WorldMarkedBuildSingleFrameLane(doc, frames, frame_pieces, frame_labels);
    EnsureWorldMarkedFrameDelays(state, slot, (int)frames.size());
}

void WorldMarkedResetSequenceToDefaults(WorldMarkedSequenceState &state, int slot)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    state.sequence_frames[slot] = state.default_frames[slot];
    WorldMarkedClearSequenceState(state, slot);
    EnsureWorldMarkedFrameDelays(state, slot, (int)state.sequence_frames[slot].size());
    WorldMarkedRestart(state);
}

void WorldMarkedDuplicateSequenceEntry(WorldMarkedSequenceState &state, int slot, int frame_idx)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    std::vector<int> &frames = state.sequence_frames[slot];
    if (frame_idx < 0 || frame_idx >= (int)frames.size()) return;
    EnsureWorldMarkedFrameDelays(state, slot, (int)frames.size());

    int insert_at = frame_idx + 1;
    frames.insert(frames.begin() + insert_at, frames[frame_idx]);
    state.frame_delays[slot].insert(state.frame_delays[slot].begin() + insert_at,
                                    state.frame_delays[slot][frame_idx]);
    state.local_dx[slot].insert(state.local_dx[slot].begin() + insert_at,
                                state.local_dx[slot][frame_idx]);
    state.local_dy[slot].insert(state.local_dy[slot].begin() + insert_at,
                                state.local_dy[slot][frame_idx]);
    state.visible_from[slot].insert(state.visible_from[slot].begin() + insert_at,
                                    state.visible_from[slot][frame_idx]);
    state.paused = true;
    state.timer = 0.0f;
    state.frame = WorldMarkedTickForFrame(state, slot, (int)frames.size(), insert_at);
}

void WorldMarkedMoveSequenceEntry(WorldMarkedSequenceState &state, int slot, int frame_idx, int dir)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    std::vector<int> &frames = state.sequence_frames[slot];
    int n = (int)frames.size();
    int j = frame_idx + dir;
    if (frame_idx < 0 || frame_idx >= n || j < 0 || j >= n) return;
    EnsureWorldMarkedFrameDelays(state, slot, n);

    std::swap(frames[frame_idx], frames[j]);
    std::swap(state.frame_delays[slot][frame_idx], state.frame_delays[slot][j]);
    std::swap(state.local_dx[slot][frame_idx],     state.local_dx[slot][j]);
    std::swap(state.local_dy[slot][frame_idx],     state.local_dy[slot][j]);
    std::swap(state.visible_from[slot][frame_idx], state.visible_from[slot][j]);

    state.paused = true;
    state.timer = 0.0f;
    state.frame = WorldMarkedTickForFrame(state, slot, n, j);
}

void WorldMarkedDeleteSequenceEntry(WorldMarkedSequenceState &state, int slot, int frame_idx)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    std::vector<int> &frames = state.sequence_frames[slot];
    if ((int)frames.size() <= 1 || frame_idx < 0 || frame_idx >= (int)frames.size()) return;
    EnsureWorldMarkedFrameDelays(state, slot, (int)frames.size());

    frames.erase(frames.begin() + frame_idx);
    state.frame_delays[slot].erase(state.frame_delays[slot].begin() + frame_idx);
    state.local_dx[slot].erase(state.local_dx[slot].begin() + frame_idx);
    state.local_dy[slot].erase(state.local_dy[slot].begin() + frame_idx);
    state.visible_from[slot].erase(state.visible_from[slot].begin() + frame_idx);
    if (frame_idx >= (int)frames.size())
        frame_idx = (int)frames.size() - 1;
    state.paused = true;
    state.timer = 0.0f;
    state.frame = WorldMarkedTickForFrame(state, slot, (int)frames.size(), frame_idx);
}

static void rebuild_world_onion_texture(IMG *img, int image_idx)
{
    if (!img || !img->data_p || img->w <= 0 || img->h <= 0 || !g_imgui_renderer)
        return;

    if (s_world_onion_tex &&
        s_world_onion_tex_w == img->w &&
        s_world_onion_tex_h == img->h &&
        s_world_onion_idx == image_idx)
        return;

    if (s_world_onion_tex) SDL_DestroyTexture(s_world_onion_tex);
    s_world_onion_tex = SDL_CreateTexture(g_imgui_renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        img->w, img->h);
    if (!s_world_onion_tex) {
        s_world_onion_tex_w = 0;
        s_world_onion_tex_h = 0;
        s_world_onion_idx = -1;
        return;
    }

    SDL_SetTextureBlendMode(s_world_onion_tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(s_world_onion_tex, SDL_ScaleModeNearest);
    s_world_onion_tex_w = img->w;
    s_world_onion_tex_h = img->h;
    s_world_onion_idx = image_idx;

    void *pix;
    int pitch;
    if (SDL_LockTexture(s_world_onion_tex, NULL, &pix, &pitch) != 0)
        return;

    int stride = (img->w + 3) & ~3;
    const unsigned char *src = (const unsigned char *)img->data_p;
    Uint32 *dst = (Uint32 *)pix;
    for (int y = 0; y < img->h; y++) {
        for (int x = 0; x < img->w; x++) {
            unsigned char ci = src[y * stride + x];
            SDL_Color c = g_palette[ci];
            Uint32 a = (ci == 0) ? 0u : 90u;  /* faint */
            dst[y * (pitch / 4) + x] =
                (a << 24) | ((Uint32)c.r << 16) |
                ((Uint32)c.g << 8) | c.b;
        }
    }
    SDL_UnlockTexture(s_world_onion_tex);
}

bool DrawWorldViewSingleSprite(ImVec2 avail, ImVec2 img_pos, ImGuiIO &io,
                               IMG *img, SDL_Texture *img_texture,
                               int image_idx, int image_count,
                               int world_w, int world_h,
                               int world_origin_x, int world_origin_y,
                               bool onion_enabled, bool mirror_active)
{
    if (!img || !img_texture || world_w <= 0 || world_h <= 0)
        return false;

    WorldCanvasLayout layout =
        ComputeWorldCanvasLayout(avail, img_pos, world_w, world_h,
                                 world_origin_x, world_origin_y);
    float wscale = layout.scale;
    float ww = layout.width;
    float wh = layout.height;
    ImVec2 wpos = layout.pos;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(wpos, ImVec2(wpos.x + ww, wpos.y + wh),
                      IM_COL32(0, 0, 0, 255));

    float ox = layout.origin_x;
    float oy = layout.origin_y;
    dl->AddLine(ImVec2(ox - 8, oy), ImVec2(ox + 8, oy),
                IM_COL32(120, 120, 120, 255));
    dl->AddLine(ImVec2(ox, oy - 8), ImVec2(ox, oy + 8),
                IM_COL32(120, 120, 120, 255));

    /* Onion-skin: faintly draw the previous sprite. */
    if (onion_enabled && image_count > 1) {
        int prev_idx = (image_idx <= 0) ? image_count - 1 : image_idx - 1;
        IMG *prev_img = get_img(prev_idx);
        if (prev_img && prev_img->data_p && prev_img->w > 0 && prev_img->h > 0) {
            rebuild_world_onion_texture(prev_img, prev_idx);
            if (s_world_onion_tex) {
                float pw = prev_img->w * wscale;
                float ph = prev_img->h * wscale;
                int pax = (int)(short)prev_img->anix;
                float pleft = mirror_active
                    ? (ox - ((int)prev_img->w - pax) * wscale)
                    : (ox - pax * wscale);
                ImVec2 ppos(pleft, oy - (int)(short)prev_img->aniy * wscale);
                ImVec2 puv0 = mirror_active ? ImVec2(1, 0) : ImVec2(0, 0);
                ImVec2 puv1 = mirror_active ? ImVec2(0, 1) : ImVec2(1, 1);
                dl->AddImage((ImTextureID)(intptr_t)s_world_onion_tex,
                             ppos, ImVec2(ppos.x + pw, ppos.y + ph),
                             puv0, puv1);
            }
        }
    }

    int ax = (int)(short)img->anix;
    int ay = (int)(short)img->aniy;
    float spw = img->w * wscale;
    float sph = img->h * wscale;
    float sleft = mirror_active
        ? (ox - ((int)img->w - ax) * wscale)
        : (ox - ax * wscale);
    ImVec2 spos(sleft, oy - ay * wscale);
    ImVec2 suv0 = mirror_active ? ImVec2(1, 0) : ImVec2(0, 0);
    ImVec2 suv1 = mirror_active ? ImVec2(0, 1) : ImVec2(1, 1);

    dl->AddImage((ImTextureID)(intptr_t)img_texture,
                 spos, ImVec2(spos.x + spw, spos.y + sph), suv0, suv1);

    dl->AddCircle(ImVec2(ox, oy), 4.0f,
                  IM_COL32(255, 200, 0, 255), 0, 1.5f);

    if (!io.WantCaptureMouse) {
        bool over_world =
            io.MousePos.x >= wpos.x && io.MousePos.x < wpos.x + ww &&
            io.MousePos.y >= wpos.y && io.MousePos.y < wpos.y + wh;
        if (over_world &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
        {
            ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            int dx = (int)(d.x / wscale);
            int dy = (int)(d.y / wscale);
            if (dx != 0 || dy != 0) {
                int next_ax = (int)(short)img->anix + (mirror_active ? dx : -dx);
                int next_ay = (int)(short)img->aniy - dy;
                set_primary_anipoint_with_sequence(img, next_ax, next_ay);
            }
        }
    }

    char buf[96];
    snprintf(buf, sizeof(buf),
             "[%d] %s%s   anix=%d aniy=%d   world=%dx%d",
             image_idx, img->n_s,
             mirror_active ? " mirror" : "",
             ax, ay, world_w, world_h);
    dl->AddRectFilled(ImVec2(wpos.x, wpos.y),
                      ImVec2(wpos.x + 320, wpos.y + 18),
                      IM_COL32(0, 0, 0, 180));
    dl->AddText(ImVec2(wpos.x + 4, wpos.y + 2),
                IM_COL32(220, 220, 220, 255), buf);

    ImGui::Dummy(ImVec2(avail.x, avail.y));
    return true;
}

void ClearCanvasUiTextures(void)
{
    if (s_world_onion_tex) {
        SDL_DestroyTexture(s_world_onion_tex);
        s_world_onion_tex = NULL;
    }
    s_world_onion_tex_w = 0;
    s_world_onion_tex_h = 0;
    s_world_onion_idx = -1;
}
