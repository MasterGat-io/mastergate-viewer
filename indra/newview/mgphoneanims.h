/**
 * @file mgphoneanims.h
 * @brief MasterGate Viewer: the MG Phone "Animations" app (animations + gestures).
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

#ifndef MG_PHONEANIMS_H
#define MG_PHONEANIMS_H

#include "llsd.h"

// The Animations app (raw/my-website/.../apps/animations/client.js). Listing,
// folders, moving, renaming, permissions and giving go through the inv_*
// commands (mgphoneinventory.cpp, inv_index / inv_folders with kind "anim");
// these are the animation and gesture specific ones. Same privacy rule as the
// Textures app: the page works with inventory item ids, never asset UUIDs.
//
//   anim_info   {id}               duration, loop, priority, hand pose, emote, body parts
//                                  (now, or "anim_info" event once the asset is in)
//   anim_play   {id, world}        on my avatar: world = everyone sees it, else only me
//   anim_stop   {id} | {all} | {everything}   the app's ones / every animation on me
//   anim_state                     what the app started and still plays
//   anim_watch  {on}               camera in front of the avatar / back
//   anim_shot   {ids}              preview photos: played locally and photographed
//                                  ("inv_thumb" events, cached like texture thumbnails)
//   anim_ao                        Firestorm AO: on/off, sets and their states
//   anim_ao_add {id, set, state}   add the animation to an AO state
//   anim_ao_enable {on}
//   gest_info   {id}               trigger, key, steps ("gest_info" event when loaded)
//   gest_play   {id}               play (activates it first when needed)
//   gest_active {id, on}
namespace MGPhoneAnims
{
    bool isCommand(const std::string& cmd);
    LLSD runCommand(const std::string& cmd, const LLSD& args);

    // every frame while the phone is open: preview photos, gestures waiting to play
    void idle();

    // phone closed: stop the local test animations, camera back
    void onClose();

    // preview photo cache file for an animation asset (shared with inv_thumbs)
    std::string shotPath(const std::string& asset, S32 size);
}

#endif // MG_PHONEANIMS_H
