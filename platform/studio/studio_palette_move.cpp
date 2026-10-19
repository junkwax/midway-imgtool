#include "studio_palette_move.h"
#include "studio_layers.h"
#include "palette_math.h"
#include <algorithm>
#include <stdexcept>
namespace studio {
namespace {
bool same_sources(const PaletteCompactionPlan &a,const PaletteCompactionPlan &b){if(a.palette_fields!=b.palette_fields||a.original!=b.original||a.source_sprites.size()!=b.source_sprites.size())return false;for(size_t i=0;i<a.source_sprites.size();++i){auto &x=a.source_sprites[i];auto &y=b.source_sprites[i];if(x.index!=y.index||x.id!=y.id||x.fields!=y.fields||x.pixels!=y.pixels||x.alternate!=y.alternate||x.layer!=y.layer)return false;}return true;}
}
bool plan_palette_move(const Artwork &art,const PaletteMoveOptions &o,PaletteMovePlan &out,std::string &error){
    error.clear();auto pal=art.palette(o.palette);if(!pal||!pal->data_p||pal->numc<2||pal->numc>256||pal->bitspix>8||o.slots[0]){error="Choose a valid palette and opaque colors to move. Index zero stays fixed.";return false;}int selected=0;for(int c=1;c<256;++c)if(o.slots[c]){if(c>=pal->numc){error="A selected color is outside this palette.";return false;}++selected;}if(!selected){error="Select at least one opaque color to move to the end.";return false;}
    try{
        size_t bytes=0;int users=0;for(int i=0;i<art.count();++i){auto im=art.sprite(i);bool used=im->palnum==o.palette;if(im->opaltbl_p){auto refs=(const unsigned char *)im->opaltbl_p;for(int a=0;a<16&&refs[a]<128;++a)used|=refs[a]==o.palette+NUMDEFPAL;}if(!used)continue;if(++users>2000||!im->data_p||!im->w||!im->h||im->w>8192||im->h>8192||(size_t)im->w*im->h>4u*1024u*1024u){error="A palette user has invalid pixels/dimensions or exceeds the 2000-user review limit.";return false;}size_t n=(size_t)((im->w+3)&~3)*im->h;bytes+=n;if(im->layer_p){bytes+=checked_layer_bytes(*im);if(img_layer(im)->visible)bytes+=n;}if(bytes>256u*1024u*1024u){error="Color-move review exceeds its 256 MiB pixel budget. Split the library first.";return false;}}
        PaletteMovePlan p;p.options=o;p.selected=selected;if(!plan_palette_compaction(art,{o.palette,0,false,false,PaletteColorOrder::Keep,true},p.preview,error))return false;auto &v=p.preview;v.words={v.original[0]};v.mapping.fill(0);v.index_changes=0;v.removed=0;v.depth=pal->bitspix;v.ppp_changed=false;
        for(bool take_selected:{false,true})for(size_t c=1;c<v.original.size();++c)if(o.slots[c]==take_selected){v.mapping[c]=(unsigned char)v.words.size();v.words.push_back(v.original[c]);}for(size_t c=1;c<v.original.size();++c)p.moved+=v.mapping[c]!=c;
        for(const auto &source:v.source_sprites){IMG im;memcpy(&im,source.fields.data(),sizeof im);PaletteMoveRow row;row.sprite=source.index;row.primary=im.palnum==o.palette;bool active=!source.alternate.empty()&&source.alternate[0]<128;if(!source.alternate.empty())for(int a=0;a<16&&source.alternate[a]<128;++a)row.alternate|=source.alternate[a]==o.palette+NUMDEFPAL;bool changed=false;int stride=(im.w+3)&~3;
            for(int y=0;y<im.h;++y)for(int x=0;x<im.w;++x){int c=source.pixels[(size_t)y*stride+x];bool shift=c!=v.mapping[c];changed|=shift;if(row.primary)row.shifted+=shift;}
            if(!source.layer.empty()){SpriteLayer layer;memcpy(&layer,source.layer.data(),sizeof layer);auto pixels=source.layer.data()+sizeof layer;for(int y=0;y<layer.h;++y)for(int x=0;x<layer.w;++x){int c=pixels[(size_t)y*layer.stride+x];changed|=c!=v.mapping[c];}}
            if(changed&&!source.layer.empty())row.error="Flatten or remove this user's overlay before moving these colors.";
            else if(changed&&(!row.primary||active))row.error="Moving these indices affects an alternate appearance. Resolve its references first.";
            p.blocked+=!row.error.empty();p.shifted+=row.shifted;p.rows.push_back(std::move(row));}
        v.index_changes=p.shifted;out=std::move(p);return true;
    }catch(const std::bad_alloc &){error="Not enough memory to review moving colors. The library is unchanged.";return false;}
    catch(const std::runtime_error &e){error=e.what();return false;}
}
std::vector<unsigned char> render_palette_move(const Artwork &art,const PaletteMovePlan &p,int row,bool after){
    auto &v=p.preview;if(v.owner!=&art||v.generation!=art.generation()||row<0||row>=(int)v.source_sprites.size()||row>=(int)p.rows.size())return {};auto &s=v.source_sprites[row];IMG im;memcpy(&im,s.fields.data(),sizeof im);int stride=(im.w+3)&~3;auto &words=after?v.words:v.original;if(!im.w||!im.h||s.pixels.size()!=(size_t)stride*im.h||words.empty()||words.size()>256)return {};
    SpriteLayer layer{};if(!s.layer.empty())memcpy(&layer,s.layer.data(),sizeof layer);std::vector<unsigned char> rgba((size_t)im.w*im.h*4);
    for(int y=0;y<im.h;++y)for(int x=0;x<im.w;++x){int c=s.pixels[(size_t)y*stride+x];if(c>=(int)v.original.size())return {};if(after&&p.rows[row].primary)c=v.mapping[c];if(layer.visible){long long sx=(long long)x-layer.x,sy=(long long)y-layer.y;if(sx>=0&&sy>=0&&sx<layer.w&&sy<layer.h){int overlay=s.layer[sizeof(SpriteLayer)+(size_t)sy*layer.stride+(size_t)sx];if(overlay)c=overlay;}}if(c>=(int)words.size())return {};auto word=words[c];unsigned char raw[]={(unsigned char)word,(unsigned char)(word>>8)};auto dest=rgba.data()+((size_t)y*im.w+x)*4;pal_word_to_rgb8(raw,dest,dest+1,dest+2);dest[3]=c?255:0;}return rgba;
}
bool Artwork::move_palette_colors_to_end(const PaletteMovePlan &reviewed,std::string &error){
    error.clear();if(pending||reviewed.preview.owner!=this||reviewed.preview.generation!=generation()){error="The library changed or an edit is pending. Cancel and reopen Move colors to end.";return false;}
    try{PaletteMovePlan p;if(!plan_palette_move(*this,reviewed.options,p,error))return false;if(!same_sources(reviewed.preview,p.preview)||reviewed.preview.words!=p.preview.words||reviewed.preview.mapping!=p.preview.mapping){error="Moving colors no longer matches the reviewed source, users or mapping. Cancel and reopen.";return false;}if(p.blocked){error="Resolve every blocked palette user first. Shared color indices cannot move for only some users.";return false;}if(!p.moved)return true;
        // All allocations and history preparation precede publication.
        State before=capture();before.label="Move palette colors to end";past.reserve(past.size()+1);auto pal=palette(p.options.palette);auto colors=(unsigned char *)pal->data_p;for(size_t c=0;c<p.preview.words.size();++c){colors[c*2]=(unsigned char)p.preview.words[c];colors[c*2+1]=(unsigned char)(p.preview.words[c]>>8);}
        for(int index:p.preview.primary_sprites){auto im=sprite(index);int stride=(im->w+3)&~3;auto pixels=(unsigned char *)im->data_p;for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x)pixels[(size_t)y*stride+x]=p.preview.mapping[pixels[(size_t)y*stride+x]];}
        past.push_back(std::move(before));future.clear();revision=next_revision++;++serial;trim_history();return true;
    }catch(const std::bad_alloc &){error="Not enough memory to move colors. Palette, pixels and history are unchanged.";return false;}
}
}
