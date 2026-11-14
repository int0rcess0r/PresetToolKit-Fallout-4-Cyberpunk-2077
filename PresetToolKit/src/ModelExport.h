#pragma once
#include "MathUtil.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ptk {

// A static triangle mesh for exporting (glTF frame: metres, Y up).
struct ModelPart {
    std::string name;
    std::vector<Vec3> pos;
    std::vector<Vec3> nrm;                       // empty: computed (smooth, across UV seams)
    std::vector<std::array<float, 2>> uv;        // may be empty
    std::vector<uint32_t> idx;
};

// Smooth vertex normals; vertices at the same position (UV seams) share theirs.
void computeSmoothNormals(ModelPart& part);

// Writes the parts as .glb (binary glTF) or .obj (Wavefront), chosen by the file extension.
void writeModel(const std::vector<ModelPart>& parts, const std::filesystem::path& out);

} // namespace ptk
