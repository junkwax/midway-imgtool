#include "studio_asm_scene.h"
#include <map>
#include <set>
#include <algorithm>
namespace studio {
namespace {
bool same_animation(const AsmSourceAnimation &a,const AsmSourceAnimation &b){
    if(a.label!=b.label||a.name!=b.name||a.partial!=b.partial||a.notices!=b.notices||a.frames.size()!=b.frames.size())return false;
    for(size_t i=0;i<a.frames.size();++i){auto &x=a.frames[i];auto &y=b.frames[i];
        if(x.name!=y.name||x.pieces!=y.pieces||x.source!=y.source||x.dx!=y.dx||x.dy!=y.dy||x.segment!=y.segment||x.line!=y.line||x.flip!=y.flip||x.flip_y!=y.flip_y||x.no_sleep!=y.no_sleep)return false;
    }return true;
}
}
bool capture_asm_scene(const AsmSourceAnimation &animation,const std::vector<AsmLibrary> &libraries,uint64_t preferred,
                       const AsmArtworkChoices &choices,const AsmSceneOptions &options,
                       std::shared_ptr<const AsmSceneSnapshot> &out,std::string &error){
    error.clear();try{
        if(animation.partial||animation.frames.empty()||animation.frames.size()>4096){error="Choose a complete expanded animation with 1–4096 frames. Partial runtime previews cannot create a scene.";return false;}
        if(options.hold<1||options.hold>120||options.origin_x<-4096||options.origin_x>4096||options.origin_y<-4096||options.origin_y>4096||options.convention<0||options.convention>1){error="Choose a 1–120 tick preview hold and an origin within 4096 pixels.";return false;}
        size_t references=0;for(auto &frame:animation.frames){
            if(frame.no_sleep){error="No-sleep frames do not have an ordinary frame hold. Keep this source in the inspector until its timing is authored explicitly.";return false;}
            if(frame.pieces.empty()||frame.pieces.size()>32||(references+=frame.pieces.size())>65536){error="Scene snapshots need 1–32 pieces per frame and at most 65536 references.";return false;}
        }
        AsmAssetReview review;if(!review_asm_assets(animation,libraries,preferred,choices,review,error))return false;
        if(review.missing||review.ambiguous||review.stale){error="Resolve every missing, ambiguous or stale symbol in Review artwork before creating a scene.";return false;}
        auto snapshot=std::make_shared<AsmSceneSnapshot>();snapshot->animation=animation;snapshot->options=options;
        auto &scene=snapshot->state;scene.origin_x=options.origin_x;scene.origin_y=options.origin_y;scene.floor=254;scene.loop=options.loop;scene.onion=false;scene.convention=options.convention;
        std::map<std::string,SceneSprite> resolved;std::map<uint64_t,int> assets;std::set<std::pair<uint64_t,uint64_t>> captured;size_t budget=0;
        for(auto &row:review.rows){auto match=std::find_if(libraries.begin(),libraries.end(),[&](const AsmLibrary &lib){return lib.uid==row.resolved.doc;});
            if(match==libraries.end()||!match->art){error="A resolved source library is unavailable.";return false;}
            const auto &art=*match->art;int index=art.sprite_index(row.resolved.sprite);if(index<0){error="A resolved source sprite is unavailable.";return false;}
            auto asset=assets.find(match->uid);if(asset==assets.end()){
                if(assets.size()>=64){error="A scene can reference at most 64 IMG libraries.";return false;}
                asset=assets.emplace(match->uid,(int)scene.assets.size()).first;scene.assets.push_back({art.path(),art.path(),{},match->uid});
            }
            resolved.emplace(asm_artwork_key(row.symbol),scene_sprite_reference(art,asset->second,index));
            if(captured.insert({match->uid,row.resolved.sprite}).second){SubframeSpriteSnapshot sprite;if(!capture_subframe_sprite(art,index,budget,sprite,error))return false;snapshot->sprites.push_back(std::move(sprite));snapshot->generations.push_back(art.generation());}
        }
        SceneLane lane;lane.name=animation.label;lane.loop=options.loop;lane.hold_end=!options.loop;lane.flip_x=options.facing;
        for(auto &source:animation.frames){SceneFrame frame;frame.name=source.name;frame.ticks=options.hold;frame.x=options.facing?-source.dx:source.dx;frame.y=source.dy;frame.flip_x=source.flip;frame.flip_y=source.flip_y;
            for(auto &symbol:source.pieces)frame.pieces.push_back(resolved.at(asm_artwork_key(symbol)));lane.frames.push_back(std::move(frame));}
        scene.lanes.push_back(std::move(lane));if(!validate_scene(scene,error))return false;
        out=std::move(snapshot);return true;
    }catch(const std::bad_alloc &){error="Not enough memory to review this ASM scene. The previous review and artwork are unchanged.";return false;}
}
std::unique_ptr<SceneProject> create_asm_scene(const AsmSceneSnapshot &snapshot,const AsmSourceAnimation &animation,
                       const std::vector<AsmLibrary> &libraries,uint64_t preferred,const AsmArtworkChoices &choices,std::string &error){
    error.clear();try{
        if(!same_animation(snapshot.animation,animation)){error="The inspected ASM changed. Rebuild the scene review.";return {};}
        std::shared_ptr<const AsmSceneSnapshot> fresh;if(!capture_asm_scene(animation,libraries,preferred,choices,snapshot.options,fresh,error))return {};
        if(snapshot.generations!=fresh->generations||snapshot.sprites.size()!=fresh->sprites.size()||snapshot.state.assets.size()!=fresh->state.assets.size()){error="Source artwork changed. Rebuild the scene review.";return {};}
        for(size_t i=0;i<snapshot.state.assets.size();++i)if(snapshot.state.assets[i].runtime_doc!=fresh->state.assets[i].runtime_doc||snapshot.state.assets[i].path!=fresh->state.assets[i].path){error="The artwork mapping changed. Rebuild the scene review.";return {};}
        for(size_t f=0;f<snapshot.state.lanes[0].frames.size();++f)for(size_t p=0;p<snapshot.state.lanes[0].frames[f].pieces.size();++p){auto &x=snapshot.state.lanes[0].frames[f].pieces[p];auto &y=fresh->state.lanes[0].frames[f].pieces[p];
            if(x.asset!=y.asset||x.runtime_id!=y.runtime_id||x.index!=y.index||x.key!=y.key){error="The artwork mapping changed. Rebuild the scene review.";return {};}}
        for(size_t i=0;i<snapshot.sprites.size();++i)if(!same_subframe_snapshot(snapshot.sprites[i],fresh->sprites[i])){error="Source artwork changed. Rebuild the scene review.";return {};}
        return std::make_unique<SceneProject>(fresh->state);
    }catch(const std::bad_alloc &){error="Not enough memory to create the ASM scene. Artwork and the open project are unchanged.";return {};}
}
}
