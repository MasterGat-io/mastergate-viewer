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
#include "llrect.h"
#include "v4color.h"
#include "llpointer.h"

class LLImageRaw;
class LLRenderTarget;
class LLVertexBuffer;

// The MG Phone Camera app's capture console
// drives these through the MG Phone bridge ("cam_*" commands):
//   cam_state                               sizes, exposure, time of day, guides...
//   cam_guides   {aspect, grid}             framing guides over the 3D view (grid: off,
//                                           thirds, golden, center, diagonals)
//   cam_exposure {ev}                       exposure in stops (RenderExposure)
//   cam_time     {hour}                     time of day for this viewer only (local sky)
//   cam_air      {mode}                     original | clear | hazy | misty
//   cam_env_reset                           back to the region's sky
//   cam_freeze   {on}                       freeze time (animations, sky, water)
//   cam_view     {preset, dist}             face | person | room | reset
//   cam_folder   {pick}                     the save folder (pick: native folder dialog)
//   cam_restore                             everything back as the user had it
// Camera Studio (full-window viewfinder):
//   cam_af       {x, y, mode, area, dist}   autofocus at a point of the window (fractions);
//                                           mode s (single) | c (continuous) | m (manual,
//                                           dist in m); area face | point; {clear} stops it
//   cam_lens     {focal, fnum, dof}         focal length (mm, full frame), aperture, depth of field
//   cam_quality  {on}                       photo quality boost (draw distance, LOD, shadows...)
//   cam_clean    {huds, names}              hide HUDs / name tags in the view
//   cam_meter    {on}                       histogram + live readouts, pushed as "cam_meter" events
//   cam_textures                            textures still loading
//   cam_prefs    {json} | {}                the app's saved settings (per account), set or read
//   cam_save_arm {name}                     the next download from the page is that photo,
//                                           written to the save folder
//   cam_move     {orbit,pan,zoom,roll,reset} the camera around its focus
//   cam_isolate  {on}                       my avatar alone: everything else, the sky, terrain
//                                           and water hidden (snapshot {alpha} = transparent PNG)
//   cam_pose     {open}                     Firestorm's poser over the viewfinder
//   cam_look     {layers, fx, fx2, split}   the live looks (mgLookF.glsl), in the view and the photo
// Nothing is saved to the user's settings except the save folder and cam_prefs:
// closing the capture screen (or the phone) puts every changed setting back.
namespace MGPhoneCamera
{
    bool isCommand(const std::string& cmd);
    LLSD runCommand(const std::string& cmd, const LLSD& args);

    // undo every live change (capture screen or phone closed)
    void restore();

    // the user's own settings back without touching the scene (viewer quitting)
    void restoreSettings();

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

    // a shot rendered `factor` times larger, averaged down (supersampling)
    void downsample(LLImageRaw* raw, S32 factor);

    // the viewfinder's area of the window in UI coordinates (empty: no viewfinder)
    void setViewRect(const LLRect& rect);

    // every frame (the phone's draw): focus tracking, the face box, meter events
    void idle();

    // right after the scene is finished, before the interface (render_ui): the meter
    void onSceneFinal();

    // the path for a download the page announced with cam_save_arm, or "" to refuse it
    std::string takeDownloadPath(const std::string& suggested_name);

    // isolation (cam_isolate): the G-buffer's clear colour (llviewerdisplay.cpp) is the
    // background behind the subject; a transparent shot renders over black then white
    bool isIsolating();
    // an object the isolation hid: its updates must not show it again (llviewerobject.cpp)
    bool isIsoHidden(const LLUUID& id);
    // the cut-out is on: the reflection probes hold the scene's light (llreflectionmapmanager.cpp)
    bool isolating();
    // the cut-out's filters: the two background renders go without the look (setMatteShot), a
    // third over black with it gives the colours (lookIntoMatte)
    void setMatteShot(bool on);
    bool lookActive();
    void lookIntoMatte(LLImageRaw* rgba, LLImageRaw* looked);
    void gbufferClear(LLColor4& color);
    void setIsolateWhite(bool white);
    // black + white renders of the same frame -> RGBA with the subject's real alpha
    LLPointer<LLImageRaw> matte(LLImageRaw* on_black, LLImageRaw* on_white);

    // the live look pass (LLPipeline::renderFinalize): true when it drew src into dst
    bool renderLook(LLRenderTarget* src, LLRenderTarget* dst, LLVertexBuffer* triangle);
    // a large photo is drawn in window-size tiles: where the photo lies in the tile drawn now
    void setShotTile(F32 x0, F32 y0, F32 w, F32 h);
    void clearShotTile();
    // the look left out (a thumbnail source for the page)
    void suspendLook(bool suspend);
}

#endif // MG_PHONECAMERA_H
