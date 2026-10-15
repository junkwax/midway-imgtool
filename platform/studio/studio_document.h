#pragma once
#include "img_format.h"
#include "load2_verify.h"
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>

namespace studio {
struct EmbeddedEntryValues;
struct PointTableEdit;
struct PaletteMergePlan;
struct PaletteCompactionPlan;
struct PaletteCleanupPlan;
struct ImportBatch;
struct PixelSelection;
struct RegionPlan;
struct SelectionTransform;
struct PaintBrush;
struct PaintLineReview;
struct CloneStroke;
struct FilterStroke;
struct RepairStroke;
struct BatchGeometryPlan;
struct BatchRenamePlan;
struct BatchPalettePlan;
struct BatchDeletePlan;
struct LibraryCopyPlan;
struct AnimationCopyPlan;
struct RestorePlan;
struct VariantSource;
struct VariantPlan;
struct Clipboard {
    int w = 0, h = 0;
    std::vector<unsigned char> pixels;
    std::vector<unsigned short> colors;
    std::vector<unsigned char> mask; // Optional coverage; distinct from transparent index zero.
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
    // Save and Save As always retain the source IMG version and record layout.
    bool save(const std::string &path, std::string &error);
    unsigned disk_version() const { return source_version; }
    bool disk_matches() const;
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
    void paint(int index, int x, int y, unsigned char color, int radius = 1,const PixelSelection *selection=nullptr);
    void fill(int index, int x, int y, unsigned char color,const PixelSelection *selection=nullptr);
    void anchor(int index, int x, int y);
    bool apply_point_table(const PointTableEdit &edit,std::string &error);
    void color(int palette_index, int slot, unsigned short value);
    Clipboard copy(int index, int x, int y, int w, int h) const;
    void paste(int index, const Clipboard &clip, int x, int y);
    void clear(int index, int x, int y, int w, int h);
    Clipboard copy_selection(int index,const PixelSelection &selection)const;
    bool clear_selection(int index,const PixelSelection &selection,std::string &error);
    bool move_selection(int index,const PixelSelection &selection,int dx,int dy,std::string &error);
    int extract_regions(const RegionPlan &plan,std::string &error);
    bool apply_selection_transform(const SelectionTransform &transform,std::string &error);
    int paint_stroke(int index,int x0,int y0,int x1,int y1,const PaintBrush &brush,const PixelSelection *selection,std::string &error);
    int fill_matching(int index,int x,int y,int color,bool match_colors,int tolerance,bool contiguous,const PixelSelection *selection,std::string &error);
    int filter_stroke(FilterStroke &stroke,int x,int y,std::string &error);
    int repair_stroke(RepairStroke &stroke,int x,int y,bool all_edges,std::string &error);
    int apply_batch_geometry(const BatchGeometryPlan &plan,std::string &error);
    int apply_batch_rename(const BatchRenamePlan &plan,std::string &error);
    int apply_batch_palette(const BatchPalettePlan &plan,bool allow_approximation,std::string &error);
    int apply_batch_delete(const BatchDeletePlan &plan,std::string &error);
    int apply_animation_copy(const Artwork &source,const AnimationCopyPlan &plan,std::string &error);
    int apply_library_copy(const Artwork &source,const LibraryCopyPlan &plan,std::string &error);
    // A split inherits the source layout; these APIs never convert an IMG.
    std::unique_ptr<Artwork> split_target() const;
    std::unique_ptr<Artwork> prepare_library_split(const LibraryCopyPlan &plan,std::string &error) const;
    std::unique_ptr<Artwork> prepare_animation_split(const AnimationCopyPlan &plan,std::string &error) const;
    int remove_split_source(const LibraryCopyPlan &copy,const BatchDeletePlan &removal,std::string &error);
    int apply_restore(const RestorePlan &plan,std::string &error);
    int smart_erase(int index,int x,int y,bool match_colors,int tolerance,bool contiguous,bool cleanup,const PixelSelection &selection,std::string &error);
    int clone_stroke(CloneStroke &stroke,int x0,int y0,int x1,int y1,std::string &error);
    bool apply_variant(const VariantSource &source,const std::vector<unsigned char> &coverage,const VariantPlan &reviewed,std::string &error);
    bool apply_paint_line(const PaintLineReview &line,std::string &error);
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
    int duplicate_palette(int index, std::string &error, int assign_sprite = -1,const std::string &name = {});
    bool rename_palette(int index,const std::string &name,std::string &error);
    int add_palette(const std::string &name,const std::vector<unsigned short> &words,std::string &error);
    bool delete_unused_palette(int index,std::string &error);
    int apply_palette_cleanup(const PaletteCleanupPlan &plan,std::string &error);
    bool replace_palette_colors(int index,const std::vector<unsigned short> &expected,const std::vector<unsigned short> &words,std::string &error);
    bool merge_palette(const PaletteMergePlan &plan,bool allow_approximation,std::string &error);
    bool compact_palette(const PaletteCompactionPlan &plan,std::string &error);
    int import_batch(const ImportBatch &batch,std::string &error);
    int import_png(const std::string &path, int match_palette, std::string &error);
    bool export_png(int index, const std::string &path, std::string &error) const;
    bool embedded_edit_entry(int record,int entry,const EmbeddedEntryValues &values,std::string &error);
    bool embedded_append(int record,int target,std::string &error);
    bool embedded_duplicate(int record,int entry,std::string &error);
    bool embedded_delete(int record,int entry,std::string &error);
    bool embedded_move(int record,int entry,int destination,std::string &error);
    int embedded_add_record(bool script,const std::string &name,std::string &error);
    bool embedded_rename(int record,const std::string &name,std::string &error);
    L2Report check(int ppp);
    Document data{}; // Owned codec data; UI reads through sprite()/palette().
private:
    friend class SpriteClipboard;
    friend class AnimationClipboard;
    int validate_animation_copy(const Artwork &source,const AnimationCopyPlan &plan,std::string &error) const;
    std::unique_ptr<Artwork> prepare_animation_snapshot(const AnimationCopyPlan &plan,std::string &error) const;
    int validate_library_copy(const Artwork &source,const LibraryCopyPlan &plan,std::string &error) const;
    void append_staged_library(Artwork &staged,int added,bool select_first);
    bool install_animation_with_artwork(const Artwork &source,const LibraryCopyPlan &artwork,const std::vector<unsigned char> &blob,unsigned sequences,unsigned scripts,std::string &error);
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
        unsigned seqcnt=0,scrcnt=0;
        uint64_t revision = 0;
        uint64_t selection = 0;
        std::string label;
        size_t bytes() const;
        bool same(const State &other) const;
    };
    State capture() const;
    void restore(const State &state);
    void trim_history();
    void assign_initial_ids();
    bool remap_sequence_indices(const std::vector<int> &mapping, std::vector<unsigned char> &blob, std::string &error) const;
    std::string unique_sprite_name(const std::string &stem) const;
    bool install_embedded(const std::vector<unsigned char> &blob,unsigned sequences,unsigned scripts,const std::string &label,std::string &error);
    std::unique_ptr<State> pending;
    std::vector<State> past, future;
    uint64_t revision = 0, saved_revision = 0, next_revision = 1, serial = 1, structure_serial = 1, next_sprite_id = 1;
    std::vector<uint64_t> sprite_ids;
    uint64_t selected_sprite_id = 0;
    unsigned source_version=0x634;
    LIB_HDR source_header{};
    std::unordered_map<uint64_t,unsigned short> legacy_reserved_alternates;
    std::string file_path;
    std::vector<unsigned char> disk_baseline;
};
}
