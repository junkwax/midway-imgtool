#include "studio_library_split_animations.h"
#include "studio_animation_copy.h"
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
    std::set<int> sequences;
    for(int i=0;i<(int)records.size();++i){
        auto &record=records[i];LibrarySplitAnimationRow row;
        row.source=i;row.script=record.script;row.name=record.name;bool positive=false;
        if(!record.script){
            if(std::any_of(record.raw.begin()+88,record.raw.begin()+94,[](unsigned char n){return n!=255;}))
                row.reason="Damage-table bindings are unsupported";
            for(auto &entry:record.entries)if(entry.values.index>=0){
                positive=true;
                if(entry.values.index>=source.count()){row.reason="Unresolved source sprite";break;}
                if(!sprites.count(entry.values.index)&&row.reason.empty())row.reason="Uses artwork outside this split";
            }
            if(!positive&&row.reason.empty())row.reason="No artwork references";
            if(row.reason.empty())sequences.insert(i);
        }else{
            for(auto &entry:record.entries)if(entry.values.index>=0){
                positive=true;
                if(entry.values.index>=(int)source.data.seqcnt){row.reason="Unresolved sequence call";break;}
                if(!sequences.count(entry.values.index)&&row.reason.empty())row.reason="Calls a sequence outside this split";
            }
            if(!positive&&row.reason.empty())row.reason="No sequence calls";
        }
        row.included=row.reason.empty();if(row.included)options.requested.insert(i);
        p.rows.push_back(std::move(row));
    }
    if(!options.requested.empty()){
        AnimationCopyPlan animations;
        if(!plan_animation_copy(source,*result,options,animations,error)||result->apply_animation_copy(source,animations,error)<0)return false;
    }
    p.sequences=result->data.seqcnt;p.scripts=result->data.scrcnt;
    bytes=(const unsigned char *)result->data.scrseqmem_p;
    if(result->data.scrseqbytes)p.result_blob.assign(bytes,bytes+result->data.scrseqbytes);
    out=std::move(p);return true;
}
bool same(const LibrarySplitAnimationPlan &a,const LibrarySplitAnimationPlan &b){
    if(a.source!=b.source||a.target!=b.target||a.source_generation!=b.source_generation||
       a.target_generation!=b.target_generation||a.sprites!=b.sprites||a.source_blob!=b.source_blob||
       a.result_blob!=b.result_blob||a.sequences!=b.sequences||a.scripts!=b.scripts||a.rows.size()!=b.rows.size())return false;
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
