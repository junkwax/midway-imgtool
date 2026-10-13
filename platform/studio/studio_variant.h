#pragma once
#include "studio_paint.h"
#include <array>
namespace studio {
struct VariantSource {
    const Artwork *owner=nullptr;uint64_t generation=0,sprite=0;
    Clipboard original;PixelSelection selection;
    int palette=0,color=1,ppp=6;
    std::vector<std::vector<unsigned short>> palettes;
    std::vector<int> depths;std::array<bool,256> used{};
};
struct VariantPlan {
    std::vector<unsigned char> pixels;
    std::vector<std::vector<unsigned short>> palettes;
    std::vector<int> depths,changed_palettes,grown_palettes,ppp_breaks;
    std::array<unsigned char,256> mapping{};
    int changed=0,transparent=0,new_slots=0,reused_slots=0;
};
bool capture_variant_source(const Artwork &art,int index,int color,int ppp,const PixelSelection &selection,VariantSource &out,std::string &error);
// Coverage is a tight, full-canvas binary mask; writes must stay in source.selection.
bool plan_variant(const VariantSource &source,const std::vector<unsigned char> &coverage,VariantPlan &out,std::string &error);
}
