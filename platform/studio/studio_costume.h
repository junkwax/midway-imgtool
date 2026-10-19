#pragma once
#include "studio_palette.h"
namespace studio {
enum class CostumeCoverage {Equal,Palette,Family};
struct CostumeSection {
    std::string name,destination,reference;
    bool enabled=true;
};
struct CostumeOptions {
    int palette=-1,strength=100;
    std::array<unsigned char,3> color{220,45,75};
    std::array<bool,256> slots{};
    std::string name; // Empty uses the source stem and tint hue.
    bool borrow=false;
    int reference=-1;
    CostumeCoverage coverage=CostumeCoverage::Family;
    std::vector<CostumeSection> sections;
};
struct CostumeSectionReview {
    std::string name;
    std::vector<int> destination,reference;
    uint64_t destination_pixels=0,reference_pixels=0;
};
struct CostumePlan {
    CostumeOptions options;
    PaletteCompactionPlan preview;
    std::vector<uint64_t> assignments;
    std::string name;
    int selected_slots=0,changed_slots=0;
    std::vector<PaletteCompactionPlan> coverage_sources;
    std::vector<CostumeSectionReview> sections;
    std::vector<unsigned short> reference_words;
    int destination_palettes=0,reference_palettes=0,destination_sprites=0,reference_sprites=0;
};
bool plan_costume(const Artwork &art,const CostumeOptions &options,const std::vector<uint64_t> &assignments,CostumePlan &plan,std::string &error);
}
