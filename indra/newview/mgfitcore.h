/**
 * @file mgfitcore.h
 * @brief <MG> Creator Lab "Make it fit": the geometry. Standard C++ only (tested offline).
 *
 * Given a body (triangles + outward normals) and a garment (triangles), both in the same
 * space and pose, find where the garment must move out and by how much:
 *  - a garment point inside the body, or closer than the margin (nearest body TRIANGLE,
 *    not the nearest body point: coarse bodies gave wrong signs and missed points);
 *  - a body point poking through a garment face between the garment's points (the skin
 *    holes in sleeves: a coarse sleeve triangle cuts the forearm while its corners are
 *    outside); the face's corners are moved so the face clears the skin;
 *  - layers of a garment (a coat's 3–4 mm shell) move together, and each push spreads to
 *    the points around it in 3D, so the surface swells smoothly (no spikes) and split
 *    seams stay closed.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (C) 2026, MasterGate
 * $/LicenseInfo$
 */

#ifndef MG_FITCORE_H
#define MG_FITCORE_H

#include <array>
#include <cstdint>
#include <vector>

namespace MGFitCore
{
    struct V3
    {
        float x = 0.f, y = 0.f, z = 0.f;
        V3() = default;
        V3(float a, float b, float c) : x(a), y(b), z(c) {}
    };

    struct Body
    {
        std::vector<V3> pos;                    // the skin, posed
        std::vector<V3> nrm;                    // outward, unit (smooth, per point)
        std::vector<std::array<int, 3>> tris;
    };

    struct Garment
    {
        std::vector<V3> pos;                    // all of the garment's points, posed
        std::vector<std::array<int, 3>> tris;
        std::vector<uint8_t> fixed;             // 1 = never moved (protected); may be empty
    };

    struct Params
    {
        float margin = 0.003f;      // the gap kept between skin and garment (m)
        float maxPush = 0.02f;      // deeper clipping is left to the creator (m)
        float plateau = 0.005f;     // points this close to a push get all of it (garment layers)
        float radius = 0.016f;      // ... and it fades out to nothing at this distance
        float shell = 0.008f;       // garment layers within this distance of the deepest one move with it
        float deepAt = 0.f;         // > 0: also count what is found deeper than this (Result::overDeep)
    };

    struct Result
    {
        std::vector<V3> push;       // per garment point: its move (zero = none)
        std::vector<V3> need;       // ... before spreading to the neighbours (what the point itself needed)
        std::vector<float> sdist;   // per garment point: signed distance to the skin (< 0 inside; 1e9 = none near)
        std::vector<uint8_t> src;   // what set its move: 1 inside the skin, 2 skin through a face, 4 spread from a neighbour
        int inside = 0;             // garment points inside the body / closer than the margin
        int pokes = 0;              // body points through a garment face
        float deepest = 0.f;        // the largest push needed (m)
        int thinParts = 0;          // worn pieces left out: thin and open (straps, ribbons, tags) or tiny
        int overDeep = 0;           // found deeper than Params::deepAt
    };

    // one pass; the caller applies the pushes (in its own file space) and may run it again
    void solve(const Body& body, const Garment& garment, const Params& p, Result& out);
}

#endif
