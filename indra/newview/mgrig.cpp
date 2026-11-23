/**
 * @file mgrig.cpp
 * @brief MasterGate Viewer: Creator Lab's rig converter (to Second Life's skeleton).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * MasterGate Viewer, based on the Firestorm Viewer and the Second Life Viewer.
 * Bone maps: Bento Buddy (Critters LLC), used with the author's permission.
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

#include "mgrig.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

namespace
{
    using MGRig::Mat;

    struct V { double x = 0, y = 0, z = 0; };
    V v3(double x, double y, double z) { V r; r.x = x; r.y = y; r.z = z; return r; }
    V operator+(const V& a, const V& b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
    V operator-(const V& a, const V& b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
    V operator*(const V& a, double s) { return v3(a.x * s, a.y * s, a.z * s); }
    double dot(const V& a, const V& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    V cross(const V& a, const V& b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
    double len(const V& a) { return std::sqrt(dot(a, a)); }
    V unit(const V& a) { const double l = len(a); return l > 1e-12 ? a * (1.0 / l) : v3(0, 0, 0); }

    // 3x3 rotations, row-major
    typedef std::array<double, 9> R3;
    const R3 I3 = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    R3 mul(const R3& a, const R3& b)
    {
        R3 r{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
        return r;
    }
    V rotate(const R3& r, const V& v) { return v3(r[0] * v.x + r[1] * v.y + r[2] * v.z, r[3] * v.x + r[4] * v.y + r[5] * v.z, r[6] * v.x + r[7] * v.y + r[8] * v.z); }
    // the rotation turning unit vector a onto unit vector b (the shortest way)
    R3 turn(const V& a, const V& b)
    {
        const V c = cross(a, b);
        const double d = dot(a, b), s = len(c);
        if (s < 1e-9)
        {
            if (d > 0) return I3;
            // opposite: half a turn about any perpendicular axis
            V p = std::fabs(a.x) < 0.9 ? cross(a, v3(1, 0, 0)) : cross(a, v3(0, 1, 0));
            p = unit(p);
            return { 2 * p.x * p.x - 1, 2 * p.x * p.y, 2 * p.x * p.z, 2 * p.y * p.x, 2 * p.y * p.y - 1, 2 * p.y * p.z, 2 * p.z * p.x, 2 * p.z * p.y, 2 * p.z * p.z - 1 };
        }
        const V k = c * (1.0 / s);
        const double t = 1 - d;
        return { d + k.x * k.x * t, k.x * k.y * t - k.z * s, k.x * k.z * t + k.y * s,
                 k.y * k.x * t + k.z * s, d + k.y * k.y * t, k.y * k.z * t - k.x * s,
                 k.z * k.x * t - k.y * s, k.z * k.y * t + k.x * s, d + k.z * k.z * t };
    }
    V place(const Mat& m) { return v3(m[3], m[7], m[11]); }
    Mat translation(const V& p) { return Mat{ 1, 0, 0, p.x, 0, 1, 0, p.y, 0, 0, 1, p.z, 0, 0, 0, 1 }; }

    std::string attr(const std::string& tag, const std::string& name)
    {
        const std::string key = " " + name + "=\"";
        size_t p = tag.find(key);
        if (p == std::string::npos) return std::string();
        p += key.size();
        const size_t e = tag.find('"', p);
        return e == std::string::npos ? std::string() : tag.substr(p, e - p);
    }

    std::string xmlText(const std::string& s)
    {
        std::string o;
        for (char c : s)
        {
            if (c == '&') o += "&amp;";
            else if (c == '<') o += "&lt;";
            else if (c == '>') o += "&gt;";
            else if (c == '"') o += "&quot;";
            else o += c;
        }
        return o;
    }
    std::string xmlId(const std::string& s)
    {
        std::string o;
        for (char c : s) o += (std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') ? c : '_';
        if (o.empty() || std::isdigit((unsigned char)o[0])) o = "_" + o;
        return o;
    }
    void num(std::string& out, double v)
    {
        char b[32];
        if (std::fabs(v) < 1e-9) v = 0.0;
        std::snprintf(b, sizeof(b), "%.7g ", v);
        out += b;
    }

    // SL's limbs, turned to their SL direction for the T-pose: bone -> the bone it points at
    const char* const LIMBS[][2] = {
        { "mShoulderLeft", "mElbowLeft" }, { "mElbowLeft", "mWristLeft" }, { "mWristLeft", "mHandMiddle1Left" },
        { "mShoulderRight", "mElbowRight" }, { "mElbowRight", "mWristRight" }, { "mWristRight", "mHandMiddle1Right" },
        { "mHipLeft", "mKneeLeft" }, { "mKneeLeft", "mAnkleLeft" },
        { "mHipRight", "mKneeRight" }, { "mKneeRight", "mAnkleRight" } };
}

namespace MGRig
{
    std::string plainName(const std::string& n)
    {
        const size_t c = n.find_last_of(':');
        return c == std::string::npos ? n : n.substr(c + 1);
    }

    bool loadMaps(const std::string& path, std::vector<BoneMap>& out)
    {
        out.clear();
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::vector<std::string> f;
            size_t s = 0;
            for (;;)
            {
                const size_t t = line.find('\t', s);
                f.push_back(line.substr(s, t == std::string::npos ? std::string::npos : t - s));
                if (t == std::string::npos) break;
                s = t + 1;
            }
            if (f[0] == "#map" && f.size() >= 2) { out.push_back(BoneMap()); out.back().name = f[1]; }
            else if (out.empty()) continue;
            else if (f[0] == "r" && f.size() >= 3) out.back().rename[plainName(f[1])] = f[2];
            else if (f[0] == "k" && f.size() >= 3) out.back().reskin[plainName(f[1])].push_back(plainName(f[2]));
        }
        return !out.empty();
    }

    Detection detect(const Model& m, const std::vector<BoneMap>& maps)
    {
        Detection best;
        std::vector<std::string> weighted;
        for (const auto& j : m.joints) if (j.weighted) weighted.push_back(plainName(j.name));
        for (size_t i = 0; i < maps.size(); ++i)
        {
            std::set<std::string> known;
            for (const auto& kv : maps[i].rename) known.insert(kv.first);
            for (const auto& kv : maps[i].reskin) for (const auto& e : kv.second) known.insert(e);
            int matched = 0, renamed = 0;
            for (const auto& n : weighted)
            {
                if (known.count(n)) ++matched;
                if (maps[i].rename.count(n)) ++renamed;
            }
            const double score = weighted.empty() ? 0.0 : (double)matched / (double)weighted.size();
            // a tie goes to the map that renames the most bones itself (finer: face, fingers)
            if (matched >= 8 && (score > best.score + 1e-9 || (std::fabs(score - best.score) < 1e-9 && renamed > best.renamed)))
            {
                best.map = (int)i;
                best.name = maps[i].name;
                best.matched = matched;
                best.weighted = (int)weighted.size();
                best.score = score;
                best.renamed = renamed;
            }
        }
        if (best.map < 0) best.weighted = (int)weighted.size();
        return best;
    }

    bool loadSlSkeleton(const std::string& path, std::vector<SlBone>& out)
    {
        out.clear();
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        const std::string t((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<int> stack;
        size_t p = 0;
        while ((p = t.find('<', p)) != std::string::npos)
        {
            const size_t e = t.find('>', p);
            if (e == std::string::npos) break;
            const std::string tag = t.substr(p, e - p + 1);
            const bool closing = tag.size() > 1 && tag[1] == '/';
            const bool selfClosed = tag.size() > 2 && tag[tag.size() - 2] == '/';
            const bool bone = !closing && tag.compare(0, 5, "<bone") == 0 && tag.size() > 5 && std::isspace((unsigned char)tag[5]);
            const bool volume = !closing && tag.compare(0, 17, "<collision_volume") == 0;
            if (bone || volume)
            {
                SlBone b;
                b.name = attr(tag, "name");
                b.volume = volume;
                b.parent = stack.empty() ? -1 : stack.back();
                std::istringstream ps(attr(tag, "pos"));
                ps >> b.local[0] >> b.local[1] >> b.local[2];
                for (int k = 0; k < 3; ++k) b.world[k] = b.local[k] + (b.parent >= 0 ? out[b.parent].world[k] : 0.0);
                out.push_back(b);
                if (bone && !selfClosed) stack.push_back((int)out.size() - 1);
            }
            else if (closing && tag.compare(0, 6, "</bone") == 0)
            {
                if (!stack.empty()) stack.pop_back();
            }
            p = e + 1;
        }
        return !out.empty();
    }

    bool convert(const Model& in, const BoneMap& map, const std::vector<SlBone>& sl, const Options& o,
                 Model& out, Report& r)
    {
        out = Model();
        r = Report();
        out.format = "sl";
        r.map = map.name;
        r.bonesIn = (int)in.joints.size();
        r.meshesIn = (int)in.meshes.size();
        if (in.joints.empty() || in.meshes.empty()) { out.error = "empty"; return false; }
        std::map<std::string, int> slIndex;
        for (size_t i = 0; i < sl.size(); ++i) slIndex[sl[i].name] = (int)i;
        if (!slIndex.count("mPelvis")) { out.error = "skeleton"; return false; }

        // 1. each source bone's SL bone: its own (rename), the one it is merged into
        // (reskin), else its nearest mapped parent's
        std::map<std::string, std::string> owner;   // extra bone -> mapped bone
        for (const auto& kv : map.reskin) for (const auto& e : kv.second) owner[e] = kv.first;
        const size_t n = in.joints.size();
        std::vector<int> target(n, -1);
        std::vector<int> direct(n, 0);   // 1 = renamed, 2 = merged
        for (size_t j = 0; j < n; ++j)
        {
            const std::string p = plainName(in.joints[j].name);
            auto rn = map.rename.find(p);
            if (rn != map.rename.end() && slIndex.count(rn->second)) { target[j] = slIndex[rn->second]; direct[j] = 1; continue; }
            auto ow = owner.find(p);
            if (ow != owner.end())
            {
                auto r2 = map.rename.find(ow->second);
                if (r2 != map.rename.end() && slIndex.count(r2->second)) { target[j] = slIndex[r2->second]; direct[j] = 2; }
            }
        }
        for (size_t j = 0; j < n; ++j)
        {
            if (target[j] >= 0) { if (direct[j] == 1) ++r.mapped; else ++r.merged; continue; }
            if (in.joints[j].weighted)
            {
                // a parent that is itself unmapped gets resolved in its own turn: walk to a mapped one
                int q = in.joints[j].parent;
                while (q >= 0 && direct[q] == 0) q = in.joints[q].parent;
                target[j] = q >= 0 ? target[q] : slIndex["mPelvis"];
                r.fallback.push_back(in.joints[j].name);
            }
        }
        // the source bone standing for each SL bone (the highest one renamed to it)
        std::map<int, int> sourceOf;
        auto depth = [&](int j) { int d = 0; while (j >= 0 && in.joints[j].parent >= 0) { j = in.joints[j].parent; ++d; } return d; };
        for (size_t j = 0; j < n; ++j)
            if (direct[j] == 1)
            {
                auto it = sourceOf.find(target[j]);
                if (it == sourceOf.end() || depth((int)j) < depth(it->second)) sourceOf[target[j]] = (int)j;
            }
        if (!sourceOf.count(slIndex["mPelvis"])) r.warnings.push_back("no_pelvis");

        // 2. T-pose: parents first; each limb turned onto SL's direction, children follow
        std::vector<V> p0(n), p1(n);
        std::vector<R3> rot(n, I3);
        for (size_t j = 0; j < n; ++j) p0[j] = place(in.joints[j].bind);
        std::vector<int> order(n);
        for (size_t j = 0; j < n; ++j) order[j] = (int)j;
        std::vector<int> dep(n);
        for (size_t j = 0; j < n; ++j) dep[j] = depth((int)j);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return dep[a] < dep[b]; });
        // source bone -> (source bone it points at, SL direction); legs only lose their sideways
        // spread (SL's own thighs lean in from its wide hips: copied, they gave knock knees)
        struct Aim { int at; V dir; bool straighten; };
        std::map<int, Aim> aim;
        if (o.tpose)
            for (const auto& L : LIMBS)
            {
                auto a = slIndex.find(L[0]), b = slIndex.find(L[1]);
                if (a == slIndex.end() || b == slIndex.end()) continue;
                auto sa = sourceOf.find(a->second), sb = sourceOf.find(b->second);
                if (sa == sourceOf.end() || sb == sourceOf.end()) continue;
                const SlBone &A = sl[a->second], &B = sl[b->second];
                const bool leg = std::string(L[0]).compare(0, 4, "mHip") == 0 || std::string(L[0]).compare(0, 5, "mKnee") == 0;
                aim[sa->second] = { sb->second, unit(v3(B.world[0] - A.world[0], B.world[1] - A.world[1], B.world[2] - A.world[2])), leg };
            }
        for (int j : order)
        {
            const int par = in.joints[j].parent;
            if (par < 0) { p1[j] = p0[j]; rot[j] = I3; }
            else { rot[j] = rot[par]; p1[j] = p1[par] + rotate(rot[par], p0[j] - p0[par]); }
            auto a = aim.find(j);
            if (a == aim.end()) continue;
            const V cur = unit(rotate(rot[j], p0[a->second.at] - p0[j]));
            if (len(cur) < 0.5) continue;
            const V want = a->second.straighten ? unit(v3(cur.x, 0.0, cur.z)) : a->second.dir;
            if (len(want) < 0.5) continue;
            const double deg = std::acos(std::max(-1.0, std::min(1.0, dot(cur, want)))) * 180.0 / 3.14159265358979;
            if (deg < 0.5) continue;
            rot[j] = mul(turn(cur, want), rot[j]);
            ++r.tposed;
            r.maxTurnDeg = std::max(r.maxTurnDeg, deg);
        }

        // 3. meshes mostly weighted to bones the map does not know (another character in the
        // file, props on their own rig) are left out rather than glued to the pelvis
        std::vector<char> keep(in.meshes.size(), 1);
        for (size_t mi = 0; mi < in.meshes.size(); ++mi)
        {
            double known = 0.0, all = 0.0;
            for (const auto& vw : in.meshes[mi].weights)
                for (const auto& jw : vw)
                {
                    if (jw.first < 0 || jw.first >= (int)n || jw.second <= 0.0) continue;
                    all += jw.second;
                    if (direct[jw.first]) known += jw.second;
                }
            if (all > 0.0 && known / all < 0.5) { keep[mi] = 0; r.leftOut.push_back(in.meshes[mi].name); }
        }
        if (!r.leftOut.empty()) r.warnings.push_back("left_out");
        // the fallback list: bones of the meshes kept only
        {
            std::set<int> inKept;
            for (size_t mi = 0; mi < in.meshes.size(); ++mi)
                if (keep[mi]) for (const auto& vw : in.meshes[mi].weights) for (const auto& jw : vw) if (jw.first >= 0 && jw.first < (int)n) inKept.insert(jw.first);
            std::vector<std::string> fb;
            for (size_t j = 0; j < n; ++j) if (!direct[j] && in.joints[j].weighted && inKept.count((int)j)) fb.push_back(in.joints[j].name);
            r.fallback = fb;
        }

        // 4. the SL bones in use (and their parents up to the pelvis), in SL's order
        std::vector<char> used(sl.size(), 0);
        for (size_t mi = 0; mi < in.meshes.size(); ++mi)
            if (keep[mi]) for (const auto& vw : in.meshes[mi].weights)
                for (const auto& jw : vw)
                    if (jw.first >= 0 && jw.first < (int)n && target[jw.first] >= 0 && jw.second > 0.0) used[target[jw.first]] = 1;
        for (size_t i = 0; i < sl.size(); ++i)
            if (used[i]) for (int p = sl[i].parent; p >= 0; p = sl[p].parent) used[p] = 1;
        std::vector<int> outIndex(sl.size(), -1);
        std::vector<V> world(sl.size());
        for (size_t i = 0; i < sl.size(); ++i)
        {
            if (!used[i]) continue;
            const SlBone& b = sl[i];
            auto s = sourceOf.find((int)i);
            if (!o.jointPositions) world[i] = v3(b.world[0], b.world[1], b.world[2]);
            else if (s != sourceOf.end()) world[i] = p1[s->second];
            else if (b.parent >= 0) world[i] = world[b.parent] + v3(b.local[0], b.local[1], b.local[2]);
            else world[i] = v3(b.world[0], b.world[1], b.world[2]);
            Joint j;
            j.name = b.name;
            j.parent = b.parent >= 0 ? outIndex[b.parent] : -1;
            j.bind = translation(world[i]);
            outIndex[i] = (int)out.joints.size();
            out.joints.push_back(j);
        }
        r.bonesOut = (int)out.joints.size();
        if (outIndex[slIndex["mPelvis"]] >= 0) r.pelvisZ = world[slIndex["mPelvis"]].z;

        // 5. the meshes: posed, weights on SL bones (4 at most, adding up to 1), split by 8 materials
        double zmin = 1e30, zmax = -1e30;
        for (size_t mi = 0; mi < in.meshes.size(); ++mi)
        {
            if (!keep[mi]) continue;
            const Mesh& m = in.meshes[mi];
            Mesh posed = m;
            for (size_t v = 0; v < m.vertexCount(); ++v)
            {
                const V p = v3(m.pos[v * 3], m.pos[v * 3 + 1], m.pos[v * 3 + 2]);
                const V nn = v3(m.nrm[v * 3], m.nrm[v * 3 + 1], m.nrm[v * 3 + 2]);
                V sp = v3(0, 0, 0), sn = v3(0, 0, 0);
                double ws = 0.0;
                std::map<int, double> merged;
                for (const auto& jw : m.weights[v])
                {
                    if (jw.first < 0 || jw.first >= (int)n || jw.second <= 0.0) continue;
                    const int j = jw.first;
                    sp = sp + (rotate(rot[j], p - p0[j]) + p1[j]) * jw.second;
                    sn = sn + rotate(rot[j], nn) * jw.second;
                    ws += jw.second;
                    if (target[j] >= 0 && outIndex[target[j]] >= 0) merged[outIndex[target[j]]] += jw.second;
                }
                if (ws > 0.0)
                {
                    sp = sp * (1.0 / ws);
                    sn = unit(sn);
                    posed.pos[v * 3] = sp.x; posed.pos[v * 3 + 1] = sp.y; posed.pos[v * 3 + 2] = sp.z;
                    posed.nrm[v * 3] = sn.x; posed.nrm[v * 3 + 1] = sn.y; posed.nrm[v * 3 + 2] = sn.z;
                }
                std::vector<std::pair<int, double>> w(merged.begin(), merged.end());
                std::sort(w.begin(), w.end(), [](const std::pair<int, double>& a, const std::pair<int, double>& b) { return a.second > b.second; });
                if (w.size() > 4) w.resize(4);
                double s = 0.0;
                for (const auto& x : w) s += x.second;
                if (s > 0.0) for (auto& x : w) x.second /= s;
                else w = { { outIndex[slIndex["mPelvis"]] >= 0 ? outIndex[slIndex["mPelvis"]] : 0, 1.0 } };
                posed.weights[v] = w;
                zmin = std::min(zmin, posed.pos[v * 3 + 2]);
                zmax = std::max(zmax, posed.pos[v * 3 + 2]);
            }
            // at most 8 materials per mesh: the rest go to more meshes
            const int nmat = (int)posed.materials.size();
            const int parts = std::max(1, (nmat + o.maxMaterials - 1) / o.maxMaterials);
            for (int part = 0; part < parts; ++part)
            {
                Mesh piece;
                piece.name = parts > 1 ? posed.name + "_" + std::to_string(part + 1) : posed.name;
                const int m0 = part * o.maxMaterials, m1 = std::min(nmat, m0 + o.maxMaterials);
                for (int k = m0; k < m1; ++k) piece.materials.push_back(posed.materials[k]);
                std::vector<int> remap(posed.vertexCount(), -1);
                for (size_t t = 0; t < posed.tris.size(); ++t)
                {
                    const int mt = posed.triMat[t];
                    if (parts > 1 && (mt < m0 || mt >= m1)) continue;
                    std::array<int, 3> tri;
                    for (int c = 0; c < 3; ++c)
                    {
                        const int v = posed.tris[t][c];
                        if (remap[v] < 0)
                        {
                            remap[v] = (int)piece.vertexCount();
                            piece.pos.insert(piece.pos.end(), { posed.pos[v * 3], posed.pos[v * 3 + 1], posed.pos[v * 3 + 2] });
                            piece.nrm.insert(piece.nrm.end(), { posed.nrm[v * 3], posed.nrm[v * 3 + 1], posed.nrm[v * 3 + 2] });
                            if (!posed.uv.empty()) piece.uv.insert(piece.uv.end(), { posed.uv[v * 2], posed.uv[v * 2 + 1] });
                            piece.weights.push_back(posed.weights[v]);
                        }
                        tri[c] = remap[v];
                    }
                    piece.tris.push_back(tri);
                    piece.triMat.push_back(parts > 1 ? mt - m0 : mt);
                }
                if (piece.tris.empty()) continue;
                std::set<int> bones;
                for (const auto& vw : piece.weights) for (const auto& jw : vw) bones.insert(jw.first);
                r.maxBonesPerMesh = std::max(r.maxBonesPerMesh, (int)bones.size());
                r.vertices += piece.vertexCount();
                r.triangles += piece.tris.size();
                out.meshes.push_back(piece);
            }
        }
        r.meshesOut = (int)out.meshes.size();
        r.height = zmax > zmin ? zmax - zmin : 0.0;
        if (r.maxBonesPerMesh > 110) r.warnings.push_back("bones");
        if (r.height < 0.3 || r.height > 4.0) r.warnings.push_back("scale");
        // the face looks forward (+X) and the left side is +Y
        auto el = slIndex.find("mEyeLeft"), er = slIndex.find("mEyeRight");
        if (el != slIndex.end() && er != slIndex.end() && sourceOf.count(el->second) && sourceOf.count(er->second))
            if (p1[sourceOf[el->second]].y < p1[sourceOf[er->second]].y) r.warnings.push_back("mirrored");
        auto hd = slIndex.find("mHead");
        if (hd != slIndex.end() && el != slIndex.end() && sourceOf.count(hd->second) && sourceOf.count(el->second))
            if (p1[sourceOf[el->second]].x < p1[sourceOf[hd->second]].x - 0.01) r.warnings.push_back("backwards");
        if (!r.fallback.empty()) r.warnings.push_back("fallback");
        return true;
    }

    bool writeDae(const Model& m, const std::string& path, std::string& error)
    {
        std::string o;
        o.reserve(1 << 24);
        o += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
             "<COLLADA xmlns=\"http://www.collada.org/2005/11/COLLADASchema\" version=\"1.4.1\">\n"
             "  <asset><contributor><authoring_tool>MasterGate Viewer Creator Lab (rig converter)</authoring_tool></contributor>"
             "<unit name=\"meter\" meter=\"1\"/><up_axis>Z_UP</up_axis></asset>\n";
        // materials (plain, by name: the uploader keeps the names as faces)
        std::vector<std::string> mats;
        for (const auto& mesh : m.meshes) for (const auto& n : mesh.materials) if (std::find(mats.begin(), mats.end(), n) == mats.end()) mats.push_back(n);
        o += "  <library_effects>\n";
        for (const auto& n : mats)
            o += "    <effect id=\"" + xmlId(n) + "-effect\"><profile_COMMON><technique sid=\"common\"><lambert><diffuse><color sid=\"diffuse\">0.8 0.8 0.8 1</color></diffuse></lambert></technique></profile_COMMON></effect>\n";
        o += "  </library_effects>\n  <library_materials>\n";
        for (const auto& n : mats)
            o += "    <material id=\"" + xmlId(n) + "-material\" name=\"" + xmlText(n) + "\"><instance_effect url=\"#" + xmlId(n) + "-effect\"/></material>\n";
        o += "  </library_materials>\n  <library_geometries>\n";
        std::vector<std::string> gid;
        // one shared point per position (a UV seam is two vertices at one place): written once,
        // like Blender, so tools that move points (Make it fit, Touch up) keep seams closed
        std::vector<std::vector<int>> posOfAll(m.meshes.size()), firstOfAll(m.meshes.size());
        for (size_t mi = 0; mi < m.meshes.size(); ++mi)
        {
            const Mesh& mesh = m.meshes[mi];
            const std::string id = xmlId(mesh.name) + "_" + std::to_string(mi);
            gid.push_back(id);
            const size_t nv = mesh.vertexCount();
            std::vector<int>& posOf = posOfAll[mi];
            std::vector<int>& firstOf = firstOfAll[mi];
            posOf.assign(nv, -1);
            {
                std::map<std::array<long long, 3>, int> at;
                for (size_t v = 0; v < nv; ++v)
                {
                    const std::array<long long, 3> k = { std::llround(mesh.pos[v * 3] * 1e6), std::llround(mesh.pos[v * 3 + 1] * 1e6), std::llround(mesh.pos[v * 3 + 2] * 1e6) };
                    auto it = at.find(k);
                    if (it == at.end()) { it = at.emplace(k, (int)firstOf.size()).first; firstOf.push_back((int)v); }
                    posOf[v] = it->second;
                }
            }
            const size_t np = firstOf.size();
            o += "    <geometry id=\"" + id + "-mesh\" name=\"" + xmlText(mesh.name) + "\"><mesh>\n";
            o += "      <source id=\"" + id + "-positions\"><float_array id=\"" + id + "-positions-array\" count=\"" + std::to_string(np * 3) + "\">";
            for (int v : firstOf) { num(o, mesh.pos[v * 3]); num(o, mesh.pos[v * 3 + 1]); num(o, mesh.pos[v * 3 + 2]); }
            o += "</float_array><technique_common><accessor source=\"#" + id + "-positions-array\" count=\"" + std::to_string(np) + "\" stride=\"3\"><param name=\"X\" type=\"float\"/><param name=\"Y\" type=\"float\"/><param name=\"Z\" type=\"float\"/></accessor></technique_common></source>\n";
            o += "      <source id=\"" + id + "-normals\"><float_array id=\"" + id + "-normals-array\" count=\"" + std::to_string(nv * 3) + "\">";
            for (double v : mesh.nrm) num(o, v);
            o += "</float_array><technique_common><accessor source=\"#" + id + "-normals-array\" count=\"" + std::to_string(nv) + "\" stride=\"3\"><param name=\"X\" type=\"float\"/><param name=\"Y\" type=\"float\"/><param name=\"Z\" type=\"float\"/></accessor></technique_common></source>\n";
            const bool uv = mesh.uv.size() == nv * 2;
            if (uv)
            {
                o += "      <source id=\"" + id + "-map-0\"><float_array id=\"" + id + "-map-0-array\" count=\"" + std::to_string(nv * 2) + "\">";
                for (double v : mesh.uv) num(o, v);
                o += "</float_array><technique_common><accessor source=\"#" + id + "-map-0-array\" count=\"" + std::to_string(nv) + "\" stride=\"2\"><param name=\"S\" type=\"float\"/><param name=\"T\" type=\"float\"/></accessor></technique_common></source>\n";
            }
            o += "      <vertices id=\"" + id + "-vertices\"><input semantic=\"POSITION\" source=\"#" + id + "-positions\"/></vertices>\n";
            for (size_t k = 0; k < mesh.materials.size(); ++k)
            {
                size_t count = 0;
                for (int tm : mesh.triMat) if (tm == (int)k) ++count;
                if (!count) continue;
                o += "      <triangles material=\"" + xmlId(mesh.materials[k]) + "-material\" count=\"" + std::to_string(count) + "\">"
                     "<input semantic=\"VERTEX\" source=\"#" + id + "-vertices\" offset=\"0\"/><input semantic=\"NORMAL\" source=\"#" + id + "-normals\" offset=\"1\"/>";
                if (uv) o += "<input semantic=\"TEXCOORD\" source=\"#" + id + "-map-0\" offset=\"1\" set=\"0\"/>";
                o += "<p>";
                // each corner: its shared point, then its own normal and uv
                for (size_t t = 0; t < mesh.tris.size(); ++t)
                    if (mesh.triMat[t] == (int)k)
                        for (int c = 0; c < 3; ++c) o += std::to_string(posOf[mesh.tris[t][c]]) + " " + std::to_string(mesh.tris[t][c]) + " ";
                o += "</p></triangles>\n";
            }
            o += "    </mesh></geometry>\n";
        }
        o += "  </library_geometries>\n  <library_controllers>\n";
        for (size_t mi = 0; mi < m.meshes.size(); ++mi)
        {
            const Mesh& mesh = m.meshes[mi];
            const std::string& id = gid[mi];
            // this mesh's bones only, in skeleton order; a mesh with its own skin (clothes rigged
            // by MGRigCloth) keeps its bones and inverse binds as they are
            std::map<int, int> local;
            std::vector<std::string> names;
            std::vector<Mat> binds;
            if (!mesh.skinJoints.empty())
            {
                for (size_t i = 0; i < mesh.skinJoints.size(); ++i)
                {
                    local[(int)i] = (int)i;
                    names.push_back(mesh.skinJoints[i]);
                    binds.push_back(i < mesh.skinInvBind.size() ? mesh.skinInvBind[i] : Mat{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 });
                }
            }
            else
            {
                std::set<int> usedSet;
                for (const auto& vw : mesh.weights) for (const auto& jw : vw) usedSet.insert(jw.first);
                for (int b : usedSet)
                {
                    local[b] = (int)names.size();
                    names.push_back(m.joints[b].name);
                    binds.push_back(translation(place(m.joints[b].bind) * -1.0));
                }
            }
            o += "    <controller id=\"" + id + "-skin\" name=\"" + xmlText(mesh.name) + "\"><skin source=\"#" + id + "-mesh\">"
                 "<bind_shape_matrix>1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</bind_shape_matrix>\n";
            o += "      <source id=\"" + id + "-joints\"><Name_array id=\"" + id + "-joints-array\" count=\"" + std::to_string(names.size()) + "\">";
            for (const auto& nm : names) o += nm + " ";
            o += "</Name_array><technique_common><accessor source=\"#" + id + "-joints-array\" count=\"" + std::to_string(names.size()) + "\" stride=\"1\"><param name=\"JOINT\" type=\"name\"/></accessor></technique_common></source>\n";
            o += "      <source id=\"" + id + "-bind_poses\"><float_array id=\"" + id + "-bind_poses-array\" count=\"" + std::to_string(binds.size() * 16) + "\">";
            for (const auto& ib : binds) for (double v : ib) num(o, v);
            o += "</float_array><technique_common><accessor source=\"#" + id + "-bind_poses-array\" count=\"" + std::to_string(binds.size()) + "\" stride=\"16\"><param name=\"TRANSFORM\" type=\"float4x4\"/></accessor></technique_common></source>\n";
            // one set of weights per shared point (the seam's vertices were given the same ones)
            const std::vector<int>& firstOf = firstOfAll[mi];
            size_t nw = 0;
            for (int v : firstOf) nw += mesh.weights[v].size();
            o += "      <source id=\"" + id + "-weights\"><float_array id=\"" + id + "-weights-array\" count=\"" + std::to_string(nw) + "\">";
            for (int v : firstOf) for (const auto& jw : mesh.weights[v]) num(o, jw.second);
            o += "</float_array><technique_common><accessor source=\"#" + id + "-weights-array\" count=\"" + std::to_string(nw) + "\" stride=\"1\"><param name=\"WEIGHT\" type=\"float\"/></accessor></technique_common></source>\n";
            o += "      <joints><input semantic=\"JOINT\" source=\"#" + id + "-joints\"/><input semantic=\"INV_BIND_MATRIX\" source=\"#" + id + "-bind_poses\"/></joints>\n";
            o += "      <vertex_weights count=\"" + std::to_string(firstOf.size()) + "\"><input semantic=\"JOINT\" source=\"#" + id + "-joints\" offset=\"0\"/><input semantic=\"WEIGHT\" source=\"#" + id + "-weights\" offset=\"1\"/><vcount>";
            for (int v : firstOf) o += std::to_string(mesh.weights[v].size()) + " ";
            o += "</vcount><v>";
            size_t wi = 0;
            for (int v : firstOf)
                for (const auto& jw : mesh.weights[v]) o += std::to_string(local[jw.first]) + " " + std::to_string(wi++) + " ";
            o += "</v></vertex_weights>\n    </skin></controller>\n";
        }
        o += "  </library_controllers>\n  <library_visual_scenes><visual_scene id=\"Scene\" name=\"Scene\">\n";
        // the SL skeleton: joint nodes, each placed from its parent
        std::vector<std::vector<int>> kids(m.joints.size());
        std::vector<int> roots;
        for (size_t j = 0; j < m.joints.size(); ++j)
        {
            if (m.joints[j].parent >= 0) kids[m.joints[j].parent].push_back((int)j);
            else roots.push_back((int)j);
        }
        std::function<void(int, int)> node = [&](int j, int indent)
        {
            const std::string pad(indent, ' ');
            V p = place(m.joints[j].bind);
            if (m.joints[j].parent >= 0) p = p - place(m.joints[m.joints[j].parent].bind);
            o += pad + "<node id=\"" + m.joints[j].name + "\" name=\"" + m.joints[j].name + "\" sid=\"" + m.joints[j].name + "\" type=\"JOINT\"><matrix sid=\"transform\">";
            const Mat t = translation(p);
            for (double v : t) num(o, v);
            o += "</matrix>\n";
            for (int k : kids[j]) node(k, indent + 2);
            o += pad + "</node>\n";
        };
        o += "    <node id=\"Armature\" name=\"Armature\" type=\"NODE\"><matrix sid=\"transform\">1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</matrix>\n";
        for (int j : roots) node(j, 6);
        o += "    </node>\n";
        const std::string root = roots.empty() ? std::string("mPelvis") : m.joints[roots[0]].name;
        for (size_t mi = 0; mi < m.meshes.size(); ++mi)
        {
            const Mesh& mesh = m.meshes[mi];
            o += "    <node id=\"" + gid[mi] + "\" name=\"" + xmlText(mesh.name) + "\" type=\"NODE\"><matrix sid=\"transform\">1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</matrix>"
                 "<instance_controller url=\"#" + gid[mi] + "-skin\"><skeleton>#" + root + "</skeleton><bind_material><technique_common>";
            for (const auto& n : mesh.materials)
                o += "<instance_material symbol=\"" + xmlId(n) + "-material\" target=\"#" + xmlId(n) + "-material\"><bind_vertex_input semantic=\"UVMap\" input_semantic=\"TEXCOORD\" input_set=\"0\"/></instance_material>";
            o += "</technique_common></bind_material></instance_controller></node>\n";
        }
        o += "  </visual_scene></library_visual_scenes>\n  <scene><instance_visual_scene url=\"#Scene\"/></scene>\n</COLLADA>\n";
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) { error = "write"; return false; }
        f.write(o.data(), (std::streamsize)o.size());
        if (!f) { error = "write"; return false; }
        return true;
    }

    std::string outPathFor(const std::string& src)
    {
        const size_t dot = src.find_last_of('.');
        const size_t slash = src.find_last_of("/\\");
        const std::string base = (dot != std::string::npos && (slash == std::string::npos || dot > slash)) ? src.substr(0, dot) : src;
        return base + "_sl.dae";
    }

    bool readModel(const std::string& path, Model& out, bool keepStatic)
    {
        std::string ext;
        const size_t dot = path.find_last_of('.');
        if (dot != std::string::npos) ext = path.substr(dot + 1);
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == "fbx" || ext == "obj") return readFbx(path, out, keepStatic);
        if (ext == "gltf" || ext == "glb" || ext == "vrm") return readGltf(path, out, keepStatic);
        if (ext == "dae") return readDae(path, out, keepStatic);
        out = Model();
        out.error = "format";
        return false;
    }
}
