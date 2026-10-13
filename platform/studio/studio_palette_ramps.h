#pragma once
#include "studio_palette.h"
namespace studio {
using GradientColor=std::array<unsigned char,3>;
struct GradientPreset {std::string name;std::vector<GradientColor> stops;};
struct PaletteRampOptions {
    bool shading=true,brightness_order=true;
    GradientColor base{255,110,30};
    int strength=100,brightness=0,contrast=0,hue=0,saturation=0,lightness=0;
    std::vector<GradientColor> stops{{0,0,0},{255,255,255}};
};
bool build_palette_ramp(const std::vector<unsigned short> &baseline,const std::array<bool,256> &selected,const PaletteRampOptions &options,std::vector<unsigned short> &out,std::string &error);
std::vector<GradientColor> seed_gradient_stops(const std::vector<unsigned short> &words,const std::array<bool,256> &selected,int count);
std::vector<GradientColor> shifted_gradient_stops(const std::vector<GradientColor> &stops,int hue,int saturation,int lightness);
std::vector<GradientColor> resample_gradient_stops(const std::vector<GradientColor> &stops,int count);
// Studio's store has the same plain text syntax as classic but a separate path.
bool read_gradient_presets(const std::string &path,std::vector<GradientPreset> &out,std::string &error);
bool write_gradient_presets(const std::string &path,const std::vector<GradientPreset> &presets,std::string &error);
}
