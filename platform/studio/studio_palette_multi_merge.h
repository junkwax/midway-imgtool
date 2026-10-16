#pragma once
#include "studio_palette.h"
namespace studio {
struct PaletteMultiMergeOptions {
    std::vector<int> sources;int target=-1,ppp=6;
    bool grow=true,perceptual=true,remove_unused=false;
};
struct PaletteMultiMergePalette {std::array<unsigned char,sizeof(PAL)> fields{};std::vector<unsigned short> words;};
struct PaletteMultiMergeImage {
    uint64_t id=0;int source=-1;
    std::array<unsigned char,sizeof(IMG)> fields{};
    std::vector<unsigned char> pixels,alternate;
};
struct PaletteMultiMergeSource {
    int palette=-1,skipped=0,alternate_refs=0;bool remove=false;
    std::array<unsigned char,256> mapping{};std::array<uint64_t,256> usage{};
    std::vector<int> sprites;
    uint64_t pixels=0,index_changes=0,color_changes=0;int approximate_colors=0,max_distance=0;
};
struct PaletteMultiMergePlan {
    const Artwork *owner=nullptr;uint64_t generation=0;unsigned version=0;
    PaletteMultiMergeOptions options;
    std::vector<PaletteMultiMergePalette> palettes;
    std::vector<PaletteMultiMergeImage> images;
    std::vector<PaletteMultiMergeSource> sources;
    std::vector<int> sprites,palette_mapping;
    std::vector<unsigned short> words;
    int target_after=-1,added=0,removed=0,depth=0,approximate_colors=0,ppp_images=0,skipped=0;
    uint64_t pixels=0,index_changes=0,color_changes=0;
};
// One final destination is built before any source mapping, so every preview
// uses the final combined colors. All inputs are owned; planning is read-only.
bool plan_palette_multi_merge(const Artwork &,const PaletteMultiMergeOptions &,PaletteMultiMergePlan &,std::string &error);
std::vector<unsigned char> render_palette_multi_merge(const Artwork &,const PaletteMultiMergePlan &,int sprite,bool merged);
}
