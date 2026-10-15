#include "studio_animation_copy.h"
#include "anipoint.h"
#include <algorithm>
#include <map>
namespace studio {
namespace {
std::string key(std::string s){for(char &c:s)if(c>='a'&&c<='z')c-=32;return trim_sprite_name(s);}
bool valid_name(const std::string &s){return !s.empty()&&s.size()<=16&&std::all_of(s.begin(),s.end(),[](unsigned char c){return c>=32&&c<127;});}
std::vector<unsigned char> blob(const Artwork &art){auto p=(const unsigned char *)art.data.scrseqmem_p;return p?std::vector<unsigned char>(p,p+art.data.scrseqbytes):std::vector<unsigned char>{};}
void word(unsigned char *p,int n){p[0]=(unsigned char)n;p[1]=(unsigned char)(n>>8);}
void append(std::vector<unsigned char> &out,const EmbeddedRecord &r,const std::map<int,int> *mapping,const std::string *name){
    auto header=r.raw;if(name){memcpy(header.data(),name->data(),name->size());if(name->size()<16)header[name->size()]=0;}out.insert(out.end(),header.begin(),header.end());
    for(size_t i=0;i<r.entries.size();++i){auto &entry=r.entries[r.script?i:r.entries.size()-1-i];auto raw=entry.raw;if(mapping&&entry.values.index>=0)word(raw.data()+4,mapping->at(entry.values.index));out.insert(out.end(),raw.begin(),raw.end());}
}
bool same(const AnimationCopyRow &a,const AnimationCopyRow &b){return a.source==b.source&&a.target==b.target&&a.entries==b.entries&&a.script==b.script&&a.included==b.included&&a.dependency==b.dependency&&a.name==b.name&&a.copied_name==b.copied_name&&a.error==b.error;}
bool same(const AnimationCopyBinding &a,const AnimationCopyBinding &b){return a.source==b.source&&a.target==b.target&&a.source_id==b.source_id&&a.target_id==b.target_id&&a.manual==b.manual&&a.copied==b.copied&&a.name==b.name&&a.target_name==b.target_name&&a.error==b.error&&a.source_fields==b.source_fields&&a.target_fields==b.target_fields;}
}
bool plan_animation_copy(const Artwork &source,const Artwork &target,const AnimationCopyOptions &o,AnimationCopyPlan &out,std::string &error){
    out={};error.clear();if(&source==&target||o.sprite_prefix.size()>15||std::any_of(o.sprite_prefix.begin(),o.sprite_prefix.end(),[](unsigned char c){return c<33||c>=127;})||o.prefix.size()>16||std::any_of(o.prefix.begin(),o.prefix.end(),[](unsigned char c){return c<32||c>=127;})){error="Choose different libraries, an animation prefix up to 16 ASCII characters and a sprite prefix up to 15 without spaces.";return false;}
    try{
        std::vector<EmbeddedRecord> src,dst;if(!read_embedded(source,src,error)||!read_embedded(target,dst,error))return false;
        for(int r:o.requested)if(r<0||r>=(int)src.size()){error="A selected source animation no longer exists.";return false;}
        for(auto &binding:o.sprite_bindings)if(source.sprite_index(binding.first)<0||target.sprite_index(binding.second)<0){error="A manual sprite binding no longer exists in the chosen libraries. Reset the binding and choose again.";return false;}
        for(auto &r:dst)for(auto &e:r.entries)if(e.values.index>=0&&e.values.index>=(r.script?(int)target.data.seqcnt:target.count())){error="An existing destination animation has an unresolved target. Repair it before copying animations.";return false;}
        AnimationCopyPlan p;p.source=&source;p.target=&target;p.source_generation=source.generation();p.target_generation=target.generation();p.source_sequences=source.data.seqcnt;p.source_scripts=source.data.scrcnt;p.target_sequences=target.data.seqcnt;p.target_scripts=target.data.scrcnt;p.options=o;p.source_blob=blob(source);p.target_blob=blob(target);
        std::set<int> included=o.requested;for(int i:o.requested)if(src[i].script)for(auto &e:src[i].entries)if(e.values.index>=0&&e.values.index<(int)source.data.seqcnt)included.insert(e.values.index);
        std::map<std::string,int> names;for(auto &r:dst)++names[key(r.name)];for(int i:included)++names[key(o.prefix+src[i].name)];
        std::map<int,int> sequence_map;std::set<int> sprites;
        for(int i=0;i<(int)src.size();++i){auto &r=src[i];AnimationCopyRow row;row.source=i;row.entries=(int)r.entries.size();row.script=r.script;row.included=included.count(i)!=0;row.dependency=row.included&&!o.requested.count(i);row.name=r.name;row.copied_name=o.prefix+r.name;
            if(row.included){std::string original;for(unsigned char c:r.raw){if(original.size()==16||!c)break;original+=(char)c;}if(!valid_name(original))row.error="Rename this source animation to 1 to 16 printable ASCII characters before copying.";else if(!valid_name(row.copied_name))row.error="The copied animation name must fit 1 to 16 printable ASCII characters.";else if(names[key(row.copied_name)]>1)row.error="Animation name collision. Change the prefix or exclude the animation.";
                if(row.script){row.target=(int)target.data.scrcnt+p.added_scripts++;for(auto &e:r.entries)if(e.values.index>=(int)source.data.seqcnt)row.error="The script has an unresolved source sequence call.";}
                else{row.target=(int)target.data.seqcnt+p.added_sequences++;sequence_map[i]=row.target;for(auto &e:r.entries)if(e.values.index>=0)sprites.insert(e.values.index);}}
            if(row.included&&!row.script&&std::any_of(r.raw.begin()+88,r.raw.begin()+94,[](unsigned char n){return n!=255;}))row.error="This sequence uses damage-table bindings. Copying those bindings is not supported yet.";
            if(row.included&&target.disk_version()<0x634){bool fits=true;for(int j=0;j<16;++j)fits&=!r.raw[22+j*4]&&!r.raw[23+j*4];for(int j=88;j<94;++j)fits&=r.raw[j]==255;fits&=!r.raw[94]&&!r.raw[95];for(auto &e:r.entries)fits&=!e.raw[2]&&!e.raw[3];if(!fits)row.error="Stored far-pointer bytes cannot fit the destination's original IMG layout.";}
            p.rows.push_back(std::move(row));
        }
        std::map<std::string,std::vector<int>> source_names,target_names;for(int i=0;i<source.count();++i)source_names[key(source.sprite(i)->n_s)].push_back(i);for(int i=0;i<target.count();++i)target_names[key(target.sprite(i)->n_s)].push_back(i);
        std::map<int,int> sprite_map;std::vector<uint64_t> missing;
        for(int index:sprites){
            AnimationCopyBinding binding;binding.source=index;auto im=source.sprite(index);
            if(!im)binding.error="Unresolved source sprite index.";
            else{
                binding.name=im->n_s;binding.target_name=o.sprite_prefix+binding.name;binding.source_id=source.sprite_id(index);memcpy(binding.source_fields.data(),im,sizeof(IMG));
                auto manual=o.sprite_bindings.find(binding.source_id);
                if(manual!=o.sprite_bindings.end()){binding.manual=true;binding.target=target.sprite_index(manual->second);}
                else{
                    auto matches=target_names.find(key(binding.target_name));
                    if(source_names[key(binding.name)].size()!=1)binding.error="Ambiguous source name. Choose a destination sprite.";
                    else if(matches==target_names.end()){
                        if(o.copy_missing){binding.copied=true;binding.target=target.count()+(int)missing.size();missing.push_back(binding.source_id);}
                        else binding.error="No name match. Choose a sprite or enable Copy missing artwork.";
                    }
                    else if(matches->second.size()!=1)binding.error="Ambiguous destination name. Choose a sprite.";
                    else binding.target=matches->second.front();
                }
                if(binding.target>=0&&!binding.copied){binding.target_name=target.sprite(binding.target)->n_s;binding.target_id=target.sprite_id(binding.target);memcpy(binding.target_fields.data(),target.sprite(binding.target),sizeof(IMG));sprite_map[index]=binding.target;}
            }
            if(binding.copied)sprite_map[index]=binding.target;
            p.bindings.push_back(std::move(binding));
        }
        if(!missing.empty()){
            LibraryCopyOptions copy;copy.prefix=o.sprite_prefix;
            if(!plan_library_copy(source,target,missing,copy,p.artwork,error))return false;
            for(auto &binding:p.bindings)if(binding.copied){
                auto row=std::find_if(p.artwork.rows.begin(),p.artwork.rows.end(),[&](const LibraryCopyRow &r){return r.sprite==binding.source_id;});
                if(row==p.artwork.rows.end()){error="A required artwork copy is missing from the review.";return false;}
                binding.error=row->error;IMG fields{};memcpy(&fields,row->fields.data(),sizeof fields);memset(fields.n_s,0,16);memcpy(fields.n_s,row->copied_name.data(),std::min(size_t(15),row->copied_name.size()));fields.palnum=(unsigned short)row->target_palette;memcpy(binding.target_fields.data(),&fields,sizeof fields);
            }
            if(target.count()+missing.size()>2000||target.palette_count()+p.artwork.added_palettes>4093){error="The animation artwork would exceed the library sprite or palette limit.";return false;}
            size_t pixels=p.artwork.added_pixels;for(int i=0;i<target.count();++i){auto im=target.sprite(i);pixels+=(((size_t)im->w+3)&~size_t(3))*im->h;}
            if(pixels>128*1024*1024){error="The destination would exceed 128 MiB of decoded pixels.";return false;}
        }
        bool ready=std::none_of(p.rows.begin(),p.rows.end(),[](const AnimationCopyRow &r){return r.included&&!r.error.empty();})&&std::none_of(p.bindings.begin(),p.bindings.end(),[](const AnimationCopyBinding &b){return !b.error.empty();});
        if(dst.size()+included.size()>4096){error="The destination would exceed 4096 embedded records.";return false;}
        size_t total=p.target_blob.size();for(int i:included)total+=98+src[i].entries.size()*18;if(total>32*1024*1024){error="The destination embedded records would exceed 32 MiB.";return false;}
        if(ready){for(unsigned i=0;i<target.data.seqcnt;++i)append(p.result_blob,dst[i],nullptr,nullptr);for(unsigned i=0;i<source.data.seqcnt;++i)if(included.count(i))append(p.result_blob,src[i],&sprite_map,o.prefix.empty()?nullptr:&p.rows[i].copied_name);for(size_t i=target.data.seqcnt;i<dst.size();++i)append(p.result_blob,dst[i],nullptr,nullptr);for(size_t i=source.data.seqcnt;i<src.size();++i)if(included.count((int)i))append(p.result_blob,src[i],&sequence_map,o.prefix.empty()?nullptr:&p.rows[i].copied_name);}
        out=std::move(p);return true;
    }catch(const std::bad_alloc &){error="Not enough memory for the animation-copy review.";return false;}
}
int Artwork::validate_animation_copy(const Artwork &source,const AnimationCopyPlan &p,std::string &error) const{
    try{
    error.clear();if(pending||source.pending){error="Finish edits in both libraries before copying animations.";return -1;}if(p.source!=&source||p.target!=this||p.source_generation!=source.generation()||p.target_generation!=generation()){error="A library changed. Cancel and reopen animation copy.";return -1;}
    AnimationCopyPlan checked;if(!plan_animation_copy(source,*this,p.options,checked,error))return -1;
    if(p.source_sequences!=checked.source_sequences||p.source_scripts!=checked.source_scripts||p.target_sequences!=checked.target_sequences||p.target_scripts!=checked.target_scripts||p.source_blob!=checked.source_blob||p.target_blob!=checked.target_blob||p.result_blob!=checked.result_blob||p.added_sequences!=checked.added_sequences||p.added_scripts!=checked.added_scripts||p.rows.size()!=checked.rows.size()||p.bindings.size()!=checked.bindings.size()||!same_library_copy_plan(p.artwork,checked.artwork)){error="The animation-copy review no longer matches the libraries.";return -1;}
    for(size_t i=0;i<p.rows.size();++i)if(!same(p.rows[i],checked.rows[i])){error="An animation no longer matches the review.";return -1;}
    for(size_t i=0;i<p.bindings.size();++i)if(!same(p.bindings[i],checked.bindings[i])){error="A sprite binding no longer matches the review.";return -1;}
    for(auto &r:p.rows)if(r.included&&!r.error.empty()){error=r.name+": "+r.error;return -1;}for(auto &b:p.bindings)if(!b.error.empty()){error=b.name+": "+b.error;return -1;}
    return p.added_sequences+p.added_scripts;
    }catch(const std::bad_alloc &){error="Not enough memory to prepare animation copy. Please reopen the review.";return -1;}
}
int Artwork::apply_animation_copy(const Artwork &source,const AnimationCopyPlan &p,std::string &error){
    try{int count=validate_animation_copy(source,p,error);if(count<=0)return count;
        return install_animation_with_artwork(source,p.artwork,p.result_blob,data.seqcnt+p.added_sequences,data.scrcnt+p.added_scripts,error)?count:-1;
    }catch(const std::bad_alloc &){error="Not enough memory to prepare animation copy. Please reopen the review.";return -1;}
}
}
