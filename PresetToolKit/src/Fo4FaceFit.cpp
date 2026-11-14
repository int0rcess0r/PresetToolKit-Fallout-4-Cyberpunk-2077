#include "Fo4FaceFit.h"
#include "Util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace ptk {

namespace {

Mat3 transpose(const Mat3& m) {
    Mat3 t;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) t(r, c) = m(c, r);
    return t;
}
Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 o;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            double s = 0;
            for (int k = 0; k < 3; ++k) s += a(r, k) * b(k, c);
            o(r, c) = s;
        }
    return o;
}
Mat3 rotXYZ(const Vec3& deg) {
    auto rad = [](double d) { return d * 3.14159265358979323846 / 180.0; };
    const double cx = std::cos(rad(deg.x)), sx = std::sin(rad(deg.x));
    const double cy = std::cos(rad(deg.y)), sy = std::sin(rad(deg.y));
    const double cz = std::cos(rad(deg.z)), sz = std::sin(rad(deg.z));
    Mat3 X, Y, Z;
    X(1, 1) = cx; X(1, 2) = -sx; X(2, 1) = sx; X(2, 2) = cx;
    Y(0, 0) = cy; Y(0, 2) = sy; Y(2, 0) = -sy; Y(2, 2) = cy;
    Z(0, 0) = cz; Z(0, 1) = -sz; Z(1, 0) = sz; Z(1, 1) = cz;
    return mul(mul(Z, Y), X);
}
double lerpSigned(double v, double mx, double mn) { return v >= 0 ? v * mx : -v * mn; }
double slot(const std::vector<double>& v, int i) { return i >= 0 && i < (int)v.size() ? v[i] : 0.0; }

// Uniform grid over V's vertices for nearest-neighbour queries.
struct Grid {
    double cell;
    std::unordered_map<long long, std::vector<int>> cells;
    const std::vector<Vec3>& pts;
    Grid(const std::vector<Vec3>& p, double c) : cell(c), pts(p) {
        for (int i = 0; i < (int)p.size(); ++i) cells[key(p[i])].push_back(i);
    }
    long long k3(long long x, long long y, long long z) const {
        return ((x + 100000) * 200003LL + (y + 100000)) * 200003LL + (z + 100000);
    }
    long long key(const Vec3& v) const {
        return k3((long long)std::floor(v.x / cell), (long long)std::floor(v.y / cell), (long long)std::floor(v.z / cell));
    }
    int nearest(const Vec3& q, double maxD) const {
        const int r = (int)std::ceil(maxD / cell);
        const long long cx = (long long)std::floor(q.x / cell), cy = (long long)std::floor(q.y / cell),
                        cz = (long long)std::floor(q.z / cell);
        int best = -1;
        double bd = maxD * maxD;
        for (int dx = -r; dx <= r; ++dx)
            for (int dy = -r; dy <= r; ++dy)
                for (int dz = -r; dz <= r; ++dz) {
                    auto it = cells.find(k3(cx + dx, cy + dy, cz + dz));
                    if (it == cells.end()) continue;
                    for (int i : it->second) {
                        Vec3 d = pts[i] - q;
                        double dd = dot(d, d);
                        if (dd < bd) { bd = dd; best = i; }
                    }
                }
        return best;
    }
};

} // namespace

