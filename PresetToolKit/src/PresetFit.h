#pragma once
#include "HeadDeformer.h"

#include <map>
#include <string>
#include <vector>

namespace ptk {

// Per-vertex displacement (glTF units) of V's base head produced by the character-creator
// choices (region -> target name such as "h141"), read from the head GLB's morph targets.
// With `relativeToAverage`, each region's displacement is measured from the average of all of that
// region's options (the "average V"), which is what Fallout 4's all-zero sliders correspond to.
std::vector<Vec3> ccMorphDisplacement(const Glb& head, const std::map<std::string, std::string>& choices,
                                      std::vector<std::string>& used, std::vector<std::string>& missing,
                                      bool relativeToAverage = true);

struct FitOptions {
    double regularization = 2e-3;   // ridge weight, relative to the mean basis energy
    bool rotations = false;         // also fit region rotations (usually not needed)
    int refinements = 3;            // outer passes that correct for non-linear overlap
};

struct FitResult {
    std::vector<RegionSculpt> regions;   // Fallout 4 sculpt values ([px,py,pz,rx,ry,rz,scale,0])
    size_t vertices = 0;                 // vertices the target moves
    double targetRmsMm = 0, targetMaxMm = 0;
    double residualRmsMm = 0, residualMaxMm = 0;
    std::vector<std::string> notes;
};

// Finds Fallout 4 face sculpt values (for the regions in mapping.fo4Regions) whose effect on
// `head`, through the same bone model used for Fallout 4 -> Cyberpunk, best reproduces `target`.
// `head` is left unchanged.
FitResult fitFo4Sculpts(const Glb& head, const std::vector<Vec3>& target, const Mapping& mapping,
                        const HeadFrame& frame, const FitOptions& opt = {});

} // namespace ptk
