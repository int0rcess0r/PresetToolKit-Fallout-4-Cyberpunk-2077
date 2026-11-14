#pragma once
#include "MathUtil.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ptk {

// Minimal binary glTF (.glb) container: JSON document + the single BIN chunk.
// This is the format WolvenKit exports .mesh/.morphtarget files to.
struct Glb {
    nlohmann::json doc;
    std::vector<uint8_t> bin;
};

Glb loadGlb(const std::filesystem::path& path);
void saveGlb(const Glb& glb, const std::filesystem::path& path);

// Reads a float VEC3 accessor (POSITION / NORMAL / morph deltas).
std::vector<Vec3> readVec3Accessor(const Glb& glb, int accessorIndex);

// Reads/writes any float accessor as flat components (VEC3 -> 3 per element, VEC4 -> 4).
std::vector<float> readFloatAccessor(const Glb& glb, int accessorIndex, int& components);
std::vector<uint32_t> readIndexAccessor(const Glb& glb, int accessorIndex);   // UBYTE / USHORT / UINT indices
// Overwrites the accessor's data in place (same count/layout). Updates min/max if present
// or if `forceMinMax`. Returns false if the accessor has no bufferView (implicit zeros).
bool writeFloatAccessor(Glb& glb, int accessorIndex, const std::vector<float>& data, bool forceMinMax = false);

// Each mesh with the world transform of the first node that instances it.
struct MeshInstance {
    int mesh = -1;
    std::string name;
    Affine world;
};
std::vector<MeshInstance> collectMeshInstances(const Glb& glb);

// World-space position of every named node (rig joints included).
std::map<std::string, Vec3> nodeWorldPositions(const Glb& glb);

} // namespace ptk
