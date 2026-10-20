#pragma once
#include "studio_palette.h"
#include "studio_selection.h"
#include "sprite_gradient.h"
namespace studio {
using SpriteRampColor=std::array<float,3>;
struct SpriteRampOptions {
    int direction=2,bpp=6,count=32,shade_span=8,start=0,end=100,dither=3,seed=17,band=0,ppp=6;
    bool content_bounds=true;
    std::vector<SpriteRampColor> stops{{0,0,0},{1,1,1}};
};
struct SpriteRampTarget {uint64_t id=0;int palette=-1,row=-1;};
struct SpriteRampSource {
    const Artwork *owner=nullptr;uint64_t generation=0;
    PixelSelection selection;
    std::vector<PaletteCompactionPlan> palettes;
    std::vector<SpriteRampTarget> targets;
};
struct SpriteRampPalette {
    std::vector<unsigned short> words;
    int depth=0;bool active=false,ppp_changed=false;
};
struct SpriteRampRow {
    bool included=false;uint64_t changed=0,color_changes=0;
    int max_before=0,max_after=0,tones=0;
    std::vector<unsigned char> after;
    std::string blocker;
};
struct SpriteRampPlan {
    std::shared_ptr<const SpriteRampSource> source;SpriteRampOptions options;
    std::vector<uint64_t> included;
    std::vector<SpriteRampPalette> palettes;
    std::vector<SpriteRampRow> rows;
    std::vector<unsigned short> ramp;
    int start=-1,count=0,free_run=0,shade_span=0,blocked=0,changed_frames=0;
    uint64_t changed=0,color_changes=0;
    std::string blocker;
};
bool capture_sprite_ramp(const Artwork &,const std::vector<uint64_t> &,const PixelSelection &,std::shared_ptr<const SpriteRampSource> &,std::string &error);
bool plan_sprite_ramp(std::shared_ptr<const SpriteRampSource>,const std::vector<uint64_t> &included,const SpriteRampOptions &,SpriteRampPlan &,std::string &error);
std::vector<SpriteRampColor> seed_sprite_ramp(const SpriteRampSource &,int target=0);
std::vector<unsigned char> render_sprite_ramp(const SpriteRampPlan &,int target,bool after);
}
