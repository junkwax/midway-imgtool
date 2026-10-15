#include "studio_animation_clipboard.h"
namespace studio {
std::shared_ptr<const AnimationClipboard> AnimationClipboard::capture(const Artwork &source,const AnimationCopyPlan &reviewed,const std::string &label,std::string &error){
    error.clear();
    try{
        auto result=std::make_shared<AnimationClipboard>();result->storage=source.prepare_animation_snapshot(reviewed,error);if(!result->storage)return {};
        result->source_label=label;
        for(auto &row:reviewed.rows)if(row.included)result->records.push_back(row);
        // The immutable clipboard needs no history copies or unsaved-tab state.
        result->storage->revision=result->storage->saved_revision=0;result->storage->next_revision=1;
        return result;
    }catch(const std::bad_alloc &){error="Not enough memory to capture animations. The previous clipboard was retained.";return {};}
}
}
