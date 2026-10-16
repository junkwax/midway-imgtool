#include "studio_reactions.h"
#include <algorithm>
#include <cctype>

namespace studio {
namespace {
std::string key(std::string s){for(auto &c:s)c=(char)std::tolower((unsigned char)c);return s;}
}
std::vector<ReactionAnimation> classify_asm_reactions(const AsmSource &source){
    std::vector<ReactionAnimation> rows;rows.reserve(source.animations.size());
    for(int i=0;i<(int)source.animations.size();++i){
        auto &a=source.animations[i];ReactionAnimation r;r.animation=i;r.label=a.label;r.name=a.name;
        r.classification=ReactionClassify(a.label.c_str(),a.name.c_str());
        if(!r.classification.thrower.empty()){
            auto name=ReactionFighterName(r.classification.thrower.c_str());
            r.thrower=name?name:r.classification.thrower;
        }
        rows.push_back(std::move(r));
    }
    std::stable_sort(rows.begin(),rows.end(),[](const ReactionAnimation &a,const ReactionAnimation &b){
        int ak=(int)a.classification.kind,bk=(int)b.classification.kind;
        if(!ak)ak=kReactionKindCount;if(!bk)bk=kReactionKindCount;
        if(ak!=bk)return ak<bk;return key(a.label)<key(b.label);
    });return rows;
}
std::vector<int> filter_asm_reactions(const std::vector<ReactionAnimation> &rows,int family,bool show_all,const std::string &search){
    std::vector<int> visible;if(family<-1||family>=kReactionKindCount)return visible;
    auto query=key(search);
    for(int i=0;i<(int)rows.size();++i){auto &r=rows[i];int kind=(int)r.classification.kind;
        if((!show_all&&!kind)||(family>=0&&kind!=family))continue;
        if(!query.empty()&&key(r.label+" "+r.name+" "+r.thrower+" "+r.classification.rule).find(query)==std::string::npos)continue;
        visible.push_back(i);
    }return visible;
}
ReactionAnchorReview review_reaction_anchors(const AsmSourceAnimation &animation,const std::vector<ReactionLibrary> &libraries,uint64_t preferred,bool facing,const AsmArtworkChoices &choices){
    ReactionAnchorReview review;std::vector<AsmLibrary> libs;
    for(auto &lib:libraries)if(lib.art)libs.push_back({lib.uid,lib.art});
    for(int frame=0;frame<(int)animation.frames.size();++frame){
        auto &f=animation.frames[frame];
        if(review.rows.size()+f.pieces.size()>32768){review.truncated=true;break;}
        auto pieces=resolve_asm_frame(f,libs,preferred,choices);
        for(int piece=0;piece<(int)pieces.size();++piece){
            ReactionAnchorRow r;r.frame=frame;r.piece=piece;r.line=f.line;r.reference=std::move(pieces[piece]);
            for(auto &lib:libraries)if(lib.uid==r.reference.doc&&lib.art){
                auto im=lib.art->sprite(lib.art->sprite_index(r.reference.sprite));if(!im)break;
                r.w=im->w;r.h=im->h;r.anix=(short)im->anix;r.aniy=(short)im->aniy;r.convention=lib.convention;
                auto p=place_asm_piece(*im,f,0,0,facing,lib.convention);r.x=p.x;r.y=p.y;
                r.flip=facing!=f.flip;r.flip_y=f.flip_y;break;
            }
            review.rows.push_back(std::move(r));
        }
    }return review;
}
}
