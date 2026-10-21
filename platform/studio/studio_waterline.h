#pragma once
#include "studio_subframes.h"
namespace studio {
struct WaterlineOptions {
    uint64_t lane=0,frame=0,fine_doc=0,fine_sprite=0;
    int y=200; // Absolute world Y; never an IMG pixel coordinate.
};
struct WaterlineRow {uint64_t sprite=0;std::string name;int hide_tick=0;}; // 0 = no reachable crossing.
struct WaterlinePlan {
    const SceneProject *owner=nullptr;WaterlineOptions options;
    SceneState before,after;
    std::vector<SubframeSpriteSnapshot> sprites;
    std::vector<WaterlineRow> rows;
    int lane_index=-1,parent_index=-1,child_index=-1,cut_tick=0,global_tick=0,fine_begin=0;
    bool replaced=false,down=true;
};
bool plan_waterline(const SceneProject &,const WaterlineOptions &,const SceneSource &,WaterlinePlan &,std::string &error);
}
