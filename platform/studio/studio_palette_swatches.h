#pragma once
#include "studio_palette.h"
namespace studio {
struct PaletteSwatch {int index=0;unsigned short word=0;};
struct PaletteSwatchClipboard {std::string source;std::vector<PaletteSwatch> slots;};
enum class SwatchPaste {SameIndices,Append,AtSlot};
struct PaletteSwatchPlan {
    const Artwork *owner=nullptr;uint64_t generation=0;
    int palette=-1,slot=1,ppp=6,depth=0,changed=0,growth=0,skipped_zero=0,primary=0,alternate=0,ppp_images=0;
    SwatchPaste mode=SwatchPaste::SameIndices;
    PaletteSwatchClipboard clipboard;
    PAL fields{};
    std::vector<unsigned short> before,words;
    std::vector<int> destinations;
};
bool plan_palette_swatches(const Artwork &,int palette,const PaletteSwatchClipboard &,SwatchPaste,int slot,int ppp,PaletteSwatchPlan &,std::string &error);
}