// ======================================================================== model
Fo4FaceModel::Fo4FaceModel(const NifFile& head, const NifFile& skeleton, const Mapping& mapping,
                           const CalibrationResult& cal)
    : mapping_(mapping), cal_(cal) {
    const NifShape* shape = nullptr;
    for (const auto& s : head.shapes())
        if (!s.verts.empty() && !s.skinBones.empty() && s.skinR.size() == s.skinBones.size()) { shape = &s; break; }
    if (!shape) throw std::runtime_error("the Fallout 4 head NIF has no skinned shape");

    // sculptable bones: everything the regions reference ("bone_X" -> node "skin_bone_X")
    std::set<std::string> sculptable;
    for (const auto& [id, r] : mapping.fo4Regions)
        for (const auto& b : r.bones) sculptable.insert("skin_" + b.name);

    auto xfFromSkeleton = [&](const std::string& n, Xf& out) {
        const NifNode* nd = skeleton.node(n);
        if (!nd || !nd->hasWorld) nd = head.node(n);
        if (!nd || !nd->hasWorld) return false;
        out.pivot = nd->worldT;
        out.A = nd->worldR;   // columns = axes
        return true;
    };
    for (size_t i = 0; i < shape->skinBones.size(); ++i) {
        const std::string& n = shape->skinBones[i];
        Xf x;
        const NifNode* nd = head.node(n);
        if (nd && nd->hasWorld) x.pivot = nd->worldT;
        else if (!xfFromSkeleton(n, x)) continue;
        x.A = transpose(shape->skinR[i]);   // rows of skinR = bone axes
        xf_[n] = x;
    }
    for (const auto& n : sculptable)
        if (!xf_.count(n)) {
            Xf x;
            if (xfFromSkeleton(n, x)) xf_[n] = x;
        }

    // per skin bone: the sculptable bones that move it (itself, then ancestors - children first)
    std::map<std::string, std::vector<std::string>> movedBy;
    std::map<std::string, size_t> subtree;
    for (const auto& s : sculptable) {
        auto desc = skeleton.descendants(s);
        subtree[s] = desc.size();
        movedBy[s].push_back(s);
        for (const auto& d : desc) movedBy[d].push_back(s);
    }
    chain_.resize(shape->skinBones.size());
    for (size_t i = 0; i < shape->skinBones.size(); ++i) {
        auto c = movedBy[shape->skinBones[i]];
        std::sort(c.begin(), c.end(), [&](const std::string& a, const std::string& b) { return subtree[a] < subtree[b]; });
        c.erase(std::unique(c.begin(), c.end()), c.end());
        chain_[i] = c;
    }

    for (const auto& v : shape->verts) {
        base_.push_back(v.pos + cal.skinOffset);
        std::array<int, 4> b{};
        std::array<float, 4> w{};
        for (int k = 0; k < 4; ++k) { b[k] = v.bone[k]; w[k] = v.weight[k]; }
        bone_.push_back(b);
        weight_.push_back(w);
        baseHead_.push_back(cal.toHead(base_.back()));
    }
}

bool Fo4FaceModel::isFace(size_t i) const {
    for (int k = 0; k < 4; ++k) {
        int b = bone_[i][k];
        if (weight_[i][k] > 0.01f && b >= 0 && b < (int)chain_.size() && !chain_[b].empty()) return true;
    }
    return false;
}

void Fo4FaceModel::setFaceMorphs(const TriFile& tri, const std::map<uint32_t, std::string>& presets,
                                 const std::map<uint32_t, std::pair<std::string, std::string>>& sliders) {
    // model vertex -> .tri vertex: same order when the counts match, otherwise by position
    std::vector<int> map(base_.size(), -1);
    if (tri.verts.size() == base_.size()) {
        for (size_t i = 0; i < map.size(); ++i) map[i] = int(i);
    } else {
        for (size_t i = 0; i < base_.size(); ++i) {
            const Vec3 p = base_[i] - cal_.skinOffset;
            double bd = 0.05 * 0.05;
            for (size_t k = 0; k < tri.verts.size(); ++k) {
                const Vec3 d = tri.verts[k] - p;
                const double l = dot(d, d);
                if (l < bd) { bd = l; map[i] = int(k); }
            }
        }
    }
    auto take = [&](const std::string& name) {
        if (name.empty() || morph_.count(name)) return !name.empty() && morph_.count(name) > 0;
        auto it = tri.morphs.find(name);
        if (it == tri.morphs.end()) return false;
        std::vector<Vec3> d(base_.size());
        bool any = false;
        for (size_t i = 0; i < base_.size(); ++i)
            if (map[i] >= 0 && size_t(map[i]) < it->second.size()) {
                d[i] = it->second[size_t(map[i])];
                any |= length(d[i]) > 1e-7;
            }
        if (!any) return false;   // DefaultFaceType0 and other empty morphs
        morph_[name] = std::move(d);
        return true;
    };
    for (const auto& [k, m] : presets)
        if (take(m)) presetKeys_[k] = m;
    for (const auto& [k, mm] : sliders) {
        const bool a = take(mm.first), b = take(mm.second);
        if (a || b) sliderKeys_[k] = mm;
    }
}

