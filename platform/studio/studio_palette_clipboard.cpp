#include "studio_palette.h"
#include "palette_math.h"
#include <algorithm>
#include <set>
namespace studio {
namespace {
std::string key(std::string name){for(char &c:name)if(c>='a'&&c<='z')c-=32;return name;}
bool valid_entry(const PaletteCopyEntry &e){return !e.fields.nxt_p&&!e.fields.data_p&&!e.fields.temp&&!e.fields.n_s[9]&&e.fields.numc>=1&&e.fields.numc<=256&&e.fields.bitspix<=8&&e.words.size()==e.fields.numc;}
struct DeletePalette {void operator()(PAL *p)const{if(p){free(p->data_p);free(p);}}};
}
bool Artwork::copy_palettes(const std::vector<int> &indices,PaletteTransfer &out,std::string &error) const{
    error.clear();if(pending){error="Finish the current edit before copying palettes.";return false;}
    if(indices.empty()||indices.size()>4093){error="Choose at least one palette to copy.";return false;}
    try{
        auto order=indices;std::sort(order.begin(),order.end());
        if(std::adjacent_find(order.begin(),order.end())!=order.end()){error="A palette occurs twice in the copy selection.";return false;}
        PaletteTransfer next;next.entries.reserve(order.size());
        for(int index:order){auto pal=palette(index);if(!pal||!pal->data_p||!pal->numc||pal->numc>256||pal->bitspix>8||pal->n_s[9]){error="A selected palette has invalid names, colors or depth. The previous clipboard was kept.";return false;}
            PaletteCopyEntry e;e.fields=*pal;e.fields.nxt_p=e.fields.data_p=e.fields.temp=nullptr;
            e.words.reserve(pal->numc);for(int c=0;c<pal->numc;++c)e.words.push_back(palette_word_at((const unsigned char *)pal->data_p,c));next.entries.push_back(std::move(e));
        }
        next.name=palette_name(next.entries.front().fields);next.words=next.entries.front().words;
        out=std::move(next);return true;
    }catch(const std::bad_alloc &){error="Not enough memory to copy every selected palette. The previous clipboard was kept.";return false;}
}
int Artwork::paste_palettes(const PaletteTransfer &clip,std::string &error,const char *history_label){
    error.clear();if(pending){error="Finish the current edit before pasting palettes.";return -1;}
    try{
        std::vector<PaletteCopyEntry> single;auto entries=&clip.entries;
        if(entries->empty()){
            // Existing single-palette PAL/ACT or programmatic color clipboard.
            if(clip.name.empty()||clip.name.size()>9||clip.words.empty()||clip.words.size()>256){error="The palette clipboard is empty or invalid.";return -1;}
            PaletteCopyEntry e;memcpy(e.fields.n_s,clip.name.data(),clip.name.size());e.fields.numc=(unsigned short)clip.words.size();e.fields.bitspix=(unsigned char)PaletteBppForColorCount(e.fields.numc);e.words=clip.words;single.push_back(std::move(e));entries=&single;
        }else if(clip.name!=palette_name(entries->front().fields)||clip.words!=entries->front().words){error="The palette clipboard no longer matches its captured entries. Copy it again.";return -1;}
        if(entries->size()>4093||palette_count()+(int)entries->size()>4093){error="Pasting every copied palette would exceed the 4093-palette IMG limit. Nothing was added.";return -1;}
        for(auto &e:*entries)if(!valid_entry(e)){error="A copied palette has invalid fields or colors. Nothing was added.";return -1;}
        std::set<std::string> used;for(int i=0;i<palette_count();++i)used.insert(key(palette_name(*palette(i))));
        std::vector<std::unique_ptr<PAL,DeletePalette>> prepared;prepared.reserve(entries->size());
        for(auto &e:*entries){
            auto source_name=palette_name(e.fields),name=source_name;std::string stem=name.empty()?"PALETTE":name;
            for(int n=2;used.count(key(name));++n){auto suffix="_"+std::to_string(n);name=stem.substr(0,9-suffix.size())+suffix;}
            used.insert(key(name));std::unique_ptr<PAL,DeletePalette> pal((PAL *)calloc(1,sizeof(PAL)));if(!pal)throw std::bad_alloc();*pal=e.fields;
            if(name!=source_name){memset(pal->n_s,0,10);memcpy(pal->n_s,name.data(),name.size());memset(pal->file_name_raw,0,10);}
            pal->data_p=malloc(e.words.size()*2);if(!pal->data_p)throw std::bad_alloc();
            auto bytes=(unsigned char *)pal->data_p;for(size_t c=0;c<e.words.size();++c){bytes[c*2]=(unsigned char)e.words[c];bytes[c*2+1]=(unsigned char)(e.words[c]>>8);}prepared.push_back(std::move(pal));
        }
        State before=capture();before.label=history_label?history_label:prepared.size()==1?"Paste palette":"Paste palettes";past.reserve(past.size()+1);
        int first=palette_count();PAL **tail=(PAL **)&data.pal_p;while(*tail)tail=(PAL **)&(*tail)->nxt_p;
        // Every allocation is complete. Publication, revision and history updates
        // below cannot fail halfway through a batch.
        for(auto &pal:prepared){*tail=pal.release();tail=(PAL **)&(*tail)->nxt_p;}
        data.palcnt+=(unsigned short)prepared.size();past.push_back(std::move(before));future.clear();revision=next_revision++;++serial;++structure_serial;trim_history();return first;
    }catch(const std::bad_alloc &){error="Not enough memory to paste every palette. No palettes were added.";return -1;}
}
}
