#pragma once
#include "studio_palette.h"
namespace studio {
struct PaletteMoveOptions {int palette=-1;std::array<bool,256> slots{};};
struct PaletteMoveRow {int sprite=-1;bool primary=false,alternate=false;uint64_t shifted=0;std::string error;};
struct PaletteMovePlan {
    PaletteMoveOptions options;
    PaletteCompactionPlan preview;
    std::vector<PaletteMoveRow> rows;
    int selected=0,moved=0,blocked=0;
    uint64_t shifted=0;
};
bool plan_palette_move(const Artwork &art,const PaletteMoveOptions &options,PaletteMovePlan &out,std::string &error);
std::vector<unsigned char> render_palette_move(const Artwork &art,const PaletteMovePlan &plan,int row,bool after);
}
