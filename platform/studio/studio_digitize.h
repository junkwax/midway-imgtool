#pragma once
#include "studio_import.h"
#include "digitize_matte.h"
#include "material_mask.h"
#include "ramp_remap.h"
#include <functional>
namespace studio {
enum class DigitizeStage {Loading,Matte,Materials,Classify,Map};
struct DigitizeControl {
    std::function<bool()> cancelled;
    std::function<void(DigitizeStage,int,int)> progress;
};
struct DigitizeSource {std::string label;int w=0,h=0,ticks=4;std::vector<unsigned char> rgba;};
struct DigitizeWork {
    DigitizeSource source;
    int w=0,h=0,opaque=0,mapped=0;
    std::vector<unsigned char> rgb,alpha,mask,indices;
    MatteStats stats{};bool manual=false,included=true;
};
struct DigitizeMaterial {
    unsigned char id=1;std::string name="Material";
    int slots=24,fit=0,start=1,count=1; // Auto / Linear / Lloyd
    MaterialRef reference{};
    bool tint=false;std::array<unsigned char,3> color{192,38,38};float strength=1;
};
struct DigitizeSession {
    std::vector<DigitizeWork> frames;
    std::vector<DigitizeMaterial> materials;
    MatteParams matte=MatteParamsDefault();MaterialSeedParams seed=MaterialSeedParamsDefault();
    bool key=true,trim=true,custom_anchor=false,sequence=true,allow_unassigned=false;
    int scale=100,anchor_x=0,anchor_y=0,palette=-1;
    std::string prefix="DIGI";
    bool matte_ready=false,fitted=false;
    uint64_t revision=1;
    AssembledPalette assembled{};
    std::vector<unsigned short> words;
    int unmapped=0;
    std::string matte_signature;
    std::vector<DigitizeMaterial> fitted_materials;
    MaterialSeedParams fitted_seed{};int fitted_palette=-1;
};
bool load_digitize_sources(const std::vector<std::string> &paths,std::vector<DigitizeSource> &out,std::string &error,const DigitizeControl &control={});
bool initialize_digitize(std::vector<DigitizeSource> sources,DigitizeSession &out,std::string &error);
bool rebuild_digitize_matte(DigitizeSession &session,std::string &error,const DigitizeControl &control={});
bool edit_digitize_mask(DigitizeSession &session,int frame,int x,int y,int material,bool brush,int radius,std::string &error);
void classify_digitize(DigitizeSession &session,bool reset_later=false);
bool classify_digitize_checked(DigitizeSession &session,bool reset_later,const DigitizeControl &control={});
bool detect_digitize_blocks(const Artwork &art,DigitizeSession &session,std::string &error);
bool fit_digitize(const Artwork &art,DigitizeSession &session,std::string &error);
bool fit_digitize_palette(const std::vector<unsigned short> &target,DigitizeSession &session,std::string &error,const DigitizeControl &control={});
bool build_digitize_batch(const Artwork &art,const DigitizeSession &session,ImportBatch &out,std::string &error);
std::vector<unsigned char> render_digitize(const DigitizeSession &session,int frame,int view);
}
