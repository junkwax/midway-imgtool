#include "studio_palette.h"
#include "palette_math.h"
#include "studio_layers.h"
#include <stdexcept>
#include <algorithm>
#include <tuple>
namespace studio {
namespace {
// Match classic BuildGradientPaletteOrder: RGB555 expands to RGB8, neutral
// colors precede hue families, each family runs from dark to light.
int color_hue(int r,int g,int b){int hi=std::max({r,g,b}),lo=std::min({r,g,b}),delta=hi-lo;if(!delta)return 0;int hue=hi==r?256*(g-b)/delta:hi==g?512+256*(b-r)/delta:1024+256*(r-g)/delta;if(hue<0)hue+=1536;if(hue>=1536)hue-=1536;return hue;}
void order_colors(std::vector<int> &order,const std::vector<unsigned short> &words,PaletteColorOrder mode){
    if(mode==PaletteColorOrder::Keep)return;
    std::array<std::tuple<int,int,int,int,int,int,int,int>,256> keys;
    for(int i:order){unsigned char raw[]={(unsigned char)words[i],(unsigned char)(words[i]>>8)},r,g,b;pal_word_to_rgb8(raw,&r,&g,&b);int luma=r*54+g*183+b*19,sat=std::max({r,g,b})-std::min({r,g,b}),hue=color_hue(r,g,b),family=sat<=18?0:1+((hue+32)%1536)/64;
        keys[i]=mode==PaletteColorOrder::ColorFamilies?std::make_tuple(family,luma,sat,hue,(int)r,(int)g,(int)b,i):std::make_tuple(luma,sat,family,hue,(int)r,(int)g,(int)b,i);}
    std::sort(order.begin(),order.end(),[&](int a,int b){return keys[a]<keys[b];});
}
bool same_sources(const PaletteCompactionPlan &a,const PaletteCompactionPlan &b){
    if(a.palette_fields!=b.palette_fields||a.original!=b.original||a.source_sprites.size()!=b.source_sprites.size())return false;
    for(size_t i=0;i<a.source_sprites.size();++i){auto &x=a.source_sprites[i];auto &y=b.source_sprites[i];if(x.index!=y.index||x.id!=y.id||x.fields!=y.fields||x.pixels!=y.pixels||x.alternate!=y.alternate||x.layer!=y.layer)return false;}return true;
}
}
bool plan_palette_compaction(const Artwork &art,const PaletteCompactionOptions &o,PaletteCompactionPlan &out,std::string &error){
    error.clear();auto pal=art.palette(o.palette);
    if(!pal||!pal->data_p||!pal->numc||pal->numc>256||pal->bitspix>8||o.ppp<0||o.ppp>8||(o.order!=PaletteColorOrder::Keep&&o.order!=PaletteColorOrder::DarkToLight&&o.order!=PaletteColorOrder::ColorFamilies)){error="Choose a valid palette, color order and PPP value from 0 to 8.";return false;}
    try{
        PaletteCompactionPlan p;p.owner=&art;p.generation=art.generation();p.options=o;p.original=copy_palette(art,o.palette).words;memcpy(p.palette_fields.data(),pal,sizeof(PAL));
        for(int i=0;i<art.count();++i){auto im=art.sprite(i);bool primary=im->palnum==o.palette,alternate=false,active=false;if(im->opaltbl_p){auto refs=(const unsigned char *)im->opaltbl_p;active=refs[0]<128;for(int a=0;a<16&&refs[a]<128;++a)alternate|=refs[a]==o.palette+NUMDEFPAL;}
            if(!primary&&!alternate)continue;if(im->layer_p&&!o.copy_only){error="Flatten or remove overlays using this palette before compaction or sorting, or create a clean palette copy.";return false;}if(!im->data_p||!im->w||!im->h){error="A sprite using this palette has invalid pixel data.";return false;}
            p.preview_sprites.push_back(i);if(primary)p.primary_sprites.push_back(i);if(alternate)++p.alternate_sprites;p.indices_locked|=alternate||(primary&&active);
            PaletteCompactionSprite source;source.index=i;source.id=art.sprite_id(i);memcpy(source.fields.data(),im,sizeof(IMG));int stride=(im->w+3)&~3;auto pixels=(const unsigned char *)im->data_p;source.pixels.assign(pixels,pixels+(size_t)stride*im->h);if(im->opaltbl_p){auto refs=(const unsigned char *)im->opaltbl_p;source.alternate.assign(refs,refs+16);}
            auto count_color=[&](int c){if(c>=(int)p.original.size())return false;if(primary)++p.primary[c];if(alternate)++p.alternate[c];return true;};
            for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x)if(!count_color(pixels[y*stride+x])){error="Sprite '"+std::string(im->n_s)+"' references a color outside this palette. Compaction is disabled.";return false;}
            if(im->layer_p){size_t n=checked_layer_bytes(*im);auto bytes=(const unsigned char *)im->layer_p;source.layer.assign(bytes,bytes+n);auto layer=img_layer(im);for(int y=0;y<layer->h;++y)for(int x=0;x<layer->w;++x){int c=layer_pixels(layer)[(size_t)y*layer->stride+x];if(c&&!count_color(c)){error="An overlay references a color outside this palette.";return false;}}if(layer->visible){source.preview_pixels=source.pixels;composite_layer_onto(layer,source.preview_pixels.data(),im->w,im->h,stride);}}
            p.source_sprites.push_back(std::move(source));}
        if(!o.copy_only&&o.pack_live&&o.order!=PaletteColorOrder::Keep&&p.indices_locked){error="Alternate palettes lock shared color indices. Keep the original order; only an unused tail can be trimmed.";return false;}
        p.words.push_back(p.original[0]);int last=0;for(size_t c=1;c<p.original.size();++c)if(p.primary[c]||p.alternate[c])last=(int)c;else ++p.unused;
        bool all=o.copy_only&&p.source_sprites.empty();if(all)last=(int)p.original.size()-1;
        bool pack=o.pack_live&&(!p.indices_locked||o.copy_only);std::vector<int> order;
        for(size_t c=1;c<p.original.size();++c)if(pack?bool(all||p.primary[c]||p.alternate[c]):(int)c<=last)order.push_back((int)c);
        if(pack)order_colors(order,p.original,o.order);
        for(int c:order){p.mapping[c]=(unsigned char)p.words.size();p.words.push_back(p.original[c]);}
        p.removed=(int)p.original.size()-(int)p.words.size();p.depth=o.recompute_depth?PaletteBppForColorCount((int)p.words.size()):pal->bitspix;
        for(size_t c=1;c<p.original.size();++c)if(p.mapping[c]!=c)p.index_changes+=p.primary[c];
        if(o.ppp){int limit=1<<o.ppp;p.ppp_changed=((int)p.original.size()>limit)!=((int)p.words.size()>limit);}
        out=std::move(p);return true;
    }catch(const std::bad_alloc &){error="Not enough memory to review palette organization. The library is unchanged.";return false;}
    catch(const std::runtime_error &e){error=e.what();return false;}
}
std::vector<unsigned char> render_palette_compaction(const Artwork &art,const PaletteCompactionPlan &p,int index,bool compacted){
    if(p.owner!=&art||p.generation!=art.generation())return {};
    auto source=std::find_if(p.source_sprites.begin(),p.source_sprites.end(),[&](const PaletteCompactionSprite &s){return s.index==index;});if(source==p.source_sprites.end())return {};
    IMG im;memcpy(&im,source->fields.data(),sizeof im);int stride=(im.w+3)&~3;auto &words=compacted?p.words:p.original;
    const auto &pixels=source->preview_pixels.empty()?source->pixels:source->preview_pixels;
    if(!im.w||!im.h||pixels.size()!=(size_t)stride*im.h||words.empty()||words.size()>256)return {};
    std::vector<unsigned char> rgba((size_t)im.w*im.h*4);
    for(int y=0;y<im.h;++y)for(int x=0;x<im.w;++x){int c=pixels[y*stride+x];if(c>=(int)p.original.size())return {};int mapped=compacted?p.mapping[c]:c;if(mapped>=(int)words.size())return {};unsigned short w=words[mapped];unsigned char word[]={(unsigned char)w,(unsigned char)(w>>8)};auto dest=rgba.data()+((size_t)y*im.w+x)*4;pal_word_to_rgb8(word,dest,dest+1,dest+2);dest[3]=mapped?255:0;}return rgba;
}
bool Artwork::compact_palette(const PaletteCompactionPlan &reviewed,std::string &error){
    if(reviewed.options.copy_only){error="This review creates a new palette. Use Create clean copy instead of editing the source.";return false;}
    error.clear();if(pending||reviewed.owner!=this||reviewed.generation!=generation()){error="The library changed or an edit is pending. Cancel and reopen the usage preview.";return false;}
    PaletteCompactionPlan p;if(!plan_palette_compaction(*this,reviewed.options,p,error))return false;
    if(!same_sources(reviewed,p)){error="Palette colors, metadata or sprite data no longer match this review. Cancel and reopen it.";return false;}
    auto pal=palette(p.options.palette);if(!p.removed&&pal->bitspix==p.depth&&!p.index_changes)return true;
    try{
        std::unique_ptr<unsigned char,decltype(&free)> colors((unsigned char *)malloc(p.words.size()*2),free);if(!colors)throw std::bad_alloc();for(size_t i=0;i<p.words.size();++i){colors.get()[i*2]=(unsigned char)p.words[i];colors.get()[i*2+1]=(unsigned char)(p.words[i]>>8);}
        State before=capture();before.label=p.options.pack_live&&p.options.order!=PaletteColorOrder::Keep?"Organize palette colors":"Compact palette colors";past.reserve(past.size()+1);
        free(pal->data_p);pal->data_p=colors.release();pal->numc=(unsigned short)p.words.size();pal->bitspix=(unsigned char)p.depth;
        for(int index:p.primary_sprites){auto im=sprite(index);int stride=(im->w+3)&~3;for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x){auto pixel=(unsigned char *)im->data_p+y*stride+x;*pixel=p.mapping[*pixel];}}
        past.push_back(std::move(before));future.clear();revision=next_revision++;++serial;trim_history();return true;
    }catch(const std::bad_alloc &){error="Not enough memory to organize palette colors. The palette, pixels and history were left unchanged.";return false;}
}
int Artwork::copy_clean_palette(const PaletteCompactionPlan &reviewed,std::string &error){
    error.clear();if(pending||!reviewed.options.copy_only||reviewed.owner!=this||reviewed.generation!=generation()){error="Choose a clean-copy review from this unchanged library and finish any pending edit.";return -1;}
    PaletteCompactionPlan p;if(!plan_palette_compaction(*this,reviewed.options,p,error))return -1;
    if(!same_sources(reviewed,p)){error="The clean-copy source changed. Cancel and reopen the review.";return -1;}
    try{
        PaletteTransfer clip;if(!copy_palettes({p.options.palette},clip,error))return -1;
        auto &entry=clip.entries.front();entry.words=p.words;entry.fields.numc=(unsigned short)p.words.size();entry.fields.bitspix=(unsigned char)p.depth;clip.words=p.words;
        return paste_palettes(clip,error,"Create clean palette copy");
    }catch(const std::bad_alloc &){error="Not enough memory to create the clean palette copy. The library and history are unchanged.";return -1;}
}
}
