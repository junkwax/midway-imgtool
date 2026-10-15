#pragma once
#include "studio_document.h"
#include <array>
namespace studio {
using SpritePoints=std::array<int,20>; // Ten signed 16-bit X/Y pairs on disk.
struct PointTableEdit {
    const Artwork *owner=nullptr;uint64_t generation=0,sprite=0;
    bool existed=false,enabled=false;
    std::array<unsigned char,40> original{};
    SpritePoints points{};
};
bool read_point_table(const Artwork &art,uint64_t sprite,PointTableEdit &out,std::string &error);
bool apply_point_table(Artwork &art,const PointTableEdit &edit,std::string &error);
}
