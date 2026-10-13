#pragma once
#include "studio_paint.h"
#include <array>
namespace studio {
enum class FilterKind {Blur,Smudge};
struct FilterBrush {FilterKind kind=FilterKind::Blur;int radius=1,strength=50;BrushShape shape=BrushShape::Round;};
struct FilterStroke {
    const Artwork *owner=nullptr;uint64_t generation=0,sprite=0;
    Clipboard original;PixelSelection selection;FilterBrush brush;
    std::vector<unsigned char> pixels,visited;
    std::vector<unsigned short> pairs;int pair_strength=-1;
    int last_x=0,last_y=0;bool started=false;
    std::array<unsigned char,32768> nearest{};
};
bool begin_filter_stroke(const Artwork &art,int index,int x,int y,const FilterBrush &brush,const PixelSelection &selection,FilterStroke &out,std::string &error);
// Pure incremental gesture. Blur touches each pixel once from the frozen source;
// smudge snapshots each stamp before writing and advances at pixel-sized steps.
int filter_stroke_pixels(FilterStroke &stroke,int x,int y,std::string &error);
}
