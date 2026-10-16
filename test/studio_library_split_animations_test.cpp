#include "studio_library_split_animations.h"
#include "studio_sequences.h"
#include "studio_img_codec.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
#include <new>
#include <cstdlib>

static long fail_allocation=-1;
void *operator new(std::size_t n){
    if(fail_allocation==0){fail_allocation=-1;throw std::bad_alloc();}
    if(fail_allocation>0)--fail_allocation;
    if(void *p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
void operator delete(void *p)noexcept{std::free(p);}
void operator delete(void *p,std::size_t)noexcept{std::free(p);}
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x " / "<<error<<"\n";++failures;}}while(0)
static std::vector<unsigned char> bytes(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static std::vector<unsigned char> blob(const studio::Artwork &a){auto p=(const unsigned char *)a.data.scrseqmem_p;return p?std::vector<unsigned char>(p,p+a.data.scrseqbytes):std::vector<unsigned char>{};}

int main(){
    using namespace studio;namespace fs=std::filesystem;std::string error;
    auto root=fs::temp_directory_path()/("studio-library-split-animations-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto source=Artwork::demo();
    // A partial sequence, a complete sequence, an empty sequence, a damaged
    // sequence, and another complete sequence. Script eligibility follows calls.
    CHECK(source->embedded_add_record(false,"PARTIAL",error)==0&&source->embedded_append(0,1,error)&&source->embedded_append(0,4,error));
    CHECK(source->embedded_add_record(false,"FULL",error)==1&&source->embedded_append(1,5,error)&&source->embedded_append(1,1,error)&&source->embedded_append(1,5,error));
    CHECK(source->embedded_add_record(false,"EMPTY",error)==2);
    CHECK(source->embedded_add_record(false,"DAMAGE",error)==3&&source->embedded_append(3,1,error));
    CHECK(source->embedded_add_record(false,"OTHER",error)==4&&source->embedded_append(4,1,error));
    CHECK(source->embedded_add_record(true,"COMPLETE",error)==5&&source->embedded_append(5,4,error)&&source->embedded_append(5,1,error));
    CHECK(source->embedded_add_record(true,"MIXED",error)==6&&source->embedded_append(6,0,error)&&source->embedded_append(6,1,error));
    CHECK(source->embedded_add_record(true,"NOCALLS",error)==7);
    CHECK(source->embedded_add_record(true,"DAMCALL",error)==8&&source->embedded_append(8,3,error));
    auto raw=(unsigned char *)source->data.scrseqmem_p;
    const size_t full=98+2*18,damage=full+98+3*18+98;
    raw[full+96]=0x73;raw[full+98+13]=0x85;raw[full+98+6]=0;
    raw[damage+88]=0; // Unsupported binding must be reported, never copied.
    auto file=root/fs::u8path(u8"source \u65e5.IMG");CHECK(source->save(file.u8string(),error));
    auto original=bytes(file),original_blob=blob(*source);source->select_sprite(source->sprite_id(7));
    auto generation=source->generation(),selection=source->selection_id();std::string undo=source->undo_label();
    auto target=source->split_target();LibraryCopyPlan copy;LibrarySplitAnimationPlan plan;LibraryCopyOptions options;
    std::vector<uint64_t> ids={source->sprite_id(6),source->sprite_id(5),source->sprite_id(1)};
    auto review=[&](){return plan_library_copy(*source,*target,ids,options,copy,error)&&plan_library_split_animations(*source,copy,plan,error);};
    CHECK(review());
    CHECK(plan.rows.size()==9&&plan.sequences==2&&plan.scripts==1&&plan.sprites.size()==3);
    CHECK(!plan.rows[0].included&&plan.rows[0].reason.find("outside")!=std::string::npos&&plan.rows[1].included&&!plan.rows[2].included&&!plan.rows[3].included&&plan.rows[3].reason.find("Damage")!=std::string::npos&&plan.rows[4].included&&plan.rows[5].included&&!plan.rows[6].included&&!plan.rows[7].included&&!plan.rows[8].included);
    auto unchanged=[&](){return source->generation()==generation&&!source->dirty()&&source->count()==8&&source->selection_id()==selection&&std::string(source->undo_label())==undo&&blob(*source)==original_blob&&target->count()==0&&bytes(file)==original;};
    auto bad=plan;bad.rows[0].included=true;CHECK(!source->prepare_library_split_animations(copy,bad,error)&&unchanged());
    bad=plan;bad.result_blob[0]^=1;CHECK(!source->prepare_library_split_animations(copy,bad,error)&&unchanged());
    bad=plan;bad.source_blob[0]^=1;CHECK(!source->prepare_library_split_animations(copy,bad,error)&&unchanged());
    bad=plan;bad.sprites.pop_back();CHECK(!source->prepare_library_split_animations(copy,bad,error)&&unchanged());
    auto other=Artwork::demo();CHECK(!other->prepare_library_split_animations(copy,plan,error)&&unchanged());
    std::unique_ptr<Artwork> result;int injected=0;
    for(long i=0;i<3000;++i){fail_allocation=i;result=source->prepare_library_split_animations(copy,plan,error);fail_allocation=-1;if(result)break;++injected;CHECK(unchanged());}
    CHECK(result&&injected>20);if(!result)return 1;
    CHECK(unchanged()&&result->count()==3&&result->dirty()&&!result->can_undo()&&!result->can_redo()&&result->path().empty()&&result->disk_version()==source->disk_version());
    // Retain every reviewed sprite, including unreferenced sprite 6. Source
    // indices 1,5 become destination 0,1; source sequence indices 1,4 become 0,1.
    CHECK(result->sprite(2)->n_s==std::string(source->sprite(6)->n_s)&&result->sprite(1)->data_p!=source->sprite(5)->data_p&&result->copy(1,0,0,48,64).pixels==source->copy(5,0,0,48,64).pixels);
    std::vector<EmbeddedRecord> records;
    CHECK(read_embedded(*result,records,error)&&records.size()==3&&records[0].name=="FULL"&&records[1].name=="OTHER"&&records[2].name=="COMPLETE");
    CHECK(records[0].entries[0].values.index==1&&records[0].entries[1].values.index==0&&records[0].entries[2].values.index==1&&records[0].raw[96]==0x73&&records[0].entries[2].raw[13]==0x85&&records[0].entries[2].values.ticks==0&&records[2].entries[0].values.index==1&&records[2].entries[1].values.index==0);
    auto output=root/"result.IMG";CHECK(result->save(output.u8string(),error));auto saved=bytes(output);auto loaded=Artwork::open(output.u8string(),error);
    CHECK(loaded&&loaded->disk_version()==0x634&&read_embedded(*loaded,records,error)&&records[2].entries[0].values.index==1&&unchanged());
    result->paint(0,0,0,2);result->commit();CHECK(result->can_undo()&&unchanged());result->undo();CHECK(result->save(output.u8string(),error)&&bytes(output)==saved);
    // Changing exclusions invalidates an old animation review even if the
    // remaining records happen to have the same count. Empty subsets are valid.
    options.excluded.insert(source->sprite_id(5));LibraryCopyPlan reduced;
    CHECK(plan_library_copy(*source,*target,ids,options,reduced,error)&&!source->prepare_library_split_animations(reduced,plan,error));
    CHECK(plan_library_split_animations(*source,reduced,plan,error)&&plan.sequences==1&&plan.scripts==0&&!plan.rows[1].included);
    options.excluded.clear();ids={source->sprite_id(6)};CHECK(review()&&plan.sequences==0&&plan.scripts==0);
    auto no_records=source->prepare_library_split_animations(copy,plan,error);CHECK(no_records&&no_records->count()==1&&!no_records->data.seqcnt&&!no_records->data.scrcnt&&no_records->dirty()&&!no_records->can_undo());
    // Pending/stale or untracked embedded mutations refuse publication.
    ids={source->sprite_id(1),source->sprite_id(5)};CHECK(review());source->begin("Pending");CHECK(!source->prepare_library_split_animations(copy,plan,error));source->cancel();
    CHECK(review());target->begin("Pending");CHECK(!source->prepare_library_split_animations(copy,plan,error));target->cancel();
    CHECK(review());source->anchor(7,1,2);source->commit();CHECK(!source->prepare_library_split_animations(copy,plan,error));source->undo();
    CHECK(review());raw=(unsigned char *)source->data.scrseqmem_p;raw[full+96]^=1;CHECK(!source->prepare_library_split_animations(copy,plan,error));raw[full+96]^=1;
    CHECK(review());auto tampered=copy;tampered.rows[0].pixels[0]^=1;CHECK(!source->prepare_library_split_animations(tampered,plan,error));
    // Negative entry sentinels stay opaque and do not become missing art/calls.
    raw=(unsigned char *)source->data.scrseqmem_p;raw[full+98+4]=0xff;raw[full+98+5]=0xff;
    CHECK(review());auto negative=source->prepare_library_split_animations(copy,plan,error);
    CHECK(negative&&read_embedded(*negative,records,error)&&records[0].entries[2].values.index==-1);
    raw=(unsigned char *)source->data.scrseqmem_p;raw[full+98+4]=5;raw[full+98+5]=0;
    // Unresolved source indices/calls are skipped with explicit reasons.
    raw[98+4]=0xfe;raw[98+5]=0x7f;CHECK(review()&&plan.rows[0].reason=="Unresolved source sprite");raw[98+4]=4;raw[98+5]=0;
    const size_t complete_call=5*98+7*18+98; // first script follows five sequences
    raw=(unsigned char *)source->data.scrseqmem_p;
    auto call_low=raw[complete_call+4],call_high=raw[complete_call+5];
    raw[complete_call+4]=0xfe;raw[complete_call+5]=0x7f;
    CHECK(review()&&!plan.rows[5].included&&plan.rows[5].reason=="Unresolved sequence call");
    raw[complete_call+4]=0xff;raw[complete_call+5]=0xff;
    CHECK(review()&&plan.rows[5].included);auto negative_call=source->prepare_library_split_animations(copy,plan,error);
    CHECK(negative_call&&read_embedded(*negative_call,records,error)&&records[2].entries[0].values.index==-1&&records[2].entries[1].values.index==0);
    raw[complete_call+4]=call_low;raw[complete_call+5]=call_high;
    // Incompatible selected record names block the review, without silently
    // discarding fully contained records or changing the source or destination.
    CHECK(review());raw=(unsigned char *)source->data.scrseqmem_p;unsigned char first=raw[full];raw[full]=1;
    LibrarySplitAnimationPlan invalid;CHECK(!plan_library_split_animations(*source,copy,invalid,error)&&invalid.rows.empty()&&target->count()==0);raw[full]=first;
    // Empty source records and sources without embedded records remain usable.
    auto plain=Artwork::demo(),empty=plain->split_target();LibraryCopyPlan pc;LibrarySplitAnimationPlan pp;
    CHECK(plan_library_copy(*plain,*empty,{plain->sprite_id(0)}, {},pc,error)&&plan_library_split_animations(*plain,pc,pp,error)&&pp.rows.empty()&&plain->prepare_library_split_animations(pc,pp,error));
    // Near-layout source/header/reserve fields must survive the artwork-first path.
    auto near=Artwork::demo();CHECK(near->embedded_add_record(false,"NEAR",error)==0&&near->embedded_append(0,5,error)&&near->embedded_append(0,1,error));
    auto near_file=root/"near.IMG";CHECK(near->save(near_file.u8string(),error));auto nb=bytes(near_file);CHECK(project_img_layout(nb,0x610,error));LIB_HDR h{};memcpy(&h,nb.data(),sizeof h);h.damcnt=0x7123;memcpy(nb.data(),&h,sizeof h);unsigned short reserve=0x6571;memcpy(nb.data()+h.oset+5*sizeof(IMAGE_disk)+48,&reserve,2);{std::ofstream f(near_file,std::ios::binary);f.write((const char *)nb.data(),nb.size());}
    near=Artwork::open(near_file.u8string(),error);CHECK(near);if(near){auto nt=near->split_target();LibraryCopyPlan nc;LibrarySplitAnimationPlan np;
        CHECK(plan_library_copy(*near,*nt,{near->sprite_id(1),near->sprite_id(5)}, {},nc,error)&&plan_library_split_animations(*near,nc,np,error));auto nr=near->prepare_library_split_animations(nc,np,error);
        CHECK(nr&&nr->disk_version()==0x610&&nr->data.seqcnt==1&&!nr->can_undo());if(nr){auto nf=root/"near-split.IMG";CHECK(nr->save(nf.u8string(),error));auto b=bytes(nf);LIB_HDR nh{};memcpy(&nh,b.data(),sizeof nh);unsigned short word=0;memcpy(&word,b.data()+nh.oset+sizeof(IMAGE_disk)+48,2);CHECK(nh.version==0x610&&nh.damcnt==0x7123&&word==reserve);auto reopen=Artwork::open(nf.u8string(),error);CHECK(reopen&&read_embedded(*reopen,records,error)&&records[0].entries[0].values.index==1&&records[0].entries[1].values.index==0);}CHECK(bytes(near_file)==nb&&!near->dirty());
    }
    CHECK(bytes(file)==original&&g_doc==nullptr);fs::remove_all(root);std::cout<<"Library split animations: "<<failures<<" failures\n";return failures?1:0;
}