std::vector<Vec3> Fo4FaceModel::withMorphs(const std::map<uint32_t, double>& presets) const {
    std::vector<Vec3> p = base_;
    auto add = [&](const std::string& name, double w) {
        auto it = morph_.find(name);
        if (it == morph_.end() || w == 0) return;
        for (size_t i = 0; i < p.size(); ++i) p[i] += it->second[i] * w;
    };
    for (const auto& [k, w] : presets) {
        if (auto it = presetKeys_.find(k); it != presetKeys_.end()) add(it->second, w);
        else if (auto is = sliderKeys_.find(k); is != sliderKeys_.end()) add(w < 0 ? is->second.first : is->second.second, std::fabs(w));
    }
    return p;
}

std::vector<Vec3> Fo4FaceModel::evaluate(const std::vector<RegionSculpt>& regions) const {
    return evaluate(regions, {});
}

std::vector<Vec3> Fo4FaceModel::evaluate(const std::vector<RegionSculpt>& regions, const std::map<uint32_t, double>& presets) const {
    const std::vector<Vec3> start = presets.empty() || morph_.empty() ? base_ : withMorphs(presets);
    struct Applied { Vec3 pivot, T; Mat3 RS; };
    std::map<std::string, std::vector<Applied>> applied;
    const Mapping& m = mapping_;
    for (const auto& rs : regions) {
        auto it = m.fo4Regions.find(rs.id);
        if (it == m.fo4Regions.end()) continue;
        const double lim = m.clampValue;
        auto cl = [&](double v) { return std::clamp(v, -lim, lim); };
        Vec3 vp(cl(slot(rs.values, m.idxPos[0])), cl(slot(rs.values, m.idxPos[1])), cl(slot(rs.values, m.idxPos[2])));
        Vec3 vr(cl(slot(rs.values, m.idxRot[0])), cl(slot(rs.values, m.idxRot[1])), cl(slot(rs.values, m.idxRot[2])));
        double vs = cl(slot(rs.values, m.idxScale));
        for (const auto& b : it->second.bones) {
            const std::string name = "skin_" + b.name;
            auto xi = xf_.find(name);
            if (xi == xf_.end()) continue;
            Vec3 t, rdeg, sc;
            for (int k = 0; k < 3; ++k) {
                t[k] = lerpSigned(vp[k], b.posMax[k], b.posMin[k]);
                rdeg[k] = lerpSigned(vr[k], b.rotMax[k], b.rotMin[k]) * m.fo4RotationScale;
                sc[k] = lerpSigned(vs, b.sclMax[k], b.sclMin[k]) * m.fo4ScaleScale;
            }
            if (length(t) < 1e-12 && length(rdeg) < 1e-12 && length(sc) < 1e-12) continue;
            Mat3 S;
            for (int k = 0; k < 3; ++k) S(k, k) = std::max(0.1, 1.0 + sc[k]);
            const Mat3& A = xi->second.A;
            applied[name].push_back({xi->second.pivot, A * t, mul(mul(mul(A, rotXYZ(rdeg)), S), transpose(A))});
        }
    }
    std::vector<Vec3> out(base_.size());
    // which skin bones move at all
    std::vector<char> moving(chain_.size(), 0);
    for (size_t b = 0; b < chain_.size(); ++b)
        for (const auto& n : chain_[b])
            if (applied.count(n)) moving[b] = 1;
    for (size_t i = 0; i < base_.size(); ++i) {
        const Vec3& p = start[i];
        Vec3 d;
        for (int k = 0; k < 4; ++k) {
            const int b = bone_[i][k];
            const float w = weight_[i][k];
            if (w <= 0 || b < 0 || b >= (int)chain_.size() || !moving[b]) continue;
            Vec3 q = p;
            for (const auto& n : chain_[b]) {
                auto it = applied.find(n);
                if (it == applied.end()) continue;
                for (const auto& a : it->second) q = a.pivot + a.T + a.RS * (q - a.pivot);
            }
            d += (q - p) * w;
        }
        out[i] = cal_.toHead(p + d);
    }
    return out;
}

