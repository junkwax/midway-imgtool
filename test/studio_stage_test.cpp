#include "studio_stage.h"
#include "studio_project.h"
#include "bdd_bg.h"
#include "stb_image.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <climits>
SDL_Renderer *g_imgui_renderer=nullptr;
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(0)
static void write(const std::filesystem::path &p,const std::string &s){std::ofstream out(p,std::ios::binary);out.write(s.data(),s.size());}
static std::string read(const std::filesystem::path &p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
static std::string pixels(){std::string out="1\n1 2 2 0\n";out.append("\0\1\2\3",4);out+="PAL 4\n";for(unsigned c:{0x7fffu,0x7c00u,0x03e0u,0x001fu}){out+=(char)c;out+=(char)(c>>8);}return out;}
int main(int argc,char **argv){
    namespace fs=std::filesystem;using namespace studio;std::string error;
    auto root=fs::temp_directory_path()/("studio-stage-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto bdd=root/"STAGE.BDD",bdb=root/"STAGE.BDB";auto art=pixels();std::string layout="TEST 64 64 255 2 1 2\nSKY 0 63 0 19\nFLOOR 0 63 20 63\n4000 10 20 1 0\n4010 12 20 1 0\n";write(bdd,art);write(bdb,layout);
    auto stage=read_stage(bdb.u8string(),error);CHECK(stage&&error.empty());
    if(stage){CHECK(stage->x==10&&stage->y==20&&stage->w==4&&stage->h==2&&stage->objects==2&&stage->skipped==0);
        std::vector<unsigned char> expected={0,0,0,0,255,0,0,255,255,0,0,255,0,0,0,0,0,255,0,255,0,0,255,255,0,0,255,255,0,255,0,255};CHECK(stage->rgba==expected);
        CHECK(stage_playfield_plane(*stage)==1&&stage->planes[1].objects==2&&stage->planes[1].playfield==2);
        StageView view;stage_align_plane(*stage,view,1,64,54);CHECK(view.plane==1&&view.camera_x==-21&&view.camera_y==-33);
        view.camera_x=10;view.camera_y=20;view.alpha=128;std::vector<unsigned char> rgba(4*2*4,20);for(size_t i=3;i<rgba.size();i+=4)rgba[i]=255;
        CHECK(blit_stage(*stage,view,4,2,rgba,error));CHECK(rgba[0]==20&&rgba[3]==255&&rgba[4]==138&&rgba[5]==10&&rgba[6]==10&&rgba[7]==255);
        auto frozen=rgba;view.enabled=false;CHECK(blit_stage(*stage,view,4,2,rgba,error)&&rgba==frozen);view.enabled=true;view.camera_x=INT_MIN;view.camera_y=INT_MAX;
        CHECK(blit_stage(*stage,view,4,2,rgba,error)&&rgba==frozen);rgba.pop_back();CHECK(!blit_stage(*stage,view,4,2,rgba,error));
        BddBackground classic;CHECK(BddBgLoad(bdd.u8string().c_str(),&classic));CHECK(classic.rgba==stage->rgba&&classic.content_x==stage->x&&classic.content_y==stage->y);BddBgFree(&classic);
        // Scene export includes the exact owned scenery without editing its IMG.
        auto doc=Artwork::demo();auto generation=doc->generation();auto format=doc->disk_version();SceneState s;s.width=64;s.height=64;s.origin_x=200;s.origin_y=200;s.floor=54;
        s.assets={{"", "", "",11}};SceneLane lane;lane.frames={{"Frame",4,0,0,0,false,false,{scene_sprite_reference(*doc,0,0)}}};s.lanes={lane};SceneProject project(s);
        view={};view.camera_x=10;view.camera_y=20;auto png=root/"stage.png";CHECK(project.export_png(png.u8string(),0,[&](uint64_t){return doc.get();},error,stage.get(),&view));
        auto bytes=read(png);int w=0,h=0,c=0;auto decoded=stbi_load_from_memory((const unsigned char *)bytes.data(),(int)bytes.size(),&w,&h,&c,4);
        CHECK(decoded&&w==64&&h==64);if(decoded){CHECK(decoded[4]==255&&decoded[5]==0&&decoded[6]==0&&decoded[7]==255&&decoded[0]==28);stbi_image_free(decoded);}
        CHECK(doc->generation()==generation&&!doc->dirty()&&doc->disk_version()==format&&read(bdd)==art&&read(bdb)==layout);
        // A hard-linked output must not replace either source half.
        std::error_code ec;auto alias=root/"alias.png";fs::create_hard_link(bdd,alias,ec);if(!ec){CHECK(!project.export_png(alias.u8string(),0,[&](uint64_t){return doc.get();},error,stage.get(),&view));CHECK(read(bdd)==art);}
        write(bdd,"broken");CHECK(stage->rgba==expected);CHECK(!read_stage(bdd.u8string(),error));write(bdd,art);
    }
    auto unicode=root/fs::u8path(u8"scenery \u65e5\u672c.bdd"),unicode_bdb=unicode;unicode_bdb.replace_extension(".bdb");write(unicode,art);write(unicode_bdb,layout);CHECK(read_stage(unicode.u8string(),error)!=nullptr);CHECK(read_stage(unicode_bdb.u8string(),error)!=nullptr);
    auto broken=[&](const std::string &a,const std::string &b){write(bdd,a);write(bdb,b);CHECK(!read_stage(bdd.u8string(),error)&&!error.empty());};
    broken(art.substr(0,art.size()-1),layout);broken("2\n"+art.substr(2),layout);broken("1\n1 9999999 2\n",layout);broken(art,"TEST 64 64 255 2 1 9\n"+layout.substr(layout.find('\n')+1));
    broken(art,"TEST 64 64 255 0 1 1\n4000 2147483647 20 1 0\n");broken(art,"TEST 64 64 255 0 1 2\n4000 -1000000 20 1 0\n4000 1000000 20 1 0\n");
    auto bad_color=art;bad_color[bad_color.find("0\n")+3]=(char)255;write(bdd,bad_color);write(bdb,layout);auto warned=read_stage(bdd.u8string(),error);CHECK(warned&&warned->invalid_pixels==2);
    write(bdd,art);write(bdb,layout);fs::remove(bdb);CHECK(!read_stage(bdd.u8string(),error));CHECK(!read_stage((root/"fake.img").u8string(),error));
    // Differential checks against the existing classic loader, read-only real pairs.
    auto fixtures=argc>1?fs::u8path(argv[1]):fs::path("../mk2-main/data");int real=0;
    for(auto name:{"DEDPOOL","FOREST","FOREST2","CEMETERY","ARMORY","PIT2","TOWER","DSRT1","HISCORE1"}){auto path=fixtures/(std::string(name)+".BDD");if(!fs::exists(path))continue;
        auto loaded=read_stage(path.u8string(),error);CHECK(loaded);if(!loaded){std::cerr<<name<<": "<<error<<'\n';continue;}BddBackground classic;
        CHECK(BddBgLoad(path.u8string().c_str(),&classic));CHECK(loaded->rgba==classic.rgba&&loaded->x==classic.content_x&&loaded->y==classic.content_y&&loaded->w==classic.w&&loaded->h==classic.h);BddBgFree(&classic);++real;}
    std::cout<<"Stage checks: "<<real<<" real pairs, "<<failures<<" failures\n";fs::remove_all(root);return failures?1:0;
}
