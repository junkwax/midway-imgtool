#include "studio_palette_ramps.h"
#include "studio_file_io.h"
#include "palette_math.h"
#include "color_ops.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
namespace studio {
namespace {
GradientColor rgb(unsigned short w){return {(unsigned char)(((w>>10)&31)*255/31),(unsigned char)(((w>>5)&31)*255/31),(unsigned char)((w&31)*255/31)};}
int luma(GradientColor c){return 30*c[0]+59*c[1]+11*c[2];}
unsigned char byte(float value){return (unsigned char)std::lround(std::clamp(value,0.f,255.f));}
bool valid_preset(const GradientPreset &p){return !p.name.empty()&&p.name.size()<=64&&p.stops.size()>=2&&p.stops.size()<=11&&std::none_of(p.name.begin(),p.name.end(),[](unsigned char c){return c<32||c==127||c=='|';});}
}
std::vector<GradientColor> shifted_gradient_stops(const std::vector<GradientColor> &stops,int hue,int saturation,int lightness){
    if(stops.size()<2||stops.size()>11)return {};if(!hue&&!saturation&&!lightness)return stops;
    unsigned char words[22]={},result[22]={},colors[33]={};bool mask[11]={};for(size_t i=0;i<stops.size();++i){rgb8_to_pal_word(stops[i][0],stops[i][1],stops[i][2],words+i*2);mask[i]=true;}
    HslAdjustPaletteWordsFromBaseline(words,(int)stops.size(),mask,std::clamp(hue,-180,180),std::clamp(saturation,-100,100),std::clamp(lightness,-100,100),result,colors);
    auto out=stops;for(size_t i=0;i<out.size();++i)for(int c=0;c<3;++c)out[i][c]=colors[i*3+c];return out;
}
bool build_palette_ramp(const std::vector<unsigned short> &baseline,const std::array<bool,256> &selected,const PaletteRampOptions &o,std::vector<unsigned short> &out,std::string &error){
    error.clear();if(baseline.empty()||baseline.size()>256){error="Choose a valid palette.";return false;}std::vector<int> slots;for(size_t i=1;i<baseline.size();++i)if(selected[i])slots.push_back((int)i);if(slots.empty()){error="Select at least one opaque palette color first.";return false;}
    if(!o.shading&&(o.stops.size()<2||o.stops.size()>11)){error="A gradient needs 2–11 color stops.";return false;}auto result=baseline;
    if(o.shading){float strength=std::clamp(o.strength,0,100)/100.f,brightness=std::clamp(o.brightness,-100,100)/100.f,contrast=std::clamp(o.contrast,-100,100)/100.f;
        if(!strength&&!brightness&&!contrast){out=baseline;return true;}
        for(int i:slots){auto w=baseline[i];int source[]={(w>>10)&31,(w>>5)&31,w&31};float light=(30.f*source[0]+59.f*source[1]+11.f*source[2])/(100.f*31.f);unsigned char color[3];for(int c=0;c<3;++c){float original=source[c]*(255.f/31.f),shaded=o.base[c]*light;float mix=original+(shaded-original)*strength;mix=(mix-127.5f)*(1.f+contrast)+127.5f+brightness*255.f;color[c]=byte(mix);}result[i]=(unsigned short)(rgb_to_word15(color[0],color[1],color[2])|(w&0x8000));}
    }else{auto stops=shifted_gradient_stops(o.stops,o.hue,o.saturation,o.lightness);int min=25500,max=0;for(int i:slots){int y=luma(rgb(baseline[i]));min=std::min(min,y);max=std::max(max,y);}
        for(size_t n=0;n<slots.size();++n){int i=slots[n];float t=o.brightness_order?(max>min?(float)(luma(rgb(baseline[i]))-min)/(max-min):.5f):(slots.size()>1?(float)n/(slots.size()-1):.5f);float scaled=t*(stops.size()-1);int segment=std::min((int)scaled,(int)stops.size()-2);float f=scaled-segment;GradientColor c;for(int k=0;k<3;++k)c[k]=byte(stops[segment][k]+(stops[segment+1][k]-stops[segment][k])*f);result[i]=(unsigned short)(rgb_to_word15(c[0],c[1],c[2])|(baseline[i]&0x8000));}
    }out=std::move(result);return true;
}
std::vector<GradientColor> seed_gradient_stops(const std::vector<unsigned short> &words,const std::array<bool,256> &selected,int count){
    if(words.empty()||words.size()>256||count<2||count>11)return {};std::vector<GradientColor> samples;for(size_t i=1;i<words.size();++i)if(selected[i])samples.push_back(rgb(words[i]));if(samples.empty())return {};
    std::stable_sort(samples.begin(),samples.end(),[](auto a,auto b){return luma(a)<luma(b);});std::vector<GradientColor> stops;for(int n=0;n<count;++n){double position=(double)n*(samples.size()-1)/(count-1);size_t low=(size_t)position,high=std::min(low+1,samples.size()-1);float f=(float)(position-low);GradientColor c;for(int k=0;k<3;++k)c[k]=byte(samples[low][k]+(samples[high][k]-samples[low][k])*f);stops.push_back(c);}return stops;
}
std::vector<GradientColor> resample_gradient_stops(const std::vector<GradientColor> &stops,int count){
    if(stops.size()<2||stops.size()>11||count<2||count>11)return {};std::vector<GradientColor> out;for(int n=0;n<count;++n){double position=(double)n*(stops.size()-1)/(count-1);size_t low=(size_t)position,high=std::min(low+1,stops.size()-1);float f=(float)(position-low);GradientColor c;for(int k=0;k<3;++k)c[k]=byte(stops[low][k]+(stops[high][k]-stops[low][k])*f);out.push_back(c);}return out;
}
bool read_gradient_presets(const std::string &path,std::vector<GradientPreset> &out,std::string &error){
    error.clear();std::ifstream f(std::filesystem::u8path(path),std::ios::binary|std::ios::ate);auto size=f?f.tellg():std::streampos(-1);if(size<0||size>65536){error="Could not read gradient presets (limit: 64 KiB).";return false;}std::string content((size_t)size,'\0');f.seekg(0);if(!content.empty()&&!f.read(&content[0],content.size())){error="Could not finish reading gradient presets.";return false;}std::istringstream lines(content);std::string line;std::vector<GradientPreset> presets;
    while(std::getline(lines,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.empty())continue;auto first=line.find('|'),second=first==std::string::npos?first:line.find('|',first+1);if(first==std::string::npos||second==std::string::npos){error="A saved gradient line is malformed.";return false;}GradientPreset p;p.name=line.substr(0,first);auto count_text=line.substr(first+1,second-first-1);if(count_text.empty()||count_text.size()>2||count_text.find_first_not_of("0123456789")!=std::string::npos){error="A saved gradient count is malformed.";return false;}int count=std::stoi(count_text);std::istringstream stops(line.substr(second+1));std::string token;while(std::getline(stops,token,',')){if(token.size()!=6||token.find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos){error="A saved gradient color is malformed.";return false;}unsigned value=std::stoul(token,nullptr,16);p.stops.push_back({(unsigned char)(value>>16),(unsigned char)(value>>8),(unsigned char)value});}
        if(!valid_preset(p)||p.stops.size()!=(size_t)count||line.back()==','||presets.size()>=256||std::any_of(presets.begin(),presets.end(),[&](const GradientPreset &existing){return existing.name==p.name;})){error="Saved gradients have an invalid name, count, duplicate or excess entries.";return false;}presets.push_back(std::move(p));}
    out=std::move(presets);return true;
}
bool write_gradient_presets(const std::string &path,const std::vector<GradientPreset> &presets,std::string &error){
    error.clear();if(presets.size()>256){error="At most 256 gradient presets can be saved.";return false;}std::ostringstream out;std::vector<std::string> names;for(auto &p:presets){if(!valid_preset(p)||std::find(names.begin(),names.end(),p.name)!=names.end()){error="Use unique gradient names (1–64 characters, without pipes or line breaks) and 2–11 stops.";return false;}names.push_back(p.name);out<<p.name<<'|'<<p.stops.size()<<'|';for(size_t i=0;i<p.stops.size();++i){if(i)out<<',';for(auto c:p.stops[i])out<<std::uppercase<<std::hex<<std::setfill('0')<<std::setw(2)<<(unsigned)c;out<<std::dec;}out<<'\n';}auto text=out.str();if(text.size()>65536){error="Gradient presets exceed 64 KiB.";return false;}return atomic_write_file(path,std::vector<unsigned char>(text.begin(),text.end()),error);
}
}
