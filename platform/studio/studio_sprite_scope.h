#pragma once
#include "studio_document.h"
namespace studio {
struct SpriteScopeRow {
    uint64_t sprite=0,parent=0; // parent=0 means explicitly selected by the scope
    std::string parent_name;
    bool operator==(const SpriteScopeRow &b)const{return sprite==b.sprite&&parent==b.parent&&parent_name==b.parent_name;}
};
struct SpriteScope {
    bool include_children=false;int ambiguous=0;
    std::vector<uint64_t> requested;
    std::vector<std::string> skipped;
    std::vector<SpriteScopeRow> rows;
    bool operator==(const SpriteScope &b)const{return include_children==b.include_children&&ambiguous==b.ambiguous&&requested==b.requested&&skipped==b.skipped&&rows==b.rows;}
};
// Classic direct-child rules: BASE1A, BASE_1A, BASE_1 and BASE1.
// Match names and source-library labels exactly; never recurse or add siblings.
// Multiple possible parent sprites are ambiguous and are not added implicitly.
bool review_sprite_scope(const Artwork &art,const std::vector<uint64_t> &requested,bool children,SpriteScope &out,std::string &error);
}
