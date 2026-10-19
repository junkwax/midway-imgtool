#pragma once
#include "studio_palette.h"
#include "studio_selection.h"
#include <utility>
namespace studio {
struct RegionMatchSample {std::array<bool,256> colors{};std::vector<std::pair<int,int>> seeds;int area=0,w=0,h=0;double cx=0,cy=0;};
RegionMatchSample sample_feature_region(const PaletteCompactionSprite &source,const PixelSelection &selection,int excluded=0);
std::vector<unsigned char> match_feature_region(const PaletteCompactionSprite &source,const RegionMatchSample &sample,bool original,const PixelSelection &selection,uint64_t &work);
}
