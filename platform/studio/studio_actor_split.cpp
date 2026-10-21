#include "studio_project.h"
#include <algorithm>
#include <climits>
#include <set>
namespace studio {
int SceneProject::split_actor_at(uint64_t lane_id,uint64_t frame_id,std::string &error){
    error.clear();if(pending){error="Finish the current project edit before splitting an actor.";return -1;}
    try{
        if(!lane_id||!frame_id||!validate_scene(state,error)){if(error.empty())error="Choose an available actor and frame.";return -1;}
        int index=-1,split=-1;for(int i=0;i<(int)state.lanes.size();++i)if(state.lanes[i].id==lane_id)index=i;
        if(index<0){error="The actor is unavailable.";return -1;}auto &original=state.lanes[index];
        for(int f=0;f<(int)original.frames.size();++f)if(original.frames[f].id==frame_id)split=f;
        if(split<=0){error="Choose a frame after the first frame; both actors need at least one frame.";return -1;}
        if(state.lanes.size()>=32){error="This project has reached its 32-actor limit.";return -1;}
        int start=scene_lane_start(state,index);if(start<0||start>100000){error="The independent actor start exceeds 100000 ticks. Reduce the source timing before splitting.";return -1;}
        if(next_entity_id>INT_MAX){error="The project has exhausted its actor identities.";return -1;}
        SceneState candidate=state;auto &head=candidate.lanes[index];SceneLane tail=head;
        tail.frames.erase(tail.frames.begin(),tail.frames.begin()+split);head.frames.erase(head.frames.begin()+split,head.frames.end());
        // Classic splits the list into two rows whose sequential clocks both
        // begin at the actor start. Explicit frame windows remain unchanged.
        // A Studio actor may itself follow another frame: freeze its current
        // start for the independent tail while retaining the head's link.
        tail.id=next_entity_id;tail.delay=start;tail.trigger_lane=tail.trigger_frame=0;
        size_t length=std::min<size_t>(tail.name.size(),120);while(length<tail.name.size()&&length&&((unsigned char)tail.name[length]&0xc0)==0x80)--length;
        tail.name.resize(length);tail.name+=" (split)";
        std::set<uint64_t> moved;for(auto &f:tail.frames)moved.insert(f.id);
        // Preserve the intended occurrence for existing effects when its frame
        // moves to another actor. Frame identities and piece order stay exact.
        for(auto &actor:candidate.lanes)if(actor.trigger_lane==lane_id&&moved.count(actor.trigger_frame))actor.trigger_lane=tail.id;
        candidate.lanes.insert(candidate.lanes.begin()+index+1,std::move(tail));
        if(!validate_scene(candidate,error))return -1;
        return install_scene_review(std::move(candidate),next_entity_id+1,index+1,"Split actor",error);
    }catch(const std::bad_alloc &){error="Not enough memory to split the actor. Project and history are unchanged.";return -1;}
}
}
