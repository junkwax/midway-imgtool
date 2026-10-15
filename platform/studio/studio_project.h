#pragma once
#include "studio_animation.h"
#include "studio_stage.h"
#include <functional>
#include <memory>

namespace studio {
struct SceneAsset {
    std::string path, absolute_path, digest;
    uint64_t runtime_doc=0;
};
struct SceneSprite {
    int asset=0, index=-1;
    std::string key, name;
    uint64_t runtime_id=0;
};
struct SceneFrame {
    std::string name;
    int ticks=default_preview_hold, x=0, y=0, z=0;
    bool flip_x=false, flip_y=false;
    std::vector<SceneSprite> pieces;
    int visible_from=0,visible_until=0; // Actor ticks; 0/0 follows the frame sequence.
    int vx=0,vy=0,cap_x=0,cap_y=0;    // Screen pixels/tick and maximum travel; 0 cap is unlimited.
    bool dual=false;
    int dual_x=0,dual_y=0,dual_z=0;    // Absolute second-copy frame offsets, not primary-relative.
    bool scheduled() const {return visible_from>0||visible_until>0;}
    uint64_t id=0; // Composition identity; independent of referenced IMG sprites.
};
struct SceneLane {
    std::string name="Actor";
    int x=0,y=0,delay=0,alpha=255;
    bool visible=true,loop=true,hold_end=true,flip_x=false;
    std::vector<SceneFrame> frames;
    int stop_tick=0; // 0 disables; otherwise freezes the actor's local clock.
    uint64_t id=0,trigger_lane=0,trigger_frame=0;
};
struct SceneState {
    int width=400,height=254,origin_x=200,origin_y=20,floor=234;
    bool onion=true,loop=true,ghost_facing=false;
    MirrorConvention convention=MirrorConvention_Ani2;
    double tick_hz=mk2_tick_hz;
    int background[3]={28,32,40};
    std::vector<SceneAsset> assets;
    std::vector<SceneLane> lanes;
};
struct SceneIssue { int asset=-1;std::string message; };
struct SceneBinding { uint64_t uid=0;Artwork *art=nullptr; };
using SceneSource = std::function<Artwork *(uint64_t)>;
using SceneBinder = std::function<SceneBinding(int,const SceneAsset &)>;
struct ScenePose {
    SceneSprite sprite;
    int x=0,y=0,z=0,alpha=255,lane=0,frame=0;
    bool flip_x=false,flip_y=false;
    bool dual=false;
};
int scene_duration(const SceneState &scene);
int scene_frame_at(const SceneLane &lane,int tick);
int scene_frame_start(const SceneLane &lane,int frame);
int scene_frame_seek_tick(const SceneLane &lane,int frame);
int scene_last_tick(const SceneState &scene);
int scene_lane_start(const SceneState &scene,int lane);
int scene_frame_at(const SceneState &scene,int lane,int tick);
int scene_frame_seek_tick(const SceneState &scene,int lane,int frame);
bool scene_set_trigger(SceneState &scene,int lane,uint64_t source_lane,uint64_t source_frame,std::string &error);
bool scene_detach_trigger(SceneState &scene,int lane,std::string &error);
bool scene_uses_lane(const SceneState &scene,uint64_t lane);
bool scene_uses_frame(const SceneState &scene,uint64_t frame);
std::vector<ScenePose> evaluate_scene(const SceneState &scene,int tick);
std::string scene_sprite_key(const IMG &image);
SceneSprite scene_sprite_reference(const Artwork &art,int asset,int index);
std::string scene_asset_candidate(const SceneAsset &asset,const std::string &project_path);
bool validate_scene(const SceneState &scene,std::string &error);

// A composition has its own history and saved revision. Artwork stays in its
// original documents. The codec never calls the classic workspace loader.
class SceneProject {
public:
    explicit SceneProject(SceneState initial,bool unsaved=true);
    static std::unique_ptr<SceneProject> read(const std::string &path,std::string &error);
    bool bind(const SceneBinder &provider,std::vector<SceneIssue> &issues);
    void establish_loaded();
    bool save(const std::string &path,const SceneSource &source,std::string &error);
    bool export_png(const std::string &path,int tick,const SceneSource &source,std::string &error,
                    const StageBackdrop *stage=nullptr,const StageView *view=nullptr) const;
    void begin(const std::string &label);
    void commit();
    void cancel();
    void undo();
    void redo();
    bool can_undo() const { return !past.empty(); }
    bool can_redo() const { return !future.empty(); }
    bool dirty() const { return pending ? !same(state,pending->state)||revision!=saved_revision : revision!=saved_revision; }
    bool transaction_open() const {return pending!=nullptr;}
    const std::string &path() const {return file_path;}
    const std::string &opened_from() const {return original_path;}
    bool imported_wax() const {return imported;}
    bool uses_doc(uint64_t uid) const;
    uint64_t allocate_id(){return next_entity_id++;}
    SceneState state;
private:
    struct Snapshot {SceneState state;uint64_t revision=0;std::string label;};
    static bool same(const SceneState &a,const SceneState &b);
    std::unique_ptr<Snapshot> pending;
    std::vector<Snapshot> past,future;
    uint64_t revision=0,saved_revision=0,next_revision=1;
    std::string file_path,original_path,disk_baseline;
    bool imported=false;
    uint64_t next_entity_id=1;
};
}
