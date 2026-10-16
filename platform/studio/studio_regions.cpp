#include "studio_regions.h"
#include "body_split.h"
#include "anipoint.h"
#include <algorithm>
#include <set>
#include <cctype>
namespace studio {
namespace {
std::string uppercase(std::string s){for(char &c:s)c=(char)std::toupper((unsigned char)c);return s;}
bool name_valid(const std::string &s){return !s.empty()&&s.size()<=15&&std::all_of(s.begin(),s.end(),[](unsigned char c){return c>=33&&c<=126;});}
size_t bytes(int w,int h){return (((size_t)w+3)&~size_t(3))*h;}
}
std::vector<RegionRequest> propose_body_regions(const IMG &im,bool &fallback){
    fallback=false;BodySplitPlan plan;std::vector<RegionRequest> out;
    if(!im.data_p||im.w<1||im.h<1||im.w>8192||im.h>8192||(size_t)im.w*im.h>4*1024*1024)return out;
    if(!BuildBodySplitPlan((const unsigned char *)im.data_p,im.w,im.h,(im.w+3)&~3,&plan))return out;
    fallback=plan.used_proportional_fallback;
    for(int i=0;i<BodyPart_Count;++i){const auto &r=plan.parts[i];char name[16];BodyPartChildName(im.n_s,i,name,sizeof name);auto selection=rectangle_selection(im.w,im.h,r.x,r.y,r.w,r.h);
        // An undetected arm still gets an editable starting box, initially off.
        if(!selection.valid&&(i==BodyPart_ArmL||i==BodyPart_ArmR)){int w=std::max(1,plan.content_w/4),x=i==BodyPart_ArmL?plan.content_x:plan.content_x+plan.content_w-w;selection=rectangle_selection(im.w,im.h,x,plan.neck_y,w,std::max(1,plan.waist_y-plan.neck_y));}
        out.push_back({BodyPartName(i),name,std::move(selection),r.enabled});}
    return out;
}
bool build_region_plan(const Artwork &art,int index,const std::vector<RegionRequest> &requests,bool trim,bool cut,RegionPlan &out,std::string &error){
    out={};error.clear();auto im=art.sprite(index);
    if(!im||!im->data_p||im->w<1||im->h<1||im->w>8192||im->h>8192||(size_t)im->w*im->h>4*1024*1024||requests.empty()||requests.size()>32){error="Choose up to 32 regions on a valid source sprite.";return false;}
    if(im->layer_p){error="Flatten or remove this overlay before extracting sprite regions.";return false;}
    auto pal=art.palette(im->palnum);if(!pal||!pal->data_p||pal->numc<1||pal->numc>256){error="The source palette is unavailable.";return false;}
    RegionPlan plan;plan.owner=&art;plan.generation=art.generation();plan.source=art.sprite_id(index);plan.requests=requests;plan.trim=trim;plan.cut=cut;
    std::set<std::string> names;size_t decoded=0;for(int i=0;i<art.count();++i){auto p=art.sprite(i);names.insert(uppercase(p->n_s));decoded+=bytes(p->w,p->h);}
    std::vector<unsigned char> coverage((size_t)im->w*im->h);int stride=(im->w+3)&~3;auto source=(const unsigned char *)im->data_p;
    for(const auto &r:requests){if(!r.enabled)continue;const auto &s=r.selection;
        if(!s.valid||!s.fits(im->w,im->h)||!name_valid(r.name)){error="Each enabled region needs an in-bounds selection and a name of 1–15 printable characters.";return false;}
        int x0=s.x,y0=s.y,x1=s.x+s.w,y1=s.y+s.h,opaque=0;
        if(trim){x0=im->w;y0=im->h;x1=y1=0;}
        for(int y=s.y;y<s.y+s.h;++y)for(int x=s.x;x<s.x+s.w;++x)if(s.contains(x,y)){
            auto value=source[y*stride+x];if(value>=pal->numc){error="A selected pixel is outside its palette.";return false;}
            auto &covered=coverage[(size_t)y*im->w+x];if(!covered){++plan.selected_pixels;if(value)++plan.opaque_pixels;}else if(covered==1&&value)++plan.overlap_pixels;if(covered<2)++covered;
            if(value){++opaque;if(trim){x0=std::min(x0,x);y0=std::min(y0,y);x1=std::max(x1,x+1);y1=std::max(y1,y+1);}}
        }
        if(!opaque){error="Region '"+r.label+"' contains no visible pixels. Disable it or adjust its bounds.";return false;}
        RegionChild child;child.x=x0;child.y=y0;child.w=x1-x0;child.h=y1-y0;child.anix=(short)im->anix-x0;child.aniy=(short)im->aniy-y0;
        if(child.anix<-32768||child.anix>32767||child.aniy<-32768||child.aniy>32767){error="An extracted anchor would exceed signed 16-bit coordinates.";return false;}
        child.name=r.name;
        for(int suffix=1;names.count(uppercase(child.name));++suffix){std::string tail="_COPY"+std::to_string(suffix);child.name=r.name.substr(0,15-tail.size())+tail;}
        names.insert(uppercase(child.name));decoded+=bytes(child.w,child.h);
        if(decoded>128*1024*1024||art.count()+plan.children.size()+1>2000){error="Extraction would exceed the 2000-sprite or 128 MiB library limit.";return false;}
        child.pixels.assign(bytes(child.w,child.h),0);int dst_stride=(child.w+3)&~3;
        for(int y=0;y<child.h;++y)for(int x=0;x<child.w;++x)if(s.contains(x+x0,y+y0))child.pixels[y*dst_stride+x]=source[(y+y0)*stride+x+x0];
        plan.children.push_back(std::move(child));
    }
    if(plan.children.empty()){error="Enable at least one region.";return false;}
    out=std::move(plan);return true;
}
int Artwork::extract_regions(const RegionPlan &input,std::string &error){
    error.clear();if(input.owner!=this||input.generation!=generation()){error="The library changed after review. Rebuild the extraction preview.";return -1;}
    int index=sprite_index(input.source);RegionPlan plan;
    // Revalidate requests, limits, names and pixel coordinates at apply time.
    if(!build_region_plan(*this,index,input.requests,input.trim,input.cut,plan,error))return -1;
    auto parent=sprite(index);auto staged=std::make_unique<Artwork>();IMG **tail=(IMG **)&staged->data.img_p;
    for(const auto &c:plan.children){auto child=(IMG *)calloc(1,sizeof(IMG));if(!child){error="Not enough memory to create extracted sprites.";return -1;}
        *tail=child;tail=(IMG **)&child->nxt_p;++staged->data.imgcnt;
        child->w=(unsigned short)c.w;child->h=(unsigned short)c.h;child->palnum=parent->palnum;child->anix=(unsigned short)c.anix;child->aniy=(unsigned short)c.aniy;
        child->opals=parent->opals;child->file_frm=child->file_pttblnum=0xffff;clear_secondary_anipoint(child);
        memcpy(child->n_s,c.name.data(),c.name.size());memcpy(child->src_filename,parent->src_filename,sizeof child->src_filename);
        child->data_p=malloc(c.pixels.size());if(!child->data_p){error="Not enough memory for extracted pixels.";return -1;}memcpy(child->data_p,c.pixels.data(),c.pixels.size());
        if(parent->opaltbl_p){child->opaltbl_p=malloc(16);if(!child->opaltbl_p){error="Not enough memory for alternate palettes.";return -1;}memcpy(child->opaltbl_p,parent->opaltbl_p,16);}
    }
    sprite_ids.reserve(sprite_ids.size()+plan.children.size());begin(plan.cut?"Cut regions into sprites":"Copy regions into sprites");int first=count();
    IMG **images=(IMG **)&data.img_p;while(*images)images=(IMG **)&(*images)->nxt_p;*images=(IMG *)staged->data.img_p;data.imgcnt+=staged->data.imgcnt;staged->data.img_p=nullptr;staged->data.imgcnt=0;
    for(size_t i=0;i<plan.children.size();++i)sprite_ids.push_back(next_sprite_id++);
    if(plan.cut){int stride=(parent->w+3)&~3;auto p=(unsigned char *)parent->data_p;for(const auto &r:plan.requests)if(r.enabled){const auto &s=r.selection;for(int y=s.y;y<s.y+s.h;++y)for(int x=s.x;x<s.x+s.w;++x)if(s.contains(x,y))p[y*stride+x]=0;}}
    selected_sprite_id=sprite_ids[first];++serial;++structure_serial;commit();return first;
}
}
