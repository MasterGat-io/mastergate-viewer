/**
 * @file mgphonecreator.h
 * @brief MasterGate Viewer: the MG Phone "Creator Lab" app: a garment from Blender
 *        worn on your own avatar, checked before upload, stress-tested for clipping.
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

#ifndef MG_PHONECREATOR_H
#define MG_PHONECREATOR_H

#include "llsd.h"

// Creator Lab, on Firestorm's Local Mesh (vjlocalmesh): a .dae / .gltf file from the
// creator's disk is loaded, reloaded on every save, and shown on a worn "stand-in"
// object (a prim the creator wears; Local Mesh replaces its shape on this screen
// only). A rigged garment is then skinned on the creator's own avatar: their body,
// their shape. Nothing is uploaded and nobody else sees it.
//
//   lab_state                       the files opened here, the stand-in, the test
//   lab_open                        file picker (dae, gltf) -> "lab_file" events
//   lab_reload {file}               lab_close {file}
//   lab_autoreload {on}             Local Mesh's auto-reload (FSLocalMeshAutoReload)
//   lab_standins                    worn objects that can carry the file; stand-ins in the inventory
//   lab_standin_wear {item}         wear an inventory stand-in (added, nothing replaced)
//   lab_wear {file, standin, force} show the file on the stand-in (object i on prim i);
//                                   refused when the file has degenerate triangles unless force
//   lab_unwear                      the stand-in gets its own shape back
//   lab_checks {file}               upload checks: bones per mesh, weights per vertex,
//                                   unweighted vertices, thin triangles, materials, LODs, fitted mesh
//   lab_bodies                      the worn rigged objects the clip finder tests against
//   lab_body {item, on}             include / leave out one of them
//   lab_clip                        clip finder now: body points poking through the garment, by region
//   lab_test / lab_test_stop        stress test: stand, walk, ground sit, crouch, arms up,
//                                   heavy and thin shape ("lab_test" per step, "lab_test_done")
//   lab_hold {key}                  hold one test pose / shape to look closely; {key:""} lets go
//   lab_versions_add                file picker: other bodies' devkits (several) -> "lab_versions_targets"
//   lab_versions_remove {index}     lab_versions_list
//   lab_versions {weights, fitted, share}  the worn garment, made on the Weights devkit (body A),
//                                   refitted to each target: <garment>_for_<body>.dae, on a worker
//                                   thread ("lab_version" per body, "lab_versions_done"); never
//                                   written over a file it reads
//   lab_versions_cancel             lab_version_try {index}  a version on the stand-in
//   lab_convert_open {tpose, joint_positions}  picker: a character (fbx, glb, gltf, vrm) re-rigged
//                                   to SL's skeleton (worker thread) -> <name>_sl.dae next to it,
//                                   opened here ("lab_convert_start", "lab_convert")
//   lab_rig_open                    picker: a garment or accessory (dae, fbx, obj, glb) -> analysed on a
//                                   worker thread ("lab_rig_start", "lab_rig_analysis": items, suggestions)
//   lab_rig_run {items[{mode skin|bone|keep|skip, bone, distance_mm, mix, smooth}], fitted, share}
//                                   rigged to the devkit (or SL) skeleton -> <name>_rigged.dae, opened
//                                   and worn on the stand-in ("lab_rig_done"); lab_rig_cancel
// Events: lab_file, lab_test, lab_test_done, lab_versions_targets, lab_version, lab_versions_done.
//
// While a test or a hold runs, the AO is paused and the shape sliders are changed on
// this screen only (never saved); both come back when it ends or the phone closes.
namespace MGPhoneCreator
{
    bool isCommand(const std::string& cmd);
    LLSD runCommand(const std::string& cmd, const LLSD& args);

    // every frame while the phone draws: file loads / reloads, the stress test
    void idle();

    // the phone closed: the test or hold ends, the AO and the shape come back
    void onClose();
}

#endif // MG_PHONECREATOR_H
