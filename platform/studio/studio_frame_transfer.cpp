#include "studio_project.h"
#include <algorithm>
#include <climits>
#include <iterator>
#include <set>
namespace studio {
int SceneProject::transfer_frames(uint64_t source,const std::vector<uint64_t> &frames,uint64_t destination,int insertion,bool move,std::string &error,const SceneState *reviewed){
    error.clear();if(pending){error="Finish the current project edit before transferring frames.";return -1;}
    try{
        if(reviewed&&!same(state,*reviewed)){error="The composition changed. Cancel and reopen the frame transfer review.";return -1;}
        if(!validate_scene(state,error))return -1;
        int src=-1,dst=-1;for(int i=0;i<(int)state.lanes.size();++i){if(source&&state.lanes[i].id==source)src=i;if(destination&&state.lanes[i].id==destination)dst=i;}
        if(src<0||dst<0||frames.empty()){error="Choose available source/destination actors and at least one frame.";return -1;}
        if(insertion<0||insertion>(int)state.lanes[dst].frames.size()){error="Choose an insertion point within the destination frame list.";return -1;}
        std::set<uint64_t> selected;for(auto id:frames)if(!id||!selected.insert(id).second){error="The selection contains a missing or repeated frame identity.";return -1;}
        size_t found=0,extra_pieces=0;int preceding=0;
        for(int i=0;i<(int)state.lanes[src].frames.size();++i)if(selected.count(state.lanes[src].frames[i].id)){++found;extra_pieces+=state.lanes[src].frames[i].pieces.size();if(i<insertion)++preceding;}
        if(found!=selected.size()){error="A selected frame is no longer in the source actor.";return -1;}
        if(move&&src!=dst&&found==state.lanes[src].frames.size()){error="Leave at least one frame in the source actor, or copy the selection instead.";return -1;}
        if(state.lanes[dst].frames.size()+((move&&src==dst)?0:found)>4096){error="The destination would exceed its 4096-frame limit.";return -1;}
        if(!move){size_t total_frames=0,total_pieces=0;for(auto &l:state.lanes){total_frames+=l.frames.size();for(auto &f:l.frames)total_pieces+=f.pieces.size();}
            if(total_frames+found>8192||total_pieces+extra_pieces>65536){error="Copying these frames exceeds the project's frame/reference limits.";return -1;}
            if(next_entity_id>INT_MAX||found>(uint64_t)INT_MAX-next_entity_id+1){error="The project has exhausted its frame identities.";return -1;}
        }
        SceneState candidate=state;std::vector<SceneFrame> transferred;transferred.reserve(found);uint64_t next=next_entity_id;
        for(auto &f:state.lanes[src].frames)if(selected.count(f.id)){transferred.push_back(f);if(!move)transferred.back().id=next++;}
        int at=insertion;if(move){if(src==dst)at-=preceding;auto &list=candidate.lanes[src].frames;list.erase(std::remove_if(list.begin(),list.end(),[&](const SceneFrame &f){return selected.count(f.id)!=0;}),list.end());}
        auto &list=candidate.lanes[dst].frames;list.insert(list.begin()+at,std::make_move_iterator(transferred.begin()),std::make_move_iterator(transferred.end()));
        if(move&&src!=dst)for(auto &l:candidate.lanes)if(l.trigger_lane==source&&selected.count(l.trigger_frame))l.trigger_lane=destination;
        // Adopting destination timing can introduce a dependency cycle. Refuse
        // the complete operation instead of silently detaching an effect.
        if(!validate_scene(candidate,error))return -1;
        return install_scene_review(std::move(candidate),next,at,move?"Move actor frames":"Copy actor frames",error);
    }catch(const std::bad_alloc &){error="Not enough memory to transfer frames. Project and history are unchanged.";return -1;}
}
}
