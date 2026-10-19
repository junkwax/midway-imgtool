#include "studio_costume_presets.h"
#include <algorithm>
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
static std::string bytes(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void write(const std::filesystem::path &p,const std::string &s){std::ofstream f(p,std::ios::binary);f.write(s.data(),s.size());}
int main(){using namespace studio;namespace fs=std::filesystem;std::string error;auto root=fs::temp_directory_path()/("studio-costume-presets-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto lp=root/"palette_layouts.txt",tp=root/"costume_templates.txt";CostumePresetFile layouts,templates;
    CHECK(read_costume_presets(lp.u8string(),CostumePresetKind::Layout,layouts,error)&&layouts.loaded&&!layouts.existed&&layouts.layouts.empty()&&!fs::exists(lp));
    CHECK(read_costume_presets(tp.u8string(),CostumePresetKind::Template,templates,error));
    std::vector<CostumeSection> current={{"Cloth","1-3, 7","4-6",true},{"Skin","4-6","1-3",true},{"Disabled","99","99",false}};PaletteLayout layout;CostumeTemplate preset;
    CHECK(capture_costume_layout("MK2 NINJA","SCORP_P",8,current,true,layout,error)&&layout.sections.size()==2&&layout.numc==8&&layout.palettes==std::vector<std::string>({"SCORP_P"}));
    CHECK(capture_costume_template("UMK3 TO MK2",8,8,current,preset,error)&&preset.dst.size()==2&&preset.ref.size()==2);
    CHECK(save_costume_layout(layouts,layout,false,error)&&layouts.existed&&bytes(lp)==FormatPaletteLayoutLine(layout)+'\n');CHECK(save_costume_template(templates,preset,false,error)&&bytes(tp)==FormatCostumeTemplateLine(preset)+'\n');
    auto exact=bytes(lp),texact=bytes(tp);auto copy=layouts;CHECK(!save_costume_layout(layouts,layout,false,error)&&bytes(lp)==exact&&layouts.baseline==copy.baseline);
    auto variant=layout;variant.name="mk2 ninja";variant.palettes={"SUB_P"};CHECK(save_costume_layout(layouts,variant,true,error)&&layouts.layouts.size()==1&&layouts.layouts[0].palettes==std::vector<std::string>({"SUB_P","SCORP_P"}));CHECK(FindLayoutForPalette(layouts.layouts,"scorp_p",8)==0&&FindLayoutForPalette(layouts.layouts,"SUB_P",16)<0);
    CostumeMappingReview r;CHECK(review_costume_template(preset,8,8,r,error)&&r.sections.size()==2&&!r.unpaired&&r.sections[0].name=="Cloth"&&r.sections[0].reference=="4-6"&&r.sections[0].enabled);
    std::reverse(preset.ref.begin(),preset.ref.end());preset.ref[1].name="cLoTh";CHECK(review_costume_template(preset,8,8,r,error)&&r.sections[0].reference=="4-6"&&r.sections[1].reference=="1-3");
    auto partial=preset;partial.ref[0].name="Trim";CHECK(review_costume_template(partial,8,8,r,error)&&r.sections.size()==3&&r.unpaired==2&&r.sections[0].enabled&&!r.sections[1].enabled&&!r.sections[2].enabled);
    auto previous=r;CHECK(!review_costume_template(preset,16,8,r,error)&&r.sections.size()==previous.sections.size());CHECK(!review_costume_template(preset,8,16,r,error));
    auto wildcard=preset;wildcard.dst_numc=wildcard.ref_numc=0;CHECK(review_costume_template(wildcard,8,8,r,error));CHECK(!review_costume_template(wildcard,4,8,r,error));
    auto local=std::vector<CostumeSection>{{"CLOTH","2","4-6",false},{"Trim","3","1-3",true}};CHECK(review_costume_layout(local,layout,true,8,8,r,error)&&r.sections.size()==3&&r.unpaired==2&&!r.sections[0].enabled&&r.sections[0].reference=="4-6"&&r.sections[1].reference.empty()&&r.sections[2].destination.empty());
    PaletteLayout reference;CHECK(capture_costume_layout("UMK3 NINJA","RAIN1_P",8,current,false,reference,error));auto active_current=current;active_current.pop_back();CHECK(review_costume_layout(active_current,reference,false,8,8,r,error)&&!r.unpaired&&r.sections[0].destination=="1-3, 7"&&r.sections[0].reference=="4-6");CHECK(!review_costume_layout(current,reference,false,8,16,r,error));
    auto invalid=current;invalid[0].name="Material name longer than fifteen";CHECK(!capture_costume_layout("BAD","PAL",8,invalid,true,layout,error));invalid=current;invalid[1].name="cloth";CHECK(!capture_costume_template("BAD",8,8,invalid,preset,error));invalid=current;invalid[1].destination="2-6";CHECK(!capture_costume_template("BAD",8,8,invalid,preset,error));invalid=current;invalid[0].destination="0-3";CHECK(!capture_costume_layout("BAD","PAL",8,invalid,true,layout,error));invalid=current;invalid[0].destination="1-300";CHECK(!capture_costume_layout("BAD","PAL",256,invalid,true,layout,error));invalid=current;invalid[0].destination="1-8";CHECK(!capture_costume_layout("BAD","PAL",8,invalid,true,layout,error));CHECK(!capture_costume_layout("Bad|Name","PAL",8,current,true,layout,error));
    CHECK(capture_costume_layout("MK2 NINJA","SCORP_P",8,current,true,layout,error)&&capture_costume_template("UMK3 TO MK2",8,8,current,preset,error));
    // Text generated by the classic formatter, including CRLF and wildcard counts.
    auto classic=FormatPaletteLayoutLine(layout)+"\r\n\r\n";write(lp,classic);CostumePresetFile reread;CHECK(read_costume_presets(lp.u8string(),CostumePresetKind::Layout,reread,error)&&reread.layouts.size()==1&&reread.baseline==classic);
    // File changes and disappearance must not be overwritten by an old session.
    CHECK(!save_costume_layout(layouts,layout,true,error)&&bytes(lp)==classic);CHECK(!delete_costume_preset(layouts,"mk2 ninja",error)&&bytes(lp)==classic);layouts=reread;fs::remove(lp);CHECK(!delete_costume_preset(layouts,layout.name,error)&&!fs::exists(lp));CHECK(read_costume_presets(lp.u8string(),CostumePresetKind::Layout,layouts,error)&&save_costume_layout(layouts,layout,false,error));
    exact=bytes(lp);std::vector<std::string> malformed={"BAD|garbage||Cloth=1-3\n","BAD|999999999999999999999||Cloth=1-3\n","BAD|8||Cloth=1-3;cloth=4-6\n","BAD|8||Cloth=1-3;Skin=3-6\n","BAD|8||Cloth=0-3\n","BAD|8||Cloth=1-9\n","BAD|8||Cloth=1-3;ignored\n","BAD|8||SixteenCharNamesX=1-3\n",exact+exact};
    auto before=layouts;for(auto &text:malformed){write(lp,text);CHECK(!read_costume_presets(lp.u8string(),CostumePresetKind::Layout,layouts,error)&&layouts.baseline==before.baseline&&bytes(lp)==text);}write(lp,exact);
    write(lp,std::string(1024*1024+1,'x'));CHECK(!read_costume_presets(lp.u8string(),CostumePresetKind::Layout,layouts,error));write(lp,exact);
    auto valid_templates=bytes(tp);for(auto &bad:std::vector<std::string>{"BAD|8|no|Cloth=1-3|Cloth=4-6\n","BAD|8|8|Cloth=1-3|Cloth=4-6;cloth=1-2\n",valid_templates+valid_templates}){write(tp,bad);auto keep=templates.baseline;CHECK(!read_costume_presets(tp.u8string(),CostumePresetKind::Template,templates,error)&&templates.baseline==keep&&bytes(tp)==bad);}write(tp,valid_templates);
    auto blocked=root/"missing"/"layouts.txt";CostumePresetFile no_dir;CHECK(read_costume_presets(blocked.u8string(),CostumePresetKind::Layout,no_dir,error)&&!save_costume_layout(no_dir,layout,false,error)&&no_dir.layouts.empty()&&!no_dir.existed&&!fs::exists(blocked));
    CostumePresetFile directory;CHECK(!read_costume_presets(root.u8string(),CostumePresetKind::Layout,directory,error));
    CHECK(delete_costume_preset(layouts,"mk2 ninja",error)&&layouts.layouts.empty()&&bytes(lp).empty());CHECK(!delete_costume_preset(layouts,"missing",error));CHECK(save_costume_layout(layouts,layout,false,error));CHECK(delete_costume_preset(templates,"umk3 to mk2",error)&&templates.templates.empty());
    // Saving/loading mappings does not create an IMG edit or an Undo command.
    auto art=Artwork::create();auto generation=art->generation();auto dirty=art->dirty();auto colors=copy_palette(*art,0).words;auto label=std::string(art->undo_label());CHECK(save_costume_template(templates,preset,false,error));CHECK(review_costume_template(templates.templates[0],8,8,r,error));CHECK(art->dirty()==dirty&&art->generation()==generation&&std::string(art->undo_label())==label&&copy_palette(*art,0).words==colors);
    // Atomic publication under injected allocation failures: files and cache agree.
    int rejected=0,succeeded=0;for(int fail=0;fail<200;++fail){auto file=root/"failure-layouts.txt";write(file,exact);CostumePresetFile store;CHECK(read_costume_presets(file.u8string(),CostumePresetKind::Layout,store,error));auto next=layout;next.name="SECOND";bool ok=false;fail_after=fail;try{ok=save_costume_layout(store,next,false,error);}catch(const std::bad_alloc &){}fail_after=-1;if(ok){++succeeded;CHECK(store.layouts.size()==2&&store.baseline==bytes(file));}else{++rejected;CHECK(store.layouts.size()==1&&store.baseline==exact&&bytes(file)==exact);}}CHECK(rejected>20&&succeeded>0);
    // Both preset limits are enforced without replacing the destination.
    auto limit=layouts;limit.layouts.clear();for(int i=0;i<256;++i){auto l=layout;l.name="Layout "+std::to_string(i);limit.layouts.push_back(l);}auto extra=layout;extra.name="Overflow";CHECK(!save_costume_layout(limit,extra,false,error)&&bytes(lp)==exact);
    CHECK(g_doc==nullptr);fs::remove_all(root);std::cout<<"Costume presets: "<<failures<<" failures\n";return failures?1:0;
}
