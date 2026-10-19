#include "studio_palette_removal.h"
#include "studio_layers.h"
#include <algorithm>
#include <stdexcept>
namespace studio {
namespace {
bool same_sources(const PaletteCompactionPlan &a,const PaletteCompactionPlan &b){if(a.palette_fields!=b.palette_fields||a.original!=b.original||a.source_sprites.size()!=b.source_sprites.size())return false;for(size_t i=0;i<a.source_sprites.size();++i){auto &x=a.source_sprites[i];auto &y=b.source_sprites[i];if(x.index!=y.index||x.id!=y.id||x.fields!=y.fields||x.pixels!=y.pixels||x.alternate!=y.alternate||x.layer!=y.layer)return false;}return true;}
}
bool plan_palette_removal(const Artwork &art,const PaletteRemovalOptions &o,PaletteRemovalPlan &out,std::string &error){
    error.clear();auto pal=art.palette(o.palette);if(!pal||!pal->data_p||pal->numc<2||pal->numc>256||pal->bitspix>8||o.ppp<0||o.ppp>8||o.slots[0]){error="Choose a valid palette, opaque slots and PPP from 0 to 8.";return false;}int count=0;for(int c=1;c<256;++c)if(o.slots[c]){if(c>=pal->numc){error="A selected slot is outside this palette.";return false;}++count;}if(!count){error="Select at least one opaque color slot to remove.";return false;}
    try{
        // Validate/bound every primary and active alternate user before capture.
        size_t bytes=0;int users=0;for(int i=0;i<art.count();++i){auto im=art.sprite(i);bool used=im->palnum==o.palette;if(im->opaltbl_p){auto refs=(const unsigned char *)im->opaltbl_p;for(int a=0;a<16&&refs[a]<128;++a)used|=refs[a]==o.palette+NUMDEFPAL;}if(!used)continue;if(++users>2000||!im->data_p||!im->w||!im->h||im->w>8192||im->h>8192||(size_t)im->w*im->h>4u*1024u*1024u){error="A palette user has invalid pixel data/dimensions, or exceeds the 2000-user limit.";return false;}size_t n=(size_t)((im->w+3)&~3)*im->h;bytes+=n;if(im->layer_p){bytes+=checked_layer_bytes(*im);if(img_layer(im)->visible)bytes+=n;}if(bytes>256u*1024u*1024u){error="Slot removal exceeds its 256 MiB review budget. Split the library first.";return false;}}
        PaletteRemovalPlan p;p.options=o;if(!plan_palette_compaction(art,{o.palette,o.ppp,false,false,PaletteColorOrder::Keep,true},p.preview,error))return false;auto &v=p.preview;v.words={v.original[0]};v.mapping.fill(0);v.index_changes=0;
        for(size_t c=1;c<v.original.size();++c)if(!o.slots[c]){v.mapping[c]=(unsigned char)v.words.size();v.words.push_back(v.original[c]);}v.removed=count;v.depth=pal->bitspix;v.ppp_changed=o.ppp&&((v.original.size()>(size_t)(1<<o.ppp))!=(v.words.size()>(size_t)(1<<o.ppp)));
        for(const auto &source:v.source_sprites){IMG fields;memcpy(&fields,source.fields.data(),sizeof fields);PaletteRemovalRow row;row.sprite=source.index;row.primary=fields.palnum==o.palette;bool active=!source.alternate.empty()&&source.alternate[0]<128;if(!source.alternate.empty())for(int a=0;a<16&&source.alternate[a]<128;++a)row.alternate|=source.alternate[a]==o.palette+NUMDEFPAL;
            bool any_change=false;int stride=(fields.w+3)&~3;for(int y=0;y<fields.h;++y)for(int x=0;x<fields.w;++x){int c=source.pixels[(size_t)y*stride+x],mapped=v.mapping[c];any_change|=c!=mapped;if(row.primary&&c!=mapped){if(mapped)++row.shifted;else ++row.cleared;}}
            if(!source.layer.empty()){SpriteLayer layer;memcpy(&layer,source.layer.data(),sizeof layer);auto pixels=source.layer.data()+sizeof layer;for(int y=0;y<layer.h;++y)for(int x=0;x<layer.w;++x){int c=pixels[(size_t)y*layer.stride+x];any_change|=c!=v.mapping[c];}}
            if(any_change&&!source.layer.empty())row.error="Flatten or remove this user's overlay before deleting these slots.";
            else if(any_change&&(!row.primary||active))row.error="These slots are used by an alternate appearance. Resolve its references before deleting them.";
            p.blocked+=!row.error.empty();p.cleared+=row.cleared;p.shifted+=row.shifted;v.index_changes+=row.cleared+row.shifted;p.rows.push_back(std::move(row));
        }
        out=std::move(p);return true;
    }catch(const std::bad_alloc &){error="Not enough memory to review slot removal. The library is unchanged.";return false;}
    catch(const std::runtime_error &e){error=e.what();return false;}
}
bool Artwork::remove_palette_slots(const PaletteRemovalPlan &reviewed,std::string &error){
    error.clear();if(pending||reviewed.preview.owner!=this||reviewed.preview.generation!=generation()){error="The library changed or an edit is pending. Cancel and reopen slot removal.";return false;}
    try{PaletteRemovalPlan p;if(!plan_palette_removal(*this,reviewed.options,p,error))return false;if(!same_sources(reviewed.preview,p.preview)||reviewed.preview.words!=p.preview.words||reviewed.preview.mapping!=p.preview.mapping){error="Slot removal no longer matches its source colors, users or result. Cancel and reopen.";return false;}if(p.blocked){error="Resolve every blocked palette user first. Slots cannot be removed from only part of a shared palette.";return false;}
        auto pal=palette(p.options.palette);std::unique_ptr<unsigned char,decltype(&free)> colors((unsigned char *)malloc(p.preview.words.size()*2),free);if(!colors)throw std::bad_alloc();for(size_t c=0;c<p.preview.words.size();++c){colors.get()[c*2]=(unsigned char)p.preview.words[c];colors.get()[c*2+1]=(unsigned char)(p.preview.words[c]>>8);}
        State before=capture();before.label="Remove palette color slots";past.reserve(past.size()+1);
        free(pal->data_p);pal->data_p=colors.release();pal->numc=(unsigned short)p.preview.words.size();
        for(int i:p.preview.primary_sprites){auto im=sprite(i);int stride=(im->w+3)&~3;auto pixels=(unsigned char *)im->data_p;for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x)pixels[(size_t)y*stride+x]=p.preview.mapping[pixels[(size_t)y*stride+x]];}
        // Overlays are retained only when every referenced index stays fixed.
        past.push_back(std::move(before));future.clear();revision=next_revision++;++serial;trim_history();return true;
    }catch(const std::bad_alloc &){error="Not enough memory to remove slots. Palette, pixels and history are unchanged.";return false;}
}
}
