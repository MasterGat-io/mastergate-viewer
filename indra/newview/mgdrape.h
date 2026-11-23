/**
 * @file mgdrape.h
 * @brief MasterGate Viewer: Creator Lab's Drape, a cloth simulation that settles a garment on
 *        the avatar's body (position-based dynamics).
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

#ifndef MG_DRAPE_H
#define MG_DRAPE_H

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

// The garment's points are particles (mass by the cloth area each carries); its edges keep their
// length (stretch), the points across two triangles keep theirs more loosely (bend), both as XPBD
// compliances solved in small sub-steps (Macklin et al. 2019), so a big garment keeps its length
// under gravity; gravity pulls; the body is a solid the cloth cannot enter: each point's skin plane
// holds it in every sub-step, Coulomb friction holds it on a slope, the skin's own points may not
// poke between the cloth's points, and contact is inelastic. Layers (a shell's two sides, a coat
// over its lining) keep the side and most of the gap they were made with. Separate pieces that
// touch are sewn. "hold" (keep its shape) is optional shape memory; 0 = only the body holds it.
// Pins hold points, a grab pulls points to a target, tighten / loosen change the rest length of
// the edges painted. Everything in one space (the viewer: agent space, Z up, meters).
// Standard C++ only: the viewer runs it on a worker thread, on copies of its data.
namespace MGDrape
{
    struct Settings
    {
        float gravity = 1.0f;      // x 9.81 m/s2
        float stretch = 0.95f;     // 0 rubbery .. 1 does not stretch
        float bend = 0.15f;        // 0 limp .. 1 stiff
        float thickness = 0.003f;  // m kept between cloth and skin
        float friction = 0.35f;    // 0 slides .. 1 sticks where it touches
        float damping = 0.03f;     // per step
        float hold = 0.03f;        // keep its shape (per step, towards the model): 0 real cloth .. 0.5 firm
        int iterations = 10;       // sub-steps per step (one solver pass each)
        bool selfCollide = true;   // the cloth does not pass through itself
    };

    class Sim
    {
    public:
        // positions: x y z per point; triangles: point indices; body: x y z per point and its
        // triangles (outward order)
        bool init(const std::vector<float>& positions, const std::vector<std::array<int, 3>>& triangles,
                  const std::vector<float>& bodyPositions, const std::vector<std::array<int, 3>>& bodyTriangles,
                  const Settings& s);
        void setSettings(const Settings& s) { mS = s; }
        const Settings& settings() const { return mS; }

        void step(float dt);

        // pins (held where they are), grabs (pulled to a target), tighten / loosen (rest length x factor)
        void pin(int i, bool on);
        void clearPins();
        void movePin(int i, float x, float y, float z);   // a pinned point put elsewhere (it stays pinned)
        void grab(int i, float x, float y, float z, float weight);
        void releaseGrabs();
        void scaleRest(int point, float factor);   // the edges touching the point
        // the shape to keep: free = follows physics only (while painted / grabbed); rebase = keep where it is now
        void setFree(int i, bool free);
        void rebase(int i);
        void rebaseAll();
        // smooth: the point drawn towards its neighbours' middle (wrinkles even out)
        void relax(int i, float amount);
        // the body moved (a pose, an animation): its points now (same triangles: fast; else rebuilt)
        void setBody(const std::vector<float>& bodyPositions, const std::vector<std::array<int, 3>>& bodyTriangles);
        // the garment's skinning changed with the pose: each point's shape to keep and pin carried
        // by its own 4x4 (column-major, 16 floats per point)
        void carry(const std::vector<float>& mats);
        size_t selfContacts() const { return mSelfContacts; }

        // <MG> the points as given (the file's vertices): points at the same place share one cloth
        // point inside (a mesh exported with every face apart is one cloth); every index in this
        // interface is a given point's, and positions() / pins() are per given point
        size_t points() const { return mRep.size(); }
        const std::vector<float>& positions();
        const std::vector<uint8_t>& pins();
        size_t count() const { return mX.size() / 3; }   // the cloth points inside (welded)
        float motion() const { return mMotion; }   // mean speed (m/s) at the last step: settled when small
        size_t contacts() const { return mContacts; }
        // test hook: signed distance of point i to the body (false: no body within reach)
        bool probe(int i, float reach, float& signedDist) { i = ip(i); if (i < 0) return false; float c[3], n[3], d; if (!nearestBody(&mX[i * 3], reach, c, n, d)) return false; const float r[3] = { mX[i*3]-c[0], mX[i*3+1]-c[1], mX[i*3+2]-c[2] }; signedDist = r[0]*n[0]+r[1]*n[1]+r[2]*n[2]; return true; }
        size_t welded() const { return mRep.size() - count(); }   // given points merged into another

    private:
        std::vector<int> mRep;                     // given point -> its cloth point
        std::vector<int> mFirst;                   // cloth point -> its first given point
        std::vector<float> mXe;                    // positions per given point (positions())
        std::vector<uint8_t> mPinE;                // pins per given point (pins())
        int ip(int i) const { return (i >= 0 && (size_t)i < mRep.size()) ? mRep[i] : -1; }
        bool initWelded(const std::vector<float>& positions, const std::vector<std::array<int, 3>>& triangles,
                        const std::vector<float>& bodyPositions, const std::vector<std::array<int, 3>>& bodyTriangles, const Settings& s);
        struct Edge { int a, b; float rest, rest0; };
        Settings mS;
        std::vector<float> mX, mPrev, mV;
        std::vector<Edge> mEdges, mBends;
        std::vector<std::vector<int>> mPointEdges;
        std::vector<uint8_t> mPin;
        std::vector<float> mPinAt;
        std::vector<float> mAnchor;                // the shape it keeps
        std::vector<float> mHoldW;                 // 1 = kept, 0 = free
        struct Grab { int i; float t[3]; float w; };
        std::vector<Grab> mGrabs;
        // the body
        std::vector<float> mB;
        std::vector<std::array<int, 3>> mBT;
        std::vector<std::array<float, 3>> mBN;
        std::vector<std::array<float, 3>> mVN;       // body point normals (smooth, area-weighted)
        std::vector<std::vector<int>> mBodyTris;   // body point -> its triangles
        float mCell = 0.03f;
        std::unordered_map<uint64_t, std::vector<int>> mGrid;
        std::vector<uint32_t> mStamp;
        uint32_t mTick = 0;
        float mMotion = 0.f;
        size_t mContacts = 0;

        uint64_t key(long x, long y, long z) const;
        long cell(float v) const;
        // n: the nearest triangle's normal; smooth (optional): the body's smooth normal there
        bool nearestBody(const float p[3], float reach, float out[3], float n[3], float& dist, int* hint = nullptr, float* smooth = nullptr);
        // is p under the skin at c? (both normals must agree: in a fold of the skin the nearest
        // triangle can face away, and a point outside would be thrown through the body)
        bool underSkin(const float p[3], const float c[3], const float n[3], const float sm[3], float& depth) const;
        void limitStretch(float maxRatio, int passes);
        void selfCollide();
        size_t mSelfContacts = 0;
        int mBodyMoved = 0;                        // steps since the body last moved (a deeper push-out is allowed)
        std::vector<uint8_t> mTouch;               // the point lies on the skin (this step)
        std::unordered_map<uint64_t, std::vector<int>> mSelfGrid;
        std::vector<int> mHint;                    // cloth point -> its nearest body point last time
        std::vector<std::vector<int>> mBodyNb;     // body point -> its neighbours
        void solve(std::vector<Edge>& edges, float k);
        void collide(bool friction);
        // <MG> Drape v4 (like a cloth solver: the body alone holds the cloth up)
        std::vector<float> mW;                     // inverse mass (by the area each point carries; 0 pinned)
        std::vector<float> mCorr;                  // the normal push from the skin this step (the friction's load)
        std::vector<float> mCN;                    // the skin's normal where the point touched it this step (x y z)
        std::vector<float> mPlane;                 // per point: the skin's plane near it this step (point x y z, normal x y z)
        std::vector<uint8_t> mHasPlane;
        std::vector<float> mSub;                   // positions at the start of the sub-step
        std::vector<float> mGap;                   // per point: the gap to the nearest other layer as made (a shell's two sides)
        bool neighbours(int i, const std::array<int, 3>& t) const;   // t touches i or one of its neighbours
        void measureGaps();
    public:
        size_t sewn() const { return mSewn; }   // stitches added between the garment's separate pieces
    private:
        size_t mSewn = 0;
        // resting contact: no speed into what it rests on (the normal points away from it)
        void stopInto(int i, const float* n, float sign = 1.f)
        {
            float* v = &mV[i * 3];
            const float vn = (v[0] * n[0] + v[1] * n[1] + v[2] * n[2]) * sign;
            if (vn < 0.f) for (int k = 0; k < 3; ++k) v[k] -= n[k] * sign * vn;
        }
        void projectPlanes();                      // every pass: no point behind its plane (the skin carries the cloth)
        std::vector<std::array<int, 3>> mT;        // the cloth's triangles
        std::unordered_map<uint64_t, std::vector<int>> mTriGrid;
        float mTriCell = 0.03f;
        std::vector<float> mTriSphere;             // per triangle: centre x y z, radius (a quick reject)
        std::vector<uint32_t> mTriStamp;
        uint32_t mTriTick = 0;
        void buildTriGrid();
        void skinThrough();                        // the body's points may not poke between the cloth's points
        float w(int i) const { return mPin[i] ? 0.f : mW[i]; }
    };
}

#endif // MG_DRAPE_H
