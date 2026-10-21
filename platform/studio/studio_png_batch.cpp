#include "studio_png_batch.h"
#include "studio_file_io.h"
#include "stb_image_write.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
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
constexpr size_t limit=128*1024*1024;
std::string lower(std::string s){for(char &c:s)if(c>='A'&&c<='Z')c+=32;return s;}
std::string safe_name(std::string s){
    for(char &c:s)if((unsigned char)c<32||(unsigned char)c>=127||std::string("<>:\"/\\|?*").find(c)!=std::string::npos)c='_';
    while(!s.empty()&&(s.back()=='.'||s.back()==' '))s.pop_back();if(s.empty())s="sprite";
    auto base=lower(s.substr(0,s.find('.')));if(base=="con"||base=="prn"||base=="aux"||base=="nul"||(base.size()==4&&(base.substr(0,3)=="com"||base.substr(0,3)=="lpt")&&base[3]>='1'&&base[3]<='9'))s="_"+s;
    return s;
}
bool alias(const std::string &a,const std::string &b){if(b.empty())return false;std::error_code ec;
    auto x=fs::absolute(fs::u8path(a),ec).lexically_normal();if(ec)return true;auto y=fs::absolute(fs::u8path(b),ec).lexically_normal();if(ec)return true;
    bool same=x==y;
#ifdef _WIN32
    same|=lower(x.u8string())==lower(y.u8string());
#endif
    return same||fs::equivalent(x,y,ec);
}
bool read_target(const std::string &path,bool &existed,Bytes &bytes,std::string &error){
    std::error_code ec;auto target=fs::u8path(path);auto status=fs::symlink_status(target,ec);
    if(ec==std::errc::no_such_file_or_directory){ec.clear();status=fs::file_status(fs::file_type::not_found);}
    if(ec){error="Cannot inspect output: "+path;return false;}existed=fs::exists(status);bytes.clear();if(!existed)return true;
    if(!fs::is_regular_file(status)){error="Output must be a regular file, not a folder or link: "+path;return false;}
    std::ifstream f(target,std::ios::binary|std::ios::ate);auto n=f?f.tellg():std::streampos(-1);
    if(n<0||n>64*1024*1024){error="Cannot review existing output (64 MiB per file limit): "+path;return false;}
    bytes.resize((size_t)n);f.seekg(0);if(n&&!f.read((char *)bytes.data(),bytes.size())){error="Cannot read existing output: "+path;return false;}return true;
}
bool same_source(const IndexedMedia &a,const IndexedMedia &b){return a.w==b.w&&a.h==b.h&&a.anchor_x==b.anchor_x&&a.anchor_y==b.anchor_y&&a.pixels==b.pixels&&a.words==b.words;}
bool same_row(const PngBatchRow &a,const PngBatchRow &b){return a.sprite==b.sprite&&a.enabled==b.enabled&&a.existed==b.existed&&a.name==b.name&&a.filename==b.filename&&a.path==b.path&&a.error==b.error&&same_source(a.source,b.source)&&a.png==b.png&&a.previous==b.previous;}
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
bool plan_impl(const Artwork &art,const std::vector<ExportRequest> &requests,const std::string &folder,const std::vector<std::string> &protected_paths,PngBatchPlan &out,std::string &error){
    error.clear();out={};if(requests.empty()||requests.size()>2000){error="Choose between 1 and 2000 sprites to export.";return false;}
    std::error_code ec;auto target=fs::absolute(fs::u8path(folder),ec).lexically_normal();if(folder.empty()||ec||!fs::is_directory(target,ec)||ec){error="Choose an existing destination folder.";return false;}
    PngBatchPlan p;p.owner=&art;p.generation=art.generation();p.folder=target.u8string();p.protected_paths=protected_paths;
    std::map<uint64_t,bool> wanted;for(auto &r:requests)if(!r.sprite||art.sprite_index(r.sprite)<0||!wanted.emplace(r.sprite,r.enabled).second){error="Export requests contain missing or duplicate sprites.";return false;}
    std::set<std::string> names;size_t bytes=0;
    for(int i=0;i<art.count();++i){auto id=art.sprite_id(i);auto found=wanted.find(id);if(found==wanted.end())continue;
        PngBatchRow r;r.sprite=id;r.enabled=found->second;r.name=art.sprite(i)->n_s;auto base=safe_name(r.name);r.filename=base+".png";
        for(int suffix=2;!names.insert(lower(r.filename)).second;++suffix)r.filename=base+"__"+std::to_string(suffix)+".png";
        r.path=(target/fs::u8path(r.filename)).u8string();p.requests.push_back({id,r.enabled});
        if(r.enabled){++p.included;
            if(alias(r.path,art.path()))r.error="Output cannot replace the source IMG.";
            for(auto &path:protected_paths)if(alias(r.path,path))r.error="Output cannot replace an open source file.";
            if(r.error.empty()&&snapshot_indexed_sprite(art,i,r.source,r.error)){
                auto &s=r.source;bytes+=s.pixels.size()+s.words.size()*2;if(bytes>limit){error="PNG review exceeds 128 MiB. Export fewer sprites.";return false;}
                Bytes rgba(s.pixels.size()*4);for(size_t pixel=0;pixel<s.pixels.size();++pixel){unsigned ci=s.pixels[pixel];unsigned char color[]={(unsigned char)s.words[ci],(unsigned char)(s.words[ci]>>8)};auto to=&rgba[pixel*4];pal_word_to_rgb8(color,to,to+1,to+2);to[3]=ci?255:0;}
                auto write=[](void *context,void *data,int n){auto &b=*(Bytes *)context;b.insert(b.end(),(unsigned char *)data,(unsigned char *)data+n);};
                if(!stbi_write_png_to_func(write,&r.png,s.w,s.h,4,rgba.data(),s.w*4))r.error="PNG encoding failed.";
                if(r.error.empty())read_target(r.path,r.existed,r.previous,r.error);
                // Unique filenames already rule out path collisions. Only
                // multiply-linked files need the more expensive alias scan.
                if(r.error.empty()&&r.existed){std::error_code link_error;auto links=fs::hard_link_count(fs::u8path(r.path),link_error);if(link_error||links>1)for(auto &prior:p.rows)if(prior.enabled&&prior.existed&&alias(r.path,prior.path))r.error="Two outputs refer to the same file.";}
                bytes+=r.png.size()+r.previous.size();if(bytes>limit){error="PNG review exceeds 128 MiB. Export fewer sprites.";return false;}
            }
            if(!r.error.empty())++p.blocked;else if(r.existed)++p.replacements;
        }
        p.rows.push_back(std::move(r));
    }
    p.ready=p.included>0&&!p.blocked;out=std::move(p);return true;
}
bool write_impl(const Artwork &art,const PngBatchPlan &p,std::string &error){
    error.clear();if(p.owner!=&art||p.generation!=art.generation()||!p.ready){error="Artwork or review changed. Build a new PNG review.";return false;}
    PngBatchPlan fresh;if(!plan_impl(art,p.requests,p.folder,p.protected_paths,fresh,error))return false;
    bool same=fresh.ready&&fresh.included==p.included&&fresh.blocked==p.blocked&&fresh.replacements==p.replacements&&fresh.rows.size()==p.rows.size();
    for(size_t i=0;same&&i<p.rows.size();++i)same=same_row(p.rows[i],fresh.rows[i]);
    if(!same){error="Source or output changed after review. Build a new PNG review.";return false;}
    struct Staged {fs::path output,backup;bool keep=false;};std::vector<Staged> staged(p.rows.size());
    struct Cleanup {std::vector<Staged> &items;~Cleanup(){std::error_code ec;for(auto &s:items){if(!s.output.empty())fs::remove(s.output,ec);if(!s.backup.empty()&&!s.keep)fs::remove(s.backup,ec);}}} cleanup{staged};
    static unsigned counter=0;auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    for(size_t i=0;i<p.rows.size();++i){auto &r=p.rows[i];if(!r.enabled)continue;auto create=[&](fs::path &path,const Bytes &b){auto temp=fs::u8path(p.folder)/(".studio-png-"+std::to_string(stamp)+"-"+std::to_string(++counter)+".staged");if(!reserve(temp)){error="Cannot prepare PNG files in this folder.";return false;}path=temp;return atomic_write_file(temp.u8string(),b,error);};
        if(!create(staged[i].output,r.png)||(r.existed&&!create(staged[i].backup,r.previous)))return false;
    }
    size_t installed=0;try{for(;installed<p.rows.size();++installed){auto &r=p.rows[installed];if(!r.enabled)continue;bool existed=false;Bytes bytes;
            if(!read_target(r.path,existed,bytes,error)||existed!=r.existed||bytes!=r.previous){error="An output changed while preparing PNGs.";break;}
            if(!install(staged[installed].output,fs::u8path(r.path))){error="Cannot install PNG: "+r.filename+" (it may be read-only or open).";break;}
        }}catch(const std::exception &e){error=std::string("Cannot finish PNG export: ")+e.what();}
    if(installed==p.rows.size())return true;
    while(installed){--installed;auto &r=p.rows[installed];if(!r.enabled)continue;auto &s=staged[installed];bool existed=false,ours=false;Bytes bytes;std::string ignored;
        try{ours=read_target(r.path,existed,bytes,ignored)&&existed&&bytes==r.png;}catch(...){ }
        bool restored=false;if(ours){if(r.existed)restored=install(s.backup,fs::u8path(r.path));else{std::error_code ec;restored=fs::remove(fs::u8path(r.path),ec)&&!ec;}}
        if(!restored){s.keep=r.existed;error+=" Earlier output could not be restored; "+(s.keep?"previous bytes kept at "+s.backup.u8string():"check "+r.path)+".";}
    }
    return false;
}
}
bool plan_png_batch(const Artwork &art,const std::vector<ExportRequest> &requests,const std::string &folder,const std::vector<std::string> &protected_paths,PngBatchPlan &out,std::string &error){try{return plan_impl(art,requests,folder,protected_paths,out,error);}catch(const std::exception &e){out={};error=std::string("Cannot review PNG export: ")+e.what();return false;}}
bool write_png_batch(const Artwork &art,const PngBatchPlan &p,std::string &error){try{return write_impl(art,p,error);}catch(const std::exception &e){error=std::string("Cannot export PNGs: ")+e.what();return false;}}
}
