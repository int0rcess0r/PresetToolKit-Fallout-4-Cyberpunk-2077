#include "HeadDeformer.h"

#include "Util.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <stdexcept>

namespace ptk {

std::string axisName(const Vec3& v);

namespace {

Vec3 axisFromString(const std::string& s, bool& ok) {
    ok = true;
    std::string a = toLower(s);
    double sign = 1;
    if (!a.empty() && (a[0] == '+' || a[0] == '-')) { sign = a[0] == '-' ? -1 : 1; a = a.substr(1); }
    if (a == "x") return {sign, 0, 0};
    if (a == "y") return {0, sign, 0};
    if (a == "z") return {0, 0, sign};
    ok = false;
    return {};
}

int dominantAxis(const Vec3& v) {
    int best = 0;
    for (int i = 1; i < 3; ++i)
        if (std::fabs(v[i]) > std::fabs(v[best])) best = i;
    return best;
}

double clampd(double v, double lim) { return lim > 0 ? std::max(-lim, std::min(lim, v)) : v; }
double slot(const std::vector<double>& v, int idx) { return (idx >= 0 && idx < (int)v.size()) ? v[idx] : 0.0; }

bool meshSelected(const std::string& name, const std::string& filter) {
    return filter.empty() || toLower(name).find(toLower(filter)) != std::string::npos;
}

// World-space positions of every selected primitive.
std::vector<Vec3> gatherWorldPositions(const Glb& glb, const std::string& filter) {
    std::vector<Vec3> pts;
    for (const auto& inst : collectMeshInstances(glb)) {
        if (!meshSelected(inst.name, filter)) continue;
        for (const auto& prim : glb.doc["meshes"][inst.mesh]["primitives"]) {
            if (!prim.contains("attributes") || !prim["attributes"].contains("POSITION")) continue;
            for (const auto& p : readVec3Accessor(glb, prim["attributes"]["POSITION"].get<int>()))
                pts.push_back(inst.world.apply(p));
        }
    }
    return pts;
}

// Lateral axis = most mirror-symmetric horizontal axis. Face side = the side whose
// centre-line profile is rougher (nose, lips, brow) than the smooth back of the skull.
Vec3 detectForward(const std::vector<Vec3>& pts, const Vec3& up, std::vector<std::string>& warnings) {
    int upAxis = dominantAxis(up);
    int cand[2], n = 0;
    for (int i = 0; i < 3; ++i)
        if (i != upAxis) cand[n++] = i;
    Vec3 mn{1e30, 1e30, 1e30}, mx{-1e30, -1e30, -1e30}, mean;
    for (const auto& p : pts) {
        for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], p[k]); mx[k] = std::max(mx[k], p[k]); }
        mean += p;
    }
    mean = mean / (double)std::max<size_t>(1, pts.size());
    auto asym = [&](int k) { double e = mx[k] - mn[k]; return e > 0 ? std::fabs(mean[k] - 0.5 * (mn[k] + mx[k])) / e : 0; };
    int lateral = asym(cand[0]) < asym(cand[1]) ? cand[0] : cand[1];
    int fwd = lateral == cand[0] ? cand[1] : cand[0];

    double upSign = up[upAxis];
    double latMid = 0.5 * (mn[lateral] + mx[lateral]), latExt = mx[lateral] - mn[lateral];
    double upExt = mx[upAxis] - mn[upAxis];
    const int bins = 32;
    std::vector<double> hi(bins, -1e30), lo(bins, 1e30);
    for (const auto& p : pts) {
        if (std::fabs(p[lateral] - latMid) > 0.06 * latExt) continue;
        double v = (p[upAxis] - mn[upAxis]) / (upExt > 0 ? upExt : 1);
        if (upSign < 0) v = 1 - v;
        if (v < 0.3 || v > 0.95) continue;
        int b = std::min(bins - 1, (int)((v - 0.3) / 0.65 * bins));
        hi[b] = std::max(hi[b], p[fwd]);
        lo[b] = std::min(lo[b], p[fwd]);
    }
    auto roughness = [&](const std::vector<double>& prof) {
        double r = 0;
        for (int i = 1; i + 1 < bins; ++i) {
            if (std::fabs(prof[i - 1]) > 1e29 || std::fabs(prof[i]) > 1e29 || std::fabs(prof[i + 1]) > 1e29) continue;
            r += std::fabs(prof[i - 1] - 2 * prof[i] + prof[i + 1]);
        }
        return r;
    };
    double rp = roughness(hi), rn = roughness(lo);
    Vec3 f;
    f[fwd] = rp >= rn ? 1.0 : -1.0;
    if (std::fabs(rp - rn) < 1e-9 * (std::fabs(rp) + std::fabs(rn) + 1e-12))
        warnings.push_back("forward axis auto-detection was inconclusive; set frame.forward in the mapping");
    return f;
}

