#include "studio_sprite_scope.h"
#include "anipoint.h"
#include <map>
#include <set>
namespace studio {
namespace {
template<size_t N> std::string text(const char (&s)[N]){size_t n=0;while(n<N&&s[n])++n;return std::string(s,n);}
}
bool review_sprite_scope(const Artwork &art,const std::vector<uint64_t> &requested,bool children,SpriteScope &out,std::string &error){
    out={};error.clear();if(requested.empty()||requested.size()>2000||art.count()>2000){error="Choose 1 to 2000 sprites to review.";return false;}
    std::set<uint64_t> selected;for(auto id:requested)if(art.sprite_index(id)<0||!selected.insert(id).second){error="The scope contains a missing or repeated sprite.";return false;}
    SpriteScope scope;scope.requested=requested;scope.include_children=children;
    try{
        using Key=std::pair<std::string,std::string>;
        std::map<Key,std::vector<int>> parents;
        if(children)for(int i=0;i<art.count();++i){auto im=art.sprite(i);auto name=trim_sprite_name(text(im->n_s));if(!name.empty())parents[{text(im->src_filename),name}].push_back(i);}
        for(int i=0;i<art.count();++i){auto id=art.sprite_id(i);if(selected.count(id)){scope.rows.push_back({id,0,{}});continue;}if(!children)continue;
            auto im=art.sprite(i);auto name=trim_sprite_name(text(im->n_s));auto source=text(im->src_filename);std::set<int> candidates;
            auto add=[&](const std::string &parent){auto found=parents.find({source,parent});if(found!=parents.end())for(int index:found->second)if(index!=i)candidates.insert(index);};
            add(InferSubframeParentName(name.c_str()));std::string numbered;if(strip_trailing_sequence_digits(name,&numbered))add(numbered);
            bool relevant=false;for(int index:candidates)relevant|=selected.count(art.sprite_id(index))!=0;if(!relevant)continue;
            if(candidates.size()!=1){++scope.ambiguous;scope.skipped.push_back(name);continue;}int parent=*candidates.begin();scope.rows.push_back({id,art.sprite_id(parent),trim_sprite_name(text(art.sprite(parent)->n_s))});
        }
    }catch(const std::bad_alloc &){error="Not enough memory to review child pieces.";return false;}
    out=std::move(scope);return true;
}
}
