#pragma once
#include "studio_document.h"
#include "tbl_diff.h"
namespace studio {
struct TableComparison {
    const Artwork *owner=nullptr;uint64_t generation=0;
    std::string path,source_text,report;
    std::vector<TblEntry> table,images;
    std::vector<uint64_t> sprites;
    std::vector<TblDiffRow> rows;std::vector<std::string> warnings;
    TblDiffSummary summary{};
};
bool compare_table(const Artwork &art,const std::vector<uint64_t> &sprites,const std::string &path,TableComparison &out,std::string &error);
}
