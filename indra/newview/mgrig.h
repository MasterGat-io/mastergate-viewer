/**
 * @file mgrig.h
 * @brief MasterGate Viewer: Creator Lab's rig converter (characters from DAZ, VRoid,
 *        Mixamo, Character Creator, Unreal... to Second Life's skeleton).
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

#ifndef MG_RIG_H
#define MG_RIG_H

#include <array>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

// The way Bento Buddy converts a character (bone maps used with the author's permission),
// without Blender:
//   read (FBX, glTF / VRM) -> recognise the rig from its bone names (one of the maps)
//   -> stand it in Second Life's T-pose (arms and legs turned to SL's bone directions,
//   the mesh follows its own skin) -> weights renamed and merged onto SL bones (twist and
//   extra bones go to the bone they belong to, unknown bones to their nearest mapped
//   parent) -> SL skeleton placed on the character's joints (joint positions) or SL's
//   own -> a Z-up, meters COLLADA file the uploader and Local Mesh read.
// Standard C++ only, except the readers (ufbx, tinygltf); tested outside the viewer.
namespace MGRig
{
    typedef std::array<double, 16> Mat;   // row-major, column vectors (COLLADA layout)

    struct Joint
    {
        std::string name;
        int parent = -1;
        Mat bind{};          // the joint in the world at bind time (SL frame: X forward, Y left, Z up, m)
        bool weighted = false;
    };

    struct Mesh
    {
        std::string name;
        std::vector<double> pos, nrm, uv;                // per vertex: 3, 3, 2 (uv may be empty)
        std::vector<std::array<int, 3>> tris;
        std::vector<int> triMat;                         // per triangle: index into materials
        std::vector<std::string> materials;
        std::vector<std::vector<std::pair<int, double>>> weights;   // per vertex: (joint, weight)
        // a mesh's own skin (clothes rigged to SL bones): when set, weights index these
        // bones instead of Model::joints, with these inverse binds (as written to the file)
        std::vector<std::string> skinJoints;
        std::vector<Mat> skinInvBind;
        bool rigged = true;        // false: a static mesh (no skin in the file)
        size_t vertexCount() const { return pos.size() / 3; }
    };

    struct Model
    {
        std::vector<Joint> joints;
        std::vector<Mesh> meshes;
        std::string format;            // "fbx", "gltf"
        std::vector<std::string> notes;   // what the reader left out (static meshes...)
        std::string error;
    };

    // readers merge a face corner into an existing vertex when position, normal and uv are the
    // same by value (exporters write one normal index per corner: by index, nothing merges)
    struct CornerKey
    {
        std::array<long long, 6> k;
        bool operator==(const CornerKey& o) const { return k == o.k; }
    };
    struct CornerKeyHash
    {
        size_t operator()(const CornerKey& c) const
        {
            unsigned long long h = 1469598103934665603ULL;
            for (long long v : c.k) { h ^= (unsigned long long)v; h *= 1099511628211ULL; }
            return (size_t)h;
        }
    };
    inline CornerKey cornerKey(long long position, double nx, double ny, double nz, double u, double v)
    {
        auto q = [](double x, double s) { return (long long)std::floor(x * s + 0.5); };
        return CornerKey{ { position, q(nx, 1e4), q(ny, 1e4), q(nz, 1e4), q(u, 1e5), q(v, 1e5) } };
    }

    // readers (positions at bind time, in SL's frame and meters). keepStatic: meshes without a
    // skin stay (rigged = false, no weights, placed by their node) instead of being left out
    bool readFbx(const std::string& path, Model& out, bool keepStatic = false);   // also .obj (ufbx)
    bool readGltf(const std::string& path, Model& out, bool keepStatic = false);
    bool readDae(const std::string& path, Model& out, bool keepStatic = false);   // COLLADA (Z up as written: SL's frame)
    bool readModel(const std::string& path, Model& out, bool keepStatic = false);   // by extension

    // bone maps: "#map name", "r source target", "k target-source extra" (tab separated)
    struct BoneMap
    {
        std::string name;
        std::map<std::string, std::string> rename;               // source bone -> SL bone
        std::map<std::string, std::vector<std::string>> reskin;  // mapped source bone -> extra bones merged into it
    };
    bool loadMaps(const std::string& path, std::vector<BoneMap>& out);

    // the bone name without a namespace ("mixamorig:Hips" -> "Hips")
    std::string plainName(const std::string& n);

    struct Detection
    {
        int map = -1;
        std::string name;
        int matched = 0;        // weighted bones the map knows
        int weighted = 0;       // weighted bones in the file
        double score = 0.0;     // matched / weighted
        int renamed = 0;        // of them, renamed by the map itself (the rest merged)
    };
    Detection detect(const Model& m, const std::vector<BoneMap>& maps);

    // SL's skeleton (avatar_skeleton.xml): bones and collision volumes
    struct SlBone { std::string name; int parent = -1; double local[3] = { 0, 0, 0 }; double world[3] = { 0, 0, 0 }; bool volume = false; };
    bool loadSlSkeleton(const std::string& avatarSkeletonXml, std::vector<SlBone>& out);

    struct Options
    {
        bool tpose = true;            // turn arms and legs to SL's T-pose
        bool jointPositions = true;   // SL bones on the character's joints (upload with joint positions)
        int maxMaterials = 8;         // SL: 8 materials per mesh (bigger meshes are split)
    };

    struct Report
    {
        std::string map;
        int bonesIn = 0, bonesOut = 0;
        int mapped = 0, merged = 0;
        std::vector<std::string> fallback;   // weighted bones with no SL match (weights went to a parent)
        std::vector<std::string> leftOut;    // meshes on bones the map does not know (not converted)
        int tposed = 0;                      // limbs turned
        double maxTurnDeg = 0.0;
        int meshesIn = 0, meshesOut = 0;
        int maxBonesPerMesh = 0;             // SL: 110
        size_t vertices = 0, triangles = 0;
        double height = 0.0;                 // m
        double pelvisZ = 0.0;
        std::vector<std::string> warnings;
    };

    // in: a read model and its map -> out: the SL model (joints = SL bones, SL parents)
    bool convert(const Model& in, const BoneMap& map, const std::vector<SlBone>& sl, const Options& o,
                 Model& out, Report& report);

    // a Z-up, meters COLLADA file with the SL skeleton (joint nodes) and one skinned
    // geometry per mesh
    bool writeDae(const Model& sl, const std::string& path, std::string& error);

    // "C:/x/judy2.fbx" -> "C:/x/judy2_sl.dae"
    std::string outPathFor(const std::string& sourcePath);
}

#endif // MG_RIG_H
