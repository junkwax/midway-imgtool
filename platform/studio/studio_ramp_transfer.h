#pragma once
#include "studio_batch_palette.h"
#include "palette_transfer.h"
namespace studio {
struct RampTransferOptions {
    int source=-1,target=-1,ppp=6,mode=kPaletteTransferRelative;
    float source_split=30,target_split=30;
    // Empty chooses automatic suggestions; otherwise one target block index
    // per source block, with -1 explicitly using nearest-color fallback.
    std::vector<int> pairs;
};
struct RampTransferPlan {
    RampTransferOptions options;
    BatchPalettePlan preview;
    std::array<unsigned char,sizeof(PAL)> source_fields{},target_fields{};
    std::vector<unsigned short> source_words;
    std::vector<TransferBlock> source_blocks,target_blocks;
    std::vector<int> pairs;
    std::array<unsigned char,256> mapping{};
    std::array<uint64_t,256> usage{};
    std::vector<uint64_t> block_usage;
    std::vector<std::vector<unsigned char>> alternates;
    uint64_t fallback_pixels=0;
};
bool plan_ramp_transfer(const Artwork &art,const std::vector<uint64_t> &sprites,const RampTransferOptions &options,RampTransferPlan &plan,std::string &error);
}
