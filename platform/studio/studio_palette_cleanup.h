#pragma once
#include "studio_document.h"
#include <array>
#include <set>
namespace studio {
struct PaletteCleanupRow {
    int index=0,target=-1,primary=0,alternate=0;
    bool remove=false;std::string name;
    std::array<unsigned char,sizeof(PAL)> fields{};
    std::vector<unsigned short> words;
};
struct PaletteCleanupSprite {
    uint64_t id=0;std::array<unsigned char,sizeof(IMG)> fields{};
    std::vector<unsigned char> alternate;
};
struct PaletteCleanupPlan {
    const Artwork *owner=nullptr;uint64_t generation=0;
    std::set<int> keep;
    std::vector<PaletteCleanupRow> rows;
    std::vector<PaletteCleanupSprite> sprites;
    int removed=0,primary_remaps=0,alternate_remaps=0;size_t color_bytes=0;
};
// Usage covers all sprites and active alternate references. Defaults 0..2 and
// bytes at/after the alternate terminator are never treated as owned palettes.
bool plan_palette_cleanup(const Artwork &art,const std::set<int> &keep,PaletteCleanupPlan &out,std::string &error);
}
