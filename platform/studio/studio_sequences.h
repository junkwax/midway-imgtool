#pragma once
#include "studio_project.h"
#include <array>

namespace studio {
struct EmbeddedEntryValues {int index=0,ticks=4,dx=0,dy=0;};
struct EmbeddedEntry {
    EmbeddedEntryValues values;
    std::array<unsigned char,18> raw{};
};
struct EmbeddedRecord {
    bool script=false;
    std::string name;
    int flags=0,start_x=0,start_y=0;
    std::array<unsigned char,98> raw{};
    // Playback order: reverse of disk order for sequences; forward for scripts.
    std::vector<EmbeddedEntry> entries;
};
bool read_embedded(const Artwork &art,std::vector<EmbeddedRecord> &records,std::string &error);
int embedded_duration(const EmbeddedRecord &record);
int embedded_frame_at(const EmbeddedRecord &record,int tick);
std::unique_ptr<SceneProject> embedded_scene(const Artwork &art,uint64_t uid,int record,std::string &error,bool flip=false,MirrorConvention convention=MirrorConvention_Ani2);
}
