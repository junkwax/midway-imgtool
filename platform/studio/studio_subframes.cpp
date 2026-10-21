#include "studio_subframes.h"
#include "studio_layers.h"
#include "img_util.h"
#include "anipoint.h"
#include <algorithm>
#include <new>
#include <stdexcept>
namespace studio {
namespace {
std::string group(const IMG &im){return im.src_filename[0]?std::string(im.src_filename,strnlen(im.src_filename,16)):"Workspace";}
bool belongs(const std::string &name,const std::string &parent){std::string stem;return InferSubframeParentName(name.c_str())==parent||(strip_trailing_sequence_digits(name,&stem)&&stem==parent);}
}
bool capture_subframe_sprite(const Artwork &art,int index,size_t &budget,SubframeSpriteSnapshot &out,std::string &error){
    auto im=art.sprite(index);auto pal=im?art.palette(im->palnum):nullptr;
    if(!im||!im->data_p||!im->w||!im->h||im->w>8192||im->h>8192||(size_t)im->w*im->h>4u*1024u*1024u||!pal||!pal->data_p||pal->numc<1||pal->numc>256){error="A parent/subframe image or palette exceeds the review limits.";return false;}
    try{size_t pixels=(size_t)((im->w+3)&~3)*im->h,layer=checked_layer_bytes(*im),bytes=pixels+layer+pal->numc*2+16;
        if(budget>128u*1024u*1024u||bytes>128u*1024u*1024u-budget){error="Subframe review exceeds its 128 MiB source budget.";return false;}
        SubframeSpriteSnapshot snap;snap.owner=&art;snap.id=art.sprite_id(index);snap.index=index;memcpy(snap.fields.data(),im,sizeof *im);memcpy(snap.palette.data(),pal,sizeof *pal);
        snap.pixels.assign((unsigned char *)im->data_p,(unsigned char *)im->data_p+pixels);snap.colors.assign((unsigned char *)pal->data_p,(unsigned char *)pal->data_p+pal->numc*2);
        if(im->opaltbl_p)snap.alternate.assign((unsigned char *)im->opaltbl_p,(unsigned char *)im->opaltbl_p+16);
        if(im->layer_p)snap.layer.assign((unsigned char *)im->layer_p,(unsigned char *)im->layer_p+layer);
        out=std::move(snap);budget+=bytes;return true;
    }catch(const std::bad_alloc &){error="Not enough memory to capture subframes.";return false;}catch(const std::runtime_error &e){error=e.what();return false;}
}
bool capture_subframe_family(const Artwork &art,int index,size_t &budget,std::vector<SubframeSpriteSnapshot> &out,std::string &error){
    auto im=art.sprite(index);if(!im){error="The fine-subframe source is unavailable.";return false;}
    try{std::vector<int> children;auto name=img_name_string(im),src_group=group(*im);
        for(int i=0;i<art.count();++i){auto child=art.sprite(i);if(i!=index&&child->data_p&&child->w&&child->h&&group(*child)==src_group&&belongs(img_name_string(child),name))children.push_back(i);}
        if(children.empty()||children.size()>32){error="The parent needs 1-32 named subframes in the same source group.";return false;}
        std::vector<SubframeSpriteSnapshot> result;size_t staged_budget=budget;SubframeSpriteSnapshot parent;
        if(!capture_subframe_sprite(art,index,staged_budget,parent,error))return false;result.push_back(std::move(parent));
        for(int i:children){SubframeSpriteSnapshot child;if(!capture_subframe_sprite(art,i,staged_budget,child,error))return false;result.push_back(std::move(child));}
        out=std::move(result);budget=staged_budget;return true;
    }catch(const std::bad_alloc &){error="Not enough memory to capture the subframe family.";return false;}
}
bool same_subframe_snapshot(const SubframeSpriteSnapshot &a,const SubframeSpriteSnapshot &b){return a.owner==b.owner&&a.id==b.id&&a.index==b.index&&a.fields==b.fields&&a.palette==b.palette&&a.pixels==b.pixels&&a.colors==b.colors&&a.alternate==b.alternate&&a.layer==b.layer;}
int subframe_motion_offset(int value,int velocity,int elapsed,int cap,bool flip){int64_t travel=(int64_t)velocity*elapsed;if(cap)travel=std::clamp(travel,-(int64_t)cap,(int64_t)cap);return (int)std::clamp((int64_t)value+travel,(int64_t)(flip?-32768:-32767),(int64_t)(flip?32767:32768));}
size_t scene_review_bytes(const SceneState &s){size_t n=sizeof s+s.assets.capacity()*sizeof(SceneAsset)+s.lanes.capacity()*sizeof(SceneLane);for(auto &a:s.assets)n+=a.path.capacity()+a.absolute_path.capacity()+a.digest.capacity();for(auto &l:s.lanes){n+=l.name.capacity()+l.frames.capacity()*sizeof(SceneFrame);for(auto &f:l.frames){n+=f.name.capacity()+f.pieces.capacity()*sizeof(SceneSprite);for(auto &p:f.pieces)n+=p.name.capacity()+p.key.capacity();}}return n;}
int SceneProject::install_scene_review(SceneState &&candidate,uint64_t next_id,int result,const char *label,std::string &error){
    try{if(same(state,candidate))return result;Snapshot before{state,revision,label};past.reserve(past.size()+1);
        state=std::move(candidate);past.push_back(std::move(before));future.clear();next_entity_id=next_id;revision=next_revision++;
        size_t budget=0;for(auto &snap:past)budget+=scene_review_bytes(snap.state);while(past.size()>1&&(past.size()>64||budget>64u*1024u*1024u)){budget-=scene_review_bytes(past.front().state);past.erase(past.begin());}return result;
    }catch(const std::bad_alloc &){error="Not enough memory to apply the scene review. Project/history are unchanged.";return -1;}
}
}
