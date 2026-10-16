#pragma once
#include "studio_document.h"
#include <array>
namespace studio {
struct PaletteCopyEntry {PAL fields{};std::vector<unsigned short> words;};
// name/words also serve the single PAL/ACT exchange path. A palette clipboard
// additionally owns each selected palette and its metadata; it borrows no IMG.
struct PaletteTransfer {std::string name;std::vector<unsigned short> words;std::vector<PaletteCopyEntry> entries;};
struct PaletteUsage {int primary=0,alternate=0;};
struct PaletteMergeOptions {
    int source=-1,target=-1,ppp=6;
    bool grow=true,perceptual=true,remove_source=false;
};
struct PaletteMergePlan {
    const Artwork *owner=nullptr;
    uint64_t generation=0;
    PaletteMergeOptions options;
    std::vector<unsigned short> source_words,target_words,words;
    std::array<unsigned char,256> mapping{};
    std::array<uint64_t,256> usage{};
    std::vector<int> sprites;
    int added=0,approximate_colors=0,max_distance=0,ppp_images=0,skipped_sprites=0,alternate_refs=0,depth=0;
    uint64_t pixels=0,index_changes=0,color_changes=0;
};
bool plan_palette_merge(const Artwork &art,const PaletteMergeOptions &options,PaletteMergePlan &plan,std::string &error);
std::vector<unsigned char> render_palette_merge(const Artwork &art,const PaletteMergePlan &plan,int sprite,bool merged);
struct PaletteCompactionOptions {int palette=-1,ppp=6;bool pack_live=true,recompute_depth=false;};
struct PaletteCompactionPlan {
    const Artwork *owner=nullptr;uint64_t generation=0;
    PaletteCompactionOptions options;
    std::vector<unsigned short> original,words;
    std::array<unsigned char,256> mapping{};
    std::array<uint64_t,256> primary{},alternate{};
    std::vector<int> primary_sprites,preview_sprites;
    bool indices_locked=false,ppp_changed=false;
    int unused=0,removed=0,depth=0,alternate_sprites=0;
    uint64_t index_changes=0;
};
bool plan_palette_compaction(const Artwork &art,const PaletteCompactionOptions &options,PaletteCompactionPlan &plan,std::string &error);
std::vector<unsigned char> render_palette_compaction(const Artwork &art,const PaletteCompactionPlan &plan,int sprite,bool compacted);
std::string palette_name(const PAL &pal);
PaletteTransfer copy_palette(const Artwork &art,int index);
PaletteUsage palette_usage(const Artwork &art,int index);
std::vector<unsigned short> adjust_palette(const std::vector<unsigned short> &baseline,const std::array<bool,256> &selected,int hue,int saturation,int lightness);
enum class PaletteColorEffect {Invert,Reverse};
// Only explicitly selected opaque slots change. Reverse moves complete words;
// invert complements RGB555 while preserving the stored high bit.
std::vector<unsigned short> palette_color_effect(const std::vector<unsigned short> &words,const std::array<bool,256> &selected,PaletteColorEffect effect);
bool read_palette_file(const std::string &path,PaletteTransfer &out,std::string &error);
bool write_palette_file(const std::string &path,const PaletteTransfer &palette,bool act,std::string &error);
}
