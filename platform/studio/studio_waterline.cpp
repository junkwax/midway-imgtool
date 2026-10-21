#include "studio_waterline.h"
#include <algorithm>
#include <climits>
#include <iterator>
#include <new>
namespace studio {
namespace {
int crossing_tick(const SceneState &s,const SceneLane &lane,const SceneFrame &f,const std::vector<IMG> &images,int start,int line,bool down){
    int last=lane.stop_tick?std::min(lane.stop_tick,99999):99999;
    int end=std::min(9999,last-start);
    for(int elapsed=1;elapsed<=end;++elapsed){int y=subframe_motion_offset(f.y,f.vy,elapsed,f.cap_y,f.flip_y);
        ScenePose pose;pose.y=s.origin_y+lane.y+y*lane.magnification;pose.flip_y=f.flip_y;pose.magnification=lane.magnification;
        int top=INT_MAX,bottom=INT_MIN;for(auto &im:images){auto bounds=scene_pose_placement(im,pose,s.convention);top=std::min(top,bounds.y);bottom=std::max(bottom,bounds.y+bounds.h);}
        if(down?bottom>line:top<line)return start+elapsed;
    }return 0;
}
}
bool plan_waterline(const SceneProject &project,const WaterlineOptions &o,const SceneSource &source,WaterlinePlan &out,std::string &error){
    error.clear();try{
        if(!o.lane||!o.frame||!o.fine_doc||!o.fine_sprite||o.y<1||o.y>9999){error="Choose an actor/frame, a fine-subframe parent and a waterline from 1 to 9999.";return false;}
        if(!validate_scene(project.state,error))return false;
        WaterlinePlan p;p.owner=&project;p.options=o;
        for(int i=0;i<(int)project.state.lanes.size();++i)if(project.state.lanes[i].id==o.lane)p.lane_index=i;
        if(p.lane_index<0){error="The waterline actor is unavailable.";return false;}auto &lane=project.state.lanes[p.lane_index];
        for(int i=0;i<(int)lane.frames.size();++i)if(lane.frames[i].id==o.frame)p.parent_index=i;
        if(p.parent_index<0){error="The waterline frame is unavailable.";return false;}auto &parent=lane.frames[p.parent_index];
        if(!parent.vy){error="Set vertical motion on the selected frame before cutting at a waterline.";return false;}p.down=parent.vy>0;
        size_t budget=0;std::vector<IMG> parent_images;
        for(auto &ref:parent.pieces){auto art=source(project.state.assets[ref.asset].runtime_doc);int index=art?art->sprite_index(ref.runtime_id):-1;SubframeSpriteSnapshot snap;if(!art||!capture_subframe_sprite(*art,index,budget,snap,error)){if(error.empty())error="A parent image is unavailable.";return false;}IMG im;memcpy(&im,snap.fields.data(),sizeof im);parent_images.push_back(im);p.sprites.push_back(std::move(snap));}
        auto fine_art=source(o.fine_doc);int fine_index=fine_art?fine_art->sprite_index(o.fine_sprite):-1;
        if(!fine_art||fine_index<0){error="The fine-subframe source is unavailable.";return false;}
        std::vector<SubframeSpriteSnapshot> family;if(!capture_subframe_family(*fine_art,fine_index,budget,family,error))return false;
        p.fine_begin=(int)p.sprites.size();for(auto &snap:family)p.sprites.push_back(std::move(snap));
        int start=parent.scheduled()?parent.visible_from:scene_frame_start(lane,p.parent_index)-lane.delay;
        p.cut_tick=crossing_tick(project.state,lane,parent,parent_images,start,o.y,p.down);
        if(!p.cut_tick){error="The parent never crosses this line within 9999 actor ticks, its travel cap and freeze. Adjust motion, the line or freeze.";return false;}
        p.before=project.state;p.after=p.before;int fine_asset=-1;
        for(int i=0;i<(int)p.after.assets.size();++i)if(p.after.assets[i].runtime_doc==o.fine_doc){fine_asset=i;break;}
        if(fine_asset<0){if(p.after.assets.size()>=64){error="This project has reached its 64-source limit.";return false;}fine_asset=(int)p.after.assets.size();p.after.assets.push_back({"","","",o.fine_doc});}
        auto &target=p.after.lanes[p.lane_index];p.child_index=p.parent_index+1;int count=(int)family.size()-1;
        // Only a complete adjacent matching run is replaced; unrelated frames remain.
        p.replaced=p.child_index+count<=(int)lane.frames.size();for(int j=0;p.replaced&&j<count;++j){auto &old=lane.frames[p.child_index+j];auto &snap=p.sprites[p.fine_begin+1+j];p.replaced=old.ticks==1&&!old.dual&&old.scheduled()&&old.visible_from==parent.visible_until&&old.pieces.size()==1&&old.pieces[0].asset==fine_asset&&old.pieces[0].runtime_id==snap.id;}
        if(!p.replaced&&target.frames.size()+count>4096){error="The fine-piece run would exceed this actor's 4096-frame limit.";return false;}
        int elapsed=p.cut_tick-start;SceneFrame base;base.ticks=1;base.visible_from=p.cut_tick;base.flip_x=parent.flip_x;base.flip_y=parent.flip_y;base.z=parent.z;
        base.x=subframe_motion_offset(parent.x,parent.vx,elapsed,parent.cap_x,lane.flip_x!=parent.flip_x);base.y=subframe_motion_offset(parent.y,parent.vy,elapsed,parent.cap_y,parent.flip_y);
        if(std::abs((long long)base.x)>4096||std::abs((long long)base.y)>4096){error="The handoff position exceeds +/-4096 source pixels. Reduce motion or move the line closer.";return false;}
        auto remaining=[&](int speed,int cap,int &v,int &c){v=speed;c=cap;if(cap){c=std::max(0,cap-(int)std::min((int64_t)cap,std::abs((int64_t)speed*elapsed)));if(!c)v=0;}};
        remaining(parent.vx,parent.cap_x,base.vx,base.cap_x);remaining(parent.vy,parent.cap_y,base.vy,base.cap_y);
        std::vector<SceneFrame> pieces;for(int j=0;j<count;++j){auto &snap=p.sprites[p.fine_begin+1+j];IMG im;memcpy(&im,snap.fields.data(),sizeof im);SceneFrame child=base;child.name=im.n_s;child.pieces={scene_sprite_reference(*fine_art,fine_asset,snap.index)};child.visible_until=crossing_tick(p.after,lane,child,{im},p.cut_tick,o.y,p.down);if(p.replaced)child.id=lane.frames[p.child_index+j].id;p.rows.push_back({snap.id,child.name,child.visible_until});pieces.push_back(std::move(child));}
        auto &head=target.frames[p.parent_index];head.visible_from=start;head.visible_until=p.cut_tick;
        if(p.replaced)for(int j=0;j<count;++j)target.frames[p.child_index+j]=std::move(pieces[j]);else target.frames.insert(target.frames.begin()+p.child_index,std::make_move_iterator(pieces.begin()),std::make_move_iterator(pieces.end()));
        p.global_tick=scene_lane_start(p.after,p.lane_index)+p.cut_tick;
        if(!validate_scene(p.after,error))return false;out=std::move(p);return true;
    }catch(const std::bad_alloc &){error="Not enough memory to review waterline cutting. Project and artwork are unchanged.";return false;}
}
int SceneProject::apply_waterline(const WaterlinePlan &reviewed,const SceneSource &source,std::string &error){
    error.clear();if(pending||reviewed.owner!=this){error="An edit is pending or this waterline review belongs to another project.";return -1;}
    try{if(!same(state,reviewed.before)){error="The project changed. Reopen the waterline review.";return -1;}WaterlinePlan p;if(!plan_waterline(*this,reviewed.options,source,p,error))return -1;
        bool equal=same(p.after,reviewed.after)&&p.cut_tick==reviewed.cut_tick&&p.global_tick==reviewed.global_tick&&p.child_index==reviewed.child_index&&p.replaced==reviewed.replaced&&p.down==reviewed.down&&p.rows.size()==reviewed.rows.size()&&p.sprites.size()==reviewed.sprites.size();
        for(size_t i=0;equal&&i<p.rows.size();++i)equal=p.rows[i].sprite==reviewed.rows[i].sprite&&p.rows[i].name==reviewed.rows[i].name&&p.rows[i].hide_tick==reviewed.rows[i].hide_tick;
        for(size_t i=0;equal&&i<p.sprites.size();++i)equal=same_subframe_snapshot(p.sprites[i],reviewed.sprites[i]);
        if(!equal){error="Source images or reviewed waterline output changed. Reopen the review.";return -1;}
        uint64_t next_id=next_entity_id;if(!p.replaced)for(size_t j=0;j<p.rows.size();++j){if(next_id>INT_MAX){error="The project has exhausted its frame identities.";return -1;}p.after.lanes[p.lane_index].frames[p.child_index+j].id=next_id++;}
        if(!validate_scene(p.after,error))return -1;return install_scene_review(std::move(p.after),next_id,p.child_index,"Cut at waterline",error);
    }catch(const std::bad_alloc &){error="Not enough memory to apply waterline cutting. Project/history are unchanged.";return -1;}
}
}
