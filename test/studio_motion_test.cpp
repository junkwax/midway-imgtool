#include "studio_project.h"
#include "stb_image.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <climits>
#include <iostream>
#include <sstream>

static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(0)
static std::string bytes(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void write(const std::filesystem::path &p,const std::string &s){std::ofstream f(p,std::ios::binary);f<<s;}
int main(){
    using namespace studio;namespace fs=std::filesystem;
    SceneState s;s.origin_x=100;s.origin_y=200;s.assets.push_back({"actor.IMG",{},{},1});
    SceneFrame a;a.name="A";a.pieces.push_back({0,0,{},{},1});a.x=5;a.y=-5;a.z=2;
    a.visible_from=2;a.visible_until=7;a.vx=3;a.vy=-2;a.cap_x=7;a.cap_y=3;a.dual=true;a.dual_x=40;a.dual_z=-1;
    SceneFrame b=a;b.name="B";b.pieces[0].runtime_id=2;b.x=20;b.y=10;b.z=0;b.visible_from=4;b.visible_until=9;b.vx=-1;b.vy=0;b.dual=false;
    SceneLane lane;lane.delay=3;lane.frames={a,b};s.lanes={lane};
    CHECK(scene_duration(s)==12);CHECK(scene_frame_seek_tick(lane,0)==5);CHECK(scene_frame_seek_tick(lane,1)==7);
    CHECK(evaluate_scene(s,-1).empty()&&evaluate_scene(s,2).empty()&&evaluate_scene(s,4).empty());
    auto poses=evaluate_scene(s,5);CHECK(poses.size()==2);
    if(poses.size()==2){CHECK(poses[0].dual&&poses[0].x==140&&poses[0].y==200);CHECK(!poses[1].dual&&poses[1].x==105&&poses[1].y==195);}
    poses=evaluate_scene(s,7);CHECK(poses.size()==3);
    if(poses.size()==3){CHECK(poses[0].dual&&poses[0].x==146&&poses[0].y==197);CHECK(poses[1].frame==1&&poses[1].x==120&&poses[1].y==210);CHECK(poses[2].frame==0&&poses[2].x==111&&poses[2].y==192);}
    poses=evaluate_scene(s,8);CHECK(poses.size()==3);if(poses.size()==3)CHECK(poses[2].x==112&&poses[2].y==192&&poses[1].x==119);
    poses=evaluate_scene(s,10);CHECK(poses.size()==1&&poses[0].frame==1&&poses[0].x==117);
    CHECK(evaluate_scene(s,12).empty()); // Hide is exclusive, including at the scene's end.
    s.loop=false;CHECK(scene_last_tick(s)==12);s.loop=true;CHECK(scene_last_tick(s)==11);
    s.lanes[0].stop_tick=5;CHECK(scene_frame_at(s.lanes[0],1000)==1);
    poses=evaluate_scene(s,1000);CHECK(poses.size()==3);if(poses.size()==3)CHECK(poses[1].x==119&&poses[2].x==112&&poses[2].y==192);
    // Freeze is per actor; another actor's clock keeps advancing.
    SceneLane other;other.frames={b};other.frames[0].visible_from=0;other.frames[0].visible_until=0;other.frames[0].vx=1;other.frames[0].cap_x=0;s.lanes.push_back(other);
    poses=evaluate_scene(s,20);CHECK(poses.size()==4);bool moving=false;for(auto &p:poses)if(p.lane==1)moving=p.x==140;CHECK(moving);
    s.lanes.resize(1);s.lanes[0].stop_tick=0;
    for(auto &f:s.lanes[0].frames){f.z=0;f.dual_z=0;}
    poses=evaluate_scene(s,7);CHECK(poses.size()==3);if(poses.size()==3)CHECK(poses[0].frame==0&&!poses[0].dual&&poses[1].frame==0&&poses[1].dual&&poses[2].frame==1);
    s.lanes[0].frames[0].ticks=10;poses=evaluate_scene(s,7);CHECK(poses.size()==3);if(poses.size()==3)CHECK(poses[0].frame==1&&poses[2].frame==0); // Current paints last at equal Z.
    // Clearing a schedule restores sequential playback; motion accumulates across actor loops.
    s.lanes[0].frames[0]=a;s.lanes[0].frames[0].visible_from=s.lanes[0].frames[0].visible_until=0;
    poses=evaluate_scene(s,3);CHECK(poses.size()==2);if(poses.size()==2)CHECK(poses.back().x==105);
    poses=evaluate_scene(s,11);CHECK(poses.size()==3);bool accumulated=false;for(auto &p:poses)if(p.frame==0&&!p.dual)accumulated=p.x==112;CHECK(accumulated);
    // Large caller ticks cannot overflow travel. Signed anipoint bounds invert under facing.
    auto &f=s.lanes[0].frames[0];f.visible_from=1;f.visible_until=0;f.cap_x=0;f.vx=128;f.dual=false;
    poses=evaluate_scene(s,INT_MAX);CHECK(poses.size()==1&&poses[0].x==32868);f.flip_x=true;
    poses=evaluate_scene(s,INT_MAX);CHECK(poses.size()==1&&poses[0].x==32867);f.vx=-128;
    poses=evaluate_scene(s,INT_MAX);CHECK(poses.size()==1&&poses[0].x==-32668);
    std::string error;f.cap_x=-1;CHECK(!validate_scene(s,error));f.cap_x=0;f.visible_from=100000;CHECK(!validate_scene(s,error));f.visible_from=1;

    auto root=fs::temp_directory_path()/("studio-motion-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto art=Artwork::demo();auto source=root/"actor.IMG";CHECK(art->save(source.u8string(),error));
    SceneState render;render.width=128;render.height=128;render.origin_x=64;render.origin_y=70;render.floor=112;render.loop=false;
    render.assets.push_back({source.u8string(),source.u8string(),{},10});SceneFrame frame;frame.name="Moving actor";frame.ticks=10;frame.pieces={scene_sprite_reference(*art,0,0)};
    frame.visible_until=3;frame.vx=3;frame.cap_x=5;frame.dual=true;frame.dual_x=-20;frame.dual_z=-1;SceneLane actor;actor.loop=false;actor.frames={frame};render.lanes={actor};
    SceneProject project(render);auto provider=[&](uint64_t uid)->Artwork *{return uid==10?art.get():nullptr;};auto file=root/"motion.ISTUDIO";
    CHECK(project.save(file.u8string(),provider,error));CHECK(bytes(file).find("version=3\n")!=std::string::npos);
    auto loaded=SceneProject::read(file.u8string(),error);CHECK(loaded!=nullptr);std::vector<SceneIssue> issues;
    if(loaded){CHECK(loaded->bind([&](int,const SceneAsset &){return SceneBinding{10,art.get()};},issues));CHECK(loaded->state.lanes[0].frames[0].vx==3&&loaded->state.lanes[0].frames[0].dual);
        CHECK(loaded->save(file.u8string(),provider,error));loaded->begin("Schedule");loaded->state.lanes[0].frames[0].visible_until=8;loaded->commit();CHECK(loaded->dirty());loaded->undo();CHECK(!loaded->dirty()&&loaded->state.lanes[0].frames[0].visible_until==3);loaded->redo();CHECK(loaded->dirty());}
    // Older native projects load with disabled motion and schedule defaults.
    auto old=bytes(file);auto at=old.find("version=3");old.replace(at,9,"version=1");std::istringstream lines(old);old.clear();std::string line;
    for(;std::getline(lines,line);){bool extra=false;for(const char *key:{".stop_tick=",".visible_from=",".visible_until=",".vx=",".vy=",".cap_x=",".cap_y=",".dual"})if(line.find(key)!=std::string::npos)extra=true;if(!extra)old+=line+'\n';}
    write(root/"old.ISTUDIO",old);auto v1=SceneProject::read((root/"old.ISTUDIO").u8string(),error);CHECK(v1&&v1->state.lanes[0].frames[0].vx==0&&!v1->state.lanes[0].frames[0].scheduled());
    auto png=root/"motion.png";CHECK(project.export_png(png.u8string(),2,provider,error));auto encoded=bytes(png);int w,h,c;auto rgba=stbi_load_from_memory((const unsigned char *)encoded.data(),(int)encoded.size(),&w,&h,&c,4);CHECK(rgba&&w==128&&h==128);
    if(rgba){unsigned char rgb[3];pal_word_to_rgb8((unsigned char *)art->palette(0)->data_p+9*2,rgb,rgb+1,rgb+2);for(int x:{44,64}){auto p=rgba+((size_t)21*128+x)*4;CHECK(p[0]==rgb[0]&&p[1]==rgb[1]&&p[2]==rgb[2]);}stbi_image_free(rgba);}
    CHECK(project.export_png(png.u8string(),3,provider,error));encoded=bytes(png);rgba=stbi_load_from_memory((const unsigned char *)encoded.data(),(int)encoded.size(),&w,&h,&c,4);CHECK(rgba!=nullptr);
    if(rgba){for(int i=0;i<w*h;++i)CHECK(rgba[i*4]==28&&rgba[i*4+1]==32&&rgba[i*4+2]==40);stbi_image_free(rgba);}
    // WAX velocity is applied before anipoint mirroring: facing reverses it.
    auto wax="format=imgtool_world_project\nversion=1\ndoc.count=1\ndoc.0.path="+source.u8string()+"\nslot.count=1\nslot.0.doc_idx=0\nslot.0.sequence_frames=0,1\nslot.0.frame_delays=2,5\nslot.0.local_dx=4,6\nslot.0.frame_mirror=0,3\nslot.0.motion_dx=3,-2\nslot.0.motion_dy=1,2\nslot.0.motion_cap_x=5,3\nslot.0.visible_from=2,4\nslot.0.visible_until=7,9\nslot.0.dual_on=1,0\nslot.0.dual_dx=40,0\nslot.0.dual_z=-3,0\nslot.0.stop_tick=5\nslot.0.hold_end=0\n";
    write(root/"moving.WAX",wax);auto imported=SceneProject::read((root/"moving.WAX").u8string(),error);CHECK(imported!=nullptr);
    if(imported){CHECK(!imported->state.loop&&imported->state.lanes[0].stop_tick==5);auto &frames=imported->state.lanes[0].frames;CHECK(frames[0].vx==3&&frames[1].vx==2&&frames[1].vy==-2);CHECK(frames[0].dual_x==-40);poses=evaluate_scene(imported->state,4);CHECK(poses.size()==3);if(poses.size()==3)CHECK(poses[0].dual&&poses[0].x==165);}
    auto independent=wax;at=independent.find("slot.0.doc_idx=0");independent.replace(at,16,"slot.0.doc_idx=-1");independent+="slot.0.frame_doc_idx=0,0\n";write(root/"independent.WAX",independent);
    auto cross_file=SceneProject::read((root/"independent.WAX").u8string(),error);CHECK(cross_file!=nullptr);if(cross_file)CHECK(cross_file->bind([&](int,const SceneAsset &){return SceneBinding{10,art.get()};},issues));
    write(root/"invalid.WAX",wax+"slot.0.spawn_slot=0\n");CHECK(!SceneProject::read((root/"invalid.WAX").u8string(),error));CHECK(error.find("spawn")!=std::string::npos);
    CHECK(fs::equivalent(root.parent_path(),fs::temp_directory_path()));fs::remove_all(root);
    std::cout<<"Studio motion checks: "<<failures<<" failure(s)\n";return failures?1:0;
}
