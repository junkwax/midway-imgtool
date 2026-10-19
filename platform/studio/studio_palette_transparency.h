#pragma once
#include "studio_batch_palette.h"
#include "studio_selection.h"
namespace studio {
struct PaletteTransparencyOptions {
    int palette=-1;
    std::array<bool,256> slots{};
    PixelSelection selection; // Valid only for a one-sprite scope.
    bool all_users=false;
};
struct PaletteTransparencyPlan {
    PaletteTransparencyOptions options;
    BatchPalettePlan preview;
    std::array<unsigned char,sizeof(PAL)> palette_fields{};
    std::vector<std::vector<unsigned char>> alternates,layers;
    std::array<uint64_t,256> usage{};
};
bool plan_palette_transparency(const Artwork &art,const std::vector<uint64_t> &ids,const PaletteTransparencyOptions &options,PaletteTransparencyPlan &out,std::string &error);
}
