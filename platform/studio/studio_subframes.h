#pragma once
#include "studio_project.h"
namespace studio {
struct SubframeSpriteSnapshot {
    const Artwork *owner=nullptr;uint64_t id=0;int index=-1;
    std::array<unsigned char,sizeof(IMG)> fields{};
    std::array<unsigned char,sizeof(PAL)> palette{};
    std::vector<unsigned char> pixels,colors,alternate,layer;
};
bool capture_subframe_sprite(const Artwork &,int index,size_t &budget,SubframeSpriteSnapshot &,std::string &error);
bool capture_subframe_family(const Artwork &,int parent,size_t &budget,std::vector<SubframeSpriteSnapshot> &,std::string &error);
bool same_subframe_snapshot(const SubframeSpriteSnapshot &,const SubframeSpriteSnapshot &);
int subframe_motion_offset(int value,int velocity,int elapsed,int cap,bool flip);
size_t scene_review_bytes(const SceneState &);
}
