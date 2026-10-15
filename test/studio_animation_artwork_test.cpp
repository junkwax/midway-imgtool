#include "studio_animation_copy.h"
#include "studio_palette.h"
#include "studio_img_codec.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
#include <new>
#include <cstdlib>
// Fail each C++ allocation leading up to publication. The failure disarms itself
// so error reporting and fixture assertions can allocate normally.
static long fail_allocation=-1;
void *operator new(std::size_t n){if(fail_allocation==0){fail_allocation=-1;throw std::bad_alloc();}if(fail_allocation>0)--fail_allocation;if(void *p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void *p)noexcept{std::free(p);}
void operator delete(void *p,std::size_t)noexcept{std::free(p);}
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x " / "<<error<<"\n";++failures;}}while(0)
static std::vector<unsigned char> bytes(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static std::vector<unsigned char> blob(const studio::Artwork &a){auto p=(const unsigned char *)a.data.scrseqmem_p;return p?std::vector<unsigned char>(p,p+a.data.scrseqbytes):std::vector<unsigned char>{};}
int main(){using namespace studio;namespace fs=std::filesystem;std::string error;auto root=fs::temp_directory_path()/("studio-animation-artwork-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto source=std::make_unique<Artwork>(),target=Artwork::create();CHECK(source->add_palette("ART",{0,0x7c00,0x001f},error)==0&&source->add_palette("ALT",{0,0x03e0,0x7fff},error)==1);
    for(auto name:{"HERO1","HERO2","UNUSED"})CHECK(source->add_sprite(name,5,2,0,error)>=0);
    const unsigned char pixels[]={0,1,2,1,0,0xaa,0xbb,0xcc,2,0,1,0,2,0xdd,0xee,0xff};
    for(int i=0;i<source->count();++i){auto im=source->sprite(i);memcpy(im->data_p,pixels,16);im->baseline_p=malloc(16);memcpy(im->baseline_p,pixels,16);im->baseline_w=5;im->baseline_h=2;im->anix=7;im->aniy=(unsigned short)-3;im->anix2=9;im->file_frm=23;memcpy(im->file_name_raw,im->n_s,16);im->file_name_raw[13]=0x76;im->pttbl_p=calloc(1,40);((short *)im->pttbl_p)[0]=-3;}
    source->sprite(0)->opaltbl_p=malloc(16);memset(source->sprite(0)->opaltbl_p,0xff,16);auto alternate=(unsigned char *)source->sprite(0)->opaltbl_p;alternate[0]=4;alternate[1]=1;alternate[15]=0x65;
    CHECK(source->embedded_add_record(false,"WALK",error)==0&&source->embedded_append(0,1,error)&&source->embedded_append(0,0,error)&&source->embedded_append(0,1,error)&&source->embedded_add_record(true,"RUN",error)==1&&source->embedded_append(1,0,error));
    CHECK(target->embedded_add_record(false,"IDLE",error)==0&&target->embedded_append(0,0,error)&&target->embedded_add_record(true,"EXIST",error)==1&&target->embedded_append(1,0,error));
    auto source_path=root/"source.IMG",target_path=root/fs::u8path(u8"target \u65e5.IMG");CHECK(source->save(source_path.u8string(),error)&&target->save(target_path.u8string(),error));auto original=bytes(target_path),original_source=bytes(source_path),original_blob=blob(*target);auto source_generation=source->generation(),selection=target->selection_id();auto existing=target->copy(0,0,0,64,64).pixels;auto existing_pointer=target->sprite(0);auto existing_palette=target->palette(0);
    AnimationCopyOptions o;o.requested={1};AnimationCopyPlan p;
    CHECK(plan_animation_copy(*source,*target,o,p,error)&&p.artwork.rows.empty()&&!p.bindings[0].error.empty()&&target->apply_animation_copy(*source,p,error)==-1);
    o.copy_missing=true;CHECK(plan_animation_copy(*source,*target,o,p,error)&&p.added_sequences==1&&p.added_scripts==1&&p.artwork.rows.size()==2&&p.artwork.added_palettes==2&&p.bindings[0].source==0&&p.bindings[0].copied&&p.bindings[0].target==1&&p.bindings[1].target==2&&p.bindings[0].target_id==0&&p.bindings[0].error.empty());
    auto unchanged=[&](){return target->count()==1&&target->palette_count()==1&&blob(*target)==original_blob&&!target->dirty()&&target->selection_id()==selection;};auto generation=target->generation();
    auto bad=p;bad.artwork.rows[0].pixels[1]=0;CHECK(target->apply_animation_copy(*source,bad,error)==-1&&unchanged()&&target->generation()==generation);
    bad=p;bad.artwork.palettes[0].words[1]^=1;CHECK(target->apply_animation_copy(*source,bad,error)==-1&&unchanged());
    bad=p;bad.artwork.options.reuse=false;CHECK(target->apply_animation_copy(*source,bad,error)==-1&&unchanged());
    bad=p;bad.artwork.rows[0].mapped_alternate[0]=3;CHECK(target->apply_animation_copy(*source,bad,error)==-1&&unchanged());
    bad=p;bad.bindings[0].copied=false;CHECK(target->apply_animation_copy(*source,bad,error)==-1&&unchanged());
    bad=p;bad.artwork.scope.requested={source->sprite_id(2)};CHECK(target->apply_animation_copy(*source,bad,error)==-1&&unchanged());
    ((unsigned char *)source->sprite(0)->data_p)[1]^=1;CHECK(target->apply_animation_copy(*source,p,error)==-1&&unchanged());((unsigned char *)source->sprite(0)->data_p)[1]^=1;
    ((unsigned char *)source->palette(1)->data_p)[2]^=1;CHECK(target->apply_animation_copy(*source,p,error)==-1&&unchanged());((unsigned char *)source->palette(1)->data_p)[2]^=1;
    source->begin("Pending");CHECK(target->apply_animation_copy(*source,p,error)==-1&&unchanged());source->cancel();CHECK(plan_animation_copy(*source,*target,o,p,error));target->begin("Pending");CHECK(target->apply_animation_copy(*source,p,error)==-1&&unchanged());target->cancel();CHECK(plan_animation_copy(*source,*target,o,p,error));existing_pointer=target->sprite(0);existing_palette=target->palette(0);source_generation=source->generation();
    int failed_allocations=0;bool published=false;generation=target->generation();std::string previous_undo=target->undo_label();
    for(long i=0;i<1000;++i){fail_allocation=i;int result=target->apply_animation_copy(*source,p,error);fail_allocation=-1;
        if(result<0){++failed_allocations;CHECK(unchanged()&&target->generation()==generation&&std::string(target->undo_label())==previous_undo);}
        else{CHECK(result==2);published=true;break;}
    }
    CHECK(published&&failed_allocations>20);
    CHECK(target->count()==3&&target->palette_count()==3&&target->data.seqcnt==2&&target->data.scrcnt==2&&target->selection_id()==selection&&target->sprite(0)==existing_pointer&&target->palette(0)==existing_palette&&target->copy(0,0,0,64,64).pixels==existing&&std::string(target->undo_label())=="Copy animations and artwork"&&source->generation()==source_generation&&!source->dirty());
    std::vector<EmbeddedRecord> records;CHECK(read_embedded(*target,records,error)&&records[1].entries[0].values.index==2&&records[1].entries[1].values.index==1&&records[1].entries[2].values.index==2&&records[2].name=="EXIST"&&records[2].entries[0].values.index==0&&records[3].entries[0].values.index==1);
    auto added=target->sprite(1);auto new_id=target->sprite_id(1);CHECK(std::string(added->n_s)=="HERO1"&&added->palnum==1&&added->data_p!=source->sprite(0)->data_p&&memcmp(added->data_p,pixels,16)==0&&added->baseline_p!=source->sprite(0)->baseline_p&&memcmp(added->baseline_p,pixels,16)==0&&((short *)added->pttbl_p)[0]==-3&&((unsigned char *)added->opaltbl_p)[0]==5&&((unsigned char *)added->opaltbl_p)[1]==1&&((unsigned char *)added->opaltbl_p)[15]==0x65&&added->file_name_raw[13]==0x76&&added->anix2==9&&added->file_frm==23);
    auto result=root/"copied.IMG";CHECK(target->save(result.u8string(),error));auto loaded=Artwork::open(result.u8string(),error);CHECK(loaded&&loaded->disk_version()==0x634&&loaded->count()==3&&loaded->palette_count()==3&&read_embedded(*loaded,records,error)&&records[1].entries[1].values.index==1&&((unsigned char *)loaded->sprite(1)->opaltbl_p)[0]==5&&loaded->copy(1,0,0,5,2).pixels==source->copy(0,0,0,5,2).pixels&&bytes(source_path)==original_source&&bytes(target_path)==original);
    target->undo();CHECK(target->count()==1&&target->palette_count()==1&&blob(*target)==original_blob&&target->selection_id()==selection);target->redo();CHECK(target->count()==3&&target->sprite_id(1)==new_id&&target->data.scrcnt==2);target->undo();CHECK(target->save(target_path.u8string(),error)&&bytes(target_path)==original);
    // Existing matching sprites and explicit bindings are retained; copy only missing dependencies.
    CHECK(target->rename_sprite(0,"HERO1",error));CHECK(plan_animation_copy(*source,*target,o,p,error)&&p.artwork.rows.size()==1&&!p.bindings[0].copied&&p.bindings[0].target==0&&p.bindings[1].copied&&p.artwork.added_palettes==1&&target->apply_animation_copy(*source,p,error)==2&&target->palette_count()==2);target->undo();target->undo();
    o.sprite_bindings[source->sprite_id(0)]=target->sprite_id(0);CHECK(plan_animation_copy(*source,*target,o,p,error)&&p.artwork.rows.size()==1&&p.bindings[0].manual&&!p.bindings[0].copied&&target->apply_animation_copy(*source,p,error)==2);target->undo();o.sprite_bindings.clear();
    // Palette reuse compares all metadata/words; existing pixels/palettes are never replaced.
    CHECK(target->add_palette("ART",{0,0x7c00,0x001f},error)==1&&target->add_palette("ALT",{0,0x03e0,0x7fff},error)==2);CHECK(plan_animation_copy(*source,*target,o,p,error)&&p.artwork.added_palettes==0&&p.artwork.palettes[0].reused&&p.artwork.palettes[1].reused&&target->apply_animation_copy(*source,p,error)==2&&target->palette_count()==3);target->undo();target->undo();target->undo();
    o.sprite_prefix="C_";CHECK(plan_animation_copy(*source,*target,o,p,error)&&p.bindings[0].target_name=="C_HERO1"&&target->apply_animation_copy(*source,p,error)==2&&std::string(target->sprite(1)->n_s)=="C_HERO1");target->undo();o.sprite_prefix=std::string(15,'X');CHECK(plan_animation_copy(*source,*target,o,p,error)&&!p.bindings[0].error.empty()&&target->apply_animation_copy(*source,p,error)==-1);o.sprite_prefix.clear();
    o.requested.clear();CHECK(plan_animation_copy(*source,*target,o,p,error)&&p.artwork.rows.empty());generation=target->generation();CHECK(target->apply_animation_copy(*source,p,error)==0&&generation==target->generation());o.requested={1};
    // Ambiguity, unresolved animation references and record conflicts cannot add partial artwork.
    char name[16];memcpy(name,source->sprite(2)->n_s,16);memcpy(source->sprite(2)->n_s,source->sprite(0)->n_s,16);CHECK(plan_animation_copy(*source,*target,o,p,error)&&!p.bindings[0].error.empty()&&target->apply_animation_copy(*source,p,error)==-1&&target->count()==1);memcpy(source->sprite(2)->n_s,name,16);
    CHECK(target->embedded_add_record(false,"WALK",error)==1&&plan_animation_copy(*source,*target,o,p,error)&&target->apply_animation_copy(*source,p,error)==-1&&target->count()==1);target->undo();
    CHECK(plan_animation_copy(*source,*target,o,p,error));target->anchor(0,1,2);target->commit();CHECK(target->apply_animation_copy(*source,p,error)==-1&&target->count()==1);target->undo();CHECK(plan_animation_copy(*source,*target,o,p,error));source->anchor(2,1,2);source->commit();CHECK(target->apply_animation_copy(*source,p,error)==-1&&target->count()==1);source->undo();
    // A near-layout destination refuses alternate tables before publication, then
    // accepts compatible artwork while retaining its exact original IMG layout.
    auto near_bytes=original;CHECK(project_img_layout(near_bytes,0x610,error));auto near_path=root/"near.IMG";{std::ofstream f(near_path,std::ios::binary);f.write((const char *)near_bytes.data(),near_bytes.size());}auto near=Artwork::open(near_path.u8string(),error);CHECK(near);
    if(near){CHECK(plan_animation_copy(*source,*near,o,p,error)&&!p.bindings[0].error.empty()&&near->apply_animation_copy(*source,p,error)==-1&&near->count()==1&&near->palette_count()==1&&!near->dirty());CHECK(source->embedded_add_record(false,"SINGLE",error)==1&&source->embedded_append(1,1,error));o.requested={1};CHECK(plan_animation_copy(*source,*near,o,p,error)&&p.artwork.rows.size()==1&&near->apply_animation_copy(*source,p,error)==1&&near->save((root/"near-copy.IMG").u8string(),error));auto reopen=Artwork::open((root/"near-copy.IMG").u8string(),error);CHECK(reopen&&reopen->disk_version()==0x610&&reopen->count()==2&&read_embedded(*reopen,records,error)&&records[1].entries[0].values.index==1);near->undo();CHECK(near->save((root/"near-undo.IMG").u8string(),error)&&bytes(root/"near-undo.IMG")==near_bytes);source->undo();source->undo();}
    CHECK(source->save(source_path.u8string(),error)&&bytes(source_path)==original_source&&target->save(target_path.u8string(),error)&&bytes(target_path)==original&&g_doc==nullptr);
    fs::remove_all(root);std::cout<<"Animation artwork: "<<failures<<" failures\n";return failures?1:0;
}
