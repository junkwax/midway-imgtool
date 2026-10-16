#include "studio_export.h"
#include "studio_file_io.h"
#include "palette_math.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <set>
#include <map>
#include <algorithm>
#include <limits>

namespace studio {
namespace {
namespace fs=std::filesystem;
constexpr size_t limit=128*1024*1024;
std::string name(const char *s,size_t size){auto end=std::find(s,s+size,0);return std::string(s,end);}
bool alpha(char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z');}
bool label(const std::string &s){if(s.empty()||(!alpha(s[0])&&s[0]!='_'))return false;for(char c:s)if(!alpha(c)&&!(c>='0'&&c<='9')&&c!='_'&&c!='$')return false;return true;}
std::string lower(std::string s){for(char &c:s)if(c>='A'&&c<='Z')c+=32;return s;}
std::string hex(uint64_t value,int width=0){std::ostringstream s;s<<'0'<<std::uppercase<<std::hex<<std::setfill('0')<<std::setw(width)<<value<<'H';return s.str();}
void word(std::vector<unsigned char> &out,uint16_t value){out.push_back((unsigned char)value);out.push_back((unsigned char)(value>>8));}
void dword(std::vector<unsigned char> &out,uint32_t value){word(out,(uint16_t)value);word(out,(uint16_t)(value>>16));}
struct Bits {
    std::vector<unsigned char> bytes;uint32_t pending=0,checksum=0;int filled=0;
    void add(unsigned value,int count){pending|=(uint32_t)value<<filled;filled+=count;while(filled>=16){auto part=(uint16_t)pending;word(bytes,part);checksum+=part;pending>>=16;filled-=16;}}
    void zeros(int count){while(count){int n=std::min(count,8);add(0,n);count-=n;}}
    void finish(){if(filled){auto part=(uint16_t)pending;word(bytes,part);checksum+=part;pending=0;filled=0;}}
};
bool row_equal(const ExportRow &a,const ExportRow &b){return a.sprite==b.sprite&&a.address==b.address&&a.sag==b.sag&&a.bits==b.bits&&a.enabled==b.enabled&&a.index==b.index&&a.w==b.w&&a.h==b.h&&a.bpp==b.bpp&&a.padding==b.padding&&a.entry==b.entry&&a.control==b.control&&a.name==b.name&&a.palette==b.palette&&a.error==b.error;}
bool read_destination(const fs::path &p,ExportDestination &out,std::string &error){
    std::error_code ec;out.existed=fs::exists(p,ec);if(ec){error="Could not inspect the output path.";return false;}if(!out.existed)return true;if(!fs::is_regular_file(p,ec)||ec){error="Choose a regular output file, not a directory.";return false;}
    std::ifstream f(p,std::ios::binary|std::ios::ate);auto size=f.tellg();if(!f||size<0||(uint64_t)size>limit){error="Could not review the existing output (limit: 128 MiB).";return false;}out.previous.resize((size_t)size);f.seekg(0);if(!out.previous.empty()&&!f.read((char *)out.previous.data(),size)){error="Could not read the existing output.";return false;}return true;
}
}
const char *export_extension(ExportKind kind){switch(kind){case ExportKind::Tbl:return ".TBL";case ExportKind::Irw:return ".IRW";case ExportKind::Anilst:return ".ASM";}return "";}
bool plan_export(const Artwork &art,const std::vector<ExportRequest> &requests,const ExportOptions &options,ExportPlan &plan,std::string &error){
    plan={};error.clear();try{
        bool list=options.kind==ExportKind::Anilst;if(options.kind!=ExportKind::Tbl&&options.kind!=ExportKind::Irw&&!list){error="Choose an export format.";return false;}
        if(!list&&(options.bpp<-1||options.bpp>8||options.bank<0||options.bank>1||(int)options.alignment<0||(int)options.alignment>2)){error="Invalid bit depth, alignment or bank setting.";return false;}
        if(!list&&(options.base&15)){error="The IRW load address must start on a 16-bit boundary. Choose a hex address ending in 0.";return false;}
        if(requests.empty()||requests.size()>2000){error="Choose 1 through 2000 sprites.";return false;}
        ExportPlan next;next.owner=&art;next.generation=art.generation();next.options=options;next.requests=requests;std::set<uint64_t> ids;
        for(auto &r:requests)if(!ids.insert(r.sprite).second||art.sprite_index(r.sprite)<0){error="A selected sprite is missing or repeated. Rebuild the review.";return false;}
        std::sort(next.requests.begin(),next.requests.end(),[&](const ExportRequest &a,const ExportRequest &b){return art.sprite_index(a.sprite)<art.sprite_index(b.sprite);});
        uint64_t address=options.base;size_t snapshot_bytes=0;std::set<std::string> names;
        for(auto &request:next.requests){int index=art.sprite_index(request.sprite);auto im=art.sprite(index);ExportRow row;ExportSource source;row.sprite=request.sprite;row.enabled=request.enabled;row.index=index;row.w=im->w;row.h=im->h;row.name=name(im->n_s,sizeof im->n_s);memcpy(source.fields.data(),im,sizeof *im);
            auto pal=art.palette(im->palnum);if(pal){row.palette=name(pal->n_s,sizeof pal->n_s);memcpy(source.palette_fields.data(),pal,sizeof *pal);}
            if(request.enabled){++next.included;row.entry=next.included-1;
                if(list){if(row.name.empty()||std::any_of(row.name.begin(),row.name.end(),[](unsigned char c){return c<32||c==127;}))row.error="Name cannot be an assembly comment.";}
                else {
                    if(im->layer_p)row.error="Flatten or remove this sprite overlay before pipeline export.";
                    else if(im->w==0||im->h==0||im->w>8192||im->h>8192||(uint64_t)im->w*im->h>4*1024*1024||!im->data_p)row.error="Invalid sprite geometry or pixels.";
                    else if(!pal||!pal->data_p||pal->numc==0||pal->numc>256)row.error="Invalid primary palette.";
                    else {int stride=(im->w+3)&~3;size_t size=(size_t)stride*im->h;snapshot_bytes+=size+pal->numc*2;if(snapshot_bytes>limit){error="The source review exceeds 128 MiB. Narrow the export scope.";return false;}source.pixels.assign((unsigned char *)im->data_p,(unsigned char *)im->data_p+size);source.colors.assign((unsigned char *)pal->data_p,(unsigned char *)pal->data_p+pal->numc*2);unsigned max=0;for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x)max=std::max(max,(unsigned)source.pixels[(size_t)y*stride+x]);
                        row.bpp=options.bpp==-1?PaletteBppForColorCount(pal->numc):options.bpp==0?PaletteBppForColorCount((int)max+1):options.bpp;
                        if(max>=pal->numc)row.error="A visible pixel is outside the palette.";else if(max>=(1u<<row.bpp))row.error="Bit depth would truncate a pixel index.";
                        row.control=(uint16_t)((row.bpp&7)<<12);row.bits=(uint64_t)im->w*im->h*row.bpp;int align=options.alignment==ExportAlignment::Word?16:options.alignment==ExportAlignment::Nibble?4:1;row.padding=(int)((align-row.bits%align)%align);row.address=address;
                        int64_t sag=(int64_t)address+(options.dual_bank?(options.bank?0x02000000ll:-0x02000000ll):0);row.sag=sag>=0?(uint64_t)sag:std::numeric_limits<uint64_t>::max();if(sag<0||sag>UINT32_MAX)row.error="Bank-adjusted SAG overflows a 32-bit address.";
                        address+=row.bits+row.padding;if(row.address>UINT32_MAX||address>0x100000000ull)row.error="Packed pixels overflow the 32-bit ROM address space.";
                        if(options.kind==ExportKind::Tbl){if(!label(row.name))row.error="Sprite name is not an assembly label.";else if(!names.insert(row.name).second)row.error="Repeated sprite label.";if(options.palette&&!row.palette.empty()&&!label(row.palette))row.error="Palette name is not an assembly label.";}
                    }
                }
            }
            next.rows.push_back(std::move(row));next.sources.push_back(std::move(source));
        }
        if(options.kind==ExportKind::Tbl&&options.palette){std::map<std::string,size_t> palettes;for(size_t i=0;i<next.rows.size();++i){auto &r=next.rows[i];if(!r.enabled||r.palette.empty())continue;if(names.count(r.palette))r.error="Palette label collides with a sprite label.";auto found=palettes.find(r.palette);if(found!=palettes.end()&&next.sources[i].colors!=next.sources[found->second].colors)r.error="Palette label refers to different colors.";else palettes.emplace(r.palette,i);}}
        for(auto &r:next.rows)next.blocked+=r.enabled&&!r.error.empty();next.ready=next.included>0&&!next.blocked;
        if(next.ready){
            if(list){std::ostringstream text;text<<"\t.asg\t1,N\r\n";for(auto &r:next.rows)if(r.enabled)text<<"\t.word\tN,"<<r.entry<<"\t;"<<r.name<<"\r\n";auto output=text.str();next.bytes.assign(output.begin(),output.end());}
            else {
                uint64_t data_bits=address-options.base;next.packed_bytes=((data_bits+15)/16)*2;if(next.packed_bytes>limit-68||options.base+next.packed_bytes*8>0x100000000ull){error="The final packed output exceeds its size or address limit.";return false;}
                if(options.kind==ExportKind::Tbl){std::ostringstream text;text<<"; Imgtool Studio raw layout. SAGs match IRW with the same scope/options.\r\n; Uncompressed pixels; CTRL derives from bit depth.\r\n\t.DATA\r\n";for(size_t i=0;i<next.rows.size();++i){auto &r=next.rows[i];if(!r.enabled)continue;IMG im{};memcpy(&im,next.sources[i].fields.data(),sizeof im);text<<r.name<<":\r\n\t.word   "<<r.w<<','<<r.h<<','<<(int)(short)im.anix<<','<<(int)(short)im.aniy;if(options.mk3)text<<','<<(int)(short)im.anix2<<','<<(int)(short)im.aniy2<<','<<(int)(short)im.aniz2;text<<"\r\n\t.long   "<<hex(r.sag)<<"\r\n\t.word   "<<hex(r.control,4)<<"\r\n";if(options.palette)text<<"\t.long   "<<(r.palette.empty()?"0":r.palette)<<"\r\n";}text<<"\t.TEXT\r\n";auto output=text.str();next.bytes.assign(output.begin(),output.end());}
                else {
                    // DOS load2.h uses 16-bit checksum/magic fields: 44-byte
                    // file header + 24-byte record, matching shipped MK2 IRWs.
                    Bits bits;bits.bytes.reserve((size_t)next.packed_bytes);for(size_t i=0;i<next.rows.size();++i){auto &r=next.rows[i];if(!r.enabled)continue;auto &s=next.sources[i];int stride=(r.w+3)&~3;for(int y=0;y<r.h;++y)for(int x=0;x<r.w;++x)bits.add(s.pixels[(size_t)y*stride+x],r.bpp);bits.zeros(r.padding);}bits.finish();const char version[]="IMAGETOOL IRW 1.0";next.bytes.resize(32,0);std::copy(version,version+sizeof version-1,next.bytes.begin());uint32_t version_sum=0;for(char c:version)version_sum+=(unsigned char)c;word(next.bytes,(uint16_t)version_sum);word(next.bytes,0x64);dword(next.bytes,0);dword(next.bytes,0);dword(next.bytes,options.base);dword(next.bytes,(uint32_t)bits.bytes.size());dword(next.bytes,bits.checksum);word(next.bytes,4);word(next.bytes,0);word(next.bytes,(uint16_t)(options.dual_bank?options.bank:-1));word(next.bytes,0);dword(next.bytes,0);next.bytes.insert(next.bytes.end(),bits.bytes.begin(),bits.bytes.end());}
            }
        }
        plan=std::move(next);return true;
    }catch(const std::exception &e){error=std::string("Could not build export review: ")+e.what();return false;}
}
bool review_export_destination(const Artwork &art,const ExportPlan &plan,const std::string &path,ExportDestination &destination,std::string &error){
    destination={};error.clear();try{if(plan.owner!=&art||!plan.ready){error="Build a valid export review first.";return false;}std::error_code ec;auto file=fs::absolute(fs::u8path(path),ec).lexically_normal();if(ec||file.filename().empty()||lower(file.extension().u8string())!=lower(export_extension(plan.options.kind))){error=std::string("Choose a ")+export_extension(plan.options.kind)+" output filename.";return false;}
        if(!art.path().empty()){auto source=fs::absolute(fs::u8path(art.path())).lexically_normal();bool same=file==source;
#ifdef _WIN32
            same=same||lower(file.u8string())==lower(source.u8string());
#endif
            if(same||fs::equivalent(file,source,ec)){error="The export cannot replace its source IMG.";return false;}}ExportDestination next;next.path=file.u8string();if(!read_destination(file,next,error))return false;destination=std::move(next);return true;
    }catch(const std::exception &e){error=std::string("Could not review the destination: ")+e.what();return false;}
}
bool write_export(const Artwork &art,const ExportPlan &plan,const ExportDestination &destination,std::string &error){
    error.clear();try{if(plan.owner!=&art||plan.generation!=art.generation()||!plan.ready){error="Artwork changed or this review belongs to another library. Rebuild it.";return false;}ExportPlan fresh;if(!plan_export(art,plan.requests,plan.options,fresh,error))return false;bool same=fresh.ready&&fresh.bytes==plan.bytes&&fresh.packed_bytes==plan.packed_bytes&&fresh.included==plan.included&&fresh.blocked==plan.blocked&&fresh.rows.size()==plan.rows.size()&&fresh.sources.size()==plan.sources.size();for(size_t i=0;same&&i<fresh.rows.size();++i){const auto &a=fresh.sources[i],&b=plan.sources[i];same=row_equal(fresh.rows[i],plan.rows[i])&&a.fields==b.fields&&a.palette_fields==b.palette_fields&&a.pixels==b.pixels&&a.colors==b.colors;}if(!same){error="Source or export preview changed. Rebuild it before writing.";return false;}
        ExportDestination current;if(!review_export_destination(art,plan,destination.path,current,error))return false;if(current.existed!=destination.existed||current.previous!=destination.previous){error="The destination changed after review. Rebuild before replacing it.";return false;}return atomic_write_file(current.path,plan.bytes,error);
    }catch(const std::exception &e){error=std::string("Could not export: ")+e.what();return false;}
}
}
