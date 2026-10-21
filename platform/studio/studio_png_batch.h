#pragma once
#include "studio_export.h"
#include "studio_legacy_media.h"
namespace studio {
struct PngBatchRow {
    uint64_t sprite=0;bool enabled=true,existed=false;
    std::string name,filename,path,error;
    IndexedMedia source;
    std::vector<unsigned char> png,previous;
};
struct PngBatchPlan {
    const Artwork *owner=nullptr;uint64_t generation=0;
    std::string folder;std::vector<std::string> protected_paths;
    std::vector<ExportRequest> requests;std::vector<PngBatchRow> rows;
    int included=0,blocked=0,replacements=0;bool ready=false;
};
// Reviews an existing folder without changing it. Filenames are unique even on
// case-insensitive filesystems; unsafe name characters are replaced with '_'.
bool plan_png_batch(const Artwork &art,const std::vector<ExportRequest> &requests,
    const std::string &folder,const std::vector<std::string> &protected_paths,
    PngBatchPlan &out,std::string &error);
// Prepares every PNG and backup before publishing. Ordinary failures roll back
// earlier outputs when they still match our bytes. A batch is not globally
// atomic against interruption or concurrent external changes.
bool write_png_batch(const Artwork &art,const PngBatchPlan &plan,std::string &error);
}
