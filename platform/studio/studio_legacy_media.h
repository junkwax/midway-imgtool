#pragma once
#include "studio_import.h"
namespace studio {
enum class LegacyMediaKind {Lbm,Tga};
struct IndexedMedia {
    std::string path,note;
    int w=0,h=0,anchor_x=0,anchor_y=0;
    std::vector<unsigned short> words;
    std::vector<unsigned char> pixels; // Visible rows, no IMG padding.
};
bool decode_indexed_media(const std::vector<unsigned char> &bytes,LegacyMediaKind kind,IndexedMedia &out,std::string &error);
bool read_indexed_media(const std::string &path,IndexedMedia &out,std::string &error);
bool build_indexed_import(const Artwork &art,const IndexedMedia &media,const std::string &name,int palette,int anchor_x,int anchor_y,ImportBatch &out,std::string &error);
bool snapshot_indexed_sprite(const Artwork &art,int sprite,IndexedMedia &out,std::string &error);
bool encode_indexed_media(const IndexedMedia &media,LegacyMediaKind kind,std::vector<unsigned char> &bytes,std::string &error);
bool export_indexed_sprite(const Artwork &art,int sprite,const std::string &path,LegacyMediaKind kind,std::string &error);
struct IndexedExportReview {
    const Artwork *owner=nullptr;uint64_t generation=0,sprite=0;
    LegacyMediaKind kind=LegacyMediaKind::Lbm;
    IndexedMedia media;std::string path;bool existed=false;
    std::vector<unsigned char> bytes,previous;
};
bool review_indexed_export(const Artwork &art,int sprite,const std::string &path,LegacyMediaKind kind,IndexedExportReview &out,std::string &error);
bool write_indexed_export(const Artwork &art,const IndexedExportReview &review,std::string &error);
}
