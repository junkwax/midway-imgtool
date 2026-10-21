#include "studio_animation_dependencies.h"
#include <algorithm>
namespace studio {
bool read_animation_dependencies(const Artwork &art,const std::vector<EmbeddedRecord> &records,
                                 std::vector<unsigned char> &damage,
                                 std::vector<AnimationDependencies> &out,std::string &error){
    const auto &d=art.data;
    if(d.damcnt>32767||d.damtblbytes!=d.damcnt*4||(d.damtblbytes&&!d.damtbl_p)){
        error="The global damage-reference table is inconsistent. Reopen or repair this library.";return false;
    }
    damage.clear();if(d.damtblbytes){auto p=(const unsigned char *)d.damtbl_p;damage.assign(p,p+d.damtblbytes);}
    out.clear();out.resize(records.size());
    auto word=[](const unsigned char *p){return p[0]|(p[1]<<8);};
    for(size_t i=0;i<records.size();++i){auto &r=records[i];auto &deps=out[i];
        for(auto &entry:r.entries)if(entry.values.index>=0){int index=entry.values.index;
            if(index>=(r.script?(int)d.seqcnt:art.count()))deps.error=r.script?"Unresolved sequence call":"Unresolved source sprite";
            else (r.script?deps.records:deps.sprites).push_back(index);
        }
        if(!r.script)for(int j=88;j<94;++j){int slot=r.raw[j];if(slot==255)continue;
            if(slot>=128||slot>=(int)d.damcnt){deps.error="Damage binding has an invalid table index.";continue;}
            auto p=damage.data()+slot*4;int kind=word(p),index=(short)word(p+2);
            if(index<0||(kind!=0&&kind!=0x40)||index>=(kind==0x40?(int)d.seqcnt:(int)d.scrcnt)){
                deps.error="Damage binding points to an unresolved sequence or script.";continue;
            }
            deps.damage.push_back(slot);deps.records.push_back(kind==0x40?index:(int)d.seqcnt+index);
        }
        for(auto list:{&deps.records,&deps.sprites,&deps.damage}){std::sort(list->begin(),list->end());list->erase(std::unique(list->begin(),list->end()),list->end());}
    }
    return true;
}
}
