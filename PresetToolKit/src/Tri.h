#pragma once
#include "MathUtil.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ptk {

// FaceGen .tri (FRTRI003) as used by Fallout 4: base vertices and named morph deltas.
struct TriFile {
    std::vector<Vec3> verts;
    std::map<std::string, std::vector<Vec3>> morphs;   // name -> delta per vertex
};
TriFile loadTri(const std::vector<uint8_t>& bytes);

} // namespace ptk