// Forward direction from the face rig, if the GLB has one (WolvenKit exports do): from the
// Head/Neck joint towards the nose-tip joint, snapped to the nearest horizontal axis.
std::optional<Vec3> forwardFromJoints(const Glb& glb, const Vec3& up) {
    if (!glb.doc.contains("nodes")) return std::nullopt;
    std::optional<Vec3> nose, head;
    for (const auto& n : glb.doc["nodes"]) {
        if (!n.contains("name") || !n.contains("translation")) continue;
        std::string name = toLower(n["name"].get<std::string>());
        const auto& t = n["translation"];
        Vec3 p(t[0].get<double>(), t[1].get<double>(), t[2].get<double>());
        if (!nose && name.find("nose_tip") != std::string::npos) nose = p;
        if (name == "head" || (!head && name == "neck1") || (!head && name == "neck")) head = p;
    }
    if (!nose || !head) return std::nullopt;
    Vec3 d = *nose - *head;
    d = d - up * dot(d, up);
    if (length(d) < 1e-6) return std::nullopt;
    int k = dominantAxis(d);
    Vec3 f;
    f[k] = d[k] > 0 ? 1.0 : -1.0;
    return f;
}

// One placed copy of a region / bone (mirrored regions produce two).
struct Placed {
    std::string region;
    Vec3 c, r, T;   // influence centre, radii, translation (head space)
    Mat3 RS;        // rotation * scale, head space
    double gain;
    Vec3 pivot;     // rotation / scale pivot (head space)
};

} // namespace

std::string axisName(const Vec3& v) {
    const char* n[3] = {"x", "y", "z"};
    for (int i = 0; i < 3; ++i) {
        if (v[i] > 0.5) return std::string("+") + n[i];
        if (v[i] < -0.5) return std::string("-") + n[i];
    }
    return "?";
}

SculptInput resolveSculpts(const LooksMenuPreset& preset, const Mapping& mapping) {
    SculptInput in;
    const double lim = mapping.clampValue;
    for (const auto& rs : preset.regions) {
        if (auto it = mapping.fo4Regions.find(rs.id); it != mapping.fo4Regions.end() && !it->second.bones.empty()) {
            in.boneSculpts.push_back({rs.id, it->second.name, rs.values});
            for (const auto& b : it->second.bones) {
                char side; std::string key;
                splitBoneName(b.name, side, key);
                if (!mapping.findBone(side, key, nullptr) &&
                    std::find(in.unknownBones.begin(), in.unknownBones.end(), b.name) == in.unknownBones.end())
                    in.unknownBones.push_back(b.name);
            }
            continue;
        }
        auto reg = mapping.regionForId(rs.id);
        if (!reg) { in.unmappedRegionIds.push_back(rs.id); continue; }
        auto& acc = in.regions[*reg];
        acc.translate += Vec3(clampd(slot(rs.values, mapping.idxPos[0]), lim), clampd(slot(rs.values, mapping.idxPos[1]), lim),
                              clampd(slot(rs.values, mapping.idxPos[2]), lim));
        acc.rotate += Vec3(clampd(slot(rs.values, mapping.idxRot[0]), lim), clampd(slot(rs.values, mapping.idxRot[1]), lim),
                           clampd(slot(rs.values, mapping.idxRot[2]), lim));
        acc.scale += clampd(slot(rs.values, mapping.idxScale), lim);
        acc.count++;
        acc.sourceIds.push_back(rs.id);
    }
    for (const auto& [id, value] : preset.morphValues) {
        auto it = mapping.valueRules.find(id);
        if (it == mapping.valueRules.end()) { in.unmappedValueIds.push_back(id); continue; }
        const auto& rule = it->second;
        double v = clampd(value, lim);
        auto& acc = in.regions[rule.region];
        acc.translate += rule.translate * v;
        acc.rotate += rule.rotate * v;
        acc.scale += rule.scale * v;
        acc.count++;
        acc.sourceIds.push_back(id);
    }
    for (const auto& kv : preset.presetWeights) in.ignoredPresets.push_back(kv.first);
    if (toLower(mapping.combine) == "average")
        for (auto& kv : in.regions) {
            auto& a = kv.second;
            if (a.count > 1) { a.translate = a.translate / a.count; a.rotate = a.rotate / a.count; a.scale /= a.count; }
        }
    return in;
}

