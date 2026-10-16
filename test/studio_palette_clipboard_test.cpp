#include "studio_palette.h"
#include "studio_img_codec.h"
#include "studio_layers.h"
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
static bool same(const studio::PaletteTransfer &a,const studio::PaletteTransfer &b){if(a.name!=b.name||a.words!=b.words||a.entries.size()!=b.entries.size())return false;for(size_t i=0;i<a.entries.size();++i)if(memcmp(&a.entries[i].fields,&b.entries[i].fields,sizeof(PAL))||a.entries[i].words!=b.entries[i].words)return false;return true;}
int main(){using namespace studio;namespace fs=std::filesystem;std::string error;
    auto root=fs::temp_directory_path()/("studio-palette-clipboard-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto source=Artwork::demo();auto words=copy_palette(*source,0).words;words[0]=0x8000;words[1]=0xffff;
    CHECK(source->add_palette("COSTUME",words,error)==1&&source->add_palette("OTHER",words,error)==2);
    auto p=source->palette(1);p->bitspix=0;p->flags=0x81;p->pad=0x3921;p->file_data=0x1234;p->file_lib=0xabcd;p->file_colind=71;p->file_cmap=29;p->file_spare=0x7531;memcpy(p->file_name_raw,p->n_s,10);p->file_name_raw[8]=0xa9;p->file_name_raw[9]=0x76;
    PaletteTransfer clip;auto g=source->generation();CHECK(source->copy_palettes({1,0},clip,error)&&clip.entries.size()==2&&clip.name=="STUDIOPAL"&&clip.words==copy_palette(*source,0).words&&source->generation()==g);
    CHECK(clip.entries[0].fields.nxt_p==nullptr&&clip.entries[0].fields.data_p==nullptr&&clip.entries[0].fields.temp==nullptr&&clip.entries[1].fields.flags==0x81&&clip.entries[1].fields.bitspix==0&&clip.entries[1].words==words);
    auto frozen=clip;CHECK(!source->copy_palettes({},clip,error)&&same(clip,frozen));CHECK(!source->copy_palettes({0,0},clip,error)&&same(clip,frozen));CHECK(!source->copy_palettes({0,3},clip,error)&&same(clip,frozen));CHECK(!source->copy_palettes({-1},clip,error)&&same(clip,frozen));
    source->begin("Pending color");CHECK(!source->copy_palettes({0},clip,error)&&same(clip,frozen));source->cancel();source->color(1,1,0x1234);source->commit();source.reset();CHECK(same(clip,frozen));
    auto a=Artwork::demo();auto original=a->copy(0,0,0,48,64).pixels;auto selection=a->selection_id();void *base=a->sprite(0)->baseline_p,*palette=a->palette(0);auto file=root/"base.IMG";CHECK(a->save(file.u8string(),error));auto original_file=bytes(file);
    CHECK(a->paste_palettes(clip,error)==1&&a->palette_count()==3&&a->dirty()&&a->selection_id()==selection&&a->sprite(0)->baseline_p==base&&a->palette(0)==palette&&a->sprite(0)->palnum==0&&a->copy(0,0,0,48,64).pixels==original&&same(clip,frozen));
    CHECK(palette_name(*a->palette(1))=="STUDIOP_2"&&palette_name(*a->palette(2))=="COSTUME");p=a->palette(2);
    CHECK(!memcmp(p->file_name_raw,frozen.entries[1].fields.file_name_raw,10)&&p->flags==0x81&&p->bitspix==0&&p->pad==0x3921&&p->file_data==0x1234&&p->file_lib==0xabcd&&p->file_colind==71&&p->file_cmap==29&&p->file_spare==0x7531&&copy_palette(*a,2).words==words);
    CHECK(a->paste_palettes(clip,error)==3&&a->palette_count()==5&&palette_name(*a->palette(3))=="STUDIOP_3"&&palette_name(*a->palette(4))=="COSTUME_2");for(auto c:a->palette(4)->file_name_raw)CHECK(c==0);
    a->undo();CHECK(a->palette_count()==3);a->undo();CHECK(a->palette_count()==1&&!a->dirty());a->redo();CHECK(a->palette_count()==3&&a->palette(2)->file_spare==0x7531);a->undo();CHECK(a->save(file.u8string(),error)&&bytes(file)==original_file);
    CHECK(a->paste_palettes(clip,error)==1&&a->save((root/"copy.IMG").u8string(),error));auto reopened=Artwork::open((root/"copy.IMG").u8string(),error);CHECK(reopened&&reopened->palette_count()==3&&reopened->disk_version()==0x634&&reopened->palette(2)->flags==0x81&&reopened->palette(2)->bitspix==0&&reopened->palette(2)->file_name_raw[9]==0x76&&reopened->palette(2)->file_spare==0x7531&&copy_palette(*reopened,2).words==words);a->undo();
    g=a->generation();auto invalid=clip;invalid.entries[1].fields.data_p=(void *)1;CHECK(a->paste_palettes(invalid,error)<0&&a->generation()==g);invalid=clip;invalid.entries[1].fields.bitspix=9;CHECK(a->paste_palettes(invalid,error)<0);invalid=clip;invalid.entries[1].fields.n_s[9]='X';CHECK(a->paste_palettes(invalid,error)<0);invalid=clip;invalid.entries[1].words.pop_back();CHECK(a->paste_palettes(invalid,error)<0);invalid=clip;invalid.words[0]^=1;CHECK(a->paste_palettes(invalid,error)<0);CHECK(a->palette_count()==1&&a->generation()==g);
    a->begin("Pending edit");CHECK(a->paste_palettes(clip,error)<0&&a->palette_count()==1);a->cancel();
    // Single PAL/ACT transfer remains compatible, without batch metadata.
    auto single=copy_palette(*a,0);CHECK(single.entries.empty()&&a->paste_palettes(single,error)==1&&a->palette_count()==2);a->undo();
    // Append leaves primary, alternate and layer references untouched.
    auto im=a->sprite(0);im->opaltbl_p=malloc(16);memset(im->opaltbl_p,0xff,16);((unsigned char *)im->opaltbl_p)[0]=NUMDEFPAL;auto pixels=a->copy(0,0,0,2,2);pixels.pixels.assign(4,9);CHECK(a->paste_layer(0,pixels,0,0,error));auto layer=im->layer_p;CHECK(a->paste_palettes(clip,error)==1&&im->layer_p==layer&&im->palnum==0&&((unsigned char *)im->opaltbl_p)[0]==NUMDEFPAL);a->undo();CHECK(a->sprite(0)->layer_p&&a->palette_count()==1);a->undo();
    auto limit=std::make_unique<Artwork>();PAL **tail=(PAL **)&limit->data.pal_p;for(int i=0;i<4092;++i){auto pal=(PAL *)calloc(1,sizeof(PAL));pal->numc=1;pal->data_p=calloc(1,2);strcpy(pal->n_s,"LIMIT");*tail=pal;tail=(PAL **)&pal->nxt_p;++limit->data.palcnt;}CHECK(limit->paste_palettes(clip,error)<0&&limit->palette_count()==4092&&!limit->dirty()&&!limit->can_undo());
    int rejected=0,succeeded=0,capture_rejected=0;
    for(int fail=0;fail<160;++fail){auto doc=Artwork::demo();doc->anchor(0,17,18);doc->commit();doc->undo();auto expected=copy_palette(*doc,0);auto generation=doc->generation();int result=-1;
        fail_after=fail;try{result=doc->paste_palettes(clip,error);}catch(const std::bad_alloc &){}fail_after=-1;
        if(result<0){++rejected;CHECK(doc->palette_count()==1&&doc->generation()==generation&&!doc->dirty()&&doc->can_redo());}else{++succeeded;CHECK(result==1&&doc->palette_count()==3);doc->undo();CHECK(doc->palette_count()==1&&!doc->dirty());}
        CHECK(copy_palette(*doc,0).words==expected.words);
        auto out=clip;bool ok=false;generation=doc->generation();fail_after=fail;try{ok=doc->copy_palettes({0},out,error);}catch(const std::bad_alloc &){}fail_after=-1;
        if(!ok){++capture_rejected;CHECK(same(out,clip));}else CHECK(out.entries.size()==1&&out.words==expected.words);CHECK(doc->generation()==generation);
    }CHECK(rejected>10&&succeeded>0&&capture_rejected>0);
    // Append and Undo retain the destination's original record layout.
    auto seed=Artwork::demo();CHECK(seed->save((root/"seed.IMG").u8string(),error));auto seed_bytes=bytes(root/"seed.IMG");
    for(unsigned version:{0x60au,0x61cu,0x61du,0x631u,0x634u}){auto b=seed_bytes;if(version<0x634)CHECK(project_img_layout(b,version,error));auto path=root/(std::to_string(version)+".IMG");{std::ofstream f(path,std::ios::binary);f.write((char *)b.data(),b.size());}auto doc=Artwork::open(path.u8string(),error);CHECK(doc);if(!doc)continue;CHECK(doc->paste_palettes(clip,error)==1&&doc->save(path.u8string(),error));auto disk=Artwork::open(path.u8string(),error);CHECK(disk&&disk->disk_version()==version&&disk->palette_count()==3&&disk->palette(2)->file_name_raw[9]==0x76&&copy_palette(*disk,2).words==words);doc->undo();CHECK(doc->save(path.u8string(),error)&&bytes(path)==b);}
    CHECK(g_doc==nullptr);fs::remove_all(root);return failures?1:0;
}
