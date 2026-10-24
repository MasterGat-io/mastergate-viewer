/**
 * @file mgphoneupdate.h
 * @brief MasterGate Viewer: install a new release from the MG Phone, over this one.
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

#ifndef MG_PHONEUPDATE_H
#define MG_PHONEUPDATE_H

#include <string>

#include "llsd.h"

// The MG Phone shell (mgos.js checkViewerUpdate) offers a newer release when the server
// has one (viewer/releases.php). Nothing happens without the user (TPV Policy 1.d):
//   update_state                              what the viewer can do / is doing
//   update_start   {url, sha256, version}     download the Setup in the background
//                                             (https://mastergate.io/viewer/ only), then
//                                             check its SHA-256 against releases.php
//   update_install                            run the checked Setup in update mode
//                                             (/MGUPDATE: no questions, same folder,
//                                             settings kept) and close the viewer; the
//                                             installer opens the new version when done
//   update_cancel                             stop a download, delete the file
// Events pushed to the shell: {type:'update', state: idle | downloading | ready |
//   failed | installing, done, total, version, error}
// Windows only for now: other systems answer "unsupported" and the shell shows the
// download link instead.
namespace MGPhoneUpdate
{
    bool isCommand(const std::string& cmd);
    LLSD runCommand(const std::string& cmd, const LLSD& args);
}

#endif // MG_PHONEUPDATE_H
