/**
 * @file mgfitcore.cpp
 * @brief <MG> Creator Lab "Make it fit": the geometry (see mgfitcore.h). Standard C++ only.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (C) 2026, MasterGate
 * $/LicenseInfo$
 */

#if !defined(MG_FITCORE_STANDALONE) && !defined(MG_CREATORDAE_STANDALONE)
#include "llviewerprecompiledheaders.h"
#endif

#include "mgfitcore.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

namespace MGFitCore
{
namespace
{
    inline V3 operator+(const V3& a, const V3& b) { return V3(a.x + b.x, a.y + b.y, a.z + b.z); }
    inline V3 operator-(const V3& a, const V3& b) { return V3(a.x - b.x, a.y - b.y, a.z - b.z); }
    inline V3 operator*(const V3& a, float s) { return V3(a.x * s, a.y * s, a.z * s); }
    inline float dot3(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline V3 cross3(const V3& a, const V3& b) { return V3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
    inline float len2(const V3& a) { return dot3(a, a); }
    inline V3 unit(const V3& a)
    {
        const float l = std::sqrt(len2(a));
        return l > 1e-12f ? a * (1.f / l) : V3();
    }

    struct Cells
    {
        float cell = 0.02f;
        std::unordered_map<uint64_t, std::vector<int>> map;
        int c(float v) const { return (int)std::floor(v / cell); }
        static uint64_t key(int x, int y, int z)
        {
            const uint64_t o = 1u << 20;
            return ((uint64_t)(x + o) << 42) | ((uint64_t)(y + o) << 21) | (uint64_t)(z + o);
        }
        const std::vector<int>* at(int x, int y, int z) const
        {
            auto it = map.find(key(x, y, z));
            return it == map.end() ? nullptr : &it->second;
        }
        // a triangle goes in every cell its box touches
        void addBox(int id, const V3& lo, const V3& hi)
        {
            for (int x = c(lo.x); x <= c(hi.x); ++x)
                for (int y = c(lo.y); y <= c(hi.y); ++y)
                    for (int z = c(lo.z); z <= c(hi.z); ++z) map[key(x, y, z)].push_back(id);
        }
    };

    // the point of triangle abc nearest p, with its barycentric weights (Ericson, RTCD 5.1.5)
    V3 closestOnTri(const V3& p, const V3& a, const V3& b, const V3& c, float& u, float& v, float& w)
    {
        const V3 ab = b - a, ac = c - a, ap = p - a;
        const float d1 = dot3(ab, ap), d2 = dot3(ac, ap);
        if (d1 <= 0.f && d2 <= 0.f) { u = 1.f; v = 0.f; w = 0.f; return a; }
        const V3 bp = p - b;
        const float d3 = dot3(ab, bp), d4 = dot3(ac, bp);
        if (d3 >= 0.f && d4 <= d3) { u = 0.f; v = 1.f; w = 0.f; return b; }
        const float vc = d1 * d4 - d3 * d2;
        if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f)
        {
            const float t = d1 / (d1 - d3);
            u = 1.f - t; v = t; w = 0.f;
            return a + ab * t;
        }
        const V3 cp = p - c;
        const float d5 = dot3(ab, cp), d6 = dot3(ac, cp);
        if (d6 >= 0.f && d5 <= d6) { u = 0.f; v = 0.f; w = 1.f; return c; }
        const float vb = d5 * d2 - d1 * d6;
        if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f)
        {
            const float t = d2 / (d2 - d6);
            u = 1.f - t; v = 0.f; w = t;
            return a + ac * t;
        }
        const float va = d3 * d6 - d5 * d4;
        if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f)
        {
            const float t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
            u = 0.f; v = 1.f - t; w = t;
            return b + (c - b) * t;
        }
        const float den = 1.f / (va + vb + vc);
        v = vb * den; w = vc * den; u = 1.f - v - w;
        return a + ab * v + ac * w;
    }