// ======================================================================== fit
NativeFitResult fitFo4SculptsNative(const Fo4FaceModel& model, const Mapping& mapping, const std::vector<Vec3>& vPos,
                                    const std::vector<Vec3>& vNrm, const NativeFitOptions& opt) {
    NativeFitResult res;
    const double maxD = opt.maxDistanceMm / 1000.0;
    Grid grid(vPos, 0.006);

    // ---- align the default Fallout 4 face to V's face
    Vec3 as(1, 1, 1), ao;
    std::vector<Vec3> rawBase = model.basePositions();
    if (opt.align) {
        HeadAlignment ha = alignFo4Face(model, vPos);
        as = ha.scale;
        ao = ha.offset;
    }
    res.alignScale = as;
    res.alignOffset = ao;
    auto alignP = [&](const Vec3& p) { return Vec3(as.x * p.x + ao.x, as.y * p.y + ao.y, as.z * p.z + ao.z); };
    auto alignD = [&](const Vec3& d) { return Vec3(as.x * d.x, as.y * d.y, as.z * d.z); };
    std::vector<Vec3> base(rawBase.size());
    for (size_t i = 0; i < base.size(); ++i) base[i] = alignP(rawBase[i]);

    // ---- basis on the Fallout 4 head (head space displacement per unit slider)
    std::vector<int> comps = {0, 1, 2, 6};
    if (opt.rotations) comps = {0, 1, 2, 3, 4, 5, 6};
    struct Col { uint32_t region; int comp; double sign; std::vector<std::pair<int, Vec3>> nz; uint32_t morphKey = 0; };
    std::vector<Col> cols;
    std::vector<char> touched(base.size(), 0);
    for (const auto& [id, reg] : mapping.fo4Regions) {
        (void)reg;
        for (int c : comps)
            for (double sg : {1.0, -1.0}) {
                RegionSculpt rs{id, std::vector<double>(8, 0.0)};
                rs.values[c] = sg;
                auto p = model.evaluate({rs});
                Col col{id, c, sg, {}};
                for (size_t i = 0; i < p.size(); ++i) {
                    Vec3 d = alignD(p[i] - rawBase[i]);
                    if (length(d) > 1e-7) { col.nz.push_back({int(i), d}); touched[i] = 1; }
                }
                if (!col.nz.empty()) cols.push_back(std::move(col));
            }
    }
    // face presets (0..1) and sliders (negative / positive half, 0..1 each): one key per distinct morph
    if (opt.faceMorphs && model.hasFaceMorphs()) {
        std::set<std::string> seen;
        auto addCol = [&](uint32_t key, double sign) {
            auto p = model.evaluate({}, {{key, sign}});
            Col col{0, -1, sign, {}, key};
            for (size_t i = 0; i < p.size(); ++i) {
                Vec3 d = alignD(p[i] - rawBase[i]);
                if (length(d) > 1e-7) { col.nz.push_back({int(i), d}); touched[i] = 1; }
            }
            if (!col.nz.empty()) cols.push_back(std::move(col));
        };
        for (const auto& [k, m] : model.presetKeys())
            if (seen.insert("p:" + m).second) addCol(k, 1.0);
        for (const auto& [k, mm] : model.sliderKeys())
            if (seen.insert("s:" + std::min(mm.first, mm.second) + "|" + std::max(mm.first, mm.second)).second) {
                addCol(k, 1.0);
                addCol(k, -1.0);
            }
    }
    if (cols.empty()) throw std::runtime_error("no Fallout 4 sculpt region moves the Fallout 4 head");
    // Regions that move the head exactly like another one (Extended Facial Sculpting's "Fine"
    // variants, for example) only split the same shape between two sliders: keep the stronger one.
    {
        std::map<std::pair<size_t, int>, std::vector<size_t>> groups;
        std::vector<double> norm(cols.size(), 0.0);
        for (size_t c = 0; c < cols.size(); ++c) {
            for (const auto& [i, d] : cols[c].nz) norm[c] += dot(d, d);
            norm[c] = std::sqrt(norm[c]);
            groups[{cols[c].nz.size(), cols[c].nz.front().first}].push_back(c);
        }
        std::vector<char> drop(cols.size(), 0);
        for (const auto& [k, g] : groups)
            for (size_t a = 0; a < g.size(); ++a)
                for (size_t b = a + 1; b < g.size(); ++b) {
                    const size_t x = g[a], y = g[b];
                    if (drop[x] || drop[y] || cols[x].morphKey || cols[y].morphKey) continue;
                    double d = 0;
                    bool same = true;
                    for (size_t q = 0; q < cols[x].nz.size() && same; ++q) {
                        if (cols[x].nz[q].first != cols[y].nz[q].first) same = false;
                        else d += dot(cols[x].nz[q].second, cols[y].nz[q].second);
                    }
                    if (same && d > 0.9995 * norm[x] * norm[y]) drop[norm[x] >= norm[y] ? y : x] = 1;
                }
        std::vector<Col> kept;
        for (size_t c = 0; c < cols.size(); ++c)
            if (!drop[c]) kept.push_back(std::move(cols[c]));
        cols.swap(kept);
    }
    const size_t N = cols.size();

    // ---- face vertices that have a counterpart on V's face
    std::vector<int> pts;
    for (size_t i = 0; i < base.size(); ++i)
        if (model.isFace(i) && grid.nearest(base[i], maxD) >= 0) pts.push_back(int(i));
    if (pts.size() < 50) throw std::runtime_error("Fallout 4's and V's heads do not line up (calibration failed?)");
    res.points = pts.size();
    std::vector<int> rowOf(base.size(), -1);
    for (size_t r = 0; r < pts.size(); ++r) rowOf[pts[r]] = int(r);

    auto toPresets = [&](const std::vector<double>& w) {
        std::map<uint32_t, double> pr;
        for (size_t j = 0; j < N; ++j)
            if (cols[j].comp < 0 && w[j] > 1e-4) pr[cols[j].morphKey] += w[j] * cols[j].sign;
        for (auto it = pr.begin(); it != pr.end();) {
            it->second = std::clamp(std::round(it->second * 1000.0) / 1000.0, -1.0, 1.0);
            if (std::fabs(it->second) < 1e-3) it = pr.erase(it); else ++it;
        }
        return pr;
    };
    auto toRegions = [&](const std::vector<double>& w) {
        std::map<uint32_t, std::vector<double>> vals;
        for (size_t j = 0; j < N; ++j) {
            if (cols[j].comp < 0) continue;
            auto& v = vals[cols[j].region];
            if (v.empty()) v.assign(8, 0.0);
            v[cols[j].comp] += w[j] * cols[j].sign;
        }
        std::vector<RegionSculpt> out;
        for (auto& [id, v] : vals) {
            for (double& x : v) x = std::clamp(std::round(x * 10000.0) / 10000.0, -1.0, 1.0);
            if (std::all_of(v.begin(), v.end(), [](double x) { return std::fabs(x) < 1e-4; })) continue;
            out.push_back({id, v});
        }
        return out;
    };
    // mean point-to-plane distance of the current shape
    auto distance = [&](const std::vector<Vec3>& cur, std::vector<int>* match) {
        double s = 0;
        int n = 0;
        if (match) match->assign(pts.size(), -1);
        for (size_t r = 0; r < pts.size(); ++r) {
            int c = grid.nearest(cur[pts[r]], maxD);
            if (c < 0) continue;
            if (match) (*match)[r] = c;
            s += std::fabs(dot(vNrm[c], vPos[c] - cur[pts[r]]));
            ++n;
        }
        return n ? 1000.0 * s / n : 0.0;
    };

    std::vector<double> w(N, 0.0);
    std::vector<Vec3> cur = base;
    std::vector<int> match;
    res.alignedMm = distance(cur, &match);
    {
        std::vector<int> m0;
        res.beforeMm = distance(rawBase, &m0);
    }
    double bestMm = res.alignedMm;
    std::vector<RegionSculpt> best;
    std::map<uint32_t, double> bestPresets;

    std::vector<double> Bn(pts.size() * N);
    for (int it = 0; it < std::max(1, opt.icpIterations); ++it) {
        // linearised point-to-plane system with robust weights
        std::vector<double> rhs(pts.size(), 0.0), rw(pts.size(), 0.0);
        std::fill(Bn.begin(), Bn.end(), 0.0);
        for (size_t r = 0; r < pts.size(); ++r) {
            int c = match[r];
            if (c < 0) continue;
            const Vec3& n = vNrm[c];
            double res0 = dot(n, vPos[c] - cur[pts[r]]);
            const double h = 0.004;   // Huber: residuals beyond 4 mm count less
            rw[r] = std::fabs(res0) <= h ? 1.0 : h / std::fabs(res0);
            rhs[r] = res0;
        }
        for (size_t j = 0; j < N; ++j)
            for (const auto& [vi, d] : cols[j].nz) {
                int r = rowOf[vi];
                if (r < 0 || match[r] < 0) continue;
                Bn[size_t(r) * N + j] = dot(vNrm[match[r]], d);
            }
        for (size_t r = 0; r < pts.size(); ++r)
            for (size_t j = 0; j < N; ++j) rhs[r] += Bn[r * N + j] * w[j];   // target for absolute w
        std::vector<double> H(N * N, 0.0), g(N, 0.0);
        for (size_t r = 0; r < pts.size(); ++r) {
            if (rw[r] == 0) continue;
            const double* row = &Bn[r * N];
            for (size_t i = 0; i < N; ++i) {
                if (row[i] == 0) continue;
                g[i] += rw[r] * row[i] * rhs[r];
                for (size_t j = i; j < N; ++j)
                    if (row[j] != 0) H[i * N + j] += rw[r] * row[i] * row[j];
            }
        }
        double meanDiag = 0;
        for (size_t i = 0; i < N; ++i) {
            for (size_t j = 0; j < i; ++j) H[i * N + j] = H[j * N + i];
            meanDiag += H[i * N + i];
        }
        meanDiag /= double(N);
        const double lambda = opt.regularization * meanDiag + 1e-12;
        for (int sweep = 0; sweep < 300; ++sweep) {
            double change = 0;
            for (size_t i = 0; i < N; ++i) {
                double s = g[i];
                for (size_t j = 0; j < N; ++j)
                    if (j != i) s -= H[i * N + j] * w[j];
                double nw = std::clamp(s / (H[i * N + i] + lambda), 0.0, 1.0);
                change = std::max(change, std::fabs(nw - w[i]));
                w[i] = nw;
            }
            if (change < 1e-6) break;
        }
        auto regions = toRegions(w);
        auto presets = toPresets(w);
        {
            auto v = model.evaluate(regions, presets);
            for (auto& p : v) p = alignP(p);
            cur = v;
        }
        double mm = distance(cur, &match);
        if (mm < bestMm) { bestMm = mm; best = regions; bestPresets = presets; }
    }
    res.regions = best;
    res.presets = bestPresets;
    res.afterMm = bestMm;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%zu basis shapes on Fallout 4's head, %zu face vertices matched", N, pts.size());
    res.notes.push_back(buf);
    return res;
}

