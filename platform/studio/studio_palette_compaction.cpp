#include "studio_palette.h"
#include "palette_math.h"
#include <algorithm>
namespace studio {
bool plan_palette_compaction(const Artwork &art,const PaletteCompactionOptions &o,PaletteCompactionPlan &out,std::string &error){
    error.clear();auto pal=art.palette(o.palette);if(!pal||o.ppp<0||o.ppp>8){error="Choose a palette and a PPP value from 0 to 8.";return false;}
    PaletteCompactionPlan p;p.owner=&art;p.generation=art.generation();p.options=o;p.original=copy_palette(art,o.palette).words;
    if(p.original.empty()||p.original.size()>256){error="This palette has invalid color data.";return false;}
    for(int i=0;i<art.count();++i){auto im=art.sprite(i);bool primary=im->palnum==o.palette,alternate=false,active=false;if(im->opaltbl_p){auto refs=(const unsigned char *)im->opaltbl_p;active=refs[0]<128;for(int a=0;a<16&&refs[a]<128;++a)alternate|=refs[a]==o.palette+NUMDEFPAL;}
        if(!primary&&!alternate)continue;p.preview_sprites.push_back(i);if(primary)p.primary_sprites.push_back(i);if(alternate)++p.alternate_sprites;p.indices_locked|=alternate||(primary&&active);
        auto pixels=(const unsigned char *)im->data_p;int stride=(im->w+3)&~3;for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x){int c=pixels[y*stride+x];if(c>=(int)p.original.size()){error="Sprite '"+std::string(im->n_s)+"' references a color outside this palette. Compaction is disabled.";return false;}if(primary)++p.primary[c];if(alternate)++p.alternate[c];}}
    p.words.push_back(p.original[0]);int last=0;for(size_t c=1;c<p.original.size();++c)if(p.primary[c]||p.alternate[c])last=(int)c;else ++p.unused;
    bool pack=o.pack_live&&!p.indices_locked;
    for(size_t c=1;c<p.original.size();++c){if(pack?bool(p.primary[c]||p.alternate[c]):(int)c<=last){p.mapping[c]=(unsigned char)p.words.size();p.words.push_back(p.original[c]);}}
    p.removed=(int)p.original.size()-(int)p.words.size();p.depth=o.recompute_depth?PaletteBppForColorCount((int)p.words.size()):pal->bitspix;
    for(size_t c=1;c<p.original.size();++c)if(p.mapping[c]!=c)p.index_changes+=p.primary[c];
    if(o.ppp){int limit=1<<o.ppp;p.ppp_changed=((int)p.original.size()>limit)!=((int)p.words.size()>limit);}
    out=std::move(p);return true;
}
std::vector<unsigned char> render_palette_compaction(const Artwork &art,const PaletteCompactionPlan &p,int index,bool compacted){
    if(p.owner!=&art||p.generation!=art.generation()||std::find(p.preview_sprites.begin(),p.preview_sprites.end(),index)==p.preview_sprites.end())return {};auto im=art.sprite(index);if(!im)return {};auto &words=compacted?p.words:p.original;std::vector<unsigned char> rgba((size_t)im->w*im->h*4);
    int stride=(im->w+3)&~3;for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x){int c=((unsigned char *)im->data_p)[y*stride+x];int mapped=compacted?p.mapping[c]:c;if(mapped>=(int)words.size())return {};unsigned short w=words[mapped];unsigned char word[]={(unsigned char)w,(unsigned char)(w>>8)};auto dest=rgba.data()+((size_t)y*im->w+x)*4;pal_word_to_rgb8(word,dest,dest+1,dest+2);dest[3]=c?255:0;}return rgba;
}
bool Artwork::compact_palette(const PaletteCompactionPlan &reviewed,std::string &error){
    error.clear();if(reviewed.owner!=this||reviewed.generation!=generation()){error="The library changed after the usage preview. Refresh it before applying.";return false;}
    PaletteCompactionPlan p;if(!plan_palette_compaction(*this,reviewed.options,p,error))return false;auto pal=palette(p.options.palette);
    if(!p.removed&&pal->bitspix==p.depth&&!p.index_changes)return true;
    auto colors=(unsigned char *)malloc(p.words.size()*2);if(!colors){error="Not enough memory for compacted palette colors.";return false;}for(size_t i=0;i<p.words.size();++i){colors[i*2]=(unsigned char)p.words[i];colors[i*2+1]=(unsigned char)(p.words[i]>>8);}
    try{begin("Compact palette colors");}catch(...){free(colors);throw;}free(pal->data_p);pal->data_p=colors;pal->numc=(unsigned short)p.words.size();pal->bitspix=(unsigned char)p.depth;
    for(int index:p.primary_sprites){auto im=sprite(index);int stride=(im->w+3)&~3;for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x){auto pixel=(unsigned char *)im->data_p+y*stride+x;*pixel=p.mapping[*pixel];}}
    ++serial;commit();return true;
}
}
