#include "studio_sequences.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
#include <new>
static int failures=0;static long fail_after=-1;
void *operator new(size_t n){if(fail_after==0){fail_after=-1;throw std::bad_alloc();}if(fail_after>0)--fail_after;if(auto p=malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void *p) noexcept{free(p);}void operator delete(void *p,size_t) noexcept{free(p);}
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x " / "<<error<<"\n";++failures;}}while(0)
static std::string bytes(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void write(const std::filesystem::path &p,const std::string &s){std::ofstream f(p,std::ios::binary);f<<s;}
static void set(std::string &text,const std::string &key,const std::string &value){auto at=text.find(key+"=");if(at!=std::string::npos)text.erase(at,text.find('\n',at)-at+1);text+=key+"="+value+"\n";}
static int embedded(const studio::SceneState &s){for(int i=0;i<(int)s.lanes.size();++i)if(s.lanes[i].name.find("Sequence:")==0)return i;return -1;}
static std::string wax(const std::filesystem::path &a,const std::filesystem::path &b,int boundary){auto slot="slot."+std::to_string(boundary+3)+".";
    return "format=imgtool_world_project\nversion=1\ndoc.count=2\ndoc.0.path="+a.u8string()+"\ndoc.1.path="+b.u8string()+"\nslot.count="+std::to_string(boundary+4)+"\nslot.source_tabs="+std::to_string(boundary)+"\nworld.origin_y=220\nstate.embedded_active=1\nstate.embedded_is_script=0\nstate.embedded_record_index=0\nstate.embedded_doc_idx=1\nstate.embedded_doc_path="+b.u8string()+"\nstate.embedded_name=WALK\nstate.embedded_label_count=2\nembedded_label.0.text=Saved \\\"pose\\\"\nembedded_label.1.text=\nslot.0.doc_idx=0\nslot.0.sequence_frames=3\n"+
        slot+"doc_idx=0\n"+slot+"sequence_frames=0,2,1\n"+slot+"default_frames=7\n"+slot+"frame_doc_idx=-1,0,-1\n"+slot+"frame_delays=6,255,3\n"+slot+"local_dx=4,-6,8\n"+slot+"local_dy=-3,2,-1\n"+slot+"frame_mirror=0,3,1\n"+slot+"mirror=1\n"+slot+"zoom=3\n"+slot+"frame_z=2,1,0\n"+slot+"hold_end=0\n"+slot+"motion_dx=2,-3,1\n"+slot+"motion_dy=1,2,-1\n"+slot+"motion_cap_x=10,20,0\n"+slot+"dual_on=1\n"+slot+"dual_dx=9\n"+slot+"dual_dy=4\n";
}
int main(){using namespace studio;namespace fs=std::filesystem;std::string error;auto root=fs::temp_directory_path()/("studio-wax-embedded-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto a=Artwork::demo(),b=Artwork::demo();CHECK(b->embedded_add_record(false,"WALK",error)==0&&b->embedded_append(0,7,error)); // The saved WAX row, not the current IMG record, is the snapshot.
    auto ap=root/"a.IMG",bp=root/fs::u8path(u8"source \u65e5.IMG");CHECK(a->save(ap.u8string(),error)&&b->save(bp.u8string(),error));auto ab=bytes(ap),bb=bytes(bp);auto ag=a->generation(),bg=b->generation();std::vector<SceneIssue> issues;
    auto sources=[&](uint64_t uid){return uid==101?a.get():uid==202?b.get():nullptr;};
    auto binder=[&](int,const SceneAsset &asset){return asset.path.find("a.IMG")!=std::string::npos?SceneBinding{101,a.get()}:SceneBinding{202,b.get()};};
    for(int boundary:{10,20}){auto text=wax(ap,bp,boundary);if(boundary==10){auto pos=text.find("slot.source_tabs=");text.erase(pos,text.find('\n',pos)-pos+1);}auto path=root/"scene.WAX";write(path,text);auto p=SceneProject::read(path.u8string(),error);CHECK(p&&p->state.lanes.size()==2&&p->imported_wax());if(!p)continue;int lane=embedded(p->state);CHECK(lane>=0);if(lane<0)continue;
        auto &l=p->state.lanes[lane];CHECK(l.name=="Sequence: WALK"&&l.frames.size()==3&&l.magnification==3&&l.flip_x&&!l.hold_end&&p->state.loop&&l.frames[1].ticks==255&&l.frames[0].x==4&&l.frames[0].y==3&&l.frames[1].x==6&&l.frames[1].y==2&&l.frames[0].vx==-2&&l.frames[1].vy==-2&&l.frames[0].dual_x==9&&l.frames[0].dual_y==-4);
        auto id=l.id,fid=l.frames[1].id;CHECK(p->bind(binder,issues));auto label=p->state.lanes[lane].frames[0].name;
        CHECK(p->state.assets[p->state.lanes[lane].frames[0].pieces[0].asset].runtime_doc==202&&p->state.assets[p->state.lanes[lane].frames[1].pieces[0].asset].runtime_doc==101&&p->state.lanes[lane].frames[1].name==a->sprite(2)->n_s&&label.find("Saved")==0&&p->bind(binder,issues)&&p->state.lanes[lane].frames[0].name==label);
        p->establish_loaded();CHECK(p->dirty()&&!p->can_undo());auto output=root/"scene.ISTUDIO";CHECK(p->save(output.u8string(),sources,error)&&p->export_png((root/"preview.png").u8string(),6,sources,error));auto saved=bytes(output);auto loaded=SceneProject::read(output.u8string(),error);CHECK(loaded&&loaded->bind(binder,issues));if(loaded){CHECK(loaded->state.lanes[lane].id==id&&loaded->state.lanes[lane].frames[1].id==fid&&loaded->state.lanes[lane].frames[0].name==label&&loaded->save(output.u8string(),sources,error)&&bytes(output)==saved);loaded->begin("Retime imported sequence");loaded->state.lanes[lane].frames[0].ticks=9;loaded->commit();CHECK(loaded->can_undo());loaded->undo();CHECK(loaded->state.lanes[lane].frames[0].ticks==6&&!loaded->dirty());loaded->redo();CHECK(loaded->state.lanes[lane].frames[0].ticks==9);}
        CHECK(bytes(path)==text);
    }
    auto text=wax(ap,bp,20);auto load=[&](const std::string &value){write(root/"case.WAX",value);return SceneProject::read((root/"case.WAX").u8string(),error);};
    // Root document/path determine the lane source; saved overrides still work.
    auto path_only=text;set(path_only,"state.embedded_doc_idx","-1");auto p=load(path_only);CHECK(p&&p->bind(binder,issues));
    // The embedded row can be the only actor, with inactive reserved slots ignored.
    auto only=text;set(only,"slot.0.sequence_frames","");only+="slot.21.sequence_frames=7\nslot.22.sequence_frames=7\n";p=load(only);CHECK(p&&p->state.lanes.size()==1&&p->bind(binder,issues));
    // Uniform effect timing follows the saved embedded frame identity.
    auto effect=text;effect+="slot.1.doc_idx=0\nslot.1.sequence_frames=3\nslot.1.frame_delays=4\nslot.1.hold=4\nslot.1.spawn_slot=23\nslot.1.spawn_entry=1\nslot.1.spawn_tick=6\nslot.1.visible_from=6\nslot.1.visible_until=10\n";p=load(effect);CHECK(p&&p->bind(binder,issues));if(p){int follower=-1,lane=embedded(p->state);for(int i=0;i<(int)p->state.lanes.size();++i)if(p->state.lanes[i].trigger_lane)follower=i;CHECK(follower>=0&&scene_lane_start(p->state,follower)==6);p->begin("Retime");p->state.lanes[lane].frames[0].ticks=10;p->commit();CHECK(scene_lane_start(p->state,follower)==10);p->undo();CHECK(scene_lane_start(p->state,follower)==6);}
    // Default-only rows saved by older builds are supported, but an explicitly
    // empty sequence never resurrects default frames or guesses from the IMG.
    auto defaults=text;auto pos=defaults.find("slot.23.sequence_frames=");defaults.erase(pos,defaults.find('\n',pos)-pos+1);p=load(defaults);CHECK(p&&p->state.lanes[embedded(p->state)].frames.size()==1);
    for(auto change:std::vector<std::pair<std::string,std::string>>{{"state.embedded_is_script","1"},{"asm.player.enabled","1"},{"state.embedded_record_index","-1"},{"slot.count","23"},{"slot.23.sequence_frames",""},{"slot.23.sequence_frames","-1"},{"slot.23.subframe_swap_tick","3"},{"slot.23.subframe_fine_source","0"},{"state.embedded_label_count","4097"},{"embedded_label.0.text",std::string(129,'X')}}){auto bad=text;set(bad,change.first,change.second);CHECK(!load(bad));}
    auto no_source=text;set(no_source,"state.embedded_doc_idx","-1");set(no_source,"state.embedded_doc_path","");CHECK(!load(no_source));
    auto stopped=text;set(stopped,"slot.23.stop_tick","5");p=load(stopped);CHECK(p&&!p->state.loop); // Other row holds at its end.
    auto invisible=text;set(invisible,"slot.23.visible","0");p=load(invisible);CHECK(p&&!p->state.loop);
    // All binding dependencies remain unbound on failure; retry is atomic.
    auto invalid=text;set(invalid,"slot.23.sequence_frames","0,1999");p=load(invalid);CHECK(p&&!p->bind(binder,issues)&&!issues.empty());if(p)for(auto &asset:p->state.assets)CHECK(!asset.runtime_doc);
    p=load(text);CHECK(p&&!p->bind([&](int index,const SceneAsset &asset){return index?SceneBinding{}:binder(index,asset);},issues));if(p){for(auto &asset:p->state.assets)CHECK(!asset.runtime_doc);CHECK(p->bind(binder,issues));}
    int rejected=0,succeeded=0;for(int n=0;n<500;++n){p=load(text);CHECK(p);bool ok=false;fail_after=n;try{ok=p->bind(binder,issues);}catch(const std::bad_alloc &){}fail_after=-1;if(!ok){++rejected;for(auto &asset:p->state.assets)CHECK(!asset.runtime_doc);CHECK(!p->can_undo());}else ++succeeded;}CHECK(rejected>10&&succeeded>0);
    CHECK(a->generation()==ag&&b->generation()==bg&&!a->dirty()&&!b->dirty()&&bytes(ap)==ab&&bytes(bp)==bb&&g_doc==nullptr);fs::remove_all(root);std::cout<<"WAX embedded import: "<<failures<<" failures\n";return failures?1:0;
}
