#include "PresetFit.h"
#include "Util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace ptk {

namespace {

struct Prim {
    int position = -1, normal = -1, tangent = -1;
    std::vector<float> pos, nrm, tan;
    int tanComp = 4;
    size_t first = 0, count = 0;   // vertex range in the flattened list
};

std::vector<Prim> collectPrims(const Glb& g) {
    std::vector<Prim> out;
    size_t total = 0;
    const nlohmann::json meshes = g.doc.value("meshes", nlohmann::json::array());
    for (const auto& m : meshes) {
        const nlohmann::json prims = m.value("primitives", nlohmann::json::array());
        for (const auto& p : prims) {
            const auto& a = p["attributes"];
            if (!a.contains("POSITION")) continue;
            Prim pr;
            int c = 3;
            pr.position = a["POSITION"].get<int>();
            pr.pos = readFloatAccessor(g, pr.position, c);
            if (a.contains("NORMAL")) { pr.normal = a["NORMAL"].get<int>(); pr.nrm = readFloatAccessor(g, pr.normal, c); }
            if (a.contains("TANGENT")) { pr.tangent = a["TANGENT"].get<int>(); pr.tan = readFloatAccessor(g, pr.tangent, pr.tanComp); }
            pr.first = total;
            pr.count = pr.pos.size() / 3;
            total += pr.count;
            out.push_back(std::move(pr));
        }
    }
    return out;
}

std::vector<Vec3> positions(const Glb& g, const std::vector<Prim>& prims) {
    std::vector<Vec3> out;
    for (const auto& p : prims) {
        int c = 3;
        auto v = readFloatAccessor(g, p.position, c);
        for (size_t i = 0; i < p.count; ++i) out.emplace_back(v[i * 3], v[i * 3 + 1], v[i * 3 + 2]);
    }
    return out;
}

void restore(Glb& g, const std::vector<Prim>& prims) {
    for (const auto& p : prims) {
        writeFloatAccessor(g, p.position, p.pos);
        if (p.normal >= 0) writeFloatAccessor(g, p.normal, p.nrm);
        if (p.tangent >= 0) writeFloatAccessor(g, p.tangent, p.tan);
    }
}

LooksMenuPreset presetFor(const std::vector<RegionSculpt>& regions) {
    LooksMenuPreset p;
    p.regions = regions;
    return p;
}

// Applies `regions` to `work` (restored afterwards) and returns the displacement of every vertex.
std::vector<Vec3> evaluate(Glb& work, const std::vector<Prim>& prims, const std::vector<Vec3>& base,
                           const std::vector<RegionSculpt>& regions, const Mapping& mapping, const HeadFrame& frame) {
    std::vector<Vec3> d(base.size());
    SculptInput s = resolveSculpts(presetFor(regions), mapping);
    if (!s.empty()) {
        DeformOptions o;
        o.adjustMorphTargets = false;
        deformHead(work, s, mapping, frame, o);
        auto now = positions(work, prims);
        for (size_t i = 0; i < d.size(); ++i) d[i] = now[i] - base[i];
        restore(work, prims);
    }
    return d;
}

struct Column {
    uint32_t region = 0;
    int component = 0;   // index into the 8 sculpt values
    double sign = 1;     // +1 or -1 (Fallout 4 maxima / minima differ)
    std::vector<std::pair<uint32_t, float>> nz;  // (row, value), rows = vertex*3 + axis
    double energy = 0;
};

} // namespace

