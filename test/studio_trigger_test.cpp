#include "studio_project.h"
#include "stb_image.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
#include <sstream>

static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(0)
static std::string bytes(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void write(const std::filesystem::path &p,const std::string &s){std::ofstream f(p,std::ios::binary);f<<s;}
static void field(std::string &s,const std::string &key,const std::string &value){auto start=s.find(key+'=');CHECK(start!=std::string::npos);if(start!=std::string::npos){auto end=s.find('\n',start);s.replace(start,end-start,key+'='+value);}}
int main(){
    using namespace studio;namespace fs=std::filesystem;std::string error;
    auto root=fs::temp_directory_path()/("studio-trigger-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto art=Artwork::demo();auto source=root/"actor.IMG";CHECK(art->save(source.u8string(),error));
    auto provider=[&](uint64_t uid)->Artwork *{return uid==10?art.get():nullptr;};
    SceneState initial;initial.width=128;initial.height=128;initial.origin_x=64;initial.origin_y=70;initial.floor=112;initial.loop=false;initial.assets.push_back({source.u8string(),source.u8string(),{},10});
    SceneFrame idle;idle.name="Windup";idle.pieces={scene_sprite_reference(*art,0,0)};SceneFrame hit=idle;hit.name="Hit";
    SceneLane actor;actor.name="Attack";actor.x=300;actor.delay=3;actor.loop=false;actor.frames={idle,hit};
    SceneFrame first=idle;first.name="Blood 1";first.ticks=2;first.visible_until=2;SceneFrame next=first;next.name="Blood 2";next.visible_from=2;next.visible_until=4;
    SceneLane effect;effect.name="Blood";effect.delay=1;effect.loop=false;effect.hold_end=false;effect.frames={first,next};initial.lanes={actor,effect};
    SceneProject p(initial);auto &s=p.state;auto actor_id=s.lanes[0].id,hit_id=s.lanes[0].frames[1].id,effect_id=s.lanes[1].id;
    CHECK(actor_id&&hit_id&&effect_id&&actor_id!=effect_id);auto file=root/"effect.ISTUDIO";CHECK(p.save(file.u8string(),provider,error));
    p.begin("Follow hit");CHECK(scene_set_trigger(s,1,actor_id,hit_id,error));p.commit();CHECK(p.dirty());
    CHECK(scene_lane_start(s,1)==8&&scene_duration(s)==12);CHECK(scene_frame_seek_tick(s,1,1)==10);CHECK(scene_frame_at(s,1,7)==-1&&scene_frame_at(s,1,8)==0);
    bool early=false;for(auto &pose:evaluate_scene(s,7))early|=pose.lane==1;CHECK(!early);
    int count=0;for(auto &pose:evaluate_scene(s,8))if(pose.lane==1){++count;CHECK(pose.frame==0&&pose.x==64);}CHECK(count==1);
    p.undo();CHECK(!p.dirty()&&!s.lanes[1].trigger_lane&&scene_lane_start(s,1)==1);p.redo();CHECK(scene_lane_start(s,1)==8);
    CHECK(p.save(file.u8string(),provider,error));CHECK(bytes(file).find("version=3\n")!=std::string::npos);
    p.begin("Retime windup");s.lanes[0].frames[0].ticks=7;p.commit();CHECK(scene_lane_start(s,1)==11&&scene_duration(s)==15);
    CHECK(s.lanes[1].frames[0].visible_until==2&&s.lanes[1].frames[1].visible_from==2); // Derived timing does not rewrite schedules.
    p.undo();CHECK(scene_lane_start(s,1)==8&&!p.dirty());p.redo();CHECK(scene_lane_start(s,1)==11&&p.dirty());
    auto png=root/"effect.png";CHECK(p.export_png(png.u8string(),11,provider,error));auto encoded=bytes(png);int w,h,c;
    auto rgba=stbi_load_from_memory((const unsigned char *)encoded.data(),(int)encoded.size(),&w,&h,&c,4);CHECK(rgba&&w==128&&h==128);
    if(rgba){unsigned char rgb[3];pal_word_to_rgb8((unsigned char *)art->palette(0)->data_p+9*2,rgb,rgb+1,rgb+2);auto pixel=rgba+((size_t)21*128+59)*4;CHECK(pixel[0]==rgb[0]&&pixel[1]==rgb[1]&&pixel[2]==rgb[2]);stbi_image_free(rgba);}
    CHECK(p.export_png(png.u8string(),10,provider,error));encoded=bytes(png);rgba=stbi_load_from_memory((const unsigned char *)encoded.data(),(int)encoded.size(),&w,&h,&c,4);CHECK(rgba!=nullptr);
    if(rgba){auto pixel=rgba+((size_t)21*128+59)*4;CHECK(pixel[0]==28&&pixel[1]==32&&pixel[2]==40);stbi_image_free(rgba);}
    // Reordering frames and actor rows preserves the intended occurrence, not an array index.
    p.begin("Move hit");std::swap(s.lanes[0].frames[0],s.lanes[0].frames[1]);s.lanes[0].frames[0].name="Renamed hit";p.commit();CHECK(scene_lane_start(s,1)==4);
    CHECK(scene_uses_lane(s,actor_id)&&scene_uses_frame(s,hit_id));
    p.begin("Move actor rows");std::swap(s.lanes[0],s.lanes[1]);p.commit();CHECK(scene_lane_start(s,0)==4&&scene_lane_start(s,1)==3);
    SceneLane chain=s.lanes[0];chain.id=p.allocate_id();chain.name="Second effect";chain.x=300;chain.delay=2;
    for(auto &f:chain.frames)f.id=p.allocate_id();chain.trigger_lane=effect_id;chain.trigger_frame=s.lanes[0].frames[1].id;
    p.begin("Chain effect");s.lanes.push_back(chain);p.commit();CHECK(validate_scene(s,error));CHECK(scene_lane_start(s,2)==8);
    p.begin("Schedule source");s.lanes[1].frames[0].visible_from=8;s.lanes[1].frames[0].visible_until=12;s.lanes[1].visible=false;p.commit();
    CHECK(scene_lane_start(s,0)==12&&scene_lane_start(s,2)==16&&scene_duration(s)==20);
    // Timing follows a hidden source's authored frame time; it does not inherit visibility.
    count=0;for(auto &pose:evaluate_scene(s,12))if(pose.lane==0)++count;CHECK(count==1);
    CHECK(!scene_set_trigger(s,1,chain.id,chain.frames[0].id,error));CHECK(error.find("cycle")!=std::string::npos&&!s.lanes[1].trigger_lane);
    CHECK(!scene_set_trigger(s,0,actor_id,s.lanes[0].frames[0].id,error));CHECK(error.find("source frame")!=std::string::npos&&s.lanes[0].trigger_frame==hit_id);
    auto bad=s;bad.lanes[1].frames.erase(bad.lanes[1].frames.begin());CHECK(!validate_scene(bad,error));CHECK(error.find("source frame")!=std::string::npos);
    bad=s;bad.lanes[1].trigger_lane=chain.id;bad.lanes[1].trigger_frame=chain.frames[0].id;CHECK(!validate_scene(bad,error)&&evaluate_scene(bad,20).empty());
    CHECK(p.save(file.u8string(),provider,error));auto saved=bytes(file);std::vector<SceneIssue> issues;
    auto loaded=SceneProject::read(file.u8string(),error);CHECK(loaded!=nullptr);
    if(loaded){CHECK(loaded->bind([&](int,const SceneAsset &){return SceneBinding{10,art.get()};},issues));CHECK(scene_lane_start(loaded->state,0)==12&&scene_lane_start(loaded->state,2)==16);
        CHECK(loaded->state.lanes[0].trigger_frame==hit_id);CHECK(loaded->save(file.u8string(),provider,error));CHECK(bytes(file)==saved);}
    p.begin("Detach");CHECK(scene_detach_trigger(s,0,error));p.commit();CHECK(!s.lanes[0].trigger_lane&&s.lanes[0].delay==12&&scene_lane_start(s,2)==16);
    p.undo();CHECK(s.lanes[0].trigger_lane==actor_id&&scene_lane_start(s,0)==12&&!p.dirty());p.redo();CHECK(s.lanes[0].delay==12);
    p.begin("Move source timing");s.lanes[1].frames[0].visible_from=10;p.commit();CHECK(scene_lane_start(s,0)==12&&scene_lane_start(s,2)==16);
    // Allocate IDs monotonically, including after a history branch.
    auto unused=p.allocate_id();p.undo();auto later=p.allocate_id();CHECK(later>unused);
    auto corrupt=saved;field(corrupt,"lane.0.trigger_frame","999999");write(root/"missing.ISTUDIO",corrupt);CHECK(!SceneProject::read((root/"missing.ISTUDIO").u8string(),error));
    corrupt=saved;field(corrupt,"lane.0.frame.0.id",std::to_string(hit_id));write(root/"duplicate.ISTUDIO",corrupt);CHECK(!SceneProject::read((root/"duplicate.ISTUDIO").u8string(),error));CHECK(error.find("identity")!=std::string::npos);
    // Version 2 has no trigger identities; its independent actor clocks remain intact.
    corrupt=saved;field(corrupt,"version","2");std::istringstream lines(corrupt);corrupt.clear();std::string line;
    while(std::getline(lines,line))if(line.find(".id=")==std::string::npos&&line.find(".trigger_")==std::string::npos)corrupt+=line+'\n';
    write(root/"old.ISTUDIO",corrupt);auto old=SceneProject::read((root/"old.ISTUDIO").u8string(),error);CHECK(old&&old->state.lanes[0].id&&!old->state.lanes[0].trigger_lane&&scene_lane_start(old->state,0)==1);
    std::string wax="format=imgtool_world_project\nversion=1\ndoc.count=1\ndoc.0.path="+source.u8string()+"\nslot.count=2\nslot.0.doc_idx=0\nslot.0.sequence_frames=0,1\nslot.0.frame_delays=4,4\nslot.1.doc_idx=0\nslot.1.sequence_frames=0,1\nslot.1.frame_delays=2,2\nslot.1.hold=2\nslot.1.spawn_slot=0\nslot.1.spawn_entry=1\nslot.1.spawn_tick=4\nslot.1.visible_from=4,6\nslot.1.visible_until=6,8\n";
    write(root/"spawn.WAX",wax);auto imported=SceneProject::read((root/"spawn.WAX").u8string(),error);CHECK(imported!=nullptr);
    if(imported){CHECK(imported->bind([&](int,const SceneAsset &){return SceneBinding{10,art.get()};},issues));auto &is=imported->state;CHECK(scene_lane_start(is,0)==4&&is.lanes[0].frames[0].visible_from==0&&is.lanes[0].frames[1].visible_from==2);
        is.lanes[1].frames[0].ticks=7;CHECK(scene_lane_start(is,0)==7);CHECK(imported->save((root/"converted.ISTUDIO").u8string(),provider,error));}
    corrupt=wax;field(corrupt,"slot.1.spawn_tick","5");write(root/"moved.WAX",corrupt);CHECK(!SceneProject::read((root/"moved.WAX").u8string(),error));CHECK(error.find("moved by hand")!=std::string::npos);
    corrupt=wax;field(corrupt,"slot.1.visible_from","4,7");write(root/"custom.WAX",corrupt);CHECK(!SceneProject::read((root/"custom.WAX").u8string(),error));CHECK(error.find("custom schedule")!=std::string::npos);
    corrupt=wax;field(corrupt,"slot.1.spawn_slot","1");write(root/"cycle.WAX",corrupt);CHECK(!SceneProject::read((root/"cycle.WAX").u8string(),error));CHECK(error.find("cycle")!=std::string::npos);
    CHECK(!art->dirty());bool safe=fs::equivalent(root.parent_path(),fs::temp_directory_path());CHECK(safe);if(safe)fs::remove_all(root);
    std::cout<<"Studio trigger checks: "<<failures<<" failure(s)\n";return failures?1:0;
}
