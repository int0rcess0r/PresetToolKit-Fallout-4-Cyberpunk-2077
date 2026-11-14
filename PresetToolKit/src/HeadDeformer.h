#pragma once
#include "Gltf.h"
#include "LooksMenuPreset.h"
#include "Mapping.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ptk {

// Orthonormal head frame (glTF/world space) + head-space bounding box.
struct HeadFrame {
    Vec3 right, up, forward;
    Vec3 bboxMin, bboxMax;   // in head space (right, up, forward)
};

struct DeformOptions {
    std::string meshFilter;          // only deform meshes whose name contains this (empty = all)
    double strength = 1.0;           // extra user multiplier
    bool adjustMorphTargets = true;  // rotate/scale existing CC morph deltas with the region
};

// Transform accumulated for one region (FO4 axes, normalised units).
struct RegionAccum {
    Vec3 translate, rotate;
    double scale = 0;
    int count = 0;
    std::vector<uint32_t> sourceIds;
};

// A preset sculpt that is applied through its Fallout 4 bones.
struct BoneSculpt {
    uint32_t id = 0;
    std::string name;
    std::vector<double> values;   // as stored in the preset
};

// Sculpt data resolved against the mapping (independent of any mesh).
struct SculptInput {
    std::vector<BoneSculpt> boneSculpts;            // via HumanRaceFacialBoneRegions*.txt
    std::map<std::string, RegionAccum> regions;     // fallback: coarse face regions
    std::vector<uint32_t> unmappedRegionIds;
    std::vector<uint32_t> unmappedValueIds;
    std::vector<uint32_t> ignoredPresets;
    std::vector<std::string> unknownBones;           // bones with no location in the mapping
    bool empty() const { return boneSculpts.empty() && regions.empty(); }
};

SculptInput resolveSculpts(const LooksMenuPreset& preset, const Mapping& mapping);

// Computes the head frame from a head GLB (the .morphtarget export is the reference).
HeadFrame computeHeadFrame(const Glb& glb, const Mapping& mapping, const std::string& meshFilter,
                           std::vector<std::string>& warnings);

struct DeformResult {
    size_t vertexCount = 0;
    size_t verticesMoved = 0;
    double maxDisplacement = 0;              // glTF units (metres)
    std::map<std::string, double> maxPerRegion;
    size_t morphTargetsAdjusted = 0;
    std::vector<std::string> warnings;
};

// Bakes the sculpt into the GLB in place: POSITION, NORMAL, TANGENT and (optionally) the
// POSITION deltas of the morph targets already in the file.
DeformResult deformHead(Glb& glb, const SculptInput& sculpt, const Mapping& mapping, const HeadFrame& frame,
                        const DeformOptions& opt);

std::string axisName(const Vec3& v);

} // namespace ptk
