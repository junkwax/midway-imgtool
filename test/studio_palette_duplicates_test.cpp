#include "studio_palette_cleanup.h"
#include "studio_palette.h"
#include "studio_layers.h"
#include "studio_img_codec.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
#include <new>
static int failures=0;static long fail_after=-1;
void *operator new(size_t n){if(fail_after==0)throw std::bad_alloc();if(fail_after>0)--fail_after;if(auto p=malloc(n?n:1))return p;throw std::bad_alloc();}
void *operator new[](size_t n){return ::operator new(n);}
void operator delete(void *p) noexcept{free(p);}void operator delete[](void *p) noexcept{free(p);}
void operator delete(void *p,size_t) noexcept{free(p);}void operator delete[](void *p,size_t) noexcept{free(p);}
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x " / "<<error<<"\n";++failures;}}while(0)
static std::vector<unsigned char> bytes(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static std::unique_ptr<studio::Artwork> fixture(bool alternate,bool layer,std::string &error){
    using namespace studio;auto a=std::make_unique<Artwork>();
    std::vector<unsigned short> first={0x8123,0xffff,0x7c00,0x801f},second={0,0x1234,0x2345},high=first,longer=first;high[1]^=0x8000;longer.push_back(0);
    for(auto words:{first,second,first,first,second,high,first,std::vector<unsigned short>{0,0x4321,0x22},longer}){int i=a->palette_count();if(a->add_palette("PAL"+std::to_string(i),words,error)!=i)return {};}
    a->palette(3)->bitspix=4;a->palette(8)->bitspix=2;
    a->palette(0)->file_spare=0x7331;a->palette(0)->flags=0x80;memcpy(a->palette(0)->file_name_raw,"PAL0\0tail",9);
    a->palette(2)->flags=0x40;a->palette(2)->file_spare=0x5555;
    unsigned char pixels[]={0,1,2,3,0,0xab,0xcd,0xef,3,2,0,1,2,0x83,0x94,0xa5};
    for(int p:{2,4,7}){int i=a->count();if(a->add_sprite("SPR"+std::to_string(i),5,2,p,error)!=i)return {};auto im=a->sprite(i);memcpy(im->data_p,pixels,16);if(p!=2)for(int y=0;y<2;++y)for(int x=0;x<5;++x)if(((unsigned char *)im->data_p)[y*8+x]==3)((unsigned char *)im->data_p)[y*8+x]=2;im->baseline_p=malloc(16);memcpy(im->baseline_p,im->data_p,16);im->baseline_w=5;im->baseline_h=2;im->anix=7;im->pttbl_p=calloc(1,40);((short *)im->pttbl_p)[0]=-5;}
    if(alternate){auto im=a->sprite(0);im->opaltbl_p=malloc(16);memset(im->opaltbl_p,0xff,16);auto alt=(unsigned char *)im->opaltbl_p;alt[0]=9;alt[1]=2;alt[2]=7;alt[15]=9;im=a->sprite(2);im->opaltbl_p=malloc(16);memset(im->opaltbl_p,9,16);}
    if(layer){auto clip=a->copy(0,0,0,2,2);clip.pixels.assign(4,3);if(!a->paste_layer(0,clip,2,0,error))return {};}
    a->select_sprite(a->sprite_id(1));return a;
}
int main(){using namespace studio;namespace fs=std::filesystem;std::string error;const auto mode=PaletteCleanupMode::Duplicates;
    auto root=fs::temp_directory_path()/("studio-palette-duplicates-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto a=fixture(true,true,error);CHECK(a);if(!a)return 1;auto path=root/"original.IMG";CHECK(a->save(path.u8string(),error));auto exact=bytes(path);
    PaletteCleanupPlan p;auto gen=a->generation(),structure=a->structure_generation();auto selected=a->selection_id();auto image=a->sprite(0);auto data=image->data_p,baseline=image->baseline_p,layer=image->layer_p;auto pal=a->palette(0);auto pal_words=copy_palette(*a,0).words;auto pal_fields=*pal;auto identity=a->sprite_id(0);
    CHECK(plan_palette_cleanup(*a,{},p,error,mode)&&p.mode==mode&&p.removed==3&&p.rows[2].match==0&&p.rows[4].match==1&&p.rows[6].match==0&&p.rows[3].match==3&&p.rows[5].match==5&&p.rows[8].match==8&&p.rows[7].target==4&&p.primary_remaps==3&&p.alternate_remaps==18&&p.color_bytes==22&&!a->dirty()&&a->generation()==gen);
    auto bad=p;bad.rows[2].target=2;CHECK(a->apply_palette_cleanup(bad,error)<0);bad=p;bad.rows[2].match=1;CHECK(a->apply_palette_cleanup(bad,error)<0);bad=p;++bad.alternate_remaps;CHECK(a->apply_palette_cleanup(bad,error)<0);bad=p;bad.mode=PaletteCleanupMode::Unused;CHECK(a->apply_palette_cleanup(bad,error)<0);auto foreign=Artwork::demo();CHECK(foreign->apply_palette_cleanup(p,error)<0);
    CHECK(a->apply_palette_cleanup(p,error)==3&&a->palette_count()==6&&a->sprite(0)->palnum==0&&a->sprite(1)->palnum==1&&a->sprite(2)->palnum==4&&a->generation()>gen&&a->structure_generation()>structure&&a->selection_id()==selected&&a->sprite_id(0)==identity&&std::string(a->undo_label())=="Merge duplicate palettes");
    CHECK(image->data_p==data&&image->baseline_p==baseline&&image->layer_p==layer&&image->anix==7&&((short *)image->pttbl_p)[0]==-5&&a->palette(0)==pal&&pal->data_p==pal_fields.data_p&&pal->flags==pal_fields.flags&&pal->file_spare==0x7331&&memcmp(pal->file_name_raw,pal_fields.file_name_raw,16)==0&&copy_palette(*a,0).words==pal_words);
    auto alt=(unsigned char *)image->opaltbl_p;CHECK(alt[0]==3&&alt[1]==2&&alt[2]==4&&alt[3]==255&&alt[15]==9);for(int i=0;i<16;++i)CHECK(((unsigned char *)a->sprite(2)->opaltbl_p)[i]==3);
    CHECK(a->save((root/"merged.IMG").u8string(),error));auto disk=Artwork::open((root/"merged.IMG").u8string(),error);CHECK(disk&&disk->disk_version()==0x634&&disk->palette_count()==6&&disk->sprite(1)->palnum==1&&copy_palette(*disk,0).words==pal_words);
    a->undo();CHECK(a->palette_count()==9&&a->sprite(0)->palnum==2&&a->sprite(0)->layer_p&&a->dirty());a->redo();CHECK(a->palette_count()==6);a->undo();CHECK(a->save(path.u8string(),error)&&bytes(path)==exact);
    CHECK(plan_palette_cleanup(*a,{2},p,error,mode)&&p.removed==2&&p.rows[2].target==2&&!p.rows[2].remove&&p.rows[6].target==0&&a->apply_palette_cleanup(p,error)==2&&a->sprite(0)->palnum==2);a->undo();
    CHECK(plan_palette_cleanup(*a,{0,1,2,3,4,5,6,7,8},p,error,mode)&&!p.removed);gen=a->generation();auto label=std::string(a->undo_label());CHECK(a->apply_palette_cleanup(p,error)==0&&a->generation()==gen&&std::string(a->undo_label())==label);
    CHECK(plan_palette_cleanup(*a,{},p,error,mode));a->begin("Pending");CHECK(a->apply_palette_cleanup(p,error)<0);a->cancel();CHECK(a->apply_palette_cleanup(p,error)<0);CHECK(plan_palette_cleanup(*a,{},p,error,mode));a->palette(2)->flags^=1;CHECK(a->apply_palette_cleanup(p,error)<0);a->palette(2)->flags^=1;
    CHECK(plan_palette_cleanup(*a,{},p,error,mode));((unsigned char *)a->sprite(0)->opaltbl_p)[15]^=1;CHECK(a->apply_palette_cleanup(p,error)<0);((unsigned char *)a->sprite(0)->opaltbl_p)[15]^=1;
    auto ref=a->sprite(0)->palnum;a->sprite(0)->palnum=99;CHECK(!plan_palette_cleanup(*a,{},p,error,mode));a->sprite(0)->palnum=ref;auto depth=a->palette(0)->bitspix;a->palette(0)->bitspix=9;CHECK(!plan_palette_cleanup(*a,{},p,error,mode));a->palette(0)->bitspix=depth;
    CHECK(!plan_palette_cleanup(*a,{-1},p,error,mode)&&!plan_palette_cleanup(*a,{},p,error,(PaletteCleanupMode)99));
    auto wide=Artwork::create();auto wide_colors=copy_palette(*wide,0).words;for(int i=1;i<125;++i)CHECK(wide->add_palette("P"+std::to_string(i),wide_colors,error)==i);wide->sprite(0)->palnum=124;wide->sprite(0)->opaltbl_p=malloc(16);memset(wide->sprite(0)->opaltbl_p,127,16);CHECK(plan_palette_cleanup(*wide,{},p,error,mode)&&p.removed==124&&p.alternate_remaps==16&&wide->apply_palette_cleanup(p,error)==124&&wide->palette_count()==1&&wide->sprite(0)->palnum==0);for(int i=0;i<16;++i)CHECK(((unsigned char *)wide->sprite(0)->opaltbl_p)[i]==3);
    int rejected=0,succeeded=0;
    for(auto scenario:{mode,PaletteCleanupMode::Unused})for(int fail=0;fail<180;++fail){auto doc=fixture(true,true,error);doc->color(0,1,0x1234);doc->commit();doc->undo();CHECK(plan_palette_cleanup(*doc,{},p,error,scenario));int expected=p.removed;auto g=doc->generation();auto dirty=doc->dirty();auto undo=std::string(doc->undo_label());auto ptr=doc->palette(0)->data_p;int n=-1;fail_after=fail;try{n=doc->apply_palette_cleanup(p,error);}catch(const std::bad_alloc &){}fail_after=-1;if(n<0){++rejected;CHECK(doc->generation()==g&&doc->dirty()==dirty&&doc->palette_count()==9&&doc->can_redo()&&std::string(doc->undo_label())==undo&&doc->palette(0)->data_p==ptr&&doc->sprite(0)->palnum==2&&((unsigned char *)doc->sprite(0)->opaltbl_p)[0]==9&&copy_palette(*doc,0).words==pal_words);}else{++succeeded;CHECK(n==expected&&!doc->can_redo());doc->undo();CHECK(doc->palette_count()==9&&copy_palette(*doc,0).words==pal_words);}}
    CHECK(rejected>20&&succeeded>0);
    auto seed=fixture(false,false,error);CHECK(seed->save((root/"seed.IMG").u8string(),error));auto seed_bytes=bytes(root/"seed.IMG");
    for(unsigned version:{0x60au,0x61cu,0x61du,0x631u,0x634u}){auto b=seed_bytes;if(version<0x634)CHECK(project_img_layout(b,version,error));auto file=root/(std::to_string(version)+".IMG");{std::ofstream f(file,std::ios::binary);f.write((char *)b.data(),b.size());}auto doc=Artwork::open(file.u8string(),error);CHECK(doc);if(!doc)continue;CHECK(plan_palette_cleanup(*doc,{},p,error,mode)&&doc->apply_palette_cleanup(p,error)==3&&doc->save(file.u8string(),error));auto reopened=Artwork::open(file.u8string(),error);CHECK(reopened&&reopened->disk_version()==version&&reopened->palette_count()==6&&reopened->sprite(0)->palnum==0);doc->undo();CHECK(doc->save(file.u8string(),error)&&bytes(file)==b);}
    CHECK(g_doc==nullptr);fs::remove_all(root);std::cout<<"Duplicate palettes: "<<failures<<" failures\n";return failures?1:0;
}
