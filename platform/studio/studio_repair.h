#pragma once
#include "studio_paint.h"
namespace studio {
enum class RepairKind {Heal,Defringe};
struct RepairBrush {RepairKind kind=RepairKind::Heal;int radius=1,passes=16,edge_width=1;BrushShape shape=BrushShape::Round;};
struct RepairStroke {
    const Artwork *owner=nullptr;uint64_t generation=0,sprite=0;
    Clipboard original;PixelSelection selection;RepairBrush brush;
    std::vector<unsigned char> pixels,coverage;
    int last_x=0,last_y=0,unresolved=0;bool started=false;
};
bool begin_repair_stroke(const Artwork &art,int index,int x,int y,const RepairBrush &brush,const PixelSelection &selection,RepairStroke &out,std::string &error);
// Recompute the accumulated brushed area from the owned source. Unreachable
// repairs keep their original pixels; no repair can grow or erase a silhouette.
int repair_stroke_pixels(RepairStroke &stroke,int x,int y,bool all_edges,std::string &error);
int smart_erase_pixels(const Clipboard &source,const PixelSelection &selection,int x,int y,bool match_colors,int tolerance,bool contiguous,bool cleanup,std::vector<unsigned char> &out,std::string &error);
}
