/**
 * @file mgcreatordae.h
 * @brief MasterGate Viewer: Creator Lab's COLLADA reader / writer for rigged garments.
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

#ifndef MG_CREATORDAE_H
#define MG_CREATORDAE_H

#include <array>
#include <string>
#include <utility>
#include <vector>

// "Make it fit" changes a garment the creator exported (Bento Buddy, Avastar, Blender)
// and saves it next to the original. The file is read as text and only the numbers
// that change are rewritten: materials, UVs, normals, bones and everything else stay
// byte for byte as the creator's tool wrote them. Standard C++ only (tested outside
// the viewer).
namespace MGCreatorDae
{
    struct SkinnedMesh
    {
        std::string geometryId;
        std::string name;

        // <float_array> of the POSITION source: where its text sits in the file
        size_t posStart = 0;
        size_t posEnd = 0;
        std::vector<double> positions;               // x y z per vertex, as in the file

        std::vector<std::array<int, 3>> triangles;   // vertex indices (polygons fanned)

        // the skin, as written in the file (COLLADA matrices: row-major, column vectors)
        std::array<double, 16> bindShape{};
        std::vector<std::string> joints;
        std::vector<std::array<double, 16>> invBind;
        std::vector<std::vector<std::pair<int, double>>> weights;   // per vertex: (joint, weight)

        // the joint names and inverse binds: where their text sits (rewritten when bones are added)
        size_t jStart = 0, jEnd = 0;
        size_t ibStart = 0, ibEnd = 0;
        // the WEIGHT <float_array>: where its text sits (rewritten when weights change)
        size_t wStart = 0;
        size_t wEnd = 0;
        size_t vcountStart = 0, vcountEnd = 0;       // <vcount> of <vertex_weights>
        size_t vStart = 0, vEnd = 0;                 // <v> of <vertex_weights>
        int jointOffset = 0, weightOffset = 1, wStride = 2;

        size_t vertexCount() const { return positions.size() / 3; }
    };

    struct File
    {
        std::string path;
        std::string text;
        std::string upAxis;     // "Z_UP", "Y_UP", "X_UP" ("" = COLLADA's default, Y_UP)
        double meter = 1.0;
        std::vector<SkinnedMesh> meshes;   // rigged geometries only
        std::string error;                 // why reading failed
    };

    // Reads every rigged geometry (a <controller><skin> on a <geometry> with a <mesh>).
    bool read(const std::string& path, File& out);

    // A mesh's new skin: bones (names + inverse binds, COLLADA layout) and weights per
    // vertex as indices into those bones. Empty members = unchanged.
    struct NewSkin
    {
        std::vector<std::string> joints;
        std::vector<std::array<double, 16>> invBind;
        std::vector<std::vector<std::pair<int, double>>> weights;
    };

    // Positions, plus bones and weights when skins[mesh] carries them.
    bool writeSkinned(const File& f,
                      const std::vector<std::vector<double>>& newPositions,
                      const std::vector<NewSkin>& skins,
                      const std::string& outPath, std::string& error);

    // Writes a copy of the file where each mesh's positions are replaced by newPositions
    // [mesh][3 * vertex] (same counts). Weights are rewritten too when newWeights is not
    // empty ([mesh][vertex] = list of (joint, weight)).
    bool write(const File& f,
               const std::vector<std::vector<double>>& newPositions,
               const std::vector<std::vector<std::vector<std::pair<int, double>>>>& newWeights,
               const std::string& outPath, std::string& error);

    // "pant.dae" -> "pant_fit.dae"; a "_fit" file stays itself (fitting again refines it)
    std::string fitPathFor(const std::string& path);
}

#endif // MG_CREATORDAE_H
