#pragma once
#include <memory>
#include <string>
#include <vector>
namespace studio {
struct StagePlane {
    std::string name;
    int x1=0,y1=0,x2=0,y2=0,objects=0,playfield=0;
    bool content=false;
    int cx1=0,cy1=0,cx2=0,cy2=0;
};
// Owned read-only scenery. No classic globals, renderer ownership or save path.
struct StageBackdrop {
    std::string name,bdd_path,bdb_path;
    int world_w=0,world_h=0,x=0,y=0,w=0,h=0,objects=0,skipped=0,invalid_pixels=0;
    std::vector<StagePlane> planes;
    std::vector<unsigned char> rgba;
};
struct StageView {
    bool enabled=true;
    int camera_x=0,camera_y=0,alpha=255,plane=-1;
};
std::shared_ptr<const StageBackdrop> read_stage(const std::string &path,std::string &error);
int stage_playfield_plane(const StageBackdrop &stage);
void stage_align_plane(const StageBackdrop &stage,StageView &view,int plane,int width,int floor);
// Same integer camera/alpha as the UI. The target is an opaque RGBA playfield.
bool blit_stage(const StageBackdrop &stage,const StageView &view,int width,int height,
                std::vector<unsigned char> &target,std::string &error);
}