    // two-sided ray o + t d against abc: t and the weights of b and c
    bool rayTri(const V3& o, const V3& d, const V3& a, const V3& b, const V3& c, float& t, float& bu, float& bv)
    {
        const V3 e1 = b - a, e2 = c - a;
        const V3 p = cross3(d, e2);
        const float det = dot3(e1, p);
        if (std::fabs(det) < 1e-14f) return false;
        const float inv = 1.f / det;
        const V3 s = o - a;
        bu = dot3(s, p) * inv;
        if (bu < 0.f || bu > 1.f) return false;
        const V3 q = cross3(s, e1);
        bv = dot3(d, q) * inv;
        if (bv < 0.f || bu + bv > 1.f) return false;
        t = dot3(e2, q) * inv;
        return true;
    }

    // keep the longer of two moves
    inline bool keepLonger(V3& cur, const V3& cand)
    {
        if (len2(cand) <= len2(cur)) return false;
        cur = cand;
        return true;
    }
}

void solve(const Body& body, const Garment& garment, const Params& prm, Result& out)
{
    const size_t gn = garment.pos.size();
    out.push.assign(gn, V3());
    out.need.assign(gn, V3());
    out.sdist.assign(gn, 1e9f);
    out.src.assign(gn, 0);
    out.inside = out.pokes = out.thinParts = out.overDeep = 0;
    out.deepest = 0.f;
    if (gn == 0 || body.pos.empty() || body.tris.empty()) return;
    const float margin = std::max(-0.005f, prm.margin);   // (below 0: a check that only counts real clipping)
    const float maxPush = std::max(0.001f, prm.maxPush);
    const float shell = std::max(0.f, prm.shell);
    const float radius = std::max(0.001f, prm.radius);
    const float plateau = std::min(std::max(0.f, prm.plateau), radius);
    const float reach = maxPush + margin + 0.002f;           // how far apart garment and skin are compared
    auto isFixed = [&](size_t v) { return v < garment.fixed.size() && garment.fixed[v]; };

    // 1. the body: points welded by place (a skin's faces split its points at UV seams and
    //    material borders: unwelded, every seam would look like an open edge with a half normal)
    const size_t bn = body.pos.size();
    std::vector<int> rep(bn);
    {
        std::unordered_map<uint64_t, int> at;
        at.reserve(bn * 2);
        const float q = 0.0001f;   // 0.1 mm
        for (size_t i = 0; i < bn; ++i)
        {
            const V3& p = body.pos[i];
            const uint64_t k = ((uint64_t)(std::lround(p.x / q) & 0x1FFFFF) << 42) | ((uint64_t)(std::lround(p.y / q) & 0x1FFFFF) << 21)
                             | (uint64_t)(std::lround(p.z / q) & 0x1FFFFF);
            auto it = at.find(k);
            if (it == at.end()) { at.emplace(k, (int)i); rep[i] = (int)i; }
            else rep[i] = it->second;
        }
    }
    // smooth outward normals (area weighted) on the welded points, then shared by the copies
    std::vector<V3> nrm(bn, V3());
    for (const auto& t : body.tris)
    {
        if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= (int)bn || t[1] >= (int)bn || t[2] >= (int)bn) continue;
        const V3 fn = cross3(body.pos[t[1]] - body.pos[t[0]], body.pos[t[2]] - body.pos[t[0]]);
        for (int k = 0; k < 3; ++k) { V3& n = nrm[rep[t[k]]]; n = n + fn; }
    }
    for (size_t i = 0; i < bn; ++i)
    {
        if ((size_t)rep[i] == i) nrm[i] = unit(nrm[i]);
    }
    for (size_t i = 0; i < bn; ++i) nrm[i] = nrm[rep[i]];
    if (body.nrm.size() == bn)
    {
        // the caller's normals win where it gave them (still welded: one direction per place)
        for (size_t i = 0; i < bn; ++i) if (len2(body.nrm[i]) > 0.5f && dot3(body.nrm[i], nrm[i]) > 0.f) nrm[i] = body.nrm[i];
    }
    // open edges of the skin (neck, wrists, alpha-cut parts): a garment point past one is not "inside".
    // <MG> 2026-11-14 (2): an edge only counts as open when it is FREE in space. A mesh body is cut into
    // many sections (alpha cuts) whose border points need not coincide exactly: there another section
    // goes on within 2 mm, so it is skin, not an edge (as open edges, the seams' clipping was skipped
    // and whole sections could be taken for straps).
    std::vector<uint8_t> open(bn, 0);
    std::vector<uint8_t> thin(bn, 0);   // on a thin open piece (left out)
    std::set<int> thinRoot;
    {
        std::unordered_map<uint64_t, int> edges;
        edges.reserve(body.tris.size() * 3);
        for (const auto& t : body.tris)
        {
            for (int k = 0; k < 3; ++k)
            {
                const int a = rep[t[k]], b = rep[t[(k + 1) % 3]];
                if (a == b) continue;
                const uint64_t e = ((uint64_t)std::min(a, b) << 32) | (uint64_t)std::max(a, b);
                ++edges[e];
            }
        }
        // the connected pieces (on welded points)
        std::vector<int> par(bn);
        for (size_t i = 0; i < bn; ++i) par[i] = (int)i;
        auto find = [&](int x) { while (par[x] != x) { par[x] = par[par[x]]; x = par[x]; } return x; };
        for (const auto& kv : edges)
        {
            const int a = find((int)(kv.first >> 32)), b = find((int)(kv.first & 0xFFFFFFFFu));
            if (a != b) par[a] = b;
        }
        // the welded points by 2 mm cells, to ask "does another piece go on here?"
        const float JOIN = 0.002f;
        Cells near2;
        near2.cell = JOIN;
        for (size_t i = 0; i < bn; ++i)
        {
            if ((size_t)rep[i] != i) continue;
            const V3& p = body.pos[i];
            near2.map[Cells::key(near2.c(p.x), near2.c(p.y), near2.c(p.z))].push_back((int)i);
        }
        auto joined = [&](const V3& m, int root) -> bool
        {
            const int cx = near2.c(m.x), cy = near2.c(m.y), cz = near2.c(m.z);
            for (int x = cx - 1; x <= cx + 1; ++x)
                for (int y = cy - 1; y <= cy + 1; ++y)
                    for (int z = cz - 1; z <= cz + 1; ++z)
                    {
                        const std::vector<int>* list = near2.at(x, y, z);
                        if (!list) continue;
                        for (int q : *list) if (find(q) != root && len2(body.pos[q] - m) < JOIN * JOIN) return true;
                    }
            return false;
        };
        std::unordered_map<int, std::array<long, 3>> comp;   // root -> edges, free open edges, triangles
        for (const auto& kv : edges)
        {
            const int a = (int)(kv.first >> 32), b = (int)(kv.first & 0xFFFFFFFFu);
            const int root = find(a);
            auto& c = comp[root];
            ++c[0];
            if (kv.second != 1) continue;
            // a section's border: another piece has points at both its ends (its points sit at the ends, not the middle)
            if (joined(body.pos[a], root) && joined(body.pos[b], root)) continue;
            ++c[1];
            open[(size_t)a] = 1;
            open[(size_t)b] = 1;
        }
        for (const auto& t : body.tris) ++comp[find(rep[t[0]])][2];
        // thin open pieces (a strap hanging from the pants, a ribbon, a tag, a flat decal) have no inside:
        // a garment point behind one is not in the body (the shirt's hem was torn into spikes by straps)
        for (const auto& kv : comp)
        {
            const bool flat = kv.second[0] > 0 && kv.second[1] * 100 > kv.second[0] * 15;   // over 15 % free edges
            const bool tiny = kv.second[2] < 40 && kv.second[1] * 4 > kv.second[0];         // small and mostly free
            if (tiny || flat) thinRoot.insert(kv.first);
        }
        if (!thinRoot.empty())
        {
            for (size_t i = 0; i < bn; ++i) thin[i] = thinRoot.count(find(rep[i])) ? 1 : 0;
            out.thinParts = (int)thinRoot.size();
        }
    }

    // the garment's box
    V3 glo(1e9f, 1e9f, 1e9f), ghi(-1e9f, -1e9f, -1e9f);
    for (const V3& p : garment.pos)
    {
        glo = V3(std::min(glo.x, p.x), std::min(glo.y, p.y), std::min(glo.z, p.z));
        ghi = V3(std::max(ghi.x, p.x), std::max(ghi.y, p.y), std::max(ghi.z, p.z));
    }
    glo = glo - V3(reach, reach, reach);
    ghi = ghi + V3(reach, reach, reach);
    auto inBox = [&](const V3& p) { return p.x >= glo.x && p.y >= glo.y && p.z >= glo.z && p.x <= ghi.x && p.y <= ghi.y && p.z <= ghi.z; };

    std::vector<V3> need(gn, V3());   // what each garment point needs before spreading
    std::vector<V3> skinN(gn, V3());  // the outward normal of the skin nearest each garment point (zero: none near)
    std::vector<uint8_t> deep(gn, 0); // buried deeper than the largest push: never moved (neither own need nor neighbours')

    // 2. garment points inside the skin, or closer than the margin: the nearest body triangle
    {
        Cells cells;
        cells.cell = std::max(reach, 0.01f);
        for (size_t ti = 0; ti < body.tris.size(); ++ti)
        {
            const auto& t = body.tris[ti];
            if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= (int)bn || t[1] >= (int)bn || t[2] >= (int)bn) continue;
            if (thin[t[0]]) continue;
            const V3 &a = body.pos[t[0]], &b = body.pos[t[1]], &c = body.pos[t[2]];
            const V3 lo(std::min({ a.x, b.x, c.x }), std::min({ a.y, b.y, c.y }), std::min({ a.z, b.z, c.z }));
            const V3 hi(std::max({ a.x, b.x, c.x }), std::max({ a.y, b.y, c.y }), std::max({ a.z, b.z, c.z }));
            if (hi.x < glo.x - reach || hi.y < glo.y - reach || hi.z < glo.z - reach || lo.x > ghi.x + reach || lo.y > ghi.y + reach || lo.z > ghi.z + reach) continue;
            cells.addBox((int)ti, lo, hi);
        }
        std::vector<uint32_t> seen(body.tris.size(), 0);
        uint32_t stamp = 0;
        for (size_t v = 0; v < gn; ++v)
        {
            if (isFixed(v)) continue;
            const V3& p = garment.pos[v];
            const int cx = cells.c(p.x), cy = cells.c(p.y), cz = cells.c(p.z);
            ++stamp;
            // twice the reach: a point buried deeper than any push must still be known as inside
            // (unknown, its neighbours' pushes dragged it along, deeper still)
            float best = 4.f * reach * reach;
            int bt = -1;
            float bu = 0.f, bv = 0.f, bw = 0.f;
            V3 bq;
            for (int x = cx - 2; x <= cx + 2; ++x)
                for (int y = cy - 2; y <= cy + 2; ++y)
                    for (int z = cz - 2; z <= cz + 2; ++z)
                    {
                        const std::vector<int>* list = cells.at(x, y, z);
                        if (!list) continue;
                        for (int ti : *list)
                        {
                            if (seen[ti] == stamp) continue;
                            seen[ti] = stamp;
                            const auto& t = body.tris[ti];
                            float u, vv, w;
                            const V3 q = closestOnTri(p, body.pos[t[0]], body.pos[t[1]], body.pos[t[2]], u, vv, w);
                            const float d2 = len2(p - q);
                            if (d2 < best) { best = d2; bt = ti; bq = q; bu = u; bv = vv; bw = w; }
                        }
                    }
            if (bt < 0) continue;
            const auto& t = body.tris[bt];
            V3 n = unit(nrm[t[0]] * bu + nrm[t[1]] * bv + nrm[t[2]] * bw);
            if (len2(n) < 0.5f) n = unit(cross3(body.pos[t[1]] - body.pos[t[0]], body.pos[t[2]] - body.pos[t[0]]));
            if (len2(n) < 0.5f) continue;
            skinN[v] = n;
            const float dist = std::sqrt(best);
            const float s = (dot3(p - bq, n) < 0.f) ? -dist : dist;   // < 0: under the skin
            if (s < 0.f)
            {
                // nearest to an open edge of the skin (every corner that counts is on one): past the edge, not inside
                const float wts[3] = { bu, bv, bw };
                bool allOpen = true;
                for (int k = 0; k < 3; ++k) if (wts[k] > 1e-3f && !open[rep[t[k]]]) allOpen = false;
                if (allOpen) { skinN[v] = V3(); continue; }
            }
            out.sdist[v] = s;
            // deeper than the largest push: a half push would only bulge it, still inside (left to the creator)
            if (s < -maxPush) { deep[v] = 1; continue; }
            if (s >= margin - 0.0002f) continue;   // within 0.2 mm of the margin: fine
            const float amount = std::min(margin - s, maxPush);
            ++out.inside;
            if (prm.deepAt > 0.f && -s > prm.deepAt) ++out.overDeep;
            if (keepLonger(need[v], n * amount)) out.src[v] = 1;
        }
    }

    // 3. body points through a garment face: the skin's outward line against the garment's triangles
    {
        Cells cells;
        cells.cell = std::max(reach, 0.01f);
        for (size_t ti = 0; ti < garment.tris.size(); ++ti)
        {
            const auto& t = garment.tris[ti];
            if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= (int)gn || t[1] >= (int)gn || t[2] >= (int)gn) continue;
            const V3 &a = garment.pos[t[0]], &b = garment.pos[t[1]], &c = garment.pos[t[2]];
            cells.addBox((int)ti, V3(std::min({ a.x, b.x, c.x }), std::min({ a.y, b.y, c.y }), std::min({ a.z, b.z, c.z })),
                                  V3(std::max({ a.x, b.x, c.x }), std::max({ a.y, b.y, c.y }), std::max({ a.z, b.z, c.z })));
        }
        std::vector<uint32_t> seen(garment.tris.size(), 0);
        uint32_t stamp = 0;
        struct Hit { float t, u, v; int tri; };
        std::vector<Hit> hits;
        const float ahead = margin + shell;
        for (size_t i = 0; i < bn; ++i)
        {
            if ((size_t)rep[i] != i || open[i] || thin[i]) continue;   // each place once; open edges and thin pieces have no outside
            const V3& b = body.pos[i];
            const V3& n = nrm[i];
            if (len2(n) < 0.5f || !inBox(b)) continue;
            const V3 s0 = b - n * reach, s1 = b + n * ahead;
            ++stamp;
            hits.clear();
            for (int x = cells.c(std::min(s0.x, s1.x)); x <= cells.c(std::max(s0.x, s1.x)); ++x)
                for (int y = cells.c(std::min(s0.y, s1.y)); y <= cells.c(std::max(s0.y, s1.y)); ++y)
                    for (int z = cells.c(std::min(s0.z, s1.z)); z <= cells.c(std::max(s0.z, s1.z)); ++z)
                    {
                        const std::vector<int>* list = cells.at(x, y, z);
                        if (!list) continue;
                        for (int ti : *list)
                        {
                            if (seen[ti] == stamp) continue;
                            seen[ti] = stamp;
                            const auto& t = garment.tris[ti];
                            float tt, u, v;
                            if (!rayTri(b, n, garment.pos[t[0]], garment.pos[t[1]], garment.pos[t[2]], tt, u, v)) continue;
                            if (tt < -reach || tt > ahead) continue;
                            hits.push_back({ tt, u, v, ti });
                        }
                    }
            if (hits.empty()) continue;
            float tmin = 1e9f;
            for (const Hit& h : hits) tmin = std::min(tmin, h.t);
            if (tmin >= margin - 0.0002f || tmin < -maxPush) continue;
            if (tmin < 0.f) ++out.pokes;
            if (prm.deepAt > 0.f && -tmin > prm.deepAt) ++out.overDeep;
            const float P = std::min(margin - tmin, maxPush);
            for (const Hit& h : hits)
            {
                if (h.t > tmin + shell) continue;   // a layer further out is clear already
                const auto& t = garment.tris[h.tri];
                const float w[3] = { 1.f - h.u - h.v, h.u, h.v };
                const float S = w[0] * w[0] + w[1] * w[1] + w[2] * w[2];
                if (S <= 1e-6f) continue;
                // the corners move so the point on the face moves by P (weights / sum of squares)
                for (int k = 0; k < 3; ++k)
                {
                    if (isFixed((size_t)t[k]) || deep[t[k]] || w[k] <= 0.f) continue;
                    if (len2(skinN[t[k]]) > 0.5f && dot3(skinN[t[k]], n) < 0.f) continue;   // that corner sits over other skin
                    if (keepLonger(need[t[k]], n * (P * std::min(1.2f, w[k] / S)))) out.src[t[k]] = 2;
                }
            }
        }
    }

    // 4. spread: every push reaches the points around it in 3D (plateau, then a smooth fade)
    out.need = need;
    std::vector<int> seeds;
    for (size_t v = 0; v < gn; ++v)
    {
        if (len2(need[v]) <= 1e-14f) continue;
        seeds.push_back((int)v);
        out.deepest = std::max(out.deepest, std::sqrt(len2(need[v])));
    }
    if (seeds.empty()) return;
    out.push = need;
    Cells pts;
    pts.cell = radius;
    for (size_t v = 0; v < gn; ++v)
    {
        if (isFixed(v) || deep[v]) continue;
        const V3& p = garment.pos[v];
        pts.map[Cells::key(pts.c(p.x), pts.c(p.y), pts.c(p.z))].push_back((int)v);
    }
    const float r2 = radius * radius;
    const float fade = std::max(1e-6f, radius - plateau);
    for (int u : seeds)
    {
        const V3& p = garment.pos[u];
        const V3& r = need[u];
        const V3 ru = unit(r);
        const int cx = pts.c(p.x), cy = pts.c(p.y), cz = pts.c(p.z);
        for (int x = cx - 1; x <= cx + 1; ++x)
            for (int y = cy - 1; y <= cy + 1; ++y)
                for (int z = cz - 1; z <= cz + 1; ++z)
                {
                    const std::vector<int>* list = pts.at(x, y, z);
                    if (!list) continue;
                    for (int v : *list)
                    {
                        if (v == u) continue;
                        const float d2 = len2(garment.pos[v] - p);
                        if (d2 >= r2) continue;
                        // a point over another part of the skin (the other side of a sleeve, a lapel over
                        // the chest) must not take this push: it would go into the body there
                        if (len2(skinN[v]) > 0.5f && dot3(skinN[v], ru) < 0.7f) continue;
                        const float d = std::sqrt(d2);
                        float f = 1.f;
                        if (d > plateau)
                        {
                            const float s = (radius - d) / fade;
                            f = s * s * (3.f - 2.f * s);
                        }
                        if (keepLonger(out.push[v], r * f)) out.src[v] = 4;
                    }
                }
    }
}
}