std::vector<Vec3> ccMorphDisplacement(const Glb& head, const std::map<std::string, std::string>& choices,
                                      std::vector<std::string>& used, std::vector<std::string>& missing,
                                      bool relativeToAverage) {
    std::vector<Vec3> d;
    const nlohmann::json meshes = head.doc.value("meshes", nlohmann::json::array());
    for (const auto& m : meshes) {
        std::vector<std::string> names;
        if (m.contains("extras") && m["extras"].contains("targetNames"))
            for (const auto& n : m["extras"]["targetNames"]) names.push_back(n.get<std::string>());
        const nlohmann::json prims = m.value("primitives", nlohmann::json::array());
        for (const auto& p : prims) {
            if (!p["attributes"].contains("POSITION")) continue;
            const size_t count = head.doc["accessors"][p["attributes"]["POSITION"].get<int>()]["count"].get<size_t>();
            const size_t first = d.size();
            d.resize(first + count);
            if (!p.contains("targets")) continue;
            const auto& targets = p["targets"];
            for (const auto& [region, target] : choices) {
                int idx = -1;
                for (size_t t = 0; t < names.size(); ++t) {
                    const std::string& n = names[t];
                    if (n == target || n.rfind(target + "_", 0) == 0) { idx = int(t); break; }
                }
                if (idx < 0 || idx >= (int)targets.size() || !targets[idx].contains("POSITION")) {
                    if (std::find(missing.begin(), missing.end(), region + " " + target) == missing.end())
                        missing.push_back(region + " " + target);
                    continue;
                }
                int c = 3;
                auto v = readFloatAccessor(head, targets[idx]["POSITION"].get<int>(), c);
                for (size_t i = 0; i < count && i * 3 + 2 < v.size(); ++i)
                    d[first + i] += Vec3(v[i * 3], v[i * 3 + 1], v[i * 3 + 2]);
                if (relativeToAverage) {
                    // all options of this region: names ending in "_<region>" (h011_eyes, h021_eyes, ...)
                    const std::string suffix = names[idx].substr(names[idx].find('_'));
                    std::vector<int> same;
                    for (size_t t = 0; t < names.size() && t < targets.size(); ++t)
                        if (names[t].size() > suffix.size() &&
                            names[t].compare(names[t].size() - suffix.size(), suffix.size(), suffix) == 0 &&
                            targets[t].contains("POSITION"))
                            same.push_back(int(t));
                    if (same.size() > 1) {
                        const double w = 1.0 / double(same.size());
                        for (int t : same) {
                            auto o = readFloatAccessor(head, targets[t]["POSITION"].get<int>(), c);
                            for (size_t i = 0; i < count && i * 3 + 2 < o.size(); ++i)
                                d[first + i] = d[first + i] - Vec3(o[i * 3], o[i * 3 + 1], o[i * 3 + 2]) * w;
                        }
                    }
                }
                const std::string u = region + " " + names[idx];
                if (std::find(used.begin(), used.end(), u) == used.end()) used.push_back(u);
            }
        }
    }
    return d;
}

