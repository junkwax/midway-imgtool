#pragma once
#include "studio_subframes.h"
namespace studio {
inline constexpr std::array<int,13> dummy_body_order={1,2,3,4,3,4,3,4,3,4,5,6,7};
inline constexpr std::array<int,13> dummy_body_holds={48,6,6,10,10,10,10,10,10,6,6,6,6};
struct DummyBodyFamily {
    std::string prefix,error;
    std::array<std::vector<int>,7> pieces;
    std::array<bool,7> wrapper{};
    int score=0;
};
// Discovery follows classic's case-insensitive DECAP/DECAPLEG/DECAPTORSO
// names. It never borrows the classic workspace or changes source artwork.
bool discover_dummy_bodies(const Artwork &,std::vector<DummyBodyFamily> &,std::string &error);
struct DummyBodyOptions {
    uint64_t doc=0,replace_lane=0;
    std::string prefix;
    bool flip_x=true; // Only for a new actor; reset retains actor settings.
};
struct DummyBodyPlan {
    const SceneProject *owner=nullptr;DummyBodyOptions options;
    SceneState before,after;DummyBodyFamily family;
    std::vector<SubframeSpriteSnapshot> sprites;
    int lane_index=-1,start_tick=0,duration=0;
};
bool plan_dummy_body(const SceneProject &,const DummyBodyOptions &,const SceneSource &,DummyBodyPlan &,std::string &error);
}
