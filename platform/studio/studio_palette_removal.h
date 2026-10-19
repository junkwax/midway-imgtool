#pragma once
#include "studio_palette.h"
namespace studio {
struct PaletteRemovalOptions {int palette=-1,ppp=6;std::array<bool,256> slots{};};
struct PaletteRemovalRow {
    int sprite=-1;bool primary=false,alternate=false;
    uint64_t cleared=0,shifted=0;
    std::string error;
};
struct PaletteRemovalPlan {
    PaletteRemovalOptions options;
    PaletteCompactionPlan preview;
    std::vector<PaletteRemovalRow> rows;
    uint64_t cleared=0,shifted=0;
    int blocked=0;
};
bool plan_palette_removal(const Artwork &art,const PaletteRemovalOptions &options,PaletteRemovalPlan &out,std::string &error);
}
