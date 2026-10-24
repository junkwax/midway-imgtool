#include "studio_project.h"
#include <algorithm>
#include <climits>
#include <map>
namespace studio {
int SceneProject::append_actors(const SceneState &incoming,const SceneSource &source,std::string &error){
    error.clear();if(pending){error="Finish the current project edit before appending actors.";return -1;}
    try{
        if(!validate_scene(state,error)||!validate_scene(incoming,error))return -1;
        if(state.lanes.size()+incoming.lanes.size()>32){error="Appending all actors exceeds the 32-actor limit. The scene is unchanged.";return -1;}
        size_t frames=0,pieces=0;for(auto scene:{static_cast<const SceneState *>(&state),&incoming})for(auto &actor:scene->lanes){frames+=actor.frames.size();for(auto &frame:actor.frames)pieces+=frame.pieces.size();}
        if(frames>8192||pieces>65536){error="Appending all actors exceeds the frame/reference limits. The scene is unchanged.";return -1;}
        uint64_t needed=incoming.lanes.size();for(auto &actor:incoming.lanes)needed+=actor.frames.size();
        if(next_entity_id>INT_MAX||needed>(uint64_t)INT_MAX-next_entity_id+1){error="The project has exhausted its actor/frame identities.";return -1;}
        SceneState candidate=state;std::vector<int> assets(incoming.assets.size(),-1);
        for(auto &actor:incoming.lanes)for(auto &frame:actor.frames)for(auto &piece:frame.pieces){
            int a=piece.asset;if(assets[a]>=0)continue;auto &asset=incoming.assets[a];auto art=asset.runtime_doc&&source?source(asset.runtime_doc):nullptr;
            if(!art){error="An appended actor needs an unavailable IMG library. Locate all sources before appending.";return -1;}
            for(int i=0;i<(int)candidate.assets.size();++i)if(candidate.assets[i].runtime_doc==asset.runtime_doc){assets[a]=i;break;}
            if(assets[a]<0){if(candidate.assets.size()>=64){error="Appending all actors exceeds the 64-source limit.";return -1;}assets[a]=(int)candidate.assets.size();candidate.assets.push_back(asset);}
        }
        std::map<uint64_t,uint64_t> identities;uint64_t next=next_entity_id;int first=(int)candidate.lanes.size();
        for(auto &actor:incoming.lanes){auto copy=actor;copy.id=next++;if(actor.id)identities.emplace(actor.id,copy.id);
            for(auto &frame:copy.frames){uint64_t old=frame.id;frame.id=next++;if(old)identities.emplace(old,frame.id);
                for(auto &piece:frame.pieces){auto &asset=incoming.assets[piece.asset];auto art=source(asset.runtime_doc);int index=art?art->sprite_index(piece.runtime_id):-1;auto im=art?art->sprite(index):nullptr;
                    if(!im||piece.key.empty()||scene_sprite_key(*im)!=piece.key){error="Appended artwork references changed or are unresolved. Reopen the append review.";return -1;}
                    piece=scene_sprite_reference(*art,assets[piece.asset],index);
                }
            }candidate.lanes.push_back(std::move(copy));
        }
        for(int i=first;i<(int)candidate.lanes.size();++i){auto &actor=candidate.lanes[i];if(actor.trigger_lane){actor.trigger_lane=identities.at(actor.trigger_lane);actor.trigger_frame=identities.at(actor.trigger_frame);}}
        if(!validate_scene(candidate,error))return -1;
        return install_scene_review(std::move(candidate),next,first,"Append project actors",error);
    }catch(const std::bad_alloc &){error="Not enough memory to append actors. Project and history are unchanged.";return -1;}
}
}
