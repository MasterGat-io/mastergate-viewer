/**
 * @file mgcreatorweights.cpp
 * @brief MasterGate Viewer: Creator Lab's weight tools for rigged garments.
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

#include "mgcreatorweights.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <unordered_map>

namespace MGCreatorWeights
{
    // ---------------- matrices (COLLADA: m[row * 4 + col], p' = M p) ----------------
    Mat multiply(const Mat& a, const Mat& b)
    {
        Mat r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
            {
                double s = 0.0;
                for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
                r[i * 4 + j] = s;
            }
        return r;
    }

    bool invert(const Mat& m, Mat& out)
    {
        // Gauss-Jordan on a 4x8 block
        double a[4][8];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 8; ++j)
                a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
        for (int c = 0; c < 4; ++c)
        {
            int piv = c;
            for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
            if (std::fabs(a[piv][c]) < 1e-12) return false;
            if (piv != c) for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[piv][j]);
            const double d = a[c][c];
            for (int j = 0; j < 8; ++j) a[c][j] /= d;
            for (int r = 0; r < 4; ++r)
            {
                if (r == c) continue;
                const double f = a[r][c];
                if (f == 0.0) continue;
                for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
            }
        }
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                out[i * 4 + j] = a[i][j + 4];
        return true;
    }

    std::array<double, 3> transformPoint(const Mat& m, const double p[3])
    {
        return { m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3],
                 m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
                 m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11] };
    }

    static Mat identity()
    {
        return { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    }

    // ---------------- helpers ----------------
    static int jointIndex(Skin& s, const std::string& name, const Mat& invBind, Stats& st)
    {
        for (size_t i = 0; i < s.joints.size(); ++i) if (s.joints[i] == name) return (int)i;
        s.joints.push_back(name);
        s.invBind.push_back(invBind);
        ++st.added;
        return (int)s.joints.size() - 1;
    }

    static void merge(VertexWeights& w)
    {
        std::map<int, double> m;
        for (const auto& jw : w) m[jw.first] += jw.second;
        w.clear();
        for (const auto& kv : m) if (kv.second > 0.0) w.push_back(kv);
    }

    static std::vector<std::vector<int>> neighbours(size_t n, const std::vector<std::array<int, 3>>& tris)
    {
        std::vector<std::vector<int>> nb(n);
        for (const auto& t : tris)
            for (int k = 0; k < 3; ++k)
            {
                const int a = t[k], b = t[(k + 1) % 3];
                if (a < 0 || b < 0 || a >= (int)n || b >= (int)n) continue;
                nb[a].push_back(b);
                nb[b].push_back(a);
            }
        for (auto& l : nb) { std::sort(l.begin(), l.end()); l.erase(std::unique(l.begin(), l.end()), l.end()); }
        return nb;
    }

    Skin fromMesh(const MGCreatorDae::SkinnedMesh& m)
    {
        Skin s;
        s.joints = m.joints;
        s.invBind = m.invBind;
        s.weights = m.weights;
        return s;
    }

    // ---------------- limit / unweighted / smooth ----------------
    Stats limit(Skin& s, int maxInfluences, double minWeight)
    {
        Stats st;
        for (auto& w : s.weights)
        {
            const VertexWeights before = w;
            merge(w);
            if ((int)w.size() > maxInfluences) ++st.over4;
            std::sort(w.begin(), w.end(), [](const std::pair<int, double>& a, const std::pair<int, double>& b) { return a.second > b.second; });
            if ((int)w.size() > maxInfluences) w.resize(maxInfluences);
            double sum = 0.0;
            for (const auto& jw : w) sum += jw.second;
            if (sum > 0.0)
            {
                VertexWeights kept;
                for (const auto& jw : w) if (jw.second / sum >= minWeight) kept.push_back(jw);
                w = kept;
                sum = 0.0;
                for (const auto& jw : w) sum += jw.second;
                for (auto& jw : w) jw.second /= sum;
            }
            if (w.empty()) ++st.unweighted;
            if (w != before) ++st.changed;
        }
        return st;
    }

    Stats fixUnweighted(Skin& s, const std::vector<std::array<int, 3>>& tris)
    {
        Stats st;
        const auto nb = neighbours(s.weights.size(), tris);
        bool progress = true;
        while (progress)
        {
            progress = false;
            for (size_t v = 0; v < s.weights.size(); ++v)
            {
                if (!s.weights[v].empty()) continue;
                VertexWeights acc;
                for (int u : nb[v]) for (const auto& jw : s.weights[u]) acc.push_back(jw);
                if (acc.empty()) continue;
                merge(acc);
                s.weights[v] = acc;
                ++st.changed;
                progress = true;
            }
        }
        Stats l = limit(s);
        st.unweighted = l.unweighted;
        return st;
    }

    Stats smooth(Skin& s, const std::vector<std::array<int, 3>>& tris, int passes, double amount)
    {
        Stats st;
        const auto nb = neighbours(s.weights.size(), tris);
        amount = std::max(0.0, std::min(1.0, amount));
        const std::vector<VertexWeights> before = s.weights;
        for (int p = 0; p < passes; ++p)
        {
            std::vector<VertexWeights> next = s.weights;
            for (size_t v = 0; v < s.weights.size(); ++v)
            {
                if (nb[v].empty()) continue;
                std::map<int, double> acc;
                for (const auto& jw : s.weights[v]) acc[jw.first] += (1.0 - amount) * jw.second;
                const double share = amount / (double)nb[v].size();
                for (int u : nb[v]) for (const auto& jw : s.weights[u]) acc[jw.first] += share * jw.second;
                VertexWeights w;
                for (const auto& kv : acc) if (kv.second > 0.0) w.push_back(kv);
                next[v] = w;
            }
            s.weights.swap(next);
            limit(s);   // 4 per vertex after every pass, as Second Life keeps
        }
        for (size_t v = 0; v < s.weights.size(); ++v) if (s.weights[v] != before[v]) ++st.changed;
        return st;
    }

    // ---------------- copy from a devkit ----------------
    struct V3 { double x, y, z; };
    static V3 sub(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    static double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    // Ericson, Real-Time Collision Detection 5.1.5: closest point on a triangle + barycentrics
    static V3 closestOnTriangle(const V3& p, const V3& a, const V3& b, const V3& c, double& u, double& v, double& w)
    {
        const V3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
        const double d1 = dot(ab, ap), d2 = dot(ac, ap);
        if (d1 <= 0 && d2 <= 0) { u = 1; v = 0; w = 0; return a; }
        const V3 bp = sub(p, b);
        const double d3 = dot(ab, bp), d4 = dot(ac, bp);
        if (d3 >= 0 && d4 <= d3) { u = 0; v = 1; w = 0; return b; }
        const double vc = d1 * d4 - d3 * d2;
        if (vc <= 0 && d1 >= 0 && d3 <= 0)
        {
            const double t = d1 / (d1 - d3);
            u = 1 - t; v = t; w = 0;
            return { a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t };
        }
        const V3 cp = sub(p, c);
        const double d5 = dot(ab, cp), d6 = dot(ac, cp);
        if (d6 >= 0 && d5 <= d6) { u = 0; v = 0; w = 1; return c; }
        const double vb = d5 * d2 - d1 * d6;
        if (vb <= 0 && d2 >= 0 && d6 <= 0)
        {
            const double t = d2 / (d2 - d6);
            u = 1 - t; v = 0; w = t;
            return { a.x + ac.x * t, a.y + ac.y * t, a.z + ac.z * t };
        }
        const double va = d3 * d6 - d5 * d4;
        if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
        {
            const double t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
            u = 0; v = 1 - t; w = t;
            return { b.x + (c.x - b.x) * t, b.y + (c.y - b.y) * t, b.z + (c.z - b.z) * t };
        }
        const double denom = 1.0 / (va + vb + vc);
        v = vb * denom;
        w = vc * denom;
        u = 1.0 - v - w;
        return { a.x + ab.x * v + ac.x * w, a.y + ab.y * v + ac.y * w, a.z + ab.z * v + ac.z * w };
    }

    static Mat localMatrix(const CollisionVolume& cv);

    Stats copyFromDevkit(Skin& s, const std::vector<double>& positions, const Mat& bindShape,
                         const MGCreatorDae::File& devkit, double maxDist,
                         const std::vector<CollisionVolume>* volumes)
    {
        Stats st;
        struct DT { int mesh; int a, b, c; };
        std::vector<V3> pts;            // devkit points in bind space, all meshes
        std::vector<size_t> base;       // first point of each devkit mesh
        std::vector<DT> tris;
        for (size_t mi = 0; mi < devkit.meshes.size(); ++mi)
        {
            const auto& dm = devkit.meshes[mi];
            base.push_back(pts.size());
            for (size_t v = 0; v < dm.vertexCount(); ++v)
            {
                const auto q = transformPoint(dm.bindShape, &dm.positions[v * 3]);
                pts.push_back({ q[0], q[1], q[2] });
            }
            for (const auto& t : dm.triangles)
            {
                if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= (int)dm.vertexCount() || t[1] >= (int)dm.vertexCount() || t[2] >= (int)dm.vertexCount()) continue;
                tris.push_back({ (int)mi, t[0], t[1], t[2] });
            }
        }
        if (tris.empty()) return st;

        // a grid of triangles
        const double cell = 0.02;
        auto key = [](long long x, long long y, long long z) { return ((x + (1 << 20)) << 42) | ((y + (1 << 20)) << 21) | (z + (1 << 20)); };
        auto ci = [cell](double v) { return (long long)std::floor(v / cell); };
        std::unordered_map<long long, std::vector<int>> grid;
        for (int i = 0; i < (int)tris.size(); ++i)
        {
            const DT& t = tris[i];
            const size_t b = base[t.mesh];
            const V3 &a = pts[b + t.a], &bb = pts[b + t.b], &c = pts[b + t.c];
            const long long x0 = ci(std::min(a.x, std::min(bb.x, c.x))), x1 = ci(std::max(a.x, std::max(bb.x, c.x)));
            const long long y0 = ci(std::min(a.y, std::min(bb.y, c.y))), y1 = ci(std::max(a.y, std::max(bb.y, c.y)));
            const long long z0 = ci(std::min(a.z, std::min(bb.z, c.z))), z1 = ci(std::max(a.z, std::max(bb.z, c.z)));
            // a triangle over 1 m across is not skin (a hidden or collapsed part): it would fill the grid
            if (x1 - x0 > 50 || y1 - y0 > 50 || z1 - z0 > 50) continue;
            for (long long x = x0; x <= x1; ++x)
                for (long long y = y0; y <= y1; ++y)
                    for (long long z = z0; z <= z1; ++z)
                        grid[key(x, y, z)].push_back(i);
        }

        const int maxRing = (int)std::ceil(maxDist / cell) + 1;
        std::vector<int> stamp(tris.size(), -1);
        const size_t n = positions.size() / 3;
        for (size_t v = 0; v < n && v < s.weights.size(); ++v)
        {
            const auto q3 = transformPoint(bindShape, &positions[v * 3]);
            const V3 q{ q3[0], q3[1], q3[2] };
            const long long cx = ci(q.x), cy = ci(q.y), cz = ci(q.z);
            double best = maxDist * maxDist;
            int bestT = -1;
            double bu = 0, bv = 0, bw = 0;
            for (int r = 0; r <= maxRing; ++r)
            {
                for (long long x = cx - r; x <= cx + r; ++x)
                    for (long long y = cy - r; y <= cy + r; ++y)
                    {
                        // the shell of this ring only: a full z column on its sides, else its two caps
                        const bool side = std::llabs(x - cx) == r || std::llabs(y - cy) == r;
                        const long long zstep = side ? 1 : (r > 0 ? 2 * (long long)r : 1);
                        for (long long z = cz - r; z <= cz + r; z += zstep)
                        {
                            auto it = grid.find(key(x, y, z));
                            if (it == grid.end()) continue;
                            for (int ti : it->second)
                            {
                                if (stamp[ti] == (int)v) continue;
                                stamp[ti] = (int)v;
                                const DT& t = tris[ti];
                                const size_t b = base[t.mesh];
                                double u, vv, w;
                                const V3 c = closestOnTriangle(q, pts[b + t.a], pts[b + t.b], pts[b + t.c], u, vv, w);
                                const V3 d = sub(q, c);
                                const double d2 = dot(d, d);
                                if (d2 < best) { best = d2; bestT = ti; bu = u; bv = vv; bw = w; }
                            }
                        }
                    }
                // nothing nearer than this ring can still be found
                if (bestT >= 0 && std::sqrt(best) <= r * cell) break;
            }
            if (bestT < 0) { ++st.tooFar; continue; }
            const DT& t = tris[bestT];
            const auto& dm = devkit.meshes[t.mesh];
            std::map<std::string, double> acc;
            const int corner[3] = { t.a, t.b, t.c };
            const double bary[3] = { bu, bv, bw };
            for (int k = 0; k < 3; ++k)
                for (const auto& jw : dm.weights[corner[k]])
                    if (jw.first >= 0 && jw.first < (int)dm.joints.size()) acc[dm.joints[jw.first]] += bary[k] * jw.second;
            VertexWeights w;
            for (const auto& kv : acc)
            {
                if (kv.second <= 0.0) continue;
                // the devkit's inverse bind for a bone the garment does not have yet
                size_t dj = 0;
                while (dj < dm.joints.size() && dm.joints[dj] != kv.first) ++dj;
                Mat ib = dj < dm.invBind.size() ? dm.invBind[dj] : identity();
                if (volumes)
                {
                    for (const auto& cv : *volumes)
                    {
                        if (cv.name != kv.first) continue;
                        for (size_t pj = 0; pj < s.joints.size(); ++pj)
                        {
                            Mat parentBind;
                            if (s.joints[pj] != cv.parent || !invert(s.invBind[pj], parentBind)) continue;
                            Mat vb;
                            if (invert(multiply(parentBind, localMatrix(cv)), vb)) ib = vb;
                        }
                    }
                }
                w.push_back({ jointIndex(s, kv.first, ib, st), kv.second });
            }
            if (w.empty()) { ++st.tooFar; continue; }
            s.weights[v] = w;
            ++st.changed;
            st.maxDist = std::max(st.maxDist, std::sqrt(best));
        }
        Stats l = limit(s);
        st.over4 = l.over4;
        st.unweighted = l.unweighted;
        return st;
    }

    // ---------------- fitted mesh ----------------
    static void readTriple(const std::string& tag, const std::string& name, double out[3])
    {
        const std::string key = " " + name + "=\"";
        const size_t p = tag.find(key);
        if (p == std::string::npos) return;
        std::istringstream in(tag.substr(p + key.size(), tag.find('"', p + key.size()) - p - key.size()));
        in >> out[0] >> out[1] >> out[2];
    }

    static std::string readAttr(const std::string& tag, const std::string& name)
    {
        const std::string key = " " + name + "=\"";
        const size_t p = tag.find(key);
        if (p == std::string::npos) return std::string();
        const size_t e = tag.find('"', p + key.size());
        return tag.substr(p + key.size(), e - p - key.size());
    }

    bool loadCollisionVolumes(const std::string& path, std::vector<CollisionVolume>& out)
    {
        out.clear();
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        const std::string t((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<std::string> stack;   // the bones we are inside
        size_t p = 0;
        while ((p = t.find('<', p)) != std::string::npos)
        {
            const size_t e = t.find('>', p);
            if (e == std::string::npos) break;
            const std::string tag = t.substr(p, e - p + 1);
            const bool closing = tag.size() > 1 && tag[1] == '/';
            const bool selfClosed = tag.size() > 2 && tag[tag.size() - 2] == '/';
            if (!closing && tag.compare(0, 5, "<bone") == 0 && (tag[5] == ' ' || tag[5] == '\n' || tag[5] == '\t' || tag[5] == '\r'))
            {
                if (!selfClosed) stack.push_back(readAttr(tag, "name"));
            }
            else if (closing && tag.compare(0, 6, "</bone") == 0)
            {
                if (!stack.empty()) stack.pop_back();
            }
            else if (!closing && tag.compare(0, 17, "<collision_volume") == 0 && !stack.empty())
            {
                CollisionVolume cv;
                cv.name = readAttr(tag, "name");
                cv.parent = stack.back();
                readTriple(tag, "pos", cv.pos);
                readTriple(tag, "rot", cv.rot);
                readTriple(tag, "scale", cv.scale);
                if (!cv.name.empty()) out.push_back(cv);
            }
            p = e + 1;
        }
        return !out.empty();
    }

    static Mat localMatrix(const CollisionVolume& cv)
    {
        // T(pos) * R(rot, degrees, X then Y then Z) * S(scale)
        const double k = 3.14159265358979323846 / 180.0;
        const double cx = std::cos(cv.rot[0] * k), sx = std::sin(cv.rot[0] * k);
        const double cy = std::cos(cv.rot[1] * k), sy = std::sin(cv.rot[1] * k);
        const double cz = std::cos(cv.rot[2] * k), sz = std::sin(cv.rot[2] * k);
        const Mat rx = { 1, 0, 0, 0, 0, cx, -sx, 0, 0, sx, cx, 0, 0, 0, 0, 1 };
        const Mat ry = { cy, 0, sy, 0, 0, 1, 0, 0, -sy, 0, cy, 0, 0, 0, 0, 1 };
        const Mat rz = { cz, -sz, 0, 0, sz, cz, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        Mat r = multiply(rz, multiply(ry, rx));
        const Mat sc = { cv.scale[0], 0, 0, 0, 0, cv.scale[1], 0, 0, 0, 0, cv.scale[2], 0, 0, 0, 0, 1 };
        r = multiply(r, sc);
        r[3] = cv.pos[0];
        r[7] = cv.pos[1];
        r[11] = cv.pos[2];
        return r;
    }

    Stats toFitted(Skin& s, const std::vector<double>& positions, const Mat& bindShape,
                   const std::vector<CollisionVolume>& volumes, double fraction)
    {
        Stats st;
        fraction = std::max(0.0, std::min(1.0, fraction));
        // each bone of the garment that has collision volumes: their binds and centres
        struct Cand { std::string name; Mat invBind; double c[3]; };
        std::map<int, std::vector<Cand>> byBone;
        for (size_t j = 0; j < s.joints.size(); ++j)
        {
            Mat parentBind;
            if (!invert(s.invBind[j], parentBind)) continue;
            for (const auto& cv : volumes)
            {
                if (cv.parent != s.joints[j]) continue;
                const Mat bind = multiply(parentBind, localMatrix(cv));
                Cand c;
                c.name = cv.name;
                if (!invert(bind, c.invBind)) continue;
                c.c[0] = bind[3]; c.c[1] = bind[7]; c.c[2] = bind[11];
                byBone[(int)j].push_back(c);
            }
        }
        if (byBone.empty() || fraction <= 0.0) return st;
        const size_t n = positions.size() / 3;
        for (size_t v = 0; v < n && v < s.weights.size(); ++v)
        {
            const auto q = transformPoint(bindShape, &positions[v * 3]);
            VertexWeights out;
            bool moved = false;
            for (const auto& jw : s.weights[v])
            {
                auto it = byBone.find(jw.first);
                if (it == byBone.end()) { out.push_back(jw); continue; }
                // distance to each of the bone's volumes in the volume's own units (its inverse
                // bind holds its size: 1 = on the volume, as the shape sliders scale it)
                std::vector<std::pair<double, const Cand*>> ds;
                for (const auto& c : it->second)
                {
                    const double qq[3] = { q[0], q[1], q[2] };
                    const auto l = transformPoint(c.invBind, qq);
                    ds.push_back({ std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]), &c });
                }
                std::sort(ds.begin(), ds.end(), [](const std::pair<double, const Cand*>& a, const std::pair<double, const Cand*>& b) { return a.first < b.first; });
                // on the body part: all of it; beyond 2.5 units it fades out, beyond 3.5 the
                // point stays on the bone (a strap far from the upper arm must not swell with it)
                const double d1 = ds[0].first;
                const double keep = fraction * std::max(0.0, std::min(1.0, (3.5 - d1) / 1.0));
                if (keep <= 0.0) { out.push_back(jw); continue; }
                // two volumes almost as near (butt / pelvis, belly / love handle): shared,
                // so there is no hard border between them
                double share1 = 1.0;
                if (ds.size() > 1 && ds[1].first < d1 * 1.5)
                {
                    const double a = 1.0 / std::max(d1, 0.05), b = 1.0 / std::max(ds[1].first, 0.05);
                    share1 = a / (a + b);
                    out.push_back({ jointIndex(s, ds[1].second->name, ds[1].second->invBind, st), jw.second * keep * (1.0 - share1) });
                }
                out.push_back({ jointIndex(s, ds[0].second->name, ds[0].second->invBind, st), jw.second * keep * share1 });
                if (keep < 1.0) out.push_back({ jw.first, jw.second * (1.0 - keep) });
                moved = true;
            }
            if (moved) { s.weights[v] = out; ++st.changed; }
        }
        Stats l = limit(s);
        st.over4 = l.over4;
        return st;
    }
}

namespace MGCreatorWeights
{
    void toSkeletonSpace(MGCreatorDae::File& body, const std::map<std::string, std::array<double, 3>>& bones,
                         const std::vector<CollisionVolume>& volumes)
    {
        auto restOf = [&](const std::string& name, Mat& out)
        {
            auto b = bones.find(name);
            if (b != bones.end()) { out = Mat{ 1, 0, 0, b->second[0], 0, 1, 0, b->second[1], 0, 0, 1, b->second[2], 0, 0, 0, 1 }; return true; }
            for (const auto& cv : volumes)
            {
                if (cv.name != name) continue;
                auto p = bones.find(cv.parent);
                if (p == bones.end()) return false;
                const Mat parent = Mat{ 1, 0, 0, p->second[0], 0, 1, 0, p->second[1], 0, 0, 1, p->second[2], 0, 0, 0, 1 };
                out = multiply(parent, localMatrix(cv));
                return true;
            }
            return false;
        };
        for (auto& m : body.meshes)
        {
            const size_t nj = m.joints.size();
            std::vector<Mat> carry(nj), newBind(nj, identity());
            std::vector<char> ok(nj, 0);
            for (size_t j = 0; j < nj && j < m.invBind.size(); ++j)
            {
                Mat rest;
                if (!restOf(m.joints[j], rest) || !invert(rest, newBind[j])) { newBind[j] = identity(); continue; }
                carry[j] = multiply(rest, multiply(m.invBind[j], m.bindShape));
                ok[j] = 1;
            }
            for (size_t v = 0; v < m.vertexCount() && v < m.weights.size(); ++v)
            {
                double acc[3] = { 0, 0, 0 }, sum = 0;
                VertexWeights kept;
                for (const auto& jw : m.weights[v])
                {
                    if (jw.first < 0 || jw.first >= (int)nj || !ok[jw.first] || jw.second <= 0) continue;
                    const auto q = transformPoint(carry[jw.first], &m.positions[v * 3]);
                    for (int k = 0; k < 3; ++k) acc[k] += jw.second * q[k];
                    sum += jw.second;
                    kept.push_back(jw);
                }
                if (sum <= 0) { m.weights[v].clear(); continue; }
                for (int k = 0; k < 3; ++k) m.positions[v * 3 + k] = acc[k] / sum;
                for (auto& jw : kept) jw.second /= sum;
                m.weights[v] = kept;
            }
            m.bindShape = identity();
            m.invBind = newBind;
        }
    }
}
