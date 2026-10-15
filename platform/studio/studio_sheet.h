#pragma once
#include "studio_legacy_media.h"
#include "studio_export.h"
namespace studio {
struct SheetOptions {int width=256,gap=2;bool rgba=false,anf=true;};
struct SheetRow {
    uint64_t sprite=0;bool enabled=true;int index=0,x=0,y=0,w=0,h=0,entry=-1;
    std::string name,error;
};
struct SheetPlan {
    const Artwork *owner=nullptr;uint64_t generation=0;SheetOptions options;
    std::vector<ExportRequest> requests;std::vector<SheetRow> rows;
    std::vector<IndexedMedia> sources;std::vector<unsigned char> pixels,tga,anf;
    std::vector<unsigned short> words;int height=0,included=0,blocked=0;
    bool ready=false,merged=false;
};
struct SheetDestination {
    std::string path;bool existed=false;
    std::vector<unsigned char> previous;
};
bool plan_sheet(const Artwork &art,const std::vector<ExportRequest> &requests,const SheetOptions &options,SheetPlan &out,std::string &error);
std::vector<unsigned char> render_sheet(const SheetPlan &plan);
bool review_sheet_destinations(const Artwork &art,const SheetPlan &plan,const std::string &path,std::vector<SheetDestination> &out,std::string &error);
// Prepares both outputs before replacement. A failed second replacement rolls
// the first back when it still matches our output. Two paths are not globally
// atomic against process/power loss or concurrent external changes.
bool write_sheet(const Artwork &art,const SheetPlan &plan,const std::vector<SheetDestination> &destinations,std::string &error);
}
