#include "Calibrator.h"

#include "Util.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace ptk {

namespace {

// Matching landmarks: FO4 bone  <->  joint(s) of the Cyberpunk face rig (character-left side / centre).
struct Landmark {
    const char* fo4Bone;
    std::vector<const char*> cpJoints;   // averaged
    bool centreLine = false;             // force onto the mid plane
    double backMeters = 0;               // move backwards (eyeball centre sits behind the lids)
};

const std::vector<Landmark>& landmarkTable() {
    static const std::vector<Landmark> t = {
        {"skin_bone_C_Nose", {"mid_J_mug_nose_tip_rowA_0_JNT"}},
        {"skin_bone_C_Nose_Bridge", {"mid_J_nose_nosabial_rowB_0_JNT", "mid_J_eye_nose_rowA_0_JNT"}},
        {"skin_bone_C_EyebrowMid", {"mid_J_eye_brows_rowB_0_JNT"}},
        {"skin_bone_C_ForeheadMid", {"mid_J_eye_brows_rowD_0_JNT"}},
        {"skin_bone_C_MouthTop", {"l_J_mug_lip_up_0_JNT"}, true},
        {"skin_bone_C_MouthBot", {"l_J_mug_lip_dn_0_JNT"}, true},
        {"skin_bone_C_Chin", {"mid_J_mug_chin_rowA_1_JNT"}},
        {"skin_bone_L_Eye", {"l_J_eye_lid_corner_inside_0_JNT", "l_J_eye_lid_up_rowD_4_JNT"}, false, 0.012},
        {"skin_bone_L_MouthCorner", {"l_J_mug_mouth_rowA_3_JNT"}},
        {"skin_bone_L_Nostril", {"l_J_mug_nose_nostril_0_JNT"}},
        {"skin_bone_L_Ear", {"l_J_jaw_ear_0_JNT"}},
        {"skin_bone_L_JawSide", {"l_J_jaw_rowB_0_JNT"}},
        {"skin_bone_L_Cheekbone", {"l_J_eye_check_rowE_0_JNT"}},
        {"skin_bone_L_EyebrowIn", {"l_J_eye_brows_rowB_0_JNT"}},
        {"skin_bone_L_EyebrowOut", {"l_J_eye_brows_rowB_3_JNT"}},
    };
    return t;
}

// FO4 world (x right, y forward, z up) -> head-space component order (right, up, forward)
Vec3 fo4ToHeadOrder(const Vec3& v) { return {v.x, v.z, v.y}; }

} // namespace

