#include "studio_layers.h"
#include "studio_selection_transform.h"
#include "palette_math.h"
#include <algorithm>
#include <stdexcept>
namespace studio {
size_t checked_layer_bytes(const IMG &im){
    auto l=(SpriteLayer *)im.layer_p;if(!l)return 0;
    if(l->w<1||l->h<1||l->w>8192||l->h>8192||(size_t)l->w*l->h>4*1024*1024||l->stride!=((l->w+3)&~3)||l->x < -32768||l->x>32767||l->y < -32768||l->y>32767)
        throw std::runtime_error("The overlay has invalid dimensions or placement.");
    return sizeof(SpriteLayer)+(size_t)l->stride*l->h;
}
unsigned char overlay_pixel(const IMG &im,int x,int y,unsigned char base){
    auto l=(SpriteLayer *)im.layer_p;
    if(l&&l->visible){long long sx=(long long)x-l->x,sy=(long long)y-l->y;
        if(sx>=0&&sy>=0&&sx<l->w&&sy<l->h){auto v=layer_pixels(l)[(size_t)sy*l->stride+(size_t)sx];if(v)return v;}}
    return base;
}
unsigned char visible_pixel(const IMG &im,int x,int y){return overlay_pixel(im,x,y,((const unsigned char *)im.data_p)[(size_t)y*((im.w+3)&~3)+x]);}
LayerCompositeScope::LayerCompositeScope(Document &doc){
    for(auto im=(IMG *)doc.img_p;im;im=(IMG *)im->nxt_p)if(im->layer_p){
        checked_layer_bytes(*im);Entry e{im,im->data_p,im->layer_p,{}};size_t n=(size_t)((im->w+3)&~3)*im->h;
        e.composite.assign((const unsigned char *)im->data_p,(const unsigned char *)im->data_p+n);
        if(img_layer(im)->visible)composite_layer_onto(img_layer(im),e.composite.data(),im->w,im->h,(im->w+3)&~3);entries.push_back(std::move(e));
    }
    for(auto &e:entries){e.image->data_p=e.composite.data();e.image->layer_p=nullptr;}
}
LayerCompositeScope::~LayerCompositeScope(){for(auto &e:entries){e.image->data_p=e.pixels;e.image->layer_p=e.layer;}}
bool Artwork::replace_layer(int index,void *replacement,bool flatten,std::string &error){
    std::unique_ptr<void,decltype(&free)> owned(replacement,free);error.clear();auto im=sprite(index);
    if(!im){error="Choose a sprite first.";return false;}
    if(pending){error="Finish the current edit before changing its overlay.";return false;}
    checked_layer_bytes(*im);
    if(flatten&&(!im->layer_p||!img_layer(im)->visible)){error="Show the overlay before flattening it, or remove it explicitly.";return false;}
    std::vector<unsigned char> pixels;
    if(flatten){auto base=(const unsigned char *)im->data_p;pixels.assign(base,base+(size_t)((im->w+3)&~3)*im->h);composite_layer_onto(img_layer(im),pixels.data(),im->w,im->h,(im->w+3)&~3);}
    State before=capture();before.label=flatten?"Flatten overlay":!replacement?"Remove overlay":!im->layer_p?"Paste as overlay":"Edit overlay";past.reserve(past.size()+1);
    if(flatten)memcpy(im->data_p,pixels.data(),pixels.size());
    free(im->layer_p);im->layer_p=owned.release();past.push_back(std::move(before));future.clear();revision=next_revision++;++serial;trim_history();return true;
}
bool Artwork::paste_layer(int index,const Clipboard &c,int x,int y,std::string &error){
    error.clear();auto im=sprite(index);if(!im){error="Choose a sprite first.";return false;}
    if(im->layer_p){error="Flatten or remove this sprite's existing overlay before adding another.";return false;}
    auto pal=palette(im->palnum);
    if(c.w<1||c.h<1||c.w>8192||c.h>8192||(size_t)c.w*c.h>4*1024*1024||c.pixels.size()!=(size_t)c.w*c.h||(!c.mask.empty()&&c.mask.size()!=c.pixels.size())||c.colors.empty()||c.colors.size()>256||!pal||!pal->data_p||pal->numc<2||pal->numc>256||x < -32768||x>32767||y < -32768||y>32767){error="The overlay clipboard, palette or position is invalid.";return false;}
    for(auto v:c.pixels)if(v>=c.colors.size()){error="The clipboard contains an unresolved palette index.";return false;}
    unsigned char map[256]={};bool same=c.colors.size()==pal->numc;for(int i=0;same&&i<pal->numc;++i)same=c.colors[i]==palette_word_at((const unsigned char *)pal->data_p,i);
    for(size_t i=1;i<c.colors.size();++i)map[i]=same?(unsigned char)i:nearest_palette_index_for_word(c.colors[i],pal);
    size_t total=layer_total_bytes(c.w,c.h);for(int i=0;i<count();++i)total+=checked_layer_bytes(*sprite(i));
    if(total>128*1024*1024){error="The library's overlays would exceed 128 MiB. Flatten or remove an existing overlay first.";return false;}
    auto l=(SpriteLayer *)calloc(1,layer_total_bytes(c.w,c.h));if(!l)throw std::bad_alloc();std::unique_ptr<void,decltype(&free)> owned(l,free);
    l->w=c.w;l->h=c.h;l->stride=(c.w+3)&~3;l->x=x;l->y=y;l->visible=1;bool opaque=false;
    for(int yy=0;yy<c.h;++yy)for(int xx=0;xx<c.w;++xx){size_t at=(size_t)yy*c.w+xx;auto v=(c.mask.empty()||c.mask[at])?map[c.pixels[at]]:0;layer_pixels(l)[(size_t)yy*l->stride+xx]=v;opaque|=v!=0;}
    if(!opaque){error="The overlay contains no opaque pixels. Index zero stays transparent.";return false;}
    return replace_layer(index,owned.release(),false,error);
}
bool Artwork::paste_layer(const SelectionTransform &s,std::string &error){
    error.clear();int i=sprite_index(s.sprite);auto im=sprite(i);
    if(s.owner!=this||s.generation!=generation()||!s.clipboard||!im||s.original!=copy(i,0,0,im->w,im->h).pixels||s.clip.colors!=copy(i,0,0,im->w,im->h).colors){error="The paste preview changed. Start it again.";return false;}
    SelectionTransformPreview p;if(!preview_selection_transform(s,p,error))return false;
    Clipboard c;c.w=s.w;c.h=s.h;c.colors=s.clip.colors;c.pixels.resize((size_t)c.w*c.h);c.mask.resize(c.pixels.size());
    for(int y=0;y<c.h;++y)for(int x=0;x<c.w;++x){size_t at=(size_t)y*c.w+x;c.mask[at]=p.selection.contains(s.x+x,s.y+y);c.pixels[at]=c.mask[at]?p.pixels[(size_t)(s.y+y)*im->w+s.x+x]:0;}
    return paste_layer(i,c,s.x,s.y,error);
}
bool Artwork::edit_layer(int index,int x,int y,bool visible,bool flip_h,bool flip_v,std::string &error){
    error.clear();auto im=sprite(index);if(!im||!im->layer_p){error="This sprite has no overlay.";return false;}
    size_t n=checked_layer_bytes(*im);auto old=img_layer(im);
    if(x < -32768||x>32767||y < -32768||y>32767){error="Keep overlay coordinates between -32768 and 32767.";return false;}
    if(x==old->x&&y==old->y&&visible==(old->visible!=0)&&!flip_h&&!flip_v)return true;
    auto l=(SpriteLayer *)malloc(n);if(!l)throw std::bad_alloc();memcpy(l,old,n);l->x=x;l->y=y;l->visible=visible;
    for(int yy=0;yy<l->h;++yy)for(int xx=0;xx<l->w;++xx)layer_pixels(l)[(size_t)yy*l->stride+xx]=layer_pixels(old)[(size_t)(flip_v?l->h-1-yy:yy)*l->stride+(flip_h?l->w-1-xx:xx)];
    return replace_layer(index,l,false,error);
}
bool Artwork::flatten_layer(int i,std::string &error){return replace_layer(i,nullptr,true,error);}
bool Artwork::remove_layer(int i,std::string &error){if(!sprite(i)||!sprite(i)->layer_p){error="This sprite has no overlay.";return false;}return replace_layer(i,nullptr,false,error);}
}
