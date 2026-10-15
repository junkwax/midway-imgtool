#pragma once
#include "studio_document.h"
#include <array>
namespace studio {
enum class BatchRenameKind {Numbered,Prepend,TailNumbered};
struct BatchRenameOptions {BatchRenameKind kind=BatchRenameKind::Numbered;std::string text="SPRITE";int start=1,padding=0;};
struct BatchRenameRow {
    uint64_t sprite=0;bool enabled=true,changed=false;std::string before,after;
    std::array<char,16> visible{},raw{};
};
struct BatchRenamePlan {
    const Artwork *owner=nullptr;uint64_t generation=0;BatchRenameOptions options;std::vector<BatchRenameRow> rows;
};
// Numbering follows library order, including excluded rows, so toggling a row
// never silently renumbers other previewed targets. No truncation or auto-suffixing.
bool plan_batch_rename(const Artwork &art,const std::vector<uint64_t> &sprites,const BatchRenameOptions &options,BatchRenamePlan &out,std::string &error);
// Returns the number of changed, included rows, or -1. Issues are aligned to rows;
// exclusions are checked against final names across the entire library.
int check_batch_rename(const Artwork &art,const BatchRenamePlan &plan,std::vector<std::string> &issues,std::string &error);
}
