/**
 * @file mgphoneposer.h
 * @brief MasterGate Viewer: the MG Phone Camera's posing panel, driving Firestorm's poser.
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

#ifndef MG_PHONEPOSER_H
#define MG_PHONEPOSER_H

#include "llsd.h"

// The Camera's own posing panel (a web panel over the viewfinder) on Firestorm's poser:
// the pose engine (FSPoserAnimator, FSPosingMotion), its per-joint axis settings and its
// pose files are those of the FS poser floater (fs_poser), built but never shown.
// Commands ("pose_*", my avatar):
//   pose_state   {}                           posing?, the joints (name, label, group, mirror,
//                                             posed, changed), my pose files and hand presets
//   pose_start / pose_stop
//   pose_get     {joint, frame}               rotation (yaw, pitch, roll, degrees), position, scale
//   pose_rot     {joint, rot, style, frame}   style none | mirror | sym; frame bone | avatar | world | camera
//   pose_pos     {joint, pos, style}          pose_scale {joint, scale, style}
//   pose_reset   {joint} | {all}              pose_undo / pose_redo {joint}
//   pose_flip    {}                           the whole pose mirrored (left <-> right)
//   pose_sym     {dir: lr | rl}               one side copied to the other
//   pose_load    {name, part}                 part all | rot | face | hand_l | hand_r (hand presets)
//   pose_save    {name}   pose_delete {name}
//   pose_pick    {x, y}                       the joint nearest a click in the view (fractions)
//   pose_markers {on, sel, group}             joint dots over the avatar in the viewfinder
//   pose_preset  {name: t | a}                 the whole avatar in a T pose (the skeleton's rest) or an A pose
namespace MGPhonePoser
{
    bool isCommand(const std::string& cmd);
    LLSD runCommand(const std::string& cmd, const LLSD& args);

    // the joint dots (the Camera's guides view draws them, under the page)
    void drawMarkers();

    // every frame while the camera is open: the gizmo stays on the panel's joint
    void idle();

    // the camera closed: the dots off (the pose stays until the user stops posing)
    void restore();

    // <MG> Creator Lab: is my avatar posed by the poser now?
    bool isPosing();

    // <MG> Animation Studio: the pose as each joint's LOCAL rotation (quaternion x,y,z,w, the
    // rotation an .anim key holds) and local position, for the joint groups asked for
    // (body, hands, face, misc; the pelvis is in body): { joint: { r: [x,y,z,w], p: [x,y,z] } }
    LLSD capturePose(const LLSD& groups);
    // the same form back onto the poser's targets (snap: no 0.25 s blend, for scrubbing)
    S32 applyPose(const LLSD& pose, bool snap);
    // a ring of the poser's gizmo is being dragged
    bool isDragging();
    // the joint the rings are on
    std::string selectedJoint();
    // the poser's joints, for the page: [{ n, label, g, mirror, parent }]
    LLSD jointList();
}

#endif // MG_PHONEPOSER_H
