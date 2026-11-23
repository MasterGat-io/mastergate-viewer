/**
 * @file mgcreatorrefit.h
 * @brief MasterGate Viewer: Creator Lab's refit of a garment from one body to another
 *        (no shared "refit base" needed).
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

#ifndef MG_CREATORREFIT_H
#define MG_CREATORREFIT_H

#include <array>
#include <map>
#include <string>
#include <vector>

#include "mgcreatordae.h"

// A garment made on body A (its devkit) gets a version for body B (another devkit).
// Body devkits are worn with their joint positions, so each file shows its body as it is
// in-world, each on its own skeleton:
//  1. per bone, B's skin is lined up with A's: the joint offset is only a first guess,
//     then the skin decides (a body whose chest bone sits 14 cm higher than its chest
//     must not drag the garment up)
//  2. a garment point near A's skin finds the matching spot of B's skin (same bone only,
//     so a left thigh never lands on the right one) and keeps its distance from it;
//     points far from the body (a hem) follow their nearest skin spot without the
//     distance rule
//  3. the moves are smoothed over the garment's own triangles
//  4. the version takes B's inverse binds, so B's skeleton draws it where it was placed,
//     uploaded with joint positions or not
// Standard C++ only (tested outside the viewer).
namespace MGCreatorRefit
{
    struct Options
    {
        double reach = 0.08;        // a garment point looks for the body this far (m)
        double farReach = 0.30;     // ... and, far from it, for a skin spot to follow (m)
        double nearSkin = 0.03;     // within this, the distance to the skin is kept fully (m)
        double fade = 0.05;         // ... then it fades to "follow the spot" over this (m)
        double minGap = 0.0005;     // never closer to B's skin than this (m)
        int smoothPasses = 2;
    };

    struct Result
    {
        size_t points = 0;
        size_t surface = 0;         // points kept at their distance from B's skin
        size_t followed = 0;        // points that followed their nearest skin spot
        size_t bonesOnly = 0;       // points too far from any skin: moved with their bones
        double maxMove = 0.0;       // m
        double meanMove = 0.0;      // m
        std::map<std::string, std::array<double, 3>> boneShift;   // how far each bone's skin moved, A to B (m)
        std::string error;
    };

    // newPositions[mesh] = the garment's raw positions for body B (same layout as the file);
    // newInvBind[mesh][joint] = the inverse binds to write with them (B's where B has the bone)
    bool refit(const MGCreatorDae::File& garment, const MGCreatorDae::File& bodyA, const MGCreatorDae::File& bodyB,
               std::vector<std::vector<double>>& newPositions,
               std::vector<std::vector<std::array<double, 16>>>& newInvBind,
               Result& result, const Options& options = Options());

    // "pant.dae" + "Hexa Body.dae" -> "pant_for_Hexa_Body.dae" (next to the garment)
    std::string versionPathFor(const std::string& garmentPath, const std::string& bodyPath);
}

#endif // MG_CREATORREFIT_H
