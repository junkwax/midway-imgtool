#include "studio_asm_artwork_library.h"
#include "studio_library_copy.h"
#include <map>
#include <algorithm>
namespace studio {
std::shared_ptr<const AsmArtworkLibrary> AsmArtworkLibrary::capture(const AsmSourceAnimation &animation,
    const std::vector<AsmLibrary> &libraries,uint64_t preferred,const AsmArtworkChoices &choices,bool allow_partial,std::string &error){
    try{
        error.clear();if(animation.partial&&!allow_partial){error="This is a partial ASM preview. Explicitly include only its expanded frames before preparing a library.";return {};}
        AsmAssetReview review;if(!review_asm_assets(animation,libraries,preferred,choices,review,error))return {};
        if(review.rows.empty()||review.rows.size()>2000){error="Choose an animation with 1 to 2000 artwork symbols.";return {};}
        if(review.missing||review.ambiguous||review.stale){error="Resolve every required artwork symbol before preparing the library.";return {};}
        std::map<uint64_t,const Artwork *> sources;for(auto &lib:libraries)if(lib.art)sources[lib.uid]=lib.art;
        struct Group {uint64_t id;std::vector<uint64_t> sprites;};std::vector<Group> groups;
        for(auto &row:review.rows){auto found=std::find_if(groups.begin(),groups.end(),[&](const Group &g){return g.id==row.resolved.doc;});
            if(found==groups.end()){groups.push_back({row.resolved.doc,{}});found=groups.end()-1;}found->sprites.push_back(row.resolved.sprite);
        }
        auto result=std::shared_ptr<AsmArtworkLibrary>(new AsmArtworkLibrary);result->animation_label=animation.label;result->partial_preview=animation.partial;
        unsigned version=sources.at(groups.front().id)->disk_version();
        std::map<std::pair<uint64_t,uint64_t>,int> targets;
        for(auto &group:groups){auto source=sources.at(group.id);
            if(source->disk_version()!=version){error="Contributing IMGs use different versions. Create separate artwork libraries for their original layouts.";return {};}
            auto target=result->storage?nullptr:source->split_target();auto destination=result->storage?result->storage.get():target.get();
            LibraryCopyPlan plan;if(!plan_library_copy(*source,*destination,group.sprites,{},plan,error))return {};
            for(auto &row:plan.rows)if(!row.error.empty()){error=row.name+": "+row.error;return {};}
            int first=destination->count();
            if(!result->storage){result->storage=source->prepare_library_split(plan,error);if(!result->storage)return {};}
            else if(result->storage->apply_library_copy(*source,plan,error)!=(int)group.sprites.size())return {};
            int offset=0;for(auto &row:plan.rows){int index=first+offset++;targets[{group.id,row.sprite}]=index;
                auto reserved=source->legacy_reserved_alternates.find(row.sprite);
                if(reserved!=source->legacy_reserved_alternates.end())result->storage->legacy_reserved_alternates[result->storage->sprite_id(index)]=reserved->second;
            }
            // The detached aggregate has no user-visible, per-group creation history.
            result->storage->past.clear();result->storage->future.clear();result->storage->pending.reset();
        }
        for(auto &row:review.rows){auto source=sources.at(row.resolved.doc);AsmArtworkOrigin origin;origin.symbol=row.symbol;origin.document=row.resolved.doc;origin.sprite=row.resolved.sprite;origin.generation=source->generation();origin.uses=row.uses;
            origin.target=targets.at({origin.document,origin.sprite});origin.name=result->storage->sprite(origin.target)->n_s;origin.library=source->path().empty()?"Unsaved library #"+std::to_string(origin.document):source->path();result->rows.push_back(std::move(origin));
        }
        result->storage->selected_sprite_id=result->storage->sprite_id(result->rows.front().target);
        return result;
    }catch(const std::exception &e){error=e.what();return {};}
}
std::unique_ptr<Artwork> AsmArtworkLibrary::create_library(std::string &error)const{
    try{auto target=storage->split_target();std::vector<uint64_t> ids;for(int i=0;i<storage->count();++i)ids.push_back(storage->sprite_id(i));
        LibraryCopyPlan plan;if(!plan_library_copy(*storage,*target,ids,{},plan,error))return {};
        auto result=storage->prepare_library_split(plan,error);if(result)result->selected_sprite_id=result->sprite_id(rows.front().target);return result;
    }catch(const std::exception &e){error=e.what();return {};}
}
}
