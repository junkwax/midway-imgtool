#include "studio_subframe_swap.h"
#include "studio_layers.h"
#include "img_util.h"
#include "anipoint.h"
#include <algorithm>
#include <climits>
#include <new>
#include <stdexcept>
namespace studio {
bool plan_subframe_swap(const SceneProject &project,uint64_t lane_id,uint64_t frame_id,int tick,const SceneSource &source,SubframeSwapPlan &out,std::string &error){
    error.clear();if(tick<0||tick>99999||!lane_id||!frame_id||!validate_scene(project.state,error)){if(error.empty())error="Choose a parent frame and an actor tick from 0 to 99999.";return false;}
    try{
        SubframeSwapPlan p;p.owner=&project;p.lane=lane_id;p.frame=frame_id;p.tick=tick;
        for(int i=0;i<(int)project.state.lanes.size();++i)if(project.state.lanes[i].id==lane_id)p.lane_index=i;
        if(p.lane_index<0){error="The actor is unavailable.";return false;}auto &lane=project.state.lanes[p.lane_index];
        for(int f=0;f<(int)lane.frames.size();++f)if(lane.frames[f].id==frame_id)p.parent_index=f;
        if(p.parent_index<0){error="The parent frame is unavailable.";return false;}auto &parent=lane.frames[p.parent_index];
        if(parent.pieces.size()!=1){error="Choose a parent-only frame. This frame already contains multiple pieces.";return false;}
        if(lane.stop_tick&&tick>lane.stop_tick){error="The actor freezes before this swap. Choose an earlier tick or remove its freeze.";return false;}
        auto &ref=parent.pieces[0];auto art=source(project.state.assets[ref.asset].runtime_doc);int index=art?art->sprite_index(ref.runtime_id):-1;auto im=art?art->sprite(index):nullptr;
        if(!im){error="The parent IMG is unavailable. Rebind the project source.";return false;}
        size_t budget=0;if(!capture_subframe_family(*art,index,budget,p.sprites,error))return false;
        auto name=img_name_string(im);p.before=project.state;p.after=p.before;auto &target=p.after.lanes[p.lane_index];
        std::vector<SceneSprite> pieces;for(size_t i=1;i<p.sprites.size();++i)pieces.push_back(scene_sprite_reference(*art,ref.asset,p.sprites[i].index));
        p.child_index=p.parent_index+1;
        if(p.child_index<(int)lane.frames.size()){auto &next=lane.frames[p.child_index];bool matching_start=next.visible_from==parent.visible_until||(parent.visible_from==1&&parent.visible_until==1&&next.visible_from==0&&next.independent_timing);bool match=next.ticks==1&&!next.dual&&next.visible_until==0&&next.scheduled()&&matching_start&&next.pieces.size()==pieces.size();for(size_t j=0;match&&j<pieces.size();++j)match=next.pieces[j].asset==pieces[j].asset&&next.pieces[j].runtime_id==pieces[j].runtime_id;p.replaced=match;}
        if(!p.replaced&&target.frames.size()>=4096){error="This actor has reached its 4096-frame limit.";return false;}
        int sequential_start=scene_frame_start(lane,p.parent_index)-lane.delay;
        bool old_zero=p.replaced&&parent.visible_from==1&&parent.visible_until==1&&lane.frames[p.child_index].independent_timing&&lane.frames[p.child_index].visible_from==0;
        int motion_start=old_zero?0:parent.scheduled()?parent.visible_from:sequential_start;int elapsed=std::max(0,tick-motion_start);
        SceneFrame child;child.name=name+" / subframes";child.ticks=1;child.visible_from=tick;child.independent_timing=tick==0;child.flip_x=parent.flip_x;child.flip_y=parent.flip_y;child.z=parent.z;child.pieces=std::move(pieces);
        child.x=subframe_motion_offset(parent.x,parent.vx,elapsed,parent.cap_x,lane.flip_x!=parent.flip_x);child.y=subframe_motion_offset(parent.y,parent.vy,elapsed,parent.cap_y,parent.flip_y);
        if(std::abs((long long)child.x)>4096||std::abs((long long)child.y)>4096){error="The handoff position exceeds +/-4096 source pixels. Reduce motion or choose an earlier tick.";return false;}
        auto remaining=[&](int speed,int cap,int &v,int &c){v=speed;c=cap;if(cap){c=std::max(0,cap-(int)std::min((int64_t)cap,std::abs((int64_t)speed*elapsed)));if(!c)v=0;}};remaining(parent.vx,parent.cap_x,child.vx,child.cap_x);remaining(parent.vy,parent.cap_y,child.vy,child.cap_y);
        if(p.replaced)child.id=target.frames[p.child_index].id;
        auto &head=target.frames[p.parent_index];if(tick>sequential_start)head.ticks=std::clamp(tick-sequential_start,1,120);
        if(!tick){head.visible_from=head.visible_until=1;}else{if(old_zero||head.visible_from>=tick)head.visible_from=0;head.visible_until=tick;}
        if(p.replaced)target.frames[p.child_index]=std::move(child);else target.frames.insert(target.frames.begin()+p.child_index,std::move(child));
        p.global_tick=scene_lane_start(p.after,p.lane_index)+tick;
        if(!validate_scene(p.after,error))return false;
        out=std::move(p);return true;
    }catch(const std::bad_alloc &){error="Not enough memory to review the subframe swap. The project is unchanged.";return false;}catch(const std::runtime_error &e){error=e.what();return false;}
}
int SceneProject::apply_subframe_swap(const SubframeSwapPlan &reviewed,const SceneSource &source,std::string &error){
    error.clear();if(pending||reviewed.owner!=this){error="The project changed or an edit is pending. Reopen the subframe review.";return -1;}
    try{
        if(!same(state,reviewed.before)){error="The project changed. Reopen the subframe review.";return -1;}
        SubframeSwapPlan p;if(!plan_subframe_swap(*this,reviewed.lane,reviewed.frame,reviewed.tick,source,p,error))return -1;
        bool equal=same(p.after,reviewed.after)&&p.replaced==reviewed.replaced&&p.child_index==reviewed.child_index&&p.sprites.size()==reviewed.sprites.size();for(size_t i=0;equal&&i<p.sprites.size();++i)equal=same_subframe_snapshot(p.sprites[i],reviewed.sprites[i]);if(!equal){error="The source images or reviewed swap output changed. Reopen the review.";return -1;}
        uint64_t next_id=next_entity_id;if(!p.replaced){if(next_id>INT_MAX){error="The project has exhausted its frame identities.";return -1;}p.after.lanes[p.lane_index].frames[p.child_index].id=next_id++;}
        if(!validate_scene(p.after,error))return -1;
        return install_scene_review(std::move(p.after),next_id,p.child_index,"Swap with subframes",error);
    }catch(const std::bad_alloc &){error="Not enough memory to apply the subframe swap. Project/history are unchanged.";return -1;}
}
}
