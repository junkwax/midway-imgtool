#pragma once
#include "studio_region_match.h"
namespace studio {
struct RegionRemapOptions {uint64_t sprite=0;int destination=1;PixelSelection selection;};
struct RegionRemapFrame {int source_row=-1;uint64_t id=0;bool is_source=false;std::vector<unsigned char> region;uint64_t feature=0;};
struct RegionRemapSource {RegionRemapOptions options;PaletteCompactionPlan palette;std::vector<RegionRemapFrame> frames;};
struct RegionRemapResult {bool included=false;uint64_t remapped=0,color_changes=0;std::vector<unsigned char> after;std::string blocker;};
struct RegionRemapPlan {
    std::shared_ptr<const RegionRemapSource> source;
    std::vector<uint64_t> included;
    std::vector<RegionRemapResult> frames;
    int blocked=0,changed_frames=0,no_match=0;
    uint64_t remapped=0,color_changes=0;
};
bool capture_region_remap(const Artwork &art,const RegionRemapOptions &options,std::shared_ptr<const RegionRemapSource> &out,std::string &error);
bool plan_region_remap(std::shared_ptr<const RegionRemapSource> source,const std::vector<uint64_t> &included,RegionRemapPlan &out,std::string &error);
std::vector<unsigned char> render_region_remap(const Artwork &art,const RegionRemapPlan &plan,int frame,bool after);
}
