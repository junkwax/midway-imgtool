#pragma once
#include "studio_sequences.h"
namespace studio {
struct AnimationDependencies {
    std::vector<int> records, sprites, damage;
    std::string error;
};
// Damage slots are signed-byte indices into {kind, record-index} word pairs.
// Only sequences interpret their six damage bytes. Script bytes stay opaque.
bool read_animation_dependencies(const Artwork &,const std::vector<EmbeddedRecord> &,
                                 std::vector<unsigned char> &damage,
                                 std::vector<AnimationDependencies> &,std::string &error);
}
