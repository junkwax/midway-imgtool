#include "studio_sequences.h"
#include <algorithm>
#include <climits>

namespace studio {
namespace {
int word(const unsigned char *p){return p[0]|(p[1]<<8);}
int signed_word(const unsigned char *p){return (short)word(p);}
void set_word(unsigned char *p,int value){p[0]=(unsigned char)value;p[1]=(unsigned char)(value>>8);}
std::string name_of(const unsigned char *p){std::string out;for(int i=0;i<16&&p[i];++i)out+=(p[i]>=32&&p[i]<127)?(char)p[i]:'.';return out;}
bool valid_name(const std::string &name,std::string &error){
    if(name.empty()||name.size()>16||std::any_of(name.begin(),name.end(),[](unsigned char c){return c<32||c>=127;})){error="Use 1–16 printable ASCII characters for the animation name.";return false;}return true;
}
std::vector<unsigned char> encode(const std::vector<EmbeddedRecord> &records){
    std::vector<unsigned char> blob;
    for(auto &record:records){auto header=record.raw;set_word(header.data()+18,(int)record.entries.size());blob.insert(blob.end(),header.begin(),header.end());
        for(size_t e=0;e<record.entries.size();++e){auto &entry=record.entries[record.script?e:record.entries.size()-1-e];auto raw=entry.raw;
            set_word(raw.data()+4,entry.values.index);raw[6]=(unsigned char)entry.values.ticks;set_word(raw.data()+8,entry.values.dx);set_word(raw.data()+10,entry.values.dy);blob.insert(blob.end(),raw.begin(),raw.end());}
    }return blob;
}
bool pick(const Artwork &art,int record,std::vector<EmbeddedRecord> &records,std::string &error){
    if(!read_embedded(art,records,error))return false;
    if(record<0||record>=(int)records.size()){error="Choose an existing embedded animation.";return false;}return true;
}
bool target_valid(const Artwork &art,const EmbeddedRecord &record,int target,std::string &error){
    if(target<0||target>=(record.script?(int)art.data.seqcnt:art.count())){error=record.script?"Choose a sequence from this IMG.":"Choose a sprite from this IMG.";return false;}return true;
}
}
bool read_embedded(const Artwork &art,std::vector<EmbeddedRecord> &records,std::string &error){
    records.clear();error.clear();auto &data=art.data;
    if(data.fileversion<0x634||data.fileversion>0x654){error="Embedded editing currently supports IMG versions 0x634–0x654.";return false;}
    if(data.seqcnt>32767||data.scrcnt>32767||data.seqcnt+data.scrcnt>4096||data.scrseqbytes>32*1024*1024){error="Embedded animation data exceeds the editor limits.";return false;}
    if(data.scrseqbytes&&!data.scrseqmem_p){error="Embedded animation data is unavailable.";return false;}
    auto blob=(const unsigned char *)data.scrseqmem_p;size_t bytes=data.scrseqbytes,offset=0;
    std::vector<EmbeddedRecord> parsed;
    for(unsigned r=0;r<data.seqcnt+data.scrcnt;++r){
        if(offset>bytes||bytes-offset<98){error="Embedded animation header is truncated.";return false;}
        EmbeddedRecord record;record.script=r>=data.seqcnt;memcpy(record.raw.data(),blob+offset,98);record.name=name_of(record.raw.data());record.flags=word(record.raw.data()+16);record.start_x=signed_word(record.raw.data()+84);record.start_y=signed_word(record.raw.data()+86);
        unsigned count=word(blob+offset+18);offset+=98;
        if(count>4096||count>(bytes-offset)/18){error="Embedded entries are truncated or exceed the 4096-entry limit.";return false;}
        record.entries.resize(count);
        for(unsigned e=0;e<count;++e){auto &entry=record.entries[record.script?e:count-1-e];memcpy(entry.raw.data(),blob+offset+e*18,18);entry.values={signed_word(entry.raw.data()+4),(int)entry.raw[6],signed_word(entry.raw.data()+8),signed_word(entry.raw.data()+10)};}
        offset+=count*18;parsed.push_back(std::move(record));
    }
    if(offset!=bytes){error="Unexpected bytes after the embedded records. Editing is disabled to preserve them.";return false;}
    records=std::move(parsed);return true;
}
int embedded_duration(const EmbeddedRecord &record){int ticks=0;for(auto &e:record.entries)ticks+=std::max(1,e.values.ticks);return std::max(1,ticks);}
int embedded_frame_at(const EmbeddedRecord &record,int tick){if(tick<0)return -1;for(int f=0;f<(int)record.entries.size();++f){int hold=std::max(1,record.entries[f].values.ticks);if(tick<hold)return f;tick-=hold;}return -1;}
bool Artwork::install_embedded(const std::vector<unsigned char> &blob,unsigned sequences,unsigned scripts,const std::string &label,std::string &error){
    error.clear();if(blob.size()>32*1024*1024){error="Embedded animation data exceeds 32 MiB.";return false;}
    if(data.seqcnt==sequences&&data.scrcnt==scripts&&data.scrseqbytes==blob.size()&&(blob.empty()||!memcmp(data.scrseqmem_p,blob.data(),blob.size())))return true;
    void *replacement=blob.empty()?nullptr:malloc(blob.size());if(!blob.empty()&&!replacement){error="Could not allocate animation data.";return false;}if(replacement)memcpy(replacement,blob.data(),blob.size());
    try{begin(label);}catch(...){free(replacement);throw;}
    free(data.scrseqmem_p);data.scrseqmem_p=replacement;data.scrseqbytes=(unsigned)blob.size();data.seqcnt=sequences;data.scrcnt=scripts;++serial;return true;
}
bool Artwork::embedded_edit_entry(int record,int entry,const EmbeddedEntryValues &v,std::string &error){
    std::vector<EmbeddedRecord> records;if(!pick(*this,record,records,error))return false;auto &r=records[record];
    if(entry<0||entry>=(int)r.entries.size()){error="Choose an existing entry.";return false;}
    if(!target_valid(*this,r,v.index,error))return false;
    if(v.ticks<0||v.ticks>255||v.dx<-32768||v.dx>32767||v.dy<-32768||v.dy>32767){error="Ticks must be 0–255; offsets must fit signed 16-bit values.";return false;}
    r.entries[entry].values=v;return install_embedded(encode(records),data.seqcnt,data.scrcnt,"Edit embedded entry",error);
}
bool Artwork::embedded_append(int record,int target,std::string &error){
    std::vector<EmbeddedRecord> records;if(!pick(*this,record,records,error))return false;auto &r=records[record];if(!target_valid(*this,r,target,error))return false;
    if(r.entries.size()>=4096){error="This animation has reached its 4096-entry limit.";return false;}
    EmbeddedEntry entry;entry.values.index=target;entry.raw[6]=4;r.entries.push_back(entry);
    bool ok=install_embedded(encode(records),data.seqcnt,data.scrcnt,"Add embedded entry",error);if(ok)commit();return ok;
}
bool Artwork::embedded_duplicate(int record,int entry,std::string &error){
    std::vector<EmbeddedRecord> records;if(!pick(*this,record,records,error))return false;auto &entries=records[record].entries;
    if(entry<0||entry>=(int)entries.size()||entries.size()>=4096){error="Choose an entry below the 4096-entry limit.";return false;}
    auto copy=entries[entry];entries.insert(entries.begin()+entry+1,copy);bool ok=install_embedded(encode(records),data.seqcnt,data.scrcnt,"Duplicate embedded entry",error);if(ok)commit();return ok;
}
bool Artwork::embedded_delete(int record,int entry,std::string &error){
    std::vector<EmbeddedRecord> records;if(!pick(*this,record,records,error))return false;auto &entries=records[record].entries;
    if(entry<0||entry>=(int)entries.size()){error="Choose an existing entry.";return false;}entries.erase(entries.begin()+entry);
    bool ok=install_embedded(encode(records),data.seqcnt,data.scrcnt,"Delete embedded entry",error);if(ok)commit();return ok;
}
bool Artwork::embedded_move(int record,int entry,int destination,std::string &error){
    std::vector<EmbeddedRecord> records;if(!pick(*this,record,records,error))return false;auto &entries=records[record].entries;
    if(entry<0||entry>=(int)entries.size()||destination<0||destination>=(int)entries.size()){error="Choose an existing destination entry.";return false;}
    auto moving=entries[entry];entries.erase(entries.begin()+entry);entries.insert(entries.begin()+destination,moving);
    bool ok=install_embedded(encode(records),data.seqcnt,data.scrcnt,"Reorder embedded entries",error);if(ok)commit();return ok;
}
int Artwork::embedded_add_record(bool script,const std::string &name,std::string &error){
    if(!valid_name(name,error))return -1;std::vector<EmbeddedRecord> records;if(!read_embedded(*this,records,error))return -1;
    if(records.size()>=4096){error="This IMG has reached its 4096-animation limit.";return -1;}
    EmbeddedRecord record;record.script=script;record.name=name;memcpy(record.raw.data(),name.data(),name.size());if(!script)set_word(record.raw.data()+16,0x40);memset(record.raw.data()+88,0xff,6);
    int index=script?(int)records.size():(int)data.seqcnt;records.insert(records.begin()+index,std::move(record));
    if(!install_embedded(encode(records),data.seqcnt+!script,data.scrcnt+script,"New embedded animation",error))return -1;commit();return index;
}
bool Artwork::embedded_rename(int record,const std::string &name,std::string &error){
    if(!valid_name(name,error))return false;std::vector<EmbeddedRecord> records;if(!pick(*this,record,records,error))return false;
    auto &r=records[record];if(r.name==name)return true;memset(r.raw.data(),0,16);memcpy(r.raw.data(),name.data(),name.size());
    return install_embedded(encode(records),data.seqcnt,data.scrcnt,"Rename embedded animation",error);
}
std::unique_ptr<SceneProject> embedded_scene(const Artwork &art,uint64_t uid,int index,std::string &error,bool flip,MirrorConvention convention){
    std::vector<EmbeddedRecord> records;if(!pick(art,index,records,error))return {};auto &record=records[index];
    if(record.script){error="Open a called sequence to create its scene snapshot. Script runtime playback is not implemented.";return {};}
    if(record.entries.empty()){error="Add at least one frame before creating a scene.";return {};}
    SceneState state;state.convention=convention;state.assets.push_back({art.path(),art.path(),{},uid});SceneLane lane;lane.name=record.name;lane.flip_x=flip;
    for(auto &entry:record.entries){if(!target_valid(art,record,entry.values.index,error))return {};
        if(std::abs(entry.values.dx)>4096||std::abs(entry.values.dy)>4096){error="An embedded offset exceeds the scene's 4096-pixel editing range.";return {};}
        auto im=art.sprite(entry.values.index);SceneFrame f;f.name=im->n_s;f.ticks=std::max(1,entry.values.ticks);f.x=flip?entry.values.dx:-entry.values.dx;f.y=-entry.values.dy;f.pieces={scene_sprite_reference(art,0,entry.values.index)};lane.frames.push_back(std::move(f));}
    state.origin_y=state.floor-(art.sprite(record.entries.front().values.index)->h-(short)art.sprite(record.entries.front().values.index)->aniy)+record.entries.front().values.dy;
    state.lanes.push_back(std::move(lane));if(!validate_scene(state,error))return {};return std::make_unique<SceneProject>(std::move(state));
}
}
