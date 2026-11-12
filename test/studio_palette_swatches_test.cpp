#include <algorithm>
#include "studio_palette_swatches.h"
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
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x " ("<<error<<")\n";++failures;}}while(0)
static std::vector<unsigned char> bytes(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(){using namespace studio;namespace fs=std::filesystem;std::string error;
    auto root=fs::temp_directory_path()/("studio-swatches-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto source=Artwork::demo();auto words=copy_palette(*source,0).words;std::array<bool,256> selected{};selected[2]=selected[8]=selected[15]=true;PaletteSwatchClipboard clip;
    auto generation=source->generation();CHECK(source->copy_palette_swatches(0,selected,1,clip,error)&&clip.slots.size()==3&&clip.slots[0].index==2&&clip.slots[2].index==15&&source->generation()==generation&&!source->dirty());
    selected[200]=true;CHECK(!source->copy_palette_swatches(0,selected,1,clip,error)&&clip.slots.size()==3);source->begin("Pending");CHECK(!source->copy_palette_swatches(0,{},1,clip,error));source->cancel();source->color(0,2,0x1234);source->commit();source.reset();CHECK(clip.slots[0].word==words[2]);
    auto a=Artwork::demo();auto original=copy_palette(*a,0).words;auto pixels=a->copy(0,0,0,48,64).pixels;auto identity=a->sprite_id(0);auto selection=a->selection_id();auto pal=a->palette(0);pal->flags=0x81;pal->pad=0x4567;pal->file_spare=0x1234;memcpy(pal->file_name_raw,pal->n_s,10);
    auto file=root/"base.IMG";CHECK(a->save(file.u8string(),error));auto original_file=bytes(file);
    auto different=clip;different.slots[0].word=0xffff;different.slots[1].word=0x4321;PaletteSwatchPlan p;
    CHECK(plan_palette_swatches(*a,0,different,SwatchPaste::SameIndices,1,4,p,error)&&p.growth==0&&p.changed==2&&p.primary==8&&p.alternate==0&&p.ppp_images==0&&p.words[0]==original[0]);
    void *colors=pal->data_p,*baseline=a->sprite(0)->baseline_p;CHECK(a->apply_palette_swatches(p,error)&&pal->data_p==colors&&pal->flags==0x81&&pal->pad==0x4567&&pal->file_spare==0x1234&&copy_palette(*a,0).words[2]==0xffff&&a->copy(0,0,0,48,64).pixels==pixels&&a->sprite(0)->baseline_p==baseline&&a->sprite_id(0)==identity&&a->selection_id()==selection);
    a->undo();CHECK(copy_palette(*a,0).words==original&&!a->dirty());a->redo();CHECK(copy_palette(*a,0).words[8]==0x4321);a->undo();
    generation=a->generation();auto label=std::string(a->undo_label());CHECK(plan_palette_swatches(*a,0,clip,SwatchPaste::SameIndices,1,4,p,error)&&!p.changed&&!p.growth&&a->apply_palette_swatches(p,error)&&a->generation()==generation&&std::string(a->undo_label())==label);
    CHECK(plan_palette_swatches(*a,0,different,SwatchPaste::Append,1,4,p,error)&&p.growth==3&&p.words.size()==19&&p.destinations==std::vector<int>({16,17,18})&&p.ppp_images==8&&p.depth==5);
    CHECK(a->apply_palette_swatches(p,error)&&a->palette(0)->numc==19&&a->palette(0)->bitspix==5&&a->palette(0)->flags==0x81);auto expanded=copy_palette(*a,0).words;CHECK(std::equal(original.begin(),original.end(),expanded.begin())&&expanded[16]==0xffff&&a->copy(0,0,0,48,64).pixels==pixels);a->undo();CHECK(copy_palette(*a,0).words==original);a->redo();CHECK(a->palette(0)->numc==19);a->undo();
    PaletteSwatchClipboard sparse;sparse.source="GAPS";sparse.slots={{0,0x7c00},{22,0x81ff}};
    CHECK(plan_palette_swatches(*a,0,sparse,SwatchPaste::SameIndices,1,4,p,error)&&p.skipped_zero==1&&p.destinations[0]==-1&&p.growth==7&&p.words[0]==original[0]&&p.words[21]==0&&p.words[22]==0x81ff);
    CHECK(a->apply_palette_swatches(p,error)&&a->palette(0)->numc==23&&a->copy(0,0,0,48,64).pixels==pixels);a->undo();
    PaletteSwatchClipboard zero;CHECK(a->copy_palette_swatches(0,{},0,zero,error)&&zero.slots.size()==1&&zero.slots[0].index==0);
    CHECK(!plan_palette_swatches(*a,0,zero,SwatchPaste::SameIndices,1,6,p,error));CHECK(!plan_palette_swatches(*a,0,zero,SwatchPaste::AtSlot,0,6,p,error));CHECK(!plan_palette_swatches(*a,0,clip,SwatchPaste::AtSlot,2,6,p,error));
    CHECK(plan_palette_swatches(*a,0,zero,SwatchPaste::Append,1,6,p,error)&&p.words[16]==original[0]&&p.destinations[0]==16);CHECK(a->apply_palette_swatches(p,error));a->undo();
    zero.slots[0].word=0x8432;CHECK(plan_palette_swatches(*a,0,zero,SwatchPaste::AtSlot,3,6,p,error)&&p.words[3]==0x8432&&p.words[0]==original[0]);CHECK(a->apply_palette_swatches(p,error)&&a->palette(0)->numc==16);a->undo();
    PaletteSwatchClipboard last;last.slots={{255,0x1234}};CHECK(plan_palette_swatches(*a,0,last,SwatchPaste::SameIndices,1,6,p,error)&&p.words.size()==256&&p.depth==8&&a->apply_palette_swatches(p,error));CHECK(!plan_palette_swatches(*a,0,clip,SwatchPaste::Append,1,6,p,error));a->undo();
    auto invalid=clip;invalid.slots[1].index=invalid.slots[0].index;CHECK(!plan_palette_swatches(*a,0,invalid,SwatchPaste::SameIndices,1,6,p,error));invalid=clip;invalid.slots[0].index=-1;CHECK(!plan_palette_swatches(*a,0,invalid,SwatchPaste::SameIndices,1,6,p,error));invalid=clip;invalid.slots[2].index=256;CHECK(!plan_palette_swatches(*a,0,invalid,SwatchPaste::SameIndices,1,6,p,error));
    CHECK(plan_palette_swatches(*a,0,different,SwatchPaste::SameIndices,1,6,p,error));auto forged=p;forged.words[0]^=1;generation=a->generation();CHECK(!a->apply_palette_swatches(forged,error)&&a->generation()==generation);forged=p;++forged.primary;CHECK(!a->apply_palette_swatches(forged,error));forged=p;++forged.destinations[0];CHECK(!a->apply_palette_swatches(forged,error));
    auto foreign=Artwork::demo();CHECK(!foreign->apply_palette_swatches(p,error));a->begin("Pending");CHECK(!a->apply_palette_swatches(p,error));a->cancel();CHECK(!a->apply_palette_swatches(p,error));
    CHECK(plan_palette_swatches(*a,0,different,SwatchPaste::Append,1,6,p,error));auto data=(unsigned char *)a->palette(0)->data_p;data[4]^=1;CHECK(!a->apply_palette_swatches(p,error));data[4]^=1;
    // A palette-only edit keeps layers, pixels and primary/alternate indices fixed.
    auto im=a->sprite(0);im->opaltbl_p=malloc(16);memset(im->opaltbl_p,0xff,16);((unsigned char *)im->opaltbl_p)[0]=NUMDEFPAL;auto image_clip=a->copy(0,0,0,2,2);image_clip.pixels.assign(4,8);CHECK(a->paste_layer(0,image_clip,2,3,error));auto layer=im->layer_p;
    CHECK(plan_palette_swatches(*a,0,different,SwatchPaste::SameIndices,1,6,p,error)&&p.alternate==1&&a->apply_palette_swatches(p,error)&&im->layer_p==layer&&im->palnum==0&&((unsigned char *)im->opaltbl_p)[0]==NUMDEFPAL&&a->copy(0,0,0,48,64).pixels==pixels);a->undo();CHECK(a->sprite(0)->layer_p);a->undo();
    int rejected=0,succeeded=0;
    for(int fail=0;fail<130;++fail){auto doc=Artwork::demo();CHECK(plan_palette_swatches(*doc,0,different,SwatchPaste::Append,1,4,p,error));auto g=doc->generation();auto before=copy_palette(*doc,0).words;auto ptr=doc->palette(0)->data_p;bool ok=false;fail_after=fail;try{ok=doc->apply_palette_swatches(p,error);}catch(const std::bad_alloc &){}fail_after=-1;
        if(!ok){++rejected;CHECK(doc->generation()==g&&!doc->dirty()&&doc->palette(0)->data_p==ptr&&copy_palette(*doc,0).words==before);}else{++succeeded;doc->undo();CHECK(copy_palette(*doc,0).words==before&&!doc->dirty());}
    }CHECK(rejected>10&&succeeded>0);
    auto seed=Artwork::demo();CHECK(seed->save((root/"seed.IMG").u8string(),error));auto seed_bytes=bytes(root/"seed.IMG");
    for(unsigned version:{0x60au,0x61cu,0x61du,0x631u,0x634u}){auto b=seed_bytes;if(version<0x634)CHECK(project_img_layout(b,version,error));auto path=root/(std::to_string(version)+".IMG");{std::ofstream f(path,std::ios::binary);f.write((char *)b.data(),b.size());}auto doc=Artwork::open(path.u8string(),error);CHECK(doc);if(!doc)continue;CHECK(plan_palette_swatches(*doc,0,different,SwatchPaste::Append,1,4,p,error)&&doc->apply_palette_swatches(p,error)&&doc->save(path.u8string(),error));auto disk=Artwork::open(path.u8string(),error);CHECK(disk&&disk->disk_version()==version&&copy_palette(*disk,0).words==p.words&&disk->palette(0)->bitspix==5);doc->undo();CHECK(doc->save(path.u8string(),error)&&bytes(path)==b);}
    CHECK(g_doc==nullptr);fs::remove_all(root);return failures?1:0;
}
