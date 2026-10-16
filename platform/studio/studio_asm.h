#pragma once
#include "studio_animation.h"
#include <memory>
#include <map>
namespace studio {
// A bounded, read-only source inspection. No assembler, game code or includes run.
struct AsmSourceFrame {
    std::string name;std::vector<std::string> pieces;
    std::string source; // Operand location, retained by linked snapshots.
    int dx=0,dy=0,segment=0,line=0;bool flip=false,flip_y=false,no_sleep=false;
};
struct AsmSourceAnimation {
    std::string label,name;std::vector<AsmSourceFrame> frames;
    std::vector<std::string> notices;bool partial=false;
};
struct AsmSourceInput {std::string path,text;};
struct AsmSource {
    std::string path;std::vector<AsmSourceAnimation> animations;
    static std::unique_ptr<AsmSource> open(const std::string &path,std::string &error);
    static std::unique_ptr<AsmSource> parse(const std::string &text,std::string &error);
    // Explicit resident snapshots only; local symbols win, external symbols must be unique.
    static std::unique_ptr<AsmSource> parse_linked(const std::vector<AsmSourceInput> &sources,
        size_t root,const std::string &label,std::string &error);
};
struct AsmLibrary {uint64_t uid=0;const Artwork *art=nullptr;};
struct AsmPiece {std::string symbol;uint64_t doc=0,sprite=0;bool ambiguous=false,chosen=false,stale=false;};
// Preview-only choices, keyed by uppercase artwork symbol; stable document/sprite IDs.
using AsmArtworkChoices=std::map<std::string,std::pair<uint64_t,uint64_t>>;
std::string asm_artwork_key(std::string symbol);
AsmPiece resolve_asm_candidates(const std::string &symbol,const std::vector<AsmPiece> &candidates,
                               uint64_t preferred,const AsmArtworkChoices &choices);
std::vector<AsmPiece> resolve_asm_frame(const AsmSourceFrame &frame,const std::vector<AsmLibrary> &libraries,uint64_t preferred,const AsmArtworkChoices &choices={});
SpritePlacement place_asm_piece(const IMG &im,const AsmSourceFrame &frame,int x,int y,bool facing,MirrorConvention convention);
}
