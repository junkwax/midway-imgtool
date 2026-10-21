#include "studio_library_split_animations.h"
#include "studio_animation_copy.h"
#include "studio_animation_dependencies.h"
#include <algorithm>
#include <set>

namespace studio {
namespace {
bool build(const Artwork &source,const LibraryCopyPlan &copy,LibrarySplitAnimationPlan &out,
           std::unique_ptr<Artwork> &result,std::string &error){
    result=source.prepare_library_split(copy,error);if(!result)return false;
    LibrarySplitAnimationPlan p;p.source=&source;p.target=copy.target;
    p.source_generation=source.generation();p.target_generation=copy.target_generation;
    std::set<int> sprites;AnimationCopyOptions options;
    for(auto &r:copy.rows)if(r.enabled){
        int index=source.sprite_index(r.sprite),destination=(int)p.sprites.size();
        p.sprites.push_back(r.sprite);sprites.insert(index);
        options.sprite_bindings[r.sprite]=result->sprite_id(destination);
    }
    std::vector<EmbeddedRecord> records;if(!read_embedded(source,records,error))return false;
    auto bytes=(const unsigned char *)source.data.scrseqmem_p;
    if(source.data.scrseqbytes)p.source_blob.assign(bytes,bytes+source.data.scrseqbytes);
    std::vector<AnimationDependencies> dependencies;
    if(!read_animation_dependencies(source,records,p.source_damage,dependencies,error))return false;
    std::vector<std::vector<int>> followers(records.size());std::vector<int> blocked;
    for(int i=0;i<(int)records.size();++i){
        auto &record=records[i];auto &deps=dependencies[i];LibrarySplitAnimationRow row;
        row.source=i;row.script=record.script;row.name=record.name;row.reason=deps.error;
        for(int index:deps.sprites)if(!sprites.count(index)&&row.reason.empty())row.reason="Uses artwork outside this split";
        if(deps.sprites.empty()&&deps.records.empty()&&row.reason.empty())row.reason=record.script?"No sequence calls":"No artwork references";
        for(int dependency:deps.records)followers[dependency].push_back(i);
        if(!row.reason.empty())blocked.push_back(i);p.rows.push_back(std::move(row));
    }
    // A damaged sequence may depend on a script, which may depend on another
    // sequence. Propagate exclusions through cycles without adding unchosen art.
    for(size_t cursor=0;cursor<blocked.size();++cursor)for(int follower:followers[blocked[cursor]])if(p.rows[follower].reason.empty()){
        p.rows[follower].reason="Calls or damage bindings require an animation outside this split: "+records[blocked[cursor]].name;blocked.push_back(follower);
    }
    for(int i=0;i<(int)p.rows.size();++i){auto &row=p.rows[i];row.included=row.reason.empty();if(row.included)options.requested.insert(i);}
    if(!options.requested.empty()){
        AnimationCopyPlan animations;
        if(!plan_animation_copy(source,*result,options,animations,error)||result->apply_animation_copy(source,animations,error)<0)return false;
    }
    p.sequences=result->data.seqcnt;p.scripts=result->data.scrcnt;
    bytes=(const unsigned char *)result->data.scrseqmem_p;
    if(result->data.scrseqbytes)p.result_blob.assign(bytes,bytes+result->data.scrseqbytes);
    if(result->data.damtblbytes){bytes=(const unsigned char *)result->data.damtbl_p;p.result_damage.assign(bytes,bytes+result->data.damtblbytes);}
    out=std::move(p);return true;
}
bool same(const LibrarySplitAnimationPlan &a,const LibrarySplitAnimationPlan &b){
    if(a.source!=b.source||a.target!=b.target||a.source_generation!=b.source_generation||
       a.target_generation!=b.target_generation||a.sprites!=b.sprites||a.source_blob!=b.source_blob||
       a.result_blob!=b.result_blob||a.source_damage!=b.source_damage||a.result_damage!=b.result_damage||a.sequences!=b.sequences||a.scripts!=b.scripts||a.rows.size()!=b.rows.size())return false;
    for(size_t i=0;i<a.rows.size();++i){auto &x=a.rows[i];auto &y=b.rows[i];
        if(x.source!=y.source||x.script!=y.script||x.included!=y.included||x.name!=y.name||x.reason!=y.reason)return false;
    }return true;
}
}
bool plan_library_split_animations(const Artwork &source,const LibraryCopyPlan &copy,LibrarySplitAnimationPlan &out,std::string &error){
    out={};error.clear();
    try{std::unique_ptr<Artwork> result;return build(source,copy,out,result,error);}
    catch(const std::bad_alloc &){error="Not enough memory to review animations for the split. The source was not changed.";return false;}
}
std::unique_ptr<Artwork> Artwork::prepare_library_split_animations(const LibraryCopyPlan &copy,const LibrarySplitAnimationPlan &plan,std::string &error)const{
    error.clear();
    try{
        LibrarySplitAnimationPlan checked;std::unique_ptr<Artwork> result;
        if(!build(*this,copy,checked,result,error))return {};
        if(!same(plan,checked)){error="The animation split review changed. Cancel and reopen it.";return {};}
        // Creation is one new unsaved document, without a synthetic Undo step.
        result->past.clear();result->future.clear();result->pending.reset();return result;
    }catch(const std::bad_alloc &){error="Not enough memory to prepare the split. The source was not changed.";return {};}
}
}
