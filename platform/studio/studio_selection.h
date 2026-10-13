#pragma once
#include "img_format.h"
#include <vector>
namespace studio {
enum class SelectionOp {Replace,Add,Subtract,Intersect};
struct SelectionPoint {float x=0,y=0;};
struct PixelSelection {
    bool valid=false;int x=0,y=0,w=0,h=0;
    int canvas_w=0,canvas_h=0;std::vector<unsigned char> mask;
    bool contains(int px,int py)const;
    bool fits(int width,int height)const;
    int count()const;
};
PixelSelection rectangle_selection(int width,int height,int x,int y,int w,int h);
PixelSelection selection_from_mask(int width,int height,std::vector<unsigned char> mask);
PixelSelection wand_selection(const IMG &image,const PAL &palette,int x,int y,int tolerance,bool contiguous);
PixelSelection lasso_selection(int width,int height,const std::vector<SelectionPoint> &points);
PixelSelection opaque_selection(const IMG &image);
PixelSelection invert_selection(const PixelSelection &selection,int width,int height);
PixelSelection combine_selection(const PixelSelection &base,const PixelSelection &added,SelectionOp operation,int width,int height);
PixelSelection translate_selection(const PixelSelection &selection,int dx,int dy,int width,int height);
}
