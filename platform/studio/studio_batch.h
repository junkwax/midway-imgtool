#pragma once
#include "studio_document.h"
#include <array>
namespace studio {
enum class BatchGeometryKind {Trim,Resize,Canvas,Transform};
struct BatchGeometryOptions {
    BatchGeometryKind kind=BatchGeometryKind::Trim;
    int w=64,h=64,percent=100,placement=4;
    bool relative=true,quality=false,anchors=true;
    Transform transform=Transform::FlipHorizontal;
};
struct BatchGeometryRow {
    uint64_t sprite=0;bool enabled=true,changed=false,empty=false;
    std::string name;int before_w=0,before_h=0,w=0,h=0,cropped_pixels=0;
    unsigned short anix=0,aniy=0,anix2=0,aniy2=0;
    std::array<unsigned char,sizeof(IMG)> source_fields{};
    std::vector<unsigned char> before,pixels,before_points,points;
    std::vector<unsigned short> colors;
};
struct BatchGeometryPlan {
    const Artwork *owner=nullptr;uint64_t generation=0;
    BatchGeometryOptions options;std::vector<BatchGeometryRow> rows;
};
// Reuses the single-sprite commands on isolated scratch documents. Preview
// cannot mutate the source, its history or the classic document ownership.
bool plan_batch_geometry(const Artwork &art,const std::vector<uint64_t> &sprites,const BatchGeometryOptions &options,BatchGeometryPlan &out,std::string &error);
}
