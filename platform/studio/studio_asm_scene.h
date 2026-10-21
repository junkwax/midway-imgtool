#pragma once
#include "studio_asm_assets.h"
#include "studio_subframes.h"
namespace studio {
struct AsmSceneOptions {
    int hold=4,origin_x=200,origin_y=100;
    bool facing=false,loop=true;
    MirrorConvention convention=MirrorConvention_Ani2;
};
class AsmSceneSnapshot;
bool capture_asm_scene(const AsmSourceAnimation &,const std::vector<AsmLibrary> &,uint64_t preferred,
                       const AsmArtworkChoices &,const AsmSceneOptions &,
                       std::shared_ptr<const AsmSceneSnapshot> &out,std::string &error);
std::unique_ptr<SceneProject> create_asm_scene(const AsmSceneSnapshot &,const AsmSourceAnimation &,
                       const std::vector<AsmLibrary> &,uint64_t preferred,const AsmArtworkChoices &,std::string &error);
// Immutable reviewed preview; artwork is referenced, never copied or modified.
class AsmSceneSnapshot {
public:
    const SceneState &scene()const{return state;}
    size_t sprite_count()const{return sprites.size();}
private:
    friend bool capture_asm_scene(const AsmSourceAnimation &,const std::vector<AsmLibrary> &,uint64_t,const AsmArtworkChoices &,const AsmSceneOptions &,std::shared_ptr<const AsmSceneSnapshot> &,std::string &);
    friend std::unique_ptr<SceneProject> create_asm_scene(const AsmSceneSnapshot &,const AsmSourceAnimation &,const std::vector<AsmLibrary> &,uint64_t,const AsmArtworkChoices &,std::string &);
    SceneState state;AsmSourceAnimation animation;AsmSceneOptions options;
    std::vector<SubframeSpriteSnapshot> sprites;
    std::vector<uint64_t> generations;
};
}
