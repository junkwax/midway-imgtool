#include "studio_project.h"
#include <algorithm>
#include <climits>
namespace studio {
int SceneProject::duplicate_actor(uint64_t lane_id,std::string &error){
    error.clear();if(pending){error="Finish the current project edit before duplicating an actor.";return -1;}
    try{
        if(!lane_id||!validate_scene(state,error)){if(error.empty())error="Choose an available actor.";return -1;}
        int index=-1;for(int i=0;i<(int)state.lanes.size();++i)if(state.lanes[i].id==lane_id)index=i;
        if(index<0){error="The actor is unavailable.";return -1;}
        if(state.lanes.size()>=32){error="This project has reached its 32-actor limit.";return -1;}
        int start=scene_lane_start(state,index);if(start<0||start>100000){error="The independent actor start exceeds 100000 ticks. Reduce the source timing before duplicating.";return -1;}
        auto &original=state.lanes[index];uint64_t needed=1+original.frames.size();
        size_t frames=0,pieces=0,extra_pieces=0;for(auto &actor:state.lanes){frames+=actor.frames.size();for(auto &frame:actor.frames)pieces+=frame.pieces.size();}for(auto &frame:original.frames)extra_pieces+=frame.pieces.size();
        if(frames+original.frames.size()>8192||pieces+extra_pieces>65536){error="Duplicating this actor exceeds the project's frame/reference limits.";return -1;}
        if(next_entity_id>INT_MAX||needed>(uint64_t)INT_MAX-next_entity_id+1){error="The project has exhausted its actor/frame identities.";return -1;}
        SceneState candidate=state;SceneLane copy=original;uint64_t next=next_entity_id;
        copy.id=next++;for(auto &frame:copy.frames)frame.id=next++;
        // Classic duplication keeps the schedule but drops the spawn link.
        // Capture a Studio follower's resolved start so its copy initially has
        // the same placement and motion, then runs independently.
        copy.delay=start;copy.trigger_lane=copy.trigger_frame=0;
        size_t length=std::min<size_t>(copy.name.size(),121);while(length<copy.name.size()&&length&&((unsigned char)copy.name[length]&0xc0)==0x80)--length;
        copy.name.resize(length);copy.name+=" (copy)";
        // Existing followers continue to refer to the original occurrence;
        // duplicating an actor does not duplicate its surrounding effect graph.
        candidate.lanes.insert(candidate.lanes.begin()+index+1,std::move(copy));
        if(!validate_scene(candidate,error))return -1;
        return install_scene_review(std::move(candidate),next,index+1,"Duplicate actor",error);
    }catch(const std::bad_alloc &){error="Not enough memory to duplicate the actor. Project and history are unchanged.";return -1;}
}
int SceneProject::move_actor(uint64_t lane_id,int direction,std::string &error){
    error.clear();if(pending){error="Finish the current project edit before moving an actor.";return -1;}
    try{
        if(!lane_id||(direction!=-1&&direction!=1)||!validate_scene(state,error)){if(error.empty())error="Choose an available actor and a forward/backward direction.";return -1;}
        int index=-1;for(int i=0;i<(int)state.lanes.size();++i)if(state.lanes[i].id==lane_id)index=i;
        if(index<0){error="The actor is unavailable.";return -1;}int target=index+direction;
        if(target<0||target>=(int)state.lanes.size())return index;
        SceneState candidate=state;std::swap(candidate.lanes[index],candidate.lanes[target]);
        return install_scene_review(std::move(candidate),next_entity_id,target,direction>0?"Bring actor forward":"Send actor backward",error);
    }catch(const std::bad_alloc &){error="Not enough memory to move the actor. Project and history are unchanged.";return -1;}
}
}
