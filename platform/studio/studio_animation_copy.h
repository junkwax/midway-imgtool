#pragma once
#include "studio_sequences.h"
#include "studio_library_copy.h"
#include <set>
#include <map>
namespace studio {
struct AnimationCopyOptions {
    std::set<int> requested;std::string prefix,sprite_prefix;bool copy_missing=false;
    // Stable IDs in the chosen source/destination libraries. Unused overrides
    // remain available when animation selections change; all IDs must exist.
    std::map<uint64_t,uint64_t> sprite_bindings;
};
struct AnimationCopyRow {
    int source=0,target=-1,entries=0;bool script=false,included=false,dependency=false;
    std::string name,copied_name,error;
};
struct AnimationCopyBinding {
    int source=-1,target=-1;uint64_t source_id=0,target_id=0;bool manual=false,copied=false;
    std::string name,target_name,error;
    std::array<unsigned char,sizeof(IMG)> source_fields{},target_fields{};
};
struct AnimationDamageBinding {
    int source=-1,target=-1,record=-1;bool script=false;
    std::string name;
};
struct AnimationCopyPlan {
    const Artwork *source=nullptr,*target=nullptr;uint64_t source_generation=0,target_generation=0;
    unsigned source_sequences=0,source_scripts=0,target_sequences=0,target_scripts=0;
    AnimationCopyOptions options;std::vector<AnimationCopyRow> rows;std::vector<AnimationCopyBinding> bindings;
    std::vector<unsigned char> source_blob,target_blob,result_blob;
    std::vector<unsigned char> source_damage,target_damage,result_damage;
    std::vector<AnimationDamageBinding> damage_bindings;
    LibraryCopyPlan artwork;
    int added_sequences=0,added_scripts=0;
};
// Match existing artwork or optionally append missing unique names. Never replaces
// sprites or existing animations. Includes transitive sequence/script/damage
// dependencies and remaps only record, sprite and damage-reference indices.
bool plan_animation_copy(const Artwork &source,const Artwork &target,const AnimationCopyOptions &options,AnimationCopyPlan &out,std::string &error);
}
