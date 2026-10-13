#include "studio_variant.h"
#include "palette_math.h"
#include <algorithm>
#include <memory>
namespace studio {
namespace {
bool dimensions(int w,int h){return w>0&&h>0&&w<=8192&&h<=8192&&(size_t)w*h<=4*1024*1024;}
unsigned short word(const std::vector<unsigned short> &p,int c){return c<(int)p.size()?p[c]:0;}
}
bool capture_variant_source(const Artwork &art,int index,int color,int ppp,const PixelSelection &selection,VariantSource &out,std::string &error){
    out={};error.clear();auto im=art.sprite(index);auto pal=im?art.palette(im->palnum):nullptr;
    if(!im||!im->data_p||!dimensions(im->w,im->h)||!pal||!pal->data_p||pal->numc<1||pal->numc>256||pal->bitspix>8||color<=0||color>=pal->numc||ppp<0||ppp>8||!selection.fits(im->w,im->h)){error="Choose an opaque target color and a valid sprite/selection.";return false;}
    VariantSource s;s.owner=&art;s.generation=art.generation();s.sprite=art.sprite_id(index);s.palette=im->palnum;s.color=color;s.ppp=ppp;s.original=art.copy(index,0,0,im->w,im->h);s.selection=selection;s.used[0]=true;
    if(art.palette_count()<1||art.palette_count()>4093){error="The library's palette count is invalid.";return false;}
    for(int i=0;i<art.palette_count();++i){auto p=art.palette(i);if(!p||!p->data_p||!p->numc||p->numc>256||p->bitspix>8){error="A library palette has invalid colors or depth.";return false;}std::vector<unsigned short> words;for(int c=0;c<p->numc;++c)words.push_back(palette_word_at((unsigned char *)p->data_p,c));s.palettes.push_back(std::move(words));s.depths.push_back(p->bitspix);}
    size_t total=0;for(int i=0;i<art.count();++i){auto sprite=art.sprite(i);auto colors=sprite?art.palette(sprite->palnum):nullptr;if(!sprite||!sprite->data_p||!colors){error="A library sprite has invalid pixels or palette.";return false;}int stride=(sprite->w+3)&~3;total+=(size_t)stride*sprite->h;if(!sprite->w||!sprite->h||total>256*1024*1024){error="The library is too large for a variant review.";return false;}
        for(int y=0;y<sprite->h;++y)for(int x=0;x<sprite->w;++x){int c=((unsigned char *)sprite->data_p)[(size_t)y*stride+x];if(c>=colors->numc){error="A sprite references a color outside its primary palette.";return false;}s.used[c]=true;}
        if(sprite->baseline_p){int bw=sprite->baseline_w?sprite->baseline_w:sprite->w,bh=sprite->baseline_h?sprite->baseline_h:sprite->h,bs=(bw+3)&~3;total+=(size_t)bs*bh;if(total>256*1024*1024){error="The library baselines are too large for a variant review.";return false;}for(int y=0;y<bh;++y)for(int x=0;x<bw;++x)s.used[((unsigned char *)sprite->baseline_p)[(size_t)y*bs+x]]=true;}}
    out=std::move(s);return true;
}
bool plan_variant(const VariantSource &s,const std::vector<unsigned char> &coverage,VariantPlan &out,std::string &error){
    out={};error.clear();auto &c=s.original;
    if(!dimensions(c.w,c.h)||c.pixels.size()!=(size_t)c.w*c.h||coverage.size()!=c.pixels.size()||s.palette<0||s.palette>=(int)s.palettes.size()||s.palettes.empty()||s.palettes.size()>4093||s.depths.size()!=s.palettes.size()||s.color<=0||s.color>=(int)s.palettes[s.palette].size()||s.ppp<0||s.ppp>8||!s.selection.fits(c.w,c.h)||c.colors!=s.palettes[s.palette]){error="The variant source or coverage is invalid.";return false;}
    for(size_t i=0;i<s.palettes.size();++i)if(s.palettes[i].empty()||s.palettes[i].size()>256||s.depths[i]<0||s.depths[i]>8){error="The variant palette snapshot is invalid.";return false;}
    std::array<bool,256> needed{};int transparent=0;auto target=s.palettes[s.palette][s.color];
    for(size_t i=0;i<c.pixels.size();++i){if(c.pixels[i]>=c.colors.size()||coverage[i]>1||(coverage[i]&&s.selection.valid&&!s.selection.contains((int)(i%c.w),(int)(i/c.w)))){error="The variant coverage or source pixels are invalid.";return false;}
        if(!coverage[i])continue;int base=c.pixels[i];if(!base)++transparent;else if((c.colors[base]&0x7fff)!=(target&0x7fff))needed[base]=true;}
    VariantPlan p;p.palettes=s.palettes;p.depths=s.depths;p.transparent=transparent;p.pixels=c.pixels;auto used=s.used;used[0]=used[s.color]=true;std::array<bool,256> reused{},allocated{};for(auto pixel:c.pixels)used[pixel]=true;
    // Base-index order makes slot assignment independent of mouse event frequency.
    for(int base=0;base<256;++base)p.mapping[base]=(unsigned char)base;
    for(int base=1;base<256;++base)if(needed[base]){
        int slot=-1;for(int candidate=1;candidate<256;++candidate){if(candidate==base)continue;bool match=true;for(size_t pal=0;pal<p.palettes.size();++pal){auto expected=pal==(size_t)s.palette?target:word(s.palettes[pal],base);if(candidate>=(int)p.palettes[pal].size()||p.palettes[pal][candidate]!=expected){match=false;break;}}if(match){slot=candidate;break;}}
        if(slot>=0){if(!allocated[slot]&&!reused[slot]){reused[slot]=true;++p.reused_slots;}}
        else {for(int candidate=1;candidate<256;++candidate)if(!used[candidate]){slot=candidate;break;}if(slot<0){error="No unused color slots can preserve all alternate palettes. Reduce the painted colors or free slots before trying again. No edits were applied.";return false;}
            ++p.new_slots;allocated[slot]=true;for(size_t pal=0;pal<p.palettes.size();++pal){if(slot>=(int)p.palettes[pal].size())p.palettes[pal].resize(slot+1,0);p.palettes[pal][slot]=pal==(size_t)s.palette?target:word(s.palettes[pal],base);}}
        used[slot]=true;p.mapping[base]=(unsigned char)slot;
    }
    for(size_t i=0;i<c.pixels.size();++i)if(coverage[i]&&p.mapping[c.pixels[i]]!=c.pixels[i]){p.pixels[i]=p.mapping[c.pixels[i]];++p.changed;}
    for(size_t i=0;i<p.palettes.size();++i){if(p.palettes[i]!=s.palettes[i])p.changed_palettes.push_back((int)i);if(p.palettes[i].size()>s.palettes[i].size()){p.grown_palettes.push_back((int)i);if(PaletteBppTooSmall(p.depths[i],(int)p.palettes[i].size()))p.depths[i]=PaletteBppForColorCount((int)p.palettes[i].size());if(s.ppp&&s.palettes[i].size()<=(size_t)(1<<s.ppp)&&p.palettes[i].size()>(size_t)(1<<s.ppp))p.ppp_breaks.push_back((int)i);}}
    out=std::move(p);return true;
}
bool Artwork::apply_variant(const VariantSource &s,const std::vector<unsigned char> &coverage,const VariantPlan &reviewed,std::string &error){
    error.clear();if(s.owner!=this||s.generation!=generation()){error="The library changed during the variant review. Paint it again.";return false;}int index=sprite_index(s.sprite);VariantSource live;
    if(!capture_variant_source(*this,index,s.color,s.ppp,s.selection,live,error))return false;
    if(live.original.pixels!=s.original.pixels||live.palettes!=s.palettes||live.depths!=s.depths||live.palette!=s.palette||live.used!=s.used){error="The variant source snapshot no longer matches the library.";return false;}
    VariantPlan p;if(!plan_variant(live,coverage,p,error))return false;
    if(p.pixels!=reviewed.pixels||p.palettes!=reviewed.palettes||p.depths!=reviewed.depths||p.mapping!=reviewed.mapping||p.ppp_breaks!=reviewed.ppp_breaks){error="The variant review no longer matches its plan.";return false;}if(!p.changed)return true;
    using Buffer=std::unique_ptr<unsigned char,decltype(&free)>;std::vector<Buffer> buffers;buffers.reserve(p.changed_palettes.size());
    for(int pi:p.changed_palettes){auto words=p.palettes[pi];Buffer bytes((unsigned char *)malloc(words.size()*2),&free);if(!bytes){error="Not enough memory for variant palette colors.";return false;}for(size_t c=0;c<words.size();++c){bytes.get()[c*2]=(unsigned char)words[c];bytes.get()[c*2+1]=(unsigned char)(words[c]>>8);}buffers.push_back(std::move(bytes));}
    begin("Paint palette variant");for(size_t i=0;i<p.changed_palettes.size();++i){int pi=p.changed_palettes[i];auto pal=palette(pi);free(pal->data_p);pal->data_p=buffers[i].release();pal->numc=(unsigned short)p.palettes[pi].size();pal->bitspix=(unsigned char)p.depths[pi];}
    auto im=sprite(index);int stride=(im->w+3)&~3;for(int y=0;y<im->h;++y)memcpy((unsigned char *)im->data_p+(size_t)y*stride,p.pixels.data()+(size_t)y*im->w,im->w);++serial;commit();return true;
}
}
