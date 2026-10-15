#pragma once
#include "studio_animation.h"
#include <memory>
namespace studio {
// A bounded, read-only source inspection. No assembler, game code or includes run.
struct AsmSourceFrame {
    std::string name;std::vector<std::string> pieces;
    int dx=0,dy=0,segment=0,line=0;bool flip=false,flip_y=false,no_sleep=false;
};
struct AsmSourceAnimation {
    std::string label,name;std::vector<AsmSourceFrame> frames;
    std::vector<std::string> notices;bool partial=false;
};
struct AsmSource {
    std::string path;std::vector<AsmSourceAnimation> animations;
    static std::unique_ptr<AsmSource> open(const std::string &path,std::string &error);
    static std::unique_ptr<AsmSource> parse(const std::string &text,std::string &error);
};
struct AsmLibrary {uint64_t uid=0;const Artwork *art=nullptr;};
struct AsmPiece {std::string symbol;uint64_t doc=0,sprite=0;bool ambiguous=false;};
std::vector<AsmPiece> resolve_asm_frame(const AsmSourceFrame &frame,const std::vector<AsmLibrary> &libraries,uint64_t preferred);
SpritePlacement place_asm_piece(const IMG &im,const AsmSourceFrame &frame,int x,int y,bool facing,MirrorConvention convention);
}
