#pragma once
#include "studio_project.h"
namespace studio {
struct SubframeSpriteSnapshot {
    const Artwork *owner=nullptr;uint64_t id=0;int index=-1;
    std::array<unsigned char,sizeof(IMG)> fields{};
    std::array<unsigned char,sizeof(PAL)> palette{};
    std::vector<unsigned char> pixels,colors,alternate,layer;
};
struct SubframeSwapPlan {
    const SceneProject *owner=nullptr;
    uint64_t lane=0,frame=0;
    int tick=0,lane_index=-1,parent_index=-1,child_index=-1,global_tick=0;
    bool replaced=false;
    SceneState before,after;
    std::vector<SubframeSpriteSnapshot> sprites; // Parent followed by children in library order.
};
bool plan_subframe_swap(const SceneProject &,uint64_t lane,uint64_t frame,int actor_tick,const SceneSource &,SubframeSwapPlan &,std::string &error);
}