HeadFrame computeHeadFrame(const Glb& glb, const Mapping& mapping, const std::string& meshFilter,
                           std::vector<std::string>& warnings) {
    auto pts = gatherWorldPositions(glb, meshFilter);
    if (pts.empty()) throw std::runtime_error("no vertices found in the head GLB (check --mesh-filter)");
    HeadFrame f;
    bool ok = true;
    if (toLower(mapping.up) == "auto") {
        Vec3 mn{1e30, 1e30, 1e30}, mx{-1e30, -1e30, -1e30};
        for (const auto& p : pts)
            for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], p[k]); mx[k] = std::max(mx[k], p[k]); }
        f.up[dominantAxis(mx - mn)] = 1;
    } else {
        f.up = axisFromString(mapping.up, ok);
        if (!ok) throw std::runtime_error("mapping frame.up must be +x/-x/+y/-y/+z/-z or auto");
    }
    if (toLower(mapping.forward) == "auto") {
        if (auto fj = forwardFromJoints(glb, f.up)) f.forward = *fj;
        else {
            f.forward = detectForward(pts, f.up, warnings);
            warnings.push_back("no face rig joints found - forward axis guessed from the mesh shape (" + axisName(f.forward) +
                               "); set frame.forward in the mapping if the face is on the wrong side");
        }
    }
    else {
        f.forward = axisFromString(mapping.forward, ok);
        if (!ok) throw std::runtime_error("mapping frame.forward must be +x/-x/+y/-y/+z/-z or auto");
    }
    if (std::fabs(dot(f.up, f.forward)) > 0.5) throw std::runtime_error("frame.up and frame.forward are the same axis");
    f.right = cross(f.forward, f.up);

    f.bboxMin = {1e30, 1e30, 1e30};
    f.bboxMax = {-1e30, -1e30, -1e30};
    for (const auto& p : pts) {
        Vec3 h(dot(p, f.right), dot(p, f.up), dot(p, f.forward));
        for (int k = 0; k < 3; ++k) { f.bboxMin[k] = std::min(f.bboxMin[k], h[k]); f.bboxMax[k] = std::max(f.bboxMax[k], h[k]); }
    }
    return f;
}

