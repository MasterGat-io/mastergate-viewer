/**
 * @file mgdrape.cpp
 * @brief MasterGate Viewer: Creator Lab's Drape (cloth simulation, position-based dynamics).
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

#include "mgdrape.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace
{
    inline float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

    // Ericson's closest point on triangle abc to p
    void closest(const float* p, const float* a, const float* b, const float* c, float* out)
    {
        float ab[3], ac[3], ap[3];
        for (int k = 0; k < 3; ++k) { ab[k] = b[k] - a[k]; ac[k] = c[k] - a[k]; ap[k] = p[k] - a[k]; }
        const float d1 = dot3(ab, ap), d2 = dot3(ac, ap);
        if (d1 <= 0 && d2 <= 0) { for (int k = 0; k < 3; ++k) out[k] = a[k]; return; }
        float bp[3];
        for (int k = 0; k < 3; ++k) bp[k] = p[k] - b[k];
        const float d3 = dot3(ab, bp), d4 = dot3(ac, bp);
        if (d3 >= 0 && d4 <= d3) { for (int k = 0; k < 3; ++k) out[k] = b[k]; return; }
        const float vc = d1 * d4 - d3 * d2;
        if (vc <= 0 && d1 >= 0 && d3 <= 0) { const float t = d1 / (d1 - d3); for (int k = 0; k < 3; ++k) out[k] = a[k] + ab[k] * t; return; }
        float cp[3];
        for (int k = 0; k < 3; ++k) cp[k] = p[k] - c[k];
        const float d5 = dot3(ab, cp), d6 = dot3(ac, cp);
        if (d6 >= 0 && d5 <= d6) { for (int k = 0; k < 3; ++k) out[k] = c[k]; return; }
        const float vb = d5 * d2 - d1 * d6;
        if (vb <= 0 && d2 >= 0 && d6 <= 0) { const float t = d2 / (d2 - d6); for (int k = 0; k < 3; ++k) out[k] = a[k] + ac[k] * t; return; }
        const float va = d3 * d6 - d5 * d4;
        if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { const float t = (d4 - d3) / ((d4 - d3) + (d5 - d6)); for (int k = 0; k < 3; ++k) out[k] = b[k] + (c[k] - b[k]) * t; return; }
        const float den = 1.f / (va + vb + vc);
        const float v = vb * den, w = vc * den;
        for (int k = 0; k < 3; ++k) out[k] = a[k] + ab[k] * v + ac[k] * w;
    }
}

namespace MGDrape
{
    uint64_t Sim::key(long x, long y, long z) const
    {
        return ((uint64_t)(x + (1 << 20)) << 42) | ((uint64_t)(y + (1 << 20)) << 21) | (uint64_t)(z + (1 << 20));
    }
    long Sim::cell(float v) const { return (long)std::floor(v / mCell); }

    // <MG> weld first: points at the same place (to 0.01 mm) become one cloth point. A mesh exported
    // with every face apart (23,597 pieces of 4 points in a pair of trousers) is then one cloth again:
    // it bends across its seams, and is not held together by tens of thousands of stitches
    bool Sim::init(const std::vector<float>& positions, const std::vector<std::array<int, 3>>& triangles,
                   const std::vector<float>& bodyPositions, const std::vector<std::array<int, 3>>& bodyTriangles,
                   const Settings& s)
    {
        const size_t ne = positions.size() / 3;
        mRep.assign(ne, -1);
        mFirst.clear();
        // within 0.5 mm (a garment's pieces meet at seams a hair apart; layers are 3 mm and more apart):
        // cells of 0.5 mm, the neighbouring cells searched too
        const double tol = 0.0005;
        struct K { long long x, y, z; bool operator==(const K& o) const { return x == o.x && y == o.y && z == o.z; } };
        struct KH { size_t operator()(const K& k) const { return (size_t)(k.x * 73856093LL ^ k.y * 19349663LL ^ k.z * 83492791LL); } };
        std::unordered_map<K, std::vector<int>, KH> at;
        at.reserve(ne);
        std::vector<float> wp;
        wp.reserve(positions.size());
        for (size_t e = 0; e < ne; ++e)
        {
            const float* pe = &positions[e * 3];
            const K k{ (long long)std::floor(pe[0] / tol), (long long)std::floor(pe[1] / tol), (long long)std::floor(pe[2] / tol) };
            int found = -1;
            for (long long x = k.x - 1; x <= k.x + 1 && found < 0; ++x)
                for (long long y = k.y - 1; y <= k.y + 1 && found < 0; ++y)
                    for (long long z = k.z - 1; z <= k.z + 1 && found < 0; ++z)
                    {
                        auto it = at.find(K{ x, y, z });
                        if (it == at.end()) continue;
                        for (int p : it->second)
                        {
                            const float dx = wp[p * 3] - pe[0], dy = wp[p * 3 + 1] - pe[1], dz = wp[p * 3 + 2] - pe[2];
                            if (dx * dx + dy * dy + dz * dz <= (float)(tol * tol)) { found = p; break; }
                        }
                    }
            if (found >= 0) { mRep[e] = found; continue; }
            const int p = (int)mFirst.size();
            at[k].push_back(p);
            mRep[e] = p;
            mFirst.push_back((int)e);
            for (int q = 0; q < 3; ++q) wp.push_back(pe[q]);
        }
        std::vector<std::array<int, 3>> wt;
        wt.reserve(triangles.size());
        for (const auto& t : triangles)
        {
            if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= (int)ne || t[1] >= (int)ne || t[2] >= (int)ne) continue;
            const std::array<int, 3> r{ mRep[t[0]], mRep[t[1]], mRep[t[2]] };
            if (r[0] == r[1] || r[1] == r[2] || r[0] == r[2]) continue;   // collapsed by the weld
            wt.push_back(r);
        }
        const bool ok = initWelded(wp, wt, bodyPositions, bodyTriangles, s);
        mXe.assign(positions.size(), 0.f);
        mPinE.assign(ne, 0);
        return ok;
    }

    const std::vector<float>& Sim::positions()
    {
        const size_t ne = mRep.size();
        mXe.resize(ne * 3);
        for (size_t e = 0; e < ne; ++e) { const int p = mRep[e]; for (int q = 0; q < 3; ++q) mXe[e * 3 + q] = mX[p * 3 + q]; }
        return mXe;
    }
    const std::vector<uint8_t>& Sim::pins()
    {
        const size_t ne = mRep.size();
        mPinE.resize(ne);
        for (size_t e = 0; e < ne; ++e) mPinE[e] = mPin[mRep[e]];
        return mPinE;
    }

    bool Sim::initWelded(const std::vector<float>& positions, const std::vector<std::array<int, 3>>& triangles,
                         const std::vector<float>& bodyPositions, const std::vector<std::array<int, 3>>& bodyTriangles,
                         const Settings& s)
    {
        mS = s;
        mX = positions;
        mPrev = positions;
        const size_t n = positions.size() / 3;
        if (n == 0 || triangles.empty()) return false;
        mV.assign(n * 3, 0.f);
        mPin.assign(n, 0);
        mPinAt = positions;
        mGrabs.clear();
        // edges (stretch) and the points across two triangles (bend)
        std::map<std::pair<int, int>, std::vector<int>> opposite;   // edge -> the third points
        for (const auto& t : triangles)
            for (int k = 0; k < 3; ++k)
            {
                int a = t[k], b = t[(k + 1) % 3], c = t[(k + 2) % 3];
                if (a < 0 || b < 0 || c < 0 || a >= (int)n || b >= (int)n || c >= (int)n || a == b) continue;
                if (a > b) std::swap(a, b);
                opposite[{ a, b }].push_back(c);
            }
        mEdges.clear();
        mBends.clear();
        mPointEdges.assign(n, {});
        auto len = [&](int a, int b)
        {
            const float dx = mX[a * 3] - mX[b * 3], dy = mX[a * 3 + 1] - mX[b * 3 + 1], dz = mX[a * 3 + 2] - mX[b * 3 + 2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        for (const auto& kv : opposite)
        {
            const int a = kv.first.first, b = kv.first.second;
            mPointEdges[a].push_back((int)mEdges.size());
            mPointEdges[b].push_back((int)mEdges.size());
            const float l = len(a, b);
            mEdges.push_back({ a, b, l, l });
            if (kv.second.size() == 2 && kv.second[0] != kv.second[1])
            {
                const float lb = len(kv.second[0], kv.second[1]);
                mBends.push_back({ kv.second[0], kv.second[1], lb, lb });
            }
        }
        // <MG> sewing: a garment's separate pieces (a trim, lace, a button, a hood modelled apart) would fall
        // off; each point of a smaller piece within 2 cm of another piece is stitched to its nearest point
        // there (a stiff edge at the distance it was made with). Pieces touching nothing stay free.
        mSewn = 0;
        {
            std::vector<int> par(n);
            for (size_t i = 0; i < n; ++i) par[i] = (int)i;
            auto fnd = [&](int x) { while (par[x] != x) { par[x] = par[par[x]]; x = par[x]; } return x; };
            for (const Edge& e : mEdges) par[fnd(e.a)] = fnd(e.b);
            std::unordered_map<int, int> size;
            for (size_t i = 0; i < n; ++i) ++size[fnd((int)i)];
            if (size.size() > 1)
            {
                const float cs2 = 0.02f;
                auto ck = [&](float v) { return (long)std::floor(v / cs2); };
                std::unordered_map<uint64_t, std::vector<int>> grid;
                for (size_t i = 0; i < n; ++i) grid[key(ck(mX[i * 3]), ck(mX[i * 3 + 1]), ck(mX[i * 3 + 2]))].push_back((int)i);
                size_t sewn = 0;
                for (size_t i = 0; i < n; ++i)
                {
                    const int ci = fnd((int)i);
                    float best = cs2 * cs2;
                    int bj = -1;
                    for (long x = ck(mX[i * 3]) - 1; x <= ck(mX[i * 3]) + 1; ++x) for (long y = ck(mX[i * 3 + 1]) - 1; y <= ck(mX[i * 3 + 1]) + 1; ++y) for (long z = ck(mX[i * 3 + 2]) - 1; z <= ck(mX[i * 3 + 2]) + 1; ++z)
                    {
                        auto it = grid.find(key(x, y, z));
                        if (it == grid.end()) continue;
                        for (int j : it->second)
                        {
                            const int cj = fnd(j);
                            // from the smaller piece to a bigger one (each seam once)
                            if (cj == ci || size[cj] < size[ci] || (size[cj] == size[ci] && cj < ci)) continue;
                            const float d2 = (mX[j * 3] - mX[i * 3]) * (mX[j * 3] - mX[i * 3]) + (mX[j * 3 + 1] - mX[i * 3 + 1]) * (mX[j * 3 + 1] - mX[i * 3 + 1]) + (mX[j * 3 + 2] - mX[i * 3 + 2]) * (mX[j * 3 + 2] - mX[i * 3 + 2]);
                            if (d2 < best) { best = d2; bj = j; }
                        }
                    }
                    if (bj < 0) continue;
                    mPointEdges[i].push_back((int)mEdges.size());
                    mPointEdges[bj].push_back((int)mEdges.size());
                    const float l = std::sqrt(best);
                    mEdges.push_back({ (int)i, bj, l, l });
                    ++sewn;
                }
                mSewn = sewn;
            }
        }
        // <MG> each point's mass: the cloth area it carries (a dense collar is not heavier per metre
        // than the hem); inverse masses scaled to average 1
        mT.clear();
        std::vector<float> area(n, 0.f);
        double edgeSum = 0;
        for (const auto& t : triangles)
        {
            if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= (int)n || t[1] >= (int)n || t[2] >= (int)n || t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) continue;
            mT.push_back(t);
            const float* a = &mX[t[0] * 3]; const float* b = &mX[t[1] * 3]; const float* c = &mX[t[2] * 3];
            const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            const float cr[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            const float A = 0.5f * std::sqrt(dot3(cr, cr));
            for (int k = 0; k < 3; ++k) area[t[k]] += A / 3.f;
            edgeSum += len(t[0], t[1]);
        }
        double areaSum = 0; size_t areaN = 0;
        for (float A : area) if (A > 0.f) { areaSum += A; ++areaN; }
        const float meanA = areaN ? (float)(areaSum / areaN) : 1.f;
        mW.assign(n, 1.f);
        for (size_t i = 0; i < n; ++i) if (area[i] > 0.f) mW[i] = std::max(0.2f, std::min(5.f, meanA / area[i]));
        mCorr.assign(n, 0.f);
        mCN.assign(n * 3, 0.f);
        mTouch.assign(n, 0);
        mPlane.assign(n * 6, 0.f);
        mHasPlane.assign(n, 0);
        mTriCell = std::max(0.015f, std::min(0.05f, mT.empty() ? 0.03f : (float)(2.0 * edgeSum / mT.size())));
        mTriStamp.assign(mT.size(), 0);
        mTriTick = 0;
        mHint.assign(n, -1);
        mBT.clear();
        setBody(bodyPositions, bodyTriangles);
        // points already under the skin (up to 3 cm): out first
        for (size_t i = 0; i < n; ++i)
        {
            float c[3], nr[3], sm[3], d, depth;
            if (!nearestBody(&mX[i * 3], 0.03f, c, nr, d, &mHint[i], sm)) continue;
            if (underSkin(&mX[i * 3], c, nr, sm, depth) && depth > -0.03f)
                for (int k = 0; k < 3; ++k) mX[i * 3 + k] = mPrev[i * 3 + k] = c[k] + sm[k] * mS.thickness;
        }
        mAnchor = mX;
        mHoldW.assign(n, 1.f);
        measureGaps();
        return true;
    }

    bool Sim::neighbours(int i, const std::array<int, 3>& t) const
    {
        for (int q = 0; q < 3; ++q)
        {
            if (t[q] == i) return true;
            for (int e : mPointEdges[i]) if (mEdges[e].a == t[q] || mEdges[e].b == t[q]) return true;
        }
        return false;
    }

    // <MG> as made, each point's distance to the nearest triangle of another layer (a garment modelled
    // as a shell has its outside and inside 3-5 mm apart): the layers may not come closer than most of it
    void Sim::measureGaps()
    {
        const size_t n = count();
        mGap.assign(n, 1.f);
        if (mT.empty()) return;
        buildTriGrid();
        const float reach = 0.02f, cs = mTriCell;
        for (size_t i = 0; i < n; ++i)
        {
            const float* p = &mX[i * 3];
            ++mTriTick;
            const long x0 = (long)std::floor((p[0] - reach) / cs), x1 = (long)std::floor((p[0] + reach) / cs);
            const long y0 = (long)std::floor((p[1] - reach) / cs), y1 = (long)std::floor((p[1] + reach) / cs);
            const long z0 = (long)std::floor((p[2] - reach) / cs), z1 = (long)std::floor((p[2] + reach) / cs);
            float best = reach * reach;
            for (long x = x0; x <= x1; ++x) for (long y = y0; y <= y1; ++y) for (long z = z0; z <= z1; ++z)
            {
                auto it = mTriGrid.find(key(x, y, z));
                if (it == mTriGrid.end()) continue;
                for (int ti : it->second)
                {
                    if (mTriStamp[ti] == mTriTick) continue;
                    mTriStamp[ti] = mTriTick;
                    if (neighbours((int)i, mT[ti])) continue;
                    const float* a = &mX[mT[ti][0] * 3]; const float* b = &mX[mT[ti][1] * 3]; const float* cc = &mX[mT[ti][2] * 3];
                    float c[3];
                    closest(p, a, b, cc, c);
                    const float dv[3] = { p[0] - c[0], p[1] - c[1], p[2] - c[2] };
                    const float d2 = dot3(dv, dv);
                    if (d2 >= best) continue;
                    // only another layer over or under it (not the same fabric beside it)
                    const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { cc[0] - a[0], cc[1] - a[1], cc[2] - a[2] };
                    float nn[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                    const float nl = std::sqrt(dot3(nn, nn));
                    if (nl < 1e-12f || std::fabs(dot3(dv, nn)) / nl < 0.7f * std::sqrt(d2)) continue;
                    best = d2;
                }
            }
            mGap[i] = std::sqrt(best);
        }
    }

    // the body point nearest p (within reach), and the outward normal there
    bool Sim::underSkin(const float p[3], const float c[3], const float n[3], const float sm[3], float& depth) const
    {
        const float rel[3] = { p[0] - c[0], p[1] - c[1], p[2] - c[2] };
        const float df = dot3(rel, n), ds = dot3(rel, sm);
        depth = std::min(df, ds);
        return df < mS.thickness && ds < mS.thickness;
    }

    bool Sim::nearestBody(const float p[3], float reach, float out[3], float n[3], float& dist, int* hint, float* smooth)
    {
        if (mGrid.empty()) return false;
        auto d2of = [&](int v) { const float dx = mB[v * 3] - p[0], dy = mB[v * 3 + 1] - p[1], dz = mB[v * 3 + 2] - p[2]; return dx * dx + dy * dy + dz * dz; };
        int near1 = -1;
        float best = 1e30f;
        // from the body point nearest last time, walk to closer neighbours (cloth moves little per step)
        if (hint && *hint >= 0)
        {
            near1 = *hint;
            best = d2of(near1);
            for (int hop = 0; hop < 64; ++hop)
            {
                int nextV = -1;
                for (int u : mBodyNb[near1]) { const float d = d2of(u); if (d < best) { best = d; nextV = u; } }
                if (nextV < 0) break;
                near1 = nextV;
            }
            if (best > 0.06f * 0.06f) { near1 = -1; best = 1e30f; }   // lost: search again
        }
        // the nearest body points in the cells around p
        const long cx = cell(p[0]), cy = cell(p[1]), cz = cell(p[2]);
        const long r = near1 >= 0 ? -1 : 1;
        if (near1 < 0) best = (reach + mCell) * (reach + mCell);
        for (long x = cx - r; x <= cx + r; ++x)
            for (long y = cy - r; y <= cy + r; ++y)
                for (long z = cz - r; z <= cz + r; ++z)
                {
                    auto it = mGrid.find(key(x, y, z));
                    if (it == mGrid.end()) continue;
                    for (int v : it->second)
                    {
                        const float dx = mB[v * 3] - p[0], dy = mB[v * 3 + 1] - p[1], dz = mB[v * 3 + 2] - p[2];
                        const float d2 = dx * dx + dy * dy + dz * dz;
                        if (d2 < best) { best = d2; near1 = v; }
                    }
                }
        if (near1 < 0) return false;
        if (hint) *hint = near1;
        // the triangles around that point and its neighbours
        ++mTick;
        float bestD = 1e30f;
        int bestT = -1;
        auto test = [&](int ti)
        {
            if (mStamp[ti] == mTick) return;
            mStamp[ti] = mTick;
            const auto& t = mBT[ti];
            float c[3];
            closest(p, &mB[t[0] * 3], &mB[t[1] * 3], &mB[t[2] * 3], c);
            const float d2 = (p[0] - c[0]) * (p[0] - c[0]) + (p[1] - c[1]) * (p[1] - c[1]) + (p[2] - c[2]) * (p[2] - c[2]);
            if (d2 < bestD) { bestD = d2; bestT = ti; out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; }
        };
        for (int ti : mBodyTris[near1])
        {
            test(ti);
            const auto& t = mBT[ti];
            for (int k = 0; k < 3; ++k) for (int tj : mBodyTris[t[k]]) test(tj);
        }
        if (bestT < 0 || bestD > reach * reach) return false;
        n[0] = mBN[bestT][0]; n[1] = mBN[bestT][1]; n[2] = mBN[bestT][2];
        dist = std::sqrt(bestD);
        if (smooth)
        {
            // the corners' normals, weighted by where the closest point sits in the triangle
            const auto& t = mBT[bestT];
            const float* a = &mB[t[0] * 3]; const float* b = &mB[t[1] * 3]; const float* cc = &mB[t[2] * 3];
            const float v0[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v1[3] = { cc[0] - a[0], cc[1] - a[1], cc[2] - a[2] }, v2[3] = { out[0] - a[0], out[1] - a[1], out[2] - a[2] };
            const float d00 = dot3(v0, v0), d01 = dot3(v0, v1), d11 = dot3(v1, v1), d20 = dot3(v2, v0), d21 = dot3(v2, v1);
            const float den = d00 * d11 - d01 * d01;
            float wv = 1.f / 3.f, ww = 1.f / 3.f;
            if (std::fabs(den) > 1e-20f) { wv = (d11 * d20 - d01 * d21) / den; ww = (d00 * d21 - d01 * d20) / den; }
            wv = std::max(0.f, std::min(1.f, wv)); ww = std::max(0.f, std::min(1.f - wv, ww));
            const float wu = 1.f - wv - ww;
            float l2 = 0.f;
            for (int q = 0; q < 3; ++q) { smooth[q] = mVN[t[0]][q] * wu + mVN[t[1]][q] * wv + mVN[t[2]][q] * ww; l2 += smooth[q] * smooth[q]; }
            if (l2 < 1e-12f) { for (int q = 0; q < 3; ++q) smooth[q] = n[q]; }
            else { const float l = std::sqrt(l2); for (int q = 0; q < 3; ++q) smooth[q] /= l; }
        }
        return true;
    }

    // XPBD, one pass per sub-step (lambda starts at 0): each edge back towards its rest length by
    // C / (wa + wb + alphaTilde); alphaTilde = compliance / h^2 (0: rigid)
    void Sim::solve(std::vector<Edge>& edges, float alphaTilde)
    {
        for (const Edge& e : edges)
        {
            const float wa = w(e.a), wb = w(e.b);
            if (wa + wb == 0.f) continue;
            float* a = &mX[e.a * 3];
            float* b = &mX[e.b * 3];
            const float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            const float l = std::sqrt(dot3(d, d));
            if (l < 1e-9f) continue;
            const float c = (l - e.rest) / (l * (wa + wb + alphaTilde));
            for (int q = 0; q < 3; ++q) { a[q] += wa * c * d[q]; b[q] -= wb * c * d[q]; }
        }
    }

    void Sim::collide(bool friction)
    {
        mContacts = 0;
        const size_t n = count();
        mTouch.assign(n, 0);
        const float reach = 0.05f;   // look this far; only points nearer the skin than the thickness are moved
        for (size_t i = 0; i < n; ++i)
        {
            if (mPin[i]) continue;
            float* p = &mX[i * 3];
            const float* q = &mPrev[i * 3];
            float c[3], nr[3], sm[3], d, depth;
            if (!nearestBody(p, reach, c, nr, d, &mHint[i], sm)) continue;
            const float rel[3] = { p[0] - c[0], p[1] - c[1], p[2] - c[2] }, relq[3] = { q[0] - c[0], q[1] - c[1], q[2] - c[2] };
            const float sf = dot3(rel, nr);
            // <MG> on the skin (within a millimetre of the thickness): friction holds it there
            const bool onSkin = std::min(sf, dot3(rel, sm)) < mS.thickness + 0.001f;
            // <MG> outside at the last step and near the skin: this skin's plane holds it in every pass
            if (dot3(relq, nr) >= -0.5f * mS.thickness && sf < mS.thickness + 0.03f)
            {
                mHasPlane[i] = 1;
                float* pl = &mPlane[i * 6];
                for (int k = 0; k < 3; ++k) { pl[k] = c[k]; pl[3 + k] = nr[k]; }
            }
            float mv[3] = { 0.f, 0.f, 0.f };
            if (dot3(relq, nr) >= -0.5f * mS.thickness && sf < mS.thickness)
            {
                // it was outside at the last step and went in: a real crossing, whatever the depth (a fast
                // fall can go 2 cm in one step); back onto the plane of the skin it went through, fully
                for (int k = 0; k < 3; ++k) mv[k] = nr[k] * (mS.thickness - sf);
            }
            else if (underSkin(p, c, nr, sm, depth))
            {
                // already under the skin (it started there, or the body moved onto it)
                // deeper than a step can go: the wrong side of a thin part, not a real crossing
                if (depth < -((mBodyMoved > 0 ? 0.06f : 0.025f) + mS.thickness)) continue;
                // out to the skin, 8 mm per pass at most: eased out, and a point in a fold of the skin (the
                // nearest side flipping between steps) wiggles instead of being thrown across
                float m2 = 0.f;
                for (int k = 0; k < 3; ++k) { mv[k] = c[k] + sm[k] * mS.thickness - p[k]; m2 += mv[k] * mv[k]; }
                const float maxPush = 0.008f;
                if (m2 > maxPush * maxPush) { const float sc = maxPush / std::sqrt(m2); for (int k = 0; k < 3; ++k) mv[k] *= sc; }
            }
            else if (!onSkin) continue;
            for (int k = 0; k < 3; ++k) p[k] += mv[k];
            mCorr[i] += std::max(0.f, dot3(mv, nr));
            mTouch[i] = 1;
            for (int k = 0; k < 3; ++k) mCN[i * 3 + k] = nr[k];
            ++mContacts;
            if (friction && mS.friction > 0.f)
            {
                // Coulomb friction: the slide along the skin this step is stopped while it is smaller than
                // mu x the skin's push (static, a little stronger), else shortened by mu x the push (kinetic).
                // A coat stays on the shoulders, cloth slides off a slope steeper than its friction allows.
                float dp[3] = { p[0] - q[0], p[1] - q[1], p[2] - q[2] };
                const float nd = dot3(dp, nr);
                float tg[3] = { dp[0] - nr[0] * nd, dp[1] - nr[1] * nd, dp[2] - nr[2] * nd };
                const float tl = std::sqrt(dot3(tg, tg));
                // the load: the skin's push this step, at least gravity's share into the skin (cloth resting on it)
                const float gIn = 9.81f * mS.gravity * (1.f / 3600.f) * std::max(0.f, nr[2]);
                const float load = std::max(mCorr[i], gIn);
                const float muS = std::min(1.2f, mS.friction * 1.3f), muK = mS.friction;
                if (tl > 1e-9f)
                {
                    const float cut = tl <= muS * load ? 1.f : std::min(1.f, muK * load / tl);
                    for (int k = 0; k < 3; ++k) p[k] -= tg[k] * cut;
                }
            }
        }
    }

    void Sim::projectPlanes()
    {
        const size_t n = count();
        for (size_t i = 0; i < n; ++i)
        {
            if (!mHasPlane[i] || mPin[i]) continue;
            float* p = &mX[i * 3];
            const float* pl = &mPlane[i * 6];
            const float s = (p[0] - pl[0]) * pl[3] + (p[1] - pl[1]) * pl[4] + (p[2] - pl[2]) * pl[5];
            if (s >= mS.thickness) continue;
            const float d = mS.thickness - s;
            for (int k = 0; k < 3; ++k) p[k] += pl[3 + k] * d;
            mCorr[i] += d;
            stopInto((int)i, pl + 3);
        }
    }

    // <MG> the cloth's triangles by the cells they cover (the body's points and the cloth's own points look them up)
    void Sim::buildTriGrid()
    {
        mTriGrid.clear();
        const float cs = mTriCell;
        mTriSphere.resize(mT.size() * 4);
        for (size_t t = 0; t < mT.size(); ++t)
        {
            float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
            for (int q = 0; q < 3; ++q) for (int k = 0; k < 3; ++k) { const float v = mX[mT[t][q] * 3 + k]; lo[k] = std::min(lo[k], v); hi[k] = std::max(hi[k], v); }
            float* sp = &mTriSphere[t * 4];
            for (int k = 0; k < 3; ++k) sp[k] = (lo[k] + hi[k]) * 0.5f;
            sp[3] = 0.f;
            for (int q = 0; q < 3; ++q)
            {
                const float* v = &mX[mT[t][q] * 3];
                sp[3] = std::max(sp[3], (v[0] - sp[0]) * (v[0] - sp[0]) + (v[1] - sp[1]) * (v[1] - sp[1]) + (v[2] - sp[2]) * (v[2] - sp[2]));
            }
            sp[3] = std::sqrt(sp[3]);
            const long x0 = (long)std::floor(lo[0] / cs), x1 = (long)std::floor(hi[0] / cs), y0 = (long)std::floor(lo[1] / cs), y1 = (long)std::floor(hi[1] / cs), z0 = (long)std::floor(lo[2] / cs), z1 = (long)std::floor(hi[2] / cs);
            // torn apart (or not a number): not a surface to collide with. Per axis: the product of the
            // spans let a long flat spike through (one span 0), and it then filled thousands of cells
            if (!(x1 - x0 <= 6 && y1 - y0 <= 6 && z1 - z0 <= 6 && x0 <= x1 && y0 <= y1 && z0 <= z1)) continue;
            for (long x = x0; x <= x1; ++x) for (long y = y0; y <= y1; ++y) for (long z = z0; z <= z1; ++z) mTriGrid[key(x, y, z)].push_back((int)t);
        }
    }

    // <MG> the skin poking out between the cloth's points (a nipple, a shoulder point, a knuckle under a
    // coarse garment): each body point near the cloth pushes the cloth triangle over it outwards, its
    // three corners sharing the move by where the point sits on it
    void Sim::skinThrough()
    {
        const size_t n = count();
        const float reach = mS.thickness + 0.01f;
        std::vector<int> cand;
        cand.reserve(n * 2);
        ++mTick;
        // the body points near the cloth: each cloth point's nearest, and their neighbours
        std::vector<uint8_t> seen(mB.size() / 3, 0);
        for (size_t i = 0; i < n; ++i)
        {
            const int h = mHint[i];
            if (h < 0 || (size_t)h >= seen.size()) continue;
            if (!seen[h]) { seen[h] = 1; cand.push_back(h); }
            for (int u : mBodyNb[h]) if (!seen[u]) { seen[u] = 1; cand.push_back(u); }
        }
        const float cs = mTriCell;
        for (int b : cand)
        {
            const float* p = &mB[b * 3];
            const float* nb = mVN[b].data();
            ++mTriTick;
            int bestT = -1;
            float bestD = reach * reach, bc[3] = { 0, 0, 0 };
            const long x0 = (long)std::floor((p[0] - reach) / cs), x1 = (long)std::floor((p[0] + reach) / cs);
            const long y0 = (long)std::floor((p[1] - reach) / cs), y1 = (long)std::floor((p[1] + reach) / cs);
            const long z0 = (long)std::floor((p[2] - reach) / cs), z1 = (long)std::floor((p[2] + reach) / cs);
            for (long x = x0; x <= x1; ++x) for (long y = y0; y <= y1; ++y) for (long z = z0; z <= z1; ++z)
            {
                auto it = mTriGrid.find(key(x, y, z));
                if (it == mTriGrid.end()) continue;
                for (int t : it->second)
                {
                    if (mTriStamp[t] == mTriTick) continue;
                    mTriStamp[t] = mTriTick;
                    {
                        const float* sp = &mTriSphere[t * 4];
                        const float dx = p[0] - sp[0], dy = p[1] - sp[1], dz = p[2] - sp[2], rr = sp[3] + reach;
                        if (dx * dx + dy * dy + dz * dz > rr * rr) continue;
                    }
                    float c[3];
                    closest(p, &mX[mT[t][0] * 3], &mX[mT[t][1] * 3], &mX[mT[t][2] * 3], c);
                    const float d2 = (c[0] - p[0]) * (c[0] - p[0]) + (c[1] - p[1]) * (c[1] - p[1]) + (c[2] - p[2]) * (c[2] - p[2]);
                    if (d2 < bestD) { bestD = d2; bestT = t; bc[0] = c[0]; bc[1] = c[1]; bc[2] = c[2]; }
                }
            }
            if (bestT < 0) continue;
            const float rel[3] = { bc[0] - p[0], bc[1] - p[1], bc[2] - p[2] };
            const float s = dot3(rel, nb);
            if (s >= mS.thickness) continue;
            // only a point beside the cloth (not one far under it: the cloth points handle those)
            if (s < -0.02f) continue;
            // where the point sits on the triangle (barycentric)
            const auto& t = mT[bestT];
            const float* a = &mX[t[0] * 3]; const float* bb = &mX[t[1] * 3]; const float* cc = &mX[t[2] * 3];
            const float v0[3] = { bb[0] - a[0], bb[1] - a[1], bb[2] - a[2] }, v1[3] = { cc[0] - a[0], cc[1] - a[1], cc[2] - a[2] }, v2[3] = { bc[0] - a[0], bc[1] - a[1], bc[2] - a[2] };
            const float d00 = dot3(v0, v0), d01 = dot3(v0, v1), d11 = dot3(v1, v1), d20 = dot3(v2, v0), d21 = dot3(v2, v1);
            const float den = d00 * d11 - d01 * d01;
            if (std::fabs(den) < 1e-20f) continue;
            float bw[3];
            bw[1] = (d11 * d20 - d01 * d21) / den; bw[2] = (d00 * d21 - d01 * d20) / den; bw[0] = 1.f - bw[1] - bw[2];
            float wsum = 0.f;
            for (int k = 0; k < 3; ++k) { bw[k] = std::max(0.f, bw[k]); wsum += bw[k] * bw[k] * w(t[k]); }
            // <MG> the triangle can hardly move there (the skin point sits on a pinned corner, the free
            // corners barely share it): the push divided by that tiny share threw a corner metres away.
            // Then it is left alone; and no corner moves more than the push itself
            if (wsum < 0.04f) continue;
            const float push = std::min(0.008f, mS.thickness - s);
            for (int k = 0; k < 3; ++k)
            {
                const float f = std::min(push, push * bw[k] * w(t[k]) / wsum);
                for (int q = 0; q < 3; ++q) mX[t[k] * 3 + q] += nb[q] * f;
                if (f > 0.f && (size_t)t[k] < mTouch.size()) { mTouch[t[k]] = 1; for (int q = 0; q < 3; ++q) mCN[t[k] * 3 + q] = nb[q]; stopInto(t[k], nb); }
            }
        }
    }

    void Sim::step(float dt)
    {
        const size_t n = count();
        if (!n) return;
        const float g = -9.81f * mS.gravity;
        const float keep = 1.f - std::max(0.f, std::min(0.5f, mS.damping));
        // <MG> small steps (Macklin et al. 2019): the step cut into sub-steps of one pass each, not one
        // step of many passes. A big mesh then keeps its length under gravity (with one step and 10
        // passes a coat stretched to the floor: "hold" used to hide it by pulling it back to its model)
        const int ns = std::max(1, std::min(40, mS.iterations));
        const float h = dt / (float)ns;
        const float keepS = std::pow(keep, 1.f / (float)ns);
        for (size_t i = 0; i < n; ++i)
        {
            for (int k = 0; k < 3; ++k) mPrev[i * 3 + k] = mX[i * 3 + k];
            if (mPin[i]) for (int k = 0; k < 3; ++k) mX[i * 3 + k] = mPinAt[i * 3 + k];
        }
        // XPBD compliance (m/N, per unit mass): the fabric's stretch and bend in physical terms, the same
        // whatever the number of sub-steps or the mesh's density. Cotton (stretch 0.959) gives ~1 % under
        // its own weight, the Stretch fabric (0.85) ~5 %; bend: cotton 0.09, leather 0.42, silk 0.018.
        const float st = std::max(0.5f, std::min(0.999f, mS.stretch));
        const float alphaS = 1.5e-7f * (1.f - st) / 0.041f;
        const float alphaB = 1e-4f * 0.09f / std::max(0.003f, mS.bend);
        const float kS = alphaS / (h * h);   // the compliances over h^2 (XPBD's alpha tilde)
        const float kB = alphaB / (h * h);
        std::fill(mCorr.begin(), mCorr.end(), 0.f);
        std::fill(mHasPlane.begin(), mHasPlane.end(), (uint8_t)0);
        // the body first (where each point meets the skin this step), then its plane in every sub-step:
        // the points the garment hangs from are not dragged into the body by the rest of it
        collide(false);
        mSub.resize(n * 3);
        const float maxMove = 0.02f / (float)ns;   // 2 cm per step at most
        for (int s = 0; s < ns; ++s)
        {
            for (size_t i = 0; i < n; ++i)
            {
                for (int k = 0; k < 3; ++k) mSub[i * 3 + k] = mX[i * 3 + k];
                if (mPin[i]) continue;
                mV[i * 3 + 2] += g * h;
                float mv[3], m2 = 0.f;
                for (int k = 0; k < 3; ++k) { mV[i * 3 + k] *= keepS; mv[k] = mV[i * 3 + k] * h; m2 += mv[k] * mv[k]; }
                if (m2 > maxMove * maxMove) { const float sc = maxMove / std::sqrt(m2); for (int k = 0; k < 3; ++k) mv[k] *= sc; }
                for (int k = 0; k < 3; ++k) mX[i * 3 + k] += mv[k];
            }
            solve(mEdges, kS);
            if (mS.bend > 0.f) solve(mBends, kB);
            for (const Grab& gr : mGrabs)
                for (int k = 0; k < 3; ++k) mX[gr.i * 3 + k] += (gr.t[k] - mX[gr.i * 3 + k]) * gr.w;
            projectPlanes();
            for (size_t i = 0; i < n; ++i)
            {
                if (mPin[i]) { for (int k = 0; k < 3; ++k) mV[i * 3 + k] = 0.f; continue; }
                for (int k = 0; k < 3; ++k) mV[i * 3 + k] = (mX[i * 3 + k] - mSub[i * 3 + k]) / h;
                if (mHasPlane[i])
                {
                    // resting on its plane: no speed away from it (inelastic)
                    const float* pl = &mPlane[i * 6];
                    const float* p = &mX[i * 3];
                    const float sd = (p[0] - pl[0]) * pl[3] + (p[1] - pl[1]) * pl[4] + (p[2] - pl[2]) * pl[5];
                    const float vn = mV[i * 3] * pl[3] + mV[i * 3 + 1] * pl[4] + mV[i * 3 + 2] * pl[5];
                    if (sd <= mS.thickness + 1e-4f && vn > 0.f) for (int k = 0; k < 3; ++k) mV[i * 3 + k] -= pl[3 + k] * vn;
                }
            }
        }
        // keep its shape: each point drawn back to the shape it keeps (loose parts still sag)
        const float hk = std::max(0.f, std::min(0.9f, mS.hold));
        if (hk > 0.f)
            for (size_t i = 0; i < n; ++i)
            {
                if (mPin[i]) continue;
                const float w = hk * mHoldW[i];
                for (int k = 0; k < 3; ++k) mX[i * 3 + k] += (mAnchor[i * 3 + k] - mX[i * 3 + k]) * w;
            }
        // never torn: no edge longer than the fabric allows (stiff cotton ~1.14x, stretch ~1.3x); before
        // the body's last word, so this cannot pull cloth back under the skin
        limitStretch(1.08f + (1.f - std::max(0.5f, std::min(0.999f, mS.stretch))) * 1.5f, 4);
        projectPlanes();
        collide(true);
        if (mBodyMoved > 0) --mBodyMoved;
        for (size_t i = 0; i < n; ++i)
        {
            for (int k = 0; k < 3; ++k) mV[i * 3 + k] = (mX[i * 3 + k] - mPrev[i * 3 + k]) / dt;
            // <MG> contact is inelastic: the skin's push does not become a speed away from it (else the
            // point bounces off and falls back for ever), and resting on it, no speed into it; the slide
            // along the skin is kept
            if (i < mTouch.size() && mTouch[i])
            {
                const float* cn = &mCN[i * 3];
                const float vn = mV[i * 3] * cn[0] + mV[i * 3 + 1] * cn[1] + mV[i * 3 + 2] * cn[2];
                for (int k = 0; k < 3; ++k) mV[i * 3 + k] -= cn[k] * vn;
            }
        }
        // <MG> then, positions only (taken as speed they make the cloth bounce for ever): the skin may not
        // poke between the cloth's points, the layers apart (after the body has placed them: two layers on
        // the same skin would sit at the same distance from it), and the skin's planes again
        buildTriGrid();
        skinThrough();
        if (mS.selfCollide) selfCollide();   // the same grid: the skin moved the cloth a few millimetres at most
        projectPlanes();
        // <MG> a safety net: no cloth moves 10 cm in 1/60 s; a point that did (or is not a number) goes
        // back where it was, still (whatever threw it, it is never thrown out of the scene again)
        for (size_t i = 0; i < n; ++i)
        {
            float* p = &mX[i * 3];
            const float* q = &mPrev[i * 3];
            const float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2], d2 = dx * dx + dy * dy + dz * dz;
            if (std::isfinite(d2) && d2 <= 0.1f * 0.1f) continue;
            if (mPin[i]) continue;   // a pin is where it was put
            for (int k = 0; k < 3; ++k) { p[k] = q[k]; mV[i * 3 + k] = 0.f; }
        }
        // how much it still moves (settled when small): the speeds after every contact
        float sum = 0.f;
        for (size_t i = 0; i < n; ++i) sum += std::sqrt(mV[i * 3] * mV[i * 3] + mV[i * 3 + 1] * mV[i * 3 + 1] + mV[i * 3 + 2] * mV[i * 3 + 2]);
        mMotion = sum / (float)n;
    }


    void Sim::setBody(const std::vector<float>& bodyPositions, const std::vector<std::array<int, 3>>& bodyTriangles)
    {
        // same triangles, same points: only where they are changed (a pose, an animation frame)
        const bool same = !mBT.empty() && bodyTriangles.size() == mBT.size() && bodyPositions.size() == mB.size()
                          && std::equal(bodyTriangles.begin(), bodyTriangles.end(), mBT.begin());
        mB = bodyPositions;
        const size_t bn = mB.size() / 3;
        if (!mX.empty()) mBodyMoved = 40;   // the body just moved: cloth it now overlaps may be deeper
        if (!same)
        {
            mBT = bodyTriangles;
            mBodyTris.assign(bn, {});
            for (size_t i = 0; i < mBT.size(); ++i)
            {
                const auto& t = mBT[i];
                if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= (int)bn || t[1] >= (int)bn || t[2] >= (int)bn) continue;
                for (int k = 0; k < 3; ++k) mBodyTris[t[k]].push_back((int)i);
            }
            mBodyNb.assign(bn, {});
            for (const auto& t : mBT)
                for (int k = 0; k < 3; ++k)
                    if (t[k] >= 0 && t[k] < (int)bn && t[(k + 1) % 3] >= 0 && t[(k + 1) % 3] < (int)bn) { mBodyNb[t[k]].push_back(t[(k + 1) % 3]); mBodyNb[t[(k + 1) % 3]].push_back(t[k]); }
            for (auto& l : mBodyNb) { std::sort(l.begin(), l.end()); l.erase(std::unique(l.begin(), l.end()), l.end()); }
            std::fill(mHint.begin(), mHint.end(), -1);
            mStamp.assign(mBT.size(), 0);
            mTick = 0;
        }
        // triangle normals (outward) and smooth point normals
        mBN.assign(mBT.size(), { 0.f, 0.f, 0.f });
        mVN.assign(bn, { 0.f, 0.f, 0.f });
        for (size_t i = 0; i < mBT.size(); ++i)
        {
            const auto& t = mBT[i];
            if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= (int)bn || t[1] >= (int)bn || t[2] >= (int)bn) continue;
            const float* a = &mB[t[0] * 3]; const float* b = &mB[t[1] * 3]; const float* cc = &mB[t[2] * 3];
            const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { cc[0] - a[0], cc[1] - a[1], cc[2] - a[2] };
            const float nn[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };   // length = 2 x area
            const float l = std::sqrt(dot3(nn, nn));
            if (l < 1e-12f) continue;
            for (int k = 0; k < 3; ++k) mBN[i][k] = nn[k] / l;
            for (int k = 0; k < 3; ++k) for (int q = 0; q < 3; ++q) mVN[t[k]][q] += nn[q];
        }
        for (auto& v : mVN) { const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-12f) for (int q = 0; q < 3; ++q) v[q] /= l; }
        // a grid of its points
        mGrid.clear();
        mCell = 0.03f;
        for (size_t v = 0; v < bn; ++v)
            if (v < mBodyTris.size() && !mBodyTris[v].empty())
                mGrid[key(cell(mB[v * 3]), cell(mB[v * 3 + 1]), cell(mB[v * 3 + 2]))].push_back((int)v);
    }

    void Sim::carry(const std::vector<float>& mats)
    {
        const size_t n = count();
        if (mats.size() < mRep.size() * 16 || mFirst.size() < n) return;   // one matrix per given point
        auto apply = [](const float* M, float* p)
        {
            const float x = p[0], y = p[1], z = p[2];
            p[0] = M[0] * x + M[4] * y + M[8] * z + M[12];
            p[1] = M[1] * x + M[5] * y + M[9] * z + M[13];
            p[2] = M[2] * x + M[6] * y + M[10] * z + M[14];
        };
        for (size_t i = 0; i < n; ++i)
        {
            const float* M = &mats[(size_t)mFirst[i] * 16];   // a welded point moves as its first given point
            apply(M, &mAnchor[i * 3]);
            apply(M, &mPinAt[i * 3]);
            for (auto& g : mGrabs) if (g.i == (int)i) apply(M, g.t);
        }
    }

    void Sim::relax(int i, float amount)
    {
        i = ip(i);
        if (i < 0 || i >= (int)count() || mPin[i] || mPointEdges[i].empty()) return;
        float m[3] = { 0.f, 0.f, 0.f };
        for (int e : mPointEdges[i])
        {
            const int o = mEdges[e].a == i ? mEdges[e].b : mEdges[e].a;
            for (int k = 0; k < 3; ++k) m[k] += mX[o * 3 + k];
        }
        const float inv = 1.f / (float)mPointEdges[i].size();
        const float a = std::max(0.f, std::min(1.f, amount));
        for (int k = 0; k < 3; ++k) mX[i * 3 + k] += (m[k] * inv - mX[i * 3 + k]) * a;
    }

    // points of the cloth that came together while it moved are pushed apart (points that were
    // that close in the shape it was made with are left: a seam, a fold, a layer as modelled)
    void Sim::selfCollide()
    {
        // <MG> each point against the cloth's triangles (not only its points: a point passes between two
        // points 1-2 cm apart, never through the triangle between them). The side a point keeps is the
        // side it was on in the shape as made (the anchor): two layers stay in their order.
        mSelfContacts = 0;
        const size_t n = count();
        if (!n || mT.empty()) return;
        const float r = std::max(0.004f, mS.thickness * 1.5f);
        const float cs = mTriCell;
        auto triNormal = [](const float* a, const float* b, const float* c, float* nn)
        {
            const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            nn[0] = e1[1] * e2[2] - e1[2] * e2[1]; nn[1] = e1[2] * e2[0] - e1[0] * e2[2]; nn[2] = e1[0] * e2[1] - e1[1] * e2[0];
            const float l = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
            if (l < 1e-12f) return false;
            for (int k = 0; k < 3; ++k) nn[k] /= l;
            return true;
        };
        for (size_t i = 0; i < n; ++i)
        {
            float* p = &mX[i * 3];
            const long x0 = (long)std::floor((p[0] - r) / cs), x1 = (long)std::floor((p[0] + r) / cs);
            const long y0 = (long)std::floor((p[1] - r) / cs), y1 = (long)std::floor((p[1] + r) / cs);
            const long z0 = (long)std::floor((p[2] - r) / cs), z1 = (long)std::floor((p[2] + r) / cs);
            ++mTriTick;
            for (long x = x0; x <= x1; ++x) for (long y = y0; y <= y1; ++y) for (long z = z0; z <= z1; ++z)
            {
                auto it = mTriGrid.find(key(x, y, z));
                if (it == mTriGrid.end()) continue;
                for (int ti : it->second)
                {
                    if (mTriStamp[ti] == mTriTick) continue;
                    mTriStamp[ti] = mTriTick;
                    const auto& t = mT[ti];
                    {
                        const float* sp = &mTriSphere[ti * 4];
                        const float dx = p[0] - sp[0], dy = p[1] - sp[1], dz = p[2] - sp[2], rr = sp[3] + r;
                        if (dx * dx + dy * dy + dz * dz > rr * rr) continue;
                    }
                    if (neighbours((int)i, t)) continue;   // its own triangles and its neighbours': the fabric itself
                    const float* a = &mX[t[0] * 3]; const float* b = &mX[t[1] * 3]; const float* c = &mX[t[2] * 3];
                    float cp[3];
                    closest(p, a, b, c, cp);
                    const float d[3] = { p[0] - cp[0], p[1] - cp[1], p[2] - cp[2] };
                    if (dot3(d, d) >= r * r) continue;
                    // as made: which side of that layer the point was on, and how far
                    const float* A = &mAnchor[i * 3];
                    const float* a0 = &mAnchor[t[0] * 3]; const float* b0 = &mAnchor[t[1] * 3]; const float* c0 = &mAnchor[t[2] * 3];
                    float cp0[3], n0[3], nn[3];
                    closest(A, a0, b0, c0, cp0);
                    const float d0[3] = { A[0] - cp0[0], A[1] - cp0[1], A[2] - cp0[2] };
                    const float l0 = std::sqrt(dot3(d0, d0));
                    if (l0 < 0.0005f) continue;   // on it as made (a seam): no side to keep
                    if (!triNormal(a0, b0, c0, n0) || !triNormal(a, b, c, nn)) continue;
                    // a layer is OVER the triangle; a point beside it on the same fabric (in its plane) has no
                    // side to keep: pushing it "out" made bumps every step
                    if (std::fabs(dot3(d0, n0)) < 0.7f * l0) continue;
                    const float side = dot3(d0, n0) >= 0.f ? 1.f : -1.f;
                    const float s = dot3(d, nn) * side;
                    // the gap to keep: the fabric's, but never more than most of the gap it was made with (a
                    // shell's two sides 3.5 mm apart are not pushed to 4.5 mm: that fought the garment every step)
                    const float keepGap = std::min(r, 0.8f * std::min(mGap[i], l0));
                    if (s >= keepGap) continue;
                    // the layer on the skin stays; else both move, by their masses
                    const bool tp = i < mTouch.size() && mTouch[i];
                    const bool tt = mTouch[t[0]] && mTouch[t[1]] && mTouch[t[2]];
                    const float wp = (tp && !tt) ? 0.f : w((int)i);
                    float bw[3];
                    {
                        const float v0[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v1[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] }, v2[3] = { cp[0] - a[0], cp[1] - a[1], cp[2] - a[2] };
                        const float e00 = dot3(v0, v0), e01 = dot3(v0, v1), e11 = dot3(v1, v1), e20 = dot3(v2, v0), e21 = dot3(v2, v1);
                        const float den = e00 * e11 - e01 * e01;
                        if (std::fabs(den) < 1e-20f) continue;
                        bw[1] = std::max(0.f, (e11 * e20 - e01 * e21) / den); bw[2] = std::max(0.f, (e00 * e21 - e01 * e20) / den); bw[0] = std::max(0.f, 1.f - bw[1] - bw[2]);
                    }
                    float wt[3], wsum = wp;
                    for (int k = 0; k < 3; ++k) { wt[k] = (tt && !tp) ? 0.f : w(t[k]); wsum += bw[k] * bw[k] * wt[k]; }
                    // <MG> hardly movable here (pinned corners, the point on the skin): left alone, else a tiny
                    // share divided the push and threw a corner metres away; no part moves more than the push
                    if (wsum < 0.04f) continue;
                    const float need = std::min(keepGap - s, 0.006f);
                    const float lam = need / wsum;
                    const float mp = std::min(need, lam * wp);
                    for (int q = 0; q < 3; ++q) p[q] += nn[q] * side * mp;
                    for (int k = 0; k < 3; ++k)
                    {
                        const float mk = std::min(need, lam * wt[k] * bw[k]);
                        for (int q = 0; q < 3; ++q) mX[t[k] * 3 + q] -= nn[q] * side * mk;
                    }
                    // resting on each other: neither moves on into the other
                    if (wp > 0.f) stopInto((int)i, nn, side);
                    for (int k = 0; k < 3; ++k) if (wt[k] > 0.f && bw[k] > 0.f) stopInto(t[k], nn, -side);
                    ++mSelfContacts;
                }
            }
        }
    }
    void Sim::limitStretch(float maxRatio, int passes)
    {
        for (int pass = 0; pass < passes; ++pass)
        {
            bool any = false;
            for (const Edge& e : mEdges)
            {
                const float wa = w(e.a), wb = w(e.b);
                if (wa + wb == 0.f) continue;
                float* a = &mX[e.a * 3];
                float* b = &mX[e.b * 3];
                const float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
                const float l = std::sqrt(dot3(d, d));
                const float lim = e.rest * maxRatio;
                if (l <= lim || l < 1e-9f) continue;
                any = true;
                const float c = (l - lim) / (l * (wa + wb));
                for (int q = 0; q < 3; ++q) { a[q] += wa * c * d[q]; b[q] -= wb * c * d[q]; }
            }
            if (!any) break;
        }
    }

    void Sim::pin(int i, bool on)
    {
        i = ip(i);
        if (i < 0 || i >= (int)count()) return;
        mPin[i] = on ? 1 : 0;
        if (on) for (int k = 0; k < 3; ++k) { mPinAt[i * 3 + k] = mX[i * 3 + k]; mV[i * 3 + k] = 0.f; }
    }
    void Sim::clearPins() { std::fill(mPin.begin(), mPin.end(), (uint8_t)0); }
    void Sim::movePin(int i, float x, float y, float z)
    {
        i = ip(i);
        if (i < 0 || i >= (int)count() || !mPin[i]) return;
        mPinAt[i * 3] = x; mPinAt[i * 3 + 1] = y; mPinAt[i * 3 + 2] = z;
    }

    void Sim::grab(int i, float x, float y, float z, float weight)
    {
        i = ip(i);
        if (i < 0 || i >= (int)count()) return;
        for (auto& g : mGrabs) if (g.i == i) { g.t[0] = x; g.t[1] = y; g.t[2] = z; g.w = weight; return; }
        Grab g;
        g.i = i; g.t[0] = x; g.t[1] = y; g.t[2] = z; g.w = weight;
        mGrabs.push_back(g);
    }
    void Sim::releaseGrabs() { mGrabs.clear(); }

    void Sim::setFree(int i, bool free) { i = ip(i); if (i >= 0 && i < (int)count()) mHoldW[i] = free ? 0.f : 1.f; }
    void Sim::rebase(int i)
    {
        i = ip(i);
        if (i < 0 || i >= (int)count()) return;
        for (int k = 0; k < 3; ++k) mAnchor[i * 3 + k] = mX[i * 3 + k];
        mHoldW[i] = 1.f;
    }
    void Sim::rebaseAll() { mAnchor = mX; std::fill(mHoldW.begin(), mHoldW.end(), 1.f); }

    void Sim::scaleRest(int point, float factor)
    {
        point = ip(point);
        if (point < 0 || point >= (int)count()) return;
        for (int e : mPointEdges[point])
        {
            Edge& ed = mEdges[e];
            // between 70 % and 130 % of the length it was made with
            ed.rest = std::max(ed.rest0 * 0.7f, std::min(ed.rest0 * 1.3f, ed.rest * factor));
        }
    }
}