HeadAlignment alignFo4Face(const Fo4FaceModel& model, const std::vector<Vec3>& vPos) {
    Grid grid(vPos, 0.006);
    const std::vector<Vec3>& rawBase = model.basePositions();
    std::vector<int> face;
    for (size_t i = 0; i < rawBase.size(); ++i)
        if (model.isFace(i)) face.push_back(int(i));
    Vec3 as(1, 1, 1), ao;
    for (int it = 0; it < 30; ++it) {
        const double trim = it < 10 ? 0.012 : 0.008;
        double S[3][5] = {};   // per axis: n, sx, sy, sxx, sxy
        for (int i : face) {
            Vec3 x(as.x * rawBase[i].x + ao.x, as.y * rawBase[i].y + ao.y, as.z * rawBase[i].z + ao.z);
            int c = grid.nearest(x, trim);
            if (c < 0) continue;
            for (int k = 0; k < 3; ++k) {
                S[k][0] += 1; S[k][1] += rawBase[i][k]; S[k][2] += vPos[c][k];
                S[k][3] += rawBase[i][k] * rawBase[i][k]; S[k][4] += rawBase[i][k] * vPos[c][k];
            }
        }
        for (int k = 0; k < 3; ++k) {
            const double n = S[k][0];
            if (n < 30) continue;
            const double den = n * S[k][3] - S[k][1] * S[k][1];
            if (std::fabs(den) < 1e-15) continue;
            double sc = (n * S[k][4] - S[k][1] * S[k][2]) / den;
            sc = std::clamp(sc, 0.85, 1.15);   // only correct the landmark calibration, never reshape
            as[k] = sc;
            ao[k] = (S[k][2] - sc * S[k][1]) / n;
        }
    }
    HeadAlignment h;
    h.scale = as;
    h.offset = ao;
    return h;
}

