#include "studio_costume_presets.h"
#include "studio_file_io.h"
#include "palette_transfer.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
namespace studio {
namespace {
constexpr size_t file_limit=1024*1024;
bool name_ok(const std::string &s,size_t limit){return !s.empty()&&s.size()<=limit&&CleanLayoutField(s,limit)==s&&std::none_of(s.begin(),s.end(),[](unsigned char c){return c<32||c==127;});}
bool count_ok(int n){return n==0||(n>=2&&n<=256);}
bool ranges(const std::string &text,int numc,std::array<bool,256> &slots,std::string &error){
    if(text.empty()||text.size()>1023||text.find('\0')!=std::string::npos){error="Each saved material needs opaque slot ranges, up to 1023 characters.";return false;}
    TransferBlock parsed[256];int n=ParseSlotRanges(text.c_str(),256,parsed,256);if(n<=0){error="Use opaque slot ranges such as 1-5, 10-32.";return false;}slots.fill(false);
    for(int i=0;i<n;++i)for(int c=parsed[i].start;c<parsed[i].start+parsed[i].count;++c){if(numc&&c>=numc){error="A saved range exceeds this palette's color count. Edit the mapping before saving or loading it.";return false;}slots[c]=true;}
    // The classic parser clips beyond 255. Compare with an explicit numeric scan
    // so a stored preset can never quietly discard an out-of-bounds endpoint.
    for(size_t i=0;i<text.size();){if(text[i]<'0'||text[i]>'9'){++i;continue;}unsigned value=0;while(i<text.size()&&text[i]>='0'&&text[i]<='9'){value=value*10+(text[i++]-'0');if(value>255){error="Saved ranges must use opaque slots 1-255; no clipping is applied.";return false;}}if(!value){error="Index zero is protected; saved ranges start at 1.";return false;}}
    return true;
}
bool sections_ok(const std::vector<PaletteLayoutSection> &sections,int numc,std::string &error){
    if(sections.empty()||sections.size()>16){error="Save 1-16 named material sections.";return false;}std::vector<std::string> names;std::array<bool,256> taken{};
    for(const auto &s:sections){if(!name_ok(s.name,15)||std::any_of(names.begin(),names.end(),[&](const std::string &n){return SameLayoutName(n,s.name);})){error="Saved material names must be unique, 1-15 characters, without separators (| ; = ,). Rename the section before saving.";return false;}names.push_back(s.name);std::array<bool,256> slots{};if(!ranges(s.ranges,numc,slots,error))return false;for(int c=1;c<256;++c)if(slots[c]){if(taken[c]){error="Saved material sections cannot overlap on a palette.";return false;}taken[c]=true;}}
    return true;
}
bool layout_ok(const PaletteLayout &l,std::string &error){if(!name_ok(l.name,31)||!count_ok(l.numc)||l.palettes.size()>256){error="Use a layout name of 1-31 characters without separators, a valid color count, and at most 256 palette names.";return false;}std::vector<std::string> names;for(auto &n:l.palettes){if(!name_ok(n,9)||std::any_of(names.begin(),names.end(),[&](const std::string &p){return SameLayoutName(p,n);})){error="Layout palette names must be unique and at most 9 characters.";return false;}names.push_back(n);}return sections_ok(l.sections,l.numc,error);}
bool template_ok(const CostumeTemplate &t,std::string &error){if(!name_ok(t.name,31)||!count_ok(t.dst_numc)||!count_ok(t.ref_numc)){error="Use a template name of 1-31 characters without separators and valid palette counts.";return false;}return sections_ok(t.dst,t.dst_numc,error)&&sections_ok(t.ref,t.ref_numc,error);}
bool read_bytes(const std::string &path,bool &existed,std::string &bytes,std::string &error){namespace fs=std::filesystem;std::error_code ec;auto p=fs::u8path(path);auto status=fs::status(p,ec);if(ec&&ec!=std::errc::no_such_file_or_directory){error="Could not inspect the costume preset file.";return false;}existed=fs::exists(status);if(!existed){bytes.clear();return true;}if(!fs::is_regular_file(status)){error="The preset path is not a regular file.";return false;}std::ifstream f(p,std::ios::binary|std::ios::ate);auto size=f?f.tellg():std::streampos(-1);if(size<0||size>(std::streampos)file_limit){error="Could not read costume presets (limit: 1 MiB).";return false;}bytes.resize((size_t)size);f.seekg(0);if(!bytes.empty()&&!f.read(&bytes[0],bytes.size())){error="Could not finish reading costume presets.";return false;}return true;}
bool numeric_field(const std::string &s){if(s.empty()||s.size()>3||s.find_first_not_of("0123456789")!=std::string::npos)return false;return count_ok(std::stoi(s));}
bool fields_ok(const std::string &line,CostumePresetKind kind){std::istringstream f(line);std::string part;std::vector<std::string> fields;while(std::getline(f,part,'|'))fields.push_back(part);return fields.size()==(kind==CostumePresetKind::Layout?4u:5u)&&numeric_field(fields[1])&&(kind==CostumePresetKind::Layout||numeric_field(fields[2]));}
bool publish(const CostumePresetFile &before,CostumePresetFile &next,std::string &error){
    if(!before.loaded||before.path.empty()){error="Reload the preset file before saving changes.";return false;}
    bool existed=false;std::string current;if(!read_bytes(before.path,existed,current,error))return false;if(existed!=before.existed||current!=before.baseline){error="The preset file changed outside Studio. Reload presets before saving or deleting.";return false;}
    std::string text;std::vector<std::string> names;
    auto add=[&](const std::string &name,const std::string &line){if(names.size()>=256||std::any_of(names.begin(),names.end(),[&](const std::string &n){return SameLayoutName(n,name);})){error="Save at most 256 uniquely named presets per file.";return false;}names.push_back(name);text+=line+'\n';if(text.size()>file_limit){error="Costume presets exceed 1 MiB.";return false;}return true;};
    if(next.kind==CostumePresetKind::Layout){for(auto &l:next.layouts)if(!layout_ok(l,error)||!add(l.name,FormatPaletteLayoutLine(l)))return false;}
    else for(auto &t:next.templates)if(!template_ok(t,error)||!add(t.name,FormatCostumeTemplateLine(t)))return false;
    next.baseline=text;next.existed=true;std::vector<unsigned char> bytes(text.begin(),text.end());return atomic_write_file(before.path,bytes,error);
}
template<class T> int named(const std::vector<T> &items,const std::string &name){for(size_t i=0;i<items.size();++i)if(SameLayoutName(items[i].name,name))return (int)i;return -1;}
bool mapping(const std::vector<PaletteLayoutSection> &dst,const std::vector<PaletteLayoutSection> &ref,int dn,int rn,const std::vector<CostumeSection> *current,CostumeMappingReview &out,std::string &error){
    if((!dst.empty()&&!sections_ok(dst,dn,error))||(!ref.empty()&&!sections_ok(ref,rn,error)))return false;CostumeMappingReview r;
    for(auto &s:dst)r.sections.push_back({s.name,s.ranges,"",true});for(auto &s:ref){int k=named(r.sections,s.name);if(k<0)r.sections.push_back({s.name,"",s.ranges,true});else r.sections[k].reference=s.ranges;}
    if(r.sections.empty()||r.sections.size()>16){error="The combined mapping needs 1-16 materials. Remove or pair unmatched names first.";return false;}
    for(auto &s:r.sections){if(s.destination.empty()||s.reference.empty()){s.enabled=false;++r.unpaired;}else if(current){int i=named(*current,s.name);if(i>=0)s.enabled=(*current)[i].enabled;}}
    out=std::move(r);return true;
}
}
bool read_costume_presets(const std::string &path,CostumePresetKind kind,CostumePresetFile &out,std::string &error){
    error.clear();try{if(kind!=CostumePresetKind::Layout&&kind!=CostumePresetKind::Template){error="Choose a layout or template file.";return false;}CostumePresetFile file;file.kind=kind;file.path=path;if(!read_bytes(path,file.existed,file.baseline,error))return false;std::istringstream lines(file.baseline);std::string line;std::vector<std::string> names;int number=0;
        while(std::getline(lines,line)){++number;if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.empty())continue;std::string name;bool valid=fields_ok(line,kind);if(valid&&kind==CostumePresetKind::Layout){PaletteLayout l;valid=ParsePaletteLayoutLine(line.c_str(),&l)&&layout_ok(l,error)&&FormatPaletteLayoutLine(l)==line;name=l.name;if(valid)file.layouts.push_back(std::move(l));}else if(valid){CostumeTemplate t;valid=ParseCostumeTemplateLine(line.c_str(),&t)&&template_ok(t,error)&&FormatCostumeTemplateLine(t)==line;name=t.name;if(valid)file.templates.push_back(std::move(t));}
            if(!valid||names.size()>=256||std::any_of(names.begin(),names.end(),[&](const std::string &n){return SameLayoutName(n,name);})){error="Preset line "+std::to_string(number)+" is invalid or duplicated. "+error+" The file was left intact.";return false;}names.push_back(name);
        }file.loaded=true;out=std::move(file);return true;
    }catch(const std::bad_alloc &){error="Not enough memory to load costume presets. Existing presets were kept.";return false;}
}
bool save_costume_layout(CostumePresetFile &file,const PaletteLayout &layout,bool replace,std::string &error){
    error.clear();try{if(file.kind!=CostumePresetKind::Layout||!layout_ok(layout,error))return false;auto next=file;int i=named(next.layouts,layout.name);if(i>=0&&!replace){error="A layout with this name exists. Choose Replace to update it.";return false;}if(i>=0){auto l=layout;for(auto &n:next.layouts[i].palettes)if(std::none_of(l.palettes.begin(),l.palettes.end(),[&](const std::string &p){return SameLayoutName(p,n);}))l.palettes.push_back(n);next.layouts[i]=std::move(l);}else next.layouts.push_back(layout);if(!publish(file,next,error))return false;file=std::move(next);return true;}catch(const std::bad_alloc &){error="Not enough memory to save layouts. Presets were kept.";return false;}
}
bool save_costume_template(CostumePresetFile &file,const CostumeTemplate &preset,bool replace,std::string &error){
    error.clear();try{if(file.kind!=CostumePresetKind::Template||!template_ok(preset,error))return false;auto next=file;int i=named(next.templates,preset.name);if(i>=0&&!replace){error="A template with this name exists. Choose Replace to update it.";return false;}if(i>=0)next.templates[i]=preset;else next.templates.push_back(preset);if(!publish(file,next,error))return false;file=std::move(next);return true;}catch(const std::bad_alloc &){error="Not enough memory to save templates. Presets were kept.";return false;}
}
bool delete_costume_preset(CostumePresetFile &file,const std::string &name,std::string &error){error.clear();try{auto next=file;int i=file.kind==CostumePresetKind::Layout?named(next.layouts,name):named(next.templates,name);if(i<0){error="Choose an existing preset to delete.";return false;}if(file.kind==CostumePresetKind::Layout)next.layouts.erase(next.layouts.begin()+i);else next.templates.erase(next.templates.begin()+i);if(!publish(file,next,error))return false;file=std::move(next);return true;}catch(const std::bad_alloc &){error="Not enough memory to delete the preset. Presets were kept.";return false;}}
bool capture_costume_layout(const std::string &name,const std::string &palette,int numc,const std::vector<CostumeSection> &sections,bool destination,PaletteLayout &out,std::string &error){error.clear();try{PaletteLayout l;l.name=name;l.numc=numc;l.palettes={palette};for(auto &s:sections)if(s.enabled)l.sections.push_back({s.name,destination?s.destination:s.reference});if(!layout_ok(l,error))return false;out=std::move(l);return true;}catch(const std::bad_alloc &){error="Not enough memory to capture the layout.";return false;}}
bool capture_costume_template(const std::string &name,int dn,int rn,const std::vector<CostumeSection> &sections,CostumeTemplate &out,std::string &error){error.clear();try{CostumeTemplate t;t.name=name;t.dst_numc=dn;t.ref_numc=rn;for(auto &s:sections)if(s.enabled){t.dst.push_back({s.name,s.destination});t.ref.push_back({s.name,s.reference});}if(!template_ok(t,error))return false;out=std::move(t);return true;}catch(const std::bad_alloc &){error="Not enough memory to capture the template.";return false;}}
bool review_costume_layout(const std::vector<CostumeSection> &current,const PaletteLayout &layout,bool destination,int dn,int rn,CostumeMappingReview &out,std::string &error){error.clear();try{if(!layout_ok(layout,error))return false;if(!count_ok(dn)||!count_ok(rn)||dn<2||rn<2||(layout.numc&&layout.numc!=(destination?dn:rn))){error="Layout color count does not match this palette. No ranges were loaded.";return false;}std::vector<PaletteLayoutSection> opposite;for(auto &s:current){auto text=destination?s.reference:s.destination;if(!text.empty())opposite.push_back({s.name,text});}return mapping(destination?layout.sections:opposite,destination?opposite:layout.sections,dn,rn,&current,out,error);}catch(const std::bad_alloc &){error="Not enough memory to review the layout.";return false;}}
bool review_costume_template(const CostumeTemplate &preset,int dn,int rn,CostumeMappingReview &out,std::string &error){error.clear();try{if(!template_ok(preset,error))return false;if(dn<2||rn<2||dn>256||rn>256||(preset.dst_numc&&preset.dst_numc!=dn)||(preset.ref_numc&&preset.ref_numc!=rn)){error="Template color counts do not match these palettes. No ranges were loaded.";return false;}return mapping(preset.dst,preset.ref,dn,rn,nullptr,out,error);}catch(const std::bad_alloc &){error="Not enough memory to review the template.";return false;}}
}
