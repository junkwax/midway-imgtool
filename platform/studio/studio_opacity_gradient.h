#pragma once
#include "studio_document.h"
#include "studio_selection.h"
namespace studio {
struct OpacityGradientOptions {
    int direction=2,start=100,end=0,pattern=0,seed=17,feather=5,depth=8;
    bool content_bounds=true,limit_depth=false,trim=false;
};
struct OpacityGradientSprite {
    uint64_t id=0;int index=-1;
    std::array<unsigned char,sizeof(IMG)> fields{};
    std::array<unsigned char,sizeof(PAL)> palette_fields{};
    std::vector<unsigned char> pixels,points,alternate,layer;
    std::vector<unsigned short> words;
};
struct OpacityGradientSource {
    const Artwork *owner=nullptr;uint64_t generation=0;
    PixelSelection selection;
    std::vector<OpacityGradientSprite> sprites;
};
struct OpacityGradientRow {
    bool enabled=true,trimmed=false,empty=false;
    int w=0,h=0,x=0,y=0;
    unsigned short anix=0,aniy=0,anix2=0,aniy2=0;
    uint64_t opaque=0,removed=0;
    std::vector<unsigned char> pixels,points;
    std::string blocker;
};
struct OpacityGradientPlan {
    std::shared_ptr<const OpacityGradientSource> source;
    OpacityGradientOptions options;
    std::vector<OpacityGradientRow> rows;
};
bool capture_opacity_gradient(const Artwork &,const std::vector<uint64_t> &,const PixelSelection &,std::shared_ptr<const OpacityGradientSource> &,std::string &error);
bool plan_opacity_gradient(std::shared_ptr<const OpacityGradientSource>,const OpacityGradientOptions &,OpacityGradientPlan &,std::string &error);
std::vector<unsigned char> render_opacity_gradient(const OpacityGradientPlan &,int row,bool after);
}
