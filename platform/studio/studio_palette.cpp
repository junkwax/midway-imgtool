#include "studio_palette.h"
#include "palette_math.h"
#include "color_ops.h"
#include "studio_file_io.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
namespace studio {
namespace {
bool valid_name(const std::string &name,std::string &error){if(name.empty()||name.size()>9||std::any_of(name.begin(),name.end(),[](unsigned char c){return c<33||c>126;})){error="Use 1–9 printable ASCII characters without spaces for the palette name.";return false;}return true;}
bool valid_words(const std::vector<unsigned short> &words,std::string &error){if(words.empty()||words.size()>256){error="A palette must contain 1–256 colors.";return false;}return true;}
int read_word(const unsigned char *p){return p[0]|p[1]<<8;}
void set_word(unsigned char *p,unsigned short v){p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);}
}
std::string palette_name(const PAL &pal){return std::string(pal.n_s,strnlen(pal.n_s,10));}
PaletteTransfer copy_palette(const Artwork &art,int index){PaletteTransfer result;if(auto p=art.palette(index)){result.name=palette_name(*p);for(int i=0;i<p->numc;++i)result.words.push_back(palette_word_at((const unsigned char *)p->data_p,i));}return result;}
PaletteUsage palette_usage(const Artwork &art,int index){PaletteUsage usage;if(index<0||index>=art.palette_count())return usage;for(int i=0;i<art.count();++i){auto im=art.sprite(i);if(im->palnum==index)++usage.primary;if(im->opaltbl_p){auto refs=(const unsigned char *)im->opaltbl_p;for(int a=0;a<16&&refs[a]<128;++a)if(refs[a]==index+NUMDEFPAL)++usage.alternate;}}return usage;}
std::vector<unsigned short> adjust_palette(const std::vector<unsigned short> &baseline,const std::array<bool,256> &selected,int hue,int saturation,int lightness){
    if(baseline.empty()||baseline.size()>256)return {};auto out=baseline;if(!hue&&!saturation&&!lightness)return out;bool any=false;for(size_t i=1;i<baseline.size();++i)any|=selected[i];if(!any)return out;
    std::vector<unsigned char> input(baseline.size()*2),result(input.size());for(size_t i=0;i<baseline.size();++i)set_word(input.data()+i*2,baseline[i]);auto mask=selected;mask[0]=false;
    HslAdjustPaletteWordsFromBaseline(input.data(),(int)baseline.size(),mask.data(),std::clamp(hue,-180,180),std::clamp(saturation,-100,100),std::clamp(lightness,-100,100),result.data(),nullptr);
    for(size_t i=1;i<out.size();++i)if(mask[i])out[i]=(unsigned short)(read_word(result.data()+i*2)|(baseline[i]&0x8000));return out;
}
std::vector<unsigned short> palette_color_effect(const std::vector<unsigned short> &words,const std::array<bool,256> &selected,PaletteColorEffect effect){
    if(words.empty()||words.size()>256||(effect!=PaletteColorEffect::Invert&&effect!=PaletteColorEffect::Reverse))return {};
    auto result=words;
    if(effect==PaletteColorEffect::Invert){
        for(size_t i=1;i<words.size();++i)if(selected[i])result[i]=(unsigned short)(words[i]^0x7fff);
    }else{
        size_t left=1,right=words.size()-1;
        while(left<right){if(!selected[left]){++left;continue;}if(!selected[right]){--right;continue;}std::swap(result[left++],result[right--]);}
    }
    return result;
}
bool Artwork::rename_palette(int index,const std::string &name,std::string &error){error.clear();auto p=palette(index);if(!p){error="Choose an existing palette.";return false;}if(!valid_name(name,error))return false;if(palette_name(*p)==name)return true;begin("Rename palette");memset(p->n_s,0,10);memcpy(p->n_s,name.data(),name.size());memset(p->file_name_raw,0,10);++serial;commit();return true;}
int Artwork::add_palette(const std::string &name,const std::vector<unsigned short> &words,std::string &error){
    error.clear();if(!valid_name(name,error)||!valid_words(words,error))return -1;if(palette_count()>=4093){error="This IMG has reached its palette limit.";return -1;}
    auto pal=(PAL *)calloc(1,sizeof(PAL));if(!pal){error="Not enough memory for a palette.";return -1;}pal->data_p=malloc(words.size()*2);if(!pal->data_p){free(pal);error="Not enough memory for palette colors.";return -1;}
    memcpy(pal->n_s,name.data(),name.size());pal->numc=(unsigned short)words.size();pal->bitspix=(unsigned char)PaletteBppForColorCount(pal->numc);for(size_t i=0;i<words.size();++i)set_word((unsigned char *)pal->data_p+i*2,words[i]);
    try{begin("Add palette");}catch(...){free(pal->data_p);free(pal);throw;}PAL **link=(PAL **)&data.pal_p;while(*link)link=(PAL **)&(*link)->nxt_p;*link=pal;++data.palcnt;++serial;++structure_serial;commit();return palette_count()-1;
}
bool Artwork::replace_palette_colors(int index,const std::vector<unsigned short> &expected,const std::vector<unsigned short> &words,std::string &error){
    error.clear();auto current=copy_palette(*this,index);if(current.words.empty()||current.words!=expected){error="The palette changed while this preview was open. Reset the preview before applying.";return false;}if(words.size()!=expected.size()){error="Replacing colors must retain the palette's color count. Paste as a new palette to change its size.";return false;}if(words==expected)return true;if(words[0]!=expected[0]){error="Index zero is reserved for transparency and stays unchanged.";return false;}
    begin("Apply palette colors");auto pal=palette(index);for(size_t i=0;i<words.size();++i)set_word((unsigned char *)pal->data_p+i*2,words[i]);++serial;commit();return true;
}
bool Artwork::delete_unused_palette(int index,std::string &error){
    error.clear();auto p=palette(index);if(!p){error="Choose an existing palette.";return false;}auto usage=palette_usage(*this,index);if(usage.primary||usage.alternate){error="This palette is referenced by sprites. Assign another palette first.";return false;}
    if(palette_count()<=1){error="Keep at least one palette in this library.";return false;}
    for(int i=0;i<count();++i){auto im=sprite(i);if(im->opaltbl_p){auto refs=(unsigned char *)im->opaltbl_p;for(int a=0;a<16&&refs[a]<128;++a)if(refs[a]>=palette_count()+NUMDEFPAL){error="An alternate palette reference is unresolved. Correct it before removing palettes.";return false;}}}
    begin("Delete unused palette");PAL **link=(PAL **)&data.pal_p;for(int i=0;i<index;++i)link=(PAL **)&(*link)->nxt_p;*link=(PAL *)p->nxt_p;free(p->data_p);free(p);--data.palcnt;
    for(int i=0;i<count();++i){auto im=sprite(i);if(im->palnum>index)--im->palnum;if(im->opaltbl_p){auto refs=(unsigned char *)im->opaltbl_p;for(int a=0;a<16&&refs[a]<128;++a)if(refs[a]>index+NUMDEFPAL)--refs[a];}}
    ++serial;++structure_serial;commit();return true;
}
bool read_palette_file(const std::string &path,PaletteTransfer &out,std::string &error){
    error.clear();namespace fs=std::filesystem;std::ifstream f(fs::u8path(path),std::ios::binary|std::ios::ate);auto n=f?f.tellg():std::streampos(-1);if(n<=0||n>772){error="Palette files must be raw PAL (2–512 bytes) or ACT (768/772 bytes).";return false;}std::vector<unsigned char> bytes((size_t)n);f.seekg(0);if(!f.read((char *)bytes.data(),bytes.size())){error="Could not read the palette file.";return false;}
    auto ext=fs::u8path(path).extension().u8string();for(char &c:ext)c=(char)std::tolower((unsigned char)c);bool act=ext==".act"||(ext!=".pal"&&(bytes.size()==768||bytes.size()==772));PaletteTransfer result;
    if(act){if(bytes.size()!=768&&bytes.size()!=772){error="ACT files require 768 RGB bytes and an optional four-byte footer.";return false;}int count=256;if(bytes.size()==772){count=bytes[768]*256+bytes[769];if(!count)count=256;int transparent=bytes[770]*256+bytes[771];if(count>256||(transparent!=0&&transparent!=65535)){error="ACT color count must be 1–256, with transparent index zero or unspecified.";return false;}}for(int i=0;i<count;++i)result.words.push_back(rgb_to_word15(bytes[i*3],bytes[i*3+1],bytes[i*3+2]));}
    else {if(bytes.size()%2||bytes.size()>512){error="Raw PAL files require 1–256 complete little-endian RGB555 words.";return false;}for(size_t i=0;i<bytes.size();i+=2)result.words.push_back((unsigned short)read_word(bytes.data()+i));}
    auto stem=fs::u8path(path).stem().u8string();for(unsigned char c:stem){if(result.name.size()==9)break;result.name+=(c>=33&&c<=126)?(char)c:'_';}if(result.name.empty())result.name="IMPORTED";out=std::move(result);return true;
}
bool write_palette_file(const std::string &path,const PaletteTransfer &palette,bool act,std::string &error){
    error.clear();if(!valid_words(palette.words,error))return false;std::vector<unsigned char> bytes(act?772:palette.words.size()*2);
    for(size_t i=0;i<palette.words.size();++i){unsigned char word[2];set_word(word,palette.words[i]);if(act)pal_word_to_rgb8(word,&bytes[i*3],&bytes[i*3+1],&bytes[i*3+2]);else set_word(bytes.data()+i*2,palette.words[i]);}
    if(act){bytes[768]=(unsigned char)(palette.words.size()>>8);bytes[769]=(unsigned char)palette.words.size();}return atomic_write_file(path,bytes,error);
}
}
