#pragma once
#include "studio_sequences.h"
namespace studio {
struct EmbeddedMetadataValues {
    int flags=0,start_x=0,start_y=0;
    std::array<int,6> damage{};
    std::array<int,3> spare{};
};
struct EmbeddedMetadataSource {
    const Artwork *owner=nullptr;uint64_t generation=0;
    unsigned disk_version=0,version=0,sequences=0,scripts=0,damage_count=0;
    int record=-1,entry=-1;size_t offset=0,entry_offset=0;
    bool script=false;std::string name;
    std::vector<unsigned char> blob,damage;
    std::array<unsigned char,4> buffer_script{};
    std::vector<std::string> damage_labels;
    std::vector<bool> damage_valid;
    EmbeddedMetadataValues original;
};
struct EmbeddedMetadataPlan {
    std::shared_ptr<const EmbeddedMetadataSource> source;
    EmbeddedMetadataValues values;
    std::array<unsigned char,98> header{};
    std::array<unsigned char,18> entry{};
    int changed_bytes=0;
};
bool capture_embedded_metadata(const Artwork &,int record,int entry,std::shared_ptr<const EmbeddedMetadataSource> &,std::string &error);
bool plan_embedded_metadata(std::shared_ptr<const EmbeddedMetadataSource>,const EmbeddedMetadataValues &,EmbeddedMetadataPlan &,std::string &error);
}
