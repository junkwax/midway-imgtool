#pragma once
#include "studio_costume.h"
#include "palette_layout.h"
namespace studio {
enum class CostumePresetKind {Layout,Template};
struct CostumePresetFile {
    CostumePresetKind kind=CostumePresetKind::Layout;
    std::string path,baseline;
    bool existed=false,loaded=false;
    std::vector<PaletteLayout> layouts;
    std::vector<CostumeTemplate> templates;
};
struct CostumeMappingReview {
    std::vector<CostumeSection> sections;
    int unpaired=0;
};
// Missing files load as an empty store. Malformed files leave out unchanged.
bool read_costume_presets(const std::string &path,CostumePresetKind kind,CostumePresetFile &out,std::string &error);
bool save_costume_layout(CostumePresetFile &file,const PaletteLayout &layout,bool replace,std::string &error);
bool save_costume_template(CostumePresetFile &file,const CostumeTemplate &preset,bool replace,std::string &error);
bool delete_costume_preset(CostumePresetFile &file,const std::string &name,std::string &error);
bool capture_costume_layout(const std::string &name,const std::string &palette,int numc,const std::vector<CostumeSection> &sections,bool destination,PaletteLayout &out,std::string &error);
bool capture_costume_template(const std::string &name,int destination_count,int reference_count,const std::vector<CostumeSection> &sections,CostumeTemplate &out,std::string &error);
bool review_costume_layout(const std::vector<CostumeSection> &current,const PaletteLayout &layout,bool destination,int destination_count,int reference_count,CostumeMappingReview &out,std::string &error);
bool review_costume_template(const CostumeTemplate &preset,int destination_count,int reference_count,CostumeMappingReview &out,std::string &error);
}
