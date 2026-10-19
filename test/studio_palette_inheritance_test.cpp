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
static std::unique_ptr<studio::Artwork> fixture(std::string &error){using namespace studio;auto a=Artwork::create();
    if(a->add_palette("TARGET",{0x9234,0xfc00,0x801f,0x8000,0x03e0},error)!=1||a->add_palette("DONOR",{0x8888,0x8000,0x801f,0xfc00,0x03e0,0x7fff},error)!=2||!a->canvas_size(0,5,2,0,error)||!a->assign_palette(0,1,false,error)||a->add_sprite("SOURCE",2,2,2,error)!=1)return {};
    auto im=a->sprite(0);unsigned char raw[]={0,1,2,3,4,0x91,0x92,0x93,4,3,2,1,0,0x94,0x95,0x96};memcpy(im->data_p,raw,16);free(im->baseline_p);im->baseline_p=malloc(16);memcpy(im->baseline_p,raw,16);im->baseline_w=5;im->baseline_h=2;im->pttbl_p=calloc(1,40);((short *)im->pttbl_p)[0]=-3;im->anix=7;
    auto pal=a->palette(1);pal->flags=0x81;pal->pad=0x4567;pal->file_spare=0x7331;memcpy(pal->file_name_raw,"TARGET\0tail",12);pal->file_name_raw[15]=0x76;a->palette(2)->pad=0x1234;a->palette(2)->bitspix=6;((unsigned char *)a->sprite(1)->data_p)[0]=5;
    return a;
}
int main(){using namespace studio;namespace fs=std::filesystem;std::string error;auto root=fs::temp_directory_path()/("studio-inherit-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto a=fixture(error);CHECK(a);if(!a)return 1;auto file=root/"original.IMG";CHECK(a->save(file.u8string(),error));auto exact=bytes(file),raw=a->copy(0,0,0,5,2).pixels,donor_pixels=a->copy(1,0,0,2,2).pixels;auto original=copy_palette(*a,1).words,donor=copy_palette(*a,2).words;auto selected=a->selection_id(),id=a->sprite_id(0),generation=a->generation();auto im=a->sprite(0);auto pixel=im->data_p,baseline=im->baseline_p;std::vector<unsigned char> baseline_bytes((unsigned char *)baseline,(unsigned char *)baseline+16);PaletteInheritanceOptions o{1,2,2,false};PaletteInheritancePlan p;
    CHECK(plan_palette_inheritance(*a,o,p,error)&&p.sprites.size()==1&&p.mapping[0]==0&&p.mapping[1]==3&&p.mapping[2]==2&&p.mapping[3]==1&&p.mapping[4]==4&&!p.color_changes&&p.index_changes==4&&p.ppp_changed==false&&!a->dirty()&&a->generation()==generation);
    auto before=render_palette_inheritance(*a,p,0,false),after=render_palette_inheritance(*a,p,0,true);CHECK(before.size()==after.size()&&before[3]==0&&before[3*4+3]==255);for(size_t i=0;i<before.size();i+=4)CHECK(before[i+3]==after[i+3]&&(!before[i+3]||!memcmp(before.data()+i,after.data()+i,3)));
    CHECK(a->inherit_palette_colors(p,false,error)&&copy_palette(*a,1).words==donor&&palette_name(*a->palette(1))=="TARGET"&&a->palette(1)->flags==0x81&&a->palette(1)->file_spare==0x7331&&a->palette(1)->file_name_raw[15]==0x76&&a->palette(1)->pad==0x1234&&a->palette(1)->bitspix==6&&copy_palette(*a,2).words==donor&&a->copy(1,0,0,2,2).pixels==donor_pixels);
    CHECK(im->data_p==pixel&&im->baseline_p==baseline&&im->palnum==1&&im->anix==7&&((short *)im->pttbl_p)[0]==-3&&a->sprite_id(0)==id&&a->selection_id()==selected&&std::string(a->undo_label())=="Inherit palette colors");auto result=a->copy(0,0,0,5,2).pixels;CHECK(result==std::vector<unsigned char>({0,3,2,1,4,4,1,2,3,0}));CHECK(((unsigned char *)im->data_p)[5]==0x91&&((unsigned char *)im->data_p)[15]==0x96&&!memcmp(im->baseline_p,baseline_bytes.data(),16));
    a->undo();CHECK(!a->dirty()&&copy_palette(*a,1).words==original&&a->copy(0,0,0,5,2).pixels==raw);a->redo();CHECK(a->copy(0,0,0,5,2).pixels==result);a->undo();CHECK(a->save(file.u8string(),error)&&bytes(file)==exact);
    // No-op inheritance leaves history/generation intact.
    CHECK(plan_palette_inheritance(*a,o,p,error)&&a->inherit_palette_colors(p,false,error));CHECK(plan_palette_inheritance(*a,o,p,error)&&!p.changed);generation=a->generation();CHECK(a->inherit_palette_colors(p,false,error)&&a->generation()==generation);a->undo();
    // A missing used color requires explicit approval. Transparent nearest color is never a candidate.
    a->color(2,3,0x7be0);a->commit();CHECK(plan_palette_inheritance(*a,o,p,error)&&p.approximate_colors==1&&p.color_changes==2&&p.mapping[1]&&p.mapping[3]==1);CHECK(!a->inherit_palette_colors(p,false,error));auto approximate=render_palette_inheritance(*a,p,0,true);CHECK(approximate!=render_palette_inheritance(*a,p,0,false)&&a->inherit_palette_colors(p,true,error));a->undo();a->undo();
    CHECK(plan_palette_inheritance(*a,o,p,error));auto foreign=Artwork::demo();CHECK(!foreign->inherit_palette_colors(p,true,error));a->begin("Pending");CHECK(!a->inherit_palette_colors(p,true,error));a->cancel();CHECK(!a->inherit_palette_colors(p,true,error));
    CHECK(plan_palette_inheritance(*a,o,p,error));((unsigned char *)a->palette(2)->data_p)[2]^=1;CHECK(!a->inherit_palette_colors(p,true,error));((unsigned char *)a->palette(2)->data_p)[2]^=1;
    CHECK(plan_palette_inheritance(*a,o,p,error));((unsigned char *)a->sprite(0)->data_p)[5]^=1;CHECK(!a->inherit_palette_colors(p,true,error));((unsigned char *)a->sprite(0)->data_p)[5]^=1;
    CHECK(plan_palette_inheritance(*a,o,p,error));a->palette(1)->file_spare^=1;CHECK(!a->inherit_palette_colors(p,true,error));a->palette(1)->file_spare^=1;
    // Active alternates on a primary user or an alternate-only reference block remapping.
    im=a->sprite(0);im->opaltbl_p=malloc(16);memset(im->opaltbl_p,0xff,16);((unsigned char *)im->opaltbl_p)[0]=5;CHECK(!plan_palette_inheritance(*a,o,p,error));((unsigned char *)im->opaltbl_p)[0]=0xff;((unsigned char *)im->opaltbl_p)[15]=4;CHECK(plan_palette_inheritance(*a,o,p,error));
    auto other=a->sprite(1);other->opaltbl_p=malloc(16);memset(other->opaltbl_p,0xff,16);((unsigned char *)other->opaltbl_p)[0]=4;CHECK(!plan_palette_inheritance(*a,o,p,error));free(other->opaltbl_p);other->opaltbl_p=nullptr;
    auto clip=a->copy(0,0,0,2,2);CHECK(a->paste_layer(0,clip,10,10,error)&&a->edit_layer(0,10,10,false,false,false,error)&&!plan_palette_inheritance(*a,o,p,error));a->undo();a->undo();
    im=a->sprite(0);((unsigned char *)im->data_p)[0]=255;CHECK(!plan_palette_inheritance(*a,o,p,error));((unsigned char *)im->data_p)[0]=0;CHECK(!plan_palette_inheritance(*a,{1,1},p,error));CHECK(!plan_palette_inheritance(*a,{1,2,9},p,error));
    CHECK(a->add_palette("UNUSED",original,error)==3&&plan_palette_inheritance(*a,{3,2},p,error)&&p.sprites.empty()&&a->inherit_palette_colors(p,false,error)&&copy_palette(*a,3).words==donor);a->undo();a->undo();
    int rejected=0,succeeded=0;for(int fail=0;fail<180;++fail){auto doc=fixture(error);doc->color(1,1,0x1234);doc->commit();doc->undo();CHECK(plan_palette_inheritance(*doc,o,p,error));auto g=doc->generation();auto dirty=doc->dirty();auto ptr=doc->palette(1)->data_p;auto undo=std::string(doc->undo_label());bool ok=false;fail_after=fail;try{ok=doc->inherit_palette_colors(p,false,error);}catch(const std::bad_alloc &){}fail_after=-1;if(!ok){++rejected;CHECK(doc->generation()==g&&doc->dirty()==dirty&&doc->can_redo()&&std::string(doc->undo_label())==undo&&doc->palette(1)->data_p==ptr&&copy_palette(*doc,1).words==original&&doc->copy(0,0,0,5,2).pixels==raw);}else{++succeeded;CHECK(!doc->can_redo());doc->undo();CHECK(copy_palette(*doc,1).words==original&&doc->copy(0,0,0,5,2).pixels==raw);}}
    CHECK(rejected>20&&succeeded>0);
    // RGB and perceptual distances choose different nearest opaque colors.
    auto weighted=Artwork::create();CHECK(weighted->add_palette("BLACK",{0x7fff,0x8000},error)==1&&weighted->add_palette("CHOICES",{0,0x1000,0x0060},error)==2&&weighted->assign_palette(0,1,false,error));((unsigned char *)weighted->sprite(0)->data_p)[0]=1;
    CHECK(plan_palette_inheritance(*weighted,{1,2,1,false},p,error)&&p.mapping[1]==2&&p.ppp_changed);CHECK(plan_palette_inheritance(*weighted,{1,2,1,true},p,error)&&p.mapping[1]==1&&p.color_changes==1&&weighted->inherit_palette_colors(p,true,error)&&((unsigned char *)weighted->sprite(0)->data_p)[0]==1);
    // The final byte index remains addressable; transparent source black cannot win matching.
    auto full=Artwork::create();std::vector<unsigned short> many(256,0x7fff);many[0]=0;many[255]=0x8000;CHECK(full->add_palette("BLACK",{0x1234,0x8000},error)==1&&full->add_palette("FULL",many,error)==2&&full->assign_palette(0,1,false,error));((unsigned char *)full->sprite(0)->data_p)[0]=1;
    CHECK(plan_palette_inheritance(*full,{1,2},p,error)&&p.mapping[1]==255&&!p.color_changes&&full->inherit_palette_colors(p,false,error)&&((unsigned char *)full->sprite(0)->data_p)[0]==255&&full->palette(1)->numc==256);
    CHECK(full->add_palette("EMPTY",{0},error)==3&&!plan_palette_inheritance(*full,{1,3},p,error));
    auto seed=fixture(error);CHECK(seed->save((root/"seed.IMG").u8string(),error));auto seed_bytes=bytes(root/"seed.IMG");for(unsigned version:{0x60au,0x61cu,0x61du,0x631u,0x634u}){auto b=seed_bytes;if(version<0x634)CHECK(project_img_layout(b,version,error));auto path=root/(std::to_string(version)+".IMG");{std::ofstream f(path,std::ios::binary);f.write((char *)b.data(),b.size());}auto doc=Artwork::open(path.u8string(),error);CHECK(doc);if(!doc)continue;CHECK(plan_palette_inheritance(*doc,o,p,error)&&doc->inherit_palette_colors(p,false,error)&&doc->save(path.u8string(),error));auto disk=Artwork::open(path.u8string(),error);CHECK(disk&&disk->disk_version()==version&&disk->sprite(0)->palnum==1&&copy_palette(*disk,1).words==donor&&copy_palette(*disk,2).words==donor&&disk->copy(0,0,0,5,2).pixels==result);doc->undo();CHECK(doc->save(path.u8string(),error)&&bytes(path)==b);}
    CHECK(g_doc==nullptr);fs::remove_all(root);std::cout<<"Palette inheritance: "<<failures<<" failures\n";return failures?1:0;
}