DeformResult deformHead(Glb& glb, const SculptInput& sculpt, const Mapping& mapping, const HeadFrame& frame,
                        const DeformOptions& opt) {
    DeformResult res;
    const Vec3 ext = frame.bboxMax - frame.bboxMin;
    const double width = ext.x > 0 ? ext.x : 1.0;

    // Basis matrix B (columns right, up, forward): world = B * head, head = B^T * world
    Mat3 B;
    for (int r = 0; r < 3; ++r) { B(r, 0) = frame.right[r]; B(r, 1) = frame.up[r]; B(r, 2) = frame.forward[r]; }
    Mat3 Bt;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) Bt(r, c) = B(c, r);

    // ------------------------------------------------ place regions
    std::vector<Placed> placed;
    auto makeR = [](const Vec3& d) { return rotZ(deg2rad(d.z)) * rotY(deg2rad(d.y)) * rotX(deg2rad(d.x)); };
    for (const auto& reg : mapping.regions) {
        auto it = sculpt.regions.find(reg.name);
        if (it == sculpt.regions.end()) continue;
        const auto& a = it->second;
        // FO4 axes (x right, y forward, z up) -> head axes (right, up, forward)
        Vec3 t = Vec3(a.translate.x, a.translate.z, a.translate.y) * (mapping.positionGain * width);
        Vec3 rdeg = Vec3(a.rotate.x, a.rotate.z, a.rotate.y) * mapping.rotationGainDeg;
        double s = std::max(0.1, 1.0 + a.scale * mapping.scaleGain);
        Mat3 S; S(0,0) = S(1,1) = S(2,2) = s;
        Vec3 c = frame.bboxMin + Vec3(reg.center.x * ext.x, reg.center.y * ext.y, reg.center.z * ext.z);
        Vec3 r(std::max(1e-6, reg.radius.x * ext.x), std::max(1e-6, reg.radius.y * ext.y), std::max(1e-6, reg.radius.z * ext.z));
        double gain = reg.strength * mapping.globalStrength * opt.strength;
        placed.push_back({reg.name, c, r, t, makeR(rdeg) * S, gain, c});
        if (reg.mirror) {
            Vec3 cm = c; cm.x = frame.bboxMin.x + (1.0 - reg.center.x) * ext.x;
            Vec3 tm = t; tm.x = -tm.x;
            Vec3 rm = rdeg; rm.y = -rm.y; rm.z = -rm.z;
            placed.push_back({reg.name, cm, r, tm, makeR(rm) * S, gain, cm});
        }
    }

    // ------------------------------------------------ place Fallout 4 bones (exact sculpt data)
    auto lerpSigned = [](double v, double mx, double mn) { return v >= 0 ? v * mx : -v * mn; };
    for (const auto& bs : sculpt.boneSculpts) {
        const auto& reg = mapping.fo4Regions.at(bs.id);
        const double lim = mapping.clampValue;
        Vec3 vp(clampd(slot(bs.values, mapping.idxPos[0]), lim), clampd(slot(bs.values, mapping.idxPos[1]), lim),
                clampd(slot(bs.values, mapping.idxPos[2]), lim));
        Vec3 vr(clampd(slot(bs.values, mapping.idxRot[0]), lim), clampd(slot(bs.values, mapping.idxRot[1]), lim),
                clampd(slot(bs.values, mapping.idxRot[2]), lim));
        double vs = clampd(slot(bs.values, mapping.idxScale), lim);
        for (const auto& b : reg.bones) {
            char side; std::string key;
            splitBoneName(b.name, side, key);
            bool mirrored = false;
            const BoneLocation* loc = mapping.findBone(side, key, &mirrored);
            if (!loc) continue;
            // values in the bone's own frame (FO4 Minima/Maxima)
            Vec3 t, rdeg, sc;
            for (int k = 0; k < 3; ++k) {
                t[k] = lerpSigned(vp[k], b.posMax[k], b.posMin[k]);
                rdeg[k] = lerpSigned(vr[k], b.rotMax[k], b.rotMin[k]);
                sc[k] = lerpSigned(vs, b.sclMax[k], b.sclMin[k]);
            }
            t = t * mapping.fo4UnitMeters;
            rdeg = rdeg * mapping.fo4RotationScale;
            sc = sc * mapping.fo4ScaleScale;
            if (length(t) < 1e-9 && length(rdeg) < 1e-9 && length(sc) < 1e-9) continue;

            // bone frame -> head frame. Calibrated bones carry their real axes; otherwise the
            // bone frame is taken as x = character right, y = up, z = forward.
            Mat3 A; // columns = bone axes in head space
            if (loc->hasAxes)
                for (int r = 0; r < 3; ++r)
                    for (int k = 0; k < 3; ++k) A(r, k) = loc->axes[k][r];
            Mat3 S;
            for (int k = 0; k < 3; ++k) S(k, k) = std::max(0.1, 1.0 + sc[k]);
            Mat3 At;
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) At(r, k) = A(k, r);
            Vec3 tHead = A * t;
            Mat3 RSHead = A * makeR(rdeg) * S * At;

            auto place = [&](Vec3 cn, Vec3 pv) {
                if (mirrored) { cn.x = 1.0 - cn.x; pv.x = 1.0 - pv.x; }
                return std::make_pair(frame.bboxMin + Vec3(cn.x * ext.x, cn.y * ext.y, cn.z * ext.z),
                                      frame.bboxMin + Vec3(pv.x * ext.x, pv.y * ext.y, pv.z * ext.z));
            };
            auto [c, pivot] = place(loc->center, loc->hasPivot ? loc->pivot : loc->center);
            if (mirrored) {
                // reuse of a right-side entry: mirror the transform across the centre plane
                Mat3 Mx; Mx(0, 0) = -1;
                tHead.x = -tHead.x;
                RSHead = Mx * RSHead * Mx;
            }
            const Vec3& rn = loc->radius;
            Vec3 r(std::max(1e-6, rn.x * ext.x), std::max(1e-6, rn.y * ext.y), std::max(1e-6, rn.z * ext.z));
            double gain = mapping.fo4Strength * mapping.globalStrength * opt.strength;
            placed.push_back({reg.name, c, r, tHead, RSHead, gain, pivot});
        }
    }

    // Deformation of one world point: new position and local linear map (world space).
    auto deformPoint = [&](const Vec3& pw, Vec3& outPos, Mat3& outLin, std::map<std::string, double>* perRegion) {
        Vec3 q = Bt * pw;
        Vec3 delta;
        Mat3 L; // identity
        Mat3 I;
        for (const auto& p : placed) {
            Vec3 d = q - p.c;
            double d2 = (d.x / p.r.x) * (d.x / p.r.x) + (d.y / p.r.y) * (d.y / p.r.y) + (d.z / p.r.z) * (d.z / p.r.z);
            if (d2 >= 1.0) continue;
            double w = (1.0 - d2) * (1.0 - d2) * p.gain;
            Vec3 qn = p.pivot + p.T + p.RS * (q - p.pivot);
            Vec3 dd = (qn - q) * w;
            delta += dd;
            for (int k = 0; k < 9; ++k) L.m[k] += w * (p.RS.m[k] - I.m[k]);
            if (perRegion) { double& m = (*perRegion)[p.region]; m = std::max(m, length(dd)); }
        }
        outPos = pw + B * delta;
        outLin = B * L * Bt;
    };

    // ------------------------------------------------ apply to every selected primitive
    std::set<int> done; // accessors already rewritten (shared accessors)
    for (const auto& inst : collectMeshInstances(glb)) {
        if (!meshSelected(inst.name, opt.meshFilter)) continue;
        const Mat3 M = inst.world.l, Minv = inst.world.l.inverse();
        auto& prims = glb.doc["meshes"][inst.mesh]["primitives"];
        for (size_t pi = 0; pi < prims.size(); ++pi) {
            auto attrs = prims[pi].value("attributes", nlohmann::json::object());
            if (!attrs.contains("POSITION")) continue;
            int posAcc = attrs["POSITION"].get<int>();
            if (done.count(posAcc)) continue;
            done.insert(posAcc);

            auto pos = readVec3Accessor(glb, posAcc);
            res.vertexCount += pos.size();
            std::vector<Mat3> lin(pos.size());
            std::vector<float> newPos(pos.size() * 3);
            for (size_t v = 0; v < pos.size(); ++v) {
                Vec3 pw = inst.world.apply(pos[v]), np;
                deformPoint(pw, np, lin[v], &res.maxPerRegion);
                double moved = length(np - pw);
                if (moved > 1e-7) res.verticesMoved++;
                res.maxDisplacement = std::max(res.maxDisplacement, moved);
                Vec3 nl = Minv * (np - inst.world.t);
                newPos[v * 3] = (float)nl.x; newPos[v * 3 + 1] = (float)nl.y; newPos[v * 3 + 2] = (float)nl.z;
                lin[v] = Minv * lin[v] * M; // into mesh-local space
            }
            writeFloatAccessor(glb, posAcc, newPos, /*forceMinMax*/ true);

            // Normals / tangents: rotate by the local linear map, renormalise.
            for (const char* sem : {"NORMAL", "TANGENT"}) {
                if (!attrs.contains(sem)) continue;
                int a = attrs[sem].get<int>();
                if (done.count(a)) continue;
                done.insert(a);
                int comps = 0;
                auto data = readFloatAccessor(glb, a, comps);
                if (comps < 3 || data.size() / comps != pos.size()) { res.warnings.push_back(std::string(sem) + " accessor layout unexpected; left unchanged"); continue; }
                for (size_t v = 0; v < pos.size(); ++v) {
                    Vec3 n(data[v * comps], data[v * comps + 1], data[v * comps + 2]);
                    Vec3 nn = normalize(lin[v] * n);
                    data[v * comps] = (float)nn.x; data[v * comps + 1] = (float)nn.y; data[v * comps + 2] = (float)nn.z;
                }
                writeFloatAccessor(glb, a, data);
            }

            // Existing morph targets (CC face options): keep them working on the new shape.
            if (opt.adjustMorphTargets && prims[pi].contains("targets")) {
                for (auto& t : prims[pi]["targets"]) {
                    if (!t.contains("POSITION")) continue;
                    int a = t["POSITION"].get<int>();
                    if (done.count(a)) continue;
                    done.insert(a);
                    int comps = 0;
                    auto data = readFloatAccessor(glb, a, comps);
                    if (comps != 3 || data.size() / 3 != pos.size()) continue;
                    for (size_t v = 0; v < pos.size(); ++v) {
                        Vec3 d(data[v * 3], data[v * 3 + 1], data[v * 3 + 2]);
                        Vec3 nd = lin[v] * d;
                        data[v * 3] = (float)nd.x; data[v * 3 + 1] = (float)nd.y; data[v * 3 + 2] = (float)nd.z;
                    }
                    if (writeFloatAccessor(glb, a, data, true)) res.morphTargetsAdjusted++;
                }
            }
        }
    }
    if (res.vertexCount == 0) throw std::runtime_error("no mesh primitives were deformed (check --mesh-filter)");
    return res;
}

} // namespace ptk
