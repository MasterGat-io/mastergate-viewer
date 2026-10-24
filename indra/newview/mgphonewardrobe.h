/**
 * @file mgphonewardrobe.h
 * @brief MasterGate Viewer: the MG Phone "Wardrobe" app (clothes, body parts,
 *        wearable objects, outfits, a 3D view of each item).
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

#ifndef MG_PHONEWARDROBE_H
#define MG_PHONEWARDROBE_H

#include "llsd.h"
#include "lluuid.h"

// The Wardrobe app (raw/my-website/.../apps/wardrobe/client.js) organizes the
// avatar's clothes, body parts and wearable objects; the index, folders, moves,
// renames, permissions and gifts are the Textures app's inv_* commands
// (inv_index {kind: "wear"}).
//
// 3D view: an object only has a shape once it is worn or rezzed (inventory holds
// no geometry). The shape of every worn attachment is read here (its prims and
// mesh at the best level of detail, rigged mesh in its rest pose, the face
// textures) and kept in the viewer's cache (cache/mgwardrobe), so the item opens
// in 3D later even when it is not worn. An item never seen yet can be "peeked":
// added for a moment, read, taken off. The 3D view is drawn here, like the mesh
// upload preview, and only its frames reach the page: never a texture, mesh or
// object asset UUID. Nothing is exported.
//
//   ward_status                      worn count, My Outfits folder, base outfit
//   ward_worn                        what the avatar wears (item ids, attach points)
//   ward_wear  {id, add}             wear (replace on its layer / point) or add
//   ward_off   {id}                  take off / detach
//   ward_outfits                     the My Outfits folders
//   ward_outfit {id}                 an outfit's items
//   ward_outfit_wear {id, add}       ward_outfit_off {id}
//   ward_outfit_save {name}          a new outfit from what is worn ("ward_saved" event)
//   ward_outfit_update               save what is worn into the current outfit
//   ward_photo {id}                  a photo of the avatar as the outfit's thumbnail ("ward_photo")
//   ward_thumb_3d {id}               the 3D view as the item's inventory thumbnail ("ward_thumb")
//   ward_shape {id}                  is the 3D shape known, its facts
//   ward_peek  {id}                  wear for a moment to read the shape ("ward_peek")
//   ward_stage {id, size}            open the 3D view ("ward_stage" frames)
//   ward_stage_set {dyaw, dpitch, dzoom, lift, view, spin, bg, mode, grid, part}
//   ward_stage_close
//   ward_cards {ids}                 3D cards for items without a thumbnail ("inv_thumb", card)
//   ward_system {id}                 a clothing layer / body part: its textures ("inv_thumb" by key)
// Events: ward_stage, ward_peek, ward_saved, ward_photo, ward_thumb, inv_thumb, ward_changed.
namespace MGPhoneWardrobe
{
    bool isCommand(const std::string& cmd);
    LLSD runCommand(const std::string& cmd, const LLSD& args);

    // every frame while the phone is open: peeks, cards, photos
    void idle();
    // the phone closed: the 3D view off, a peek undone
    void onClose();
    // for the whole session: read the shapes of worn attachments as they load
    void startWatching();

    // the cached 3D card of an item ("" if none), and whether its shape is known
    std::string cardPath(const LLUUID& item);
    bool hasShape(const LLUUID& item);
    // where a known item sits on the body: body, hair, head, neck, hands, feet, top,
    // bottom, full, tail, wings ("" if not known)
    std::string regionOf(const LLUUID& item);
}

#endif // MG_PHONEWARDROBE_H
