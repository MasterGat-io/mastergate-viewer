/**
 * @file mgphoneinventory.h
 * @brief MasterGate Viewer: the MG Phone "Textures" app's access to the inventory.
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

#ifndef MG_PHONEINVENTORY_H
#define MG_PHONEINVENTORY_H

#include "llsd.h"
#include "lluuid.h"

// The Textures app (raw/my-website/.../apps/textures/client.js) browses and
// organizes the avatar's textures and snapshots through "inv_*" bridge commands.
//
// PRIVACY RULE: the page never receives a texture's asset UUID. It works with
// inventory item ids only; thumbnails are rendered here from the texture and
// sent as small JPEGs; duplicates are matched with a per-session hash. The
// asset UUID only ever leaves the viewer through the viewer's own clipboard,
// and only for textures the avatar may copy, modify and transfer.
//
// PBR materials (AT_MATERIAL) live in the same index, flagged "m". Their pictures
// are a studio "material ball" rendered on the CPU from the material's factors
// and maps; their details carry the factors, small map previews and the
// inventory textures that match each map (by item id) -- never a material or
// map UUID either.
//
//   inv_status                         fetch state, object selection
//   inv_folders                        every folder + counts: textures n / all, materials mn / mall
//   inv_index      {offset, limit}     every texture / snapshot / material (compact rows)
//                  {kind: "anim"}      animations + gestures   {kind: "wear"} clothing, body parts, objects
//   inv_item       {id}                details: permissions, creator, folder path...
//   inv_thumbs     {ids, size}         cached thumbnails now, the rest as "inv_thumb" events
//   inv_full       {id, max}           a large preview ("inv_full" event); materials also take
//                  {shape, light, bg, view} and add "info" (factors + maps) unless view
//   inv_cancel                         drop queued thumbnails
//   inv_open / inv_apply {id}          preview window or material editor / apply to the selected faces
//   inv_mat_clear                      remove the PBR material from the selected faces
//   inv_move       {ids, folder}       inv_rename {id, name}   inv_trash {ids}
//   inv_folder_new {parent, name}      ("inv_folder_new" event)   inv_folder_trash {id}
//   inv_copy_uuid / inv_save {id}      full-permission items only (a material saves its maps + JSON)
//   inv_give       {ids | id, to}      any resident; transferable items only (no-copy: the viewer confirms)
//   inv_give_folder {id, to}           a whole folder
//   inv_people     {q} | {nearby}      resident search ("inv_people" event) / avatars around
//   inv_perms      {ids, next}         next owner permissions (bits C1 M2 T4), Properties panel rules
//   inv_full       {..., draft}        a material preview with unsaved edits (textures by item id,
//                                      copy + transfer ones only, like the material editor)
//   inv_mat_save   {id, draft, copy, name}  save the edits (modify) or as a copy ("inv_mat_saved" event)
// Events: inv_thumb, inv_full, inv_saved, inv_folder_new, inv_changed, inv_people, inv_mat_saved.
namespace MGPhoneInventory
{
    bool isCommand(const std::string& cmd);
    LLSD runCommand(const std::string& cmd, const LLSD& args);

    // every frame while the phone is open: start / feed / time out thumbnail jobs
    void idle();

    // <MG> the Wardrobe app: a texture's picture sent as "inv_thumb" under event_id
    // (the page never learns the texture), and the cached one if any ("" if none)
    void queueThumb(const LLUUID& event_id, const LLUUID& texture, S32 size);
    std::string cachedThumb(const LLUUID& texture, S32 size);
}

#endif // MG_PHONEINVENTORY_H
