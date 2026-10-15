#include "studio_batch_palette.h"
#include "studio_palette.h"
#include "palette_math.h"
#include <algorithm>
#include <set>
namespace studio {
namespace {
bool palette_ok(const PAL *p){return p&&p->data_p&&p->numc>=1&&p->numc<=256;}
bool equal_row(const BatchPaletteRow &a,const BatchPaletteRow &b){return a.sprite==b.sprite&&a.changed==b.changed&&a.alternates==b.alternates&&a.packing_risk==b.packing_risk&&a.source==b.source&&a.w==b.w&&a.h==b.h&&a.index_changes==b.index_changes&&a.color_changes==b.color_changes&&a.unknown_colors==b.unknown_colors&&a.name==b.name&&a.error==b.error&&a.fields==b.fields&&a.before==b.before&&a.after==b.after&&a.source_words==b.source_words;}
}
bool plan_batch_palette(const Artwork &art,const std::vector<uint64_t> &ids,const BatchPaletteOptions &o,BatchPalettePlan &out,std::string &error){
    out={};error.clear();auto target=art.palette(o.target);if(ids.empty()||ids.size()>2000||!palette_ok(target)||o.ppp<0||o.ppp>8){error="Choose sprites, a valid destination palette and PPP from 0 to 8.";return false;}
    std::set<uint64_t> seen;for(auto id:ids)if(art.sprite_index(id)<0||!seen.insert(id).second){error="The palette scope contains a missing or repeated sprite.";return false;}
    BatchPalettePlan p;p.owner=&art;p.generation=art.generation();p.options=o;p.target_words=copy_palette(art,o.target).words;size_t total=0;
    try{for(int index=0;index<art.count();++index){auto id=art.sprite_id(index);if(!seen.count(id))continue;auto im=art.sprite(index);BatchPaletteRow row;row.sprite=id;row.name=im->n_s;row.source=im->palnum;row.w=im->w;row.h=im->h;memcpy(row.fields.data(),im,sizeof(IMG));row.alternates=im->opaltbl_p&&((const unsigned char *)im->opaltbl_p)[0]<128;
        if(!im->data_p||row.w<1||row.h<1||row.w>8192||row.h>8192||(size_t)row.w*row.h>4*1024*1024){error="A sprite has invalid pixel data or dimensions.";return false;}size_t size=(((size_t)row.w+3)&~size_t(3))*row.h;total+=size*2;if(total>256*1024*1024){error="The palette preview exceeds its 256 MiB pixel limit.";return false;}
        row.before.assign((const unsigned char *)im->data_p,(const unsigned char *)im->data_p+size);row.after=row.before;auto source=art.palette(row.source);row.source_words=palette_ok(source)?copy_palette(art,row.source).words:std::vector<unsigned short>{};
        row.changed=row.source!=o.target;if(row.changed){
            if(!palette_ok(source))row.error="Source palette is unavailable.";
            else if(o.match&&target->numc<2)row.error="Matching needs at least one opaque destination color.";
            else{unsigned char mapping[256]={};if(o.match)for(int c=1;c<source->numc;++c)mapping[c]=nearest_palette_index_for_word(row.source_words[c],target);else for(int c=1;c<256;++c)mapping[c]=(unsigned char)c;
                int stride=(row.w+3)&~3;for(int y=0;y<row.h&&row.error.empty();++y)for(int x=0;x<row.w;++x){auto at=y*stride+x;int before=row.before[at];if(o.match&&before>=source->numc){row.error="Unresolved source indices cannot be matched. Keep indices or exclude this sprite.";break;}int after=mapping[before];if(after>=target->numc){row.error="Destination palette is too small for these indices. Match colors or exclude this sprite.";break;}
                    row.after[at]=(unsigned char)after;row.index_changes+=before!=after;if(before){bool unknown=before>=source->numc;row.unknown_colors+=unknown;row.color_changes+=unknown||PaletteColorDistance5(row.source_words[before],p.target_words[after])!=0;}}
                if(row.error.empty()&&row.alternates&&row.index_changes)row.error="Remapping would change alternate-palette appearances. Keep indices or exclude this sprite.";
            }row.packing_risk=o.ppp&&row.source_words.size()<=(size_t)(1<<o.ppp)&&p.target_words.size()>(size_t)(1<<o.ppp);
        }if(!row.error.empty())row.after=row.before;p.rows.push_back(std::move(row));
    }}catch(const std::bad_alloc &){error="Not enough memory for the palette preview.";return false;}out=std::move(p);return true;
}
std::vector<unsigned char> render_batch_palette(const BatchPalettePlan &p,int index,bool after){
    if(index<0||index>=(int)p.rows.size())return {};auto &r=p.rows[index];if(r.w<1||r.h<1||r.w>8192||r.h>8192||(size_t)r.w*r.h>4*1024*1024)return {};if(after&&!r.error.empty())return {};auto &pixels=after?r.after:r.before;auto &words=after&&r.changed?p.target_words:r.source_words;int stride=(r.w+3)&~3;
    if(pixels.size()!=(size_t)stride*r.h)return {};std::vector<unsigned char> rgba((size_t)r.w*r.h*4);for(int y=0;y<r.h;++y)for(int x=0;x<r.w;++x){int c=pixels[y*stride+x];auto out=rgba.data()+((size_t)y*r.w+x)*4;if(c>=(int)words.size()){out[0]=out[2]=255;out[1]=0;}else{unsigned short word=words[c];unsigned char bytes[]={(unsigned char)word,(unsigned char)(word>>8)};pal_word_to_rgb8(bytes,out,out+1,out+2);}out[3]=c?255:0;}return rgba;
}
int Artwork::apply_batch_palette(const BatchPalettePlan &p,bool allow_approximation,std::string &error){
    error.clear();if(pending){error="Finish the current edit before assigning palettes.";return -1;}if(p.owner!=this||p.generation!=generation()){error="The library changed. Cancel and reopen the palette review.";return -1;}
    std::vector<uint64_t> ids;for(auto &r:p.rows)ids.push_back(r.sprite);BatchPalettePlan checked;if(!plan_batch_palette(*this,ids,p.options,checked,error))return -1;
    if(p.target_words!=checked.target_words||p.rows.size()!=checked.rows.size()){error="The palette review no longer matches its destination.";return -1;}int count=0;
    for(size_t i=0;i<p.rows.size();++i){auto &r=p.rows[i];if(!equal_row(r,checked.rows[i])){error="The palette review no longer matches its source or settings.";return -1;}if(!r.enabled||!r.changed)continue;if(!r.error.empty()){error=r.name+": "+r.error;return -1;}if(p.options.match&&r.color_changes&&!allow_approximation){error="Review approximate matches and enable Allow approximate colors before applying.";return -1;}++count;}
    if(!count)return 0;begin("Batch assign palettes");for(auto &r:p.rows)if(r.enabled&&r.changed){auto im=sprite(sprite_index(r.sprite));memcpy(im->data_p,r.after.data(),r.after.size());im->palnum=(unsigned short)p.options.target;}++serial;commit();return count;
}
}
