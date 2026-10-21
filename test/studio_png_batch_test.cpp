#include "studio_png_batch.h"
#include "studio_layers.h"
#include "studio_img_codec.h"
#include "stb_image.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <set>
#include <algorithm>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
namespace fs=std::filesystem;using Bytes=std::vector<unsigned char>;static int failures=0;
#define CHECK(x) do{if(!(x)){if(failures<20)std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(0)
static Bytes read(const fs::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void write(const fs::path &p,const Bytes &b){std::ofstream f(p,std::ios::binary);f.write((const char *)b.data(),b.size());}
static std::vector<studio::ExportRequest> all(const studio::Artwork &a){std::vector<studio::ExportRequest> r;for(int i=0;i<a.count();++i)r.push_back({a.sprite_id(i),true});return r;}
static void pixels(const studio::PngBatchRow &r){auto &s=r.source;int w=0,h=0,c=0;auto rgba=stbi_load_from_memory(r.png.data(),(int)r.png.size(),&w,&h,&c,4);CHECK(rgba&&w==s.w&&h==s.h);if(!rgba)return;for(size_t i=0;i<s.pixels.size();++i){auto ci=s.pixels[i];unsigned char color[]={(unsigned char)s.words[ci],(unsigned char)(s.words[ci]>>8)},red,green,blue;pal_word_to_rgb8(color,&red,&green,&blue);CHECK(rgba[i*4]==red&&rgba[i*4+1]==green&&rgba[i*4+2]==blue&&rgba[i*4+3]==(ci?255:0));}stbi_image_free(rgba);}
int main(){auto parent=fs::temp_directory_path();auto folder=parent/("studio-png-batch-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directory(folder);auto target=folder/fs::u8path(u8"PNG-é-日");fs::create_directory(target);std::string error;
    auto art=studio::Artwork::demo();CHECK(art->save((folder/"source.IMG").u8string(),error));auto source=read(folder/"source.IMG");auto requests=all(*art);studio::PngBatchPlan p;auto gen=art->generation();auto version=art->disk_version();auto selection=art->selection_id();bool undo=art->can_undo(),redo=art->can_redo();
    CHECK(studio::plan_png_batch(*art,requests,target.u8string(),{},p,error));CHECK(p.ready&&p.included==art->count()&&p.replacements==0);for(auto &r:p.rows)pixels(r);
    CHECK(studio::write_png_batch(*art,p,error));for(auto &r:p.rows)CHECK(read(fs::u8path(r.path))==r.png);CHECK(art->generation()==gen&&art->disk_version()==version&&art->selection_id()==selection&&!art->dirty()&&art->can_undo()==undo&&art->can_redo()==redo&&read(folder/"source.IMG")==source);
    CHECK(!studio::write_png_batch(*art,p,error));CHECK(error.find("changed after review")!=std::string::npos);
    CHECK(studio::plan_png_batch(*art,requests,target.u8string(),{},p,error));CHECK(p.replacements==art->count());CHECK(studio::write_png_batch(*art,p,error));
    // Edited bytes, including raw changes that bypass the generation counter,
    // are never exported under a stale review.
    CHECK(studio::plan_png_batch(*art,requests,target.u8string(),{},p,error));auto pixel=(unsigned char *)art->sprite(0)->data_p;auto old=*pixel;*pixel^=1;CHECK(!studio::write_png_batch(*art,p,error));*pixel=old;
    auto pal=(unsigned char *)art->palette(art->sprite(0)->palnum)->data_p;pal[2]^=1;CHECK(!studio::write_png_batch(*art,p,error));pal[2]^=1;
    auto tampered=p;tampered.rows[0].filename="tampered.png";CHECK(!studio::write_png_batch(*art,tampered,error));tampered=p;tampered.rows[0].png[0]^=1;CHECK(!studio::write_png_batch(*art,tampered,error));
    art->anchor(0,12,15);art->commit();CHECK(!studio::write_png_batch(*art,p,error));art->undo();
    CHECK(studio::plan_png_batch(*art,requests,target.u8string(),{},p,error));write(fs::u8path(p.rows.back().path),{1,2,3});auto first=read(fs::u8path(p.rows.front().path));CHECK(!studio::write_png_batch(*art,p,error));CHECK(read(fs::u8path(p.rows.front().path))==first&&read(fs::u8path(p.rows.back().path))==Bytes({1,2,3}));
    // Exclusion affects only this export. A blocked row can be excluded.
    auto blocked=fs::u8path(p.rows[1].path);fs::remove(blocked);fs::create_directory(blocked);CHECK(studio::plan_png_batch(*art,requests,target.u8string(),{},p,error));CHECK(!p.ready&&p.blocked==1);requests[1].enabled=false;CHECK(studio::plan_png_batch(*art,requests,target.u8string(),{},p,error));CHECK(p.ready&&p.included==art->count()-1);CHECK(studio::write_png_batch(*art,p,error));fs::remove(blocked);requests=all(*art);
    // Safe, unique filenames are deterministic in library order, including
    // case collisions, path traversal, device names and suffix-name collisions.
    const char *names[]={"Same","same","Same__2","../bad\\name","CON","...","a?b","a*b"};for(int i=0;i<art->count();++i){memset(art->sprite(i)->n_s,0,16);strncpy(art->sprite(i)->n_s,names[i],15);}
    std::reverse(requests.begin(),requests.end());CHECK(studio::plan_png_batch(*art,requests,target.u8string(),{},p,error));CHECK(p.ready&&p.rows[0].filename=="Same.png"&&p.rows[1].filename=="same__2.png"&&p.rows[2].filename=="Same__2__2.png"&&p.rows[4].filename=="_CON.png"&&p.rows[5].filename=="sprite.png");std::set<std::string> names_seen;for(auto &r:p.rows){CHECK(fs::u8path(r.path).parent_path()==target);auto key=r.filename;for(char &c:key)c=(char)std::tolower((unsigned char)c);CHECK(names_seen.insert(key).second);}CHECK(studio::write_png_batch(*art,p,error));
    // Export the visible overlay without flattening it or changing base pixels.
    auto clip=art->copy(0,0,0,2,2);clip.pixels.assign(4,2);CHECK(art->paste_layer(0,clip,0,0,error));auto im=art->sprite(0);auto layer=im->layer_p;auto base=art->copy(0,0,0,im->w,im->h).pixels;gen=art->generation();CHECK(studio::plan_png_batch(*art,requests,target.u8string(),{},p,error));CHECK(p.rows[0].source.pixels[0]==2);pixels(p.rows[0]);CHECK(studio::write_png_batch(*art,p,error));CHECK(im->layer_p==layer&&art->generation()==gen&&art->copy(0,0,0,im->w,im->h).pixels==base);
    CHECK(art->edit_layer(0,0,0,false,false,false,error));CHECK(studio::plan_png_batch(*art,requests,target.u8string(),{},p,error));CHECK(p.rows[0].source.pixels[0]==base[0]);
    // Opaque black is not confused with the transparent zero index.
    auto black=studio::Artwork::create();CHECK(black->canvas_size(0,3,1,0,error));((unsigned char *)black->sprite(0)->data_p)[0]=0;((unsigned char *)black->sprite(0)->data_p)[1]=1;black->color(black->sprite(0)->palnum,1,0);black->commit();CHECK(studio::plan_png_batch(*black,all(*black),target.u8string(),{},p,error));CHECK(p.ready);pixels(p.rows[0]);
    // Source aliases (including other open libraries) and output hardlink
    // collisions cannot replace IMG bytes. Symlink outputs are rejected too.
    CHECK(studio::plan_png_batch(*art,requests,target.u8string(),{},p,error));auto alias=fs::u8path(p.rows[0].path);fs::remove(alias);std::error_code ec;fs::create_hard_link(folder/"source.IMG",alias,ec);if(!ec){CHECK(studio::plan_png_batch(*art,requests,target.u8string(),{},p,error));CHECK(!p.ready&&p.blocked>0);CHECK(!studio::write_png_batch(*art,p,error));CHECK(read(folder/"source.IMG")==source);fs::remove(alias);}
    write(alias,{8});CHECK(studio::plan_png_batch(*art,requests,target.u8string(),{alias.u8string()},p,error));CHECK(!p.ready&&p.blocked>0);
    fs::remove(alias);fs::create_symlink(folder/"source.IMG",alias,ec);if(!ec){CHECK(studio::plan_png_batch(*art,requests,target.u8string(),{},p,error));CHECK(!p.ready&&p.blocked>0);fs::remove(alias);}
    auto alias_two=fs::u8path(p.rows[1].path);write(alias,{3,4});fs::remove(alias_two);ec.clear();fs::create_hard_link(alias,alias_two,ec);if(!ec){CHECK(studio::plan_png_batch(*art,requests,target.u8string(),{},p,error));CHECK(!p.ready&&p.blocked>0);}fs::remove(alias);fs::remove(alias_two);
#ifdef _WIN32
    // Denying replacement of the second output exercises real rollback after
    // the first new/old file was installed; no temporary files may be left.
    requests={{art->sprite_id(0),true},{art->sprite_id(1),true}};CHECK(studio::plan_png_batch(*art,requests,target.u8string(),{},p,error));auto one=fs::u8path(p.rows[0].path),two=fs::u8path(p.rows[1].path);write(one,{9,8});write(two,{7,6});CHECK(studio::plan_png_batch(*art,requests,target.u8string(),{},p,error));CHECK(SetFileAttributesW(two.c_str(),FILE_ATTRIBUTE_READONLY));CHECK(!studio::write_png_batch(*art,p,error));CHECK(read(one)==Bytes({9,8})&&read(two)==Bytes({7,6}));CHECK(SetFileAttributesW(two.c_str(),FILE_ATTRIBUTE_NORMAL));fs::remove(one);CHECK(studio::plan_png_batch(*art,requests,target.u8string(),{},p,error));CHECK(SetFileAttributesW(two.c_str(),FILE_ATTRIBUTE_READONLY));CHECK(!studio::write_png_batch(*art,p,error));CHECK(!fs::exists(one)&&read(two)==Bytes({7,6}));CHECK(SetFileAttributesW(two.c_str(),FILE_ATTRIBUTE_NORMAL));
#endif
    // Historical near-record and current far-record libraries stay byte-for-byte
    // unchanged. This creates test fixtures only, not an application conversion.
    for(unsigned v:{0x60au,0x61cu,0x61du,0x631u,0x634u}){auto b=source;if(v!=0x634)CHECK(studio::project_img_layout(b,v,error));auto file=folder/(std::to_string(v)+".IMG");write(file,b);auto old_art=studio::Artwork::open(file.u8string(),error);CHECK(old_art);if(!old_art)continue;CHECK(studio::plan_png_batch(*old_art,all(*old_art),target.u8string(),{},p,error));CHECK(p.ready&&studio::write_png_batch(*old_art,p,error));CHECK(old_art->disk_version()==v&&!old_art->dirty()&&!old_art->can_undo()&&read(file)==b);}
    auto bad=requests;bad.push_back(bad[0]);CHECK(!studio::plan_png_batch(*art,bad,target.u8string(),{},p,error));bad[0].sprite=999999;CHECK(!studio::plan_png_batch(*art,bad,target.u8string(),{},p,error));CHECK(!studio::plan_png_batch(*art,{},target.u8string(),{},p,error));CHECK(!studio::plan_png_batch(*art,requests,(target/"missing").u8string(),{},p,error));
    CHECK(!studio::plan_png_batch(*art,requests,(folder/"source.IMG").u8string(),{},p,error));
    for(auto &e:fs::directory_iterator(target))CHECK(e.path().extension()!=".staged"&&e.path().extension()!=".tmp");CHECK(read(folder/"source.IMG")==source);CHECK(fs::weakly_canonical(folder).parent_path()==fs::weakly_canonical(parent));if(fs::weakly_canonical(folder).parent_path()==fs::weakly_canonical(parent))fs::remove_all(folder);std::cout<<"Bulk PNG export: "<<failures<<" failure(s)\n";return failures?1:0;
}
