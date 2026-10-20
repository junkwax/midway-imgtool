#include "studio_project.h"
#include "studio_layers.h"
#include "stb_image_write.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <chrono>
#include <climits>
#include <set>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace studio {
namespace fs=std::filesystem;
namespace {
using Values=std::map<std::string,std::string>;
std::string lower(std::string text){for(char &c:text)if(c>='A'&&c<='Z')c+=32;return text;}
std::string number(int i){return std::to_string(i);}
std::string field(const char *prefix,int i,const char *name){return std::string(prefix)+"."+number(i)+"."+name;}
bool read_text(const std::string &path,std::string &out){
    std::ifstream file(fs::u8path(path),std::ios::binary|std::ios::ate);auto n=file?file.tellg():std::streampos(-1);
    if(n<0||n>8*1024*1024)return false;out.resize((size_t)n);file.seekg(0);return out.empty()||bool(file.read(out.data(),out.size()));
}
std::string file_digest(const std::string &path){
    std::ifstream f(fs::u8path(path),std::ios::binary);if(!f)return {};
    uint64_t hash=14695981039346656037ull;char bytes[8192];
    while(f.read(bytes,sizeof bytes)||f.gcount())for(std::streamsize i=0;i<f.gcount();++i){hash^=(unsigned char)bytes[i];hash*=1099511628211ull;}
    if(!f.eof())return {};std::ostringstream out;out<<std::hex<<std::setw(16)<<std::setfill('0')<<hash;return out.str();
}
Values parse_values(const std::string &text){
    Values result;std::istringstream input(text);std::string line;
    while(std::getline(input,line)){
        if(line.size()>65536)throw std::runtime_error("A project line is too long.");
        if(!line.empty()&&line.back()=='\r')line.pop_back();auto start=line.find_first_not_of(" \t");
        if(start==std::string::npos||line[start]=='#')continue;auto equal=line.find('=',start);
        if(equal==std::string::npos)throw std::runtime_error("Project lines must use key=value.");
        std::string key=line.substr(start,equal-start);while(!key.empty()&&(key.back()==' '||key.back()=='\t'))key.pop_back();
        if(key.empty()||result.count(key))throw std::runtime_error("Empty or duplicate project field: "+key);
        result.emplace(key,line.substr(equal+1));
    }return result;
}
std::string raw(const Values &v,const std::string &key,std::string fallback={}){auto it=v.find(key);return it==v.end()?fallback:it->second;}
int integer(const Values &v,const std::string &key,int fallback,int minimum=INT_MIN,int maximum=INT_MAX){
    auto it=v.find(key);if(it==v.end())return fallback;size_t used=0;long long n;
    try{n=std::stoll(it->second,&used);}catch(...){throw std::runtime_error("Invalid integer: "+key);}
    if(used!=it->second.size()||n<minimum||n>maximum)throw std::runtime_error("Out-of-range project field: "+key);return (int)n;
}
bool boolean(const Values &v,const std::string &key,bool fallback){return integer(v,key,fallback?1:0,0,1)!=0;}
double real(const Values &v,const std::string &key,double fallback){
    if(!v.count(key))return fallback;size_t used;double n;try{n=std::stod(v.at(key),&used);}catch(...){throw std::runtime_error("Invalid clock: "+key);}
    if(used!=v.at(key).size()||!std::isfinite(n)||n<1||n>240)throw std::runtime_error("Invalid clock: "+key);return n;
}
std::string quoted(const Values &v,const std::string &key,std::string fallback={}){
    if(!v.count(key))return fallback;const auto &text=v.at(key);std::string result;bool closed=false;size_t i=0;
    if(text.empty()||text[i++]!='"')throw std::runtime_error("Invalid quoted string: "+key);
    for(;i<text.size();++i){char c=text[i];if(c=='"'){closed=true;++i;break;}
        if(c=='\\'){if(++i==text.size())break;c=text[i];if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';else if(c!='\\'&&c!='"')throw std::runtime_error("Invalid escape: "+key);}
        if(!c)throw std::runtime_error("Invalid NUL in string: "+key);result+=c;
    }
    if(!closed||text.find_first_not_of(" \t",i)!=std::string::npos)throw std::runtime_error("Invalid quoted string: "+key);return result;
}
std::vector<int> integers(const Values &v,const std::string &key){
    std::vector<int> out;std::istringstream in(raw(v,key));std::string token;
    while(std::getline(in,token,',')){if(token.empty())throw std::runtime_error("Empty list value: "+key);Values one{{key,token}};out.push_back(integer(one,key,0));if(out.size()>8192)throw std::runtime_error("Too many frames: "+key);}return out;
}
void put(std::ostream &out,const std::string &key,const std::string &value){out<<key<<"=\"";for(char c:value){switch(c){case '\n':out<<"\\n";break;case '\r':out<<"\\r";break;case '\t':out<<"\\t";break;case '"':case '\\':out<<'\\'<<c;break;default:out<<c;}}out<<"\"\n";}
template<class T>void put(std::ostream &out,const std::string &key,T value){out<<key<<'='<<value<<'\n';}
std::string encode(const SceneState &s,bool history){
    int version=3;for(auto &lane:s.lanes)for(auto &frame:lane.frames)if(frame.ticks>120)version=4;
    for(auto &lane:s.lanes)if(lane.magnification!=1)version=5;
    for(auto &lane:s.lanes)for(auto &frame:lane.frames)if(frame.independent_timing)version=6;
    std::ostringstream out;out<<std::setprecision(17);out<<"format=imgtool_studio_project\nversion="<<version<<'\n';
    put(out,"world.w",s.width);put(out,"world.h",s.height);put(out,"world.origin_x",s.origin_x);put(out,"world.origin_y",s.origin_y);put(out,"world.floor",s.floor);
    put(out,"world.onion",s.onion);put(out,"world.loop",s.loop);put(out,"world.ghost_facing",s.ghost_facing);put(out,"world.convention",(int)s.convention);put(out,"world.tick_hz",s.tick_hz);
    put(out,"world.background_r",s.background[0]);put(out,"world.background_g",s.background[1]);put(out,"world.background_b",s.background[2]);
    put(out,"asset.count",s.assets.size());for(size_t i=0;i<s.assets.size();++i){auto &a=s.assets[i];std::string p="asset."+number((int)i)+".";
        if(history)put(out,p+"uid",a.runtime_doc);else{put(out,p+"path",a.path);put(out,p+"absolute",a.absolute_path);put(out,p+"digest",a.digest);}}
    put(out,"lane.count",s.lanes.size());for(size_t i=0;i<s.lanes.size();++i){auto &l=s.lanes[i];std::string p="lane."+number((int)i)+".";
        put(out,p+"name",l.name);put(out,p+"x",l.x);put(out,p+"y",l.y);put(out,p+"delay",l.delay);put(out,p+"alpha",l.alpha);put(out,p+"visible",l.visible);put(out,p+"loop",l.loop);put(out,p+"hold_end",l.hold_end);put(out,p+"flip",l.flip_x);
        put(out,p+"stop_tick",l.stop_tick);
        if(history||version>=5)put(out,p+"magnification",l.magnification);
        put(out,p+"id",l.id);put(out,p+"trigger_lane",l.trigger_lane);put(out,p+"trigger_frame",l.trigger_frame);
        put(out,p+"frame.count",l.frames.size());for(size_t f=0;f<l.frames.size();++f){auto &frame=l.frames[f];std::string q=p+"frame."+number((int)f)+".";
            put(out,q+"name",frame.name);put(out,q+"ticks",frame.ticks);put(out,q+"x",frame.x);put(out,q+"y",frame.y);put(out,q+"z",frame.z);put(out,q+"flip_x",frame.flip_x);put(out,q+"flip_y",frame.flip_y);
            put(out,q+"visible_from",frame.visible_from);put(out,q+"visible_until",frame.visible_until);
            if(history||version>=6)put(out,q+"independent_timing",frame.independent_timing);
            put(out,q+"vx",frame.vx);put(out,q+"vy",frame.vy);put(out,q+"cap_x",frame.cap_x);put(out,q+"cap_y",frame.cap_y);
            put(out,q+"dual",frame.dual);put(out,q+"dual_x",frame.dual_x);put(out,q+"dual_y",frame.dual_y);put(out,q+"dual_z",frame.dual_z);
            put(out,q+"id",frame.id);
            put(out,q+"piece.count",frame.pieces.size());for(size_t j=0;j<frame.pieces.size();++j){auto &ref=frame.pieces[j];std::string r=q+"piece."+number((int)j)+".";put(out,r+"asset",ref.asset);
                if(history)put(out,r+"id",ref.runtime_id);else{put(out,r+"index",ref.index);put(out,r+"key",ref.key);put(out,r+"name",ref.name);}}
        }
    }return out.str();
}
SceneState decode_native(const Values &v){
    int version=integer(v,"version",0);if(version<1||version>6)throw std::runtime_error("Unsupported Studio project version.");SceneState s;
    s.width=integer(v,"world.w",400,64,2048);s.height=integer(v,"world.h",254,64,2048);s.origin_x=integer(v,"world.origin_x",200,-4096,4096);s.origin_y=integer(v,"world.origin_y",20,-4096,4096);s.floor=integer(v,"world.floor",s.height,0,s.height);
    s.onion=boolean(v,"world.onion",true);s.loop=boolean(v,"world.loop",true);s.ghost_facing=boolean(v,"world.ghost_facing",false);s.convention=(MirrorConvention)integer(v,"world.convention",0,0,1);s.tick_hz=real(v,"world.tick_hz",mk2_tick_hz);
    s.background[0]=integer(v,"world.background_r",28,0,255);s.background[1]=integer(v,"world.background_g",32,0,255);s.background[2]=integer(v,"world.background_b",40,0,255);
    int assets=integer(v,"asset.count",0,1,64);for(int i=0;i<assets;++i)s.assets.push_back({quoted(v,field("asset",i,"path")),quoted(v,field("asset",i,"absolute")),quoted(v,field("asset",i,"digest")),0});
    int lanes=integer(v,"lane.count",0,1,32);size_t total=0,refs=0;
    for(int i=0;i<lanes;++i){SceneLane l;std::string p="lane."+number(i)+".";l.name=quoted(v,p+"name","Actor");l.x=integer(v,p+"x",0,-4096,4096);l.y=integer(v,p+"y",0,-4096,4096);l.delay=integer(v,p+"delay",0,0,100000);l.alpha=integer(v,p+"alpha",255,0,255);
        l.visible=boolean(v,p+"visible",true);l.loop=boolean(v,p+"loop",true);l.hold_end=boolean(v,p+"hold_end",true);l.flip_x=boolean(v,p+"flip",false);
        l.stop_tick=integer(v,p+"stop_tick",0,0,99999);
        l.magnification=integer(v,p+"magnification",1,1,4);
        if(version<5&&l.magnification!=1)throw std::runtime_error("Magnification requires Studio project version 5.");
        if(version>=3){l.id=integer(v,p+"id",0,1,INT_MAX);if(!l.id)throw std::runtime_error("A project lane is missing its identity.");l.trigger_lane=integer(v,p+"trigger_lane",0,0,INT_MAX);l.trigger_frame=integer(v,p+"trigger_frame",0,0,INT_MAX);}
        int frames=integer(v,p+"frame.count",0,1,4096);total+=frames;if(total>8192)throw std::runtime_error("A project can hold at most 8192 frames.");
        for(int f=0;f<frames;++f){SceneFrame item;std::string q=p+"frame."+number(f)+".";item.name=quoted(v,q+"name");item.ticks=integer(v,q+"ticks",4,1,version>=4?255:120);item.x=integer(v,q+"x",0,-4096,4096);item.y=integer(v,q+"y",0,-4096,4096);item.z=integer(v,q+"z",0,-4096,4096);item.flip_x=boolean(v,q+"flip_x",false);item.flip_y=boolean(v,q+"flip_y",false);
            item.visible_from=integer(v,q+"visible_from",0,0,99999);item.visible_until=integer(v,q+"visible_until",0,0,99999);
            item.independent_timing=boolean(v,q+"independent_timing",false);
            if(version<6&&item.independent_timing)throw std::runtime_error("Explicit independent timing requires Studio project version 6.");
            item.vx=integer(v,q+"vx",0,-128,128);item.vy=integer(v,q+"vy",0,-128,128);item.cap_x=integer(v,q+"cap_x",0,0,99999);item.cap_y=integer(v,q+"cap_y",0,0,99999);
            item.dual=boolean(v,q+"dual",false);item.dual_x=integer(v,q+"dual_x",0,-4096,4096);item.dual_y=integer(v,q+"dual_y",0,-4096,4096);item.dual_z=integer(v,q+"dual_z",0,-4096,4096);
            if(version>=3){item.id=integer(v,q+"id",0,1,INT_MAX);if(!item.id)throw std::runtime_error("A project frame is missing its identity.");}
            int pieces=integer(v,q+"piece.count",0,1,32);refs+=pieces;if(refs>65536)throw std::runtime_error("Too many project sprite references.");
            for(int j=0;j<pieces;++j){std::string r=q+"piece."+number(j)+".";SceneSprite ref;ref.asset=integer(v,r+"asset",0,0,assets-1);ref.index=integer(v,r+"index",-1,0,1999);ref.key=quoted(v,r+"key");ref.name=quoted(v,r+"name");
                if(ref.key.size()!=32||ref.key.find_first_not_of("0123456789abcdef")!=std::string::npos)throw std::runtime_error("Invalid sprite identity: "+r+"key");item.pieces.push_back(std::move(ref));}
            l.frames.push_back(std::move(item));
        }s.lanes.push_back(std::move(l));
    }return s;
}
SceneState decode_wax(const Values &v){
    if(integer(v,"version",0)!=1)throw std::runtime_error("Unsupported classic WAX version.");
    if(boolean(v,"asm.player.enabled",false)||boolean(v,"asm.opp.enabled",false)||boolean(v,"state.embedded_active",false)||boolean(v,"state.dummy_decap_body",false))throw std::runtime_error("This WAX uses ASM, embedded sequences or dummy-body lanes, which have not migrated yet. Open it in classic imgtool.");
    SceneState s;s.width=integer(v,"world.w",400,64,2048);s.height=integer(v,"world.h",254,64,2048);s.origin_x=integer(v,"world.origin_x",200,-4096,4096);s.origin_y=integer(v,"world.origin_y",20,-4096,4096);s.floor=s.height;s.onion=boolean(v,"world.onion",false);s.tick_hz=mk2_tick_hz;
    bool solid=boolean(v,"world.bg_solid",false);s.background[0]=solid?integer(v,"world.bg_solid_r",0,0,255):0;s.background[1]=solid?integer(v,"world.bg_solid_g",0,0,255):0;s.background[2]=solid?integer(v,"world.bg_solid_b",0,0,255):0;
    int docs=integer(v,"doc.count",0,1,128),slots=integer(v,"slot.count",0,1,64),hold=integer(v,"state.default_hold",4,1,120);
    auto add_asset=[&](int index,const std::string &path){std::string name=path.empty()?raw(v,field("doc",index,"path")):path;if(name.empty())throw std::runtime_error("A WAX lane is missing its IMG path.");
        for(int i=0;i<(int)s.assets.size();++i)if(s.assets[i].path==name)return i;s.assets.push_back({name,name,{ },0});if(s.assets.size()>64)throw std::runtime_error("Too many IMG dependencies.");return (int)s.assets.size()-1;};
    std::vector<int> row_order,saved_slots,spawn_source,spawn_entry;
    uint64_t next_id=1;
    for(int slot=0;slot<slots;++slot){std::string p="slot."+number(slot)+".";auto indices=integers(v,p+(v.count(p+"sequence_frames")?"sequence_frames":"default_frames"));if(indices.empty())continue;
        // auto_* and chain_* numeric fields are generator inputs, not live
        // animation. Their applied result is already in the per-frame arrays.
        for(const char *key:{"subframe_swap_tick","subframe_waterline_y"})if(integer(v,p+key,0)!=0)throw std::runtime_error("WAX lane "+number(slot)+" has "+key+" settings, but its complete generated piece lists are not stored in WAX. Open it in classic or rebuild its handoff in Studio.");
        if(integer(v,p+"subframe_fine_source",-1)!=-1)throw std::runtime_error("This WAX has substituted subframes. Open it in classic imgtool.");
        int doc=integer(v,p+"doc_idx",-1,-1,docs-1);std::string path=raw(v,p+"doc_path");
        int asset=doc>=0||!path.empty()?add_asset(doc,path):-1;SceneLane lane;lane.name="WAX row "+number(slot+1);lane.visible=boolean(v,p+"visible",true);lane.hold_end=boolean(v,p+"hold_end",true);lane.loop=!lane.hold_end;lane.flip_x=boolean(v,p+"mirror",false);
        lane.magnification=std::clamp(integer(v,p+"zoom",1),1,4); // Matches classic WorldMarkedLaneZoom.
        lane.stop_tick=integer(v,p+"stop_tick",0,0,99999);
        lane.id=next_id++;
        auto delays=integers(v,p+"frame_delays"),dx=integers(v,p+"local_dx"),dy=integers(v,p+"local_dy"),mirror=integers(v,p+"frame_mirror"),depth=integers(v,p+"frame_z"),overrides=integers(v,p+"frame_doc_idx");
        auto from=integers(v,p+"visible_from"),until=integers(v,p+"visible_until"),vx=integers(v,p+"motion_dx"),vy=integers(v,p+"motion_dy"),capx=integers(v,p+"motion_cap_x"),capy=integers(v,p+"motion_cap_y");
        auto dual=integers(v,p+"dual_on"),dualx=integers(v,p+"dual_dx"),dualy=integers(v,p+"dual_dy"),dualz=integers(v,p+"dual_z");
        int default_hold=boolean(v,p+"hold_custom",false)?integer(v,p+"hold",hold,1,120):hold;
        auto at=[](const std::vector<int> &a,size_t i,int fallback){return i<a.size()?a[i]:fallback;};
        for(size_t f=0;f<indices.size();++f){int index=indices[f],source=at(overrides,f,-1);if(index<0||index>1999||source>=docs||source<-1)throw std::runtime_error("Invalid WAX frame reference.");int bits=at(mirror,f,0);if(bits<0||bits>3)throw std::runtime_error("Invalid WAX mirror flags.");
            SceneFrame frame;frame.name="Frame "+number((int)f+1);frame.ticks=at(delays,f,default_hold);frame.z=at(depth,f,0);frame.flip_x=(bits&1)!=0;frame.flip_y=(bits&2)!=0;
            frame.id=next_id++;
            // Classic adjusts the anipoint before mirroring. Convert to our
            // absolute screen offsets using the entry's final facing.
            int local_x=at(dx,f,0),local_y=at(dy,f,0);if(local_x<-4096||local_x>4096||local_y<-4096||local_y>4096)throw std::runtime_error("WAX frame offsets exceed the Studio limit.");
            frame.x=(lane.flip_x!=frame.flip_x)?local_x:-local_x;frame.y=frame.flip_y?local_y:-local_y;
            int speed_x=at(vx,f,0),speed_y=at(vy,f,0);if(speed_x<-128||speed_x>128||speed_y<-128||speed_y>128)throw std::runtime_error("Invalid WAX motion speed.");
            frame.visible_from=at(from,f,0);frame.visible_until=at(until,f,0);frame.vx=(lane.flip_x!=frame.flip_x)?-speed_x:speed_x;frame.vy=frame.flip_y?-speed_y:speed_y;frame.cap_x=at(capx,f,0);frame.cap_y=at(capy,f,0);
            int enabled=at(dual,f,0);if(enabled!=0&&enabled!=1)throw std::runtime_error("Invalid WAX second-copy flag.");frame.dual=enabled!=0;
            int second_x=at(dualx,f,0),second_y=at(dualy,f,0);if(second_x<-4096||second_x>4096||second_y<-4096||second_y>4096)throw std::runtime_error("WAX second-copy offsets exceed the Studio limit.");
            frame.dual_x=(lane.flip_x!=frame.flip_x)?second_x:-second_x;frame.dual_y=frame.flip_y?second_y:-second_y;frame.dual_z=at(dualz,f,0);
            if(source<0&&asset<0)throw std::runtime_error("WAX row "+number(slot)+" frame "+number((int)f)+" has no source document.");
            frame.pieces.push_back({source<0?asset:add_asset(source,{}),index,{},{},0});lane.frames.push_back(std::move(frame));}
        int spawned=integer(v,p+"spawn_slot",-1,-1,slots-1);
        if(spawned>=0){
            if(lane.stop_tick)throw std::runtime_error("This WAX combines a spawn binding with a freeze tick. Detach that row in classic before importing it.");
            int base=lane.frames.front().visible_from,witness=integer(v,p+"spawn_tick",-1,-1,99999);
            int rate=integer(v,p+"hold",hold,1,120);
            if(witness>=0&&witness!=base)throw std::runtime_error("This WAX spawn schedule was moved by hand. Detach it in classic before importing it.");
            for(size_t f=0;f<lane.frames.size();++f){auto &frame=lane.frames[f];if(frame.visible_from!=base+(int)f*rate||frame.visible_until!=base+((int)f+1)*rate)throw std::runtime_error("This WAX spawn row has a custom schedule. Detach it in classic before importing it.");}
            for(auto &frame:lane.frames){frame.visible_from-=base;frame.visible_until-=base;}
        }
        saved_slots.push_back(slot);spawn_source.push_back(spawned);spawn_entry.push_back(integer(v,p+"spawn_entry",-1,-1,4095));
        row_order.push_back(integer(v,p+"order",slot,-1,99999));s.lanes.push_back(std::move(lane));
    }
    if(s.lanes.empty())throw std::runtime_error("This WAX has no explicit sprite rows to import.");
    for(size_t i=0;i<s.lanes.size();++i)if(spawn_source[i]>=0){auto found=std::find(saved_slots.begin(),saved_slots.end(),spawn_source[i]);if(found==saved_slots.end())throw std::runtime_error("A WAX spawn binding refers to a missing source row.");
        auto &source=s.lanes[found-saved_slots.begin()];if(spawn_entry[i]<0||spawn_entry[i]>=(int)source.frames.size())throw std::runtime_error("A WAX spawn binding refers to a missing source frame.");
        s.lanes[i].trigger_lane=source.id;s.lanes[i].trigger_frame=source.frames[spawn_entry[i]].id;
    }
    std::vector<int> order;for(int i=0;i<(int)s.lanes.size();++i)order.push_back(i);
    int next=0;for(int rank:row_order)next=std::max(next,rank+1);for(int &rank:row_order)if(rank<0)rank=next++;
    // Classic paints its first displayed row last at equal Z. Store lanes
    // back-to-front, which is the native evaluator's stable tie order.
    std::stable_sort(order.begin(),order.end(),[&](int a,int b){return row_order[a]>row_order[b];});
    static const int alpha[]={255,185,170,155,205,235,190,220,175,210};
    auto lanes=std::move(s.lanes);for(int i:order){auto lane=std::move(lanes[i]);int display=(int)order.size()-1-(int)s.lanes.size();lane.alpha=alpha[display%10];s.lanes.push_back(std::move(lane));}
    s.loop=false;for(int slot=0;slot<slots;++slot){auto p="slot."+number(slot)+".";
        if(!integers(v,p+(v.count(p+"sequence_frames")?"sequence_frames":"default_frames")).empty()&&boolean(v,p+"visible",true)&&!boolean(v,p+"hold_end",true)&&!boolean(v,p+"chain_pingpong",false)&&integer(v,p+"stop_tick",0)==0)s.loop=true;}
    return s;
}
std::vector<bool> referenced_assets(const SceneState &s){std::vector<bool> used(s.assets.size());for(auto &l:s.lanes)for(auto &f:l.frames)for(auto &r:f.pieces)used[r.asset]=true;return used;}
void compact_assets(SceneState &s){
    auto used=referenced_assets(s);std::vector<int> remap(s.assets.size(),-1);std::vector<SceneAsset> assets;
    for(int a=0;a<(int)s.assets.size();++a)if(used[a]){remap[a]=(int)assets.size();assets.push_back(s.assets[a]);}
    for(auto &l:s.lanes)for(auto &f:l.frames)for(auto &r:f.pieces)r.asset=remap[r.asset];s.assets=std::move(assets);
}
bool atomic_write(const fs::path &dest,const std::string &bytes,std::string &error){
    static unsigned counter=0;auto temp=dest;temp+=".studio-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+number(++counter)+".tmp";
#ifdef _WIN32
    HANDLE h=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(h==INVALID_HANDLE_VALUE){error="Cannot create a temporary file in this folder.";return false;}CloseHandle(h);
#else
    int fd=::open(temp.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0){error="Cannot create a temporary file in this folder.";return false;}close(fd);
#endif
    std::ofstream out(temp,std::ios::binary);out.write(bytes.data(),bytes.size());out.close();bool ok=!out.fail();
#ifdef _WIN32
    if(ok)ok=MoveFileExW(temp.c_str(),dest.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
    std::error_code ec;if(ok){fs::rename(temp,dest,ec);ok=!ec;}
#endif
    if(!ok){std::error_code ignored;fs::remove(temp,ignored);error="Could not finish writing the project. The destination was left intact.";}return ok;
}
}

int scene_frame_start(const SceneLane &lane,int frame){int ticks=lane.delay;for(int i=0;i<frame&&i<(int)lane.frames.size();++i)ticks+=lane.frames[i].ticks;return ticks;}
int scene_frame_seek_tick(const SceneLane &lane,int frame){
    if(frame<0||frame>=(int)lane.frames.size())return lane.delay;
    return lane.frames[frame].scheduled()?lane.delay+lane.frames[frame].visible_from:scene_frame_start(lane,frame);
}
namespace {
bool resolve_starts(const SceneState &scene,std::vector<int> &starts,std::string &error){
    starts.resize(scene.lanes.size());std::vector<int> visit(scene.lanes.size());
    std::function<bool(int)> resolve=[&](int index){
        if(visit[index]==2)return true;
        if(visit[index]==1){error="Effect timing cannot contain a cycle.";return false;}
        visit[index]=1;auto &lane=scene.lanes[index];starts[index]=lane.delay;
        if(lane.trigger_lane||lane.trigger_frame){
            int source=-1;for(int i=0;i<(int)scene.lanes.size();++i)if(scene.lanes[i].id==lane.trigger_lane&&lane.trigger_lane){source=i;break;}
            if(source<0){error="An effect refers to a missing source actor.";return false;}
            auto &parent=scene.lanes[source];int frame=-1;for(int i=0;i<(int)parent.frames.size();++i)if(parent.frames[i].id==lane.trigger_frame&&lane.trigger_frame){frame=i;break;}
            if(frame<0){error="An effect refers to a missing source frame.";return false;}
            if(!resolve(source))return false;
            starts[index]+=starts[source]+scene_frame_seek_tick(parent,frame)-parent.delay;
        }
        visit[index]=2;return true;
    };
    for(int i=0;i<(int)scene.lanes.size();++i)if(!resolve(i))return false;
    return true;
}
}
int scene_lane_start(const SceneState &scene,int lane){
    std::vector<int> starts;std::string error;
    return lane>=0&&lane<(int)scene.lanes.size()&&resolve_starts(scene,starts,error)?starts[lane]:-1;
}
int scene_frame_seek_tick(const SceneState &scene,int lane,int frame){
    int start=scene_lane_start(scene,lane);if(start<0)return 0;
    auto &actor=scene.lanes[lane];return start+scene_frame_seek_tick(actor,frame)-actor.delay;
}
bool scene_set_trigger(SceneState &scene,int lane,uint64_t source_lane,uint64_t source_frame,std::string &error){
    if(lane<0||lane>=(int)scene.lanes.size()){error="Choose an available effect actor.";return false;}
    auto &target=scene.lanes[lane];auto old_lane=target.trigger_lane,old_frame=target.trigger_frame;
    target.trigger_lane=source_lane;target.trigger_frame=source_frame;
    if(!validate_scene(scene,error)){target.trigger_lane=old_lane;target.trigger_frame=old_frame;return false;}
    return true;
}
bool scene_detach_trigger(SceneState &scene,int lane,std::string &error){
    int start=scene_lane_start(scene,lane);
    if(start<0){error="The effect has an unresolved source.";return false;}
    if(start>100000){error="The detached start exceeds 100000 ticks. Reduce the source timing or effect delay first.";return false;}
    auto &actor=scene.lanes[lane];actor.trigger_lane=actor.trigger_frame=0;actor.delay=start;error.clear();return true;
}
bool scene_uses_lane(const SceneState &scene,uint64_t lane){return lane&&std::any_of(scene.lanes.begin(),scene.lanes.end(),[&](const SceneLane &l){return l.trigger_lane==lane;});}
bool scene_uses_frame(const SceneState &scene,uint64_t frame){return frame&&std::any_of(scene.lanes.begin(),scene.lanes.end(),[&](const SceneLane &l){return l.trigger_frame==frame;});}
int scene_duration(const SceneState &scene){
    int duration=1;std::vector<int> starts;std::string error;if(!resolve_starts(scene,starts,error))return duration;
    for(size_t i=0;i<scene.lanes.size();++i){auto &lane=scene.lanes[i];if(!lane.visible)continue;
        int span=starts[i]+scene_frame_start(lane,(int)lane.frames.size())-lane.delay;
        for(auto &frame:lane.frames){span=std::max(span,starts[i]+frame.visible_from+1);span=std::max(span,starts[i]+frame.visible_until);}
        if(lane.stop_tick>0)span=std::max(span,starts[i]+lane.stop_tick+1);
        duration=std::max(duration,span);
    }
    return duration;
}
int scene_last_tick(const SceneState &scene){return scene_duration(scene)-(scene.loop?1:0);}
namespace {
int actor_tick(const SceneLane &lane,int tick){
    if(tick<lane.delay)return -1;
    int local=tick-lane.delay;
    return lane.stop_tick>0?std::min(local,lane.stop_tick):local;
}
int frame_at_actor_tick(const SceneLane &lane,int tick){
    if(lane.frames.empty()||tick<0)return -1;
    int duration=0;for(auto &frame:lane.frames)duration+=frame.ticks;
    if(duration<1)return -1;
    if(lane.loop)tick%=duration;
    else if(tick>=duration)return lane.hold_end?(int)lane.frames.size()-1:-1;
    for(int f=0;f<(int)lane.frames.size();++f){if(tick<lane.frames[f].ticks)return f;tick-=lane.frames[f].ticks;}
    return -1;
}
bool frame_visible(const SceneFrame &frame,int tick){return tick>=frame.visible_from&&(!frame.visible_until||tick<frame.visible_until);}
int moving_offset(int offset,int velocity,int elapsed,int cap,bool flip){
    int64_t travel=(int64_t)velocity*elapsed;
    if(cap>0)travel=std::clamp(travel,-(int64_t)cap,(int64_t)cap);
    // The original WAX motion clamps its adjusted signed anipoint. Converted
    // screen offsets have the inverted bound on an unflipped axis.
    return (int)std::clamp((int64_t)offset+travel,(int64_t)(flip?-32768:-32767),(int64_t)(flip?32767:32768));
}
}
int scene_frame_at(const SceneLane &lane,int tick){
    return frame_at_actor_tick(lane,actor_tick(lane,tick));
}
int scene_frame_at(const SceneState &scene,int lane,int tick){
    int start=scene_lane_start(scene,lane);if(start<0||tick<start)return -1;
    auto &actor=scene.lanes[lane];int local=tick-start;if(actor.stop_tick>0)local=std::min(local,actor.stop_tick);
    return frame_at_actor_tick(actor,local);
}
std::vector<ScenePose> evaluate_scene(const SceneState &scene,int tick){
    std::vector<ScenePose> poses;std::vector<int> starts;std::string error;if(!resolve_starts(scene,starts,error))return poses;
    for(int i=0;i<(int)scene.lanes.size();++i){
        auto &lane=scene.lanes[i];if(!lane.visible)continue;
        if(tick<starts[i])continue;int local=tick-starts[i];if(lane.stop_tick>0)local=std::min(local,lane.stop_tick);
        int current=frame_at_actor_tick(lane,local);
        auto emit=[&](int f){
            auto &frame=lane.frames[f];if(!frame_visible(frame,local))return;
            bool flip_x=lane.flip_x!=frame.flip_x,flip_y=frame.flip_y;
            int start=frame.scheduled()?frame.visible_from:scene_frame_start(lane,f)-lane.delay;
            int elapsed=std::max(0,local-start);
            auto copy=[&](bool dual){
                int x=moving_offset(dual?frame.dual_x:frame.x,frame.vx,elapsed,frame.cap_x,flip_x);
                int y=moving_offset(dual?frame.dual_y:frame.y,frame.vy,elapsed,frame.cap_y,flip_y);
                int scale=std::clamp(lane.magnification,1,4);
                for(auto &piece:frame.pieces)poses.push_back({piece,scene.origin_x+lane.x+x*scale,scene.origin_y+lane.y+y*scale,dual?frame.dual_z:frame.z,lane.alpha,i,f,flip_x,flip_y,dual,scale});
            };
            copy(false);if(frame.dual)copy(true);
        };
        // Scheduled entries coexist, even when the sequential cursor is on
        // another entry. Current paints last within a lane at equal depth.
        for(int f=0;f<(int)lane.frames.size();++f)if(f!=current&&lane.frames[f].scheduled())emit(f);
        if(current>=0)emit(current);
    }
    std::stable_sort(poses.begin(),poses.end(),[](const ScenePose &a,const ScenePose &b){return a.z<b.z;});return poses;
}
std::string scene_sprite_key(const IMG &image){
    unsigned char name[16];memcpy(name,image.file_name_raw,16);size_t n=strnlen(image.n_s,15);memcpy(name,image.n_s,n);name[n]=0;
    const char *hex="0123456789abcdef";std::string out;for(auto b:name){out+=hex[b>>4];out+=hex[b&15];}return out;
}
SpritePlacement scene_pose_placement(const IMG &im,const ScenePose &pose,MirrorConvention convention){
    int scale=std::clamp(pose.magnification,1,4);
    return {pose.x-anipoint_effective((short)im.anix,im.w,pose.flip_x,convention)*scale,
            pose.y-anipoint_effective((short)im.aniy,im.h,pose.flip_y,convention)*scale,
            im.w*scale,im.h*scale};
}
SceneSprite scene_sprite_reference(const Artwork &art,int asset,int index){auto im=art.sprite(index);if(!im)return {asset,index,{},{},0};return {asset,index,scene_sprite_key(*im),im->n_s,art.sprite_id(index)};}
std::string scene_asset_candidate(const SceneAsset &asset,const std::string &project_path){
    std::error_code ec;fs::path dir=fs::u8path(project_path).parent_path();
    std::vector<fs::path> candidates{dir/fs::u8path(asset.path),fs::u8path(asset.absolute_path)};
    auto sep=asset.path.find_last_of("/\\");candidates.push_back(dir/fs::u8path(sep==std::string::npos?asset.path:asset.path.substr(sep+1)));
    for(auto &path:candidates)if(!path.empty()&&fs::is_regular_file(path,ec))return fs::absolute(path,ec).lexically_normal().u8string();return {};
}
bool validate_scene(const SceneState &s,std::string &error){
    for(int c:s.background)if(c<0||c>255){error="Invalid scene background color.";return false;}
    if(s.width<64||s.width>2048||s.height<64||s.height>2048||s.floor<0||s.floor>s.height||std::abs((long long)s.origin_x)>4096||std::abs((long long)s.origin_y)>4096||!std::isfinite(s.tick_hz)||s.tick_hz<1||s.tick_hz>240||s.convention<0||s.convention>1){error="Invalid playfield or clock settings.";return false;}
    if(s.assets.empty()||s.assets.size()>64||s.lanes.empty()||s.lanes.size()>32){error="A project needs 1–64 IMG sources and 1–32 lanes.";return false;}
    std::set<uint64_t> identities;
    auto identity=[&](uint64_t id){return id<=INT_MAX&&(!id||identities.insert(id).second);};
    size_t frames=0,pieces=0;for(auto &lane:s.lanes){
        if(!identity(lane.id)){error="Duplicate or invalid actor identity.";return false;}
        if(lane.name.size()>128||lane.frames.empty()||lane.frames.size()>4096||lane.delay<0||lane.delay>100000||lane.stop_tick<0||lane.stop_tick>99999||lane.alpha<0||lane.alpha>255||lane.magnification<1||lane.magnification>4||std::abs((long long)lane.x)>4096||std::abs((long long)lane.y)>4096){error="Invalid lane settings, magnification or frame count.";return false;}
        frames+=lane.frames.size();for(auto &f:lane.frames){if(f.ticks<1||f.ticks>255||f.pieces.empty()||f.pieces.size()>32||f.name.size()>128||std::abs((long long)f.x)>4096||std::abs((long long)f.y)>4096||std::abs((long long)f.z)>4096){error="Invalid frame duration, placement or pieces.";return false;}
            if(!identity(f.id)){error="Duplicate or invalid frame identity.";return false;}
            if(f.visible_from<0||f.visible_from>99999||f.visible_until<0||f.visible_until>99999||std::abs((long long)f.vx)>128||std::abs((long long)f.vy)>128||f.cap_x<0||f.cap_x>99999||f.cap_y<0||f.cap_y>99999||std::abs((long long)f.dual_x)>4096||std::abs((long long)f.dual_y)>4096||std::abs((long long)f.dual_z)>4096){error="Invalid visibility, motion or second-copy settings.";return false;}
            pieces+=f.pieces.size();for(auto &ref:f.pieces)if(ref.asset<0||ref.asset>=(int)s.assets.size()){error="A frame refers to an unknown IMG source.";return false;}}
    }if(frames>8192||pieces>65536){error="This project exceeds the frame/reference limits.";return false;}
    std::vector<int> starts;if(!resolve_starts(s,starts,error))return false;error.clear();return true;
}
SceneProject::SceneProject(SceneState initial,bool unsaved):state(std::move(initial)){
    for(auto &lane:state.lanes){next_entity_id=std::max(next_entity_id,lane.id+1);for(auto &frame:lane.frames)next_entity_id=std::max(next_entity_id,frame.id+1);}
    for(auto &lane:state.lanes){if(!lane.id)lane.id=allocate_id();for(auto &frame:lane.frames)if(!frame.id)frame.id=allocate_id();}
    if(unsaved)revision=next_revision++;
}
std::unique_ptr<SceneProject> SceneProject::read(const std::string &path,std::string &error){
    error.clear();std::string text;if(!read_text(path,text)){error="Could not read the project (limit: 8 MiB).";return {};}
    try{auto values=parse_values(text);bool legacy=raw(values,"format")=="imgtool_world_project";
        if(!legacy&&raw(values,"format")!="imgtool_studio_project")throw std::runtime_error("Not an imgtool Studio or classic WAX project.");
        SceneState scene=legacy?decode_wax(values):decode_native(values);if(!validate_scene(scene,error))return {};compact_assets(scene);
        auto p=std::make_unique<SceneProject>(std::move(scene),legacy);p->imported=legacy;p->original_path=fs::absolute(fs::u8path(path)).lexically_normal().u8string();
        if(!legacy){p->file_path=p->original_path;p->disk_baseline=text;}return p;
    }catch(const std::exception &e){error=e.what();return {};}
}
bool SceneProject::bind(const SceneBinder &provider,std::vector<SceneIssue> &issues){
    issues.clear();for(int a=0;a<(int)state.assets.size();++a){auto binding=provider(a,state.assets[a]);if(!binding.art){issues.push_back({a,"Could not load IMG: "+state.assets[a].path});continue;}
        auto &asset=state.assets[a];asset.runtime_doc=binding.uid;bool unchanged=!binding.art->dirty()&&!asset.digest.empty()&&file_digest(binding.art->path())==asset.digest;
        for(auto &lane:state.lanes)for(auto &frame:lane.frames)for(auto &ref:frame.pieces)if(ref.asset==a){
            int index=-1;if(imported&&ref.key.empty())index=ref.index;
            else{std::vector<int> matches;for(int i=0;i<binding.art->count();++i)if(scene_sprite_key(*binding.art->sprite(i))==ref.key)matches.push_back(i);
                if(matches.size()==1)index=matches.front();else if(unchanged&&std::find(matches.begin(),matches.end(),ref.index)!=matches.end())index=ref.index;}
            if(!binding.art->sprite(index)){issues.push_back({a,"Missing or ambiguous sprite: "+(ref.name.empty()?number(ref.index):ref.name)});continue;}
            ref=scene_sprite_reference(*binding.art,a,index);if(frame.name.empty()||imported)frame.name=ref.name;
        }
    }return issues.empty();
}
void SceneProject::establish_loaded(){pending.reset();past.clear();future.clear();revision=imported?next_revision++:0;saved_revision=0;}
bool SceneProject::same(const SceneState &a,const SceneState &b){return encode(a,true)==encode(b,true);}
bool SceneProject::uses_doc(uint64_t uid) const{
    auto uses=[&](const SceneState &s){auto used=referenced_assets(s);for(size_t a=0;a<s.assets.size();++a)if(used[a]&&s.assets[a].runtime_doc==uid)return true;return false;};
    if(uses(state)||(pending&&uses(pending->state)))return true;for(auto &s:past)if(uses(s.state))return true;for(auto &s:future)if(uses(s.state))return true;return false;
}
void SceneProject::begin(const std::string &label){if(!pending)pending=std::make_unique<Snapshot>(Snapshot{state,revision,label});}
void SceneProject::commit(){if(!pending)return;if(!same(state,pending->state)){past.push_back(std::move(*pending));future.clear();revision=next_revision++;size_t size=0;for(auto &p:past)size+=encode(p.state,true).size();while(past.size()>1&&(past.size()>64||size>64*1024*1024)){size-=encode(past.front().state,true).size();past.erase(past.begin());}}pending.reset();}
void SceneProject::cancel(){if(pending){state=std::move(pending->state);revision=pending->revision;pending.reset();}}
void SceneProject::undo(){commit();if(past.empty())return;future.push_back({state,revision,past.back().label});state=std::move(past.back().state);revision=past.back().revision;past.pop_back();}
void SceneProject::redo(){commit();if(future.empty())return;past.push_back({state,revision,future.back().label});state=std::move(future.back().state);revision=future.back().revision;future.pop_back();}
bool SceneProject::save(const std::string &path,const SceneSource &source,std::string &error){
    commit();error.clear();if(!validate_scene(state,error))return false;std::error_code ec;auto dest=fs::absolute(fs::u8path(path),ec).lexically_normal();
    if(ec||lower(dest.extension().u8string())!=".istudio"){error="Save Studio projects with an .ISTUDIO extension. Classic WAX imports are saved as a separate Studio project.";return false;}
    std::string current;if(!file_path.empty()&&(dest==fs::u8path(file_path)||fs::equivalent(dest,fs::u8path(file_path),ec))&&(!read_text(file_path,current)||current!=disk_baseline)){error="This project changed on disk. Save As a new project or reopen it.";return false;}
    SceneState written=state;
    auto used=referenced_assets(written);
    for(int a=0;a<(int)written.assets.size();++a){if(!used[a])continue;auto &asset=written.assets[a];auto art=source(asset.runtime_doc);
        if(!art||art->path().empty()||art->dirty()||!art->disk_matches()){error="Save the referenced IMG artwork first: "+(art&&!art->path().empty()?fs::u8path(art->path()).filename().u8string():"unsaved or closed library");return false;}
        if(fs::equivalent(dest,fs::u8path(art->path()),ec)){error="A project cannot replace its source IMG.";return false;}
        asset.absolute_path=art->path();auto relative=fs::relative(fs::u8path(art->path()),dest.parent_path(),ec);asset.path=ec?art->path():relative.generic_u8string();asset.digest=file_digest(art->path());
        if(asset.digest.empty()){error="Could not read referenced IMG: "+art->path();return false;}
        for(auto &lane:written.lanes)for(auto &frame:lane.frames)for(auto &ref:frame.pieces)if(ref.asset==a){int index=art->sprite_index(ref.runtime_id);if(index<0){error="A referenced sprite was deleted. Replace that project frame before saving.";return false;}ref=scene_sprite_reference(*art,a,index);}
    }
    auto serialized=written;compact_assets(serialized);std::string bytes=encode(serialized,false);if(bytes.size()>8*1024*1024){error="The project exceeds the 8 MiB file limit.";return false;}
    if(!atomic_write(dest,bytes,error))return false;state=std::move(written);file_path=dest.u8string();disk_baseline=std::move(bytes);saved_revision=revision;imported=false;return true;
}
bool SceneProject::export_png(const std::string &path,int tick,const SceneSource &source,std::string &error,const StageBackdrop *stage,const StageView *view) const{
    error.clear();if(!validate_scene(state,error))return false;std::error_code ec;auto dest=fs::absolute(fs::u8path(path),ec);
    if(ec||lower(dest.extension().u8string())!=".png"){error="Choose a .png filename.";return false;}
    std::vector<unsigned char> rgba((size_t)state.width*state.height*4);for(size_t i=0;i<rgba.size();i+=4){rgba[i]=(unsigned char)state.background[0];rgba[i+1]=(unsigned char)state.background[1];rgba[i+2]=(unsigned char)state.background[2];rgba[i+3]=255;}
    if(stage&&view){for(auto &p:{stage->bdd_path,stage->bdb_path})if(fs::equivalent(dest,fs::u8path(p),ec)){error="PNG export cannot replace source scenery.";return false;}
        if(!blit_stage(*stage,*view,state.width,state.height,rgba,error))return false;}
    for(auto &pose:evaluate_scene(state,tick)){auto art=source(state.assets[pose.sprite.asset].runtime_doc);int index=art?art->sprite_index(pose.sprite.runtime_id):-1;auto im=art?art->sprite(index):nullptr;
        if(!im){error="A scene source or sprite is missing.";return false;}checked_layer_bytes(*im);auto pal=art->palette(im->palnum);auto p=scene_pose_placement(*im,pose,state.convention);
        for(int y=std::max(0,p.y);y<std::min(state.height,p.y+p.h);++y)for(int x=std::max(0,p.x);x<std::min(state.width,p.x+p.w);++x){int sx=(x-p.x)/pose.magnification,sy=(y-p.y)/pose.magnification;if(pose.flip_x)sx=im->w-1-sx;if(pose.flip_y)sy=im->h-1-sy;
            int ci=visible_pixel(*im,sx,sy);if(!ci)continue;unsigned char color[3]={255,0,255};if(ci<pal->numc)pal_word_to_rgb8((unsigned char *)pal->data_p+ci*2,color,color+1,color+2);
            auto target=rgba.data()+((size_t)y*state.width+x)*4;for(int channel=0;channel<3;++channel)target[channel]=(unsigned char)((color[channel]*pose.alpha+target[channel]*(255-pose.alpha)+127)/255);
        }
    }
    std::vector<unsigned char> png;auto write=[](void *context,void *bytes,int n){auto &out=*(std::vector<unsigned char> *)context;out.insert(out.end(),(unsigned char *)bytes,(unsigned char *)bytes+n);};
    if(!stbi_write_png_to_func(write,&png,state.width,state.height,4,rgba.data(),state.width*4)){error="Could not encode scene PNG.";return false;}
    for(auto &asset:state.assets)if(auto art=source(asset.runtime_doc))if(!art->path().empty()&&fs::equivalent(dest,fs::u8path(art->path()),ec)){error="PNG export cannot replace source artwork.";return false;}
    return atomic_write(dest,std::string((char *)png.data(),png.size()),error);
}
}
