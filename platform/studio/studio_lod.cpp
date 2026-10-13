#include "studio_lod.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <exception>

namespace studio {
namespace {
namespace fs=std::filesystem;
std::string trim(std::string s){auto first=s.find_first_not_of(" \t\r\n");return first==std::string::npos?std::string():s.substr(first,s.find_last_not_of(" \t\r\n")-first+1);}
std::string upper(std::string s){for(char &c:s)if(c>='a'&&c<='z')c-=32;return s;}
fs::path portable(std::string s){std::replace(s.begin(),s.end(),'\\','/');return fs::u8path(s);}
bool read_manifest(const std::string &path,std::vector<unsigned char> &bytes){std::ifstream f(fs::u8path(path),std::ios::binary|std::ios::ate);auto size=f.tellg();if(!f||size<0||size>1024*1024)return false;bytes.resize((size_t)size);f.seekg(0);return bytes.empty()||bool(f.read((char *)bytes.data(),size));}
bool file(const fs::path &p){std::error_code ec;return fs::is_regular_file(p,ec);}
bool same(const std::string &a,const std::string &b){std::error_code ec;if(fs::equivalent(fs::u8path(a),fs::u8path(b),ec))return true;
#ifdef _WIN32
    return upper(a)==upper(b);
#else
    return a==b;
#endif
}
void resolve(LodLibrary &row,const fs::path &folder,const fs::path &override_dir,const fs::path &imgdir){
    auto raw=portable(row.source);std::vector<std::pair<fs::path,const char *>> choices;
    if(!override_dir.empty())choices.push_back({override_dir/raw.filename(),"Override folder"});
    else if(raw.is_absolute())choices.push_back({raw,"As written"});
    else {choices.push_back({folder/raw,"Manifest folder"});choices.push_back({fs::absolute(raw),"As written"});if(!imgdir.empty())choices.push_back({imgdir/raw,"IMGDIR"});}
    for(auto &choice:choices)if(file(choice.first)){row.path=fs::absolute(choice.first).lexically_normal().u8string();row.resolution=choice.second;return;}
    row.path=fs::absolute(choices.front().first).lexically_normal().u8string();row.resolution=choices.front().second;row.error="File not found. Choose an override folder or exclude this row.";
}
}
bool review_lod(const std::string &path,const std::string &override_directory,const std::string &img_directory,LodReview &review,std::string &error){
    review={};error.clear();LodReview next;
    try {
        next.path=fs::absolute(fs::u8path(path)).lexically_normal().u8string();next.override_directory=override_directory;next.img_directory=img_directory;
        fs::path override_dir,imgdir;if(!override_directory.empty()){override_dir=fs::absolute(portable(override_directory));std::error_code ec;if(!fs::is_directory(override_dir,ec)){error="The override folder does not exist.";return false;}}if(!img_directory.empty())imgdir=fs::absolute(portable(img_directory));
        if(!read_manifest(next.path,next.source_bytes)){error="Could not read this LOD manifest (limit: 1 MiB).";return false;}
        auto &bytes=next.source_bytes;if(std::find(bytes.begin(),bytes.end(),0)!=bytes.end()||(!bytes.empty()&&(bytes[0]==0xff||bytes[0]==0xfe))){error="Use a UTF-8 or ASCII LOD manifest, without NUL bytes.";return false;}
        size_t start=bytes.size()>=3&&bytes[0]==0xef&&bytes[1]==0xbb&&bytes[2]==0xbf?3:0;std::string content(bytes.begin()+start,bytes.end());std::istringstream stream(content);std::string line;int number=0,ppp=0;bool explicit_ppp=false;
        const std::vector<std::string> keywords={"--->","***>","GLO>","ASM>","ROM>","ZON>","ZOF>","CON>","COF>","PON>","POF>","XON>","XOF>","FRM>","BBB>","UFN>","UGL>","UNI>","IHDR","MON>","MOF>","BON>","BOF>","ZAL>","USQ>","LOADS>","OF>","ADDRESS>"};
        while(std::getline(stream,line)){
            ++number;auto fail=[&](const std::string &why){error="Line "+std::to_string(number)+": "+why;return false;};if(line.size()>8192)return fail("Line is too long (limit: 8192 bytes).");line=trim(line);if(line.empty()||line[0]==';'||line.rfind("//",0)==0||line.rfind("/ ",0)==0||line.rfind("/*",0)==0||line=="/")continue;
            bool quoted=false;size_t comment=std::string::npos;for(size_t i=0;i<line.size();++i){if(line[i]=='\"')quoted=!quoted;if(line[i]==';'&&!quoted){comment=i;break;}}if(comment!=std::string::npos)line=trim(line.substr(0,comment));if(line.empty())continue;
            auto key=upper(line);if(key.rfind("PPP>",0)==0){std::istringstream value(line.substr(4));std::string extra;if(!(value>>ppp)||ppp<0||ppp>8||(value>>extra))return fail("PPP must be an integer from 0 (auto) through 8.");explicit_ppp=true;continue;}
            bool directive=false;for(auto &kw:keywords)if(key.rfind(kw,0)==0){next.directives.push_back({number,line});directive=true;break;}if(directive)continue;
            if(line.find('>')!=std::string::npos)return fail("Unknown LOAD2 directive: "+line);
            if(line.front()=='\"'){if(line.size()<2||line.back()!='\"')return fail("Unclosed library path quote.");line=line.substr(1,line.size()-2);}else if(line.find('"')!=std::string::npos)return fail("Quote the entire library path.");
            if(upper(portable(line).extension().u8string())!=".IMG")return fail("Expected an IMG library path or a LOAD2 directive.");if(next.libraries.size()>=128)return fail("Too many library entries (limit: 128).");
            LodLibrary row;row.line=number;row.source=line;row.ppp=ppp;row.explicit_ppp=explicit_ppp;resolve(row,fs::u8path(next.path).parent_path(),override_dir,imgdir);
            for(size_t i=0;i<next.libraries.size();++i)if(same(row.path,next.libraries[i].path)){row.duplicate_of=(int)i;row.enabled=false;row.error=row.ppp==next.libraries[i].ppp?"Repeated library; opens once.":"Repeated library with different PPP; first occurrence supplies the tab setting.";break;}
            next.libraries.push_back(std::move(row));
        }
        if(next.libraries.empty()){error="No IMG libraries were found in this manifest.";return false;}
        size_t decoded=0;
        for(auto &row:next.libraries){if(!row.error.empty())continue;std::error_code ec;auto size=fs::file_size(fs::u8path(row.path),ec);if(ec||size>64*1024*1024){row.error="Cannot stage this file (per-library limit: 64 MiB).";continue;}
            auto art=Artwork::open(row.path,row.error);if(!art)continue;size_t cost=0;for(int i=0;i<art->count();++i){auto im=art->sprite(i);cost+=(size_t)((im->w+3)&~3)*im->h*2;}if(decoded+cost>128*1024*1024){row.error="Review memory limit reached (128 MiB of sprite pixels). Open this library separately.";continue;}decoded+=cost;
            if(!art->disk_matches()){row.error="Library changed while reading. Rebuild the review.";continue;}row.artwork=std::move(art);
        }
        review=std::move(next);return true;
    } catch(const std::exception &e){error=std::string("Could not review LOD: ")+e.what();return false;}
}
bool validate_lod(const LodReview &review,std::vector<size_t> &included,std::string &error){
    included.clear();error.clear();try{std::vector<unsigned char> bytes;if(!read_manifest(review.path,bytes)||bytes!=review.source_bytes){error="The manifest changed on disk. Rebuild the review.";return false;}
        std::vector<size_t> ready;for(size_t i=0;i<review.libraries.size();++i){auto &r=review.libraries[i];if(!r.enabled)continue;if(!r.artwork||r.duplicate_of>=0||!r.error.empty()){error="Line "+std::to_string(r.line)+": resolve or exclude "+r.source+" before opening.";return false;}if(!r.artwork->disk_matches()){error=r.source+" changed on disk. Rebuild the review.";return false;}ready.push_back(i);}
        if(ready.empty()){error="Include at least one library.";return false;}included=std::move(ready);return true;
    }catch(const std::exception &e){error=std::string("Could not validate LOD: ")+e.what();return false;}
}
}
