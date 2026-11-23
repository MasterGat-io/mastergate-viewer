/**
 * @file mgphoneanimstudio.h
 * @brief <MG> MG Phone Animation Studio: make poses, animations, dances and face motions on
 *        your own avatar, keyframe by keyframe, with a timeline at the bottom of the 3D view.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * MasterGate Viewer, based on the Firestorm Viewer and the Second Life Viewer.
 * $/LicenseInfo$
 */

#ifndef MG_PHONEANIMSTUDIO_H
#define MG_PHONEANIMSTUDIO_H

#include "llsd.h"

// The page (apps/animstudio/client.js) holds the clip (keys per joint) and works out the pose
// at any frame; the viewer poses the avatar (Firestorm's poser, see mgphoneposer.cpp), draws
// the timeline in the 3D view, plays the clip as a real animation and writes .anim files.
// Commands ("studio_*"):
//   studio_open     {start: current | t | a}   posing on, AO off; { joints, pose, cost, female }
//   studio_close    {}                          all off, the AO back, the avatar let go
//   studio_capture  {groups}                    the pose now (see MGPhonePoser::capturePose)
//   studio_set      {pose, snap}                a pose onto the avatar (a frame, a key, a preset)
//   studio_tool     {mode: rotate | move | off, joint, group: body | hands | face}
//   studio_play     {anim}  studio_stop {}      the clip as an animation on my avatar (only me)
//   studio_timeline {frames, fps, frame, playing, play_from, loop, loop_in, loop_out, auto,
//                    tracks: [{id, label, keys, sel, kind}], hint, title}   (refresh every 2 s)
//   studio_upload   {anim, name, desc}          -> event studio_uploaded
//   studio_save     {anim, name}                .anim file on disk -> event studio_saved
//   studio_import   {}                          a .bvh / .anim file -> event studio_imported
//   studio_projects {what: list | save | load | delete, name, data}
//   studio_camera   {view: body | face | hands | feet | reset, turn}
//   studio_world    {asset, on}                 an uploaded animation played for everyone
// Anim form (both ways): { duration, loop, loop_in, loop_out, priority, ease_in, ease_out,
//   hand, emote, joints: { name: { r: [t,x,y,z,w, ...], p: [t,x,y,z, ...], pri } } }
// Events: studio_tl {ev: scrub | key | move | btn | loop, ...}, studio_joint {joint},
//   studio_posed {joint}, studio_uploaded, studio_saved, studio_imported, studio_closed.
namespace MGPhoneAnimStudio
{
    bool isCommand(const std::string& cmd);
    LLSD runCommand(const std::string& cmd, const LLSD& args);
    void idle();
    void onClose();
}

#endif // MG_PHONEANIMSTUDIO_H
