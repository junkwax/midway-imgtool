#pragma once
#include "studio_palette.h"
#include "studio_selection.h"
namespace studio {
enum class PaletteZeroScope {CopyOnly,Current,Selection};
struct PaletteZeroOptions {
    int palette=-1,slot=-1,ppp=6; // -1 chooses a safe automatic destination.
    PaletteZeroScope scope=PaletteZeroScope::CopyOnly;
    uint64_t sprite=0;
    PixelSelection selection;
};
struct PaletteZeroRow {int sprite=-1;bool primary=false,alternate=false;uint64_t uses=0;std::string error;};
struct PaletteZeroPlan {
    PaletteZeroOptions options;
    PaletteCompactionPlan source;
    std::vector<unsigned short> words;
    std::vector<unsigned char> target_after;
    std::vector<PaletteZeroRow> rows;
    int slot=-1,depth=0,growth=0,blocked=0;
    uint64_t filled=0,recolored=0;
    bool changed=false,ppp_changed=false;
};
bool plan_palette_zero(const Artwork &art,const PaletteZeroOptions &options,PaletteZeroPlan &out,std::string &error);
std::vector<unsigned char> render_palette_zero(const Artwork &art,const PaletteZeroPlan &plan,int row,bool after);
}
