#include "studio_palette_transparency.h"
#include "studio_palette.h"
#include "studio_layers.h"
#include <algorithm>
#include <set>
#include <stdexcept>
namespace studio {
namespace {
bool same_row(const BatchPaletteRow &a,const BatchPaletteRow &b){return a.sprite==b.sprite&&a.changed==b.changed&&a.alternates==b.alternates&&a.source==b.source&&a.w==b.w&&a.h==b.h&&a.index_changes==b.index_changes&&a.unknown_colors==b.unknown_colors&&a.name==b.name&&a.error==b.error&&a.fields==b.fields&&a.before==b.before&&a.after==b.after&&a.source_words==b.source_words;}
}
bool plan_palette_transparency(const Artwork &art,const std::vector<uint64_t> &ids,const PaletteTransparencyOptions &o,PaletteTransparencyPlan &out,std::string &error){
    error.clear();auto pal=art.palette(o.palette);if(!pal||!pal->data_p||pal->numc<2||pal->numc>256||pal->bitspix>8||ids.empty()||ids.size()>2000||o.slots[0]){error="Choose a valid source palette, opaque colors and 1-2000 sprites.";return false;}
    int selected=0;for(int c=1;c<256;++c)if(o.slots[c]){if(c>=pal->numc){error="A chosen color is outside the source palette.";return false;}++selected;}if(!selected){error="Select at least one opaque palette color.";return false;}
    if((o.selection.valid&&(ids.size()!=1||o.all_users))||(!o.selection.valid&&!o.selection.mask.empty())||o.selection.mask.size()>4u*1024u*1024u){error="Canvas selection requires one sprite and a valid bounded mask.";return false;}
    try{
        std::set<uint64_t> chosen;for(auto id:ids){int i=art.sprite_index(id);if(i<0||!chosen.insert(id).second||art.sprite(i)->palnum!=o.palette){error="The transparency scope contains a missing/repeated sprite or another palette.";return false;}}
        if(o.all_users)for(int i=0;i<art.count();++i)if(art.sprite(i)->palnum==o.palette&&!chosen.count(art.sprite_id(i))){error="All-on-palette scope changed. Reopen the review.";return false;}
        PaletteTransparencyPlan p;p.options=o;p.preview.owner=&art;p.preview.generation=art.generation();p.preview.options={o.palette,0,false};p.preview.target_words=copy_palette(art,o.palette).words;memcpy(p.palette_fields.data(),pal,sizeof(PAL));size_t bytes=o.selection.mask.size();
        for(int i=0;i<art.count();++i){if(!chosen.count(art.sprite_id(i)))continue;auto im=art.sprite(i);if(!im->data_p||!im->w||!im->h||im->w>8192||im->h>8192||(size_t)im->w*im->h>4u*1024u*1024u){error="A source sprite has invalid pixels or dimensions.";return false;}if(o.selection.valid&&!o.selection.fits(im->w,im->h)){error="The canvas selection no longer fits this sprite.";return false;}
            int stride=(im->w+3)&~3;size_t size=(size_t)stride*im->h,layer_size=im->layer_p?checked_layer_bytes(*im):0;bytes+=size*2+layer_size;if(bytes>256u*1024u*1024u){error="Transparency review exceeds its 256 MiB pixel budget. Use a smaller scope.";return false;}
            BatchPaletteRow row;row.sprite=art.sprite_id(i);row.name=std::string(im->n_s,strnlen(im->n_s,16));row.source=o.palette;row.w=im->w;row.h=im->h;row.source_words=p.preview.target_words;memcpy(row.fields.data(),im,sizeof(IMG));auto pixels=(const unsigned char *)im->data_p;row.before.assign(pixels,pixels+size);row.after=row.before;
            std::vector<unsigned char> alternate,layer;if(im->opaltbl_p){auto refs=(const unsigned char *)im->opaltbl_p;alternate.assign(refs,refs+16);row.alternates=refs[0]<128;}if(im->layer_p){auto raw=(const unsigned char *)im->layer_p;layer.assign(raw,raw+layer_size);}
            for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x){size_t at=(size_t)y*stride+x;int c=pixels[at];if(c>=pal->numc){++row.unknown_colors;continue;}if(o.selection.valid&&!o.selection.contains(x,y))continue;++p.usage[c];if(o.slots[c]){row.after[at]=0;++row.index_changes;}}
            row.changed=row.index_changes!=0;
            if(row.unknown_colors)row.error="Unresolved color indices need repair or exclusion.";
            else if(row.changed&&im->layer_p)row.error="Flatten or remove this sprite's overlay first, or exclude it.";
            else if(row.changed&&row.alternates)row.error="Removing pixels would change alternate appearances. Resolve alternate references or exclude this sprite.";
            if(!row.error.empty())row.after=row.before;
            p.preview.rows.push_back(std::move(row));p.alternates.push_back(std::move(alternate));p.layers.push_back(std::move(layer));
        }
        out=std::move(p);return true;
    }catch(const std::bad_alloc &){error="Not enough memory to review transparency. The library is unchanged.";return false;}
    catch(const std::runtime_error &e){error=e.what();return false;}
}
int Artwork::apply_palette_transparency(const PaletteTransparencyPlan &reviewed,std::string &error){
    error.clear();if(pending||reviewed.preview.owner!=this||reviewed.preview.generation!=generation()){error="The library changed or an edit is pending. Cancel and reopen the transparency review.";return -1;}
    try{
        std::vector<uint64_t> ids;for(auto &r:reviewed.preview.rows)ids.push_back(r.sprite);PaletteTransparencyPlan p;if(!plan_palette_transparency(*this,ids,reviewed.options,p,error))return -1;
        if(p.palette_fields!=reviewed.palette_fields||p.preview.target_words!=reviewed.preview.target_words||p.alternates!=reviewed.alternates||p.layers!=reviewed.layers||p.preview.rows.size()!=reviewed.preview.rows.size()){error="The transparency palette or source data changed. Cancel and reopen the review.";return -1;}
        int count=0;for(size_t i=0;i<p.preview.rows.size();++i){auto &r=reviewed.preview.rows[i];if(!same_row(r,p.preview.rows[i])){error="The transparency review no longer matches its source pixels or settings.";return -1;}if(!r.enabled)continue;if(!r.error.empty()){error=r.name+": "+r.error;return -1;}count+=r.changed;}
        if(!count)return 0;State before=capture();before.label="Make palette colors transparent";past.reserve(past.size()+1);
        for(size_t i=0;i<p.preview.rows.size();++i){auto &r=p.preview.rows[i];if(reviewed.preview.rows[i].enabled&&r.changed)memcpy(sprite(sprite_index(r.sprite))->data_p,r.after.data(),r.after.size());}
        past.push_back(std::move(before));future.clear();revision=next_revision++;++serial;trim_history();return count;
    }catch(const std::bad_alloc &){error="Not enough memory to remove pixels. Artwork and history are unchanged.";return -1;}
}
}