FitResult fitFo4Sculpts(const Glb& head, const std::vector<Vec3>& target, const Mapping& mapping,
                        const HeadFrame& frame, const FitOptions& opt) {
    FitResult res;
    Glb work = head;
    std::vector<Prim> prims = collectPrims(work);
    std::vector<Vec3> base = positions(work, prims);
    if (base.size() != target.size()) throw std::runtime_error("fit: target and head have different vertex counts");
    const size_t rows = base.size() * 3;

    // ---- basis: the displacement of one region component at +1 and at -1
    std::vector<int> comps = {0, 1, 2, 6};
    if (opt.rotations) comps = {0, 1, 2, 3, 4, 5, 6};
    std::vector<Column> cols;
    for (const auto& [id, region] : mapping.fo4Regions) {
        (void)region;
        for (int c : comps)
            for (double sgn : {1.0, -1.0}) {
                RegionSculpt rs;
                rs.id = id;
                rs.values.assign(8, 0.0);
                rs.values[c] = sgn * (c >= 3 && c <= 5 ? 0.25 : 1.0);   // rotations: smaller probe
                auto d = evaluate(work, prims, base, {rs}, mapping, frame);
                Column col;
                col.region = id;
                col.component = c;
                col.sign = rs.values[c];
                for (size_t v = 0; v < d.size(); ++v)
                    for (int a = 0; a < 3; ++a)
                        if (std::fabs(d[v][a]) > 1e-7) {
                            col.nz.push_back({uint32_t(v * 3 + a), float(d[v][a])});
                            col.energy += d[v][a] * d[v][a];
                        }
                if (col.energy > 1e-10) cols.push_back(std::move(col));
            }
    }
    if (cols.empty()) throw std::runtime_error("fit: no Fallout 4 region moves V's head - check the region data");
    const size_t N = cols.size();

    // ---- normal equations H w = g (H shared by all passes)
    std::vector<double> H(N * N, 0.0), dense(rows, 0.0);
    double meanEnergy = 0;
    for (size_t i = 0; i < N; ++i) {
        for (auto& [r, v] : cols[i].nz) dense[r] = v;
        for (size_t j = i; j < N; ++j) {
            double s = 0;
            for (auto& [r, v] : cols[j].nz) s += dense[r] * v;
            H[i * N + j] = H[j * N + i] = s;
        }
        for (auto& [r, v] : cols[i].nz) dense[r] = 0;
        meanEnergy += cols[i].energy;
    }
    meanEnergy /= double(N);
    const double lambda = opt.regularization * meanEnergy;

    auto solve = [&](const std::vector<double>& rhs) {
        std::vector<double> g(N, 0.0), w(N, 0.0);
        for (size_t i = 0; i < N; ++i)
            for (auto& [r, v] : cols[i].nz) g[i] += double(v) * rhs[r];
        for (int sweep = 0; sweep < 400; ++sweep) {
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
        return w;
    };
    auto toRegions = [&](const std::vector<double>& w) {
        std::map<uint32_t, std::vector<double>> vals;
        for (size_t i = 0; i < N; ++i) {
            auto& v = vals[cols[i].region];
            if (v.empty()) v.assign(8, 0.0);
            v[cols[i].component] += w[i] * cols[i].sign;
        }
        std::vector<RegionSculpt> out;
        for (auto& [id, v] : vals) {
            for (double& x : v) x = std::clamp(std::round(x * 10000.0) / 10000.0, -1.0, 1.0);
            if (std::all_of(v.begin(), v.end(), [](double x) { return std::fabs(x) < 1e-4; })) continue;
            out.push_back({id, v});
        }
        return out;
    };
    auto rmsMax = [&](const std::vector<Vec3>& r, const std::vector<char>& active, double& rms, double& mx) {
        double s = 0;
        size_t n = 0;
        mx = 0;
        for (size_t i = 0; i < r.size(); ++i)
            if (active[i]) {
                double l = length(r[i]);
                s += l * l;
                mx = std::max(mx, l);
                ++n;
            }
        rms = n ? std::sqrt(s / double(n)) * 1000.0 : 0.0;
        mx *= 1000.0;
    };

    std::vector<char> active(base.size(), 0);
    for (size_t i = 0; i < base.size(); ++i)
        if (length(target[i]) > 1e-5) { active[i] = 1; ++res.vertices; }
    rmsMax(target, active, res.targetRmsMm, res.targetMaxMm);

    // ---- solve, then correct for the non-linear overlap of regions with a few outer passes
    std::vector<double> rhs(rows);
    for (size_t v = 0; v < base.size(); ++v)
        for (int a = 0; a < 3; ++a) rhs[v * 3 + a] = target[v][a];
    std::vector<RegionSculpt> best;
    double bestRms = 1e300, bestMax = 0;
    for (int pass = 0; pass <= std::max(0, opt.refinements); ++pass) {
        auto regions = toRegions(solve(rhs));
        auto got = evaluate(work, prims, base, regions, mapping, frame);
        std::vector<Vec3> resid(base.size());
        for (size_t i = 0; i < base.size(); ++i) resid[i] = target[i] - got[i];
        double rms, mx;
        rmsMax(resid, active, rms, mx);
        if (rms < bestRms) { bestRms = rms; bestMax = mx; best = regions; }
        for (size_t v = 0; v < base.size(); ++v)
            for (int a = 0; a < 3; ++a) rhs[v * 3 + a] += resid[v][a];
    }
    res.regions = best;
    res.residualRmsMm = bestRms;
    res.residualMaxMm = bestMax;
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%zu basis shapes from %zu regions", N, mapping.fo4Regions.size());
    res.notes.push_back(buf);
    return res;
}

} // namespace ptk
