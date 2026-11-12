#include <algorithm>
#include "studio_animation_clipboard.h"
#include "studio_library_split_animations.h"
#include "studio_img_codec.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <new>
static long fail_after=-1;
void *operator new(std::size_t n){if(fail_after==0){fail_after=-1;throw std::bad_alloc();}if(fail_after>0)--fail_after;if(void *p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void *p)noexcept{std::free(p);}
void operator delete(void *p,std::size_t)noexcept{std::free(p);}
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x " / "<<error<<"\n";++failures;}}while(0)
using namespace studio;
static std::vector<unsigned char> bytes(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static std::vector<unsigned char> blob(const Artwork &a){auto p=(const unsigned char *)a.data.scrseqmem_p;return p?std::vector<unsigned char>(p,p+a.data.scrseqbytes):std::vector<unsigned char>{};}
static std::vector<unsigned char> damage(const Artwork &a){auto p=(const unsigned char *)a.data.damtbl_p;return p?std::vector<unsigned char>(p,p+a.data.damtblbytes):std::vector<unsigned char>{};}
static unsigned char *record(Artwork &a,int n){auto p=(unsigned char *)a.data.scrseqmem_p;while(n--)p+=98+18*(p[18]+256*p[19]);return p;}
static void table(Artwork &a,const std::vector<unsigned char> &b){free(a.data.damtbl_p);a.data.damtbl_p=b.empty()?nullptr:malloc(b.size());if(!b.empty())memcpy(a.data.damtbl_p,b.data(),b.size());a.data.damtblbytes=(unsigned)b.size();a.data.damcnt=(unsigned)b.size()/4;}
static std::unique_ptr<Artwork> source_fixture(std::string &error){auto a=Artwork::demo();
    if(a->embedded_add_record(false,"BASE",error)!=0||!a->embedded_append(0,0,error)||a->embedded_add_record(false,"BROKEN",error)!=1||!a->embedded_append(1,1,error)||a->embedded_add_record(false,"UNUSED",error)!=2||!a->embedded_append(2,7,error)||a->embedded_add_record(true,"REACT",error)!=3||!a->embedded_append(3,1,error)||a->embedded_add_record(true,"RUN",error)!=4||!a->embedded_append(4,0,error))return {};
    table(*a,{0x40,0,1,0, 0,0,0,0, 0x40,0,0,0, 0x40,0,1,0, 0x99,0,0,0});
    auto p=record(*a,0);p[88]=0;p[89]=1;p[90]=3;p[96]=0x7b;p[98+13]=0x6c;
    record(*a,1)[88]=2;record(*a,3)[88]=0x80; // Script damage bytes are opaque.
    return a;
}
static std::unique_ptr<Artwork> target_fixture(std::string &error){auto a=Artwork::demo();
    if(a->embedded_add_record(false,"IDLE",error)!=0||!a->embedded_append(0,2,error)||a->embedded_add_record(true,"EXIST",error)!=1||!a->embedded_append(1,0,error))return {};
    table(*a,{0x40,0,0,0});record(*a,0)[88]=0;return a;
}
int main(){namespace fs=std::filesystem;std::string error;auto root=fs::temp_directory_path()/("studio-animation-damage-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto source=source_fixture(error),target=target_fixture(error);CHECK(source&&target);if(!source||!target)return 1;
    auto sf=root/"source.IMG",tf=root/"target.IMG";CHECK(source->save(sf.u8string(),error)&&target->save(tf.u8string(),error));
    auto sb=blob(*source),sd=damage(*source),tb=blob(*target),td=damage(*target),sfbytes=bytes(sf),tfbytes=bytes(tf);auto sg=source->generation(),tg=target->generation(),selection=target->selection_id();
    AnimationCopyOptions options;options.requested={4};AnimationCopyPlan p;
    CHECK(plan_animation_copy(*source,*target,options,p,error)&&p.added_sequences==2&&p.added_scripts==2&&p.rows[0].dependency&&p.rows[1].dependency&&!p.rows[2].included&&p.rows[3].dependency&&p.rows[4].included&&p.bindings.size()==2&&p.damage_bindings.size()==4&&p.result_damage==std::vector<unsigned char>({0x40,0,0,0,0x40,0,2,0,0,0,1,0,0x40,0,1,0}));
    for(auto &r:p.rows)CHECK(r.error.empty());
    // Review fields and source/destination raw-table changes are checked again.
    auto bad=p;bad.result_damage[0]^=1;CHECK(target->apply_animation_copy(*source,bad,error)==-1);
    bad=p;bad.source_damage[0]^=1;CHECK(target->apply_animation_copy(*source,bad,error)==-1);
    bad=p;bad.damage_bindings[0].target++;CHECK(target->apply_animation_copy(*source,bad,error)==-1);
    ((unsigned char *)source->data.damtbl_p)[0]^=1;CHECK(target->apply_animation_copy(*source,p,error)==-1);((unsigned char *)source->data.damtbl_p)[0]^=1;
    ((unsigned char *)target->data.damtbl_p)[0]^=1;CHECK(target->apply_animation_copy(*source,p,error)==-1);((unsigned char *)target->data.damtbl_p)[0]^=1;
    CHECK(target->generation()==tg&&!target->dirty()&&blob(*target)==tb&&damage(*target)==td);
    CHECK(target->apply_animation_copy(*source,p,error)==4&&target->data.seqcnt==3&&target->data.scrcnt==3&&target->data.damcnt==4&&target->selection_id()==selection&&target->count()==8&&source->generation()==sg&&!source->dirty());
    std::vector<EmbeddedRecord> records;CHECK(read_embedded(*target,records,error)&&records.size()==6&&records[1].raw[88]==1&&records[1].raw[89]==2&&records[1].raw[90]==1&&records[2].raw[88]==3&&records[4].raw[88]==0x80&&records[4].entries[0].values.index==2&&records[5].entries[0].values.index==1&&records[1].raw[96]==0x7b&&records[1].entries[0].raw[13]==0x6c&&damage(*target)==p.result_damage);
    CHECK(std::equal(tb.begin(),tb.begin()+116,blob(*target).begin())&&std::equal(tb.begin()+116,tb.end(),blob(*target).begin()+348));
    auto output=root/"copied.IMG";CHECK(target->save(output.u8string(),error));auto reopened=Artwork::open(output.u8string(),error);CHECK(reopened&&reopened->disk_version()==0x634&&damage(*reopened)==p.result_damage&&blob(*reopened)==p.result_blob);
    target->undo();CHECK(blob(*target)==tb&&damage(*target)==td&&target->data.damcnt==1&&target->can_redo());target->redo();CHECK(damage(*target)==p.result_damage);target->undo();CHECK(target->save(tf.u8string(),error)&&bytes(tf)==tfbytes);
    // Prefixes apply to damage dependencies too. Negative script opcodes remain.
    options.prefix="CP_";CHECK(plan_animation_copy(*source,*target,options,p,error)&&p.damage_bindings[0].name=="CP_BROKEN"&&target->apply_animation_copy(*source,p,error)==4);target->undo();options.prefix.clear();
    for(int kind=0;kind<4;++kind){auto saved=sd;auto raw=(unsigned char *)source->data.damtbl_p;if(kind==0)raw[0]=0x99;if(kind==1)raw[2]=99;if(kind==2)raw[3]=255;if(kind==3)record(*source,0)[88]=128;
        CHECK(plan_animation_copy(*source,*target,options,p,error)&&!p.rows[0].error.empty()&&target->apply_animation_copy(*source,p,error)==-1&&blob(*target)==tb&&damage(*target)==td);memcpy(source->data.damtbl_p,sd.data(),sd.size());record(*source,0)[88]=0;
    }
    source->data.damtblbytes--;CHECK(!plan_animation_copy(*source,*target,options,p,error));source->data.damtblbytes++;
    // A full destination can reuse exact pairs, including slot 127; otherwise it
    // blocks without deleting/reordering any existing reference.
    std::vector<unsigned char> full(128*4,0xff);full[0]=0x40;full[1]=full[2]=full[3]=0;table(*target,full);CHECK(plan_animation_copy(*source,*target,options,p,error)&&!p.rows[0].error.empty()&&target->apply_animation_copy(*source,p,error)==-1);
    full[125*4]=0x40;full[125*4+1]=0;full[125*4+2]=2;full[125*4+3]=0;
    full[126*4]=0;full[126*4+1]=0;full[126*4+2]=1;full[126*4+3]=0;
    full[127*4]=0x40;full[127*4+1]=0;full[127*4+2]=1;full[127*4+3]=0;table(*target,full);
    CHECK(plan_animation_copy(*source,*target,options,p,error)&&p.result_damage==full&&p.damage_bindings.back().target==125&&target->apply_animation_copy(*source,p,error)==4&&record(*target,2)[88]==127);target->undo();CHECK(damage(*target)==full);table(*target,td);
    // High source indices are byte signed; #127 resolves, #128 never does.
    auto high=std::vector<unsigned char>(128*4,0xff);std::copy(sd.begin(),sd.end(),high.begin());std::copy(sd.begin(),sd.begin()+4,high.begin()+127*4);table(*source,high);record(*source,0)[88]=127;
    CHECK(plan_animation_copy(*source,*target,options,p,error)&&p.damage_bindings.back().source==127&&target->apply_animation_copy(*source,p,error)==4);target->undo();table(*source,sd);record(*source,0)[88]=0;
    // Atomic allocation failures preserve damage, records, artwork and Redo.
    target->color(0,1,0x1234);target->commit();target->undo();CHECK(plan_animation_copy(*source,*target,options,p,error));auto serial=target->generation();int rejected=0,succeeded=0;
    for(int i=0;i<600;++i){int count=-1;fail_after=i;try{count=target->apply_animation_copy(*source,p,error);}catch(const std::bad_alloc &){}fail_after=-1;
        if(count<0){++rejected;CHECK(target->generation()==serial&&target->can_redo()&&blob(*target)==tb&&damage(*target)==td&&target->count()==8);}
        else{++succeeded;CHECK(count==4&&damage(*target)==p.result_damage&&!target->can_redo());target->undo();break;}
    }CHECK(rejected>20&&succeeded==1);
    // Required new artwork and damage references publish in the same command.
    auto artwork_target=Artwork::create();AnimationCopyOptions with_artwork=options;with_artwork.copy_missing=true;
    CHECK(plan_animation_copy(*source,*artwork_target,with_artwork,p,error)&&p.artwork.rows.size()==2);serial=artwork_target->generation();rejected=0;succeeded=0;
    for(int i=0;i<1600;++i){int count=-1;fail_after=i;try{count=artwork_target->apply_animation_copy(*source,p,error);}catch(const std::bad_alloc &){}fail_after=-1;
        if(count<0){++rejected;CHECK(artwork_target->generation()==serial&&!artwork_target->data.damcnt&&!artwork_target->data.seqcnt&&artwork_target->count()==1&&!artwork_target->can_undo());}
        else{++succeeded;CHECK(count==4&&artwork_target->data.damcnt==3&&artwork_target->count()==3);artwork_target->undo();CHECK(artwork_target->count()==1&&!artwork_target->data.damtbl_p);artwork_target->redo();CHECK(artwork_target->count()==3&&artwork_target->data.damcnt==3);break;}
    }CHECK(rejected>40&&succeeded==1);
    // A reachable malformed destination binding is refused before new records
    // could accidentally turn its dangling index into a valid animation.
    record(*target,0)[88]=127;CHECK(!plan_animation_copy(*source,*target,options,p,error));record(*target,0)[88]=0;
    // Clipboard snapshots own only reached slots and survive closing the source.
    auto empty=source->split_target();AnimationCopyOptions extraction;extraction.requested={4};extraction.copy_missing=true;AnimationCopyPlan review;
    CHECK(plan_animation_copy(*source,*empty,extraction,review,error));auto clip=AnimationClipboard::capture(*source,review,"damage source",error);CHECK(clip&&clip->artwork().data.damcnt==3&&clip->artwork().data.seqcnt==2&&clip->artwork().data.scrcnt==2&&clip->artwork().count()==2&&!clip->artwork().can_undo());
    auto split=source->prepare_animation_split(review,error);CHECK(split&&split->data.damcnt==3&&split->dirty()&&!split->can_undo()&&split->save((root/"split.IMG").u8string(),error));
    // Automatic split excludes complete transitive chains if any required art
    // falls outside the selected sprites; it never expands the reviewed art set.
    auto split_target=source->split_target();LibraryCopyOptions co;LibraryCopyPlan cp;LibrarySplitAnimationPlan sp;
    CHECK(plan_library_copy(*source,*split_target,{source->sprite_id(0)},co,cp,error)&&plan_library_split_animations(*source,cp,sp,error)&&sp.sequences==0&&sp.scripts==0&&!sp.rows[0].included&&sp.rows[0].reason.find("outside")!=std::string::npos);
    CHECK(plan_library_copy(*source,*split_target,{source->sprite_id(0),source->sprite_id(1)},co,cp,error)&&plan_library_split_animations(*source,cp,sp,error)&&sp.sequences==2&&sp.scripts==2&&!sp.rows[2].included);
    auto sbad=sp;sbad.result_damage[0]^=1;CHECK(!source->prepare_library_split_animations(cp,sbad,error));auto automatic=source->prepare_library_split_animations(cp,sp,error);CHECK(automatic&&automatic->count()==2&&automatic->data.damcnt==3&&automatic->save((root/"auto.IMG").u8string(),error));
    // Later supported versions retain the exact header version and layout too.
    auto latebytes=tfbytes;LIB_HDR lateheader{};memcpy(&lateheader,latebytes.data(),sizeof lateheader);lateheader.version=0x654;memcpy(latebytes.data(),&lateheader,sizeof lateheader);
    auto latefile=root/"later.IMG";{std::ofstream f(latefile,std::ios::binary);f.write((const char *)latebytes.data(),latebytes.size());}auto late=Artwork::open(latefile.u8string(),error);
    CHECK(late&&plan_animation_copy(*source,*late,options,p,error)&&late->apply_animation_copy(*source,p,error)==4&&late->save((root/"later-copy.IMG").u8string(),error));auto latecopy=Artwork::open((root/"later-copy.IMG").u8string(),error);CHECK(latecopy&&latecopy->disk_version()==0x654&&latecopy->data.damcnt==4);late->undo();CHECK(late->save((root/"later-undo.IMG").u8string(),error)&&bytes(root/"later-undo.IMG")==latebytes);
    // Incompatible original layouts refuse transfer before mutation/conversion.
    auto nearbytes=tfbytes;table(*target,{});record(*target,0)[88]=255;CHECK(target->save((root/"plain.IMG").u8string(),error));nearbytes=bytes(root/"plain.IMG");CHECK(project_img_layout(nearbytes,0x610,error));auto nearfile=root/"near.IMG";{std::ofstream f(nearfile,std::ios::binary);f.write((const char *)nearbytes.data(),nearbytes.size());}auto near=Artwork::open(nearfile.u8string(),error);
    CHECK(near&&plan_animation_copy(*source,*near,options,p,error)&&!p.rows[0].error.empty()&&near->apply_animation_copy(*source,p,error)==-1&&!near->dirty()&&near->disk_version()==0x610&&near->save((root/"near-save.IMG").u8string(),error)&&bytes(root/"near-save.IMG")==nearbytes);
    CHECK(blob(*source)==sb&&damage(*source)==sd&&!source->dirty()&&source->generation()==sg&&bytes(sf)==sfbytes&&bytes(tf)==tfbytes);
    source.reset();empty.reset();split_target.reset();CHECK(clip&&clip->artwork().data.damcnt==3);auto destination=Artwork::create();AnimationCopyOptions paste;paste.requested={3};paste.copy_missing=true;
    CHECK(plan_animation_copy(clip->artwork(),*destination,paste,p,error)&&destination->apply_animation_copy(clip->artwork(),p,error)==4&&destination->data.damcnt==3&&destination->count()==3);destination->undo();CHECK(!destination->data.damcnt&&!destination->data.damtblbytes&&!destination->data.damtbl_p&&destination->count()==1);destination->redo();CHECK(destination->data.damcnt==3);
    CHECK(g_doc==nullptr);fs::remove_all(root);std::cout<<"Animation damage transfer: "<<failures<<" failures\n";return failures?1:0;
}
