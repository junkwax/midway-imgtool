#pragma once
#include "studio_palette.h"
#include "studio_selection.h"
#include "palette_math.h"
namespace studio {
struct PaletteIsolationOptions {uint64_t sprite=0;int ppp=6;PixelSelection selection;};
struct PaletteIsolationPlan {
    PaletteIsolationOptions options;
    PaletteCompactionPlan source;
    PaletteIsolatePlan allocation{};
    std::vector<unsigned short> words;
    std::vector<unsigned char> target_after;
    std::array<bool,256> reserved{},contested{};
    int target_row=-1,reserved_count=0,unresolved=0,depth=0,other_users=0;
    uint64_t inside=0,shifted=0,color_changes=0,other_pixels=0;
    bool ppp_changed=false;
    std::string blocker;
};
bool plan_palette_isolation(const Artwork &art,const PaletteIsolationOptions &options,PaletteIsolationPlan &out,std::string &error);
std::vector<unsigned char> render_palette_isolation(const Artwork &art,const PaletteIsolationPlan &plan,int row,bool after);
}
