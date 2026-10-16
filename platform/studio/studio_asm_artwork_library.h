#pragma once
#include "studio_asm_assets.h"
namespace studio {
struct AsmArtworkOrigin {
    std::string symbol,name,library;uint64_t document=0,sprite=0,generation=0;int target=0,uses=0;
};
// Owned, immutable review. Creating a library never re-reads or edits sources.
class AsmArtworkLibrary {
public:
    static std::shared_ptr<const AsmArtworkLibrary> capture(const AsmSourceAnimation &animation,
        const std::vector<AsmLibrary> &libraries,uint64_t preferred,const AsmArtworkChoices &choices,
        bool allow_partial,std::string &error);
    const Artwork &artwork()const{return *storage;}
    const std::vector<AsmArtworkOrigin> &origins()const{return rows;}
    const std::string &label()const{return animation_label;}
    bool partial()const{return partial_preview;}
    std::unique_ptr<Artwork> create_library(std::string &error)const;
private:
    AsmArtworkLibrary()=default;
    std::unique_ptr<Artwork> storage;std::vector<AsmArtworkOrigin> rows;
    std::string animation_label;bool partial_preview=false;
};
}
