/**
 * @file mgphonecalls.h
 * @brief MasterGate Viewer: Second Life voice calls in the MG Phone.
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

#ifndef MG_PHONECALLS_H
#define MG_PHONECALLS_H

#include <string>

#include "llsd.h"

class LLViewerMediaImpl;

// The MG Phone is the call UI for the viewer's own voice calls (SL voice, P2P / group /
// ad-hoc): the MGOS shell shows the incoming-call screen and the in-call screen, the
// "Phone" app places calls. Nothing new on the wire: every action goes through the
// viewer's normal call code (LLAvatarActions::startCall, LLIncomingCallDialog's
// responses, LLIMMgr::endCall), so the other side sees an ordinary SL call.
//
// Incoming call (MGPhoneHandlesCalls):
//   * phone open and ready  -> it rings in the phone instead of the incoming_call dialog;
//   * phone closed          -> the phone opens and rings (MGPhoneOpenOnCall); if its
//                              shell is not up within a few seconds, the normal dialog
//                              shows instead, so a call can never be lost to the phone.
// Calls are logged per account (mg_call_log.xml, kept on this computer only).
//
// Bridge commands (mgfloaterphone.cpp forwards every "call_*" command here):
//   call_start {id}  call_answer {session}  call_decline {session}  call_message {session}
//   call_hangup  call_mute {muted}  call_state  call_contacts  call_nearby
//   call_photos {ids[]}  call_openim {id}  call_profile {id}  call_log  call_log_clear
//   call_presence {ids[]}  call_log_add {id, name, at, eid}  (a missed call MasterGate
//   reported: someone called while this avatar was offline)
// call_start fails with "offline" for an offline friend, "busy" during a call,
// "voice_off" when voice is disabled.
// Events pushed to the shell: {type:'call', event: incoming | invite_closed | state |
//   photo | log, ...}
namespace MGPhoneCalls
{
    // after login (MGPhoneAuth::onLogin)
    void init();

    // llimview.cpp LLIMMgr::inviteToSession / onInviteNameLookup, right before the
    // incoming_call dialog: true = the phone took the call (do not show the dialog)
    bool onIncomingInvite(const LLSD& payload);

    // llimview.cpp LLIncomingCallDialog::processCallResponse (0 accept, 1 decline, 2 IM)
    void onInviteResponse(S32 response, const LLSD& payload);

    // llimview.cpp LLCallDialogManager: the phone shows outgoing-call progress itself
    bool handlesCallUi();

    bool isCommand(const std::string& cmd);
    LLSD runCommand(const std::string& cmd, const LLSD& args);

    // the phone shell said hello: hand it the current call and any ringing invite
    void onShellReady();

    // ---- the phone works with the viewer's media, sound, mic and voice turned off ----
    // Phone-only voice line (MGPhoneKeepCallsVoice): with voice chat off, the voice
    // client keeps running for calls but never joins nearby (spatial) voice, so nobody
    // around hears you or is heard; calls ring and connect as usual.
    bool phoneOnlyVoice();
    bool voiceLineWanted();      // run the voice client (voice chat on, or the phone's line)
    bool nearbyVoiceBlocked();   // llvoicewebrtc.cpp: stay out of nearby voice
    bool inCall();               // a person / group call is being set up or running
    // llvoiceclient.cpp: during a call the phone's Mute decides the mic (open by default),
    // whatever push-to-talk, "disable mic" or "mute when minimized" say (MGPhoneCallOpensMic)
    void micOverride(bool& mic_muted);
    // llvieweraudio.cpp: during a call, the phone's call volume and the mic level,
    // whatever the voice / master mutes say. false = not in a call (normal volumes)
    bool callVolume(F32& speaker, F32& mic);
    // llviewermedia.cpp: the phone panel's own media volume (MGPhoneVolume) and priority
    void setPhoneMedia(LLViewerMediaImpl* impl);
    bool isPhoneMedia(const LLViewerMediaImpl* impl);
    F32  phoneMediaVolume();
}

#endif // MG_PHONECALLS_H
