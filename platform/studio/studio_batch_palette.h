#pragma once
#include "studio_document.h"
#include <array>
namespace studio {
struct BatchPaletteOptions {int target=-1,ppp=6;bool match=true;};
struct BatchPaletteRow {
    uint64_t sprite=0;bool enabled=true,changed=false,alternates=false,packing_risk=false;
    int source=-1,w=0,h=0;uint64_t index_changes=0,color_changes=0,unknown_colors=0;
    std::string name,error;std::array<unsigned char,sizeof(IMG)> fields{};
    std::vector<unsigned char> before,after;
    std::vector<unsigned short> source_words;
};
struct BatchPalettePlan {
    const Artwork *owner=nullptr;uint64_t generation=0;BatchPaletteOptions options;
    std::vector<unsigned short> target_words;std::vector<BatchPaletteRow> rows;
};
// Rows with incompatible indices / alternate palettes stay visible for exclusion.
bool plan_batch_palette(const Artwork &art,const std::vector<uint64_t> &sprites,const BatchPaletteOptions &options,BatchPalettePlan &out,std::string &error);
std::vector<unsigned char> render_batch_palette(const BatchPalettePlan &plan,int row,bool after);
}
