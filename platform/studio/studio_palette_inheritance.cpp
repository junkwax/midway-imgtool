#include "studio_palette.h"
#include "palette_math.h"
#include <algorithm>
namespace studio {
namespace {
bool same_sources(const PaletteInheritancePlan &a,const PaletteInheritancePlan &b){
    if(a.target_fields!=b.target_fields||a.source_fields!=b.source_fields||a.original!=b.original||a.words!=b.words||a.sprites.size()!=b.sprites.size())return false;
    for(size_t i=0;i<a.sprites.size();++i){auto &x=a.sprites[i];auto &y=b.sprites[i];if(x.index!=y.index||x.id!=y.id||x.fields!=y.fields||x.pixels!=y.pixels||x.alternate!=y.alternate)return false;}return true;
}
}
bool plan_palette_inheritance(const Artwork &art,const PaletteInheritanceOptions &o,PaletteInheritancePlan &out,std::string &error){
    error.clear();auto target=art.palette(o.target),source=art.palette(o.source);
    if(!target||!source||target==source||!target->data_p||!source->data_p||!target->numc||target->numc>256||source->numc<2||source->numc>256||target->bitspix>8||source->bitspix>8||o.ppp<0||o.ppp>8){error="Choose two different valid palettes; the marked source needs at least one opaque color.";return false;}
    try{
        PaletteInheritancePlan p;p.owner=&art;p.generation=art.generation();p.options=o;
        memcpy(p.target_fields.data(),target,sizeof(PAL));memcpy(p.source_fields.data(),source,sizeof(PAL));p.original=copy_palette(art,o.target).words;p.words=copy_palette(art,o.source).words;
        size_t snapshot_bytes=0;
        for(int i=0;i<art.count();++i){auto im=art.sprite(i);bool primary=im->palnum==o.target,alternate=false,active=false;
            if(im->opaltbl_p){auto refs=(const unsigned char *)im->opaltbl_p;active=refs[0]<128;for(int a=0;a<16&&refs[a]<128;++a)alternate|=refs[a]==o.target+NUMDEFPAL;}
            if(alternate||(primary&&active)){error="This palette shares indices with alternate palettes. Remove those alternate references before inheriting colors.";return false;}
            if(!primary)continue;
            if(im->layer_p){error="Flatten or remove overlays using this palette before inheriting colors.";return false;}
            if(!im->data_p||!im->w||!im->h||im->w>8192||im->h>8192||(size_t)im->w*im->h>4u*1024u*1024u){error="A sprite using this palette has invalid dimensions or pixels.";return false;}
            int stride=(im->w+3)&~3;snapshot_bytes+=(size_t)stride*im->h;if(snapshot_bytes>256u*1024u*1024u){error="Palette inheritance review exceeds the 256 MiB pixel budget. Split the library before reviewing.";return false;}
            PaletteCompactionSprite s;s.index=i;s.id=art.sprite_id(i);memcpy(s.fields.data(),im,sizeof(IMG));auto pixels=(const unsigned char *)im->data_p;s.pixels.assign(pixels,pixels+(size_t)stride*im->h);if(im->opaltbl_p){auto refs=(const unsigned char *)im->opaltbl_p;s.alternate.assign(refs,refs+16);}
            for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x){int c=pixels[y*stride+x];if(c>=(int)p.original.size()){error="A sprite references a color outside the selected palette.";return false;}++p.usage[c];}
            p.sprites.push_back(std::move(s));
        }
        for(size_t c=1;c<p.original.size();++c){int best=1,distance=PaletteColorDistance5W(p.original[c],p.words[1],o.perceptual);for(size_t d=2;d<p.words.size();++d){int delta=PaletteColorDistance5W(p.original[c],p.words[d],o.perceptual);if(delta<distance){best=(int)d;distance=delta;}}p.mapping[c]=(unsigned char)best;if(best!=(int)c)p.index_changes+=p.usage[c];if(distance&&p.usage[c]){++p.approximate_colors;p.color_changes+=p.usage[c];}}
        p.ppp_changed=o.ppp&&((target->numc>(1<<o.ppp))!=(source->numc>(1<<o.ppp)));
        p.changed=p.original!=p.words||target->bitspix!=source->bitspix||target->pad!=source->pad||p.index_changes;
        out=std::move(p);return true;
    }catch(const std::bad_alloc &){error="Not enough memory to review inherited colors. The library is unchanged.";return false;}
}
std::vector<unsigned char> render_palette_inheritance(const Artwork &art,const PaletteInheritancePlan &p,int index,bool inherited){
    if(p.owner!=&art||p.generation!=art.generation())return {};
    auto s=std::find_if(p.sprites.begin(),p.sprites.end(),[&](const PaletteCompactionSprite &s){return s.index==index;});if(s==p.sprites.end())return {};
    IMG im;memcpy(&im,s->fields.data(),sizeof im);int stride=(im.w+3)&~3;auto &words=inherited?p.words:p.original;
    if(!im.w||!im.h||im.w>8192||im.h>8192||s->pixels.size()!=(size_t)stride*im.h||words.empty()||words.size()>256||p.original.size()>256)return {};
    std::vector<unsigned char> rgba((size_t)im.w*im.h*4);
    for(int y=0;y<im.h;++y)for(int x=0;x<im.w;++x){int c=s->pixels[y*stride+x];if(c>=(int)p.original.size())return {};int mapped=inherited?p.mapping[c]:c;if(mapped>=(int)words.size())return {};unsigned short w=words[mapped];unsigned char raw[]={(unsigned char)w,(unsigned char)(w>>8)};auto dst=rgba.data()+((size_t)y*im.w+x)*4;pal_word_to_rgb8(raw,dst,dst+1,dst+2);dst[3]=c?255:0;}return rgba;
}
bool Artwork::inherit_palette_colors(const PaletteInheritancePlan &reviewed,bool allow_approximation,std::string &error){
    error.clear();if(pending||reviewed.owner!=this||reviewed.generation!=generation()){error="The library changed or an edit is pending. Cancel and reopen the inheritance review.";return false;}
    PaletteInheritancePlan p;if(!plan_palette_inheritance(*this,reviewed.options,p,error))return false;
    if(!same_sources(reviewed,p)){error="Palette colors, metadata or sprite data changed. Cancel and reopen the inheritance review.";return false;}
    if(p.color_changes&&!allow_approximation){error="Review the before/after result and allow approximate colors before applying.";return false;}if(!p.changed)return true;
    try{
        auto target=palette(p.options.target),source=palette(p.options.source);
        std::unique_ptr<unsigned char,decltype(&free)> colors((unsigned char *)malloc(p.words.size()*2),free);if(!colors)throw std::bad_alloc();for(size_t i=0;i<p.words.size();++i){colors.get()[i*2]=(unsigned char)p.words[i];colors.get()[i*2+1]=(unsigned char)(p.words[i]>>8);}
        State before=capture();before.label="Inherit palette colors";past.reserve(past.size()+1);
        free(target->data_p);target->data_p=colors.release();target->numc=source->numc;target->bitspix=source->bitspix;target->pad=source->pad;
        for(const auto &s:p.sprites){auto im=sprite(s.index);int stride=(im->w+3)&~3;for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x){auto pixel=(unsigned char *)im->data_p+y*stride+x;*pixel=p.mapping[*pixel];}}
        past.push_back(std::move(before));future.clear();revision=next_revision++;++serial;trim_history();return true;
    }catch(const std::bad_alloc &){error="Not enough memory to inherit colors. Palette, pixels and history are unchanged.";return false;}
}
}
