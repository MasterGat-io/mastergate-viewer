/**
 * @file mgcreatorweights.h
 * @brief MasterGate Viewer: Creator Lab's weight tools for rigged garments
 *        (copy from a devkit, limit, smooth, fix unweighted, convert to fitted mesh).
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

#ifndef MG_CREATORWEIGHTS_H
#define MG_CREATORWEIGHTS_H

#include <array>
#include <string>
#include <utility>
#include <map>
#include <vector>

#include "mgcreatordae.h"

// Weight tools, the way Bento Buddy offers them in Blender, on a garment file read by
// MGCreatorDae. Everything happens in the file's bind space (SL's frame: X forward,
// Y left, Z up, meters). Standard C++ only (tested outside the viewer).
namespace MGCreatorWeights
{
    typedef std::vector<std::pair<int, double>> VertexWeights;   // (joint, weight)
    typedef std::array<double, 16> Mat;                          // COLLADA layout: row-major, column vectors

    struct Skin
    {
        std::vector<std::string> joints;
        std::vector<Mat> invBind;
        std::vector<VertexWeights> weights;
    };

    struct Stats
    {
        size_t changed = 0;      // vertices whose weights changed
        size_t over4 = 0;        // vertices that had more than 4 influences
        size_t unweighted = 0;   // vertices without weights (after the step)
        size_t tooFar = 0;       // devkit copy: vertices too far from the devkit (kept)
        double maxDist = 0.0;    // devkit copy: the farthest copied vertex (m)
        size_t added = 0;        // bones added to the garment
    };

    Skin fromMesh(const MGCreatorDae::SkinnedMesh& m);

    // keep the 4 strongest, drop tiny ones, normalize to 1
    Stats limit(Skin& s, int maxInfluences = 4, double minWeight = 0.001);

    // a vertex without weights takes its weighted neighbours' average
    Stats fixUnweighted(Skin& s, const std::vector<std::array<int, 3>>& triangles);

    // each pass blends every vertex's weights with its neighbours' (amount 0..1)
    Stats smooth(Skin& s, const std::vector<std::array<int, 3>>& triangles, int passes, double amount);

    // Second Life's collision volumes (the bones the shape sliders scale)
    struct CollisionVolume
    {
        std::string name, parent;
        double pos[3] = { 0, 0, 0 }, rot[3] = { 0, 0, 0 }, scale[3] = { 1, 1, 1 };
    };

    // each vertex takes the weights of the nearest point of the devkit's surface
    // (barycentric blend of that triangle's three vertices); bones the garment does not
    // have yet are added with the devkit's inverse binds, except collision volumes whose
    // parent bone the garment has: those are bound from that parent and SL's skeleton
    // (a devkit's own volume places only hold when it is uploaded with joint positions).
    // Vertices farther than maxDist from the devkit keep their own weights.
    Stats copyFromDevkit(Skin& s, const std::vector<double>& positions, const Mat& bindShape,
                         const MGCreatorDae::File& devkit, double maxDist,
                         const std::vector<CollisionVolume>* volumes = nullptr);

    bool loadCollisionVolumes(const std::string& avatarSkeletonXml, std::vector<CollisionVolume>& out);


    // fitted mesh: a share of each bone's weight moves to the bone's nearest collision
    // volume, whose inverse bind is built from the parent bone's bind in this file and
    // the volume's default place in SL's skeleton
    Stats toFitted(Skin& s, const std::vector<double>& positions, const Mat& bindShape,
                   const std::vector<CollisionVolume>& volumes, double fraction);

    // matrix helpers (COLLADA layout)
    // A body in its own bind space (any size: some are built 10x and scaled by their binds)
    // brought to Second Life's skeleton as it stands: each point is carried by its bones from
    // the body's binds to SL's (bones: name -> position; volumes from their parent). The body
    // then has no bind shape and SL's own inverse binds. Weights on unknown bones are dropped.
    void toSkeletonSpace(MGCreatorDae::File& body, const std::map<std::string, std::array<double, 3>>& bones,
                         const std::vector<CollisionVolume>& volumes);
    Mat multiply(const Mat& a, const Mat& b);
    bool invert(const Mat& m, Mat& out);
    std::array<double, 3> transformPoint(const Mat& m, const double p[3]);
}

#endif // MG_CREATORWEIGHTS_H
