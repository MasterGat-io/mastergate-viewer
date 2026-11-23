/**
 * @file mgcreatorrefit.cpp
 * @brief MasterGate Viewer: Creator Lab's refit of a garment from one body to another.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * MasterGate Viewer, based on the Firestorm Viewer and the Second Life Viewer.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * $/LicenseInfo$
 */

#ifndef MG_CREATORDAE_STANDALONE
#include "llviewerprecompiledheaders.h"
#endif

#include "mgcreatorrefit.h"
#include "mgcreatorweights.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <unordered_map>

namespace
{
    using MGCreatorWeights::Mat;

    struct V3 { double x = 0, y = 0, z = 0; };
    V3 v3(double x, double y, double z) { V3 r; r.x = x; r.y = y; r.z = z; return r; }
    V3 operator+(const V3& a, const V3& b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
    V3 operator-(const V3& a, const V3& b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
    V3 operator*(const V3& a, double s) { return v3(a.x * s, a.y * s, a.z * s); }
    double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    V3 cross(const V3& a, const V3& b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
    double len(const V3& a) { return std::sqrt(dot(a, a)); }
    V3 unit(const V3& a) { const double l = len(a); return l > 1e-12 ? a * (1.0 / l) : v3(0, 0, 0); }

    const Mat IDENTITY = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

    // collision volumes move with their parent bone
    std::string boneOf(const std::string& j)
    {
        static const std::map<std::string, std::string> CV = {
            { "PELVIS", "mPelvis" }, { "BUTT", "mPelvis" }, { "BELLY", "mTorso" }, { "LEFT_HANDLE", "mTorso" },
            { "RIGHT_HANDLE", "mTorso" }, { "LOWER_BACK", "mTorso" }, { "CHEST", "mChest" }, { "LEFT_PEC", "mChest" },
            { "RIGHT_PEC", "mChest" }, { "UPPER_BACK", "mChest" }, { "NECK", "mNeck" }, { "HEAD", "mHead" },
            { "L_CLAVICLE", "mCollarLeft" }, { "R_CLAVICLE", "mCollarRight" }, { "L_UPPER_ARM", "mShoulderLeft" },
            { "R_UPPER_ARM", "mShoulderRight" }, { "L_LOWER_ARM", "mElbowLeft" }, { "R_LOWER_ARM", "mElbowRight" },
            { "L_HAND", "mWristLeft" }, { "R_HAND", "mWristRight" }, { "L_UPPER_LEG", "mHipLeft" },
            { "R_UPPER_LEG", "mHipRight" }, { "L_LOWER_LEG", "mKneeLeft" }, { "R_LOWER_LEG", "mKneeRight" },
            { "L_FOOT", "mAnkleLeft" }, { "R_FOOT", "mAnkleRight" } };
        auto it = CV.find(j);
        return it == CV.end() ? j : it->second;
    }

    // a file's inverse bind for each bone (the first mesh that has it)
    typedef std::map<std::string, Mat> Binds;
    Binds bindsOf(const MGCreatorDae::File& f)
    {
        Binds out;
        for (const auto& m : f.meshes)
            for (size_t j = 0; j < m.joints.size() && j < m.invBind.size(); ++j)
                if (!out.count(m.joints[j])) out[m.joints[j]] = m.invBind[j];
        return out;
    }

    bool jointPlace(const Binds& b, const std::string& name, V3& out)
    {
        auto it = b.find(name);
        Mat w;
        if (it == b.end() || !MGCreatorWeights::invert(it->second, w)) return false;
        out = v3(w[3], w[7], w[11]);
        return true;
    }

    // per joint of a mesh: the matrix that moves its bind space onto another file's
    // skeleton (that file's joint times the mesh's own inverse bind; identity when the
    // other file does not have the bone)
    std::vector<Mat> onSkeleton(const MGCreatorDae::SkinnedMesh& m, const Binds& other)
    {
        std::vector<Mat> k(m.joints.size(), IDENTITY);
        for (size_t j = 0; j < m.joints.size() && j < m.invBind.size(); ++j)
        {
            auto it = other.find(m.joints[j]);
            Mat w;
            if (it != other.end() && MGCreatorWeights::invert(it->second, w)) k[j] = MGCreatorWeights::multiply(w, m.invBind[j]);
        }
        return k;
    }

    Mat vertexMatrix(const MGCreatorDae::SkinnedMesh& m, size_t v, const std::vector<Mat>& k)
    {
        Mat acc{};
        double ws = 0.0;
        if (v < m.weights.size())
            for (const auto& jw : m.weights[v])
            {
                if (jw.first < 0 || jw.first >= (int)k.size() || jw.second <= 0.0) continue;
                for (int i = 0; i < 16; ++i) acc[i] += k[jw.first][i] * jw.second;
                ws += jw.second;
            }
        if (ws <= 0.0) return m.bindShape;
        for (int i = 0; i < 16; ++i) acc[i] /= ws;
        return MGCreatorWeights::multiply(acc, m.bindShape);
    }

    V3 placeAt(const Mat& vm, const double* p)
    {
        const auto q = MGCreatorWeights::transformPoint(vm, p);
        return v3(q[0], q[1], q[2]);
    }

    // a body's skin: points, outward normals, weights by bone name, triangles in a grid
    struct Surface
    {
        std::vector<V3> p, n;
        std::vector<std::vector<std::pair<std::string, double>>> w;
        std::vector<std::string> vBone;     // each point's main bone (volumes as their parent)
        std::vector<std::array<int, 3>> tris;
        std::vector<std::string> triBone;
        std::set<std::string> bones;        // bones that have skin triangles
        double cell = 0.02;
        std::unordered_map<long long, std::vector<int>> grid;
        long long key(long long x, long long y, long long z) const { return ((x + (1 << 20)) << 42) | ((y + (1 << 20)) << 21) | (z + (1 << 20)); }
        long long ci(double v) const { return (long long)std::floor(v / cell); }
    };

    std::string dominantBone(const std::vector<std::pair<std::string, double>>& w)
    {
        std::map<std::string, double> acc;
        for (const auto& jw : w) acc[boneOf(jw.first)] += jw.second;
        std::string best;
        double bw = -1.0;
        for (const auto& kv : acc) if (kv.second > bw) { bw = kv.second; best = kv.first; }
        return best;
    }

    void buildSurface(const MGCreatorDae::File& f, Surface& s)
    {
        for (const auto& m : f.meshes)
        {
            const int base = (int)s.p.size();
            const size_t n = m.vertexCount();
            for (size_t v = 0; v < n; ++v)
            {
                s.p.push_back(placeAt(m.bindShape, &m.positions[v * 3]));
                std::vector<std::pair<std::string, double>> w;
                if (v < m.weights.size())
                    for (const auto& jw : m.weights[v]) if (jw.first >= 0 && jw.first < (int)m.joints.size()) w.push_back({ m.joints[jw.first], jw.second });
                s.vBone.push_back(dominantBone(w));
                s.w.push_back(w);
            }
            // outward: a closed body has a positive volume with outward triangle order
            double vol = 0.0;
            for (const auto& t : m.triangles)
            {
                if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= (int)n || t[1] >= (int)n || t[2] >= (int)n) continue;
                vol += dot(s.p[base + t[0]], cross(s.p[base + t[1]], s.p[base + t[2]]));
            }
            const double sign = vol < 0.0 ? -1.0 : 1.0;
            std::vector<V3> acc(n);
            for (const auto& t : m.triangles)
            {
                if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= (int)n || t[1] >= (int)n || t[2] >= (int)n) continue;
                const V3 a = s.p[base + t[0]], b = s.p[base + t[1]], c = s.p[base + t[2]];
                const V3 fn = cross(b - a, c - a) * sign;
                for (int k = 0; k < 3; ++k) acc[t[k]] = acc[t[k]] + fn;
                s.tris.push_back({ base + t[0], base + t[1], base + t[2] });
            }
            for (size_t v = 0; v < n; ++v) s.n.push_back(unit(acc[v]));
        }
        // each triangle belongs to the bone most of its corners follow
        for (const auto& t : s.tris)
        {
            std::map<std::string, int> votes;
            for (int k = 0; k < 3; ++k) ++votes[s.vBone[t[k]]];
            std::string best;
            int bv = -1;
            for (const auto& kv : votes) if (kv.second > bv) { bv = kv.second; best = kv.first; }
            s.triBone.push_back(best);
            s.bones.insert(best);
        }
        for (int i = 0; i < (int)s.tris.size(); ++i)
        {
            const V3 &a = s.p[s.tris[i][0]], &b = s.p[s.tris[i][1]], &c = s.p[s.tris[i][2]];
            for (long long x = s.ci(std::min(a.x, std::min(b.x, c.x))); x <= s.ci(std::max(a.x, std::max(b.x, c.x))); ++x)
                for (long long y = s.ci(std::min(a.y, std::min(b.y, c.y))); y <= s.ci(std::max(a.y, std::max(b.y, c.y))); ++y)
                    for (long long z = s.ci(std::min(a.z, std::min(b.z, c.z))); z <= s.ci(std::max(a.z, std::max(b.z, c.z))); ++z)
                        s.grid[s.key(x, y, z)].push_back(i);
        }
    }

    // Ericson's closest point on a triangle, with barycentrics
    V3 closest(const V3& p, const V3& a, const V3& b, const V3& c, double& u, double& v, double& w)
    {
        const V3 ab = b - a, ac = c - a, ap = p - a;
        const double d1 = dot(ab, ap), d2 = dot(ac, ap);
        if (d1 <= 0 && d2 <= 0) { u = 1; v = 0; w = 0; return a; }
        const V3 bp = p - b;
        const double d3 = dot(ab, bp), d4 = dot(ac, bp);
        if (d3 >= 0 && d4 <= d3) { u = 0; v = 1; w = 0; return b; }
        const double vc = d1 * d4 - d3 * d2;
        if (vc <= 0 && d1 >= 0 && d3 <= 0) { const double t = d1 / (d1 - d3); u = 1 - t; v = t; w = 0; return a + ab * t; }
        const V3 cp = p - c;
        const double d5 = dot(ab, cp), d6 = dot(ac, cp);
        if (d6 >= 0 && d5 <= d6) { u = 0; v = 0; w = 1; return c; }
        const double vb = d5 * d2 - d1 * d6;
        if (vb <= 0 && d2 >= 0 && d6 <= 0) { const double t = d2 / (d2 - d6); u = 1 - t; v = 0; w = t; return a + ac * t; }
        const double va = d3 * d6 - d5 * d4;
        if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { const double t = (d4 - d3) / ((d4 - d3) + (d5 - d6)); u = 0; v = 1 - t; w = t; return b + (c - b) * t; }
        const double den = 1.0 / (va + vb + vc);
        v = vb * den; w = vc * den; u = 1.0 - v - w;
        return a + ab * v + ac * w;
    }

    struct Hit { int tri = -1; V3 p, n; double bary[3] = { 0, 0, 0 }; double dist = 0; };

    // the nearest point of the skin on one of the given bones (any bone: empty), within reach
    struct Finder
    {
        const Surface& s;
        std::vector<int> stamp;
        int tick = 0;
        explicit Finder(const Surface& surface) : s(surface), stamp(surface.tris.size(), -1) {}

        bool nearest(const V3& q, const std::set<std::string>& bones, double reach, Hit& hit)
        {
            ++tick;
            hit = Hit();
            double best = reach * reach;
            const long long cx = s.ci(q.x), cy = s.ci(q.y), cz = s.ci(q.z);
            const int rings = (int)std::ceil(reach / s.cell) + 1;
            for (int r = 0; r <= rings; ++r)
            {
                for (long long x = cx - r; x <= cx + r; ++x)
                    for (long long y = cy - r; y <= cy + r; ++y)
                        for (long long z = cz - r; z <= cz + r; ++z)
                        {
                            if (std::max(std::llabs(x - cx), std::max(std::llabs(y - cy), std::llabs(z - cz))) != r) continue;
                            auto it = s.grid.find(s.key(x, y, z));
                            if (it == s.grid.end()) continue;
                            for (int ti : it->second)
                            {
                                if (stamp[ti] == tick) continue;
                                stamp[ti] = tick;
                                if (!bones.empty() && !bones.count(s.triBone[ti])) continue;
                                const auto& t = s.tris[ti];
                                double u, v, w;
                                const V3 c = closest(q, s.p[t[0]], s.p[t[1]], s.p[t[2]], u, v, w);
                                const V3 d = q - c;
                                const double d2 = dot(d, d);
                                if (d2 < best)
                                {
                                    best = d2;
                                    hit.tri = ti;
                                    hit.p = c;
                                    hit.bary[0] = u; hit.bary[1] = v; hit.bary[2] = w;
                                }
                            }
                        }
                if (hit.tri >= 0 && std::sqrt(best) <= r * s.cell) break;
            }
            if (hit.tri < 0) return false;
            const auto& t = s.tris[hit.tri];
            hit.n = unit(s.n[t[0]] * hit.bary[0] + s.n[t[1]] * hit.bary[1] + s.n[t[2]] * hit.bary[2]);
            hit.dist = std::sqrt(best);
            return true;
        }
    };

    // the bones of B that can stand in for this bone of A (its own, or any when B lacks it)
    std::set<std::string> filterFor(const Surface& b, const std::set<std::string>& bones)
    {
        std::set<std::string> out;
        for (const auto& n : bones) if (b.bones.count(n)) out.insert(n);
        return out;   // empty = any bone
    }
}

namespace MGCreatorRefit
{
    bool refit(const MGCreatorDae::File& garment, const MGCreatorDae::File& bodyA, const MGCreatorDae::File& bodyB,
               std::vector<std::vector<double>>& newPositions,
               std::vector<std::vector<std::array<double, 16>>>& newInvBind,
               Result& result, const Options& o)
    {
        result = Result();
        newPositions.clear();
        newInvBind.clear();
        if (garment.meshes.empty() || bodyA.meshes.empty() || bodyB.meshes.empty()) { result.error = "empty"; return false; }

        const Binds BA = bindsOf(bodyA), BB = bindsOf(bodyB);
        Surface SA, SB;
        buildSurface(bodyA, SA);
        buildSurface(bodyB, SB);
        if (SA.tris.empty() || SB.tris.empty()) { result.error = "body"; return false; }
        Finder FA(SA), FB(SB);

        // 1. per bone, line B's skin up with A's. Guesses: no move, the bone's joint offset,
        // the pelvis's; the best one is refined against B's skin (a few closest-point steps)
        std::map<std::string, std::vector<int>> byBone;
        for (int i = 0; i < (int)SA.p.size(); ++i) byBone[SA.vBone[i]].push_back(i);
        auto score = [&](const std::vector<int>& pts, const std::set<std::string>& filter, const V3& c, V3* resid) -> double
        {
            const double cap = 0.12;
            double sum = 0.0;
            V3 r = v3(0, 0, 0);
            int found = 0;
            for (int i : pts)
            {
                Hit h;
                const V3 q = SA.p[i] + c;
                if (FB.nearest(q, filter, cap, h)) { sum += h.dist; r = r + (h.p - q); ++found; }
                else sum += cap;
            }
            if (resid) *resid = found ? r * (1.0 / found) : v3(0, 0, 0);
            return pts.empty() ? cap : sum / (double)pts.size();
        };
        V3 pelvis = v3(0, 0, 0), pa, pb;
        if (jointPlace(BA, "mPelvis", pa) && jointPlace(BB, "mPelvis", pb)) pelvis = pb - pa;
        std::map<std::string, V3> shift;
        for (const auto& kv : byBone)
        {
            const std::string& bone = kv.first;
            std::vector<int> pts;
            const size_t step = std::max<size_t>(1, kv.second.size() / 250);
            for (size_t i = 0; i < kv.second.size(); i += step) pts.push_back(kv.second[i]);
            const std::set<std::string> filter = filterFor(SB, { bone });
            std::vector<V3> guesses = { v3(0, 0, 0), pelvis };
            if (jointPlace(BA, bone, pa) && jointPlace(BB, bone, pb)) guesses.push_back(pb - pa);
            V3 best = guesses[0];
            double bestScore = 1e30;
            for (const auto& g : guesses)
            {
                const double sc = score(pts, filter, g, nullptr);
                if (sc < bestScore) { bestScore = sc; best = g; }
            }
            V3 c = best;
            for (int it = 0; it < 8; ++it)
            {
                V3 r;
                score(pts, filter, c, &r);
                c = c + r;
                if (len(r) < 0.0003) break;
            }
            if (score(pts, filter, c, nullptr) > bestScore) c = best;
            shift[bone] = c;
            result.boneShift[bone] = { c.x, c.y, c.z };
        }
        auto shiftOfJoint = [&](const std::string& joint, V3& out) -> bool
        {
            auto it = shift.find(boneOf(joint));
            if (it == shift.end()) return false;
            out = it->second;
            return true;
        };
        // each point of A's skin: its bones' shifts, blended by its weights
        std::vector<V3> skinShift(SA.p.size());
        for (size_t i = 0; i < SA.p.size(); ++i)
        {
            V3 s = v3(0, 0, 0), j;
            double ws = 0.0;
            for (const auto& jw : SA.w[i]) if (shiftOfJoint(jw.first, j)) { s = s + j * jw.second; ws += jw.second; }
            skinShift[i] = ws > 0.0 ? s * (1.0 / ws) : shift[SA.vBone[i]];
        }

        double sumMove = 0.0;
        for (const auto& m : garment.meshes)
        {
            const size_t n = m.vertexCount();
            // the garment as body A wears it
            const std::vector<Mat> onA = onSkeleton(m, BA);
            std::vector<V3> g(n), D(n);
            for (size_t v = 0; v < n; ++v) g[v] = placeAt(vertexMatrix(m, v, onA), &m.positions[v * 3]);
            for (size_t v = 0; v < n; ++v)
            {
                std::set<std::string> bones;
                V3 own = v3(0, 0, 0), j;
                double ws = 0.0;
                if (v < m.weights.size())
                    for (const auto& jw : m.weights[v])
                    {
                        if (jw.first < 0 || jw.first >= (int)m.joints.size()) continue;
                        if (jw.second >= 0.15) bones.insert(boneOf(m.joints[jw.first]));
                        if (shiftOfJoint(m.joints[jw.first], j)) { own = own + j * jw.second; ws += jw.second; }
                    }
                if (ws > 0.0) own = own * (1.0 / ws);
                const std::set<std::string> filterA = filterFor(SA, bones), filterB = filterFor(SB, bones);
                V3 out = g[v] + own;
                Hit ha, hb;
                if (FA.nearest(g[v], filterA, o.farReach, ha))
                {
                    const auto& t = SA.tris[ha.tri];
                    const V3 s = skinShift[t[0]] * ha.bary[0] + skinShift[t[1]] * ha.bary[1] + skinShift[t[2]] * ha.bary[2];
                    if (FB.nearest(ha.p + s, filterB, o.reach, hb))
                    {
                        // 2. near the skin: the same distance from B's skin as from A's (the
                        // sideways part kept too, so nothing moves when A = B). A point already
                        // under A's skin stays as it was made: fixing that is Make it fit's job
                        const double dA = dot(g[v] - ha.p, ha.n);
                        const V3 side = (g[v] - ha.p) - ha.n * dA;
                        const V3 nearP = hb.p + side + hb.n * (dA >= 0.0 ? std::max(dA, o.minGap) : dA);
                        // far from it: follow the skin spot
                        const V3 farP = g[v] + (hb.p - ha.p);
                        const double f = std::max(0.0, std::min(1.0, 1.0 - (std::fabs(dA) - o.nearSkin) / o.fade));
                        out = nearP * f + farP * (1.0 - f);
                        if (f > 0.0) ++result.surface; else ++result.followed;
                    }
                    else { out = g[v] + s; ++result.bonesOnly; }
                }
                else ++result.bonesOnly;
                D[v] = out - g[v];
            }
            // 3. smooth the moves over the garment's triangles (no ridge where two bones meet)
            std::vector<std::vector<int>> nb(n);
            for (const auto& t : m.triangles)
                for (int k = 0; k < 3; ++k)
                {
                    const int a = t[k], b = t[(k + 1) % 3];
                    if (a < 0 || b < 0 || a >= (int)n || b >= (int)n) continue;
                    nb[a].push_back(b);
                    nb[b].push_back(a);
                }
            for (int pass = 0; pass < o.smoothPasses; ++pass)
            {
                std::vector<V3> next = D;
                for (size_t v = 0; v < n; ++v)
                {
                    if (nb[v].empty()) continue;
                    V3 s = v3(0, 0, 0);
                    for (int u : nb[v]) s = s + D[u];
                    next[v] = D[v] * 0.5 + s * (0.5 / (double)nb[v].size());
                }
                D.swap(next);
            }
            // 4. B's inverse binds where B has the bone: B's skeleton then draws the version
            // at its bind place, so the positions are simply the bind shape undone
            std::vector<std::array<double, 16>> ib(m.invBind.begin(), m.invBind.end());
            for (size_t j = 0; j < m.joints.size() && j < ib.size(); ++j)
            {
                auto it = BB.find(m.joints[j]);
                if (it != BB.end()) ib[j] = it->second;
            }
            Mat inv;
            if (!MGCreatorWeights::invert(m.bindShape, inv)) { result.error = "bindshape"; return false; }
            std::vector<double> np(m.positions.size());
            for (size_t v = 0; v < n; ++v)
            {
                const V3 b = g[v] + D[v];
                const double bp[3] = { b.x, b.y, b.z };
                const auto r = MGCreatorWeights::transformPoint(inv, bp);
                np[v * 3] = r[0]; np[v * 3 + 1] = r[1]; np[v * 3 + 2] = r[2];
                const double mv = len(D[v]);
                result.maxMove = std::max(result.maxMove, mv);
                sumMove += mv;
            }
            result.points += n;
            newPositions.push_back(np);
            newInvBind.push_back(ib);
        }
        result.meanMove = result.points ? sumMove / (double)result.points : 0.0;
        return true;
    }

    std::string versionPathFor(const std::string& garmentPath, const std::string& bodyPath)
    {
        std::string g = garmentPath;
        const size_t dot = g.find_last_of('.');
        const size_t slash = g.find_last_of("/\\");
        std::string base = (dot != std::string::npos && (slash == std::string::npos || dot > slash)) ? g.substr(0, dot) : g;
        if (base.size() >= 4 && base.compare(base.size() - 4, 4, "_fit") == 0) base = base.substr(0, base.size() - 4);
        std::string body = bodyPath;
        const size_t bs = body.find_last_of("/\\");
        if (bs != std::string::npos) body = body.substr(bs + 1);
        const size_t bd = body.find_last_of('.');
        if (bd != std::string::npos) body = body.substr(0, bd);
        std::string clean;
        for (char c : body) clean += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
        if (clean.empty()) clean = "body";
        return base + "_for_" + clean + ".dae";
    }
}
