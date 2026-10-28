/**
 * @file mgphoneplaces.h
 * @brief MasterGate Viewer: the MG Phone "Places" app (the avatar's landmarks).
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

#ifndef MG_PHONEPLACES_H
#define MG_PHONEPLACES_H

#include "llsd.h"

// The Places app (raw/my-website/.../apps/places/client.js) shows the avatar's
// landmarks as places: the parcel's picture, region, rating, traffic, distance,
// favorites (the Favorites bar), the teleport history, and the place the avatar is in.
// The page works with inventory item ids; the landmark asset never reaches it. Parcel
// pictures and positions are public (search, the world map) and are sent as is.
//
//   place_list                          landmarks (compact rows), their folders, favorites
//   place_info     {ids} | {points}     place details: cached now, the rest as "place_info"
//                                       events (landmark -> position -> parcel). points:
//                                       [{key, gx, gy, gz}] for history entries
//   place_here                          the current region / parcel, the landmark here if any
//   place_history                       the teleport history, newest first
//   place_tp       {id} | {gx, gy, gz}  teleport (a landmark, or a global position)
//   place_map      {id} | {gx, gy, gz}  the world map, tracking it
//   place_slurl    {id} | {gx, gy, gz, copy}   "place_slurl" event; copy: to the clipboard too
//   place_create   {name, desc, folder} a landmark of the current place ("place_changed")
//   place_edit     {id, name, desc}     rename / notes
//   place_move     {ids, folder}        place_trash {ids}   place_folder_new {parent, name}
//   place_fav      {id, on}             add to / remove from the Favorites bar (a link)
//   place_give     {ids, to}            send landmarks to a resident (transferable ones)
//   place_friends                       the friends list (with online state), for sharing;
//                                       search / nearby use inv_people (mgphoneinventory.cpp)
//   place_folder_rename {id, name} / place_folder_trash {id}   user folders only
//   place_home_set / place_home_go      set home here / teleport home
// place_list rows carry "dup": a per-session hash of the landmark asset (exact copies share
// it, the asset stays in the viewer) and "nt" for no-transfer landmarks.
// Events: place_info, place_slurl, place_changed, place_folder_new.
namespace MGPhonePlaces
{
    bool isCommand(const std::string& cmd);
    LLSD runCommand(const std::string& cmd, const LLSD& args);

    // every frame while the phone is open: landmark / parcel lookups
    void idle();
}

#endif // MG_PHONEPLACES_H
