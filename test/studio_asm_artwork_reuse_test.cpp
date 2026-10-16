#include "studio_asm_artwork_library.h"
#include "studio_library_copy.h"
#include "studio_img_codec.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
#include <cstdlib>
#include <new>
static long fail_allocation=-1;
void *operator new(std::size_t n){if(fail_allocation==0){fail_allocation=-1;throw std::bad_alloc();}if(fail_allocation>0)--fail_allocation;if(void *p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void *p)noexcept{std::free(p);}void operator delete(void *p,std::size_t)noexcept{std::free(p);}
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x " / "<<error<<"\n";++failures;}}while(0)
static std::vector<unsigned char> read(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void write(const std::filesystem::path &p,const std::vector<unsigned char> &b){std::ofstream f(p,std::ios::binary);f.write((const char *)b.data(),b.size());}
int main(){using namespace studio;namespace fs=std::filesystem;std::string error;
    auto root=fs::temp_directory_path()/("studio-asm-reuse-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto source=Artwork::demo(),other=Artwork::demo();CHECK(other->rename_sprite(0,"FOREIGN1",error));other->color(0,1,0x03e0);other->commit();
    CHECK(source->save((root/"source.IMG").u8string(),error));auto original=read(root/"source.IMG");auto target=Artwork::open((root/"source.IMG").u8string(),error);
    AsmSourceAnimation animation;animation.label="a_reuse";AsmSourceFrame frame;frame.pieces={"SENTINEL1","FOREIGN1","SENTINEL2"};animation.frames={frame};
    auto held=AsmArtworkLibrary::capture(animation,{{1,source.get()},{2,other.get()}},1,{},false,error);CHECK(held);if(!held)return 1;
    LibraryCopyOptions options;LibraryCopyPlan p;CHECK(held->plan_copy(*target,options,p,error));CHECK(p.reused.empty()&&!p.rows[0].error.empty());
    CHECK(held->plan_copy(*target,options,p,error,true));CHECK(p.reuse_named&&p.reused.size()==2&&!p.rows[0].enabled&&!p.rows[1].enabled&&p.rows[2].enabled&&p.added_palettes==1);
    int count=target->count(),palettes=target->palette_count();auto id=target->sprite_id(0),generation=target->generation();auto baseline=target->sprite(0)->baseline_p;CHECK(baseline&&baseline!=held->artwork().sprite(0)->baseline_p);
    CHECK(held->copy_to(*target,p,error)==1&&target->count()==count+1&&target->palette_count()==palettes+1&&target->sprite_id(0)==id&&target->sprite(0)->baseline_p==baseline&&target->can_undo());
    target->undo();CHECK(target->count()==count&&target->palette_count()==palettes&&!target->dirty());target->redo();CHECK(target->count()==count+1);target->undo();
    // All reused means no copy, history, generation, selection or file changes.
    auto complete=held->create_library(error);CHECK(complete&&complete->save((root/"complete.IMG").u8string(),error));auto complete_bytes=read(root/"complete.IMG");complete=Artwork::open((root/"complete.IMG").u8string(),error);
    CHECK(held->plan_copy(*complete,options,p,error,true)&&p.reused.size()==3&&p.added_palettes==0&&p.added_pixels==0);generation=complete->generation();auto selection=complete->selection_id();
    CHECK(held->copy_to(*complete,p,error)==0&&complete->generation()==generation&&complete->selection_id()==selection&&!complete->dirty()&&!complete->can_undo()&&read(root/"complete.IMG")==complete_bytes);
    auto forged=p;forged.reused[0].target=9999;CHECK(held->copy_to(*complete,forged,error)<0&&!complete->dirty());forged=p;forged.reused.erase(forged.reused.begin());CHECK(held->copy_to(*complete,forged,error)<0);
    complete->anchor(0,2,3);complete->commit();CHECK(held->copy_to(*complete,p,error)<0);complete->undo();
    auto matches=[&](){LibraryCopyPlan plan;CHECK(held->plan_copy(*target,{},plan,error,true));for(auto &r:plan.reused)if(r.target==id)return true;return false;};
    CHECK(matches());auto im=target->sprite(0);((unsigned char *)im->baseline_p)[0]^=1;CHECK(matches());((unsigned char *)im->baseline_p)[0]^=1;IMG saved=*im;
    for(int field=0;field<10;++field){*im=saved;switch(field){case 0:im->flags^=1;break;case 1:++im->anix;break;case 2:++im->aniy;break;case 3:++im->anix2;break;case 4:++im->aniy2;break;case 5:++im->aniz2;break;case 6:++im->file_data;break;case 7:++im->file_lib;break;case 8:++im->file_frm;break;case 9:im->file_name_raw[14]^=1;break;}CHECK(!matches());}*im=saved;
    auto pixel=(unsigned char *)im->data_p;pixel[0]^=1;CHECK(!matches());pixel[0]^=1;
    auto pal=target->palette(im->palnum);auto words=(unsigned short *)pal->data_p;words[1]^=1;CHECK(!matches());words[1]^=1;pal->file_spare^=1;CHECK(!matches());pal->file_spare^=1;pal->n_s[0]^=1;CHECK(!matches());pal->n_s[0]^=1;
    im->pttbl_p=calloc(1,40);CHECK(!matches());free(im->pttbl_p);im->pttbl_p=nullptr;
    im->file_oset+=999;memset(im->src_filename,0x41,sizeof im->src_filename);CHECK(matches());*im=saved;
    // Raw writes without a generation update still invalidate reuse at Apply.
    CHECK(held->plan_copy(*target,{},p,error,true));pixel[0]^=1;generation=target->generation();CHECK(held->copy_to(*target,p,error)<0&&target->generation()==generation&&target->count()==count);pixel[0]^=1;
    char old_name[16];memcpy(old_name,target->sprite(1)->n_s,16);snprintf(target->sprite(1)->n_s,16,"SENTINEL1");CHECK(!matches());memcpy(target->sprite(1)->n_s,old_name,16);
    options.excluded.insert(held->artwork().sprite_id(0));CHECK(held->plan_copy(*target,options,p,error,true)&&p.reused.size()==1);options.excluded.clear();
    options.prefix="CP_";CHECK(held->plan_copy(*target,options,p,error,true)&&p.reused.empty()&&held->copy_to(*target,p,error)==3);CHECK(held->plan_copy(*target,options,p,error,true)&&p.reused.size()==3&&held->copy_to(*target,p,error)==0);target->undo();options.prefix.clear();
    // Alternate palette references may use different numeric slots when their stored palettes match.
    auto alt_source=Artwork::demo();CHECK(alt_source->add_palette("ALT",{0,0x1234,0x731f},error)==1);auto ai=alt_source->sprite(0);ai->opaltbl_p=malloc(16);memset(ai->opaltbl_p,0xff,16);((unsigned char *)ai->opaltbl_p)[0]=4;ai->pttbl_p=calloc(1,40);((short *)ai->pttbl_p)[0]=-7;
    CHECK(alt_source->save((root/"alt.IMG").u8string(),error));auto alt_target=Artwork::open((root/"alt.IMG").u8string(),error);frame.pieces={"SENTINEL1"};animation.frames={frame};auto alt=AsmArtworkLibrary::capture(animation,{{1,alt_source.get()}},1,{},false,error);
    CHECK(alt&&alt->plan_copy(*alt_target,{},p,error,true)&&p.reused.size()==1);
    auto primary=alt_target->palette(0),alternate=alt_target->palette(1);alt_target->data.pal_p=alternate;alternate->nxt_p=primary;primary->nxt_p=nullptr;auto ti=alt_target->sprite(0);ti->palnum=1;((unsigned char *)ti->opaltbl_p)[0]=3;
    CHECK(alt->plan_copy(*alt_target,{},p,error,true)&&p.reused.size()==1);((short *)ti->pttbl_p)[0]++;CHECK(alt->plan_copy(*alt_target,{},p,error,true)&&p.reused.empty());((short *)ti->pttbl_p)[0]--;
    ((unsigned char *)ti->opaltbl_p)[15]=0x80;CHECK(alt->plan_copy(*alt_target,{},p,error,true)&&p.reused.empty());
    int refused=0;for(int n=0;n<150;++n){auto dest=Artwork::open((root/"source.IMG").u8string(),error);CHECK(held->plan_copy(*dest,{},p,error,true));auto gen=dest->generation();auto sel=dest->selection_id();fail_allocation=n;int added=held->copy_to(*dest,p,error);fail_allocation=-1;if(added<0){++refused;CHECK(dest->count()==count&&dest->generation()==gen&&dest->selection_id()==sel&&!dest->dirty()&&!dest->can_undo());}else{CHECK(added==1);dest->undo();CHECK(dest->count()==count&&!dest->dirty());}}CHECK(refused>40);
    auto near=original;CHECK(project_img_layout(near,0x610,error));LIB_HDR h{};memcpy(&h,near.data(),sizeof h);unsigned short reserve=0x7123;memcpy(near.data()+h.oset+48,&reserve,2);write(root/"near.IMG",near);auto ns=Artwork::open((root/"near.IMG").u8string(),error),nt=Artwork::open((root/"near.IMG").u8string(),error);auto legacy=AsmArtworkLibrary::capture(animation,{{1,ns.get()}},1,{},false,error);
    CHECK(legacy&&legacy->plan_copy(*nt,{},p,error,true)&&p.reused.size()==1&&legacy->copy_to(*nt,p,error)==0&&!nt->dirty());
    reserve=0x7351;memcpy(near.data()+h.oset+48,&reserve,2);write(root/"near-other.IMG",near);auto different=Artwork::open((root/"near-other.IMG").u8string(),error);CHECK(legacy->plan_copy(*different,{},p,error,true)&&p.reused.empty()&&legacy->copy_to(*different,p,error)<0&&different->disk_version()==0x610);
    CHECK(read(root/"source.IMG")==original&&g_doc==nullptr);fs::remove_all(root);std::cout<<"ASM artwork reuse: "<<failures<<" failures\n";return failures?1:0;
}
