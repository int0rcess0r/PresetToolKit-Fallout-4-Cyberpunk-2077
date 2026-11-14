#pragma once
#include "Calibrator.h"
#include "LooksMenuPreset.h"
#include "Mapping.h"
#include "Nif.h"
#include "Tri.h"

#include <string>
#include <vector>

namespace ptk {

// Fallout 4's own face sculpting, simulated on its head mesh: every sculpt region moves its face
// bones (FO4 Maxima/Minima), master bones carry their children, and the head's skin weights blend
// the bones into the vertices - the same thing the game does.
class Fo4FaceModel {
public:
    Fo4FaceModel(const NifFile& head, const NifFile& skeleton, const Mapping& mapping, const CalibrationResult& cal);

    size_t vertexCount() const { return base_.size(); }
    // Head vertices in V's head space (metres; right, up, forward), with `regions` applied.
    std::vector<Vec3> evaluate(const std::vector<RegionSculpt>& regions) const;
    const std::vector<Vec3>& basePositions() const { return baseHead_; }
    // Vertices skinned to a sculptable face bone.
    bool isFace(size_t i) const;

    // Fallout 4's face presets / sliders (Morphs.Presets): .tri morphs applied before the bones.
    void setFaceMorphs(const TriFile& chargenTri, const std::map<uint32_t, std::string>& presets,
                       const std::map<uint32_t, std::pair<std::string, std::string>>& sliders);
    bool hasFaceMorphs() const { return !morph_.empty(); }
    // Keys the model knows: preset key -> morph, slider key -> (negative, positive) morph
    const std::map<uint32_t, std::string>& presetKeys() const { return presetKeys_; }
    const std::map<uint32_t, std::pair<std::string, std::string>>& sliderKeys() const { return sliderKeys_; }
    // With face presets / slider values (key -> weight, as in a LooksMenu preset).
    std::vector<Vec3> evaluate(const std::vector<RegionSculpt>& regions, const std::map<uint32_t, double>& presets) const;

private:
    struct Xf { Vec3 pivot; Mat3 A; };   // bone pivot + axes (columns) in FO4 world
    std::vector<Vec3> base_;              // FO4 world
    std::vector<Vec3> baseHead_;          // V's head space
    std::vector<std::array<int, 4>> bone_;
    std::vector<std::array<float, 4>> weight_;
    std::vector<std::vector<std::string>> chain_;   // per skin bone: sculptable names, self first then ancestors
    std::map<std::string, Xf> xf_;
    const Mapping& mapping_;
    CalibrationResult cal_;
    std::map<std::string, std::vector<Vec3>> morph_;           // per model vertex
    std::map<uint32_t, std::string> presetKeys_;
    std::map<uint32_t, std::pair<std::string, std::string>> sliderKeys_;
    std::vector<Vec3> withMorphs(const std::map<uint32_t, double>& presets) const;
};

struct NativeFitOptions {
    double regularization = 3e-3;   // ridge, relative to the mean basis energy
    int icpIterations = 6;          // correspondence updates
    double maxDistanceMm = 20;      // ignore matches farther apart than this
    bool rotations = true;
    bool align = true;              // first line the heads up on the face surface (per-axis scale + offset)
    bool faceMorphs = true;         // also fit Fallout 4's face presets / sliders (needs setFaceMorphs)
};

struct NativeFitResult {
    std::vector<RegionSculpt> regions;
    std::map<uint32_t, double> presets;   // face presets / sliders (Morphs.Presets)
    size_t points = 0;              // Fallout 4 face vertices matched to V's face
    double beforeMm = 0;            // mean surface distance, Fallout 4 default face -> V
    double afterMm = 0;             // ... with the fitted sculpts
    double alignedMm = 0;           // default face after aligning the heads (what the sliders start from)
    Vec3 alignScale{1, 1, 1}, alignOffset;   // head-space alignment used (x' = s*x + o)
    std::vector<std::string> notes;
};

// Fits Fallout 4 sculpt values so that Fallout 4's head, sculpted, matches V's face surface
// (`vPos` / `vNrm`: V's head vertices and normals in head space) as closely as the sliders allow.
NativeFitResult fitFo4SculptsNative(const Fo4FaceModel& model, const Mapping& mapping, const std::vector<Vec3>& vPos,
                                    const std::vector<Vec3>& vNrm, const NativeFitOptions& opt = {});

// Lines Fallout 4's default face up with V's face (head space): per-axis scale + offset, trimmed ICP.
struct HeadAlignment {
    Vec3 scale{1, 1, 1}, offset;
    Vec3 point(const Vec3& p) const { return {scale.x * p.x + offset.x, scale.y * p.y + offset.y, scale.z * p.z + offset.z}; }
    Vec3 vec(const Vec3& d) const { return {scale.x * d.x, scale.y * d.y, scale.z * d.z}; }
};
HeadAlignment alignFo4Face(const Fo4FaceModel& model, const std::vector<Vec3>& vPos);

// Fallout 4 -> V: sculpts Fallout 4's own head with `regions` and carries the movement of its face
// over to V's vertices (`vPos`, head space) with a smooth field. Returns head-space displacements.
std::vector<Vec3> transferFo4Sculpt(const Fo4FaceModel& model, const std::vector<RegionSculpt>& regions,
                                    const HeadAlignment& align, const std::vector<Vec3>& vPos,
                                    const std::map<uint32_t, double>& presets = {});

// Mean surface distance (mm) between Fallout 4's head with `regions` and V's face, over the face
// vertices that the fit uses; for comparing presets.
double nativeSurfaceDistanceMm(const Fo4FaceModel& model, const std::vector<RegionSculpt>& regions,
                               const std::vector<Vec3>& vPos, const std::vector<Vec3>& vNrm, double maxDistanceMm = 20,
                               Vec3 alignScale = {1, 1, 1}, Vec3 alignOffset = {});

} // namespace ptk
