#include "studio_animation_copy.h"
namespace studio {
std::unique_ptr<Artwork> Artwork::prepare_animation_snapshot(const AnimationCopyPlan &p,std::string &error)const{
    error.clear();auto target=p.target;
    if(!target||target->count()||target->palette_count()||target->data.seqcnt||target->data.scrcnt||target->disk_version()!=disk_version()||!p.options.copy_missing||!p.options.prefix.empty()||!p.options.sprite_prefix.empty()||!p.options.sprite_bindings.empty()){
        error="Animation extraction requires an empty matching-layout target, unchanged names and all required artwork.";return {};
    }
    try{
        int count=target->validate_animation_copy(*this,p,error);if(count<=0){if(!count)error="Choose at least one animation.";return {};}
        auto result=split_target();AnimationCopyPlan owned;
        if(!plan_animation_copy(*this,*result,p.options,owned,error)||result->apply_animation_copy(*this,owned,error)!=count)return {};
        int destination=0;for(auto &row:p.artwork.rows)if(row.enabled){auto found=legacy_reserved_alternates.find(row.sprite);if(found!=legacy_reserved_alternates.end())result->legacy_reserved_alternates[result->sprite_id(destination)]=found->second;++destination;}
        result->past.clear();result->future.clear();result->pending.reset();
        return result;
    }catch(const std::bad_alloc &){error="Not enough memory to prepare the animations. The source was not changed.";return {};}
}
std::unique_ptr<Artwork> Artwork::prepare_animation_split(const AnimationCopyPlan &p,std::string &error)const{
    auto result=prepare_animation_snapshot(p,error);if(!result)return {};
    if(!result->count()){error="Select an animation that references artwork to create a new IMG library.";return {};}
    // A new tab is unsaved and has no artificial placeholder or creation Undo.
    return result;
}
}
