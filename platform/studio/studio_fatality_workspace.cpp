#include "studio_fatality_workspace.h"
#include <filesystem>
#include <algorithm>
#include <cctype>
namespace studio {
namespace {
namespace fs=std::filesystem;
std::string key(std::string s){for(auto &c:s)c=(char)std::tolower((unsigned char)c);return s;}
bool same_path(const std::string &a,const std::string &b){
    auto left=fs::absolute(fs::u8path(a)).lexically_normal(),right=fs::absolute(fs::u8path(b)).lexically_normal();
#ifdef _WIN32
    if(key(left.u8string())==key(right.u8string()))return true;
#else
    if(left==right)return true;
#endif
    std::error_code ec;return fs::equivalent(left,right,ec)&&!ec;
}
}
std::shared_ptr<FatalitySource> FatalityWorkspace::document(uint64_t id)const{
    for(auto &entry:entries)if(entry.id==id)return entry.source;return {};
}
uint64_t FatalityWorkspace::find_path(const std::string &path)const{
    for(auto &entry:entries)if(same_path(entry.source->path(),path))return entry.id;return 0;
}
uint64_t FatalityWorkspace::add(const std::string &path,std::string &error){
    error.clear();try{
        if(auto found=find_path(path))return found;
        if(entries.size()>=limit){error="16 fatality sources are already open. Close one before adding another.";return 0;}
        auto source=FatalitySource::open(path,error);if(!source)return 0;return adopt(std::move(source),0,false,error);
    }catch(const std::exception &e){error=e.what();return 0;}
}
uint64_t FatalityWorkspace::adopt(std::unique_ptr<FatalitySource> source,uint64_t replace,bool discard,std::string &error){
    error.clear();try{
        auto previous=document(replace);
        if(!source||(replace&&!previous)){error="The source being replaced is no longer open.";return 0;}
        if(previous&&previous->dirty()&&!discard){error="Save or explicitly discard that source before replacing it.";return 0;}
        if(!destination_available(replace,source->path(),error))return 0;
        if(!replace&&entries.size()>=limit){error="16 fatality sources are already open. Close one before adding another.";return 0;}
        std::shared_ptr<FatalitySource> prepared=std::move(source);
        if(replace){for(auto &entry:entries)if(entry.id==replace){entry.source=std::move(prepared);++entry.epoch;return replace;}}
        entries.reserve(entries.size()+1);uint64_t id=next_id;entries.push_back({id,std::move(prepared)});++next_id;return id;
    }catch(const std::exception &e){error=e.what();return 0;}
}
bool FatalityWorkspace::close(uint64_t id,bool discard,std::string &error){
    error.clear();for(auto it=entries.begin();it!=entries.end();++it)if(it->id==id){
        if(it->source->dirty()&&!discard){error="Save or explicitly discard that source before closing it.";return false;}
        entries.erase(it);return true;
    }error="That source is no longer open.";return false;
}
uint64_t FatalityWorkspace::first_dirty()const{for(auto &entry:entries)if(entry.source->dirty())return entry.id;return 0;}
size_t FatalityWorkspace::dirty_count()const{size_t count=0;for(auto &entry:entries)count+=entry.source->dirty();return count;}
std::vector<FatalityMatch> FatalityWorkspace::matches(const std::string &label,FatalityKind kind,uint64_t preferred)const{
    std::vector<FatalityMatch> found;auto name=key(label);
    for(auto &entry:entries){auto &blocks=entry.source->blocks();for(int i=0;i<(int)blocks.size();++i)if(blocks[i].kind==kind&&key(blocks[i].label)==name)
        found.push_back({entry.id,entry.source->generation(),i,kind,blocks[i].label,entry.epoch});}
    size_t local=0;for(auto &match:found)local+=match.document==preferred;
    if(local)found.erase(std::remove_if(found.begin(),found.end(),[&](const FatalityMatch &match){return match.document!=preferred;}),found.end());
    return found;
}
bool FatalityWorkspace::resolve(const FatalityMatch &match,std::shared_ptr<FatalitySource> &source,std::string &error)const{
    error.clear();auto doc=document(match.document);uint64_t epoch=0;for(auto &entry:entries)if(entry.id==match.document)epoch=entry.epoch;
    if(!doc||epoch!=match.epoch||doc->generation()!=match.generation||match.block<0||match.block>=(int)doc->blocks().size()||doc->blocks()[match.block].kind!=match.kind||doc->blocks()[match.block].label!=match.label){error="The linked source changed. Refresh the combo choices.";return false;}
    source=std::move(doc);return true;
}
bool FatalityWorkspace::destination_available(uint64_t id,const std::string &path,std::string &error)const{
    for(auto &entry:entries)if(entry.id!=id&&same_path(entry.source->path(),path)){error="That path belongs to another open fatality source. Select that source and save it separately.";return false;}return true;
}
bool FatalityWorkspace::review_save(uint64_t id,const std::string &path,FatalityWriteReview &out,std::string &error)const{
    out={};error.clear();try{auto source=document(id);if(!source){error="That source is no longer open.";return false;}
        return destination_available(id,path,error)&&source->review_save(path,out,error);
    }catch(const std::exception &e){error=e.what();return false;}
}
bool FatalityWorkspace::save(uint64_t id,const FatalityWriteReview &review,std::string &error){
    error.clear();try{auto source=document(id);if(!source||review.owner!=source.get()){error="The reviewed source is no longer active in this workspace.";return false;}
        return destination_available(id,review.path,error)&&source->save(review,error);
    }catch(const std::exception &e){error=e.what();return false;}
}
}
