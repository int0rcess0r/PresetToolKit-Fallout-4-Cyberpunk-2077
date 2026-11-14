#pragma once
#include <array>
#include "MathUtil.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ptk {

// Minimal Fallout 4 NIF (20.2.0.7, BS version 130) reader: what the face calibration and the model
// export need.
struct NifNode {
    std::string name;
    std::string type;
    Mat3 localR;           // rows/cols as stored (local -> parent: v' = R*v*s + t)
    Vec3 localT;
    double localS = 1.0;
    std::vector<int> children;
    // world transform (filled by NifFile)
    Mat3 worldR;           // columns = node axes in world
    Vec3 worldT;
    double worldS = 1.0;
    bool hasWorld = false;
};

struct NifSkinnedVertex {
    Vec3 pos;              // as stored in the shape (skin space)
    uint8_t bone[4] = {0, 0, 0, 0};
    float weight[4] = {0, 0, 0, 0};
};

struct NifShape {
    std::string name;
    std::vector<NifSkinnedVertex> verts;
    std::vector<std::string> skinBones;        // BSSkin::Instance bone list (names)
    std::vector<Mat3> skinR;                   // BSSkin::BoneData: world(skin) -> bone rotation
    std::vector<Vec3> skinT;
    std::vector<double> skinS;
    std::vector<std::array<float, 2>> uv;      // first UV set (when the shape has one)
    std::vector<std::array<uint16_t, 3>> tris;
};

class NifFile {
public:
    explicit NifFile(const std::vector<uint8_t>& bytes, const std::string& label = "nif");

    const std::map<std::string, NifNode*>& nodesByName() const { return byName_; }
    const NifNode* node(const std::string& name) const;
    const std::vector<NifShape>& shapes() const { return shapes_; }
    // all named descendants of a node (depth-first)
    std::vector<std::string> descendants(const std::string& name) const;

private:
    std::vector<NifNode> nodes_;               // indexed by block number (non-nodes are empty)
    std::vector<bool> isNode_;
    std::map<std::string, NifNode*> byName_;
    std::vector<NifShape> shapes_;
    void computeWorld(int idx, const Mat3& R, const Vec3& T, double S, int depth);
};

} // namespace ptk
