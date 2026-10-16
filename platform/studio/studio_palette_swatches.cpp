#include "studio_palette_swatches.h"
#include "palette_math.h"
#include <algorithm>
namespace studio {
namespace {
bool valid(const PaletteSwatchClipboard &c){
    if(c.slots.empty()||c.slots.size()>256||c.source.size()>64)return false;
    int previous=-1;for(auto &v:c.slots){if(v.index<=previous||v.index>255)return false;previous=v.index;}return true;
}
bool equal(const PaletteSwatchPlan &a,const PaletteSwatchPlan &b){
    if(a.palette!=b.palette||a.slot!=b.slot||a.ppp!=b.ppp||a.mode!=b.mode||a.depth!=b.depth||a.changed!=b.changed||a.growth!=b.growth||a.skipped_zero!=b.skipped_zero||a.primary!=b.primary||a.alternate!=b.alternate||a.ppp_images!=b.ppp_images||memcmp(&a.fields,&b.fields,sizeof(PAL))||a.before!=b.before||a.words!=b.words||a.destinations!=b.destinations||a.clipboard.source!=b.clipboard.source||a.clipboard.slots.size()!=b.clipboard.slots.size())return false;
    for(size_t i=0;i<a.clipboard.slots.size();++i)if(a.clipboard.slots[i].index!=b.clipboard.slots[i].index||a.clipboard.slots[i].word!=b.clipboard.slots[i].word)return false;return true;
}
}
bool Artwork::copy_palette_swatches(int index,const std::array<bool,256> &selected,int highlighted,PaletteSwatchClipboard &out,std::string &error) const{
    error.clear();auto pal=palette(index);if(pending||!pal||!pal->data_p||!pal->numc||pal->numc>256){error="Finish the current edit and choose a valid source palette.";return false;}
    try{
        PaletteSwatchClipboard next;next.source=palette_name(*pal);
        for(int i=0;i<256;++i)if(selected[i]){if(i>=pal->numc){error="A selected swatch is outside this palette. The previous color clipboard was kept.";return false;}next.slots.push_back({i,palette_word_at((const unsigned char *)pal->data_p,i)});}
        if(next.slots.empty()){if(highlighted<0||highlighted>=pal->numc){error="Select a palette color first.";return false;}next.slots.push_back({highlighted,palette_word_at((const unsigned char *)pal->data_p,highlighted)});}
        out=std::move(next);return true;
    }catch(const std::bad_alloc &){error="Not enough memory to copy the selected colors. The previous color clipboard was kept.";return false;}
}
bool plan_palette_swatches(const Artwork &art,int index,const PaletteSwatchClipboard &clip,SwatchPaste mode,int slot,int ppp,PaletteSwatchPlan &out,std::string &error){
    error.clear();auto pal=art.palette(index);
    if(!pal||!pal->data_p||!pal->numc||pal->numc>256||pal->bitspix>8||!valid(clip)||ppp<0||ppp>8||(mode!=SwatchPaste::SameIndices&&mode!=SwatchPaste::Append&&mode!=SwatchPaste::AtSlot)){error="Choose a valid palette, copied colors and paste mode.";return false;}
    if(mode==SwatchPaste::AtSlot&&(clip.slots.size()!=1||slot<1||slot>=pal->numc)){error="Paste one copied color into an existing opaque slot (index 1 or above).";return false;}
    if(mode==SwatchPaste::Append&&pal->numc+clip.slots.size()>256){error="Every copied color must fit within 256 slots. Clear space or copy fewer colors; nothing will be dropped.";return false;}
    try{
        PaletteSwatchPlan p;p.owner=&art;p.generation=art.generation();p.palette=index;p.mode=mode;p.slot=slot;p.ppp=ppp;p.clipboard=clip;p.fields=*pal;p.fields.nxt_p=p.fields.temp=p.fields.data_p=nullptr;p.before=p.words=copy_palette(art,index).words;
        for(auto &v:clip.slots){int dest=mode==SwatchPaste::Append?(int)p.before.size()+(int)p.destinations.size():mode==SwatchPaste::AtSlot?slot:v.index;
            if(dest==0){++p.skipped_zero;p.destinations.push_back(-1);continue;}
            p.destinations.push_back(dest);if(dest>=(int)p.words.size())p.words.resize(dest+1,0);
            if(dest>=(int)p.before.size()||p.words[dest]!=v.word)++p.changed;p.words[dest]=v.word;
        }
        if((int)clip.slots.size()==p.skipped_zero){error="Index zero stays transparent and unchanged. Append its color or paste it into an opaque slot instead.";return false;}
        p.growth=(int)p.words.size()-(int)p.before.size();p.depth=p.growth?std::max<int>(pal->bitspix,PaletteBppForColorCount((int)p.words.size())):pal->bitspix;
        auto usage=palette_usage(art,index);p.primary=usage.primary;p.alternate=usage.alternate;
        if(ppp&&p.before.size()<=(size_t)(1<<ppp)&&p.words.size()>(size_t)(1<<ppp))p.ppp_images=p.primary;
        out=std::move(p);return true;
    }catch(const std::bad_alloc &){error="Not enough memory to prepare the color paste. Artwork is unchanged.";return false;}
}
bool Artwork::apply_palette_swatches(const PaletteSwatchPlan &reviewed,std::string &error){
    error.clear();if(pending||reviewed.owner!=this||reviewed.generation!=generation()){error="The library changed or an edit is pending. Cancel and reopen the color paste review.";return false;}
    PaletteSwatchPlan p;if(!plan_palette_swatches(*this,reviewed.palette,reviewed.clipboard,reviewed.mode,reviewed.slot,reviewed.ppp,p,error))return false;
    if(!equal(reviewed,p)){error="The color paste no longer matches its reviewed palette or mapping. Reopen the review.";return false;}
    if(!p.changed&&!p.growth)return true;
    try{
        std::unique_ptr<unsigned char,decltype(&free)> colors(nullptr,free);
        if(p.growth){colors.reset((unsigned char *)malloc(p.words.size()*2));if(!colors)throw std::bad_alloc();for(size_t i=0;i<p.words.size();++i){colors.get()[i*2]=(unsigned char)p.words[i];colors.get()[i*2+1]=(unsigned char)(p.words[i]>>8);}}
        State before=capture();before.label="Paste palette colors";past.reserve(past.size()+1);auto pal=palette(p.palette);
        if(p.growth){free(pal->data_p);pal->data_p=colors.release();pal->numc=(unsigned short)p.words.size();pal->bitspix=(unsigned char)p.depth;}
        else for(size_t i=0;i<p.words.size();++i){auto bytes=(unsigned char *)pal->data_p;bytes[i*2]=(unsigned char)p.words[i];bytes[i*2+1]=(unsigned char)(p.words[i]>>8);}
        past.push_back(std::move(before));future.clear();revision=next_revision++;++serial;trim_history();return true;
    }catch(const std::bad_alloc &){error="Not enough memory to apply every copied color. The palette and history were left unchanged.";return false;}
}
}
