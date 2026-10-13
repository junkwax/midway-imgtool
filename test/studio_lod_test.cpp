#include "studio_lod.h"
#include "img_io.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x " ("<<error<<")\n";++failures;}}while(0)
static std::vector<char> bytes(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void text(const std::filesystem::path &p,const std::string &s){std::ofstream f(p,std::ios::binary);f.write(s.data(),s.size());}
int main(int argc,char **argv){using namespace studio;namespace fs=std::filesystem;std::string error;auto root=fs::temp_directory_path()/("studio-lod-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root/"assets");fs::create_directories(root/"fallback");auto a=Artwork::demo(),b=Artwork::demo();auto af=root/"assets"/fs::u8path(u8"演员 A.IMG"),bf=root/"assets"/"B.IMG",manifest=root/"review.LOD";CHECK(a->save(af.u8string(),error)&&b->save(bf.u8string(),error));auto ab=bytes(af),bb=bytes(bf);int saved_ppp=g_load2_ppp;a->anchor(0,30,61);a->commit();auto generation=a->generation();CHECK(a->dirty());
    LodReview plan;std::vector<size_t> included;
    auto review=[&](const std::string &s,const std::string &over=std::string(),const std::string &env=std::string()){text(manifest,s);return review_lod(manifest.u8string(),over,env,plan,error);};
    CHECK(review(std::string("\xef\xbb\xbf")+u8"; comment\n  PPP> 2 ; two bits\r\n \"assets/演员 A.IMG\"\r\nPPP> 0\nassets\\B.IMG\nPPP> 6\nassets/B.IMG\nMISSING.IMG\nASM> output.TBL\nZON>\nLOADS> 2\nOF> 3\nADDRESS> .\n---> SENTINEL1\n"));
    if(plan.libraries.size()!=4){fs::remove_all(root);return 1;}
    CHECK(plan.libraries.size()==4&&plan.directives.size()==6&&plan.libraries[0].line==3&&plan.libraries[0].ppp==2&&plan.libraries[0].explicit_ppp&&plan.libraries[1].ppp==0&&plan.libraries[1].explicit_ppp);CHECK(plan.libraries[0].artwork&&plan.libraries[0].artwork->count()==8&&!plan.libraries[0].artwork->dirty());CHECK(plan.libraries[2].duplicate_of==1&&!plan.libraries[2].enabled&&!plan.libraries[2].artwork&&!plan.libraries[2].error.empty());CHECK(plan.libraries[3].enabled&&!plan.libraries[3].artwork);
    CHECK(!validate_lod(plan,included,error)&&included.empty());plan.libraries[3].enabled=false;CHECK(validate_lod(plan,included,error)&&included==std::vector<size_t>({0,1}));CHECK(a->dirty()&&a->generation()==generation&&a->sprite(0)->anix==30&&g_load2_ppp==saved_ppp&&g_doc==nullptr&&bytes(af)==ab&&bytes(bf)==bb);
    auto bad=bb;bad.back()^=1;{std::ofstream f(bf,std::ios::binary);f.write(bad.data(),bad.size());}CHECK(!validate_lod(plan,included,error)&&included.empty());{std::ofstream f(bf,std::ios::binary);f.write(bb.data(),bb.size());}CHECK(validate_lod(plan,included,error));text(manifest,"assets/B.IMG\n");CHECK(!validate_lod(plan,included,error)&&included.empty());
    CHECK(review("assets/B.IMG\n"));CHECK(!plan.libraries[0].explicit_ppp&&plan.libraries[0].ppp==0&&validate_lod(plan,included,error));plan.libraries[0].enabled=false;CHECK(!validate_lod(plan,included,error));plan.libraries[0].enabled=true;plan.libraries[0].artwork.reset();CHECK(!validate_lod(plan,included,error));
    CHECK(review(u8"other\\演员 A.IMG\n",(root/"assets").u8string()));CHECK(plan.libraries[0].path==af.u8string()&&plan.libraries[0].resolution=="Override folder"&&validate_lod(plan,included,error));CHECK(!review("B.IMG\n",(root/"not-a-folder").u8string())&&plan.libraries.empty());
    CHECK(review("B.IMG\n",{},(root/"assets").u8string()));CHECK(plan.libraries[0].path==bf.u8string()&&plan.libraries[0].resolution=="IMGDIR"&&validate_lod(plan,included,error));fs::copy_file(bf,root/"B.IMG");CHECK(review("B.IMG\n",{},(root/"assets").u8string()));CHECK(plan.libraries[0].path==(root/"B.IMG").u8string()&&plan.libraries[0].resolution=="Manifest folder");
    // As-written fallback is deterministic, with a separate working directory.
    auto cwd=fs::current_path();fs::current_path(root/"assets");CHECK(review("B.IMG\n"));CHECK(plan.libraries[0].resolution=="Manifest folder");fs::remove(root/"B.IMG");CHECK(review("B.IMG\n"));CHECK(plan.libraries[0].resolution=="As written"&&validate_lod(plan,included,error));fs::current_path(cwd);
    CHECK(review("\""+bf.u8string()+"\"\n"));CHECK(plan.libraries[0].resolution=="As written"&&validate_lod(plan,included,error));
    CHECK(review("/ Classic comment\n/* Banner\n// Comment\nassets/B.IMG\n"));CHECK(validate_lod(plan,included,error));
    fs::copy_file(bf,root/"assets"/"semi;colon.IMG");CHECK(review("\"assets/semi;colon.IMG\" ; trailing comment\n"));CHECK(validate_lod(plan,included,error));
    text(root/"broken.IMG","not an IMG");CHECK(review("assets/B.IMG\nbroken.IMG\n"));CHECK(plan.libraries[0].artwork&&!plan.libraries[1].artwork&&!plan.libraries[1].error.empty()&&!validate_lod(plan,included,error)&&included.empty());plan.libraries[1].enabled=false;CHECK(validate_lod(plan,included,error));
    CHECK(review("PPP> 1\nassets/B.IMG\nPPP> 8\n\"assets/semi;colon.IMG\"\n"));CHECK(plan.libraries[0].ppp==1&&plan.libraries[1].ppp==8);CHECK(validate_lod(plan,included,error));
    for(auto invalid:{"PPP> -1\nassets/B.IMG\n","PPP> 9\nassets/B.IMG\n","PPP> word\nassets/B.IMG\n","PPP> 2.5\nassets/B.IMG\n","PPP> 2 extra\nassets/B.IMG\n","FUTURE> thing\nassets/B.IMG\n","\"assets/B.IMG\n","notes.txt\n","; empty\n"})CHECK(!review(invalid)&&plan.libraries.empty());
    CHECK(!review(std::string(8193,'a')+".IMG\n"));std::string many;for(int i=0;i<129;++i)many+="assets/B.IMG\n";CHECK(!review(many));CHECK(!review(std::string(1024*1024+1,';')));CHECK(!review(std::string("a\0b",3)));CHECK(!review(std::string("\xff\xfe",2)+"B.IMG"));
    CHECK(!review_lod((root/"missing.LOD").u8string(),{},{},plan,error)&&plan.libraries.empty());
    for(int i=1;i<argc;++i){CHECK(review_lod(argv[i],{},{},plan,error));if(plan.libraries.empty())continue;int ready=0;for(auto &r:plan.libraries){r.enabled=bool(r.artwork);ready+=r.enabled;}if(ready)CHECK(validate_lod(plan,included,error));}
    CHECK(bytes(af)==ab&&bytes(bf)==bb&&a->generation()==generation&&g_load2_ppp==saved_ppp&&g_doc==nullptr);plan={};fs::remove_all(root);return failures?1:0;
}
