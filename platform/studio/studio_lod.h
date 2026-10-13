#pragma once
#include "studio_document.h"
#include <string>
#include <vector>

namespace studio {
struct LodLibrary {
    int line=0,ppp=0,duplicate_of=-1;
    bool explicit_ppp=false,enabled=true;
    std::string source,path,resolution,error;
    std::unique_ptr<Artwork> artwork;
};
struct LodDirective { int line=0; std::string text; };
// Review owns decoded libraries. Nothing in the current workspace is changed.
struct LodReview {
    std::string path,override_directory,img_directory;
    std::vector<unsigned char> source_bytes;
    std::vector<LodLibrary> libraries;
    std::vector<LodDirective> directives;
};
bool review_lod(const std::string &path,const std::string &override_directory,
                const std::string &img_directory,LodReview &review,std::string &error);
// Validates every included source before any library is installed into the UI.
bool validate_lod(const LodReview &review,std::vector<size_t> &included,std::string &error);
}
