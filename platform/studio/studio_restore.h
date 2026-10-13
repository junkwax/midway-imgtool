#pragma once
#include "studio_document.h"
#include <array>
namespace studio {
enum class RestoreMode {Reconstruct,Diff,Replace,FillTransparent};
enum class RestoreMatch {Names,SelectedParent};
enum class RestoreStatus {Ready,NoPatternMatch,MissingParent,AmbiguousParent,Self,PaletteMismatch,MissingBaseline,NoOverlap};
struct RestoreOptions {RestoreMode mode=RestoreMode::Reconstruct;RestoreMatch matching=RestoreMatch::Names;uint64_t parent=0;std::string pattern="^(.+)[A-Z]$";};
struct RestoreSnapshot {
    uint64_t id=0;std::string name;int w=0,h=0,anix=0,aniy=0,palette=0;
    std::array<unsigned char,sizeof(IMG)> fields{};
    std::vector<unsigned char> pixels,baseline;std::vector<unsigned short> colors;
};
struct RestoreRow {
    uint64_t child=0,parent=0;std::string child_name,parent_name;
    RestoreStatus status=RestoreStatus::Ready;bool enabled=false;
    int dx=0,dy=0,covered=0,total=0,changed=0,added=0,erased=0;
    std::vector<unsigned char> pixels;
};
struct RestorePlan {const Artwork *owner=nullptr;uint64_t generation=0;RestoreOptions options;std::vector<RestoreSnapshot> sources;std::vector<RestoreRow> rows;};
const RestoreSnapshot *restore_source(const RestorePlan &plan,uint64_t id);
bool plan_restore(const Artwork &art,const std::vector<uint64_t> &children,const RestoreOptions &options,RestorePlan &out,std::string &error);
}