CalibrationResult calibrateBones(const NifFile& faceHead, const NifFile& skeleton, const Glb& cpHead,
                                 const HeadFrame& frame) {
    CalibrationResult res;

    // ------------------------------------------------ FO4 head: skinned shape
    const NifShape* shape = nullptr;
    for (const auto& s : faceHead.shapes())
        if (!s.verts.empty() && !s.skinBones.empty() && s.skinR.size() == s.skinBones.size()) { shape = &s; break; }
    if (!shape) throw std::runtime_error("the FO4 head NIF has no skinned shape");

    // skin space -> world offset (vertices are stored relative to the HEAD bone)
    Vec3 off;
    int nOff = 0;
    for (size_t i = 0; i < shape->skinBones.size(); ++i) {
        const NifNode* n = faceHead.node(shape->skinBones[i]);
        if (!n || !n->hasWorld) continue;
        const Mat3& M = shape->skinR[i];
        const Vec3& T = shape->skinT[i];
        Vec3 pv(-(M(0, 0) * T.x + M(1, 0) * T.y + M(2, 0) * T.z),
                -(M(0, 1) * T.x + M(1, 1) * T.y + M(2, 1) * T.z),
                -(M(0, 2) * T.x + M(1, 2) * T.y + M(2, 2) * T.z));
        off += n->worldT - pv;
        ++nOff;
    }
    if (nOff == 0) throw std::runtime_error("the FO4 head NIF's skin bones have no nodes");
    off = off / nOff;

    // ------------------------------------------------ bone positions + axes
    std::map<std::string, Vec3> pos;       // FO4 world
    std::map<std::string, Mat3> axesRows;  // rows = bone X/Y/Z in FO4 world
    for (size_t i = 0; i < shape->skinBones.size(); ++i) {
        const std::string& n = shape->skinBones[i];
        if (const NifNode* nd = faceHead.node(n); nd && nd->hasWorld) pos[n] = nd->worldT;
        axesRows[n] = shape->skinR[i];
    }
    for (const auto& [name, nd] : skeleton.nodesByName()) {
        if (name.rfind("skin_bone_", 0) != 0 || !nd->hasWorld) continue;
        if (!pos.count(name)) pos[name] = nd->worldT;
        if (!axesRows.count(name)) {
            Mat3 rows;
            for (int k = 0; k < 3; ++k)
                for (int c = 0; c < 3; ++c) rows(k, c) = nd->worldR(c, k);   // column k -> row k
            axesRows[name] = rows;
        }
    }

    // ------------------------------------------------ landmark fit FO4 -> V (head space)
    auto cpPos = nodeWorldPositions(cpHead);
    auto toHead = [&](const Vec3& p) { return Vec3(dot(p, frame.right), dot(p, frame.up), dot(p, frame.forward)); };
    double midRight = 0;
    if (auto it = cpPos.find("mid_J_mug_nose_tip_rowA_0_JNT"); it != cpPos.end()) midRight = toHead(it->second).x;

    std::vector<std::pair<Vec3, Vec3>> pairs;   // (fo4 in head order, cp head)
    for (const auto& lm : landmarkTable()) {
        auto fp = pos.find(lm.fo4Bone);
        if (fp == pos.end()) continue;
        Vec3 sum;
        int n = 0;
        for (const char* j : lm.cpJoints)
            if (auto it = cpPos.find(j); it != cpPos.end()) { sum += toHead(it->second); ++n; }
        if (n != (int)lm.cpJoints.size()) continue;
        Vec3 h = sum / n;
        if (lm.centreLine) h.x = midRight;
        h.z -= lm.backMeters;
        pairs.push_back({fo4ToHeadOrder(fp->second), h});
    }
    if (pairs.size() < 6)
        throw std::runtime_error("V's head GLB has too few face-rig joints for calibration (" + std::to_string(pairs.size()) +
                                 " landmarks) - use the WolvenKit export of h0_000_p?a__morphs.morphtarget");
    double a[3], b[3];
    for (int k = 0; k < 3; ++k) {
        double mx = 0, my = 0;
        for (auto& [f, c] : pairs) { mx += f[k]; my += c[k]; }
        mx /= pairs.size(); my /= pairs.size();
        double sxy = 0, sxx = 0;
        for (auto& [f, c] : pairs) { sxy += (f[k] - mx) * (c[k] - my); sxx += (f[k] - mx) * (f[k] - mx); }
        a[k] = sxx > 0 ? sxy / sxx : 0.014;
        b[k] = my - a[k] * mx;
    }
    double errSum = 0, errMax = 0;
    for (auto& [f, c] : pairs) {
        Vec3 pr(a[0] * f.x + b[0], a[1] * f.y + b[1], a[2] * f.z + b[2]);
        double e = length(pr - c);
        errSum += e;
        errMax = std::max(errMax, e);
    }
    res.landmarks = (int)pairs.size();
    for (int k = 0; k < 3; ++k) { res.axisScale[k] = a[k]; res.axisOffset[k] = b[k]; }
    res.skinOffset = off;
    res.meanErrorMm = 1000.0 * errSum / pairs.size();
    res.maxErrorMm = 1000.0 * errMax;

    const Vec3 ext = frame.bboxMax - frame.bboxMin;
    auto toCpNorm = [&](const Vec3& fo4World) {
        Vec3 f = fo4ToHeadOrder(fo4World);
        Vec3 h(a[0] * f.x + b[0], a[1] * f.y + b[1], a[2] * f.z + b[2]);
        return Vec3((h.x - frame.bboxMin.x) / ext.x, (h.y - frame.bboxMin.y) / ext.y, (h.z - frame.bboxMin.z) / ext.z);
    };

    // ------------------------------------------------ skin-weight statistics (masters carry their children)
    std::map<std::string, int> skinIdx;
    for (size_t i = 0; i < shape->skinBones.size(); ++i) skinIdx[shape->skinBones[i]] = (int)i;

    BoneSet& set = res.bones;
    set.present = true;
    set.metersPerUnit = (std::fabs(a[0]) + std::fabs(a[1]) + std::fabs(a[2])) / 3.0;

    for (const auto& [name, p] : pos) {
        if (name.rfind("skin_bone_", 0) != 0 || !axesRows.count(name)) continue;
        std::set<int> group;
        if (auto it = skinIdx.find(name); it != skinIdx.end()) group.insert(it->second);
        for (const auto& d : skeleton.descendants(name))
            if (auto it = skinIdx.find(d); it != skinIdx.end()) group.insert(it->second);

        double W = 0;
        Vec3 S1, S2;
        for (const auto& v : shape->verts) {
            double w = 0;
            for (int k = 0; k < 4; ++k)
                if (v.weight[k] > 0.02f && group.count(v.bone[k])) w += v.weight[k];
            if (w <= 0) continue;
            Vec3 pw = v.pos + off;
            W += w;
            for (int k = 0; k < 3; ++k) { S1[k] += w * pw[k]; S2[k] += w * pw[k] * pw[k]; }
        }
        BoneLocation loc;
        loc.pivot = toCpNorm(p);
        loc.hasPivot = true;
        Vec3 sd(1, 1, 1);
        if (W > 0) {
            Vec3 mean = S1 / W;
            for (int k = 0; k < 3; ++k) sd[k] = std::sqrt(std::max(0.0, S2[k] / W - mean[k] * mean[k]));
            loc.center = toCpNorm(mean);
        } else {
            loc.center = loc.pivot;
        }
        Vec3 sdh = fo4ToHeadOrder(sd);
        for (int k = 0; k < 3; ++k) loc.radius[k] = std::max(0.008, 2.5 * sdh[k] * std::fabs(a[k])) / ext[k];
        const Mat3& R = axesRows[name];
        for (int k = 0; k < 3; ++k) loc.axes[k] = fo4ToHeadOrder(Vec3(R(k, 0), R(k, 1), R(k, 2)));
        loc.hasAxes = true;
        set.locations[toLower(name.substr(10))] = loc;   // "skin_bone_R_Eye" -> "r_eye"
    }
    if (set.locations.empty()) throw std::runtime_error("no FO4 face bones found");
    return res;
}

} // namespace ptk
