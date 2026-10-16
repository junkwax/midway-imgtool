#include "studio_asm_artwork_library.h"
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
int main(int argc,char **argv){using namespace studio;namespace fs=std::filesystem;std::string error;
    auto root=fs::temp_directory_path()/("studio-asm-library-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto make=[&](const std::string &prefix,unsigned short alt){auto art=std::make_unique<Artwork>();CHECK(art->add_palette("PRIMARY",{0,0x7c00,0x001f},error)==0);CHECK(art->add_palette("ALT",{0,alt,0x03e0},error)==1);
        for(int i=1;i<=2;++i){CHECK(art->add_sprite(prefix+std::to_string(i),3,2,0,error)>=0);auto im=art->sprite(i-1);unsigned char pixels[]={0,1,2,0xab,2,1,0,0xcd};memcpy(im->data_p,pixels,8);im->baseline_w=3;im->baseline_h=2;im->baseline_p=malloc(8);memcpy(im->baseline_p,pixels,8);im->anix=7;im->aniy=9;im->aniz2=13;im->pttbl_p=calloc(1,40);((short *)im->pttbl_p)[0]=-17;im->opaltbl_p=malloc(16);memset(im->opaltbl_p,0xff,16);((unsigned char *)im->opaltbl_p)[0]=4;memcpy(im->file_name_raw,im->n_s,16);im->file_name_raw[14]=0x76;}
        return art;};
    auto a=make("A",0x011f),b=make("B",0x731f);CHECK(a->embedded_add_record(false,"NATIVE",error)==0&&a->embedded_append(0,0,error));
    auto ap=root/fs::u8path(u8"source \u65e5.IMG"),bp=root/"other.IMG";CHECK(a->save(ap.u8string(),error)&&b->save(bp.u8string(),error));auto ab=read(ap),bb=read(bp);auto ag=a->generation(),bg=b->generation();
    AsmSourceAnimation animation;animation.label="a_collected";AsmSourceFrame f;f.pieces={"B1","A2","B1","A1"};animation.frames={f};std::vector<AsmLibrary> libs={{17,a.get()},{29,b.get()}};AsmArtworkChoices choices;
    auto snapshot=AsmArtworkLibrary::capture(animation,libs,0,choices,false,error);CHECK(snapshot);if(!snapshot)return 1;
    CHECK(snapshot->artwork().count()==3&&snapshot->artwork().palette_count()==3&&snapshot->artwork().disk_version()==0x634&&snapshot->label()==animation.label&&!snapshot->partial());
    CHECK(snapshot->origins().size()==3&&snapshot->origins()[0].document==29&&snapshot->origins()[0].uses==2&&snapshot->origins()[1].target==2&&snapshot->origins()[1].name=="A2");
    auto result=snapshot->create_library(error);CHECK(result&&result->dirty()&&!result->can_undo()&&!result->can_redo()&&result->path().empty()&&result->data.seqcnt==0&&result->data.scrcnt==0);
    CHECK(result->sprite(0)->data_p!=b->sprite(0)->data_p&&memcmp(result->sprite(0)->data_p,b->sprite(0)->data_p,8)==0&&result->sprite(0)->aniz2==13&&result->sprite(0)->file_name_raw[14]==0x76&&((short *)result->sprite(0)->pttbl_p)[0]==-17);
    CHECK(((unsigned char *)result->sprite(0)->opaltbl_p)[0]==4&&((unsigned char *)result->sprite(1)->opaltbl_p)[0]==5&&memcmp(result->sprite(2)->baseline_p,a->sprite(1)->baseline_p,8)==0);
    auto output=root/"collected.IMG";CHECK(result->save(output.u8string(),error));auto exact=read(output);auto reopened=Artwork::open(output.u8string(),error);CHECK(reopened&&reopened->count()==3&&reopened->disk_version()==0x634&&reopened->sprite(2)->file_name_raw[14]==0x76);
    CHECK(a->generation()==ag&&b->generation()==bg&&!a->dirty()&&!b->dirty()&&read(ap)==ab&&read(bp)==bb);
    int refused=0;for(int allocation=0;allocation<180;++allocation){fail_allocation=allocation;auto failed=AsmArtworkLibrary::capture(animation,libs,0,choices,false,error);fail_allocation=-1;if(!failed)++refused;CHECK(a->generation()==ag&&b->generation()==bg&&!a->dirty()&&!b->dirty());}CHECK(refused>50);
    for(int allocation=0;allocation<60;++allocation){fail_allocation=allocation;auto clone=snapshot->create_library(error);fail_allocation=-1;CHECK(snapshot->artwork().count()==3&&!snapshot->artwork().can_undo());}
    a->anchor(0,-4,-5);a->commit();b.reset();libs.clear();auto independent=snapshot->create_library(error);CHECK(independent&&independent->sprite(0)->anix==7&&independent->save((root/"independent.IMG").u8string(),error)&&read(root/"independent.IMG")==exact);a->undo();
    auto duplicate=make("A",0x1111);libs={{17,a.get()},{29,duplicate.get()}};f.pieces={"A1"};animation.frames={f};CHECK(!AsmArtworkLibrary::capture(animation,libs,0,{},false,error));
    choices["A1"]={17,a->sprite_id(0)};CHECK(AsmArtworkLibrary::capture(animation,libs,0,choices,false,error));choices["A1"]={99,1};CHECK(!AsmArtworkLibrary::capture(animation,libs,0,choices,false,error));
    f.pieces={"MISSING"};animation.frames={f};CHECK(!AsmArtworkLibrary::capture(animation,libs,17,{},false,error));
    animation.frames.clear();CHECK(!AsmArtworkLibrary::capture(animation,libs,17,{},false,error));f.pieces={"A1"};animation.frames={f};animation.partial=true;
    CHECK(!AsmArtworkLibrary::capture(animation,libs,17,{},false,error));auto partial=AsmArtworkLibrary::capture(animation,libs,17,{},true,error);CHECK(partial&&partial->partial());animation.partial=false;
    a->begin("Pending");CHECK(!AsmArtworkLibrary::capture(animation,libs,17,{},false,error));a->cancel();
    // Original near layouts and non-point-table reserve words survive aggregation.
    auto modern=Artwork::demo();CHECK(modern->save((root/"modern.IMG").u8string(),error));auto near=read(root/"modern.IMG");CHECK(project_img_layout(near,0x610,error));LIB_HDR header{};memcpy(&header,near.data(),sizeof header);unsigned short reserve=0x7123;memcpy(near.data()+header.oset+48,&reserve,2);write(root/"near1.IMG",near);auto na=Artwork::open((root/"near1.IMG").u8string(),error);
    reserve=0x7351;memcpy(near.data()+header.oset+sizeof(IMAGE_disk)+48,&reserve,2);write(root/"near2.IMG",near);auto nb=Artwork::open((root/"near2.IMG").u8string(),error);
    f.pieces={"SENTINEL1","SENTINEL2"};animation.frames={f};choices={{"SENTINEL1",{1,na->sprite_id(0)}},{"SENTINEL2",{2,nb->sprite_id(1)}}};
    auto legacy=AsmArtworkLibrary::capture(animation,{{1,na.get()},{2,nb.get()}},0,choices,false,error);CHECK(legacy&&legacy->artwork().disk_version()==0x610);if(legacy){auto library=legacy->create_library(error);CHECK(library&&library->save((root/"legacy.IMG").u8string(),error));auto saved=read(root/"legacy.IMG");memcpy(&header,saved.data(),sizeof header);unsigned short r1=0,r2=0;memcpy(&r1,saved.data()+header.oset+48,2);memcpy(&r2,saved.data()+header.oset+sizeof(IMAGE_disk)+48,2);CHECK(header.version==0x610&&r1==0x7123&&r2==0x7351);}
    choices["SENTINEL2"]={2,modern->sprite_id(1)};CHECK(!AsmArtworkLibrary::capture(animation,{{1,na.get()},{2,modern.get()}},0,choices,false,error));
    if(argc>2&&fs::exists(fs::u8path(argv[1]))&&fs::exists(fs::u8path(argv[2]))){auto original=read(fs::u8path(argv[2]));auto game=Artwork::open(argv[2],error);AsmSourceAnimation picked;picked.label="real selected artwork";AsmSourceFrame frame;for(int i=0;i<std::min(4,game->count());++i)frame.pieces.push_back(game->sprite(i)->n_s);picked.frames={frame};auto real=AsmArtworkLibrary::capture(picked,{{1,game.get()}},1,{},false,error);CHECK(real);if(real){auto library=real->create_library(error);CHECK(library&&library->disk_version()==game->disk_version()&&library->save((root/"real-artwork.IMG").u8string(),error));}CHECK(read(fs::u8path(argv[2]))==original&&!game->dirty());}
    CHECK(read(ap)==ab&&read(bp)==bb&&g_doc==nullptr);fs::remove_all(root);std::cout<<"ASM artwork library: "<<failures<<" failures\n";return failures?1:0;
}
