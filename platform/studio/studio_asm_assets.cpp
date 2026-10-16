#include "studio_asm_assets.h"
#include <unordered_map>
#include <unordered_set>
namespace studio {
bool review_asm_assets(const AsmSourceAnimation &animation,const std::vector<AsmLibrary> &libraries,
                       uint64_t preferred,const AsmArtworkChoices &choices,AsmAssetReview &out,std::string &error){
    try{
        error.clear();AsmAssetReview next;std::unordered_map<std::string,size_t> rows;size_t uses=0,candidates=0;
        for(size_t frame=0;frame<animation.frames.size();++frame){auto &f=animation.frames[frame];
            for(auto &symbol:f.pieces){
                if(++uses>262144){error="Animation artwork review exceeds 262144 references.";return false;}
                auto key=asm_artwork_key(symbol);auto found=rows.find(key);
                if(found==rows.end()){
                    if(next.rows.size()>=32768){error="Animation artwork review exceeds 32768 symbols.";return false;}
                    AsmAssetRow row;row.symbol=symbol;row.source=f.source;row.first_frame=(int)frame;row.line=f.line;
                    found=rows.emplace(key,next.rows.size()).first;next.rows.push_back(std::move(row));
                }
                ++next.rows[found->second].uses;
            }
        }
        std::unordered_set<uint64_t> seen;
        for(auto &lib:libraries){if(!lib.uid||!seen.insert(lib.uid).second){error="Artwork review requires unique, nonzero library IDs.";return false;}
            if(!lib.art)continue;
            for(int i=0;i<lib.art->count();++i){auto im=lib.art->sprite(i);auto found=rows.find(asm_artwork_key(im->n_s));if(found==rows.end())continue;
                if(++candidates>65536){error="Animation artwork review exceeds 65536 matching sprites.";return false;}
                auto &row=next.rows[found->second];AsmPiece p;p.symbol=row.symbol;p.doc=lib.uid;p.sprite=lib.art->sprite_id(i);row.candidates.push_back(std::move(p));
            }
        }
        for(auto &row:next.rows){row.resolved=resolve_asm_candidates(row.symbol,row.candidates,preferred,choices);
            if(row.resolved.sprite)++next.resolved;else if(row.resolved.stale)++next.stale;else if(row.resolved.ambiguous)++next.ambiguous;else ++next.missing;
        }
        out=std::move(next);return true;
    }catch(const std::exception &e){error=e.what();return false;}
}
}
