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
static std::unique_ptr<studio::Artwork> fixture(bool extra,std::string &error){using namespace studio;auto a=Artwork::create();std::vector<unsigned short> words(32,0x1111);words[0]=0x9234;words[1]=0x7fff;words[2]=0x801f;words[4]=0x8000;words[6]=0x7c00;words[8]=0x03e0;words[9]=0x7fe0;
    if(a->add_palette("SOURCE",words,error)!=1||!a->canvas_size(0,5,2,0,error)||!a->assign_palette(0,1,false,error))return {};auto im=a->sprite(0);unsigned char raw[]={0,2,4,6,0,0x91,0x92,0x93,6,2,4,0,2,0x94,0x95,0x96};memcpy(im->data_p,raw,16);free(im->baseline_p);im->baseline_p=malloc(16);memcpy(im->baseline_p,raw,16);im->baseline_w=5;im->baseline_h=2;im->pttbl_p=calloc(1,40);((short *)im->pttbl_p)[0]=-3;im->anix=7;
    auto pal=a->palette(1);pal->flags=0x81;pal->pad=0x4567;pal->file_spare=0x7331;memcpy(pal->file_name_raw,"SOURCE\0tail",12);pal->file_name_raw[15]=0x76;
    if(extra){if(a->add_sprite("ALT",5,2,0,error)!=1)return {};auto alt=a->sprite(1);((unsigned char *)alt->data_p)[0]=1;alt->opaltbl_p=malloc(16);memset(alt->opaltbl_p,0xff,16);((unsigned char *)alt->opaltbl_p)[0]=4;((unsigned char *)alt->opaltbl_p)[15]=4;auto clip=a->copy(0,0,0,2,2);clip.pixels={8,9,0,0};if(!a->paste_layer(0,clip,20,20,error)||!a->edit_layer(0,20,20,false,false,false,error))return {};}
    return a;
}
int main(){using namespace studio;namespace fs=std::filesystem;std::string error;auto root=fs::temp_directory_path()/("studio-clean-copy-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto a=fixture(true,error);CHECK(a);if(!a)return 1;auto file=root/"source.IMG";CHECK(a->save(file.u8string(),error));auto exact=bytes(file);auto words=copy_palette(*a,1).words;auto im=a->sprite(0);auto pixel=im->data_p,baseline=im->baseline_p,layer=im->layer_p;auto pal=a->palette(1);auto buffer=pal->data_p;auto selected=a->selection_id(),id=a->sprite_id(0),generation=a->generation();auto raw=a->copy(0,0,0,5,2).pixels;
    PaletteCompactionOptions o{1,4,true,false,PaletteColorOrder::DarkToLight,true};PaletteCompactionPlan p;
    CHECK(plan_palette_compaction(*a,o,p,error)&&p.words.size()==7&&p.words[0]==words[0]&&p.mapping[1]&&p.mapping[8]&&p.mapping[9]&&p.depth==5&&p.alternate_sprites==1&&p.primary[8]==1&&p.alternate[1]==1&&!a->dirty()&&a->generation()==generation);
    CHECK(render_palette_compaction(*a,p,0,false)==render_palette_compaction(*a,p,0,true)&&!a->compact_palette(p,error));auto expected=p.words;
    CHECK(a->copy_clean_palette(p,error)==2&&a->palette_count()==3&&palette_name(*a->palette(2))=="SOURCE_2"&&copy_palette(*a,2).words==expected&&a->palette(2)->bitspix==5&&a->palette(2)->flags==0x81&&a->palette(2)->pad==0x4567&&a->palette(2)->file_spare==0x7331&&a->palette(2)->file_name_raw[9]==0&&a->palette(2)->file_name_raw[15]==0x76);
    CHECK(copy_palette(*a,1).words==words&&a->sprite(0)->palnum==1&&((unsigned char *)a->sprite(1)->opaltbl_p)[0]==4&&((unsigned char *)a->sprite(1)->opaltbl_p)[15]==4&&im->data_p==pixel&&im->baseline_p==baseline&&im->layer_p==layer&&pal->data_p==buffer&&a->copy(0,0,0,5,2).pixels==raw&&a->selection_id()==selected&&a->sprite_id(0)==id&&im->anix==7&&((short *)im->pttbl_p)[0]==-3&&std::string(a->undo_label())=="Create clean palette copy");
    a->undo();CHECK(a->palette_count()==2&&!a->dirty());a->redo();CHECK(a->palette_count()==3);a->undo();CHECK(a->save(file.u8string(),error)&&bytes(file)==exact);
    o.recompute_depth=true;o.order=PaletteColorOrder::ColorFamilies;CHECK(plan_palette_compaction(*a,o,p,error)&&p.depth==3&&a->copy_clean_palette(p,error)==2);CHECK(plan_palette_compaction(*a,o,p,error)&&a->copy_clean_palette(p,error)==3&&palette_name(*a->palette(3))=="SOURCE_3");a->undo();a->undo();
    CHECK(a->edit_layer(0,0,0,true,false,false,error)&&plan_palette_compaction(*a,o,p,error));auto visible=render_palette_compaction(*a,p,0,false);CHECK(visible==render_palette_compaction(*a,p,0,true)&&visible[3]==255);auto l=img_layer(a->sprite(0));layer_pixels(l)[0]=10;CHECK(a->copy_clean_palette(p,error)<0);layer_pixels(l)[0]=8;CHECK(a->copy_clean_palette(p,error)==2);a->undo();a->undo();
    CHECK(plan_palette_compaction(*a,o,p,error));auto foreign=Artwork::demo();CHECK(foreign->copy_clean_palette(p,error)<0);a->begin("Pending");CHECK(a->copy_clean_palette(p,error)<0);a->cancel();CHECK(a->copy_clean_palette(p,error)<0);CHECK(plan_palette_compaction(*a,o,p,error));((unsigned char *)a->palette(1)->data_p)[4]^=1;CHECK(a->copy_clean_palette(p,error)<0);((unsigned char *)a->palette(1)->data_p)[4]^=1;
    auto inplace=o;inplace.copy_only=false;inplace.order=PaletteColorOrder::Keep;auto plain=fixture(false,error);CHECK(plan_palette_compaction(*plain,inplace,p,error)&&plain->copy_clean_palette(p,error)<0);
    // A palette without users keeps all its colors, rather than guessing that they are disposable.
    CHECK(a->add_palette("UNUSED",words,error)==2);o.palette=2;CHECK(plan_palette_compaction(*a,o,p,error)&&p.preview_sprites.empty()&&p.words.size()==words.size()&&a->copy_clean_palette(p,error)==3&&a->palette(3)->numc==32);a->undo();a->undo();o.palette=1;
    CHECK(plan_palette_compaction(*a,o,p,error));auto count=a->data.palcnt;a->data.palcnt=4093;CHECK(a->copy_clean_palette(p,error)<0);a->data.palcnt=count;
    auto invalid=img_layer(a->sprite(0));int stride=invalid->stride;invalid->stride=1;CHECK(!plan_palette_compaction(*a,o,p,error));invalid->stride=stride;
    int rejected=0,succeeded=0;for(int fail=0;fail<180;++fail){auto doc=fixture(true,error);doc->color(1,1,0x1234);doc->commit();doc->undo();CHECK(plan_palette_compaction(*doc,o,p,error));auto g=doc->generation();auto dirty=doc->dirty();auto ptr=doc->palette(1)->data_p;auto undo=std::string(doc->undo_label());int copied=-1;fail_after=fail;try{copied=doc->copy_clean_palette(p,error);}catch(const std::bad_alloc &){}fail_after=-1;if(copied<0){++rejected;CHECK(doc->generation()==g&&doc->dirty()==dirty&&doc->can_redo()&&doc->palette_count()==2&&std::string(doc->undo_label())==undo&&doc->palette(1)->data_p==ptr&&copy_palette(*doc,1).words==words&&doc->sprite(0)->palnum==1);}else{++succeeded;CHECK(copied==2&&!doc->can_redo());doc->undo();CHECK(doc->palette_count()==2&&copy_palette(*doc,1).words==words);}}
    CHECK(rejected>20&&succeeded>0);
    auto seed=fixture(false,error);CHECK(seed->save((root/"seed.IMG").u8string(),error));auto seed_bytes=bytes(root/"seed.IMG");for(unsigned version:{0x60au,0x61cu,0x61du,0x631u,0x634u}){auto b=seed_bytes;if(version<0x634)CHECK(project_img_layout(b,version,error));auto path=root/(std::to_string(version)+".IMG");{std::ofstream f(path,std::ios::binary);f.write((char *)b.data(),b.size());}auto doc=Artwork::open(path.u8string(),error);CHECK(doc);if(!doc)continue;CHECK(plan_palette_compaction(*doc,o,p,error));auto source=copy_palette(*doc,1).words;CHECK(doc->copy_clean_palette(p,error)==2&&doc->save(path.u8string(),error));auto disk=Artwork::open(path.u8string(),error);CHECK(disk&&disk->disk_version()==version&&disk->palette_count()==3&&disk->sprite(0)->palnum==1&&copy_palette(*disk,1).words==source&&copy_palette(*disk,2).words==p.words&&disk->palette(2)->bitspix==p.depth);doc->undo();CHECK(doc->save(path.u8string(),error)&&bytes(path)==b);}
    CHECK(g_doc==nullptr);fs::remove_all(root);std::cout<<"Clean palette copy: "<<failures<<" failures\n";return failures?1:0;
}
