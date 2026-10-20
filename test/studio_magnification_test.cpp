#include "studio_project.h"
#include "studio_layers.h"
#include "anipoint_mirror.h"
#include "stb_image.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
#include <algorithm>
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x " / "<<error<<"\n";++failures;}}while(0)
static std::string bytes(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void write(const std::filesystem::path &p,const std::string &s){std::ofstream f(p,std::ios::binary);f<<s;}
int main(){using namespace studio;namespace fs=std::filesystem;std::string error;
    auto root=fs::temp_directory_path()/("studio-magnification-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto art=Artwork::create();CHECK(art->canvas_size(0,5,3,0,error)&&art->add_sprite("PIECE",5,3,0,error)==1);
    for(int i=0;i<2;++i){auto im=art->sprite(i);memset(im->data_p,0,24);im->anix=(unsigned short)(i?-2:2);im->aniy=(unsigned short)(i?-1:2);((unsigned char *)im->data_p)[9]=8;((unsigned char *)im->data_p)[20]=12;}
    auto layer=art->copy(0,1,1,1,1);layer.pixels={15};CHECK(art->paste_layer(0,layer,2,0,error));
    auto img=root/"actor.IMG";CHECK(art->save(img.u8string(),error));auto original=bytes(img);auto pixels=art->copy(0,0,0,5,3).pixels;auto generation=art->generation();
    SceneState base;base.width=base.height=128;base.floor=112;base.origin_x=40;base.origin_y=40;base.background[0]=7;base.background[1]=11;base.background[2]=19;base.assets={{img.u8string(),img.u8string(),{},101}};
    SceneLane lane;lane.x=3;lane.y=-2;lane.loop=false;SceneFrame frame;frame.name="Composite";frame.ticks=10;frame.x=-4;frame.y=-3;frame.pieces={scene_sprite_reference(*art,0,0),scene_sprite_reference(*art,0,1)};lane.frames={frame};base.lanes={lane};auto sources=[&](uint64_t uid){return uid==101?art.get():nullptr;};
    auto png=root/"scaled.png";
    // Independent pixel goldens: scale each original source pixel and anchor,
    // then clip each duplicated block. Covers both mirrors, multipart spacing,
    // alpha, visible pasted layers and negative off-canvas placement.
    for(int scale=1;scale<=4;++scale)for(int convention=0;convention<2;++convention)for(int bits=0;bits<4;++bits)for(int clipped=0;clipped<2;++clipped){auto s=base;s.convention=(MirrorConvention)convention;s.lanes[0].magnification=scale;s.lanes[0].frames[0].flip_x=bits&1;s.lanes[0].frames[0].flip_y=bits&2;s.lanes[0].alpha=clipped?128:255;if(clipped){s.origin_x=4;s.origin_y=3;}SceneProject project(s);CHECK(project.export_png(png.u8string(),0,sources,error));int w=0,h=0,c=0;auto rgba=stbi_load(png.u8string().c_str(),&w,&h,&c,4);CHECK(rgba&&w==128&&h==128);if(!rgba)continue;
        std::vector<unsigned char> expected(128*128*4);for(size_t i=0;i<expected.size();i+=4){expected[i]=7;expected[i+1]=11;expected[i+2]=19;expected[i+3]=255;}
        for(int piece=0;piece<2;++piece){auto im=art->sprite(piece);bool fx=bits&1,fy=bits&2;int ax=fx?(convention?im->w-1:im->w)-(short)im->anix:(short)im->anix;int ay=fy?(convention?im->h-1:im->h)-(short)im->aniy:(short)im->aniy;int left=s.origin_x+3+(-4-ax)*scale,top=s.origin_y-2+(-3-ay)*scale;
            auto poses=evaluate_scene(s,0);auto p=scene_pose_placement(*im,poses[piece],s.convention);CHECK(p.x==left&&p.y==top&&p.w==im->w*scale&&p.h==im->h*scale);
            for(int sy=0;sy<im->h;++sy)for(int sx=0;sx<im->w;++sx){int ci=visible_pixel(*im,sx,sy);if(!ci)continue;unsigned char rgb[3];pal_word_to_rgb8((unsigned char *)art->palette(0)->data_p+ci*2,rgb,rgb+1,rgb+2);for(int yy=0;yy<scale;++yy)for(int xx=0;xx<scale;++xx){int x=left+(fx?im->w-1-sx:sx)*scale+xx,y=top+(fy?im->h-1-sy:sy)*scale+yy;if(x<0||y<0||x>=128||y>=128)continue;auto dst=expected.data()+((size_t)y*128+x)*4;for(int channel=0;channel<3;++channel)dst[channel]=(unsigned char)((rgb[channel]*s.lanes[0].alpha+dst[channel]*(255-s.lanes[0].alpha)+127)/255);}}
        }CHECK(!memcmp(rgba,expected.data(),expected.size()));stbi_image_free(rgba);
    }
    // Motion/local offsets and dual placements scale; actor/world translations,
    // depth, timing and trigger starts do not. Freeze caps the same actor clock.
    auto moving=base;auto &actor=moving.lanes[0];actor.magnification=3;actor.x=9;actor.delay=2;actor.stop_tick=4;auto &f=actor.frames[0];f.vx=2;f.vy=-1;f.cap_x=5;f.dual=true;f.dual_x=6;f.dual_y=7;f.dual_z=-1;auto poses=evaluate_scene(moving,7);CHECK(poses.size()==4&&poses[0].dual&&poses[0].x==40+9+(6+5)*3&&poses[0].y==40-2+(7-4)*3&&poses[0].z==-1&&poses[2].x==40+9+(-4+5)*3);CHECK(scene_lane_start(moving,0)==2&&scene_frame_at(moving,0,7)==0&&scene_duration(moving)==12);
    SceneLane effect;effect.id=30;effect.frames=actor.frames;effect.frames[0].id=31;effect.magnification=4;actor.id=10;actor.frames[0].id=11;moving.lanes.push_back(effect);CHECK(scene_set_trigger(moving,1,10,11,error)&&scene_lane_start(moving,1)==2);CHECK(evaluate_scene(moving,1).empty());
    // Native saves use version 5 only when required; history and stable references
    // preserve scale independently of the IMG document and source bytes.
    SceneProject native(base);auto file=root/"native.ISTUDIO";CHECK(native.save(file.u8string(),sources,error));auto unscaled=bytes(file);CHECK(unscaled.find("version=3\n")!=std::string::npos&&unscaled.find("magnification=")==std::string::npos);
    native.begin("Scale actor");native.state.lanes[0].magnification=4;native.state.lanes[0].frames[0].ticks=255;native.commit();CHECK(native.dirty()&&native.can_undo()&&native.save(file.u8string(),sources,error));auto scaled=bytes(file);CHECK(scaled.find("version=5\n")!=std::string::npos&&scaled.find("magnification=4\n")!=std::string::npos);auto reopened=SceneProject::read(file.u8string(),error);std::vector<SceneIssue> issues;CHECK(reopened&&reopened->bind([&](int,const SceneAsset &){return SceneBinding{101,art.get()};},issues));if(reopened){reopened->establish_loaded();CHECK(reopened->state.lanes[0].magnification==4&&reopened->state.lanes[0].frames[0].ticks==255&&reopened->save(file.u8string(),sources,error)&&bytes(file)==scaled);}
    native.undo();CHECK(native.state.lanes[0].magnification==1&&native.can_redo());native.redo();CHECK(native.state.lanes[0].magnification==4);native.undo();CHECK(native.save(file.u8string(),sources,error)&&bytes(file)==unscaled);
    for(int invalid:{-1,0,5,2147483647}){native.state.lanes[0].magnification=invalid;CHECK(!validate_scene(native.state,error)&&!native.export_png(png.u8string(),0,sources,error)&&!native.save(file.u8string(),sources,error)&&bytes(file)==unscaled);}native.state.lanes[0].magnification=1;
    auto long_state=base;long_state.lanes[0].frames[0].ticks=255;SceneProject long_project(long_state);auto long_file=root/"long.ISTUDIO";CHECK(long_project.save(long_file.u8string(),sources,error));auto long_bytes=bytes(long_file);CHECK(long_bytes.find("version=4\n")!=std::string::npos&&long_bytes.find("magnification=")==std::string::npos);auto long_read=SceneProject::read(long_file.u8string(),error);CHECK(long_read&&long_read->bind([&](int,const SceneAsset &){return SceneBinding{101,art.get()};},issues));if(long_read)CHECK(long_read->save(long_file.u8string(),sources,error)&&bytes(long_file)==long_bytes);
    IMG edge=*art->sprite(0);edge.anix=(unsigned short)-32768;edge.aniy=32767;ScenePose edge_pose;edge_pose.x=1;edge_pose.y=2;edge_pose.flip_x=edge_pose.flip_y=true;edge_pose.magnification=4;auto edge_bounds=scene_pose_placement(edge,edge_pose,MirrorConvention_Ani2);CHECK(edge_bounds.x==1-(5+32768)*4&&edge_bounds.y==2-(3-32767)*4&&edge_bounds.w==20&&edge_bounds.h==12);
    if(scaled.empty())return 1;
    auto badversion=scaled;badversion.replace(badversion.find("version=5"),9,"version=4");write(root/"bad.ISTUDIO",badversion);CHECK(!SceneProject::read((root/"bad.ISTUDIO").u8string(),error));auto invalid=scaled;invalid.replace(invalid.find("magnification=4"),15,"magnification=5");write(root/"bad.ISTUDIO",invalid);CHECK(!SceneProject::read((root/"bad.ISTUDIO").u8string(),error));
    // Classic clamps its row zoom to 1..4. Mirror/local/motion conversion must
    // remain correct when those offsets are subsequently magnified.
    std::string wax="format=imgtool_world_project\nversion=1\ndoc.count=1\ndoc.0.path="+img.u8string()+"\nslot.count=1\nslot.0.doc_idx=0\nslot.0.sequence_frames=0\nslot.0.frame_delays=10\nslot.0.local_dx=3\nslot.0.local_dy=-2\nslot.0.motion_dx=1\nslot.0.motion_dy=2\nslot.0.dual_on=1\nslot.0.dual_dx=7\nslot.0.dual_dy=8\n";
    for(int zoom:{-9,1,2,3,4,99})for(int bits=0;bits<4;++bits){auto input=root/"zoom.WAX";write(input,wax+"slot.0.zoom="+std::to_string(zoom)+"\nslot.0.frame_mirror="+std::to_string(bits)+"\n");auto imported=SceneProject::read(input.u8string(),error);CHECK(imported&&imported->state.lanes[0].magnification==std::clamp(zoom,1,4));if(!imported)continue;CHECK(imported->bind([&](int,const SceneAsset &){return SceneBinding{101,art.get()};},issues));auto p=evaluate_scene(imported->state,2);int scale=std::clamp(zoom,1,4),dx=bits&1?1:-1,dy=bits&2?-1:1;CHECK(p.size()==2&&p[0].x==200+dx*(3-2)*scale&&p[0].y==20+dy*(2+4)*scale&&p[1].x==200+dx*(7-2)*scale);CHECK(imported->save((root/"import.ISTUDIO").u8string(),sources,error));auto disk=SceneProject::read((root/"import.ISTUDIO").u8string(),error);CHECK(disk&&disk->state.lanes[0].magnification==scale);}
    write(root/"zoom.WAX",wax+"slot.0.zoom=2\nslot.0.subframe_swap_tick=4\n");CHECK(!SceneProject::read((root/"zoom.WAX").u8string(),error)&&error.find("subframe")!=std::string::npos);
    CHECK(art->generation()==generation&&!art->dirty()&&art->copy(0,0,0,5,3).pixels==pixels&&bytes(img)==original&&g_doc==nullptr);fs::remove_all(root);std::cout<<"Magnification: "<<failures<<" failures\n";return failures?1:0;
}
