#include "studio_costume.h"
#include "studio_layers.h"
#include "palette_ramp.h"
#include "palette_transfer.h"
#include <algorithm>
#include <set>
#include <stdexcept>
namespace studio {
namespace {
std::string upper(std::string s){for(char &c:s)if(c>='a'&&c<='z')c-=32;return s;}
std::string costume_name(const Artwork &art,const CostumeOptions &o){
    std::string stem=o.name;if(stem.empty()){char source[5]={};CostumeNameStem(art.palette(o.palette)->n_s,4,source);if(o.borrow){char ref[4]={};CostumeNameStem(art.palette(o.reference)->n_s,3,ref);stem=std::string(source[0]?source:"ART")+(ref[0]?ref:"REF")+"_P";}else stem=std::string(source[0]?source:"ART")+HueTag(o.color[0],o.color[1],o.color[2])+"_P";}
    stem=upper(stem);auto name=stem;auto exists=[&](const std::string &n){for(int i=0;i<art.palette_count();++i)if(upper(palette_name(*art.palette(i)))==n)return true;return false;};for(int n=2;exists(name);++n){auto suffix="_"+std::to_string(n);name=stem.substr(0,9-suffix.size())+suffix;}return name;
}
bool same_sources(const PaletteCompactionPlan &a,const PaletteCompactionPlan &b){
    if(a.palette_fields!=b.palette_fields||a.original!=b.original||a.source_sprites.size()!=b.source_sprites.size())return false;
    for(size_t i=0;i<a.source_sprites.size();++i){auto &x=a.source_sprites[i];auto &y=b.source_sprites[i];if(x.index!=y.index||x.id!=y.id||x.fields!=y.fields||x.pixels!=y.pixels||x.alternate!=y.alternate||x.layer!=y.layer)return false;}return true;
}
}
bool plan_costume(const Artwork &art,const CostumeOptions &o,const std::vector<uint64_t> &assignments,CostumePlan &out,std::string &error){
    error.clear();auto valid_palette=[&](int index){auto pal=art.palette(index);return pal&&pal->data_p&&pal->numc>=2&&pal->numc<=256&&pal->bitspix<=8&&!pal->n_s[9];};
    if(!valid_palette(o.palette)||o.strength<0||o.strength>100||(!o.borrow&&o.slots[0])||o.name.size()>9||std::any_of(o.name.begin(),o.name.end(),[](unsigned char c){return !((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_');})){error="Choose a valid source palette, tint strength from 0 to 100, and a name up to 9 letters/digits/underscores.";return false;}
    if(o.borrow&&(!valid_palette(o.reference)||o.reference==o.palette||o.sections.empty()||o.sections.size()>16||(o.coverage!=CostumeCoverage::Equal&&o.coverage!=CostumeCoverage::Palette&&o.coverage!=CostumeCoverage::Family))){error="Choose a different valid reference palette, 1-16 named sections and a coverage mode.";return false;}
    if(art.palette_count()>=4093){error="The IMG already has 4093 palettes. No costume copy can be added.";return false;}
    try{
        CostumePlan p;p.options=o;auto pal=art.palette(o.palette);
        std::array<bool,256> destination_slots{},reference_slots{};std::set<std::string> names;
        if(o.borrow){
            for(const auto &section:o.sections){
                auto name=section.name;while(!name.empty()&&name.back()==' ')name.pop_back();auto first=name.find_first_not_of(' ');name=first==std::string::npos?"":name.substr(first);
                if(name.empty()||name.size()>32||std::any_of(name.begin(),name.end(),[](unsigned char c){return c<32||c==127;})||!names.insert(upper(name)).second){error="Each material section needs a unique name, up to 32 characters.";return false;}
                if(!section.enabled)continue;
                CostumeSectionReview row;row.name=name;
                auto parse=[&](const std::string &text,int numc,std::array<bool,256> &taken,std::vector<int> &slots){if(text.size()>1023)return false;TransferBlock ranges[256];int n=ParseSlotRanges(text.c_str(),numc,ranges,256);if(n<0)return false;std::array<bool,256> selected{};for(int i=0;i<n;++i)for(int c=ranges[i].start;c<ranges[i].start+ranges[i].count;++c)selected[c]=true;for(int c=1;c<numc;++c)if(selected[c]){if(taken[c])return false;taken[c]=true;slots.push_back(c);}return !slots.empty();};
                if(!parse(section.destination,pal->numc,destination_slots,row.destination)||!parse(section.reference,art.palette(o.reference)->numc,reference_slots,row.reference)){error="Section '"+name+"' needs opaque ranges on both palettes. Enabled sections cannot share slots on either side.";return false;}
                p.sections.push_back(std::move(row));
            }
            if(p.sections.empty()){error="Enable at least one material section to borrow.";return false;}
        }else{for(int c=1;c<256;++c)if(o.slots[c]){if(c>=pal->numc){error="A selected color lies outside the source palette.";return false;}destination_slots[c]=true;}}
        p.selected_slots=(int)std::count(destination_slots.begin(),destination_slots.end(),true);if(!p.selected_slots){error="Choose at least one opaque ramp or slot range to tint.";return false;}
        if(assignments.size()>2000){error="Choose at most 2000 sprites for costume assignment.";return false;}std::set<uint64_t> chosen;for(auto id:assignments){int index=art.sprite_index(id);if(index<0||!chosen.insert(id).second||art.sprite(index)->palnum!=o.palette){error="Costume assignments contain a missing/repeated sprite or a different source palette.";return false;}}
        // Bound all captured sources before allocating each owned pixel snapshot.
        size_t bytes=0;int users=0;
        auto snapshot=[&](int index,PaletteCompactionPlan &plan){
            for(int i=0;i<art.count();++i){auto im=art.sprite(i);bool used=im->palnum==index;if(im->opaltbl_p){auto refs=(const unsigned char *)im->opaltbl_p;for(int a=0;a<16&&refs[a]<128;++a)used|=refs[a]==index+NUMDEFPAL;}if(!used)continue;
                if(++users>(o.borrow?4000:2000)||!im->data_p||!im->w||!im->h||im->w>8192||im->h>8192||(size_t)im->w*im->h>4u*1024u*1024u){error="A costume source user has invalid pixels/dimensions, or the review exceeds its captured-user limit (2000 tint / 4000 borrowing).";return false;}
                size_t size=(size_t)((im->w+3)&~3)*im->h;bytes+=size;if(im->layer_p){bytes+=checked_layer_bytes(*im);if(img_layer(im)->visible)bytes+=size;}if(bytes>256u*1024u*1024u){error="Costume review exceeds the 256 MiB pixel budget. Split the library first.";return false;}
            }
            return plan_palette_compaction(art,{index,0,false,false,PaletteColorOrder::Keep,true},plan,error);
        };
        if(!snapshot(o.palette,p.preview))return false;
        for(int i:p.preview.primary_sprites)if(chosen.count(art.sprite_id(i)))p.assignments.push_back(art.sprite_id(i));
        auto &v=p.preview;v.words=v.original;for(size_t c=0;c<v.original.size();++c)v.mapping[c]=(unsigned char)c;
        if(o.borrow){
            p.reference_words=copy_palette(art,o.reference).words;
            std::array<double,256> destination_weight{},reference_weight{};
            for(int index=0;index<art.palette_count();++index){
                auto candidate=art.palette(index);if(!candidate||!candidate->data_p||candidate->numc<2||candidate->numc>256)continue;
                auto words=copy_palette(art,index).words;
                bool dst=index==o.palette,ref=index==o.reference;
                if(o.coverage==CostumeCoverage::Family){dst|=IsCostumeFamily(v.original.data(),(int)v.original.size(),words.data(),(int)words.size());ref|=IsCostumeFamily(p.reference_words.data(),(int)p.reference_words.size(),words.data(),(int)words.size());}
                if(!dst&&!ref)continue;
                const PaletteCompactionPlan *source=&v;
                if(index!=o.palette){PaletteCompactionPlan owned;if(!snapshot(index,owned))return false;p.coverage_sources.push_back(std::move(owned));source=&p.coverage_sources.back();}
                if(dst){++p.destination_palettes;p.destination_sprites+=(int)source->primary_sprites.size();}
                if(ref){++p.reference_palettes;p.reference_sprites+=(int)source->primary_sprites.size();}
                // Match classic coverage: base pixels only, no padding, overlays or alternate-only users.
                for(const auto &im:source->source_sprites){IMG fields;memcpy(&fields,im.fields.data(),sizeof fields);if(fields.palnum!=index)continue;int stride=(fields.w+3)&~3;for(int y=0;y<fields.h;++y)for(int x=0;x<fields.w;++x){int c=im.pixels[(size_t)y*stride+x];if(dst)++destination_weight[c];if(ref)++reference_weight[c];}}
            }
            for(auto &row:p.sections){std::vector<double> dw,rw;for(int c:row.destination){dw.push_back(destination_weight[c]);row.destination_pixels+=(uint64_t)destination_weight[c];}for(int c:row.reference){rw.push_back(reference_weight[c]);row.reference_pixels+=(uint64_t)reference_weight[c];}
                BorrowRampByCoverage(v.original.data(),row.destination.data(),o.coverage==CostumeCoverage::Equal?nullptr:dw.data(),(int)dw.size(),p.reference_words.data(),row.reference.data(),o.coverage==CostumeCoverage::Equal?nullptr:rw.data(),(int)rw.size(),v.words.data());
            }
            for(size_t c=1;c<v.words.size();++c)if(destination_slots[c])v.words[c]=(unsigned short)((v.words[c]&0x7fff)|(v.original[c]&0x8000));
        }else for(size_t c=1;c<v.words.size();++c)if(o.slots[c]){auto w=v.original[c];RampColor color{(unsigned char)((w>>10)&31),(unsigned char)((w>>5)&31),(unsigned char)(w&31)};ColorizeRamp(&color,1,o.color[0],o.color[1],o.color[2],o.strength/100.0,&color);v.words[c]=(unsigned short)(RampColorWord(color)|(w&0x8000));}
        for(size_t c=1;c<v.words.size();++c)p.changed_slots+=v.words[c]!=v.original[c];
        v.preview_sprites=v.primary_sprites;v.removed=0;v.index_changes=0;p.name=costume_name(art,o);out=std::move(p);return true;
    }catch(const std::bad_alloc &){error="Not enough memory to review the costume copy. The library is unchanged.";return false;}
    catch(const std::runtime_error &e){error=e.what();return false;}
}
int Artwork::create_costume_palette(const CostumePlan &reviewed,std::string &error){
    error.clear();if(pending||reviewed.preview.owner!=this||reviewed.preview.generation!=generation()){error="The library changed or an edit is pending. Cancel and reopen the costume review.";return -1;}
    try{
        CostumePlan p;if(!plan_costume(*this,reviewed.options,reviewed.assignments,p,error))return -1;
        bool sources_match=reviewed.coverage_sources.size()==p.coverage_sources.size();for(size_t i=0;sources_match&&i<p.coverage_sources.size();++i)sources_match=reviewed.coverage_sources[i].options.palette==p.coverage_sources[i].options.palette&&same_sources(reviewed.coverage_sources[i],p.coverage_sources[i]);
        if(!sources_match||!same_sources(reviewed.preview,p.preview)||reviewed.reference_words!=p.reference_words||reviewed.name!=p.name||reviewed.preview.words!=p.preview.words||reviewed.assignments!=p.assignments){error="The costume source, result name/colors or assignments changed. Cancel and reopen the review.";return -1;}
        std::vector<IMG *> chosen;chosen.reserve(p.assignments.size());for(auto id:p.assignments)chosen.push_back(sprite(sprite_index(id)));
        PaletteTransfer clip;if(!copy_palettes({p.options.palette},clip,error))return -1;auto &entry=clip.entries.front();memset(entry.fields.n_s,0,10);memcpy(entry.fields.n_s,p.name.data(),p.name.size());memset(entry.fields.file_name_raw,0,10);entry.words=p.preview.words;clip.name=p.name;clip.words=entry.words;
        // Paste preallocates the palette and history, then publishes once. All
        // pointers/assignments are prepared first; no allocation can follow it.
        int added=paste_palettes(clip,error,"Create costume palette");if(added<0)return -1;for(auto im:chosen)im->palnum=(unsigned short)added;return added;
    }catch(const std::bad_alloc &){error="Not enough memory to create the costume copy. Palettes, assignments and history are unchanged.";return -1;}
}
}
