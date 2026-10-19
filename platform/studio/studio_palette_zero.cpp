#include "studio_palette_zero.h"
#include "studio_layers.h"
#include "palette_math.h"
#include <algorithm>
#include <stdexcept>
namespace studio {
namespace {
bool same_sources(const PaletteCompactionPlan &a,const PaletteCompactionPlan &b){if(a.palette_fields!=b.palette_fields||a.original!=b.original||a.source_sprites.size()!=b.source_sprites.size())return false;for(size_t i=0;i<a.source_sprites.size();++i){auto &x=a.source_sprites[i];auto &y=b.source_sprites[i];if(x.index!=y.index||x.id!=y.id||x.fields!=y.fields||x.pixels!=y.pixels||x.alternate!=y.alternate||x.layer!=y.layer)return false;}return true;}
}
bool plan_palette_zero(const Artwork &art,const PaletteZeroOptions &o,PaletteZeroPlan &out,std::string &error){
    error.clear();auto pal=art.palette(o.palette);if(!pal||!pal->data_p||!pal->numc||pal->numc>256||pal->bitspix>8||o.slot< -1||o.slot==0||o.slot>255||o.ppp<0||o.ppp>8||(o.scope!=PaletteZeroScope::CopyOnly&&o.scope!=PaletteZeroScope::Current&&o.scope!=PaletteZeroScope::Selection)){error="Choose a valid palette, automatic or opaque destination slot, and copy/remap mode.";return false;}
    int target=art.sprite_index(o.sprite);bool remap=o.scope!=PaletteZeroScope::CopyOnly;if(remap&&(target<0||art.sprite(target)->palnum!=o.palette)){error="Current/selection remapping needs the selected artwork sprite on this palette.";return false;}
    if(o.selection.mask.size()>4u*1024u*1024u||(o.scope==PaletteZeroScope::Selection?(!o.selection.valid||!o.selection.fits(art.sprite(target)->w,art.sprite(target)->h)):(o.selection.valid||!o.selection.mask.empty()))){error="Selection remapping needs a valid bounded canvas selection; other modes use no selection mask.";return false;}
    try{
        size_t bytes=o.selection.mask.size();int users=0;for(int i=0;i<art.count();++i){auto im=art.sprite(i);bool used=im->palnum==o.palette;if(im->opaltbl_p){auto refs=(const unsigned char *)im->opaltbl_p;for(int a=0;a<16&&refs[a]<128;++a)used|=refs[a]==o.palette+NUMDEFPAL;}if(!used)continue;if(++users>2000||!im->data_p||!im->w||!im->h||im->w>8192||im->h>8192||(size_t)im->w*im->h>4u*1024u*1024u){error="A palette user has invalid pixels/dimensions or exceeds the 2000-user review limit.";return false;}size_t n=(size_t)((im->w+3)&~3)*im->h;bytes+=n;if(remap&&i==target)bytes+=n;if(im->layer_p){bytes+=checked_layer_bytes(*im);if(img_layer(im)->visible)bytes+=n;}if(bytes>256u*1024u*1024u){error="Opaque-color review exceeds its 256 MiB pixel budget. Split the library first.";return false;}}
        PaletteZeroPlan p;p.options=o;if(!plan_palette_compaction(art,{o.palette,o.ppp,false,false,PaletteColorOrder::Keep,true},p.source,error))return false;auto &s=p.source;
        p.slot=o.slot;if(p.slot<0){if(pal->numc<256)p.slot=pal->numc;else{for(int c=1;c<256;++c)if(!s.primary[c]&&!s.alternate[c]){p.slot=c;break;}if(p.slot<0)for(int c=1;c<256;++c)if(s.original[c]==s.original[0]){p.slot=c;break;}}}
        if(p.slot<1){error="No safe automatic slot is available. Choose an explicit opaque slot and review its existing users, or free a slot first.";return false;}
        p.words=s.original;p.words.resize(std::max(p.words.size(),(size_t)p.slot+1),0);p.words[p.slot]=s.original[0];p.growth=(int)p.words.size()-(int)s.original.size();p.depth=p.growth?std::max<int>(pal->bitspix,PaletteBppForColorCount((int)p.words.size())):pal->bitspix;p.ppp_changed=o.ppp&&(s.original.size()<=(size_t)(1<<o.ppp)&&p.words.size()>(size_t)(1<<o.ppp));bool recolor=p.slot<(int)s.original.size()&&((s.original[p.slot]^s.original[0])&0x7fff);
        for(const auto &source:s.source_sprites){IMG im;memcpy(&im,source.fields.data(),sizeof im);PaletteZeroRow row;row.sprite=source.index;row.primary=im.palnum==o.palette;if(!source.alternate.empty())for(int a=0;a<16&&source.alternate[a]<128;++a)row.alternate|=source.alternate[a]==o.palette+NUMDEFPAL;bool is_target=remap&&source.id==o.sprite;int stride=(im.w+3)&~3;if(is_target)p.target_after=source.pixels;
            for(int y=0;y<im.h;++y)for(int x=0;x<im.w;++x){size_t at=(size_t)y*stride+x;int c=source.pixels[at];row.uses+=c==p.slot;if(is_target&&!c&&(o.scope!=PaletteZeroScope::Selection||o.selection.contains(x,y))){p.target_after[at]=(unsigned char)p.slot;++p.filled;}}
            if(!source.layer.empty()){SpriteLayer layer;memcpy(&layer,source.layer.data(),sizeof layer);auto pixels=source.layer.data()+sizeof layer;for(int y=0;y<layer.h;++y)for(int x=0;x<layer.w;++x)row.uses+=pixels[(size_t)y*layer.stride+x]==p.slot;}
            if(is_target&&p.filled){if(!source.layer.empty())row.error="Flatten or remove the target sprite's overlay before making its transparent pixels opaque.";else if(!source.alternate.empty()&&source.alternate[0]<128)row.error="Remapping zero would change the target's alternate silhouettes. Resolve its alternate references first.";}
            if(recolor)p.recolored+=row.uses;p.blocked+=!row.error.empty();p.rows.push_back(std::move(row));
        }
        p.changed=p.words!=s.original||p.filled;out=std::move(p);return true;
    }catch(const std::bad_alloc &){error="Not enough memory to review index-zero copying. Artwork is unchanged.";return false;}
    catch(const std::runtime_error &e){error=e.what();return false;}
}
std::vector<unsigned char> render_palette_zero(const Artwork &art,const PaletteZeroPlan &p,int row,bool after){
    if(p.source.owner!=&art||p.source.generation!=art.generation()||row<0||row>=(int)p.source.source_sprites.size())return {};auto &s=p.source.source_sprites[row];IMG im;memcpy(&im,s.fields.data(),sizeof im);int stride=(im.w+3)&~3;auto &words=after?p.words:p.source.original;auto &pixels=after&&s.id==p.options.sprite&&!p.target_after.empty()?p.target_after:s.pixels;if(pixels.size()!=(size_t)stride*im.h||words.empty()||words.size()>256)return {};
    SpriteLayer layer{};if(!s.layer.empty())memcpy(&layer,s.layer.data(),sizeof layer);std::vector<unsigned char> rgba((size_t)im.w*im.h*4);
    for(int y=0;y<im.h;++y)for(int x=0;x<im.w;++x){int c=pixels[(size_t)y*stride+x];if(layer.visible){long long sx=(long long)x-layer.x,sy=(long long)y-layer.y;if(sx>=0&&sy>=0&&sx<layer.w&&sy<layer.h){int v=s.layer[sizeof(SpriteLayer)+(size_t)sy*layer.stride+(size_t)sx];if(v)c=v;}}if(c>=(int)words.size())return {};auto word=words[c];unsigned char raw[]={(unsigned char)word,(unsigned char)(word>>8)};auto dest=rgba.data()+((size_t)y*im.w+x)*4;pal_word_to_rgb8(raw,dest,dest+1,dest+2);dest[3]=c?255:0;}return rgba;
}
bool Artwork::copy_zero_to_opaque(const PaletteZeroPlan &reviewed,bool allow_recolor,std::string &error){
    error.clear();if(pending||reviewed.source.owner!=this||reviewed.source.generation!=generation()){error="The library changed or an edit is pending. Cancel and reopen index-zero copying.";return false;}
    try{PaletteZeroPlan p;if(!plan_palette_zero(*this,reviewed.options,p,error))return false;if(!same_sources(reviewed.source,p.source)||reviewed.words!=p.words||reviewed.slot!=p.slot||reviewed.depth!=p.depth||reviewed.target_after!=p.target_after){error="Index-zero copying no longer matches its source, slot or reviewed pixels. Cancel and reopen.";return false;}if(p.blocked){error="Resolve the target's overlay or alternate references before remapping zero pixels.";return false;}if(p.recolored&&!allow_recolor){error="The destination color is already used. Review and allow recoloring its existing users before applying.";return false;}if(!p.changed)return true;
        std::unique_ptr<unsigned char,decltype(&free)> colors((unsigned char *)malloc(p.words.size()*2),free);if(!colors)throw std::bad_alloc();for(size_t c=0;c<p.words.size();++c){colors.get()[c*2]=(unsigned char)p.words[c];colors.get()[c*2+1]=(unsigned char)(p.words[c]>>8);}State before=capture();before.label="Copy zero color to opaque";past.reserve(past.size()+1);auto pal=palette(p.options.palette);
        free(pal->data_p);pal->data_p=colors.release();pal->numc=(unsigned short)p.words.size();pal->bitspix=(unsigned char)p.depth;if(!p.target_after.empty())memcpy(sprite(sprite_index(p.options.sprite))->data_p,p.target_after.data(),p.target_after.size());past.push_back(std::move(before));future.clear();revision=next_revision++;++serial;trim_history();return true;
    }catch(const std::bad_alloc &){error="Not enough memory to copy zero color. Palette, pixels and history are unchanged.";return false;}
}
}
