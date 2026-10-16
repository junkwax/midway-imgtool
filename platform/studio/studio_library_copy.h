#pragma once
#include "studio_document.h"
#include <array>
#include "studio_sprite_scope.h"
#include <set>
namespace studio {
struct LibraryCopyOptions {std::string prefix;bool reuse=true,include_children=false;std::set<uint64_t> excluded;};
struct LibraryCopyPalette {int source=-1,target=-1;bool reused=false;std::array<unsigned char,sizeof(PAL)> fields{};std::vector<unsigned short> words;};
struct LibraryCopyRow {
    uint64_t sprite=0;bool enabled=true;int w=0,h=0,source_palette=-1,target_palette=-1;
    std::string name,copied_name,error;std::array<unsigned char,sizeof(IMG)> fields{};
    std::vector<unsigned char> pixels,baseline,points,alternate,mapped_alternate;
};
struct LibraryCopyReuse {
    uint64_t source=0,target=0;
    bool operator==(const LibraryCopyReuse &b)const{return source==b.source&&target==b.target;}
};
struct LibraryCopyPlan {
    const Artwork *source=nullptr,*target=nullptr;uint64_t source_generation=0,target_generation=0;
    SpriteScope scope;LibraryCopyOptions options;std::vector<LibraryCopyRow> rows;std::vector<LibraryCopyPalette> palettes;
    int added_palettes=0;size_t added_pixels=0;
    // Artwork-snapshot reuse proof. Reused rows are excluded from the append plan.
    bool reuse_named=false;LibraryCopyOptions reuse_options;std::vector<LibraryCopyReuse> reused;
};
bool plan_library_copy(const Artwork &source,const Artwork &target,const std::vector<uint64_t> &sprites,const LibraryCopyOptions &options,LibraryCopyPlan &out,std::string &error);
bool same_library_copy_plan(const LibraryCopyPlan &a,const LibraryCopyPlan &b);
std::vector<unsigned char> render_library_copy(const LibraryCopyPlan &plan,int row);
}
