#include "studio_embedded_metadata.h"
#include <algorithm>
namespace studio {
namespace {
int word(const unsigned char *p){return p[0]|(p[1]<<8);}
void set_word(unsigned char *p,int n){p[0]=(unsigned char)n;p[1]=(unsigned char)(n>>8);}
bool signed_word(int n){return n>=-32768&&n<=32767;}
}
bool capture_embedded_metadata(const Artwork &art,int record,int entry,std::shared_ptr<const EmbeddedMetadataSource> &out,std::string &error){
    error.clear();try{
        std::vector<EmbeddedRecord> records;if(!read_embedded(art,records,error))return false;
        if(record<0||record>=(int)records.size()||entry<-1||entry>=(int)records[record].entries.size()||(!records[record].entries.empty()&&entry<0)){error="Choose a stored animation and one of its entries, if present.";return false;}
        const auto &d=art.data;if(d.damcnt>32767||d.damtblbytes!=d.damcnt*4||(d.damtblbytes&&!d.damtbl_p)){error="The global damage-reference table is inconsistent. Metadata editing is disabled.";return false;}
        auto s=std::make_shared<EmbeddedMetadataSource>();s->owner=&art;s->generation=art.generation();s->disk_version=art.disk_version();s->version=d.fileversion;s->sequences=d.seqcnt;s->scripts=d.scrcnt;s->damage_count=d.damcnt;s->record=record;s->entry=entry;s->script=records[record].script;s->name=records[record].name;
        auto blob=(const unsigned char *)d.scrseqmem_p;s->blob.assign(blob,blob+d.scrseqbytes);if(d.damtblbytes){auto p=(const unsigned char *)d.damtbl_p;s->damage.assign(p,p+d.damtblbytes);}std::copy(d.file_bufscr,d.file_bufscr+4,s->buffer_script.begin());
        for(int i=0;i<record;++i)s->offset+=98+records[i].entries.size()*18;auto h=s->blob.data()+s->offset;s->original.flags=word(h+16);s->original.start_x=(short)word(h+84);s->original.start_y=(short)word(h+86);for(int i=0;i<6;++i)s->original.damage[i]=(signed char)h[88+i];
        if(entry>=0){s->entry_offset=s->offset+98+(s->script?entry:records[record].entries.size()-1-entry)*18;for(int i=0;i<3;++i)s->original.spare[i]=(short)word(s->blob.data()+s->entry_offset+12+i*2);}
        for(unsigned i=0;i<d.damcnt;++i){auto p=s->damage.data()+i*4;int flag=word(p),index=(short)word(p+2),r=flag==0x40?index:flag==0?(int)d.seqcnt+index:-1;bool valid=index>=0&&r>=0&&r<(int)records.size()&&((flag==0x40&&r<(int)d.seqcnt)||(flag==0&&r>=(int)d.seqcnt));s->damage_valid.push_back(valid);s->damage_labels.push_back("#"+std::to_string(i)+(valid?(flag==0x40?" sequence: ":" script: ")+records[r].name:" / unresolved target"));}
        out=std::move(s);return true;
    }catch(const std::bad_alloc &){error="Not enough memory to capture animation metadata. Artwork is unchanged.";return false;}
}
bool plan_embedded_metadata(std::shared_ptr<const EmbeddedMetadataSource> s,const EmbeddedMetadataValues &v,EmbeddedMetadataPlan &out,std::string &error){
    error.clear();if(!s||!s->owner||s->offset>s->blob.size()||s->blob.size()-s->offset<98||s->entry<-1||(s->entry>=0&&(s->entry_offset>s->blob.size()||s->blob.size()-s->entry_offset<18))||s->damage_labels.size()!=s->damage_count||s->damage_valid.size()!=s->damage_count){error="Choose a captured animation metadata review.";return false;}
    if(v.flags<0||v.flags>65535||!signed_word(v.start_x)||!signed_word(v.start_y)||std::any_of(v.spare.begin(),v.spare.end(),[](int n){return !signed_word(n);})){error="Flags must fit 16 bits; coordinates and spare words must fit signed 16-bit values.";return false;}
    if(s->entry<0&&v.spare!=s->original.spare){error="An empty animation has no entry spare words.";return false;}
    for(int i=0;i<6;++i){int n=v.damage[i];if(n<-128||n>127){error="Damage references must fit a signed byte.";return false;}if(n==s->original.damage[i])continue;if(n<-1||(n>=0&&(s->disk_version<0x634||n>=(int)s->damage_valid.size()||!s->damage_valid[n]))){error="Choose None or an existing resolved damage-table reference supported by this IMG's original layout.";return false;}}
    EmbeddedMetadataPlan p;p.source=s;p.values=v;memcpy(p.header.data(),s->blob.data()+s->offset,98);set_word(p.header.data()+16,v.flags);set_word(p.header.data()+84,v.start_x);set_word(p.header.data()+86,v.start_y);for(int i=0;i<6;++i)p.header[88+i]=(unsigned char)v.damage[i];for(int i=0;i<98;++i)p.changed_bytes+=p.header[i]!=s->blob[s->offset+i];
    if(s->entry>=0){memcpy(p.entry.data(),s->blob.data()+s->entry_offset,18);for(int i=0;i<3;++i)set_word(p.entry.data()+12+i*2,v.spare[i]);for(int i=0;i<18;++i)p.changed_bytes+=p.entry[i]!=s->blob[s->entry_offset+i];}out=std::move(p);return true;
}
bool Artwork::edit_embedded_metadata(const EmbeddedMetadataPlan &reviewed,std::string &error){
    error.clear();auto s=reviewed.source;if(pending||!s||s->owner!=this||s->generation!=generation()){error="The library changed or an edit is pending. Reopen animation metadata.";return false;}
    try{std::shared_ptr<const EmbeddedMetadataSource> fresh;if(!capture_embedded_metadata(*this,s->record,s->entry,fresh,error))return false;
        if(fresh->blob!=s->blob||fresh->damage!=s->damage||fresh->buffer_script!=s->buffer_script||fresh->sequences!=s->sequences||fresh->scripts!=s->scripts||fresh->damage_count!=s->damage_count||fresh->version!=s->version||fresh->disk_version!=s->disk_version||fresh->offset!=s->offset||fresh->entry_offset!=s->entry_offset){error="The stored animation or reference data changed. Reopen the review.";return false;}
        EmbeddedMetadataPlan plan;if(!plan_embedded_metadata(fresh,reviewed.values,plan,error))return false;if(plan.header!=reviewed.header||plan.entry!=reviewed.entry){error="The reviewed metadata does not match its edited fields.";return false;}if(!plan.changed_bytes)return true;
        State before=capture();before.label="Edit embedded animation metadata";past.reserve(past.size()+1);auto blob=(unsigned char *)data.scrseqmem_p;memcpy(blob+fresh->offset,plan.header.data(),98);if(fresh->entry>=0)memcpy(blob+fresh->entry_offset,plan.entry.data(),18);past.push_back(std::move(before));future.clear();revision=next_revision++;++serial;trim_history();return true;
    }catch(const std::bad_alloc &){error="Not enough memory to apply animation metadata. Artwork and history are unchanged.";return false;}
}
}
