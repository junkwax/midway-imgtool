#include "studio_project.h"
#include "stb_image.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(0)
static std::string bytes(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void write(const std::filesystem::path &p,const std::string &s){std::ofstream f(p,std::ios::binary);f<<s;}
int main(int argc,char **argv){
    namespace fs=std::filesystem;std::string error;
    auto root=fs::temp_directory_path()/("studio-project-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto art=studio::Artwork::demo();auto reference=studio::Artwork::demo();
    auto source_file=root/fs::u8path(u8"actor \u65e5.IMG"),ref_file=root/"reference.IMG",project_file=root/"scene.ISTUDIO";
    CHECK(art->save(source_file.u8string(),error));CHECK(reference->save(ref_file.u8string(),error));
    studio::SceneState state;state.width=128;state.height=128;state.origin_x=64;state.origin_y=70;state.floor=112;
    state.assets={{source_file.u8string(),source_file.u8string(),{},101},{ref_file.u8string(),ref_file.u8string(),{},202}};
    studio::SceneLane lane;lane.name="Actor = \"test\"\nsecond line";
    lane.frames={{"first",2,0,0,0,false,false,{studio::scene_sprite_reference(*art,0,0)}},
                 {"second",5,3,-2,1,true,false,{studio::scene_sprite_reference(*art,0,1)}}};
    state.lanes.push_back(lane);auto other=lane;other.name="Reference";other.frames.resize(1);other.frames[0].pieces={studio::scene_sprite_reference(*reference,1,0)};other.x=300;other.loop=false;other.alpha=150;state.lanes.push_back(other);
    studio::SceneProject p(state);auto sources=[&](uint64_t uid){return uid==101?art.get():uid==202?reference.get():nullptr;};
    CHECK(p.dirty());CHECK(p.save(project_file.u8string(),sources,error));CHECK(!p.dirty());auto original=bytes(project_file);
    auto loaded=studio::SceneProject::read(project_file.u8string(),error);CHECK(loaded!=nullptr);
    std::vector<studio::SceneIssue> issues;
    if(loaded){CHECK(loaded->state.lanes[0].name==lane.name);CHECK(loaded->state.assets[0].path==source_file.filename().u8string());
        CHECK(loaded->bind([&](int a,const studio::SceneAsset &){return studio::SceneBinding{a?202ull:101ull,a?reference.get():art.get()};},issues));loaded->establish_loaded();CHECK(!loaded->dirty());
        CHECK(loaded->state.lanes[0].frames[0].pieces[0].runtime_id==art->sprite_id(0));CHECK(loaded->save(project_file.u8string(),sources,error));CHECK(bytes(project_file)==original);
    }
    p.begin("Change timing");p.state.lanes[0].frames[0].ticks=6;p.commit();CHECK(p.dirty());CHECK(p.can_undo());
    p.undo();CHECK(!p.dirty()&&p.state.lanes[0].frames[0].ticks==2);p.redo();CHECK(p.dirty()&&p.state.lanes[0].frames[0].ticks==6);
    p.begin("Drag lane");p.state.lanes[0].x=99;p.cancel();CHECK(p.state.lanes[0].x==0);
    p.undo();p.begin("New branch");p.state.floor=100;p.commit();CHECK(!p.can_redo());
    CHECK(!p.save((root/"missing"/"failed.ISTUDIO").u8string(),sources,error));CHECK(p.dirty());CHECK(bytes(project_file)==original);
    art->paint(0,0,0,8);art->commit();CHECK(!p.save(project_file.u8string(),sources,error));CHECK(error.find("IMG artwork")!=std::string::npos);CHECK(bytes(project_file)==original);
    art->undo();CHECK(!art->dirty());
    write(project_file,original+"# external change\n");CHECK(!p.save(project_file.u8string(),sources,error));CHECK(error.find("changed on disk")!=std::string::npos);
    auto second=root/"new.ISTUDIO";CHECK(p.save(second.u8string(),sources,error));CHECK(!p.dirty());CHECK(!p.save((root/"classic.WAX").u8string(),sources,error));
    // Only current dependencies are serialized; undo keeps its earlier sources.
    p.begin("Remove reference");p.state.lanes.erase(p.state.lanes.begin()+1);p.commit();
    reference->paint(0,0,0,8);reference->commit();CHECK(p.uses_doc(202));
    auto compact=root/"compact.ISTUDIO";CHECK(p.save(compact.u8string(),sources,error));
    auto compact_read=studio::SceneProject::read(compact.u8string(),error);CHECK(compact_read&&compact_read->state.assets.size()==1);
    p.undo();CHECK(p.state.lanes.size()==2&&p.dirty());p.redo();CHECK(p.state.lanes.size()==1&&!p.dirty());reference->undo();
    fs::remove(compact);CHECK(!p.save(compact.u8string(),sources,error));CHECK(error.find("changed on disk")!=std::string::npos);
    // An externally modified IMG must not be used to claim a reproducible save.
    auto source_bytes=bytes(source_file);write(source_file,source_bytes+"external");
    CHECK(!p.save((root/"stale.ISTUDIO").u8string(),sources,error));CHECK(error.find("IMG artwork")!=std::string::npos);write(source_file,source_bytes);
    // Saved frame identity survives sprite reordering and different runtime IDs.
    auto reordered=studio::Artwork::open(source_file.u8string(),error);CHECK(reordered!=nullptr);
    if(reordered){CHECK(reordered->move_sprite(0,5,error));CHECK(reordered->save(source_file.u8string(),error));loaded=studio::SceneProject::read(second.u8string(),error);CHECK(loaded!=nullptr);
        if(loaded){CHECK(loaded->bind([&](int a,const studio::SceneAsset &){return studio::SceneBinding{a?22ull:11ull,a?reference.get():reordered.get()};},issues));
            auto ref=loaded->state.lanes[0].frames[0].pieces[0];CHECK(ref.index==5&&ref.runtime_id==reordered->sprite_id(5));}
    }
    loaded=studio::SceneProject::read(second.u8string(),error);CHECK(loaded!=nullptr);if(loaded){CHECK(!loaded->bind([](int,const studio::SceneAsset &){return studio::SceneBinding{};},issues));CHECK(issues.size()==2);}
    // Same evaluation feeds the canvas and export, including timing, ordering and facing.
    CHECK(studio::scene_frame_at(lane,0)==0);CHECK(studio::scene_frame_at(lane,2)==1);CHECK(studio::scene_frame_at(lane,7)==0);
    lane.loop=false;lane.hold_end=false;CHECK(studio::scene_frame_at(lane,7)==-1);lane.hold_end=true;CHECK(studio::scene_frame_at(lane,7)==1);
    auto poses=studio::evaluate_scene(state,2);CHECK(poses.size()==2&&poses.back().frame==1&&poses.back().flip_x&&poses.back().x==67);
    studio::SceneProject render(state);auto png=root/"scene.png";CHECK(render.export_png(png.u8string(),0,sources,error));
    auto image_bytes=bytes(png);int w,h,c;auto rgba=stbi_load_from_memory((const unsigned char *)image_bytes.data(),(int)image_bytes.size(),&w,&h,&c,4);
    CHECK(rgba&&w==128&&h==128);if(rgba){unsigned char rgb[3];pal_word_to_rgb8((unsigned char *)art->palette(0)->data_p+9*2,rgb,rgb+1,rgb+2);auto at=rgba+((size_t)21*128+59)*4;CHECK(at[0]==rgb[0]&&at[1]==rgb[1]&&at[2]==rgb[2]&&at[3]==255);stbi_image_free(rgba);}
    // The known head pixel lands at a different coordinate in each mirror convention.
    for(int convention=0;convention<2;++convention){auto mirrored=state;mirrored.convention=(MirrorConvention)convention;mirrored.lanes[0].frames[0].flip_x=true;mirrored.lanes[0].frames[0].flip_y=true;
        studio::SceneProject flipped(mirrored);CHECK(flipped.export_png(png.u8string(),0,sources,error));auto encoded=bytes(png);
        rgba=stbi_load_from_memory((const unsigned char *)encoded.data(),(int)encoded.size(),&w,&h,&c,4);CHECK(rgba!=nullptr);
        if(rgba){unsigned char rgb[3];pal_word_to_rgb8((unsigned char *)art->palette(0)->data_p+9*2,rgb,rgb+1,rgb+2);auto at=rgba+((size_t)(118+convention)*128+68+convention)*4;CHECK(at[0]==rgb[0]&&at[1]==rgb[1]&&at[2]==rgb[2]);stbi_image_free(rgba);}}
    // Basic classic rows import; active spawn/embedded rules are explicit failures.
    std::string wax="format=imgtool_world_project\nversion=1\ndoc.count=1\ndoc.0.path="+ref_file.u8string()+"\nslot.count=1\nslot.0.doc_idx=0\nslot.0.sequence_frames=0,1\nslot.0.frame_delays=2,5\nslot.0.local_dx=4,6\nslot.0.frame_mirror=0,3\n";
    write(root/"basic.WAX",wax);auto classic=studio::SceneProject::read((root/"basic.WAX").u8string(),error);CHECK(classic&&classic->imported_wax());
    if(classic){CHECK(classic->bind([&](int,const studio::SceneAsset &){return studio::SceneBinding{202,reference.get()};},issues));classic->establish_loaded();CHECK(classic->dirty());CHECK(classic->state.lanes[0].frames[1].flip_y);CHECK(classic->state.lanes[0].frames[0].x==-4&&classic->state.lanes[0].frames[1].x==6);CHECK(classic->save((root/"converted.ISTUDIO").u8string(),sources,error));}
    write(root/"settings.WAX",wax+"slot.0.auto_life=30\nslot.0.auto_step=3\nslot.0.chain_count=3\n");CHECK(studio::SceneProject::read((root/"settings.WAX").u8string(),error)!=nullptr);
    std::string layered=wax;auto count_pos=layered.find("slot.count=1");layered.replace(count_pos,12,"slot.count=2");
    layered+="slot.0.order=0\nslot.1.order=1\nslot.1.doc_idx=0\nslot.1.sequence_frames=2\nslot.1.frame_delays=3\nslot.1.local_dx=7\nslot.1.local_dy=9\nslot.1.mirror=1\nworld.bg_solid=1\nworld.bg_solid_r=23\nworld.bg_solid_g=45\nworld.bg_solid_b=67\n";
    write(root/"layered.WAX",layered);auto layers=studio::SceneProject::read((root/"layered.WAX").u8string(),error);
    CHECK(layers&&layers->state.lanes.size()==2);if(layers){CHECK(layers->state.lanes[0].frames[0].pieces[0].index==2&&layers->state.lanes[0].alpha==185);CHECK(layers->state.lanes[1].alpha==255);CHECK(layers->state.lanes[0].frames[0].x==7&&layers->state.lanes[0].frames[0].y==-9);CHECK(layers->state.background[1]==45);}
    write(root/"unsupported.WAX",wax+"slot.0.spawn_slot=0\n");CHECK(!studio::SceneProject::read((root/"unsupported.WAX").u8string(),error));CHECK(error.find("spawn")!=std::string::npos);
    write(root/"embedded.WAX",wax+"state.embedded_active=1\n");CHECK(!studio::SceneProject::read((root/"embedded.WAX").u8string(),error));CHECK(error.find("embedded")!=std::string::npos);
    write(root/"bad.ISTUDIO","format=imgtool_studio_project\nversion=1\nasset.count=999999999\n");CHECK(!studio::SceneProject::read((root/"bad.ISTUDIO").u8string(),error));
    write(root/"bad.ISTUDIO","format=imgtool_studio_project\nversion=1\nversion=1\n");CHECK(!studio::SceneProject::read((root/"bad.ISTUDIO").u8string(),error));
    for(int arg=1;arg<argc;++arg){auto inspect=studio::SceneProject::read(argv[arg],error);std::cout<<"WAX: "<<argv[arg]<<" -> "<<(inspect?"supported rows":error)<<"\n";
        if(!inspect)continue;std::vector<std::unique_ptr<studio::Artwork>> dependencies(inspect->state.assets.size());
        bool available=true;for(int a=0;a<(int)dependencies.size();++a){auto candidate=studio::scene_asset_candidate(inspect->state.assets[a],argv[arg]);if(candidate.empty()){available=false;break;}dependencies[a]=studio::Artwork::open(candidate,error);if(!dependencies[a]){available=false;break;}}
        if(!available){std::cout<<"  unavailable fixture dependency\n";continue;}
        bool bound=inspect->bind([&](int a,const studio::SceneAsset &){return studio::SceneBinding{(uint64_t)100+a,dependencies[a].get()};},issues);CHECK(bound);if(!bound)continue;
        auto find_source=[&](uint64_t uid)->studio::Artwork *{return uid>=100&&uid<100+dependencies.size()?dependencies[uid-100].get():nullptr;};
        auto output=root/"real.ISTUDIO";CHECK(inspect->save(output.u8string(),find_source,error));auto reopened=studio::SceneProject::read(output.u8string(),error);CHECK(reopened!=nullptr);
        if(reopened){CHECK(reopened->bind([&](int a,const studio::SceneAsset &){return studio::SceneBinding{(uint64_t)100+a,dependencies[a].get()};},issues));CHECK(studio::scene_duration(reopened->state)==studio::scene_duration(inspect->state));}
        CHECK(inspect->export_png((root/"real.png").u8string(),0,find_source,error));
    }
    bool safe_cleanup=fs::equivalent(root.parent_path(),fs::temp_directory_path());CHECK(safe_cleanup);if(safe_cleanup)fs::remove_all(root);CHECK(g_doc==nullptr);
    std::cout<<"Studio project checks: "<<failures<<" failure(s)\n";return failures?1:0;
}
