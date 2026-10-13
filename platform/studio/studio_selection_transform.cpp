#include "studio_selection_transform.h"
#include <algorithm>
namespace studio {
namespace {
bool dimensions(int w,int h){return w>0&&h>0&&w<=8192&&h<=8192&&(size_t)w*h<=4*1024*1024;}
bool snapshot_valid(const SelectionTransform &s){
    if(!dimensions(s.canvas_w,s.canvas_h)||!s.source.valid||!s.source.fits(s.canvas_w,s.canvas_h)||s.clip.w!=s.source.w||s.clip.h!=s.source.h||s.clip.pixels.size()!=(size_t)s.clip.w*s.clip.h||s.clip.mask.size()!=s.clip.pixels.size()||s.original.size()!=(size_t)s.canvas_w*s.canvas_h||s.clip.colors.empty()||s.clip.colors.size()>256)return false;
    for(int y=0;y<s.clip.h;++y)for(int x=0;x<s.clip.w;++x){size_t p=(size_t)y*s.clip.w+x;bool covered=s.source.contains(s.source.x+x,s.source.y+y);
        if((s.clip.mask[p]!=0)!=covered||s.clip.pixels[p]!=s.original[(size_t)(s.source.y+y)*s.canvas_w+s.source.x+x])return false;}
    return std::all_of(s.original.begin(),s.original.end(),[&](unsigned char v){return v<s.clip.colors.size();});
}
}
bool begin_selection_transform(const Artwork &art,int index,const PixelSelection &selection,SelectionTransform &out,std::string &error){
    out={};error.clear();auto im=art.sprite(index);
    if(!im||!im->data_p||!dimensions(im->w,im->h)||!selection.valid||!selection.fits(im->w,im->h)){error="Select pixels on this sprite first.";return false;}
    SelectionTransform s;s.owner=&art;s.generation=art.generation();s.sprite=art.sprite_id(index);s.canvas_w=im->w;s.canvas_h=im->h;s.source=selection;
    s.clip=art.copy_selection(index,selection);s.original=art.copy(index,0,0,im->w,im->h).pixels;s.x=selection.x;s.y=selection.y;s.w=selection.w;s.h=selection.h;
    if(!snapshot_valid(s)){error="The selection or source pixels are unavailable.";return false;}out=std::move(s);return true;
}
bool preview_selection_transform(const SelectionTransform &s,SelectionTransformPreview &out,std::string &error){
    out={};error.clear();
    if(!snapshot_valid(s)||!dimensions(s.w,s.h)||s.rotation<0||s.rotation>3){error="The transform has invalid source pixels, size or orientation.";return false;}
    if(s.x<0||s.y<0||s.w>s.canvas_w||s.h>s.canvas_h||s.x>s.canvas_w-s.w||s.y>s.canvas_h-s.h){error="Keep the whole selection inside the sprite canvas, or increase the canvas size first.";return false;}
    SelectionTransformPreview preview;preview.pixels=s.original;std::vector<unsigned char> mask(preview.pixels.size());
    if(!s.copy)for(int y=s.source.y;y<s.source.y+s.source.h;++y)for(int x=s.source.x;x<s.source.x+s.source.w;++x)if(s.source.contains(x,y))preview.pixels[(size_t)y*s.canvas_w+x]=0;
    const int ow=s.rotation%2?s.clip.h:s.clip.w,oh=s.rotation%2?s.clip.w:s.clip.h;
    // Sample the original oriented selection at pixel centers, never a previous resize.
    for(int y=0;y<s.h;++y)for(int x=0;x<s.w;++x){int ox=(int)(((2LL*x+1)*ow)/(2LL*s.w)),oy=(int)(((2LL*y+1)*oh)/(2LL*s.h));if(s.flip_h)ox=ow-1-ox;if(s.flip_v)oy=oh-1-oy;
        int sx=ox,sy=oy;switch(s.rotation){case 1:sx=oy;sy=s.clip.h-1-ox;break;case 2:sx=s.clip.w-1-ox;sy=s.clip.h-1-oy;break;case 3:sx=s.clip.w-1-oy;sy=ox;break;default:break;}
        size_t source=(size_t)sy*s.clip.w+sx,dest=(size_t)(s.y+y)*s.canvas_w+s.x+x;
        if(s.clip.mask[source]){preview.pixels[dest]=s.clip.pixels[source];mask[dest]=1;}
    }
    preview.selection=selection_from_mask(s.canvas_w,s.canvas_h,std::move(mask));
    if(!preview.selection.valid){error="This size removes every selected pixel. Use a larger selection size.";return false;}
    out=std::move(preview);return true;
}
bool Artwork::apply_selection_transform(const SelectionTransform &s,std::string &error){
    error.clear();if(s.owner!=this||s.generation!=generation()){error="The library changed during the preview. Start the transform again.";return false;}
    int index=sprite_index(s.sprite);SelectionTransform live;
    if(!begin_selection_transform(*this,index,s.source,live,error))return false;
    if(live.canvas_w!=s.canvas_w||live.canvas_h!=s.canvas_h||live.original!=s.original||live.clip.pixels!=s.clip.pixels||live.clip.mask!=s.clip.mask||live.clip.colors!=s.clip.colors){error="The source snapshot no longer matches the sprite.";return false;}
    SelectionTransformPreview preview;if(!preview_selection_transform(s,preview,error))return false;
    auto im=sprite(index);begin(s.copy?"Duplicate selected pixels":"Transform selected pixels");int stride=(im->w+3)&~3;
    for(int y=0;y<im->h;++y)memcpy((unsigned char *)im->data_p+y*stride,preview.pixels.data()+(size_t)y*im->w,im->w);
    ++serial;commit();return true;
}
}
