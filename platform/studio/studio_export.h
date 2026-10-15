#pragma once
#include "studio_document.h"
#include <array>
namespace studio {
enum class ExportKind {Tbl,Irw,Anilst};
enum class ExportAlignment {Word,Nibble,Packed};
struct ExportOptions {
    ExportKind kind=ExportKind::Tbl;
    ExportAlignment alignment=ExportAlignment::Word;
    uint32_t base=0x02000000;
    int bpp=-1,bank=0; // -1 palette size, 0 used indices, 1..8 fixed.
    bool mk3=false,palette=true,dual_bank=false;
};
struct ExportRequest {uint64_t sprite=0;bool enabled=true;};
struct ExportRow {
    uint64_t sprite=0,address=0,sag=0,bits=0;
    bool enabled=true;int index=0,w=0,h=0,bpp=0,padding=0,entry=-1;
    uint16_t control=0;
    std::string name,palette,error;
};
struct ExportSource {
    std::array<unsigned char,sizeof(IMG)> fields{};
    std::array<unsigned char,sizeof(PAL)> palette_fields{};
    std::vector<unsigned char> pixels,colors;
};
struct ExportPlan {
    const Artwork *owner=nullptr;uint64_t generation=0;
    ExportOptions options;std::vector<ExportRequest> requests;
    std::vector<ExportRow> rows;std::vector<ExportSource> sources;
    std::vector<unsigned char> bytes;uint64_t packed_bytes=0;
    int included=0,blocked=0;
    bool ready=false;
};
struct ExportDestination {
    std::string path;bool existed=false;
    std::vector<unsigned char> previous;
};
const char *export_extension(ExportKind kind);
// Pure read-only planning. Blocked included rows stay visible for exclusion.
bool plan_export(const Artwork &art,const std::vector<ExportRequest> &requests,const ExportOptions &options,ExportPlan &plan,std::string &error);
bool review_export_destination(const Artwork &art,const ExportPlan &plan,const std::string &path,ExportDestination &destination,std::string &error);
// Re-derives source/layout/bytes and checks the reviewed destination before
// replacing one output file atomically. Never changes IMG flags or history.
bool write_export(const Artwork &art,const ExportPlan &plan,const ExportDestination &destination,std::string &error);
}
