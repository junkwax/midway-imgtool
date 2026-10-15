#include "studio_sheet.h"
#include "studio_file_io.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <map>
#include <numeric>
#include <chrono>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
namespace studio {
namespace {
namespace fs=std::filesystem;using Bytes=std::vector<unsigned char>;
void word(Bytes &b,unsigned v){b.push_back((unsigned char)v);b.push_back((unsigned char)(v>>8));}
void dword(Bytes &b,uint32_t v){word(b,v);word(b,v>>16);}
std::string lower(std::string s){for(char &c:s)if(c>='A'&&c<='Z')c+=32;return s;}
bool read_target(SheetDestination &d,std::string &error){
    std::error_code ec;d.existed=fs::exists(fs::u8path(d.path),ec);if(ec){error="Could not inspect an output path.";return false;}d.previous.clear();
    if(d.existed){std::ifstream f(fs::u8path(d.path),std::ios::binary|std::ios::ate);auto n=f?f.tellg():std::streampos(-1);if(n<0||n>64*1024*1024){error="Could not review the output (limit: 64 MiB per file).";return false;}d.previous.resize((size_t)n);f.seekg(0);if(n&&!f.read((char *)d.previous.data(),d.previous.size())){error="Could not read an existing output.";return false;}}
    return true;
}
bool alias(const std::string &a,const std::string &b){if(b.empty())return false;std::error_code ec;auto x=fs::absolute(fs::u8path(a),ec).lexically_normal();if(ec)return true;auto y=fs::absolute(fs::u8path(b),ec).lexically_normal();if(ec)return true;
    bool same=x==y;
#ifdef _WIN32
    same|=lower(x.u8string())==lower(y.u8string());
#endif
    return same||fs::equivalent(x,y,ec);
}
bool same_media(const IndexedMedia &a,const IndexedMedia &b){return a.w==b.w&&a.h==b.h&&a.anchor_x==b.anchor_x&&a.anchor_y==b.anchor_y&&a.words==b.words&&a.pixels==b.pixels;}
bool same_row(const SheetRow &a,const SheetRow &b){return a.sprite==b.sprite&&a.enabled==b.enabled&&a.index==b.index&&a.x==b.x&&a.y==b.y&&a.w==b.w&&a.h==b.h&&a.entry==b.entry&&a.name==b.name&&a.error==b.error;}
bool reserve(const fs::path &path){
#ifdef _WIN32
    auto h=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(h==INVALID_HANDLE_VALUE)return false;CloseHandle(h);return true;
#else
    int fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)return false;::close(fd);return true;
#endif
}
bool install(const fs::path &from,const fs::path &to){
#ifdef _WIN32
    return MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
    std::error_code ec;fs::rename(from,to,ec);return !ec;
#endif
}
}
static bool plan_sheet_impl(const Artwork &art,const std::vector<ExportRequest> &requests,const SheetOptions &o,SheetPlan &out,std::string &error){
    error.clear();if(requests.empty()||requests.size()>2000||o.width<1||o.width>8192||o.gap<0||o.gap>32||(o.anf&&(o.width!=256||o.rgba))){error="Choose a width of 1–8192 and spacing of 0–32. ANF requires a 256-wide indexed sheet.";return false;}
    SheetPlan p;p.owner=&art;p.generation=art.generation();p.options=o;std::map<uint64_t,bool> wanted;
    for(auto &r:requests)if(!r.sprite||art.sprite_index(r.sprite)<0||!wanted.emplace(r.sprite,r.enabled).second){error="Sheet requests contain missing or duplicate sprites.";return false;}
    size_t bytes=0;for(int i=0;i<art.count();++i){auto id=art.sprite_id(i);auto found=wanted.find(id);if(found==wanted.end())continue;p.requests.push_back({id,found->second});auto im=art.sprite(i);SheetRow r;r.sprite=id;r.enabled=found->second;r.index=i;r.name=im->n_s;r.w=im->w;r.h=im->h;IndexedMedia source;
        if(r.enabled){if(!snapshot_indexed_sprite(art,i,source,r.error)){}else if(r.w>o.width)r.error="Wider than the sheet.";else if(o.anf&&(r.w>256||r.h>256))r.error="ANF face coordinates require sprites no larger than 256 x 256.";
            bytes+=source.pixels.size()+source.words.size()*2;if(bytes>128*1024*1024){error="Sheet sources exceed the 128 MiB snapshot limit.";return false;}++p.included;}
        p.rows.push_back(std::move(r));p.sources.push_back(std::move(source));}
    auto blocked_plan=[&](const std::string &message){error=message;for(auto &r:p.rows)if(r.enabled){if(r.error.empty())r.error=message;++p.blocked;}out=std::move(p);return true;};
    if(o.anf&&p.included>1000)return blocked_plan("The classic ANF face map supports at most 1000 sprites. Exclude sprites or disable ANF.");
    // Height sorting changes packing only. Face entries retain library order,
    // matching doc/it/itimg.asm's final AFACE loop.
    std::vector<int> order;for(int i=0;i<(int)p.rows.size();++i)if(p.rows[i].enabled&&p.rows[i].error.empty())order.push_back(i);
    std::stable_sort(order.begin(),order.end(),[&](int a,int b){return p.rows[a].h>p.rows[b].h;});
    int x=0,y=0,shelf=0;for(int i:order){auto &r=p.rows[i];if(x+r.w>o.width){x=0;y+=shelf+o.gap;shelf=0;}r.x=x;r.y=y;x+=r.w+o.gap;shelf=std::max(shelf,r.h);p.height=std::max(p.height,y+r.h);}
    if(p.height>(o.anf?6000:8192)||(uint64_t)o.width*p.height>4*1024*1024)return blocked_plan("Packed sheet exceeds 6000 ANF rows, 8192 general rows or four million pixels. Increase width or export fewer sprites.");
    if(!o.rgba&&!order.empty()){
        p.words=p.sources[order[0]].words;bool same=true;for(int i:order)same&=p.sources[i].words==p.words;
        if(!same){p.merged=true;p.words={0};std::map<unsigned short,unsigned char> colors;
            // Exact used-color union: zero stays transparent; opaque black is
            // a distinct nonzero slot. No palette quantization or silent loss.
            for(size_t i=0;i<p.rows.size();++i)if(p.rows[i].enabled&&p.rows[i].error.empty()){auto &s=p.sources[i];for(auto c:s.pixels)if(c){auto color=(unsigned short)(s.words[c]&0x7fff);if(!colors.count(color)){if(p.words.size()==256){p.rows[i].error="Mixed palettes exceed 255 opaque colors. Use RGBA without ANF or split the sheet.";break;}colors[color]=(unsigned char)p.words.size();p.words.push_back(color);}}}
        }
    }
    for(auto &r:p.rows)if(r.enabled&&!r.error.empty())++p.blocked;
    p.ready=p.included>0&&!p.blocked&&p.height>0;
    if(p.ready){int components=o.rgba?4:1;p.pixels.assign((size_t)o.width*p.height*components,0);std::map<unsigned short,unsigned char> mapping;if(p.merged)for(size_t c=1;c<p.words.size();++c)mapping[p.words[c]]=(unsigned char)c;
        int entry=0;for(size_t i=0;i<p.rows.size();++i){auto &r=p.rows[i];if(!r.enabled)continue;r.entry=entry++;auto &s=p.sources[i];for(int sy=0;sy<s.h;++sy)for(int sx=0;sx<s.w;++sx){unsigned ci=s.pixels[(size_t)sy*s.w+sx];size_t to=((size_t)(r.y+sy)*o.width+r.x+sx)*components;
                if(o.rgba){unsigned char color[]={(unsigned char)s.words[ci],(unsigned char)(s.words[ci]>>8)};pal_word_to_rgb8(color,&p.pixels[to],&p.pixels[to+1],&p.pixels[to+2]);p.pixels[to+3]=ci?255:0;}
                else p.pixels[to]=p.merged?(ci?mapping[(unsigned short)(s.words[ci]&0x7fff)]:0):(unsigned char)ci;}}
        if(o.rgba){p.tga={0,0,2,0,0,0,0,0,0,0,0,0};word(p.tga,o.width);word(p.tga,p.height);p.tga.insert(p.tga.end(),{32,40});for(size_t i=0;i<p.pixels.size();i+=4)p.tga.insert(p.tga.end(),{p.pixels[i+2],p.pixels[i+1],p.pixels[i],p.pixels[i+3]});}
        else{p.tga={0,1,1,0,0};word(p.tga,(unsigned)p.words.size());p.tga.insert(p.tga.end(),{15,0,0,0,0});word(p.tga,o.width);word(p.tga,p.height);p.tga.insert(p.tga.end(),{8,0});for(auto c:p.words)word(p.tga,c);for(int row=p.height-1;row>=0;--row)p.tga.insert(p.tga.end(),p.pixels.begin()+(size_t)row*o.width,p.pixels.begin()+(size_t)(row+1)*o.width);}
        if(o.anf){p.anf={'A','N','F',' '};dword(p.anf,(uint32_t)p.included-1);for(size_t i=0;i<p.rows.size();++i){auto &r=p.rows[i];if(!r.enabled)continue;auto &s=p.sources[i];dword(p.anf,0);dword(p.anf,(uint32_t)(unsigned short)s.anchor_x|((uint32_t)(unsigned short)s.anchor_y<<16));dword(p.anf,0);dword(p.anf,0);dword(p.anf,3);dword(p.anf,6);dword(p.anf,9);word(p.anf,(unsigned short)-r.x);word(p.anf,(unsigned char)(r.w-1-r.x));word(p.anf,((r.h-1)<<8)|(unsigned char)(r.w-1-r.x));word(p.anf,((r.h-1)<<8)|(unsigned char)-r.x);dword(p.anf,r.y);}}
    }
    out=std::move(p);return true;
}
std::vector<unsigned char> render_sheet(const SheetPlan &p){if(!p.ready||p.options.width<1||p.options.width>8192||p.height<1||p.height>8192||(uint64_t)p.options.width*p.height>4*1024*1024||p.pixels.size()!=(size_t)p.options.width*p.height*(p.options.rgba?4:1)||(!p.options.rgba&&(p.words.empty()||p.words.size()>256)))return {};if(p.options.rgba)return p.pixels;Bytes rgba((size_t)p.options.width*p.height*4);for(size_t i=0;i<p.pixels.size();++i){unsigned c=p.pixels[i];if(c>=p.words.size())return {};unsigned char color[]={(unsigned char)p.words[c],(unsigned char)(p.words[c]>>8)};pal_word_to_rgb8(color,&rgba[i*4],&rgba[i*4+1],&rgba[i*4+2]);rgba[i*4+3]=c?255:0;}return rgba;}
static bool review_sheet_destinations_impl(const Artwork &art,const SheetPlan &p,const std::string &path,std::vector<SheetDestination> &out,std::string &error){
    error.clear();out.clear();if(p.owner!=&art||!p.ready){error="Build a valid sheet review first.";return false;}std::error_code ec;auto target=fs::absolute(fs::u8path(path),ec).lexically_normal();if(ec||lower(target.extension().u8string())!=".tga"){error="Choose a .tga filename.";return false;}
    std::vector<fs::path> paths{target};if(p.options.anf){auto side=target;side.replace_extension(".ANF");paths.push_back(side);}
    for(auto &file:paths){SheetDestination d;d.path=file.u8string();if(alias(d.path,art.path())){error="Sheet output cannot replace its source IMG.";return false;}for(auto &prior:out)if(alias(d.path,prior.path)){error="The sheet and face map cannot refer to the same file.";return false;}if(!read_target(d,error))return false;out.push_back(std::move(d));}return true;
}
static bool write_sheet_impl(const Artwork &art,const SheetPlan &p,const std::vector<SheetDestination> &destinations,std::string &error){
    error.clear();if(p.owner!=&art||p.generation!=art.generation()||!p.ready||destinations.size()!=(p.options.anf?2:1)){error="Artwork or sheet review changed. Rebuild before exporting.";return false;}
    SheetPlan fresh;if(!plan_sheet(art,p.requests,p.options,fresh,error))return false;bool same=fresh.ready&&fresh.tga==p.tga&&fresh.anf==p.anf&&fresh.pixels==p.pixels&&fresh.words==p.words&&fresh.height==p.height&&fresh.included==p.included&&fresh.blocked==p.blocked&&fresh.merged==p.merged&&fresh.rows.size()==p.rows.size()&&fresh.sources.size()==p.sources.size();
    for(size_t i=0;same&&i<p.rows.size();++i)same=same_row(fresh.rows[i],p.rows[i])&&same_media(fresh.sources[i],p.sources[i]);if(!same){error="Source or sheet preview changed. Rebuild before exporting.";return false;}
    std::vector<SheetDestination> current;if(!review_sheet_destinations(art,p,destinations[0].path,current,error))return false;
    for(size_t i=0;i<current.size();++i)if(current[i].path!=destinations[i].path||current[i].existed!=destinations[i].existed||current[i].previous!=destinations[i].previous){error="An output changed after review. Rebuild before replacing either file.";return false;}
    struct Staged {fs::path output,backup;bool keep_backup=false;};std::vector<Staged> staged(current.size());
    struct Cleanup {std::vector<Staged> &items;~Cleanup(){std::error_code ec;for(auto &s:items){if(!s.output.empty())fs::remove(s.output,ec);if(!s.backup.empty()&&!s.keep_backup)fs::remove(s.backup,ec);}}} cleanup{staged};
    static unsigned counter=0;auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    for(size_t i=0;i<current.size();++i){auto &s=staged[i];auto create=[&](fs::path &result,const Bytes &bytes){auto temp=fs::u8path(current[i].path);temp+=".studio-sheet-"+std::to_string(stamp)+"-"+std::to_string(++counter)+".staged";if(!reserve(temp)){error="Could not stage both sheet outputs.";return false;}result=temp;return atomic_write_file(temp.u8string(),bytes,error);};
        if(!create(s.output,i?fresh.anf:fresh.tga)||(current[i].existed&&!create(s.backup,current[i].previous)))return false;}
    size_t installed=0;try{for(;installed<current.size();++installed){SheetDestination now;now.path=current[installed].path;if(!read_target(now,error)||now.existed!=current[installed].existed||now.previous!=current[installed].previous){error="An output changed while preparing the export.";break;}if(!install(staged[installed].output,fs::u8path(current[installed].path))){error="Could not replace both outputs (one may be read-only or open).";break;}}}catch(const std::exception &e){error=std::string("Could not finish replacing outputs: ")+e.what();}
    if(installed==current.size())return true;
    while(installed){--installed;auto &s=staged[installed];SheetDestination now;now.path=current[installed].path;std::string ignored;bool ours=false;try{ours=read_target(now,ignored)&&now.existed&&now.previous==(installed?fresh.anf:fresh.tga);}catch(...){ }bool restored=false;
        if(ours){if(current[installed].existed)restored=install(s.backup,fs::u8path(now.path));else{std::error_code ec;restored=fs::remove(fs::u8path(now.path),ec)&&!ec;}}
        if(!restored){s.keep_backup=current[installed].existed;error+=" Earlier output could not be restored; "+(s.keep_backup?"previous bytes kept at "+s.backup.u8string():"check "+now.path)+".";}}
    return false;
}
bool plan_sheet(const Artwork &art,const std::vector<ExportRequest> &requests,const SheetOptions &options,SheetPlan &out,std::string &error){
    try{return plan_sheet_impl(art,requests,options,out,error);}catch(const std::exception &e){error=std::string("Could not build the sheet review: ")+e.what();return false;}
}
bool review_sheet_destinations(const Artwork &art,const SheetPlan &plan,const std::string &path,std::vector<SheetDestination> &out,std::string &error){
    try{return review_sheet_destinations_impl(art,plan,path,out,error);}catch(const std::exception &e){out.clear();error=std::string("Could not review sheet outputs: ")+e.what();return false;}
}
bool write_sheet(const Artwork &art,const SheetPlan &plan,const std::vector<SheetDestination> &destinations,std::string &error){
    try{return write_sheet_impl(art,plan,destinations,error);}catch(const std::exception &e){error=std::string("Could not export the sheet: ")+e.what();return false;}
}

}
