#pragma once
#include "Gltf.h"
#include "HeadDeformer.h"
#include "Mapping.h"
#include "Nif.h"

#include <string>
#include <vector>

namespace ptk {

struct CalibrationResult {
    BoneSet bones;
    double meanErrorMm = 0, maxErrorMm = 0;
    int landmarks = 0;
    std::vector<std::string> notes;
    // The landmark fit itself: Fallout 4 world -> V's head space (right, up, forward), per axis
    // h[k] = axisScale[k] * f[k] + axisOffset[k] with f = (x, z, y) of the FO4 world position.
    double axisScale[3] = {0, 0, 0}, axisOffset[3] = {0, 0, 0};
    Vec3 skinOffset;   // skin-space vertex -> Fallout 4 world
    Vec3 toHead(const Vec3& fo4World) const {
        return {axisScale[0] * fo4World.x + axisOffset[0], axisScale[1] * fo4World.z + axisOffset[1],
                axisScale[2] * fo4World.y + axisOffset[2]};
    }
};

// Places every Fallout 4 face bone on V's head:
//   faceHead : Base{Female,Male}Head_faceBones.nif  (bone positions/orientations, skin weights)
//   skeleton : skeleton[_female]_faceBones.nif      (master bones and their children)
//   cpHead   : V's head GLB with its face rig (WolvenKit export), and its head frame
// FO4 -> V uses a per-axis least-squares fit over matching facial landmarks.
// Throws if the inputs lack what is needed (no skinned shape, missing rig joints...).
CalibrationResult calibrateBones(const NifFile& faceHead, const NifFile& skeleton, const Glb& cpHead,
                                 const HeadFrame& frame);

} // namespace ptk
