#include "studio_dummy_body.h"
#include "img_util.h"
#include <algorithm>
#include <climits>
#include <new>
namespace studio {
namespace {
std::string upper(std::string s){for(char &c:s)if(c>='a'&&c<='z')c-=32;return s;}
bool body_name(const std::string &raw,std::string &prefix,int &frame,int &kind){
    auto name=upper(raw);size_t pos=name.rfind("DECAPLEG"),length=8;kind=0;
    if(pos==std::string::npos){pos=name.rfind("DECAPTORSO");length=10;kind=1;}
    if(pos==std::string::npos){if(name.find("DECAPHEAD")!=std::string::npos)return false;pos=name.rfind("DECAP");length=5;kind=2;}
    if(pos==std::string::npos)return false;size_t end=pos+length;if(end==name.size()||name[end]<'0'||name[end]>'9')return false;
    int n=0;while(end<name.size()&&name[end]>='0'&&name[end]<='9'){n=std::min(8,n*10+name[end]-'0');++end;}
    if(n<1||n>7||(kind==2&&end!=name.size()))return false;prefix=name.substr(0,pos);frame=n-1;return true;
}
}
bool discover_dummy_bodies(const Artwork &art,std::vector<DummyBodyFamily> &out,std::string &error){
    error.clear();try{struct Candidate {DummyBodyFamily family;std::array<int,7> wrappers{};std::array<bool,7> leg{},torso{};std::array<std::vector<int>,7> pieces;};std::vector<Candidate> candidates;
        for(int i=0;i<art.count();++i){auto im=art.sprite(i);std::string prefix;int frame,kind;if(!body_name(img_name_string(im),prefix,frame,kind))continue;
            auto found=std::find_if(candidates.begin(),candidates.end(),[&](const Candidate &c){return c.family.prefix==prefix;});if(found==candidates.end()){candidates.emplace_back();found=candidates.end()-1;found->family.prefix=prefix;found->wrappers.fill(-1);}auto &c=*found;
            if(kind==2){if(c.wrappers[frame]>=0)c.family.error="Duplicate whole-body frame names are ambiguous. Rename or remove the duplicate first.";else c.wrappers[frame]=i;}
            else{c.pieces[frame].push_back(i);(kind==0?c.leg:c.torso)[frame]=true;}
        }
        std::vector<DummyBodyFamily> result;for(auto &c:candidates){int wrappers=0,paired=0;for(int f=0;f<7;++f){wrappers+=c.wrappers[f]>=0;paired+=c.leg[f]&&c.torso[f];c.family.wrapper[f]=c.wrappers[f]>=0;
                if(c.wrappers[f]>=0)c.family.pieces[f]={c.wrappers[f]};else{auto &pieces=c.pieces[f];std::stable_sort(pieces.begin(),pieces.end(),[&](int a,int b){std::string pa,pb;int fa,fb,ka,kb;body_name(img_name_string(art.sprite(a)),pa,fa,ka);body_name(img_name_string(art.sprite(b)),pb,fb,kb);return ka!=kb?ka<kb:img_name_string(art.sprite(a))<img_name_string(art.sprite(b));});c.family.pieces[f]=std::move(pieces);}
                if(c.family.pieces[f].size()>32)c.family.error="A body frame exceeds the 32-piece composition limit.";
            }c.family.score=wrappers*10+paired;if(wrappers<7&&paired<7&&c.family.error.empty())c.family.error="Needs DECAP1-7 whole frames, or both DECAPLEG and DECAPTORSO for all seven frames.";result.push_back(std::move(c.family));}
        out=std::move(result);return true;
    }catch(const std::bad_alloc &){error="Not enough memory to discover dummy bodies.";return false;}
}
bool plan_dummy_body(const SceneProject &project,const DummyBodyOptions &o,const SceneSource &source,DummyBodyPlan &out,std::string &error){
    error.clear();try{if(!validate_scene(project.state,error))return false;auto art=o.doc?source(o.doc):nullptr;if(!art){error="Choose an available IMG body source.";return false;}std::vector<DummyBodyFamily> families;if(!discover_dummy_bodies(*art,families,error))return false;
        auto family=std::find_if(families.begin(),families.end(),[&](const DummyBodyFamily &f){return f.prefix==o.prefix;});if(family==families.end()){error="Choose a DECAP body family in this IMG.";return false;}if(!family->error.empty()){error=family->error;return false;}
        DummyBodyPlan p;p.owner=&project;p.options=o;p.family=*family;p.before=project.state;p.after=p.before;size_t budget=0;for(auto &frame:family->pieces)for(int i:frame){SubframeSpriteSnapshot snap;if(!capture_subframe_sprite(*art,i,budget,snap,error))return false;p.sprites.push_back(std::move(snap));}
        int asset=-1;for(int i=0;i<(int)p.after.assets.size();++i)if(p.after.assets[i].runtime_doc==o.doc){asset=i;break;}if(asset<0){if(p.after.assets.size()>=64){error="This project has reached its 64-source limit.";return false;}asset=(int)p.after.assets.size();p.after.assets.push_back({"","","",o.doc});}
        SceneLane lane;if(o.replace_lane){for(int i=0;i<(int)p.after.lanes.size();++i)if(p.after.lanes[i].id==o.replace_lane)p.lane_index=i;if(p.lane_index<0){error="The actor to reset is unavailable.";return false;}lane=p.after.lanes[p.lane_index];for(size_t i=13;i<lane.frames.size();++i)if(scene_uses_frame(p.before,lane.frames[i].id)){error="An effect follows a frame that reset would remove. Detach that effect first.";return false;}}
        else{if(p.after.lanes.size()>=32){error="This project has reached its 32-actor limit.";return false;}p.lane_index=(int)p.after.lanes.size();lane.name="Dummy body ["+o.prefix+"]";lane.flip_x=o.flip_x;lane.loop=false;lane.hold_end=true;}
        auto old=std::move(lane.frames);lane.frames.clear();for(int i=0;i<13;++i){int f=dummy_body_order[i]-1;SceneFrame frame;frame.name=o.prefix+"DECAP"+std::to_string(f+1);frame.ticks=dummy_body_holds[i];if(i<(int)old.size()){frame.id=old[i].id;frame.flip_x=old[i].flip_x;frame.flip_y=old[i].flip_y;frame.z=old[i].z;}for(int index:family->pieces[f])frame.pieces.push_back(scene_sprite_reference(*art,asset,index));p.duration+=frame.ticks;lane.frames.push_back(std::move(frame));}
        if(o.replace_lane)p.after.lanes[p.lane_index]=std::move(lane);else p.after.lanes.push_back(std::move(lane));p.start_tick=scene_lane_start(p.after,p.lane_index);if(!validate_scene(p.after,error))return false;out=std::move(p);return true;
    }catch(const std::bad_alloc &){error="Not enough memory to review the dummy body. Project/artwork are unchanged.";return false;}
}
int SceneProject::apply_dummy_body(const DummyBodyPlan &reviewed,const SceneSource &source,std::string &error){
    error.clear();if(pending||reviewed.owner!=this){error="An edit is pending or this body review belongs to another project.";return -1;}try{if(!same(state,reviewed.before)){error="The project changed. Reopen the body review.";return -1;}DummyBodyPlan p;if(!plan_dummy_body(*this,reviewed.options,source,p,error))return -1;
        bool equal=same(p.after,reviewed.after)&&p.lane_index==reviewed.lane_index&&p.start_tick==reviewed.start_tick&&p.duration==reviewed.duration&&p.sprites.size()==reviewed.sprites.size()&&p.family.prefix==reviewed.family.prefix&&p.family.error==reviewed.family.error&&p.family.pieces==reviewed.family.pieces&&p.family.wrapper==reviewed.family.wrapper&&p.family.score==reviewed.family.score;for(size_t i=0;equal&&i<p.sprites.size();++i)equal=same_subframe_snapshot(p.sprites[i],reviewed.sprites[i]);
        if(!equal){error="Body source images or reviewed output changed. Reopen the review.";return -1;}uint64_t next=next_entity_id;auto &lane=p.after.lanes[p.lane_index];auto identify=[&](uint64_t &id){if(!id){if(next>INT_MAX)return false;id=next++;}return true;};if(!identify(lane.id)){error="The project has exhausted its actor identities.";return -1;}for(auto &frame:lane.frames)if(!identify(frame.id)){error="The project has exhausted its frame identities.";return -1;}if(!validate_scene(p.after,error))return -1;
        return install_scene_review(std::move(p.after),next,p.lane_index,"Build dummy body",error);
    }catch(const std::bad_alloc &){error="Not enough memory to apply the body review. Project/history are unchanged.";return -1;}
}
}
