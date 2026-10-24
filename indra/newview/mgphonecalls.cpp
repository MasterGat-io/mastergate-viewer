/**
 * @file mgphonecalls.cpp
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

#include "llviewerprecompiledheaders.h"

#include "mgphonecalls.h"

#include <set>

#include "llagent.h"
#include "llavataractions.h"
#include "llavatariconctrl.h"
#include "llavatarnamecache.h"
#include "llavatarpropertiesprocessor.h"
#include "llcallbacklist.h"
#include "llcallingcard.h"
#include "lldir.h"
#include "llfile.h"
#include "llfloaterreg.h"
#include "llgl.h"
#include "llimview.h"
#include "llnotificationsutil.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llvoicechannel.h"
#include "llvoiceclient.h"
#include "llworld.h"
#include "mgfloaterphone.h"
#include "mgphoneauth.h"

namespace
{
    constexpr F64 SHELL_WAIT = 10.0;     // phone opened for a call: its shell must be up by then
    constexpr F64 RING_REPEAT = 4.0;     // ring sound while the phone rings
    constexpr F64 P2P_RING_TIME = 30.0;  // like the incoming_call dialog's lifetime
    constexpr F64 P2P_RING_MAX = 60.0;   // a call the voice server still reports as valid
    constexpr F64 GROUP_RING_TIME = 60.0;// then the group/ad-hoc invite moves to the normal dialog
    constexpr S32 LOG_MAX = 200;

    struct Invite
    {
        LLSD payload;
        F64  since = 0.0;
        F64  lastRing = 0.0;
        bool delivered = false;  // the shell has it
        bool fallback = false;   // the incoming_call dialog took over
        bool timedOut = false;   // declined because nobody answered (missed)
    };

    std::map<LLUUID, Invite> sInvites;

    // the current (non-nearby) voice call
    boost::signals2::connection sChannelConn;
    boost::signals2::connection sStateConn;
    LLUUID      sCallSession;
    std::string sCallState = "idle";
    bool        sCallOut = false;
    F64         sConnectedAt = 0.0;
    bool        sPhoneMicMuted = false;  // the phone's Mute button (calls only)
    LLViewerMediaImpl* sPhoneMedia = nullptr; // the phone panel's media (own volume / priority)
    S32         sLogIndex = -1;          // call log entry of the current call
    bool        sIdle = false;
    bool        sInited = false;

    LLSD        sLog = LLSD::emptyArray(); // newest last
    bool        sLogLoaded = false;

    F64 now() { return LLFrameTimer::getElapsedSeconds(); }

    MGFloaterPhone* readyPhone()
    {
        MGFloaterPhone* p = LLFloaterReg::findTypedInstance<MGFloaterPhone>("mg_phone");
        return (p && p->getVisible() && p->isShellReady()) ? p : nullptr;
    }

    void push(const LLSD& ev)
    {
        if (MGFloaterPhone* p = readyPhone())
        {
            p->pushToPage(ev);
        }
    }

    std::string kindOf(const LLSD& payload)
    {
        const std::string t = payload["notify_box_type"].asString();
        if (t == "VoiceInviteP2P")
        {
            return "p2p";
        }
        if (t == "VoiceInviteGroup")
        {
            return "group";
        }
        if (t == "VoiceInviteAdHoc")
        {
            return "adhoc";
        }
        return "";
    }

    std::string displayName(const LLUUID& id, const std::string& fallback)
    {
        LLAvatarName av;
        if (id.notNull() && LLAvatarNameCache::get(id, &av))
        {
            return av.getDisplayName();
        }
        return fallback;
    }

    // ---- call log (per account, this computer only) -------------------------------
    std::string logPath()
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "mg_call_log.xml");
    }

    void loadLog()
    {
        if (sLogLoaded)
        {
            return;
        }
        sLogLoaded = true;
        const std::string path = logPath();
        llifstream in(path.c_str());
        if (in.is_open())
        {
            LLSD data;
            if (LLSDSerialize::fromXML(data, in) > 0 && data.isArray())
            {
                sLog = data;
            }
        }
    }

    void saveLog()
    {
        while ((S32)sLog.size() > LOG_MAX)
        {
            sLog.erase(0);
            if (sLogIndex >= 0)
            {
                --sLogIndex;
            }
        }
        const std::string path = logPath();
        if (path.empty())
        {
            return;
        }
        llofstream out(path.c_str());
        if (out.is_open())
        {
            LLSDSerialize::toPrettyXML(sLog, out);
        }
    }

    // status: answered | missed | declined | messaged | cancelled | failed
    S32 addLog(const LLUUID& peer, const std::string& name, const std::string& kind, bool out, const std::string& status)
    {
        loadLog();
        LLSD e;
        e["id"] = peer;
        e["name"] = name;
        e["kind"] = kind;
        e["dir"] = out ? "out" : "in";
        e["status"] = status;
        e["at"] = (S32)LLDate::now().secondsSinceEpoch();
        e["dur"] = 0;
        sLog.append(e);
        saveLog();
        push(LLSD().with("type", "call").with("event", "log").with("entry", e));
        return (S32)sLog.size() - 1;
    }

    void updateLog(S32 index, const std::string& status, S32 dur)
    {
        if (index < 0 || index >= (S32)sLog.size())
        {
            return;
        }
        if (!status.empty())
        {
            sLog[index]["status"] = status;
        }
        if (dur >= 0)
        {
            sLog[index]["dur"] = dur;
        }
        saveLog();
        push(LLSD().with("type", "call").with("event", "log").with("entry", sLog[index]));
    }

    // ---- profile photos for the call screens ----------------------------------------
    std::string photoUrl(const LLUUID& image)
    {
        return image.isNull() ? std::string() : "https://secondlife.com/app/image/" + image.asString() + "/1";
    }

    void pushPhoto(const LLUUID& avatar, const LLUUID& image)
    {
        push(LLSD().with("type", "call").with("event", "photo").with("id", avatar).with("url", photoUrl(image)));
    }

    class PhotoFetcher : public LLAvatarPropertiesObserver
    {
    public:
        void request(const LLUUID& id)
        {
            if (id.isNull())
            {
                return;
            }
            if (LLUUID* icon = LLAvatarIconIDCache::getInstance()->get(id))
            {
                pushPhoto(id, *icon);
                return;
            }
            if (mPending.insert(id).second)
            {
                LLAvatarPropertiesProcessor::getInstance()->addObserver(id, this);
                LLAvatarPropertiesProcessor::getInstance()->sendAvatarLegacyPropertiesRequest(id);
            }
        }

        void processProperties(void* data, EAvatarProcessorType type) override
        {
            LLUUID avatar, image;
            if (type == APT_PROPERTIES_LEGACY && data)
            {
                const LLAvatarLegacyData* d = static_cast<const LLAvatarLegacyData*>(data);
                avatar = d->avatar_id;
                image = d->image_id;
            }
            else if (type == APT_PROPERTIES && data)
            {
                const LLAvatarData* d = static_cast<const LLAvatarData*>(data);
                avatar = d->avatar_id;
                image = d->image_id;
            }
            else
            {
                return;
            }
            if (mPending.erase(avatar) == 0)
            {
                return;
            }
            LLAvatarPropertiesProcessor::getInstance()->removeObserver(avatar, this);
            LLAvatarIconIDCache::getInstance()->add(avatar, image);
            pushPhoto(avatar, image);
        }

    private:
        std::set<LLUUID> mPending;
    };

    PhotoFetcher sPhotos;

    // ---- the current call --------------------------------------------------------
    bool isNearby(LLVoiceChannel* ch)
    {
        return !ch || dynamic_cast<LLVoiceChannelProximal*>(ch) != nullptr;
    }

    LLSD callInfo()
    {
        LLSD c;
        c["type"] = "call";
        c["event"] = "state";
        c["state"] = sCallState;
        c["session"] = sCallSession;
        c["direction"] = sCallOut ? "out" : "in";
        c["voice"] = MGPhoneCalls::voiceLineWanted() && LLVoiceClient::getInstance()->isVoiceWorking();
        c["nearby_voice"] = LLVoiceClient::getInstance()->voiceEnabled();
        c["muted"] = gSavedSettings.getBOOL("MGPhoneCallOpensMic") ? sPhoneMicMuted : !LLVoiceClient::getInstance()->getUserPTTState();
        c["secs"] = (sConnectedAt > 0.0) ? (S32)(now() - sConnectedAt) : 0;
        if (LLIMModel::LLIMSession* s = LLIMModel::getInstance()->findIMSession(sCallSession))
        {
            c["name"] = s->mName;
            if (s->isP2PSessionType())
            {
                c["kind"] = "p2p";
                c["peer"] = s->mOtherParticipantID;
                c["name"] = displayName(s->mOtherParticipantID, s->mName);
            }
            else
            {
                c["kind"] = s->isGroupSessionType() ? "group" : "adhoc";
            }
        }
        return c;
    }

    // the call started or ended: the mic override (llvoiceclient.cpp) changes with it
    void refreshMic()
    {
        if (LLVoiceClient::instanceExists())
        {
            LLVoiceClient::getInstance()->updateMicMuteLogic();
        }
    }

    void endCurrent(const std::string& final_state)
    {
        if (sLogIndex >= 0)
        {
            const bool connected = sConnectedAt > 0.0;
            std::string status;
            if (!connected && sCallOut)
            {
                status = (final_state == "failed") ? "failed" : "cancelled";
            }
            updateLog(sLogIndex, status, connected ? (S32)(now() - sConnectedAt) : 0);
        }
        sLogIndex = -1;
        sConnectedAt = 0.0;
        sPhoneMicMuted = false;
        refreshMic();
    }

    void onState(LLVoiceChannel::EState state, LLVoiceChannel::EDirection dir, bool by_agent)
    {
        std::string s;
        switch (state)
        {
        case LLVoiceChannel::STATE_CALL_STARTED: s = (dir == LLVoiceChannel::OUTGOING_CALL) ? "calling" : "connecting"; break;
        case LLVoiceChannel::STATE_RINGING:      s = "ringing"; break;
        case LLVoiceChannel::STATE_CONNECTED:    s = "connected"; break;
        case LLVoiceChannel::STATE_HUNG_UP:      s = "ended"; break;
        case LLVoiceChannel::STATE_ERROR:        s = "failed"; break;
        default:                                 return; // no channel info / ready: not a call step
        }
        if (s == sCallState)
        {
            return;
        }
        sCallOut = (dir == LLVoiceChannel::OUTGOING_CALL);

        if (s == "calling" && sLogIndex < 0)
        {
            LLSD c = callInfo();
            sLogIndex = addLog(c["peer"].asUUID(), c["name"].asString(), c.has("kind") ? c["kind"].asString() : "p2p", true, "cancelled");
        }
        if (s == "connected" && sConnectedAt <= 0.0)
        {
            sConnectedAt = now();
            if (sLogIndex >= 0 && sCallOut)
            {
                updateLog(sLogIndex, "answered", -1);
            }
        }
        sCallState = s;
        refreshMic(); // a call opens the mic (the phone's Mute decides from here)
        LLSD c = callInfo();
        c["ended_by_agent"] = by_agent;
        push(c);
        if (s == "ended" || s == "failed")
        {
            endCurrent(s);
        }
    }

    void attachChannel(LLVoiceChannel* ch)
    {
        sStateConn.disconnect();
        if (isNearby(ch))
        {
            if (sCallState != "idle")
            {
                if (sCallState != "ended" && sCallState != "failed")
                {
                    endCurrent("ended");
                }
                sCallState = "idle";
                sCallSession.setNull();
                push(callInfo());
            }
            return;
        }
        if (ch->getSessionID() != sCallSession)
        {
            sCallSession = ch->getSessionID();
            sCallState = "idle";
            sConnectedAt = 0.0;
        }
        sStateConn = ch->setStateChangedCallback(
            [](const LLVoiceChannel::EState&, const LLVoiceChannel::EState& new_state,
               const LLVoiceChannel::EDirection& dir, bool by_agent, const LLUUID&)
            {
                onState(new_state, dir, by_agent);
            });
        onState(ch->getState(), ch->getCallDirection(), false);
    }

    // ---- ringing in the phone -----------------------------------------------------
    LLSD inviteEvent(const Invite& inv)
    {
        const LLSD& p = inv.payload;
        LLSD ev;
        ev["type"] = "call";
        ev["event"] = "incoming";
        ev["session"] = p["session_id"];
        ev["peer"] = p["caller_id"];
        ev["kind"] = kindOf(p);
        ev["name"] = displayName(p["caller_id"].asUUID(), p["caller_name"].asString());
        ev["group"] = (ev["kind"].asString() == "p2p") ? std::string() : p["session_name"].asString();
        LLGroupData group;
        if (gAgent.getGroupData(p["session_id"].asUUID(), group))
        {
            ev["group"] = group.mName;
        }
        ev["secs"] = (S32)(now() - inv.since);
        return ev;
    }

    LLFloater* findIncomingDialog(const LLUUID& session)
    {
        for (LLFloater* f : LLFloaterReg::getFloaterList("incoming_call"))
        {
            if (f && f->getKey()["session_id"].asUUID() == session)
            {
                return f;
            }
        }
        return nullptr;
    }

    // answer / decline / message, through the dialog when it is up so it closes too
    void respond(const LLUUID& session, S32 response)
    {
        auto it = sInvites.find(session);
        if (it == sInvites.end())
        {
            return;
        }
        if (LLIncomingCallDialog* dlg = dynamic_cast<LLIncomingCallDialog*>(findIncomingDialog(session)))
        {
            if (response == 0)
            {
                LLIncomingCallDialog::onAccept(dlg);
            }
            else if (response == 1)
            {
                LLIncomingCallDialog::onReject(dlg);
            }
            else
            {
                LLIncomingCallDialog::onStartIM(dlg);
            }
            return;
        }
        const LLSD payload = it->second.payload; // processCallResponse erases the invite
        LLIncomingCallDialog::processCallResponse(response, payload);
    }

    void missedCallToast(const std::string& name)
    {
        LLSD sub;
        sub["TITLE"] = "Missed call";
        sub["MESSAGE"] = name;
        LLNotificationsUtil::add("MGPhoneNotification", sub, LLSD(), [](const LLSD& n, const LLSD& r)
            {
                if (LLNotificationsUtil::getSelectedOption(n, r) == 0)
                {
                    LLFloaterReg::showInstance("mg_phone", LLSD().with("dest", "com.mastergate.phone"), true);
                }
                return false;
            });
        MGPhoneAuth::addBadge(1);
    }

    void onIdle(void*)
    {
        const F64 t = now();
        std::vector<LLUUID> decline;
        for (auto& [session, inv] : sInvites)
        {
            if (inv.fallback)
            {
                continue; // the dialog runs it now
            }
            const F64 age = t - inv.since;
            if (!inv.delivered)
            {
                if (MGFloaterPhone* p = readyPhone())
                {
                    p->pushToPage(inviteEvent(inv));
                    inv.delivered = true;
                }
                else if (age > SHELL_WAIT)
                {
                    LL_INFOS("MGPhone") << "phone not ready for the call, showing the call dialog" << LL_ENDL;
                    inv.fallback = true;
                    LLFloaterReg::showInstance("incoming_call", inv.payload, false);
                    continue;
                }
            }
            if (inv.delivered && !readyPhone())
            {
                // the phone was closed while ringing: hand the call to the dialog
                inv.fallback = true;
                LLFloaterReg::showInstance("incoming_call", inv.payload, false);
                continue;
            }
            if (!inv.delivered && t - inv.lastRing > RING_REPEAT)
            {
                // the phone is still opening; once it has the call it plays its ringtone
                inv.lastRing = t;
                make_ui_sound("UISndIncomingVoiceCall");
            }
            if (kindOf(inv.payload) == "p2p")
            {
                const bool valid = (bool)LLVoiceClient::getInstance()->getIncomingCallInterface(inv.payload["voice_channel_info"]);
                if (age > (valid ? P2P_RING_MAX : P2P_RING_TIME))
                {
                    inv.timedOut = true;
                    decline.push_back(session);
                }
            }
            else if (age > GROUP_RING_TIME)
            {
                inv.fallback = true;
                push(LLSD().with("type", "call").with("event", "invite_closed").with("session", session).with("response", -1));
                LLFloaterReg::showInstance("incoming_call", inv.payload, false);
            }
        }
        for (const LLUUID& session : decline)
        {
            respond(session, 1);
        }
        bool active = false;
        for (const auto& [session, inv] : sInvites)
        {
            active = active || !inv.fallback;
        }
        if (!active && sIdle)
        {
            gIdleCallbacks.deleteFunction(&onIdle, nullptr);
            sIdle = false;
        }
    }

    void startIdle()
    {
        if (!sIdle)
        {
            gIdleCallbacks.addFunction(&onIdle, nullptr);
            sIdle = true;
        }
    }
}

void MGPhoneCalls::init()
{
    if (sInited)
    {
        return;
    }
    sInited = true;
    loadLog();
    // turning the phone's voice line on or off restarts / stops the voice client
    if (LLControlVariable* keep = gSavedSettings.getControl("MGPhoneKeepCallsVoice"))
    {
        keep->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&)
            {
                if (LLVoiceClient::instanceExists())
                {
                    LLVoiceClient::getInstance()->updateSettings();
                }
            });
    }
    // voice was off at login: start the phone's voice line now
    if (phoneOnlyVoice() && LLVoiceClient::instanceExists())
    {
        LLVoiceClient::getInstance()->updateSettings();
    }
    sChannelConn = LLVoiceChannel::setCurrentVoiceChannelChangedCallback([](const LLUUID& session)
        {
            LLVoiceChannel* ch = LLVoiceChannel::getChannelByID(session);
            if (!ch)
            {
                ch = LLVoiceChannel::getCurrentVoiceChannel();
            }
            attachChannel(ch);
        });
    attachChannel(LLVoiceChannel::getCurrentVoiceChannel());
}

bool MGPhoneCalls::onIncomingInvite(const LLSD& payload)
{
    if (!gSavedSettings.getBOOL("MGPhoneHandlesCalls"))
    {
        return false;
    }
    const std::string kind = kindOf(payload);
    const LLUUID session = payload["session_id"].asUUID();
    if (kind.empty() || session.isNull())
    {
        return false;
    }
    Invite& inv = sInvites[session];
    inv = Invite();
    inv.payload = payload;
    inv.since = now();
    sPhotos.request(payload["caller_id"].asUUID());

    if (MGFloaterPhone* p = readyPhone())
    {
        p->pushToPage(inviteEvent(inv));
        inv.delivered = true;
        startIdle();
        return true;
    }
    LLFloater* phone = LLFloaterReg::findInstance("mg_phone");
    if ((phone && phone->getVisible()) || gSavedSettings.getBOOL("MGPhoneOpenOnCall"))
    {
        // the phone opens (or is still loading) and rings once its shell is up;
        // onIdle shows the normal dialog if that takes too long
        LLFloaterReg::showInstance("mg_phone", LLSD::emptyMap(), false);
        startIdle();
        return true;
    }
    sInvites.erase(session);
    return false;
}

void MGPhoneCalls::onInviteResponse(S32 response, const LLSD& payload)
{
    const LLUUID session = payload["session_id"].asUUID();
    LLUUID caller = payload["caller_id"].asUUID();
    const std::string kind = kindOf(payload);
    auto it = sInvites.find(session);
    const bool tracked = it != sInvites.end();
    const bool timedOut = tracked && it->second.timedOut;
    if (tracked)
    {
        sInvites.erase(it);
    }
    if (kind.empty())
    {
        return;
    }
    push(LLSD().with("type", "call").with("event", "invite_closed").with("session", session)
               .with("response", response).with("missed", timedOut));

    // log every incoming call (one the viewer declined by itself, e.g. Do Not Disturb or
    // friends-only calls, never rang: it counts as missed)
    const std::string name = displayName(caller, payload["caller_name"].asString());
    std::string status = (response == 0) ? "answered" : (response == 2 ? "messaged" : ((timedOut || !tracked) ? "missed" : "declined"));
    const S32 index = addLog(kind == "p2p" ? caller : session, kind == "p2p" ? name : payload["session_name"].asString(), kind, false, status);
    if (response == 0)
    {
        sLogIndex = index; // its duration is filled in when the call ends
    }
    if (timedOut && !readyPhone())
    {
        missedCallToast(name);
    }
}

bool MGPhoneCalls::handlesCallUi()
{
    return gSavedSettings.getBOOL("MGPhoneHandlesCalls") && readyPhone() != nullptr;
}

bool MGPhoneCalls::isCommand(const std::string& cmd)
{
    return cmd.rfind("call_", 0) == 0;
}

LLSD MGPhoneCalls::runCommand(const std::string& cmd, const LLSD& args)
{
    LLSD result = LLSD().with("ok", true);
    LLUUID id;
    id.set(args["id"].asString(), false);
    LLUUID session;
    session.set(args["session"].asString(), false);

    if (cmd == "call_start")
    {
        if (id.isNull() || id == gAgentID)
        {
            return LLSD().with("ok", false).with("error", "bad avatar");
        }
        if (!MGPhoneCalls::voiceLineWanted() || !LLVoiceClient::getInstance()->isVoiceWorking())
        {
            return LLSD().with("ok", false).with("error", "voice_off");
        }
        if (!isNearby(LLVoiceChannel::getCurrentVoiceChannel()))
        {
            return LLSD().with("ok", false).with("error", "busy");
        }
        if (LLAvatarTracker::instance().isBuddy(id) && !LLAvatarTracker::instance().isBuddyOnline(id))
        {
            // an offline friend cannot take a voice call: the phone offers a missed-call notice
            return LLSD().with("ok", false).with("error", "offline");
        }
        sPhoneMicMuted = false;
        LLAvatarActions::startCall(id);
        sPhotos.request(id);
    }
    else if (cmd == "call_answer" || cmd == "call_decline" || cmd == "call_message")
    {
        if (sInvites.find(session) == sInvites.end())
        {
            return LLSD().with("ok", false).with("error", "no such call");
        }
        if (cmd == "call_answer")
        {
            sPhoneMicMuted = false;
        }
        respond(session, cmd == "call_answer" ? 0 : (cmd == "call_decline" ? 1 : 2));
    }
    else if (cmd == "call_hangup")
    {
        LLVoiceChannel* ch = LLVoiceChannel::getCurrentVoiceChannel();
        if (!isNearby(ch))
        {
            gIMMgr->endCall(ch->getSessionID());
        }
    }
    else if (cmd == "call_mute")
    {
        if (gSavedSettings.getBOOL("MGPhoneCallOpensMic"))
        {
            sPhoneMicMuted = args["muted"].asBoolean();
            refreshMic();
            result["muted"] = sPhoneMicMuted;
        }
        else
        {
            LLVoiceClient::getInstance()->setUserPTTState(!args["muted"].asBoolean());
            result["muted"] = !LLVoiceClient::getInstance()->getUserPTTState();
        }
    }
    else if (cmd == "call_state")
    {
        result["call"] = callInfo();
        LLSD invites = LLSD::emptyArray();
        for (const auto& [s, inv] : sInvites)
        {
            if (!inv.fallback)
            {
                invites.append(inviteEvent(inv));
            }
        }
        result["invites"] = invites;
    }
    else if (cmd == "call_contacts")
    {
        LLAvatarTracker::buddy_map_t buddies;
        LLAvatarTracker::instance().copyBuddyList(buddies);
        LLSD list = LLSD::emptyArray();
        for (const auto& [buddy, rel] : buddies)
        {
            LLAvatarName av;
            const bool known = LLAvatarNameCache::get(buddy, &av);
            LLSD c;
            c["id"] = buddy;
            c["name"] = known ? av.getDisplayName() : std::string();
            c["user"] = known ? av.getUserName() : std::string();
            c["online"] = rel && rel->isOnline();
            list.append(c);
        }
        result["contacts"] = list;
    }
    else if (cmd == "call_nearby")
    {
        uuid_vec_t ids;
        std::vector<LLVector3d> positions;
        LLWorld::getInstance()->getAvatars(&ids, &positions, gAgent.getPositionGlobal(), 96.f);
        LLSD list = LLSD::emptyArray();
        for (size_t i = 0; i < ids.size(); ++i)
        {
            if (ids[i] == gAgentID)
            {
                continue;
            }
            LLAvatarName av;
            const bool known = LLAvatarNameCache::get(ids[i], &av);
            LLSD c;
            c["id"] = ids[i];
            c["name"] = known ? av.getDisplayName() : std::string();
            c["user"] = known ? av.getUserName() : std::string();
            c["dist"] = ll_round((F32)dist_vec(positions[i], gAgent.getPositionGlobal()));
            c["friend"] = LLAvatarTracker::instance().isBuddy(ids[i]);
            list.append(c);
        }
        result["nearby"] = list;
    }
    else if (cmd == "call_photos")
    {
        for (const LLSD& v : llsd::inArray(args["ids"]))
        {
            LLUUID a;
            if (a.set(v.asString(), false))
            {
                sPhotos.request(a);
            }
        }
    }
    else if (cmd == "call_openim")
    {
        if (id.notNull())
        {
            LLAvatarActions::startIM(id);
        }
    }
    else if (cmd == "call_profile")
    {
        if (id.notNull())
        {
            LLAvatarActions::showProfile(id);
        }
    }
    else if (cmd == "call_prefs" || cmd == "call_prefs_set")
    {
        // the phone's own sound and voice settings (Phone app: Sound & calls)
        if (cmd == "call_prefs_set")
        {
            if (args.has("ring_volume"))
            {
                gSavedSettings.setF32("MGPhoneVolume", llclamp((F32)args["ring_volume"].asReal(), 0.f, 1.f));
            }
            if (args.has("call_volume"))
            {
                gSavedSettings.setF32("MGPhoneCallVolume", llclamp((F32)args["call_volume"].asReal(), 0.f, 1.f));
            }
            if (args.has("keep_calls"))
            {
                gSavedSettings.setBOOL("MGPhoneKeepCallsVoice", args["keep_calls"].asBoolean());
            }
            if (args.has("nearby_voice"))
            {
                gSavedSettings.setBOOL("EnableVoiceChat", args["nearby_voice"].asBoolean());
            }
        }
        result["ring_volume"] = gSavedSettings.getF32("MGPhoneVolume");
        result["call_volume"] = gSavedSettings.getF32("MGPhoneCallVolume");
        result["keep_calls"] = gSavedSettings.getBOOL("MGPhoneKeepCallsVoice");
        result["nearby_voice"] = LLVoiceClient::getInstance()->voiceEnabled();
        result["voice"] = MGPhoneCalls::voiceLineWanted() && LLVoiceClient::getInstance()->isVoiceWorking();
    }
    else if (cmd == "call_presence")
    {
        // friend / online for the people on a list (online is only known for friends
        // who share it; everyone else is "unknown")
        LLSD people = LLSD::emptyMap();
        for (const LLSD& v : llsd::inArray(args["ids"]))
        {
            LLUUID a;
            if (!a.set(v.asString(), false) || a.isNull())
            {
                continue;
            }
            LLSD p;
            const bool buddy = LLAvatarTracker::instance().isBuddy(a);
            p["friend"] = buddy;
            if (buddy)
            {
                p["online"] = LLAvatarTracker::instance().isBuddyOnline(a);
            }
            people[a.asString()] = p;
        }
        result["people"] = people;
    }
    else if (cmd == "call_log_add")
    {
        // a missed call reported by MasterGate (the caller tried while we were offline);
        // eid makes it idempotent (the same notice can arrive live and in a replay)
        loadLog();
        const std::string eid = args["eid"].asString();
        bool known = eid.empty();
        for (const LLSD& e : llsd::inArray(sLog))
        {
            known = known || (e.has("eid") && e["eid"].asString() == eid);
        }
        if (!known && id.notNull())
        {
            LLSD e;
            e["id"] = id;
            e["name"] = args["name"].asString();
            e["kind"] = "p2p";
            e["dir"] = "in";
            e["status"] = "missed";
            e["at"] = args.has("at") ? args["at"].asInteger() : (S32)LLDate::now().secondsSinceEpoch();
            e["dur"] = 0;
            e["eid"] = eid;
            e["offline"] = true;
            sLog.append(e);
            saveLog();
            push(LLSD().with("type", "call").with("event", "log").with("entry", e));
        }
        result["added"] = !known;
    }
    else if (cmd == "call_log")
    {
        loadLog();
        result["log"] = sLog;
    }
    else if (cmd == "call_log_clear")
    {
        sLog = LLSD::emptyArray();
        sLogIndex = -1;
        saveLog();
    }
    else
    {
        result = LLSD().with("ok", false).with("error", "unknown command");
    }
    return result;
}

void MGPhoneCalls::onShellReady()
{
    MGFloaterPhone* p = readyPhone();
    if (!p)
    {
        return;
    }
    if (sCallState != "idle")
    {
        p->pushToPage(callInfo());
    }
    for (auto& [session, inv] : sInvites)
    {
        if (!inv.fallback)
        {
            p->pushToPage(inviteEvent(inv));
            inv.delivered = true;
        }
    }
}

// ---- the phone works with the viewer's media, sound, mic and voice turned off ----------

bool MGPhoneCalls::phoneOnlyVoice()
{
    static LLCachedControl<bool> keep(gSavedSettings, "MGPhoneKeepCallsVoice", true);
    static LLCachedControl<bool> handles(gSavedSettings, "MGPhoneHandlesCalls", true);
    static LLCachedControl<bool> voice_chat(gSavedSettings, "EnableVoiceChat", true);
    static LLCachedControl<bool> cmd_line_off(gSavedSettings, "CmdLineDisableVoice", false);
    return keep && handles && !voice_chat && !cmd_line_off && !gNonInteractive;
}

bool MGPhoneCalls::voiceLineWanted()
{
    return (LLVoiceClient::instanceExists() && LLVoiceClient::getInstance()->voiceEnabled()) || phoneOnlyVoice();
}

bool MGPhoneCalls::nearbyVoiceBlocked()
{
    return phoneOnlyVoice();
}

bool MGPhoneCalls::inCall()
{
    return sCallState == "calling" || sCallState == "ringing" || sCallState == "connecting" || sCallState == "connected";
}

void MGPhoneCalls::micOverride(bool& mic_muted)
{
    static LLCachedControl<bool> opens(gSavedSettings, "MGPhoneCallOpensMic", true);
    static LLCachedControl<bool> handles(gSavedSettings, "MGPhoneHandlesCalls", true);
    if (opens && handles && inCall())
    {
        mic_muted = sPhoneMicMuted;
    }
}

bool MGPhoneCalls::callVolume(F32& speaker, F32& mic)
{
    static LLCachedControl<bool> handles(gSavedSettings, "MGPhoneHandlesCalls", true);
    static LLCachedControl<F32> call_volume(gSavedSettings, "MGPhoneCallVolume", 0.7f);
    static LLCachedControl<F32> mic_level(gSavedSettings, "AudioLevelMic", 1.f);
    if (!handles || !inCall())
    {
        return false;
    }
    speaker = llclamp((F32)call_volume, 0.f, 1.f);
    mic = mic_level;
    return true;
}

void MGPhoneCalls::setPhoneMedia(LLViewerMediaImpl* impl)
{
    sPhoneMedia = impl;
}

bool MGPhoneCalls::isPhoneMedia(const LLViewerMediaImpl* impl)
{
    return impl && impl == sPhoneMedia;
}

F32 MGPhoneCalls::phoneMediaVolume()
{
    static LLCachedControl<F32> volume(gSavedSettings, "MGPhoneVolume", 1.f);
    return llclamp((F32)volume, 0.f, 1.f);
}
