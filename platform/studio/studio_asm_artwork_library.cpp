#include "studio_asm_artwork_library.h"
#include "studio_library_copy.h"
#include <map>
#include <algorithm>
#include <cstdio>
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
namespace {
template<size_t N> std::array<unsigned char,N> stored_name(const unsigned char (&raw)[N],const std::string &name){
    std::array<unsigned char,N> bytes;memcpy(bytes.data(),raw,N);size_t size=std::min(name.size(),N-1);memcpy(bytes.data(),name.data(),size);bytes[size]=0;return bytes;
}
bool identical_palette(const PAL *a,const PAL *b){
    if(!a||!b||!a->data_p||!b->data_p||a->numc<1||a->numc>256||a->numc!=b->numc)return false;
    return stored_name(a->file_name_raw,a->n_s)==stored_name(b->file_name_raw,b->n_s)&&a->flags==b->flags&&a->bitspix==b->bitspix&&
        a->file_data==b->file_data&&a->file_lib==b->file_lib&&a->file_colind==b->file_colind&&a->file_cmap==b->file_cmap&&a->file_spare==b->file_spare&&memcmp(a->data_p,b->data_p,a->numc*2)==0;
}
bool same_optional(const void *a,const void *b,size_t size){return (!a&&!b)||(a&&b&&memcmp(a,b,size)==0);}
}
bool AsmArtworkLibrary::identical_sprite(const Artwork &target,int source_index,int target_index,const std::string &copied_name)const{
    auto a=storage->sprite(source_index),b=target.sprite(target_index);
    if(!a||!b||a->layer_p||b->layer_p||!a->data_p||!b->data_p||a->w!=b->w||a->h!=b->h)return false;
    if(stored_name(a->file_name_raw,copied_name)!=stored_name(b->file_name_raw,b->n_s)||a->flags!=b->flags||a->anix!=b->anix||a->aniy!=b->aniy||
        a->anix2!=b->anix2||a->aniy2!=b->aniy2||a->aniz2!=b->aniz2||a->file_data!=b->file_data||a->file_lib!=b->file_lib||a->file_frm!=b->file_frm)return false;
    size_t pixels=(((size_t)a->w+3)&~size_t(3))*a->h;
    if(memcmp(a->data_p,b->data_p,pixels)||!same_optional(a->pttbl_p,b->pttbl_p,40)||!identical_palette(storage->palette(a->palnum),target.palette(b->palnum)))return false;
    if((bool)a->opaltbl_p!=(bool)b->opaltbl_p)return false;
    if(a->opaltbl_p){auto left=(const unsigned char *)a->opaltbl_p,right=(const unsigned char *)b->opaltbl_p;
        for(int i=0;i<16;++i){if(left[i]>=128||right[i]>=128){if(memcmp(left+i,right+i,16-i))return false;break;}
            if(left[i]<NUMDEFPAL||right[i]<NUMDEFPAL){if(left[i]!=right[i])return false;}
            else if(!identical_palette(storage->palette(left[i]-NUMDEFPAL),target.palette(right[i]-NUMDEFPAL)))return false;
        }
    }
    if(storage->disk_version()<0x61d){auto reserved=[](const Artwork &art,uint64_t id){auto found=art.legacy_reserved_alternates.find(id);return found==art.legacy_reserved_alternates.end()?(unsigned short)0xffff:found->second;};
        if(reserved(*storage,storage->sprite_id(source_index))!=reserved(target,target.sprite_id(target_index)))return false;
    }
    // Baselines, file offsets, list/source-group pointers and pool indices are editor/location state.
    return true;
}
bool AsmArtworkLibrary::plan_copy(const Artwork &target,const LibraryCopyOptions &options,LibraryCopyPlan &out,std::string &error,bool reuse_identical)const{
    try{error.clear();if(target.disk_version()!=storage->disk_version()){error="The destination must use the captured artwork's original IMG layout (0x";char version[16];snprintf(version,sizeof version,"%X",storage->disk_version());error+=version;error+="). No IMG format conversion is performed.";return false;}
        std::vector<uint64_t> ids;auto append=options;std::vector<LibraryCopyReuse> reused;
        for(int i=0;i<storage->count();++i){auto id=storage->sprite_id(i);ids.push_back(id);if(!reuse_identical||options.excluded.count(id))continue;
            auto name=options.prefix+storage->sprite(i)->n_s;int match=-1,count=0;
            for(int j=0;j<target.count();++j)if(asm_artwork_key(target.sprite(j)->n_s)==asm_artwork_key(name)){match=j;++count;}
            if(count==1&&identical_sprite(target,i,match,name)){append.excluded.insert(id);reused.push_back({id,target.sprite_id(match)});}
        }
        LibraryCopyPlan next;if(!plan_library_copy(*storage,target,ids,append,next,error))return false;
        next.reuse_named=reuse_identical;if(reuse_identical)next.reuse_options=options;next.reused=std::move(reused);out=std::move(next);return true;
    }catch(const std::exception &e){error=e.what();return false;}
}
int AsmArtworkLibrary::copy_to(Artwork &target,const LibraryCopyPlan &reviewed,std::string &error)const{
    try{if(target.disk_version()!=storage->disk_version()){error="The destination IMG layout differs from the captured artwork. Neither IMG format can change.";return -1;}
        if(reviewed.reuse_named||!reviewed.reused.empty()){
            auto requested=reviewed.reuse_options;
            LibraryCopyPlan checked;if(!plan_copy(target,requested,checked,error,reviewed.reuse_named))return -1;
            if(!same_library_copy_plan(reviewed,checked)){error="The artwork reuse review no longer matches. Reopen the copy review.";return -1;}
        }
        return target.apply_library_copy(*storage,reviewed,error);
    }catch(const std::exception &e){error=e.what();return -1;}
}
}
