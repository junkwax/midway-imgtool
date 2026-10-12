#pragma once
#include "img_format.h"
#include "load2_verify.h"
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace studio {
struct Clipboard {
    int w = 0, h = 0;
    std::vector<unsigned char> pixels;
    std::vector<unsigned short> colors;
};

enum class Transform { FlipHorizontal, FlipVertical, RotateClockwise, RotateCounterclockwise, Rotate180 };
// Owns an IMG independently of the classic tab/undo system. Stable IDs survive
// insertion, ordering, and history restores; disk indices are serialization details.
class Artwork {
public:
    Artwork();
    ~Artwork();
    Artwork(const Artwork &) = delete;
    Artwork &operator=(const Artwork &) = delete;
    static std::unique_ptr<Artwork> open(const std::string &path, std::string &error);
    static std::unique_ptr<Artwork> demo();
    static std::unique_ptr<Artwork> create();
    bool save(const std::string &path, std::string &error);
    IMG *sprite(int index) const;
    PAL *palette(int index) const;
    int count() const { return (int)data.imgcnt; }
    int palette_count() const { return (int)data.palcnt; }
    const std::string &path() const { return file_path; }
    bool dirty() const { return revision != saved_revision; }
    uint64_t generation() const { return serial; }
    uint64_t structure_generation() const { return structure_serial; }
    uint64_t sprite_id(int index) const;
    int sprite_index(uint64_t id) const;
    uint64_t selection_id() const { return selected_sprite_id; }
    void select_sprite(uint64_t id) { if(sprite_index(id)>=0)selected_sprite_id=id; }
    void begin(const std::string &label);
    void commit();
    void cancel();
    void undo();
    void redo();
    bool can_undo() const { return !past.empty(); }
    bool can_redo() const { return !future.empty(); }
    const char *undo_label() const;
    void paint(int index, int x, int y, unsigned char color, int radius = 1);
    void fill(int index, int x, int y, unsigned char color);
    void anchor(int index, int x, int y);
    void color(int palette_index, int slot, unsigned short value);
    Clipboard copy(int index, int x, int y, int w, int h) const;
    void paste(int index, const Clipboard &clip, int x, int y);
    void clear(int index, int x, int y, int w, int h);
    bool rename_sprite(int index, const std::string &name, std::string &error);
    int add_sprite(const std::string &name, int w, int h, int palette_index, std::string &error);
    int duplicate_sprite(int index, std::string &error);
    bool delete_sprite(int index, std::string &error);
    bool move_sprite(int index, int destination, std::string &error);
    bool resize_sprite(int index, int w, int h, bool quality, bool scale_anchors, std::string &error);
    bool canvas_size(int index, int w, int h, int placement, std::string &error);
    bool trim_sprite(int index, std::string &error);
    bool transform_sprite(int index, Transform transform, bool transform_anchors, std::string &error);
    bool assign_palette(int index, int palette_index, bool match_colors, std::string &error);
    int duplicate_palette(int index, std::string &error, int assign_sprite = -1);
    int import_png(const std::string &path, int match_palette, std::string &error);
    bool export_png(int index, const std::string &path, std::string &error) const;
    L2Report check(int ppp);
    Document data{}; // Owned codec data; UI reads through sprite()/palette().
private:
    struct State {
        struct Sprite {
            IMG fields{};
            uint64_t id=0;
            std::vector<unsigned char> pixels, baseline, points, alternate;
        };
        struct Palette { PAL fields{}; std::vector<unsigned char> colors; };
        std::vector<Sprite> sprites;
        std::vector<Palette> palettes;
        std::vector<unsigned char> sequences;
        uint64_t revision = 0;
        uint64_t selection = 0;
        std::string label;
        size_t bytes() const;
        bool same(const State &other) const;
    };
    State capture() const;
    void restore(const State &state);
    void assign_initial_ids();
    bool remap_sequence_indices(const std::vector<int> &mapping, std::vector<unsigned char> &blob, std::string &error) const;
    std::string unique_sprite_name(const std::string &stem) const;
    std::unique_ptr<State> pending;
    std::vector<State> past, future;
    uint64_t revision = 0, saved_revision = 0, next_revision = 1, serial = 1, structure_serial = 1, next_sprite_id = 1;
    std::vector<uint64_t> sprite_ids;
    uint64_t selected_sprite_id = 0;
    std::string file_path;
    std::vector<unsigned char> disk_baseline;
};
}