std::vector<Vec3> transferFo4Sculpt(const Fo4FaceModel& model, const std::vector<RegionSculpt>& regions,
                                    const HeadAlignment& align, const std::vector<Vec3>& vPos,
                                    const std::map<uint32_t, double>& presets) {
    const std::vector<Vec3>& b0 = model.basePositions();
    const std::vector<Vec3> b1 = model.evaluate(regions, presets);
    // moved Fallout 4 vertices -> grid
    std::vector<Vec3> P, D;
    for (size_t i = 0; i < b0.size(); ++i) {
        const Vec3 d = align.vec(b1[i] - b0[i]);
        if (!model.isFace(i) && length(d) < 1e-7) continue;
        P.push_back(align.point(b0[i]));
        D.push_back(d);
    }
    const double cell = 0.01, sigma = 0.005, radius = 0.016;
    std::unordered_map<long long, std::vector<int>> grid;
    auto key = [](long long x, long long y, long long z) { return ((x + 100000) * 200003LL + (y + 100000)) * 200003LL + (z + 100000); };
    for (size_t i = 0; i < P.size(); ++i)
        grid[key((long long)std::floor(P[i].x / cell), (long long)std::floor(P[i].y / cell), (long long)std::floor(P[i].z / cell))].push_back(int(i));
    std::vector<Vec3> out(vPos.size());
    for (size_t v = 0; v < vPos.size(); ++v) {
        const Vec3& q = vPos[v];
        const long long cx = (long long)std::floor(q.x / cell), cy = (long long)std::floor(q.y / cell), cz = (long long)std::floor(q.z / cell);
        double ws = 0, wmax = 0;
        Vec3 d;
        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = -2; dy <= 2; ++dy)
                for (int dz = -2; dz <= 2; ++dz) {
                    auto it = grid.find(key(cx + dx, cy + dy, cz + dz));
                    if (it == grid.end()) continue;
                    for (int i : it->second) {
                        const double r = length(P[i] - q);
                        if (r > radius) continue;
                        const double w = std::exp(-(r * r) / (sigma * sigma));
                        ws += w;
                        wmax = std::max(wmax, w);
                        d += D[i] * w;
                    }
                }
        if (ws < 1e-9) continue;
        // full strength on the face, fading out where Fallout 4's face is far away (back of the head)
        const double conf = std::clamp((wmax - 0.05) / 0.45, 0.0, 1.0);
        out[v] = d * (conf / ws);
    }
    return out;
}

double nativeSurfaceDistanceMm(const Fo4FaceModel& model, const std::vector<RegionSculpt>& regions,
                               const std::vector<Vec3>& vPos, const std::vector<Vec3>& vNrm, double maxDistanceMm,
                               Vec3 as, Vec3 ao) {
    Grid grid(vPos, 0.006);
    const double maxD = maxDistanceMm / 1000.0;
    auto alignP = [&](const Vec3& p) { return Vec3(as.x * p.x + ao.x, as.y * p.y + ao.y, as.z * p.z + ao.z); };
    auto base = model.basePositions();
    for (auto& p : base) p = alignP(p);
    auto cur = model.evaluate(regions);
    for (auto& p : cur) p = alignP(p);
    double s = 0;
    int n = 0;
    for (size_t i = 0; i < cur.size(); ++i) {
        if (!model.isFace(i) || grid.nearest(base[i], maxD) < 0) continue;
        int c = grid.nearest(cur[i], maxD);
        if (c < 0) continue;
        s += std::fabs(dot(vNrm[c], vPos[c] - cur[i]));
        ++n;
    }
    return n ? 1000.0 * s / n : 0.0;
}

} // namespace ptk
