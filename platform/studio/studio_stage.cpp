#include "studio_stage.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
namespace studio {
namespace {
namespace fs=std::filesystem;
struct Cursor {
    std::vector<unsigned char> bytes;size_t pos=0;
    explicit Cursor(const fs::path &path){
        std::ifstream in(path,std::ios::binary|std::ios::ate);auto n=in?in.tellg():std::streampos(-1);
        if(n<0||n>64*1024*1024)throw std::runtime_error("Cannot read stage file (64 MiB limit): "+path.u8string());
        bytes.resize((size_t)n);in.seekg(0);if(n&&!in.read((char *)bytes.data(),n))throw std::runtime_error("Could not finish reading stage file.");
    }
    std::string line(){
        while(pos<bytes.size()&&(bytes[pos]=='\r'||bytes[pos]=='\n'))++pos;
        std::string out;while(pos<bytes.size()&&bytes[pos]!='\n'){char c=(char)bytes[pos++];if(c=='\r')continue;
            if(!c||out.size()>=511)throw std::runtime_error("Invalid or oversized stage header.");out+=c;}
        if(pos<bytes.size())++pos;return out;
    }
    std::vector<unsigned char> block(size_t n){
        if(n>bytes.size()-pos)throw std::runtime_error("Truncated stage pixel or palette block.");
        std::vector<unsigned char> out(bytes.begin()+pos,bytes.begin()+pos+n);pos+=n;return out;
    }
};
std::vector<std::string> words(const std::string &line){std::istringstream in(line);std::vector<std::string> out;std::string s;while(in>>s)out.push_back(s);return out;}
int number(const std::string &s,int base=10,int lo=-1000000,int hi=1000000){size_t used=0;long long n;
    try{n=std::stoll(s,&used,base);}catch(...){throw std::runtime_error("Invalid stage number: "+s);}
    if(used!=s.size()||n<lo||n>hi)throw std::runtime_error("Stage number outside supported range: "+s);return (int)n;
}
std::string lower(std::string s){for(char &c:s)if(c>='A'&&c<='Z')c+=32;return s;}
fs::path sibling(fs::path path,const char *upper,const char *small){std::error_code ec;path.replace_extension(upper);if(fs::is_regular_file(path,ec))return path;
    path.replace_extension(small);if(fs::is_regular_file(path,ec))return path;throw std::runtime_error("Missing matching "+std::string(upper)+" file. A stage needs both BDD pixels and BDB placements.");}
struct Image {int w=0,h=0;std::vector<unsigned char> pixels;};
struct Object {int flags=0,x=0,y=0,image=0,palette=0;};
}
std::shared_ptr<const StageBackdrop> read_stage(const std::string &path,std::string &error){
    error.clear();try{
        auto given=fs::absolute(fs::u8path(path)).lexically_normal();auto ext=lower(given.extension().u8string());
        if(ext!=".bdd"&&ext!=".bdb")throw std::runtime_error("Choose a .BDD or .BDB stage file.");
        auto bdd=ext==".bdd"?given:sibling(given,".BDD",".bdd");auto bdb=ext==".bdb"?given:sibling(given,".BDB",".bdb");
        Cursor art(bdd),layout(bdb);auto result=std::make_shared<StageBackdrop>();result->bdd_path=bdd.u8string();result->bdb_path=bdb.u8string();
        auto fields=words(layout.line());if(fields.size()<3||fields.size()>7)throw std::runtime_error("Invalid BDB stage header.");
        result->name=fields[0];result->world_w=number(fields[1],10,1,1000000);result->world_h=number(fields[2],10,1,1000000);
        int modules=fields.size()>4?number(fields[4],10,0,256):0,count=fields.size()>6?number(fields[6],10,-1,16384):-1;
        for(int m=0;m<modules;++m){auto f=words(layout.line());if(f.size()!=5)throw std::runtime_error("Invalid BDB plane header.");StagePlane p;p.name=f[0];p.x1=number(f[1]);p.x2=number(f[2]);p.y1=number(f[3]);p.y2=number(f[4]);
            if(p.x2<p.x1||p.y2<p.y1)throw std::runtime_error("Inverted stage plane bounds.");result->planes.push_back(p);}
        std::vector<Object> objects;while(true){auto line=layout.line();if(line.empty())break;auto f=words(line);if(f.size()!=5||objects.size()>=16384)throw std::runtime_error("Invalid BDB placement or too many objects.");
            objects.push_back({number(f[0],16,0,65535),number(f[1]),number(f[2]),number(f[3],16,0,65535),number(f[4],10,0,255)});}
        if(objects.empty()||(count>=0&&objects.size()!=(size_t)count))throw std::runtime_error("Missing or truncated BDB placements.");
        auto f=words(art.line());if(f.size()!=1)throw std::runtime_error("Invalid BDD image count.");int image_count=number(f[0],10,1,4096);std::map<int,Image> images;
        for(int i=0;i<image_count;++i){f=words(art.line());if(f.size()<3||f.size()>4)throw std::runtime_error("Invalid BDD image header.");int id=number(f[0],16,0,65535);Image im;im.w=number(f[1],10,1,4096);im.h=number(f[2],10,1,4096);im.pixels=art.block((size_t)im.w*im.h);
            if(!images.emplace(id,std::move(im)).second)throw std::runtime_error("Duplicate BDD image ID.");}
        std::vector<std::vector<std::array<unsigned char,3>>> palettes;
        while(true){auto line=art.line();if(line.empty())break;f=words(line);if(f.size()!=2||palettes.size()>=256)throw std::runtime_error("Invalid BDD palette header.");int n=number(f[1],10,1,256);auto bytes=art.block((size_t)n*2);std::vector<std::array<unsigned char,3>> colors;
            for(int i=0;i<n;++i){unsigned c=bytes[i*2]|(bytes[i*2+1]<<8);std::array<unsigned char,3> rgb{};for(int k=0;k<3;++k){int v=(c>>(10-k*5))&31;rgb[k]=(unsigned char)((v<<3)|(v>>2));}colors.push_back(rgb);}palettes.push_back(std::move(colors));}
        if(palettes.empty())throw std::runtime_error("BDD contains no palettes.");
        bool any=false;int left=0,top=0,right=0,bottom=0;uint64_t work=0;
        for(auto &o:objects){auto found=images.find(o.image);if(found==images.end()||o.palette>=(int)palettes.size()){++result->skipped;continue;}auto &im=found->second;
            work+=(uint64_t)im.w*im.h;if(work>128*1024*1024)throw std::runtime_error("Stage exceeds the compositing work limit.");
            if(!any){left=o.x;top=o.y;right=o.x+im.w;bottom=o.y+im.h;any=true;}else{left=std::min(left,o.x);top=std::min(top,o.y);right=std::max(right,o.x+im.w);bottom=std::max(bottom,o.y+im.h);}
            for(auto &p:result->planes)if(o.x>=p.x1&&o.y>=p.y1&&o.x+im.w-1<=p.x2&&o.y+im.h-1<=p.y2){++p.objects;int layer=(o.flags>>8)&255;if(layer==0x40||layer==0x41)++p.playfield;
                if(!p.content){p.cx1=o.x;p.cy1=o.y;p.cx2=o.x+im.w-1;p.cy2=o.y+im.h-1;p.content=true;}else{p.cx1=std::min(p.cx1,o.x);p.cy1=std::min(p.cy1,o.y);p.cx2=std::max(p.cx2,o.x+im.w-1);p.cy2=std::max(p.cy2,o.y+im.h-1);}break;}
        }
        result->x=left;result->y=top;result->w=right-left;result->h=bottom-top;result->objects=(int)objects.size();
        if(!any||result->w<1||result->h<1||result->w>8192||result->h>8192||(uint64_t)result->w*result->h>16*1024*1024)throw std::runtime_error("Stage bounds exceed the supported 8192-pixel / 16-megapixel limits.");
        result->rgba.assign((size_t)result->w*result->h*4,0);
        for(auto &o:objects){auto found=images.find(o.image);if(found==images.end()||o.palette>=(int)palettes.size())continue;auto &im=found->second;auto &pal=palettes[o.palette];
            for(int y=0;y<im.h;++y)for(int x=0;x<im.w;++x){int sx=o.flags&0x10?im.w-1-x:x,sy=o.flags&0x20?im.h-1-y:y;unsigned ci=im.pixels[(size_t)sy*im.w+sx];if(!ci)continue;
                if(ci>=pal.size()){++result->invalid_pixels;continue;}auto dst=result->rgba.data()+((size_t)(o.y-top+y)*result->w+o.x-left+x)*4;
                std::copy(pal[ci].begin(),pal[ci].end(),dst);dst[3]=255;}
        }return result;
    }catch(const std::exception &e){error=e.what();return {};}
}
int stage_playfield_plane(const StageBackdrop &s){int best=-1;for(int i=0;i<(int)s.planes.size();++i)if(best<0||s.planes[i].playfield>s.planes[best].playfield||(s.planes[i].playfield==s.planes[best].playfield&&s.planes[i].objects>s.planes[best].objects))best=i;return best;}
void stage_align_plane(const StageBackdrop &s,StageView &v,int plane,int width,int floor){
    v.plane=plane;int x1=s.x,x2=s.x+s.w-1,y2=s.y+s.h-1;
    if(plane>=0&&plane<(int)s.planes.size()){auto &p=s.planes[plane];x1=p.content?p.cx1:p.x1;x2=p.content?p.cx2:p.x2;y2=p.content?p.cy2:p.y2;}
    v.camera_x=(x1+x2)/2-width/2;v.camera_y=y2-floor;
}
bool blit_stage(const StageBackdrop &s,const StageView &v,int w,int h,std::vector<unsigned char> &target,std::string &error){
    if(w<1||h<1||w>2048||h>2048||target.size()!=(size_t)w*h*4||s.w<1||s.h<1||s.w>8192||s.h>8192||s.rgba.size()!=(size_t)s.w*s.h*4){error="Invalid stage or playfield buffer.";return false;}
    if(!v.enabled)return true;int alpha=std::clamp(v.alpha,0,255);int64_t dx=(int64_t)s.x-v.camera_x,dy=(int64_t)s.y-v.camera_y;
    for(int y=0;y<h;++y){int64_t sy=y-dy;if(sy<0||sy>=s.h)continue;for(int x=0;x<w;++x){int64_t sx=x-dx;if(sx<0||sx>=s.w)continue;
        auto src=s.rgba.data()+((size_t)sy*s.w+(size_t)sx)*4;auto dst=target.data()+((size_t)y*w+x)*4;int a=(src[3]*alpha+127)/255;
        for(int c=0;c<3;++c)dst[c]=(unsigned char)((src[c]*a+dst[c]*(255-a)+127)/255);}}
    return true;
}
}
