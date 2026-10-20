#pragma once
#include "studio_document.h"
#include "studio_selection.h"
namespace studio {
// A separate full-canvas IMG, not the runtime-only pasted overlay layer.
struct OverlayFramePlan {
    const Artwork *owner=nullptr;
    uint64_t generation=0,source=0;
    bool split=false;
    PixelSelection selection;
    std::string requested_name,name;
    int opaque=0;
    std::array<unsigned char,sizeof(IMG)> fields{};
    std::array<unsigned char,sizeof(PAL)> palette_fields{};
    std::vector<unsigned char> pixels,colors,alternate,points,baseline,remaining,child;
};
bool plan_overlay_frame(const Artwork &,int source,const PixelSelection &,bool split,const std::string &name,OverlayFramePlan &,std::string &error);
// part 0 = original, 1 = remaining source, 2 = new full-canvas frame.
std::vector<unsigned char> render_overlay_frame(const OverlayFramePlan &,int part);
}
