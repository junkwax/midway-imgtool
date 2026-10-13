#pragma once
#include "studio_document.h"
#include "studio_selection.h"
namespace studio {
// Frozen source pixels plus editable placement. Preview never changes Artwork.
struct SelectionTransform {
    const Artwork *owner=nullptr;uint64_t generation=0,sprite=0;
    int canvas_w=0,canvas_h=0;
    PixelSelection source;
    Clipboard clip;
    std::vector<unsigned char> original;
    int x=0,y=0,w=0,h=0,rotation=0;
    bool flip_h=false,flip_v=false,copy=false;
};
struct SelectionTransformPreview {
    std::vector<unsigned char> pixels; // Tight canvas rows, including remaining source.
    PixelSelection selection;
};
bool begin_selection_transform(const Artwork &art,int sprite,const PixelSelection &selection,SelectionTransform &out,std::string &error);
bool preview_selection_transform(const SelectionTransform &transform,SelectionTransformPreview &out,std::string &error);
}
