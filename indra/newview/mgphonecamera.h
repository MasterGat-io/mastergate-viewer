/**
 * @file mgphonecamera.h
 * @brief MasterGate Viewer: the MG Phone Camera's pro capture controls.
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

#ifndef MG_PHONECAMERA_H
#define MG_PHONECAMERA_H

#include "llsd.h"

class LLImageRaw;

// The Camera app's capture console (raw/my-website/.../apps/camera/client.js)
// drives these through the MG Phone bridge ("cam_*" commands):
//   cam_state                               sizes, exposure, time of day, guides...
//   cam_guides   {aspect, grid}             framing guides over the 3D view
//   cam_exposure {ev}                       exposure in stops (RenderExposure)
//   cam_time     {hour}                     time of day for this viewer only (local sky)
//   cam_air      {mode}                     original | clear | hazy | misty
//   cam_env_reset                           back to the region's sky
//   cam_freeze   {on}                       freeze time (animations, sky, water)
//   cam_view     {preset, dist}             face | person | room | reset
//   cam_folder   {pick}                     the save folder (pick: native folder dialog)
//   cam_restore                             everything back as the user had it
// Nothing is saved to the user's settings except the save folder: closing the
// capture screen (or the phone) puts exposure, sky, freeze and guides back.
namespace MGPhoneCamera
{
    bool isCommand(const std::string& cmd);
    LLSD runCommand(const std::string& cmd, const LLSD& args);

    // undo every live change (capture screen or phone closed)
    void restore();

    // framing guides off while a shot that includes the interface is rendered
    void hideGuidesForShot(bool hide);

    // where "save to computer" writes (MGCameraFolder, else Pictures/MasterGate)
    std::string saveFolder();

    // write the shot; name pattern {date} {time} {n} {w} {h}; returns the path or ""
    std::string saveShot(LLImageRaw* raw, const std::string& format, const std::string& pattern);

    // grade the shot in place: look {preset, strength, contrast, highlights, shadows,
    // whites, blacks, grain, vignette, halation}. The Camera app runs the same math
    // on its live preview (gradeLook in apps/camera/client.js): keep them in step.
    void applyLook(LLImageRaw* raw, const LLSD& look);
}

#endif // MG_PHONECAMERA_H
