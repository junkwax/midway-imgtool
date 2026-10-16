#pragma once
#include "studio_asm.h"
#include "reaction_class.h"
#include <array>

namespace studio {
struct ReactionAnimation {
    int animation=0;
    std::string label,name,thrower;
    ReactionInfo classification;
};
// Owned directory rows, grouped by reaction family with other animations last.
std::vector<ReactionAnimation> classify_asm_reactions(const AsmSource &source);
// family=-1 means all families. Other animations are hidden unless show_all.
std::vector<int> filter_asm_reactions(const std::vector<ReactionAnimation> &rows,
                                    int family,bool show_all,const std::string &search);
struct ReactionLibrary {
    uint64_t uid=0;const Artwork *art=nullptr;
    MirrorConvention convention=MirrorConvention_Ani2;
};
struct ReactionAnchorRow {
    int frame=0,piece=0,line=0;
    AsmPiece reference;
    int w=0,h=0,anix=0,aniy=0,x=0,y=0;
    MirrorConvention convention=MirrorConvention_Ani2;
    bool flip=false,flip_y=false;
};
struct ReactionAnchorReview {
    std::vector<ReactionAnchorRow> rows;
    bool truncated=false;
};
// Positions are relative to object origin (0,0), using the same placement as
// the ASM canvas. No borrowed sprite pointers or edits. Bounded to 32768 rows.
ReactionAnchorReview review_reaction_anchors(const AsmSourceAnimation &animation,
                                            const std::vector<ReactionLibrary> &libraries,
                                            uint64_t preferred,bool facing,const AsmArtworkChoices &choices={});
}
