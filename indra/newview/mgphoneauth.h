/**
 * @file mgphoneauth.h
 * @brief MasterGate Viewer: automatic MG Phone sign-in (MGPhoneAuthURL).
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

#ifndef MG_PHONEAUTH_H
#define MG_PHONEAUTH_H

#include <functional>
#include <string>

#include "llsd.h"

class LLUUID;

// Signs the logged-in avatar into MasterGate without any user action:
//   1. a saved 30-day viewer token (per account) is exchanged for a fresh SSO link;
//   2. otherwise the server IMs the avatar a one-time code through MasterBots, the
//      viewer consumes that IM silently and exchanges the code for the SSO link + token.
// The SSO link opens the phone in viewer mode (unknown avatars get the in-phone sign-up).
namespace MGPhoneAuth
{
    // cb receives the SSO URL, or "" when sign-in is not possible (the caller then
    // falls back to the website login page).
    typedef std::function<void(const std::string& sso_url)> sso_callback_t;
    void requestSso(const std::string& dest, sso_callback_t cb);

    // Called for every incoming IM. Returns true when it was a MasterGate sign-in
    // code IM ("MGV-AUTH:<24 hex>"), which is then consumed and never displayed.
    bool consumeIM(const LLUUID& from_id, const std::string& message);

    // Viewer token for native MasterGate API calls (notifications), "" if none yet.
    bool getToken(std::string& ts, std::string& sig);

    // Called once the avatar is in-world (llstartup.cpp, STATE_STARTED): signs in
    // silently if there is no token yet (MGPhoneAutoSignIn) and starts the native
    // notification poller (MGPhoneNativeNotifications).
    void onLogin();

    // The phone panel was opened: clear the unread badge on its toolbar button.
    void onPhoneOpened();

    // Add to the unread badge on the toolbar button (e.g. a missed call).
    void addBadge(S32 count);

    // Full push events received while the phone was closed (cleared by the call):
    // the panel hands them to the shell so they appear on its lock screen / drawer.
    LLSD takeUnreadEvents();
}

#endif // MG_PHONEAUTH_H
