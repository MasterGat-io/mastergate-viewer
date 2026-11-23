/**
 * @file mgrigcloth.h
 * @brief MasterGate Viewer: Creator Lab, rigging clothes and accessories to SL's skeleton
 *        (Bento Buddy's Quick Skinning, in the viewer).
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

#ifndef MG_RIGCLOTH_H
#define MG_RIGCLOTH_H

#include <map>
#include <string>
#include <vector>

#include "mgcreatordae.h"
#include "mgcreatorweights.h"
#include "mgrig.h"

// A garment or accessory file (unrigged or rigged; dae, fbx, obj, glb) modeled on a body
// devkit becomes a rigged SL mesh, item by item:
//   skin  - weights from the devkit's nearest surface (Transfer Weights): reach, mix with the
//           item's own weights, smoothing; points out of reach (a hem) take their neighbours'
//   auto  - no body: weights from the distance to the skeleton's bone segments (automatic)
//   bone  - rides on one bone, rigid (an earring on the head, a watch on the wrist)
//   keep  - its own weights (already on SL bones)
//   skip  - left out
// Fitted mesh (shape sliders) as an option. The skeleton is the devkit's (its inverse binds),
// else SL's own. Standard C++ only.
namespace MGRigCloth
{
    struct Item
    {
        std::string name;
        size_t verts = 0;
        int materials = 0;
        bool rigged = false;      // has a skin in the file
        bool slBones = false;     // ... on SL bones
        double nearBody = -1.0;   // share of its points within 5 cm of the devkit (-1: no devkit)
        double size = 0.0;        // bounding box diagonal (m)
        std::string mode;         // suggested: skin, bone, keep
        std::string bone;         // suggested bone (the nearest to its centre)
    };
    void analyse(const MGRig::Model& g, const MGCreatorDae::File* devkit, const std::vector<MGRig::SlBone>& sl,
                 std::vector<Item>& items);

    struct Choice
    {
        std::string mode = "skin";
        std::string bone;
        double distance = 0.10;   // m: how far a point looks for the body
        double mix = 1.0;         // 1 = the body's weights only, 0 = the item's own
        int smooth = 1;
    };
    struct Options
    {
        bool fitted = false;      // collision volumes (shape sliders)
        double share = 1.0;
        // Place on the body, before rigging: the pieces moved (m, SL's frame: X forward, Y left,
        // Z up), turned about the vertical (degrees) and resized, about their common centre
        double move[3] = { 0.0, 0.0, 0.0 };
        double turn = 0.0;
        double scale = 1.0;
        // or the whole placement as one 4x4 (column-major, bind space): moved, turned on any axis,
        // resized per axis (the viewer's gizmo); used instead of move / turn / scale when set
        bool hasMatrix = false;
        double matrix[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        // the avatar posed to the garment (a garment made in an A pose, say): per bone, the move from
        // the rest pose to the pose, in bind space (COLLADA layout). The weights are found against the
        // posed skeleton / body, then each point is taken back to the rest pose with them.
        std::map<std::string, MGCreatorWeights::Mat> pose;
        bool unpose = true;       // false: the pose only guides the weights (the viewer then bakes what it shows)
        // <MG> the skeleton as it shows around the garment: per bone, its world matrix seen from the
        // garment's space (the preview's pelvis skinning)^-1 x world (COLLADA layout). When given, the
        // body is put where it shows through each of its OWN binds (a fitted body's volumes keep their
        // offset, turn and size) and the automatic weights measure from these joints; it replaces pose.
        std::map<std::string, MGCreatorWeights::Mat> shown;
    };
    struct Result
    {
        int skinned = 0, attached = 0, kept = 0, skipped = 0;
        size_t farPoints = 0;     // points out of reach that took their neighbours' weights
        int maxBones = 0;         // per mesh (SL: 110)
        std::vector<std::string> warnings;
    };
    // out: meshes with their own skins (Mesh::skinJoints / skinInvBind), joints = the SL
    // skeleton in use (for the file's joint nodes)
    // myBody: the body the user wears (built in memory by the viewer, never written), the
    // weight source of the pieces whose mode is "mybody"
    bool rig(const MGRig::Model& g, const std::vector<Choice>& choices, const MGCreatorDae::File* devkit,
             const std::vector<MGRig::SlBone>& sl, const std::vector<MGCreatorWeights::CollisionVolume>& cvs,
             const Options& o, MGRig::Model& out, Result& r, std::string& error,
             const MGCreatorDae::File* myBody = nullptr);

    // "C:/x/skirt.obj" -> "C:/x/skirt_rigged.dae"
    std::string outPathFor(const std::string& sourcePath);

    // The body to copy weights from, out of any rigged file: a devkit .dae as it is, or a
    // project file (.glb, .gltf, .fbx, .dae) holding the body rigged to SL bones among other
    // pieces: the mesh named "body" (else the largest one on SL bones). pieces = what was used.
    bool loadBody(const std::string& path, const std::vector<MGRig::SlBone>& sl, MGCreatorDae::File& out,
                  std::vector<std::string>& pieces, std::string& error);
}

#endif // MG_RIGCLOTH_H
