#pragma once
#include "studio_document.h"
#include "img_io.h"
#include <array>
namespace studio {
enum class MediaKind {Sheet,Gif};
struct ImportMedia {MediaKind kind=MediaKind::Sheet;std::string path;int w=0,h=0;std::vector<unsigned char> rgba;std::vector<int> delays_ms;};
struct ImportOptions {
    bool grid=false,clear_paper=true,trim=true,sequence=true;
    bool analysis_only=false,custom_anchor=false;
    int anchor_x=0,anchor_y=0;
    SpriteSheetImportOptions detect{SpriteSheetDetect_Auto,245,160,2,true,"FRAME"};
    int cell_w=64,cell_h=64,margin_x=0,margin_y=0,gap_x=0,gap_y=0;
    int palette=-1,hold=4,first=0,last=1999,every=1,blend=GifBlend_Normal,opacity=100,trim_tolerance=8;
    std::string prefix="FRAME";
    std::vector<bool> selected;
};
struct ImportFrame {int w=0,h=0,x=0,y=0,crop_x=0,crop_y=0,anix=0,aniy=0,ticks=4,source=0;std::string name;std::vector<unsigned char> pixels;};
struct ImportBatch {
    const Artwork *owner=nullptr;uint64_t generation=0;
    std::vector<ImportFrame> frames;
    std::vector<unsigned short> words;
    int palette=-1,unique_colors=0,source_count=0;
    bool sequence=true,review_only=false;std::string sequence_name,palette_name,note;
};
bool decode_import_media(const std::string &path,ImportMedia &out,std::string &error);
bool build_import_batch(const Artwork &art,const ImportMedia &media,const ImportOptions &options,ImportBatch &out,std::string &error);
bool name_import_batch(const Artwork &art,const std::string &prefix,ImportBatch &batch,std::string &error);
std::vector<unsigned char> render_import_frame(const ImportBatch &batch,int frame);
}
