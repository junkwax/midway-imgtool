#pragma once
#include "studio_document.h"
#include "studio_selection.h"
namespace studio {
enum class BrushShape {Round,Square};
struct PaintBrush {
    int color=1,radius=1,source=-1,tolerance=0;
    BrushShape shape=BrushShape::Round;
    bool match_colors=false;
};
// Indexed buffers have tight or four-byte-padded rows. Returns changed pixels,
// or -1 for invalid input. A selection clips both writes and fill connectivity.
int paint_line_pixels(unsigned char *pixels,int w,int h,int stride,const PAL &palette,int x0,int y0,int x1,int y1,const PaintBrush &brush,const PixelSelection *selection,std::string &error);
int fill_matching_pixels(unsigned char *pixels,int w,int h,int stride,const PAL &palette,int x,int y,int color,bool match_colors,int tolerance,bool contiguous,const PixelSelection *selection,std::string &error);
// Source pixels and the destination mask are frozen for one clone gesture.
struct CloneStroke {
    const Artwork *owner=nullptr;uint64_t generation=0,sprite=0;
    Clipboard original;PixelSelection selection;
    PaintBrush brush;int dx=0,dy=0;bool copy_transparent=false;
};
int clone_line_pixels(unsigned char *pixels,int w,int h,int stride,const Clipboard &source,int dx,int dy,int x0,int y0,int x1,int y1,const PaintBrush &brush,bool copy_transparent,const PixelSelection *selection,std::string &error);
bool begin_clone_stroke(const Artwork &art,int index,int dx,int dy,const PaintBrush &brush,bool copy_transparent,const PixelSelection &selection,CloneStroke &out,std::string &error);
// Shared bounded center path for brushes that need one complete stamp per step.
bool make_brush_path(int w,int h,int radius,int x0,int y0,int x1,int y1,std::vector<std::pair<int,int>> &out,std::string &error);
struct PaintLineReview {
    const Artwork *owner=nullptr;uint64_t generation=0,sprite=0;
    Clipboard original;PixelSelection selection;
    PaintBrush brush;int x0=0,y0=0,x1=0,y1=0;
};
bool begin_paint_line(const Artwork &art,int index,int x,int y,const PaintBrush &brush,const PixelSelection &selection,PaintLineReview &out,std::string &error);
bool preview_paint_line(const PaintLineReview &line,std::vector<unsigned char> &pixels,int &changed,std::string &error);
}
