#pragma once
#include "studio_animation.h"
namespace studio {
struct AnchorLinkOptions {
    int reference_x=0,reference_y=0,target_x=0,target_y=0;
    int offset_x=0,offset_y=0;
    bool reference_flip=false,target_flip=false;
    MirrorConvention convention=MirrorConvention_Ani2;
};
struct AnchorLinkPlan {
    const Artwork *reference=nullptr,*target=nullptr;
    uint64_t reference_generation=0,target_generation=0,reference_sprite=0,target_sprite=0;
    AnchorLinkOptions options;
    int old_x=0,old_y=0,x=0,y=0;
    bool changed=false;
};
bool plan_anchor_link(const Artwork &reference,uint64_t reference_sprite,const Artwork &target,uint64_t target_sprite,
                      const AnchorLinkOptions &options,AnchorLinkPlan &out,std::string &error);
bool apply_anchor_link(Artwork &target,const Artwork &reference,const AnchorLinkPlan &plan,std::string &error);
}
