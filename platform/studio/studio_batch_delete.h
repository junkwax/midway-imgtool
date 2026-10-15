#pragma once
#include "studio_document.h"
#include <array>
#include "studio_sprite_scope.h"
namespace studio {
struct BatchDeleteReference {std::string name;int record=0,entries=0;};
struct BatchDeleteRow {
    uint64_t sprite=0;bool enabled=true;int index=0,w=0,h=0;std::string name;
    std::array<unsigned char,sizeof(IMG)> fields{};
    std::vector<unsigned char> pixels;std::vector<unsigned short> colors;
    std::vector<BatchDeleteReference> references;
};
struct BatchDeletePlan {
    const Artwork *owner=nullptr;uint64_t generation=0;unsigned sequences=0,scripts=0;
    SpriteScope scope;std::vector<unsigned char> embedded;std::vector<BatchDeleteRow> rows;
};
// Referenced rows remain visible for explicit exclusion; no references are removed.
bool plan_batch_delete(const Artwork &art,const std::vector<uint64_t> &sprites,BatchDeletePlan &out,std::string &error,bool include_children=false);
// UI checks every open composition again at Apply; stable IDs survive index shifts.
std::vector<std::string> batch_delete_scene_uses(const BatchDeletePlan &plan,uint64_t document,const struct SceneState &scene,const std::string &name);
}
