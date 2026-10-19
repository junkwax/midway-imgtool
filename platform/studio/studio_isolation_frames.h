#pragma once
#include "studio_palette_isolation.h"
namespace studio {
struct IsolationFrame {
    int source_row=-1;uint64_t id=0;bool is_source=false;
    std::vector<unsigned char> region;
    uint64_t feature=0,outside=0;
};
struct IsolationFramesSource {
    PaletteIsolationOptions options;
    PaletteCompactionPlan palette;
    std::array<bool,256> reserved{};
    std::vector<IsolationFrame> frames;
    int reserved_count=0;
};
struct IsolationFrameResult {
    bool included=false;uint64_t shifted=0,color_changes=0;
    std::vector<unsigned char> after;
    std::string blocker;
};
struct IsolationFramesPlan {
    std::shared_ptr<const IsolationFramesSource> source;
    std::vector<uint64_t> included;
    int ppp=6,depth=0,blocked=0,unresolved=0,no_match=0,omitted_users=0;
    uint64_t shifted=0,color_changes=0,omitted_pixels=0;
    bool ppp_changed=false;
    PaletteIsolatePlan allocation{};
    std::vector<unsigned short> words;
    std::vector<IsolationFrameResult> frames;
};
bool capture_isolation_frames(const Artwork &art,const PaletteIsolationOptions &options,std::shared_ptr<const IsolationFramesSource> &out,std::string &error);
bool plan_isolation_frames(std::shared_ptr<const IsolationFramesSource> source,const std::vector<uint64_t> &included,int ppp,IsolationFramesPlan &out,std::string &error);
std::vector<unsigned char> render_isolation_frames(const Artwork &art,const IsolationFramesPlan &plan,int frame,bool after);
}
