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
    auto root=fs::temp_directory_path()/("studio-asm-copy-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto first=Artwork::demo(),second=Artwork::demo(),target=Artwork::demo();CHECK(second->rename_sprite(0,"FOREIGN1",error));second->color(0,1,0x03e0);second->commit();
    AsmSourceAnimation animation;animation.label="a_collected";AsmSourceFrame f;f.pieces={"SENTINEL1","FOREIGN1","SENTINEL2"};animation.frames={f};
    auto snapshot=AsmArtworkLibrary::capture(animation,{{1,first.get()},{2,second.get()}},1,{},false,error);CHECK(snapshot);if(!snapshot)return 1;auto sg=first->generation(),og=second->generation();
    CHECK(target->save((root/"target.IMG").u8string(),error));auto baseline=read(root/"target.IMG");auto count=target->count(),palettes=target->palette_count();auto original_id=target->sprite_id(0);auto pixels=target->copy(0,0,0,48,64).pixels;
    LibraryCopyOptions options;LibraryCopyPlan plan;CHECK(snapshot->plan_copy(*target,options,plan,error));auto generation=target->generation();CHECK(snapshot->copy_to(*target,plan,error)<0&&target->count()==count&&target->generation()==generation&&!target->dirty());
    options.prefix="CP_";CHECK(snapshot->plan_copy(*target,options,plan,error));CHECK(snapshot->copy_to(*target,plan,error)==3&&target->count()==count+3&&target->palette_count()==palettes+1&&target->dirty()&&target->can_undo());
    CHECK(target->sprite_id(0)==original_id&&target->copy(0,0,0,48,64).pixels==pixels&&std::string(target->sprite(count)->n_s)=="CP_SENTINEL1"&&first->generation()==sg&&second->generation()==og);
    CHECK(target->save((root/"copied.IMG").u8string(),error));auto reopened=Artwork::open((root/"copied.IMG").u8string(),error);CHECK(reopened&&reopened->count()==count+3&&reopened->disk_version()==0x634);
    target->undo();CHECK(target->count()==count&&target->palette_count()==palettes&&target->sprite_id(0)==original_id&&target->save((root/"undo.IMG").u8string(),error)&&read(root/"undo.IMG")==baseline);target->redo();CHECK(target->count()==count+3);
    target->undo();CHECK(snapshot->plan_copy(*target,options,plan,error));target->anchor(0,5,6);target->commit();CHECK(snapshot->copy_to(*target,plan,error)<0);target->undo();
    CHECK(snapshot->plan_copy(*target,options,plan,error));auto forged=plan;forged.rows[0].pixels[0]^=1;generation=target->generation();CHECK(snapshot->copy_to(*target,forged,error)<0&&target->generation()==generation&&target->count()==count);
    options.excluded.insert(snapshot->artwork().sprite_id(1));CHECK(snapshot->plan_copy(*target,options,plan,error)&&snapshot->copy_to(*target,plan,error)==2&&target->count()==count+2);target->undo();
    options.excluded.clear();CHECK(snapshot->plan_copy(*target,options,plan,error));target->begin("Pending");CHECK(snapshot->copy_to(*target,plan,error)<0);target->cancel();
    // Captured sources may close; one append still owns all selected data.
    first.reset();second.reset();CHECK(snapshot->plan_copy(*target,options,plan,error)&&snapshot->copy_to(*target,plan,error)==3);target->undo();
    int refused=0;for(int allocation=0;allocation<180;++allocation){auto dest=Artwork::demo();LibraryCopyPlan reviewed;CHECK(snapshot->plan_copy(*dest,options,reviewed,error));auto gen=dest->generation();auto selected=dest->selection_id();int n=dest->count(),p=dest->palette_count();bool history=dest->can_undo();
        fail_allocation=allocation;int copied=snapshot->copy_to(*dest,reviewed,error);fail_allocation=-1;
        if(copied<0){++refused;CHECK(dest->generation()==gen&&dest->count()==n&&dest->palette_count()==p&&dest->selection_id()==selected&&dest->can_undo()==history&&!dest->dirty());}
        else{CHECK(copied==3&&dest->count()==n+3);dest->undo();CHECK(dest->count()==n&&dest->palette_count()==p&&!dest->dirty());}
    }CHECK(refused>40);
    auto modern=Artwork::demo();CHECK(modern->save((root/"modern.IMG").u8string(),error));auto near=read(root/"modern.IMG");CHECK(project_img_layout(near,0x610,error));LIB_HDR h{};memcpy(&h,near.data(),sizeof h);unsigned short reserve=0x7351;memcpy(near.data()+h.oset+48,&reserve,2);write(root/"near.IMG",near);auto legacy=Artwork::open((root/"near.IMG").u8string(),error);
    auto previous=plan;CHECK(!snapshot->plan_copy(*legacy,options,plan,error)&&plan.target==previous.target&&snapshot->copy_to(*legacy,previous,error)<0&&legacy->disk_version()==0x610&&!legacy->dirty());
    animation.frames[0].pieces={"SENTINEL1"};auto old=AsmArtworkLibrary::capture(animation,{{1,legacy.get()}},1,{},false,error);CHECK(old);options.prefix="L_";CHECK(old->plan_copy(*legacy,options,plan,error));CHECK(old->copy_to(*legacy,plan,error)==1&&legacy->save((root/"legacy-copy.IMG").u8string(),error));
    auto bytes=read(root/"legacy-copy.IMG");memcpy(&h,bytes.data(),sizeof h);unsigned short copied_word=0;memcpy(&copied_word,bytes.data()+h.oset+(legacy->count()-1)*sizeof(IMAGE_disk)+48,2);CHECK(h.version==0x610&&copied_word==reserve);
    legacy->undo();CHECK(legacy->save((root/"legacy-undo.IMG").u8string(),error)&&read(root/"legacy-undo.IMG")==near);legacy->redo();CHECK(legacy->save((root/"legacy-redo.IMG").u8string(),error)&&read(root/"legacy-redo.IMG")==bytes);
    CHECK(g_doc==nullptr);fs::remove_all(root);std::cout<<"ASM artwork copy: "<<failures<<" failures\n";return failures?1:0;
}
