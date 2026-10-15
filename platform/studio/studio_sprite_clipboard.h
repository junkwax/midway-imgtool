#pragma once
#include "studio_library_copy.h"
namespace studio {
// Session-local, owned sprite data. It neither borrows an open tab nor uses the
// OS/pixel clipboard. Paste reviews retain a shared snapshot until they close.
class SpriteClipboard {
public:
    static std::shared_ptr<const SpriteClipboard> capture(const Artwork &source,const LibraryCopyPlan &reviewed,const std::string &label,std::string &error);
    const Artwork &artwork()const{return *storage;}
    const SpriteScope &origin()const{return scope;}
    const std::string &label()const{return source_label;}
private:
    std::unique_ptr<Artwork> storage;
    SpriteScope scope;
    std::string source_label;
};
}
