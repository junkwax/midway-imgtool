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
enum class PaletteColorOrder {Keep,DarkToLight,ColorFamilies};
struct PaletteCompactionOptions {int palette=-1,ppp=6;bool pack_live=true,recompute_depth=false;PaletteColorOrder order=PaletteColorOrder::Keep;bool copy_only=false;};
struct PaletteCompactionSprite {
    int index=-1;uint64_t id=0;
    std::array<unsigned char,sizeof(IMG)> fields{};
    std::vector<unsigned char> pixels,alternate,layer,preview_pixels;
};
struct PaletteCompactionPlan {
    const Artwork *owner=nullptr;uint64_t generation=0;
    PaletteCompactionOptions options;
    std::vector<unsigned short> original,words;
    std::array<unsigned char,sizeof(PAL)> palette_fields{};
    std::vector<PaletteCompactionSprite> source_sprites;
    std::array<unsigned char,256> mapping{};
    std::array<uint64_t,256> primary{},alternate{};
    std::vector<int> primary_sprites,preview_sprites;
    bool indices_locked=false,ppp_changed=false;
    int unused=0,removed=0,depth=0,alternate_sprites=0;
    uint64_t index_changes=0;
};
bool plan_palette_compaction(const Artwork &art,const PaletteCompactionOptions &options,PaletteCompactionPlan &plan,std::string &error);
std::vector<unsigned char> render_palette_compaction(const Artwork &art,const PaletteCompactionPlan &plan,int sprite,bool compacted);
struct PaletteInheritanceOptions {int target=-1,source=-1,ppp=6;bool perceptual=false;};
struct PaletteInheritancePlan {
    const Artwork *owner=nullptr;uint64_t generation=0;
    PaletteInheritanceOptions options;
    std::array<unsigned char,sizeof(PAL)> target_fields{},source_fields{};
    std::vector<unsigned short> original,words;
    std::vector<PaletteCompactionSprite> sprites;
    std::array<unsigned char,256> mapping{};
    std::array<uint64_t,256> usage{};
    uint64_t index_changes=0,color_changes=0;
    int approximate_colors=0;bool ppp_changed=false,changed=false;
};
bool plan_palette_inheritance(const Artwork &art,const PaletteInheritanceOptions &options,PaletteInheritancePlan &plan,std::string &error);
std::vector<unsigned char> render_palette_inheritance(const Artwork &art,const PaletteInheritancePlan &plan,int sprite,bool inherited);
enum class BloodLaneFill {RedRamp,Blank,KeepExisting};
struct PaletteReductionOptions {
    int palette=-1,depth=7,ppp=6;
    // Nonzero reserves the last N slots within the existing count/depth.
    int reserve_tail=0;
    BloodLaneFill blood_fill=BloodLaneFill::RedRamp;
};
struct PaletteReductionPlan {
    PaletteReductionOptions options;
    // Owned usage snapshot and remapped before/after rendering use the same
    // representation as exact palette organization; Apply has a distinct API.
    PaletteCompactionPlan preview;
    int active_colors=0,approximate_colors=0,max_distance=0;
    uint64_t color_changes=0;
    bool quantized=false,changed=false;
};
bool plan_palette_reduction(const Artwork &art,const PaletteReductionOptions &options,PaletteReductionPlan &plan,std::string &error);
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
