#pragma once
#include "studio_document.h"
#include "anipoint_mirror.h"
#include <string>
#include <vector>

namespace studio {
constexpr double mk2_tick_hz = 54.7068;
constexpr int default_preview_hold = 4;

struct PreviewFrame {
    std::string name;
    int sprite = -1;                 // Full sprite, or first piece if no parent exists.
    std::vector<int> pieces;         // Children replace the full sprite; never double draw.
    uint64_t sprite_id=0;
};
struct PreviewSequence {
    std::string name;
    std::vector<PreviewFrame> frames;
};

// Name-based browsing only. Does not interpret or change embedded SEQSCR/ASM.
std::vector<PreviewSequence> infer_preview_sequences(const Artwork &art);
bool locate_preview_sprite(const std::vector<PreviewSequence> &sequences, int sprite,
                           int &sequence, int &frame);
std::vector<int> preview_pieces(const PreviewFrame &frame, bool multipart);

struct PreviewPlayer {
    int frame = 0;
    double elapsed_ticks = 0;
    bool playing = false, loop = true;
    void seek(int position, int count);
    void step(int delta, int count);
    void advance(double seconds, const std::vector<int> &holds, double tick_hz);
};

struct SpritePlacement { int x, y, w, h; };
SpritePlacement place_sprite(const IMG &sprite, int origin_x, int origin_y,
                             bool flip, MirrorConvention convention);

// Whole-frame motion is one artwork command, with the same signed/clamped
// anchor behavior as editing a single sprite. Pixel buffers are never touched.
void shift_frame_anchors(Artwork &art, const std::vector<int> &pieces, int dx, int dy);
}
