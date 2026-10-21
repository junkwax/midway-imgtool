#pragma once
#include "studio_library_copy.h"

namespace studio {
struct LibrarySplitAnimationRow {
    int source=0;
    bool script=false,included=false;
    std::string name,reason;
};
struct LibrarySplitAnimationPlan {
    const Artwork *source=nullptr,*target=nullptr;
    uint64_t source_generation=0,target_generation=0;
    std::vector<uint64_t> sprites;
    std::vector<LibrarySplitAnimationRow> rows;
    std::vector<unsigned char> source_blob,result_blob,source_damage,result_damage;
    unsigned sequences=0,scripts=0;
};
// Copy only records fully supported by the reviewed sprite set. Never extends
// that set or removes anything from the source. Skipped records have reasons.
bool plan_library_split_animations(const Artwork &source,const LibraryCopyPlan &copy,
                                   LibrarySplitAnimationPlan &out,std::string &error);
}
