#pragma once
#include "studio_selection.h"
#include "studio_document.h"
#include <string>
#include <vector>
namespace studio {
struct RegionRequest {std::string label,name;PixelSelection selection;bool enabled=true;};
struct RegionChild {std::string name;int x=0,y=0,w=0,h=0,anix=0,aniy=0;std::vector<unsigned char> pixels;};
struct RegionPlan {
    const Artwork *owner=nullptr;uint64_t generation=0,source=0;
    bool trim=true,cut=false;
    std::vector<RegionRequest> requests;
    std::vector<RegionChild> children;
    int selected_pixels=0,opaque_pixels=0,overlap_pixels=0;
};
std::vector<RegionRequest> propose_body_regions(const IMG &image,bool &fallback);
bool build_region_plan(const Artwork &art,int source,const std::vector<RegionRequest> &requests,bool trim,bool cut,RegionPlan &out,std::string &error);
}
