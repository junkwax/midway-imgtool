#pragma once
#include "studio_animation_copy.h"
namespace studio {
// Owns only the captured records and their required sprites/palettes. Independent
// of source tabs, disk files and the pixel, palette and sprite clipboards.
class AnimationClipboard {
public:
    static std::shared_ptr<const AnimationClipboard> capture(const Artwork &source,const AnimationCopyPlan &reviewed,const std::string &label,std::string &error);
    const Artwork &artwork()const{return *storage;}
    const std::vector<AnimationCopyRow> &origin()const{return records;}
    const std::string &label()const{return source_label;}
private:
    std::unique_ptr<Artwork> storage;
    std::vector<AnimationCopyRow> records;
    std::string source_label;
};
}
