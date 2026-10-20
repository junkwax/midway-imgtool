#include "studio_overlay_frame.h"
#include <algorithm>
#include <cctype>
#include <new>
namespace studio {
namespace {
size_t pixel_bytes(int w,int h){return (((size_t)w+3)&~size_t(3))*h;}
bool equal_name(const std::string &a,const char *b){size_t n=0;while(n<16&&b[n])++n;return a.size()==n&&std::equal(a.begin(),a.end(),b,[](unsigned char x,unsigned char y){return std::toupper(x)==std::toupper(y);});}
}
bool plan_overlay_frame(const Artwork &art,int index,const PixelSelection &selection,bool split,const std::string &name,OverlayFramePlan &out,std::string &error){
    error.clear();auto im=art.sprite(index);
    if(!im||!im->data_p||!im->w||!im->h||im->w>8192||im->h>8192||(size_t)im->w*im->h>4u*1024u*1024u||!selection.valid||!selection.fits(im->w,im->h)){error="Select pixels on a valid sprite before creating an overlay frame.";return false;}
    if(im->layer_p){error="Flatten or remove the pasted overlay before creating an overlay frame.";return false;}
    auto pal=art.palette(im->palnum);if(!pal||!pal->data_p||pal->numc<1||pal->numc>256){error="The source palette is unavailable.";return false;}
    if(art.count()>=2000){error="Creating this frame would exceed the 2000-sprite limit.";return false;}
    size_t total=pixel_bytes(im->w,im->h);for(int i=0;i<art.count();++i){auto p=art.sprite(i);total+=pixel_bytes(p->w,p->h);if(total>128u*1024u*1024u){error="Creating this full-canvas frame would exceed the 128 MiB library limit.";return false;}}
    if(name.size()>15||(!name.empty()&&!std::all_of(name.begin(),name.end(),[](unsigned char c){return c>=33&&c<=126;}))){error="Use 1-15 printable characters without spaces for the frame name.";return false;}
    try{
        OverlayFramePlan p;p.owner=&art;p.generation=art.generation();p.source=art.sprite_id(index);p.selection=selection;p.split=split;p.requested_name=name;
        p.name=name.empty()?std::string(im->n_s,std::find(im->n_s,im->n_s+12,'\0'))+"OVR":name;
        auto taken=[&](const std::string &candidate){for(int i=0;i<art.count();++i)if(equal_name(candidate,art.sprite(i)->n_s))return true;return false;};
        if(taken(p.name)){std::string base=p.name;for(int n=1;;++n){std::string suffix="_COPY"+(n==1?std::string():std::to_string(n));p.name=base.substr(0,15-suffix.size())+suffix;if(!taken(p.name))break;}}
        memcpy(p.fields.data(),im,sizeof *im);memcpy(p.palette_fields.data(),pal,sizeof *pal);
        auto copy=[](void *from,size_t count,std::vector<unsigned char> &to){if(from)to.assign((unsigned char *)from,(unsigned char *)from+count);};
        copy(im->data_p,pixel_bytes(im->w,im->h),p.pixels);copy(pal->data_p,(size_t)pal->numc*2,p.colors);copy(im->opaltbl_p,16,p.alternate);copy(im->pttbl_p,40,p.points);
        if(im->baseline_p){if(im->baseline_w>8192||im->baseline_h>8192||(size_t)im->baseline_w*im->baseline_h>4u*1024u*1024u){error="The source packing baseline exceeds the bounded review size.";return false;}copy(im->baseline_p,pixel_bytes(im->baseline_w,im->baseline_h),p.baseline);}
        p.remaining=p.pixels;p.child.assign(p.pixels.size(),0);int stride=(im->w+3)&~3;
        for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x){size_t at=(size_t)y*stride+x;auto c=p.pixels[at];if(c>=pal->numc){error="Repair pixels that reference an unknown palette index first.";return false;}if(c&&selection.contains(x,y)){p.child[at]=c;if(split)p.remaining[at]=0;++p.opaque;}}
        if(!p.opaque){error="No opaque selected pixels to copy or split.";return false;}
        out=std::move(p);return true;
    }catch(const std::bad_alloc &){error="Not enough memory to review this overlay frame. Artwork is unchanged.";return false;}
}
std::vector<unsigned char> render_overlay_frame(const OverlayFramePlan &p,int part){
    IMG im;memcpy(&im,p.fields.data(),sizeof im);auto &pixels=part==2?p.child:part==1?p.remaining:p.pixels;
    if(part<0||part>2||!im.w||!im.h||pixels.size()!=pixel_bytes(im.w,im.h))return {};
    std::vector<unsigned char> rgba((size_t)im.w*im.h*4);int stride=(im.w+3)&~3;
    for(int y=0;y<im.h;++y)for(int x=0;x<im.w;++x){int c=pixels[(size_t)y*stride+x];if((size_t)c*2+1>=p.colors.size())return {};auto dst=rgba.data()+((size_t)y*im.w+x)*4;pal_word_to_rgb8(p.colors.data()+c*2,dst,dst+1,dst+2);dst[3]=c?255:0;}return rgba;
}
int Artwork::create_overlay_frame(const OverlayFramePlan &reviewed,std::string &error){
    error.clear();if(pending||reviewed.owner!=this||reviewed.generation!=generation()){error="The library changed or an edit is pending. Reopen the overlay-frame review.";return -1;}
    try{
        OverlayFramePlan p;if(!plan_overlay_frame(*this,sprite_index(reviewed.source),reviewed.selection,reviewed.split,reviewed.requested_name,p,error))return -1;
        if(p.name!=reviewed.name||p.opaque!=reviewed.opaque||p.fields!=reviewed.fields||p.palette_fields!=reviewed.palette_fields||p.pixels!=reviewed.pixels||p.colors!=reviewed.colors||p.alternate!=reviewed.alternate||p.points!=reviewed.points||p.baseline!=reviewed.baseline||p.remaining!=reviewed.remaining||p.child!=reviewed.child){error="The source, frame name or reviewed output changed. Reopen the overlay-frame review.";return -1;}
        auto staged=std::make_unique<Artwork>();auto child=(IMG *)calloc(1,sizeof(IMG));if(!child)throw std::bad_alloc();staged->data.img_p=child;staged->data.imgcnt=1;
        auto src=sprite(sprite_index(p.source));child->w=src->w;child->h=src->h;child->palnum=src->palnum;child->flags=src->flags;
        child->anix=src->anix;child->aniy=src->aniy;child->anix2=src->anix2;child->aniy2=src->aniy2;child->aniz2=src->aniz2;child->opals=src->opals;
        memcpy(child->n_s,p.name.data(),p.name.size());memcpy(child->src_filename,src->src_filename,sizeof child->src_filename);child->src_filename[15]=0;
        // Classic starts a new record: frame number 0, no point table/baseline/layer.
        child->file_pttblnum=0xffff;child->data_p=malloc(p.child.size());if(!child->data_p)throw std::bad_alloc();memcpy(child->data_p,p.child.data(),p.child.size());
        if(!p.alternate.empty()){child->opaltbl_p=malloc(16);if(!child->opaltbl_p)throw std::bad_alloc();memcpy(child->opaltbl_p,p.alternate.data(),16);}
        State before=capture();before.label=p.split?"Split selection to overlay frame":"Copy selection to overlay frame";
        sprite_ids.reserve(sprite_ids.size()+1);past.reserve(past.size()+1);
        // Publication below performs no allocations; a failed review leaves Redo intact.
        int first=count();IMG **tail=(IMG **)&data.img_p;while(*tail)tail=(IMG **)&(*tail)->nxt_p;*tail=child;staged->data.img_p=nullptr;staged->data.imgcnt=0;++data.imgcnt;
        sprite_ids.push_back(next_sprite_id++);selected_sprite_id=sprite_ids.back();if(p.split)memcpy(src->data_p,p.remaining.data(),p.remaining.size());
        past.push_back(std::move(before));future.clear();revision=next_revision++;++serial;++structure_serial;trim_history();return first;
    }catch(const std::bad_alloc &){error="Not enough memory to create the overlay frame. Artwork and history are unchanged.";return -1;}
}
}
