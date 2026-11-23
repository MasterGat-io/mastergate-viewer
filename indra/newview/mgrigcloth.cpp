/**
 * @file mgrigcloth.cpp
 * @brief MasterGate Viewer: Creator Lab, rigging clothes and accessories to SL's skeleton.
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

#include "mgrigcloth.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <map>
#include <set>

namespace
{
    using MGCreatorWeights::Mat;
    using MGCreatorWeights::Skin;
    const Mat IDENT = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

    struct Skel
    {
        std::map<std::string, int> index;   // SL bone or volume -> its place in sl
        std::map<std::string, Mat> devkit;  // devkit's inverse binds, by name
        const std::vector<MGRig::SlBone>* sl = nullptr;

        bool known(const std::string& n) const { return index.count(n) > 0; }
        bool volume(const std::string& n) const { auto it = index.find(n); return it != index.end() && (*sl)[it->second].volume; }
        // the inverse bind to give a bone: the devkit's, else SL's skeleton as it stands
        Mat bindFor(const std::string& n) const
        {
            auto d = devkit.find(n);
            if (d != devkit.end()) return d->second;
            auto it = index.find(n);
            if (it == index.end()) return IDENT;
            const MGRig::SlBone& b = (*sl)[it->second];
            return Mat{ 1, 0, 0, -b.world[0], 0, 1, 0, -b.world[1], 0, 0, 1, -b.world[2], 0, 0, 0, 1 };
        }
    };

    Skel skeleton(const MGCreatorDae::File* devkit, const std::vector<MGRig::SlBone>& sl)
    {
        Skel s;
        s.sl = &sl;
        for (size_t i = 0; i < sl.size(); ++i) s.index[sl[i].name] = (int)i;
        if (devkit)
            for (const auto& m : devkit->meshes)
                for (size_t j = 0; j < m.joints.size() && j < m.invBind.size(); ++j)
                    if (!s.devkit.count(m.joints[j])) s.devkit[m.joints[j]] = m.invBind[j];
        return s;
    }

    // bones an accessory is suggested on: the body's main bones and the ears (not the face's
    // small bones, the fingers or the spine's helpers)
    bool attachable(const std::string& n)
    {
        static const std::set<std::string> CORE = {
            "mPelvis", "mTorso", "mChest", "mNeck", "mHead", "mSkull", "mCollarLeft", "mCollarRight",
            "mShoulderLeft", "mShoulderRight", "mElbowLeft", "mElbowRight", "mWristLeft", "mWristRight",
            "mHipLeft", "mHipRight", "mKneeLeft", "mKneeRight", "mAnkleLeft", "mAnkleRight", "mFootLeft", "mFootRight",
            "mFaceEar1Left", "mFaceEar1Right" };
        return CORE.count(n) > 0;
    }

    void bounds(const MGRig::Mesh& m, double lo[3], double hi[3])
    {
        for (int k = 0; k < 3; ++k) { lo[k] = 1e30; hi[k] = -1e30; }
        for (size_t v = 0; v < m.vertexCount(); ++v)
            for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], m.pos[v * 3 + k]); hi[k] = std::max(hi[k], m.pos[v * 3 + k]); }
    }

    bool onSlBones(const MGRig::Mesh& m, const Skel& sk)
    {
        if (m.skinJoints.empty()) return false;
        size_t ok = 0;
        for (const auto& n : m.skinJoints) if (sk.known(n)) ++ok;
        return ok * 10 >= m.skinJoints.size() * 9;
    }

    int indexOf(Skin& s, const std::string& name, const Mat& ib)
    {
        for (size_t i = 0; i < s.joints.size(); ++i) if (s.joints[i] == name) return (int)i;
        s.joints.push_back(name);
        s.invBind.push_back(ib);
        return (int)s.joints.size() - 1;
    }

    // the devkit's bones (not its volumes) first: volumes are then bound from their parent
    // and SL's skeleton (a devkit's own volume binds hold only with joint positions)
    Skin seeded(const Skel& sk, size_t n)
    {
        Skin s;
        for (const auto& kv : sk.devkit)
            if (sk.known(kv.first) && !sk.volume(kv.first)) { s.joints.push_back(kv.first); s.invBind.push_back(kv.second); }
        s.weights.assign(n, MGCreatorWeights::VertexWeights());
        return s;
    }

    // Automatic weights (no body to copy from), like Blender's: each point is weighted by its
    // distance to the bone segments of the skeleton (the devkit's joints, else SL's), the
    // nearest few blended. The arms' and legs' segments end at the next joint; hands, feet and
    // head get a short segment of their own.
    Skin autoWeights(const MGRig::Mesh& m, const Skel& sk)
    {
        struct Seg { std::string bone; double a[3], b[3]; };
        auto at = [&](const std::string& n, double p[3]) -> bool
        {
            Mat w;
            if (!sk.known(n) || !MGCreatorWeights::invert(sk.bindFor(n), w)) return false;
            p[0] = w[3]; p[1] = w[7]; p[2] = w[11];
            return true;
        };
        static const char* const CHAIN[][2] = {
            { "mPelvis", "mTorso" }, { "mTorso", "mChest" }, { "mChest", "mNeck" }, { "mNeck", "mHead" },
            { "mCollarLeft", "mShoulderLeft" }, { "mShoulderLeft", "mElbowLeft" }, { "mElbowLeft", "mWristLeft" },
            { "mCollarRight", "mShoulderRight" }, { "mShoulderRight", "mElbowRight" }, { "mElbowRight", "mWristRight" },
            { "mHipLeft", "mKneeLeft" }, { "mKneeLeft", "mAnkleLeft" }, { "mAnkleLeft", "mFootLeft" },
            { "mHipRight", "mKneeRight" }, { "mKneeRight", "mAnkleRight" }, { "mAnkleRight", "mFootRight" } };
        std::vector<Seg> segs;
        for (const auto& c : CHAIN)
        {
            Seg g;
            g.bone = c[0];
            if (at(c[0], g.a) && at(c[1], g.b)) segs.push_back(g);
        }
        // the ends: head up, hands along the forearm, feet forward
        auto tip = [&](const char* bone, const char* from, double len)
        {
            Seg g;
            double f[3];
            g.bone = bone;
            if (!at(bone, g.a)) return;
            if (from && at(from, f))
            {
                double d[3] = { g.a[0] - f[0], g.a[1] - f[1], g.a[2] - f[2] };
                const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                for (int k = 0; k < 3; ++k) g.b[k] = g.a[k] + (l > 1e-6 ? d[k] / l : 0.0) * len;
            }
            else { g.b[0] = g.a[0]; g.b[1] = g.a[1]; g.b[2] = g.a[2] + len; }
            segs.push_back(g);
        };
        tip("mHead", nullptr, 0.18);
        tip("mWristLeft", "mElbowLeft", 0.16);
        tip("mWristRight", "mElbowRight", 0.16);
        tip("mFootLeft", "mAnkleLeft", 0.10);
        tip("mFootRight", "mAnkleRight", 0.10);
        Skin s;
        std::map<std::string, int> idx;
        for (const auto& g : segs)
            if (!idx.count(g.bone)) { idx[g.bone] = (int)s.joints.size(); s.joints.push_back(g.bone); s.invBind.push_back(sk.bindFor(g.bone)); }
        const size_t n = m.vertexCount();
        const size_t nb = s.joints.size();
        s.weights.assign(n, MGCreatorWeights::VertexWeights());

        // <MG> 2026-11-14: a bone only counts for a point when it lies BEHIND the garment's surface there
        // (on the inner side of its normal), as seen through the cloth. A garment made in an A pose has
        // its sleeves' undersides right next to the torso: by distance alone they took the torso's,
        // chest's and hips' bones, and raising the arms tore them into flaps. Points no bone clearly
        // shows behind are then filled in from their neighbours ON THE GARMENT (along the sleeve, not
        // across the gap to the torso): the weight inpainting of robust skin weight transfer.
        std::vector<double> N(n * 3, 0.0);
        bool haveN = m.nrm.size() >= n * 3;
        if (haveN)
        {
            size_t zero = 0;
            for (size_t v = 0; v < n; ++v) if (std::fabs(m.nrm[v * 3]) + std::fabs(m.nrm[v * 3 + 1]) + std::fabs(m.nrm[v * 3 + 2]) < 1e-9) ++zero;
            haveN = zero * 10 < n;
        }
        if (haveN) N.assign(m.nrm.begin(), m.nrm.begin() + n * 3);
        else
        {
            for (const auto& t : m.tris)
            {
                if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= (int)n || t[1] >= (int)n || t[2] >= (int)n) continue;
                const double* a = &m.pos[t[0] * 3]; const double* b = &m.pos[t[1] * 3]; const double* c = &m.pos[t[2] * 3];
                const double e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
                const double fn[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                for (int k = 0; k < 3; ++k) for (int q = 0; q < 3; ++q) N[t[k] * 3 + q] += fn[q];
            }
        }
        for (size_t v = 0; v < n; ++v)
        {
            const double l = std::sqrt(N[v * 3] * N[v * 3] + N[v * 3 + 1] * N[v * 3 + 1] + N[v * 3 + 2] * N[v * 3 + 2]);
            if (l > 1e-12) for (int q = 0; q < 3; ++q) N[v * 3 + q] /= l;
        }
#ifdef MG_CREATORDAE_STANDALONE
        // offline tuning only
        const double SURE_FACING = std::getenv("SURE_FACING") ? std::atof(std::getenv("SURE_FACING")) : 0.7;
        const double SURE_RATIO = std::getenv("SURE_RATIO") ? std::atof(std::getenv("SURE_RATIO")) : 1.2;
#else
        const double SURE_FACING = 0.7, SURE_RATIO = 1.2;   // tuned on an A-pose T-shirt (aposetest, 2026-11-14)
#endif
        struct Near { double dist, facing; int bone; };
        std::vector<std::vector<Near>> boneNear(n);   // (not "near": a Windows macro in the viewer)
        long agree = 0;
        for (size_t v = 0; v < n; ++v)
        {
            const double p[3] = { m.pos[v * 3], m.pos[v * 3 + 1], m.pos[v * 3 + 2] };
            auto& d = boneNear[v];
            for (const auto& g : segs)
            {
                double ab[3], ap[3];
                for (int k = 0; k < 3; ++k) { ab[k] = g.b[k] - g.a[k]; ap[k] = p[k] - g.a[k]; }
                const double l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
                double t = l2 > 1e-12 ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l2 : 0.0;
                t = std::max(0.0, std::min(1.0, t));
                double e = 0.0, f = 0.0, q[3];
                for (int k = 0; k < 3; ++k) { q[k] = p[k] - (g.a[k] + ab[k] * t); e += q[k] * q[k]; }
                const double dist = std::sqrt(e);
                if (dist > 1e-9) f = (q[0] * N[v * 3] + q[1] * N[v * 3 + 1] + q[2] * N[v * 3 + 2]) / dist;   // 1: the bone right behind
                d.push_back({ dist, f, idx[g.bone] });
            }
            std::sort(d.begin(), d.end(), [](const Near& a, const Near& b) { return a.dist < b.dist; });
            if (!d.empty()) agree += d[0].facing > 0.0 ? 1 : d[0].facing < 0.0 ? -1 : 0;
        }
        const double orient = agree < 0 ? -1.0 : 1.0;   // the file's normals point inward: read them the other way
        // the nearest few of the given bones, those almost as near shared (no hard line between two bones)
        auto blend = [&](const std::vector<const Near*>& c, std::vector<double>& w)
        {
            w.assign(nb, 0.0);
            double sum = 0.0;
            for (size_t k = 0; k < c.size() && k < 4; ++k)
            {
                if (k > 0 && c[k]->dist > c[0]->dist * 1.6 + 0.01) break;
                const double x = 1.0 / std::pow(c[k]->dist + 0.02, 4.0);
                w[c[k]->bone] += x;
                sum += x;
            }
            if (sum > 0.0) for (double& x : w) x /= sum;
        };
        std::vector<std::vector<double>> W(n);
        std::vector<char> sure(n, 0);
        std::vector<const Near*> c;
        for (size_t v = 0; v < n; ++v)
        {
            // bones behind the surface (clearly: sure; a little: used, then refined from the neighbours)
            for (const double sign : { orient, -orient })
            {
                c.clear();
                for (const auto& x : boneNear[v]) if (x.facing * sign >= 0.2) c.push_back(&x);
                if (c.empty()) continue;
                blend(c, W[v]);
                // sure: on the normal's own side, the bone well behind, and not much farther than the nearest of all
                sure[v] = sign == orient && c[0]->facing * sign >= SURE_FACING && c[0]->dist <= boneNear[v][0].dist * SURE_RATIO + 0.03;
                break;
            }
            if (W[v].empty())
            {
                for (const auto& x : boneNear[v]) c.push_back(&x);
                blend(c, W[v]);
            }
        }
        // the unsure points: filled in from their garment neighbours (Gauss-Seidel on the mesh, the sure ones held)
        std::vector<std::vector<int>> adj(n);
        for (const auto& t : m.tris)
        {
            for (int k = 0; k < 3; ++k)
            {
                const int a = t[k], b = t[(k + 1) % 3];
                if (a < 0 || b < 0 || a >= (int)n || b >= (int)n || a == b) continue;
                adj[a].push_back(b);
                adj[b].push_back(a);
            }
        }
        size_t unsure = 0;
        for (size_t v = 0; v < n; ++v) if (!sure[v] && !adj[v].empty()) ++unsure;
        if (unsure > 0 && unsure < n)
        {
            std::vector<double> acc(nb);
            for (int it = 0; it < 150; ++it)
            {
                double change = 0.0;
                for (size_t v = 0; v < n; ++v)
                {
                    if (sure[v] || adj[v].empty()) continue;
                    std::fill(acc.begin(), acc.end(), 0.0);
                    for (int u : adj[v]) for (size_t j = 0; j < nb; ++j) acc[j] += W[u][j];
                    const double inv = 1.0 / (double)adj[v].size();
                    for (size_t j = 0; j < nb; ++j) { const double x = acc[j] * inv; change = std::max(change, std::fabs(x - W[v][j])); W[v][j] = x; }
                }
                if (change < 1e-4) break;
            }
        }
        for (size_t v = 0; v < n; ++v)
        {
            double sum = 0.0;
            for (size_t j = 0; j < nb; ++j) if (W[v][j] >= 0.01) sum += W[v][j];
            if (sum <= 0.0) continue;
            for (size_t j = 0; j < nb; ++j) if (W[v][j] >= 0.01) s.weights[v].push_back({ (int)j, W[v][j] / sum });
        }
        return s;
    }

    // Place on the body: moved, turned about the vertical, resized, about the pieces' centre
    void place(MGRig::Model& g, const MGRigCloth::Options& o)
    {
        if (o.hasMatrix)
        {
            const double* M = o.matrix;   // column-major
            // normals: the inverse transpose of the 3x3 part
            const double a = M[0], b = M[4], cc = M[8], d = M[1], e = M[5], f = M[9], gg = M[2], h = M[6], i = M[10];
            const double A = e * i - f * h, B = -(d * i - f * gg), C = d * h - e * gg;
            const double det = a * A + b * B + cc * C;
            double N[9] = { 0 };
            if (std::fabs(det) > 1e-12)
            {
                // inverse(M3)^T = cofactor matrix / det
                N[0] = A / det; N[1] = B / det; N[2] = C / det;
                N[3] = -(b * i - cc * h) / det; N[4] = (a * i - cc * gg) / det; N[5] = -(a * h - b * gg) / det;
                N[6] = (b * f - cc * e) / det; N[7] = -(a * f - cc * d) / det; N[8] = (a * e - b * d) / det;
            }
            for (auto& m : g.meshes)
                for (size_t v = 0; v < m.vertexCount(); ++v)
                {
                    const double x = m.pos[v * 3], y = m.pos[v * 3 + 1], z = m.pos[v * 3 + 2];
                    m.pos[v * 3] = M[0] * x + M[4] * y + M[8] * z + M[12];
                    m.pos[v * 3 + 1] = M[1] * x + M[5] * y + M[9] * z + M[13];
                    m.pos[v * 3 + 2] = M[2] * x + M[6] * y + M[10] * z + M[14];
                    if (m.nrm.size() >= (v + 1) * 3 && std::fabs(det) > 1e-12)
                    {
                        const double nx = m.nrm[v * 3], ny = m.nrm[v * 3 + 1], nz = m.nrm[v * 3 + 2];
                        double r[3] = { N[0] * nx + N[1] * ny + N[2] * nz, N[3] * nx + N[4] * ny + N[5] * nz, N[6] * nx + N[7] * ny + N[8] * nz };   // N row-major
                        const double l = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
                        if (l > 1e-12) for (int k = 0; k < 3; ++k) m.nrm[v * 3 + k] = r[k] / l;
                    }
                }
            return;
        }
        if (std::fabs(o.move[0]) + std::fabs(o.move[1]) + std::fabs(o.move[2]) < 1e-9 && std::fabs(o.turn) < 1e-9 && std::fabs(o.scale - 1.0) < 1e-9) return;
        double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
        for (const auto& m : g.meshes)
        {
            double a[3], b[3];
            bounds(m, a, b);
            for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], a[k]); hi[k] = std::max(hi[k], b[k]); }
        }
        const double c[3] = { (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2 };
        const double a = o.turn * 3.14159265358979323846 / 180.0, cs = std::cos(a), sn = std::sin(a);
        const double sc = std::max(0.1, std::min(10.0, o.scale));
        for (auto& m : g.meshes)
        {
            for (size_t v = 0; v < m.vertexCount(); ++v)
            {
                const double x = (m.pos[v * 3] - c[0]) * sc, y = (m.pos[v * 3 + 1] - c[1]) * sc, z = (m.pos[v * 3 + 2] - c[2]) * sc;
                m.pos[v * 3] = c[0] + cs * x - sn * y + o.move[0];
                m.pos[v * 3 + 1] = c[1] + sn * x + cs * y + o.move[1];
                m.pos[v * 3 + 2] = c[2] + z + o.move[2];
                if (m.nrm.size() >= (v + 1) * 3)
                {
                    const double nx = m.nrm[v * 3], ny = m.nrm[v * 3 + 1];
                    m.nrm[v * 3] = cs * nx - sn * ny;
                    m.nrm[v * 3 + 1] = sn * nx + cs * ny;
                }
            }
        }
    }

    // points at the same place (a UV seam's two sides) take the same weights: their average
    void weld(Skin& s, const std::vector<double>& pos)
    {
        std::map<std::array<long long, 3>, std::vector<size_t>> at;
        const size_t n = std::min(s.weights.size(), pos.size() / 3);
        for (size_t v = 0; v < n; ++v)
            at[{ std::llround(pos[v * 3] * 1e6), std::llround(pos[v * 3 + 1] * 1e6), std::llround(pos[v * 3 + 2] * 1e6) }].push_back(v);
        for (const auto& kv : at)
        {
            if (kv.second.size() < 2) continue;
            std::map<int, double> acc;
            for (size_t v : kv.second) for (const auto& jw : s.weights[v]) acc[jw.first] += jw.second / (double)kv.second.size();
            MGCreatorWeights::VertexWeights w(acc.begin(), acc.end());
            for (size_t v : kv.second) s.weights[v] = w;
        }
    }

    // bones nothing is weighted to are dropped
    void compact(Skin& s)
    {
        std::vector<int> used(s.joints.size(), 0);
        for (const auto& w : s.weights) for (const auto& jw : w) if (jw.first >= 0 && jw.first < (int)used.size() && jw.second > 0.0) used[jw.first] = 1;
        std::vector<int> remap(s.joints.size(), -1);
        Skin c;
        for (size_t j = 0; j < s.joints.size(); ++j)
            if (used[j]) { remap[j] = (int)c.joints.size(); c.joints.push_back(s.joints[j]); c.invBind.push_back(s.invBind[j]); }
        c.weights.resize(s.weights.size());
        for (size_t v = 0; v < s.weights.size(); ++v)
            for (const auto& jw : s.weights[v]) if (jw.first >= 0 && jw.first < (int)remap.size() && remap[jw.first] >= 0) c.weights[v].push_back({ remap[jw.first], jw.second });
        s = c;
    }
}

namespace MGRigCloth
{
    void analyse(const MGRig::Model& g, const MGCreatorDae::File* devkit, const std::vector<MGRig::SlBone>& sl, std::vector<Item>& items)
    {
        items.clear();
        const Skel sk = skeleton(devkit, sl);
        // the bones an accessory can ride on, and where they are
        std::vector<std::pair<std::string, std::array<double, 3>>> bones;
        for (const auto& b : sl)
        {
            if (b.volume || !attachable(b.name)) continue;
            Mat w;
            if (MGCreatorWeights::invert(sk.bindFor(b.name), w)) bones.push_back({ b.name, { w[3], w[7], w[11] } });
        }
        for (const auto& m : g.meshes)
        {
            Item it;
            it.name = m.name;
            it.verts = m.vertexCount();
            it.materials = (int)m.materials.size();
            it.rigged = m.rigged && !m.skinJoints.empty();
            it.slBones = onSlBones(m, sk);
            double lo[3], hi[3];
            bounds(m, lo, hi);
            it.size = std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]));
            const double c[3] = { (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2 };
            if (devkit && it.verts)
            {
                Skin tmp;
                tmp.weights.assign(it.verts, MGCreatorWeights::VertexWeights());
                const MGCreatorWeights::Stats st = MGCreatorWeights::copyFromDevkit(tmp, m.pos, IDENT, *devkit, 0.05, nullptr);
                it.nearBody = 1.0 - (double)st.tooFar / (double)it.verts;
            }
            double best = 1e30;
            for (const auto& b : bones)
            {
                const double d = (b.second[0] - c[0]) * (b.second[0] - c[0]) + (b.second[1] - c[1]) * (b.second[1] - c[1]) + (b.second[2] - c[2]) * (b.second[2] - c[2]);
                if (d < best) { best = d; it.bone = b.first; }
            }
            if (it.rigged && it.slBones) it.mode = "keep";
            else if (it.size < 0.12) it.mode = "bone";                     // earrings, rings, piercings
            else if (devkit && it.nearBody >= 0.05) it.mode = "skin";
            else it.mode = "auto";   // no body to copy from: weights from SL's bones (never one rigid bone)
            items.push_back(it);
        }
    }

    // the posed move of a bone (identity when not posed or unknown)
    Mat poseOf(const Options& o, const std::string& name)
    {
        auto it = o.pose.find(name);
        return it != o.pose.end() ? it->second : IDENT;
    }

    // a body (the reference devkit or the one worn) put into the pose: each point moved by its bones
    MGCreatorDae::File posedSource(const MGCreatorDae::File& src, const Options& o)
    {
        MGCreatorDae::File out;
        out.path = src.path; out.upAxis = src.upAxis; out.meter = src.meter;
        out.meshes = src.meshes;
        for (auto& m : out.meshes)
        {
            std::vector<Mat> D(m.joints.size());
            std::vector<char> ok(m.joints.size(), 1);
            for (size_t j = 0; j < m.joints.size(); ++j)
            {
                if (o.shown.empty()) { D[j] = poseOf(o, m.joints[j]); continue; }
                // <MG> where the body shows: the bone as shown x the body's own bind (as the renderer does)
                auto sh = o.shown.find(m.joints[j]);
                if (sh == o.shown.end() || j >= m.invBind.size()) { ok[j] = 0; continue; }
                D[j] = MGCreatorWeights::multiply(sh->second, m.invBind[j]);
            }
            for (size_t v = 0; v < m.vertexCount(); ++v)
            {
                const auto p = MGCreatorWeights::transformPoint(m.bindShape, &m.positions[v * 3]);
                double acc[3] = { 0, 0, 0 }, sum = 0;
                if (v < m.weights.size())
                    for (const auto& jw : m.weights[v])
                    {
                        if (jw.first < 0 || jw.first >= (int)D.size() || !ok[jw.first] || jw.second <= 0) continue;
                        const auto q = MGCreatorWeights::transformPoint(D[jw.first], p.data());
                        for (int k = 0; k < 3; ++k) acc[k] += jw.second * q[k];
                        sum += jw.second;
                    }
                for (int k = 0; k < 3; ++k) m.positions[v * 3 + k] = sum > 0 ? acc[k] / sum : p[k];
            }
            m.bindShape = IDENT;
        }
        return out;
    }

    // the skeleton in the pose (the automatic weights measure from the posed bones)
    Skel posedSkel(const Skel& sk, const Options& o)
    {
        Skel s = sk;
        if (!o.shown.empty())
        {
            // <MG> the joints where they show (only their places are measured)
            for (const auto& kv : o.shown)
            {
                Mat bi;
                if (sk.known(kv.first) && MGCreatorWeights::invert(kv.second, bi)) s.devkit[kv.first] = bi;
            }
            return s;
        }
        for (const auto& kv : sk.index)
        {
            Mat D = poseOf(o, kv.first), Di;
            if (!MGCreatorWeights::invert(D, Di)) continue;
            s.devkit[kv.first] = MGCreatorWeights::multiply(sk.bindFor(kv.first), Di);
        }
        return s;
    }

    // back to the rest pose: each point (and normal) through the inverse of its bones' blended pose
    void unpose(const Skin& s, const Options& o, std::vector<double>& pos, std::vector<double>& nrm)
    {
        std::vector<Mat> D(s.joints.size());
        for (size_t j = 0; j < s.joints.size(); ++j) D[j] = poseOf(o, s.joints[j]);
        const size_t n = pos.size() / 3;
        for (size_t v = 0; v < n && v < s.weights.size(); ++v)
        {
            Mat M{};
            double sum = 0;
            for (const auto& jw : s.weights[v])
            {
                if (jw.first < 0 || jw.first >= (int)D.size() || jw.second <= 0) continue;
                for (int k = 0; k < 16; ++k) M[k] += jw.second * D[jw.first][k];
                sum += jw.second;
            }
            if (sum <= 0) continue;
            for (int k = 0; k < 16; ++k) M[k] /= sum;
            Mat Mi;
            if (!MGCreatorWeights::invert(M, Mi)) continue;
            const auto q = MGCreatorWeights::transformPoint(Mi, &pos[v * 3]);
            for (int k = 0; k < 3; ++k) pos[v * 3 + k] = q[k];
            if (nrm.size() >= (v + 1) * 3)
            {
                // the inverse of the inverse transpose: the transpose of M's 3x3 part (row-major layout)
                const double nx = nrm[v * 3], ny = nrm[v * 3 + 1], nz = nrm[v * 3 + 2];
                double r[3] = { M[0] * nx + M[4] * ny + M[8] * nz, M[1] * nx + M[5] * ny + M[9] * nz, M[2] * nx + M[6] * ny + M[10] * nz };
                const double l = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
                if (l > 1e-12) for (int k = 0; k < 3; ++k) nrm[v * 3 + k] = r[k] / l;
            }
        }
    }

    bool rig(const MGRig::Model& g0, const std::vector<Choice>& choices, const MGCreatorDae::File* devkit,
             const std::vector<MGRig::SlBone>& sl, const std::vector<MGCreatorWeights::CollisionVolume>& cvs,
             const Options& o, MGRig::Model& out, Result& r, std::string& error,
             const MGCreatorDae::File* myBody)
    {
        out = MGRig::Model();
        r = Result();
        out.format = "sl";
        // the pieces where they were placed on the body
        MGRig::Model placed = g0;
        place(placed, o);
        const MGRig::Model& g = placed;
        const Skel sk = skeleton(devkit ? devkit : myBody, sl);
        if (!sk.known("mPelvis")) { error = "skeleton"; return false; }
        const bool posed = !o.pose.empty() || !o.shown.empty();
        MGCreatorDae::File devkitPosed, myBodyPosed;
        if (posed && devkit) devkitPosed = posedSource(*devkit, o);
        if (posed && myBody) myBodyPosed = posedSource(*myBody, o);
        const Skel skPosed = posed ? posedSkel(sk, o) : sk;
        std::map<std::string, Mat> usedBind;   // the bind each bone was given (for the joint nodes)
        for (size_t mi = 0; mi < g.meshes.size(); ++mi)
        {
            const MGRig::Mesh& m = g.meshes[mi];
            Choice ch = mi < choices.size() ? choices[mi] : Choice();
            const size_t n = m.vertexCount();
            if (ch.mode == "skip" || n == 0) { ++r.skipped; continue; }
            const bool own = m.rigged && onSlBones(m, sk);
            if (ch.mode == "keep" && !own)
            {
                r.warnings.push_back("keep_not_sl:" + m.name);
                ch.mode = devkit ? "skin" : myBody ? "mybody" : "bone";
            }
            if (ch.mode == "skin" && !devkit) { error = "devkit"; return false; }
            if (ch.mode == "mybody" && !myBody) { error = "my_body"; return false; }
            Skin s;
            if (ch.mode == "keep")
            {
                s.joints = m.skinJoints;
                s.invBind = m.skinInvBind;
                s.weights = m.weights;
                while (s.invBind.size() < s.joints.size()) s.invBind.push_back(sk.bindFor(s.joints[s.invBind.size()]));
                ++r.kept;
            }
            else if (ch.mode == "auto")
            {
                s = autoWeights(m, posed ? skPosed : sk);
                if (posed) for (size_t k = 0; k < s.joints.size(); ++k) s.invBind[k] = sk.bindFor(s.joints[k]);   // the rest binds in the file
                MGCreatorWeights::smooth(s, m.tris, std::max(2, std::min(8, ch.smooth + 1)), 0.5);
                ++r.skinned;
            }
            else if (ch.mode == "bone")
            {
                const std::string bone = sk.known(ch.bone) && !sk.volume(ch.bone) ? ch.bone : std::string("mPelvis");
                s.joints = { bone };
                s.invBind = { sk.bindFor(bone) };
                s.weights.assign(n, MGCreatorWeights::VertexWeights{ { 0, 1.0 } });
                ++r.attached;
            }
            else
            {
                // the body's nearest surface within reach (the reference body, or the one worn) ...
                const MGCreatorDae::File* src = ch.mode == "mybody" ? myBody : devkit;
                if (!src) { error = "devkit"; return false; }
                if (posed) src = ch.mode == "mybody" ? &myBodyPosed : &devkitPosed;   // the body as it stands in the pose
                s = seeded(sk, n);
                const MGCreatorWeights::Stats st = MGCreatorWeights::copyFromDevkit(s, m.pos, IDENT, *src, std::max(0.005, ch.distance), &cvs);
                r.farPoints += st.tooFar;
                if (st.tooFar * 2 > n) r.warnings.push_back("far:" + m.name);
                // ... out of reach: the neighbours' weights, then (separate pieces) the nearest surface
                MGCreatorWeights::fixUnweighted(s, m.tris);
                bool empty = false;
                for (const auto& w : s.weights) if (w.empty()) { empty = true; break; }
                if (empty)
                {
                    // the body's points, at most ~4000 of them (fast whatever the distance)
                    struct BP { double x, y, z; int mesh; size_t v; };
                    std::vector<BP> bp;
                    size_t bodyPts = 0;
                    for (const auto& dm : src->meshes) bodyPts += dm.vertexCount();
                    const size_t stride = std::max<size_t>(1, bodyPts / 4000);
                    size_t k = 0;
                    for (size_t mi2 = 0; mi2 < src->meshes.size(); ++mi2)
                    {
                        const auto& dm = src->meshes[mi2];
                        for (size_t v = 0; v < dm.vertexCount(); ++v, ++k)
                        {
                            if (k % stride || v >= dm.weights.size() || dm.weights[v].empty()) continue;
                            const auto q = MGCreatorWeights::transformPoint(dm.bindShape, &dm.positions[v * 3]);
                            bp.push_back({ q[0], q[1], q[2], (int)mi2, v });
                        }
                    }
                    for (size_t v = 0; v < n && !bp.empty(); ++v)
                    {
                        if (!s.weights[v].empty()) continue;
                        const double* p = &m.pos[v * 3];
                        double best = 1e30;
                        const BP* hit = nullptr;
                        for (const BP& b : bp)
                        {
                            const double d = (b.x - p[0]) * (b.x - p[0]) + (b.y - p[1]) * (b.y - p[1]) + (b.z - p[2]) * (b.z - p[2]);
                            if (d < best) { best = d; hit = &b; }
                        }
                        if (!hit) continue;
                        const auto& dm = src->meshes[hit->mesh];
                        for (const auto& jw : dm.weights[hit->v])
                        {
                            if (jw.first < 0 || jw.first >= (int)dm.joints.size()) continue;
                            const Mat ib = jw.first < (int)dm.invBind.size() ? dm.invBind[jw.first] : sk.bindFor(dm.joints[jw.first]);
                            s.weights[v].push_back({ indexOf(s, dm.joints[jw.first], ib), jw.second });
                        }
                    }
                }
                // ... mixed with the item's own weights (on SL bones)
                if (ch.mix < 0.999 && own)
                {
                    const double a = std::max(0.0, std::min(1.0, ch.mix));
                    for (size_t v = 0; v < n && v < m.weights.size(); ++v)
                    {
                        std::map<int, double> acc;
                        for (const auto& jw : s.weights[v]) acc[jw.first] += a * jw.second;
                        for (const auto& jw : m.weights[v])
                        {
                            if (jw.first < 0 || jw.first >= (int)m.skinJoints.size()) continue;
                            const Mat ib = jw.first < (int)m.skinInvBind.size() ? m.skinInvBind[jw.first] : sk.bindFor(m.skinJoints[jw.first]);
                            acc[indexOf(s, m.skinJoints[jw.first], ib)] += (1.0 - a) * jw.second;
                        }
                        s.weights[v].assign(acc.begin(), acc.end());
                    }
                }
                if (ch.smooth > 0) MGCreatorWeights::smooth(s, m.tris, std::min(8, ch.smooth), 0.5);
                ++r.skinned;
            }
            // posed to the garment: each point back to the rest pose with the weights just found
            std::vector<double> restPos = m.pos, restNrm = m.nrm;
            if (posed && o.unpose && !o.pose.empty()) unpose(s, o, restPos, restNrm);
            if (o.fitted && ch.mode != "bone" && !cvs.empty()) MGCreatorWeights::toFitted(s, restPos, IDENT, cvs, std::max(0.0, std::min(1.0, o.share)));
            weld(s, restPos);   // a seam's two sides: the same weights (else it opens when it moves)
            MGCreatorWeights::limit(s);
            compact(s);
            MGRig::Mesh om = m;
            om.pos = restPos;
            om.nrm = restNrm;
            om.skinJoints = s.joints;
            om.skinInvBind.assign(s.invBind.begin(), s.invBind.end());
            om.weights = s.weights;
            om.rigged = true;
            r.maxBones = std::max(r.maxBones, (int)s.joints.size());
            for (size_t j = 0; j < s.joints.size(); ++j) if (!usedBind.count(s.joints[j])) usedBind[s.joints[j]] = s.invBind[j];
            out.meshes.push_back(om);
        }
        if (out.meshes.empty()) { error = "nothing"; return false; }
        if (r.maxBones > 110) r.warnings.push_back("bones");

        // the joint nodes: bones in use and their parents, placed by their binds (else SL's offsets)
        std::vector<char> need(sl.size(), 0);
        for (const auto& kv : usedBind)
        {
            auto it = sk.index.find(kv.first);
            if (it == sk.index.end()) continue;
            for (int p = it->second; p >= 0; p = sl[p].parent) need[p] = 1;
        }
        std::vector<int> outIndex(sl.size(), -1);
        std::vector<std::array<double, 3>> world(sl.size());
        for (size_t i = 0; i < sl.size(); ++i)
        {
            if (!need[i]) continue;
            const MGRig::SlBone& b = sl[i];
            Mat w;
            auto ub = usedBind.find(b.name);
            const Mat ib = ub != usedBind.end() ? ub->second : sk.bindFor(b.name);
            if ((ub != usedBind.end() || sk.devkit.count(b.name)) && MGCreatorWeights::invert(ib, w)) world[i] = { w[3], w[7], w[11] };
            else if (b.parent >= 0) world[i] = { world[b.parent][0] + b.local[0], world[b.parent][1] + b.local[1], world[b.parent][2] + b.local[2] };
            else world[i] = { b.world[0], b.world[1], b.world[2] };
            MGRig::Joint j;
            j.name = b.name;
            j.parent = b.parent >= 0 ? outIndex[b.parent] : -1;
            j.bind = Mat{ 1, 0, 0, world[i][0], 0, 1, 0, world[i][1], 0, 0, 1, world[i][2], 0, 0, 0, 1 };
            outIndex[i] = (int)out.joints.size();
            out.joints.push_back(j);
        }
        return true;
    }

    bool loadBody(const std::string& path, const std::vector<MGRig::SlBone>& sl, MGCreatorDae::File& out,
                  std::vector<std::string>& pieces, std::string& error)
    {
        out = MGCreatorDae::File();
        pieces.clear();
        std::string ext;
        const size_t dot = path.find_last_of('.');
        if (dot != std::string::npos) ext = path.substr(dot + 1);
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        // a devkit .dae with one body: as it is
        if (ext == "dae" && MGCreatorDae::read(path, out) && !out.meshes.empty() && out.meshes.size() <= 2 && (out.upAxis.empty() || out.upAxis == "Z_UP"))
        {
            for (const auto& m : out.meshes) pieces.push_back(m.name.empty() ? m.geometryId : m.name);
            return true;
        }
        // a project file: the body among the pieces rigged to SL bones
        MGRig::Model model;
        if (!MGRig::readModel(path, model, false)) { error = model.error.empty() ? std::string("read") : model.error; return false; }
        std::set<std::string> slNames;
        for (const auto& b : sl) slNames.insert(b.name);
        struct Cand { size_t mesh; std::vector<std::string> names; std::vector<Mat> binds; bool body; };
        std::vector<Cand> cands;
        for (size_t mi = 0; mi < model.meshes.size(); ++mi)
        {
            const MGRig::Mesh& m = model.meshes[mi];
            Cand c;
            c.mesh = mi;
            if (!m.skinJoints.empty())
            {
                c.names = m.skinJoints;
                for (size_t j = 0; j < c.names.size(); ++j) c.binds.push_back(j < m.skinInvBind.size() ? m.skinInvBind[j] : IDENT);
            }
            else
            {
                for (const auto& j : model.joints)
                {
                    c.names.push_back(MGRig::plainName(j.name));
                    c.binds.push_back(Mat{ 1, 0, 0, -j.bind[3], 0, 1, 0, -j.bind[7], 0, 0, 1, -j.bind[11], 0, 0, 0, 1 });
                }
            }
            // on SL bones: nine in ten of the bones it is weighted to
            std::set<int> used;
            for (const auto& w : m.weights) for (const auto& jw : w) used.insert(jw.first);
            size_t ok = 0;
            for (int j : used) if (j >= 0 && j < (int)c.names.size() && slNames.count(c.names[j])) ++ok;
            if (used.empty() || ok * 10 < used.size() * 9) continue;
            std::string lower = m.name;
            for (auto& ch : lower) ch = (char)std::tolower((unsigned char)ch);
            c.body = lower.find("body") != std::string::npos;
            cands.push_back(c);
        }
        if (cands.empty()) { error = "no_body_mesh"; return false; }
        // the body: the piece standing the tallest and widest (a whole figure; shoes can have
        // more points), and its other parts (same name, ".001" ... endings: head, feet, eyes)
        auto extent = [&](const Cand& c)
        {
            double lo[3], hi[3];
            bounds(model.meshes[c.mesh], lo, hi);
            return (hi[2] - lo[2]) * std::max(hi[0] - lo[0], hi[1] - lo[1]);
        };
        auto base = [&](const Cand& c)
        {
            std::string n = model.meshes[c.mesh].name;
            const size_t d = n.find_last_of('.');
            if (d != std::string::npos && d + 1 < n.size() && std::all_of(n.begin() + d + 1, n.end(), [](char ch) { return std::isdigit((unsigned char)ch); })) n = n.substr(0, d);
            return n;
        };
        const Cand* main = &cands[0];
        for (const auto& c : cands) if (extent(c) > extent(*main)) main = &c;
        std::vector<const Cand*> pick;
        const std::string mainBase = base(*main);
        for (const auto& c : cands) if (&c == main || base(c) == mainBase || c.body) pick.push_back(&c);
        out.path = path;
        out.upAxis = "Z_UP";
        out.meter = 1.0;
        for (const Cand* c : pick)
        {
            const MGRig::Mesh& m = model.meshes[c->mesh];
            MGCreatorDae::SkinnedMesh sm;
            sm.name = m.name;
            sm.positions = m.pos;
            sm.triangles = m.tris;
            sm.bindShape = IDENT;
            // only the bones it uses, renumbered
            std::map<int, int> remap;
            sm.weights.resize(m.vertexCount());
            for (size_t v = 0; v < m.vertexCount() && v < m.weights.size(); ++v)
                for (const auto& jw : m.weights[v])
                {
                    if (jw.first < 0 || jw.first >= (int)c->names.size()) continue;
                    auto it = remap.find(jw.first);
                    if (it == remap.end())
                    {
                        it = remap.emplace(jw.first, (int)sm.joints.size()).first;
                        sm.joints.push_back(c->names[jw.first]);
                        sm.invBind.push_back(c->binds[jw.first]);
                    }
                    sm.weights[v].push_back({ it->second, jw.second });
                }
            out.meshes.push_back(sm);
            pieces.push_back(m.name);
        }
        return true;
    }

    std::string outPathFor(const std::string& src)
    {
        const size_t dot = src.find_last_of('.');
        const size_t slash = src.find_last_of("/\\");
        const std::string base = (dot != std::string::npos && (slash == std::string::npos || dot > slash)) ? src.substr(0, dot) : src;
        return base + "_rigged.dae";
    }
}
