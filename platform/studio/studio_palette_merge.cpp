#include "studio_palette.h"
#include "palette_math.h"
#include <algorithm>
namespace studio {
namespace {
bool active_alternates(const IMG &im){return im.opaltbl_p&&((const unsigned char *)im.opaltbl_p)[0]<128;}
}
bool plan_palette_merge(const Artwork &art,const PaletteMergeOptions &o,PaletteMergePlan &out,std::string &error){
    error.clear();auto source=art.palette(o.source),target=art.palette(o.target);
    if(!source||!target||o.source==o.target||o.ppp<0||o.ppp>8){error="Choose different source and destination palettes, with PPP from 0 to 8.";return false;}
    PaletteMergePlan p;p.owner=&art;p.generation=art.generation();p.options=o;p.source_words=copy_palette(art,o.source).words;p.target_words=p.words=copy_palette(art,o.target).words;
    if(p.words.empty()||p.words.size()>256||p.source_words.empty()||p.source_words.size()>256){error="The selected palettes do not have valid color data.";return false;}
    p.alternate_refs=palette_usage(art,o.source).alternate;
    for(int i=0;i<art.count();++i){auto im=art.sprite(i);if(im->palnum!=o.source)continue;if(active_alternates(*im)){++p.skipped_sprites;continue;}p.sprites.push_back(i);
        auto pixels=(const unsigned char *)im->data_p;int stride=(im->w+3)&~3;for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x){int c=pixels[y*stride+x];if(c>=(int)p.source_words.size()){error="Sprite '"+std::string(im->n_s)+"' has an unresolved color index. Fix it before merging.";return false;}if(c)++p.usage[c];}}
    if(p.sprites.empty()){error=p.skipped_sprites?"All source sprites use alternate palettes. This merge preserves them by skipping those sprites.":"The source palette has no primary sprites to merge.";return false;}
    if(o.remove_source&&(p.skipped_sprites||p.alternate_refs)){error="Keep the source palette: it is still used by skipped sprites or alternate references.";return false;}
    // Grow only for colors actually used by eligible source sprites. Existing
    // destination slots never move, and zero is never an opaque match.
    if(o.grow)for(size_t c=1;c<p.source_words.size();++c)if(p.usage[c]){bool found=false;for(size_t d=1;d<p.words.size();++d)if(PaletteColorDistance5(p.source_words[c],p.words[d])==0){found=true;break;}if(!found&&p.words.size()<256){p.words.push_back(p.source_words[c]);++p.added;}}
    PAL merged=*target;merged.numc=(unsigned short)p.words.size();std::vector<unsigned char> packed(p.words.size()*2);for(size_t i=0;i<p.words.size();++i){packed[i*2]=(unsigned char)p.words[i];packed[i*2+1]=(unsigned char)(p.words[i]>>8);}merged.data_p=packed.data();
    for(size_t c=1;c<p.source_words.size();++c){int mapped=FindNearestMergedSlot(&merged,(int)p.words.size(),nullptr,0,p.source_words[c],o.perceptual);p.mapping[c]=(unsigned char)mapped;if(!p.usage[c])continue;
        if(!mapped){error="The destination has no opaque colors. Enable palette growth or choose another palette.";return false;}int distance=PaletteColorDistance5(p.source_words[c],p.words[mapped]);p.max_distance=std::max(p.max_distance,distance);p.pixels+=p.usage[c];if(mapped!=(int)c)p.index_changes+=p.usage[c];if(distance){++p.approximate_colors;p.color_changes+=p.usage[c];}}
    p.depth=p.added?std::max<int>(target->bitspix,PaletteBppForColorCount((int)p.words.size())):target->bitspix;
    if(o.ppp){int limit=1<<o.ppp;if((int)p.words.size()>limit){if((int)p.source_words.size()<=limit)p.ppp_images+=(int)p.sprites.size();if((int)p.target_words.size()<=limit)p.ppp_images+=palette_usage(art,o.target).primary;}}
    if(o.remove_source)for(int i=0;i<art.count();++i){auto im=art.sprite(i);if(!im->opaltbl_p)continue;auto refs=(const unsigned char *)im->opaltbl_p;for(int a=0;a<16&&refs[a]<128;++a)if(refs[a]>=art.palette_count()+NUMDEFPAL){error="An alternate palette reference is unresolved. Keep the source palette or correct that reference first.";return false;}}
    out=std::move(p);return true;
}
std::vector<unsigned char> render_palette_merge(const Artwork &art,const PaletteMergePlan &plan,int index,bool merged){
    if(plan.owner!=&art||plan.generation!=art.generation()||std::find(plan.sprites.begin(),plan.sprites.end(),index)==plan.sprites.end())return {};auto im=art.sprite(index);if(!im)return {};
    std::vector<unsigned char> rgba((size_t)im->w*im->h*4);auto pixels=(const unsigned char *)im->data_p;int stride=(im->w+3)&~3;auto &words=merged?plan.words:plan.source_words;
    for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x){int c=pixels[y*stride+x];int mapped=merged?plan.mapping[c]:c;if(mapped>=(int)words.size())return {};unsigned short w=words[mapped];unsigned char bytes[]={(unsigned char)w,(unsigned char)(w>>8)};auto p=rgba.data()+((size_t)y*im->w+x)*4;pal_word_to_rgb8(bytes,p,p+1,p+2);p[3]=c?255:0;}return rgba;
}
bool Artwork::merge_palette(const PaletteMergePlan &reviewed,bool allow_approximation,std::string &error){
    error.clear();if(reviewed.owner!=this||reviewed.generation!=generation()){error="This library changed after the merge preview. Refresh it before applying.";return false;}
    PaletteMergePlan p;if(!plan_palette_merge(*this,reviewed.options,p,error))return false;if(p.color_changes&&!allow_approximation){error="This merge approximates used colors. Enable Allow approximate colors after reviewing the preview.";return false;}
    void *colors=malloc(p.words.size()*2);if(!colors){error="Not enough memory for the merged palette.";return false;}for(size_t i=0;i<p.words.size();++i){((unsigned char *)colors)[i*2]=(unsigned char)p.words[i];((unsigned char *)colors)[i*2+1]=(unsigned char)(p.words[i]>>8);}
    try{begin("Merge palette sprites");}catch(...){free(colors);throw;}auto target=palette(p.options.target);free(target->data_p);target->data_p=colors;target->numc=(unsigned short)p.words.size();target->bitspix=(unsigned char)p.depth;
    for(int index:p.sprites){auto im=sprite(index);auto pixels=(unsigned char *)im->data_p;int stride=(im->w+3)&~3;for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x)pixels[y*stride+x]=p.mapping[pixels[y*stride+x]];im->palnum=(unsigned short)p.options.target;}
    ++serial;if(p.options.remove_source){if(!delete_unused_palette(p.options.source,error)){cancel();return false;}}else commit();return true;
}
}
