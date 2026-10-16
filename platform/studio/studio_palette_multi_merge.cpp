#include "studio_palette_multi_merge.h"
#include "palette_math.h"
#include <algorithm>
#include <numeric>
namespace studio {
namespace {
bool same_review(const PaletteMultiMergePlan &a,const PaletteMultiMergePlan &b){
    if(a.version!=b.version||a.palettes.size()!=b.palettes.size()||a.images.size()!=b.images.size()||a.sources.size()!=b.sources.size()||a.sprites!=b.sprites||a.palette_mapping!=b.palette_mapping||a.words!=b.words||a.target_after!=b.target_after||a.added!=b.added||a.removed!=b.removed||a.depth!=b.depth||a.approximate_colors!=b.approximate_colors||a.ppp_images!=b.ppp_images||a.skipped!=b.skipped||a.pixels!=b.pixels||a.index_changes!=b.index_changes||a.color_changes!=b.color_changes)return false;
    for(size_t i=0;i<a.palettes.size();++i)if(a.palettes[i].fields!=b.palettes[i].fields||a.palettes[i].words!=b.palettes[i].words)return false;
    for(size_t i=0;i<a.images.size();++i){auto &x=a.images[i];auto &y=b.images[i];if(x.id!=y.id||x.source!=y.source||x.fields!=y.fields||x.pixels!=y.pixels||x.alternate!=y.alternate)return false;}
    for(size_t i=0;i<a.sources.size();++i){auto &x=a.sources[i];auto &y=b.sources[i];if(x.palette!=y.palette||x.skipped!=y.skipped||x.alternate_refs!=y.alternate_refs||x.remove!=y.remove||x.mapping!=y.mapping||x.usage!=y.usage||x.sprites!=y.sprites||x.pixels!=y.pixels||x.index_changes!=y.index_changes||x.color_changes!=y.color_changes||x.approximate_colors!=y.approximate_colors||x.max_distance!=y.max_distance)return false;}
    return true;
}
}
bool plan_palette_multi_merge(const Artwork &art,const PaletteMultiMergeOptions &requested,PaletteMultiMergePlan &out,std::string &error){
    error.clear();if(art.palette_count()<1||art.palette_count()>4093||art.count()<1||art.count()>2000||requested.target<0||requested.target>=art.palette_count()||requested.ppp<0||requested.ppp>8||requested.sources.empty()||requested.sources.size()>4092){error="Choose at least one marked source and a destination, with PPP from 0 to 8.";return false;}
    try{
        PaletteMultiMergePlan p;p.owner=&art;p.generation=art.generation();p.version=art.disk_version();p.options=requested;auto &o=p.options;std::sort(o.sources.begin(),o.sources.end());
        for(size_t i=0;i<o.sources.size();++i)if(o.sources[i]<0||o.sources[i]>=art.palette_count()||o.sources[i]==o.target||(i&&o.sources[i]==o.sources[i-1])){error="Source palettes must be unique and different from the destination.";return false;}
        std::vector<int> by_palette(art.palette_count(),-1),primary(art.palette_count()),alternate(art.palette_count());
        for(int index:o.sources){PaletteMultiMergeSource s;s.palette=index;by_palette[index]=(int)p.sources.size();p.sources.push_back(std::move(s));}
        for(int i=0;i<art.palette_count();++i){auto pal=art.palette(i);if(!pal||!pal->data_p||pal->numc<1||pal->numc>256){error="A library palette has invalid color data.";return false;}PaletteMultiMergePalette snapshot;memcpy(snapshot.fields.data(),pal,sizeof(PAL));snapshot.words=copy_palette(art,i).words;p.palettes.push_back(std::move(snapshot));}
        size_t bytes=0;
        for(int i=0;i<art.count();++i){auto im=art.sprite(i);if(!im||!im->data_p||!im->w||!im->h||im->w>8192||im->h>8192||(size_t)im->w*im->h>4*1024*1024||im->palnum>=art.palette_count()){error="A sprite has invalid geometry, pixels or palette references.";return false;}
            if(im->layer_p){error="Flatten or remove library overlays before merging marked palettes.";return false;}
            PaletteMultiMergeImage snapshot;snapshot.id=art.sprite_id(i);memcpy(snapshot.fields.data(),im,sizeof(IMG));++primary[im->palnum];
            bool active=false;if(im->opaltbl_p){auto refs=(const unsigned char *)im->opaltbl_p;snapshot.alternate.assign(refs,refs+16);active=refs[0]<128;for(int j=0;j<16&&refs[j]<128;++j){if(refs[j]<NUMDEFPAL)continue;int ref=refs[j]-NUMDEFPAL;if(ref>=art.palette_count()){error="An alternate palette reference is unresolved. Repair it before merging.";return false;}++alternate[ref];}}
            int row=by_palette[im->palnum];if(row>=0){auto &source=p.sources[row];if(active){++source.skipped;++p.skipped;}else{
                snapshot.source=row;int stride=(im->w+3)&~3;size_t size=(size_t)stride*im->h;bytes+=size;if(bytes>128*1024*1024){error="The merge exceeds 128 MiB of source pixels.";return false;}auto data=(const unsigned char *)im->data_p;snapshot.pixels.assign(data,data+size);source.sprites.push_back(i);p.sprites.push_back(i);
                for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x){int c=data[y*stride+x];if(c>=(int)p.palettes[im->palnum].words.size()){error="Sprite '"+std::string(im->n_s)+"' has an unresolved color index.";return false;}if(c)++source.usage[c];}
            }}p.images.push_back(std::move(snapshot));
        }
        p.words=p.palettes[o.target].words;
        // Add the complete used-color union first. Source order is canonical;
        // destination slots never move, and opaque black cannot map to zero.
        if(o.grow)for(auto &s:p.sources){auto &words=p.palettes[s.palette].words;for(size_t c=1;c<words.size();++c)if(s.usage[c]){bool found=false;for(size_t d=1;d<p.words.size();++d)if(PaletteColorDistance5(words[c],p.words[d])==0){found=true;break;}if(!found&&p.words.size()<256){p.words.push_back(words[c]);++p.added;}}}
        PAL target=*art.palette(o.target);target.numc=(unsigned short)p.words.size();std::vector<unsigned char> packed(p.words.size()*2);for(size_t i=0;i<p.words.size();++i){packed[i*2]=(unsigned char)p.words[i];packed[i*2+1]=(unsigned char)(p.words[i]>>8);}target.data_p=packed.data();
        p.depth=p.added?std::max<int>(target.bitspix,PaletteBppForColorCount((int)p.words.size())):target.bitspix;
        for(auto &s:p.sources){auto &words=p.palettes[s.palette].words;s.alternate_refs=alternate[s.palette];s.remove=o.remove_unused&&!s.sprites.empty()&&!s.skipped&&!s.alternate_refs;
            for(size_t c=1;c<words.size();++c){int mapped=FindNearestMergedSlot(&target,(int)p.words.size(),nullptr,0,words[c],o.perceptual);s.mapping[c]=(unsigned char)mapped;if(!s.usage[c])continue;if(!mapped){error="The destination has no opaque colors. Enable growth or choose another destination.";return false;}int distance=PaletteColorDistance5(words[c],p.words[mapped]);s.max_distance=std::max(s.max_distance,distance);s.pixels+=s.usage[c];if(mapped!=(int)c)s.index_changes+=s.usage[c];if(distance){++s.approximate_colors;s.color_changes+=s.usage[c];}}
            p.pixels+=s.pixels;p.index_changes+=s.index_changes;p.color_changes+=s.color_changes;p.approximate_colors+=s.approximate_colors;
            if(o.ppp&&p.words.size()>(size_t)(1<<o.ppp)&&words.size()<=(size_t)(1<<o.ppp))p.ppp_images+=(int)s.sprites.size();
        }
        if(o.ppp&&p.words.size()>(size_t)(1<<o.ppp)&&p.palettes[o.target].words.size()<=(size_t)(1<<o.ppp))p.ppp_images+=primary[o.target];
        p.palette_mapping.resize(art.palette_count());int next=0;for(int i=0;i<art.palette_count();++i){bool remove=by_palette[i]>=0&&p.sources[by_palette[i]].remove;p.palette_mapping[i]=remove?-1:next++;p.removed+=remove;}p.target_after=p.palette_mapping[o.target];
        out=std::move(p);return true;
    }catch(const std::bad_alloc &){error="Not enough memory to review marked palette merge.";return false;}
}
std::vector<unsigned char> render_palette_multi_merge(const Artwork &art,const PaletteMultiMergePlan &p,int index,bool merged){
    if(p.owner!=&art||p.generation!=art.generation()||index<0||index>=(int)p.images.size())return {};auto &image=p.images[index];if(image.source<0||image.source>=(int)p.sources.size())return {};IMG im;memcpy(&im,image.fields.data(),sizeof im);auto &source=p.sources[image.source];if(!im.w||!im.h||im.w>8192||im.h>8192||(size_t)im.w*im.h>4*1024*1024||image.pixels.size()!=(size_t)((im.w+3)&~3)*im.h||source.palette<0||source.palette>=(int)p.palettes.size())return {};auto &words=merged?p.words:p.palettes[source.palette].words;std::vector<unsigned char> rgba((size_t)im.w*im.h*4);int stride=(im.w+3)&~3;
    for(int y=0;y<im.h;++y)for(int x=0;x<im.w;++x){int c=image.pixels[(size_t)y*stride+x],mapped=merged?source.mapping[c]:c;if(mapped>=(int)words.size())return {};auto word=words[mapped];unsigned char raw[]={(unsigned char)word,(unsigned char)(word>>8)};auto q=rgba.data()+((size_t)y*im.w+x)*4;pal_word_to_rgb8(raw,q,q+1,q+2);q[3]=c?255:0;}return rgba;
}
int Artwork::merge_marked_palettes(const PaletteMultiMergePlan &reviewed,bool allow_approximation,std::string &error){
    error.clear();if(pending){error="Finish the current edit before merging palettes.";return -1;}if(reviewed.owner!=this||reviewed.generation!=generation()){error="The library changed. Cancel and reopen marked palette merge.";return -1;}
    PaletteMultiMergePlan checked;if(!plan_palette_multi_merge(*this,reviewed.options,checked,error))return -1;
    if(!same_review(reviewed,checked)){error="Palette data or sprite references changed after review. Reopen marked merge.";return -1;}auto &p=checked;
    if(p.color_changes&&!allow_approximation){error="Review and allow approximate colors before merging.";return -1;}if(p.sprites.empty())return 0;
    try{
        std::unique_ptr<void,decltype(&free)> colors(malloc(p.words.size()*2),free);if(!colors){error="Not enough memory for destination colors.";return -1;}auto raw=(unsigned char *)colors.get();for(size_t i=0;i<p.words.size();++i){raw[i*2]=(unsigned char)p.words[i];raw[i*2+1]=(unsigned char)(p.words[i]>>8);}
        std::vector<PAL *> kept,removed;for(int i=0;i<palette_count();++i)(p.palette_mapping[i]<0?removed:kept).push_back(palette(i));
        State before=capture();before.label="Merge marked palettes";past.reserve(past.size()+1);
        // Every allocation and validation is complete. The installation below
        // cannot fail halfway, and records exactly one destination Undo step.
        auto target=palette(p.options.target);free(target->data_p);target->data_p=colors.release();target->numc=(unsigned short)p.words.size();target->bitspix=(unsigned char)p.depth;
        for(int i=0;i<count();++i){auto im=sprite(i);auto &snapshot=p.images[i];if(snapshot.source>=0){auto &source=p.sources[snapshot.source];auto pixels=(unsigned char *)im->data_p;int stride=(im->w+3)&~3;for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x)pixels[(size_t)y*stride+x]=source.mapping[pixels[(size_t)y*stride+x]];im->palnum=(unsigned short)p.options.target;}
            im->palnum=(unsigned short)p.palette_mapping[im->palnum];if(im->opaltbl_p){auto refs=(unsigned char *)im->opaltbl_p;for(int j=0;j<16&&refs[j]<128;++j)if(refs[j]>=NUMDEFPAL)refs[j]=(unsigned char)(p.palette_mapping[refs[j]-NUMDEFPAL]+NUMDEFPAL);}
        }
        data.pal_p=kept.front();for(size_t i=0;i<kept.size();++i)kept[i]->nxt_p=i+1<kept.size()?kept[i+1]:nullptr;data.palcnt=(unsigned short)kept.size();for(auto pal:removed)FreePal(pal);
        past.push_back(std::move(before));future.clear();revision=next_revision++;++serial;if(p.removed)++structure_serial;trim_history();return (int)p.sprites.size();
    }catch(const std::bad_alloc &){error="Not enough memory to apply marked palette merge. The library is unchanged.";return -1;}
}
}
