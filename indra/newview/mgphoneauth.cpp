/**
 * @file mgphoneauth.cpp
 * @brief MasterGate Viewer: automatic MG Phone sign-in (viewer_auth.php).
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

#include "mgphoneauth.h"

#include <boost/json.hpp>
#include <boost/regex.hpp>

#include "llagent.h"
#include "llcallbacklist.h"
#include "llcorehttputil.h"
#include "lleventtimer.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llnotificationsutil.h"
#include "llsdutil.h"
#include "llbadge.h"
#include "llcommandmanager.h"
#include "lltoolbar.h"
#include "lltoolbarview.h"
#include "lluictrlfactory.h"
#include "llsdjson.h"
#include "lluri.h"
#include "llviewercontrol.h"
#include "fscorehttputil.h"
#include "mgphonecalls.h"

namespace
{
    constexpr F32 CODE_WAIT_SECONDS = 120.f;

    bool sPending = false;              // a code IM is expected
    U32  sGeneration = 0;               // invalidates stale timeouts
    std::string sDest = "app:home";
    std::vector<MGPhoneAuth::sso_callback_t> sWaiters;

    std::string authUrl(const std::string& action)
    {
        std::string url = gSavedSettings.getString("MGPhoneAuthURL");
        url += (url.find('?') == std::string::npos) ? "?" : "&";
        url += "a=" + action + "&u=" + gAgentID.asString() + "&dest=" + LLURI::escape(sDest);
        return url;
    }

    LLSD parseBody(const LLSD& result)
    {
        const LLSD::Binary& raw = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        try
        {
            return LlsdFromJson(boost::json::parse(std::string(raw.begin(), raw.end())));
        }
        catch (...)
        {
            return LLSD();
        }
    }

    void finish(const std::string& sso)
    {
        sPending = false;
        ++sGeneration;
        std::vector<MGPhoneAuth::sso_callback_t> waiters;
        waiters.swap(sWaiters);
        for (auto& cb : waiters)
        {
            cb(sso);
        }
    }

    void saveToken(const LLSD& token)
    {
        if (gSavedPerAccountSettings.controlExists("MGPhoneTokenTS"))
        {
            gSavedPerAccountSettings.setString("MGPhoneTokenTS", token["t"].asString());
            gSavedPerAccountSettings.setString("MGPhoneTokenSig", token["s"].asString());
        }
    }

    void clearToken()
    {
        if (gSavedPerAccountSettings.controlExists("MGPhoneTokenTS"))
        {
            gSavedPerAccountSettings.setString("MGPhoneTokenTS", "");
            gSavedPerAccountSettings.setString("MGPhoneTokenSig", "");
        }
    }

    // Ask the server to IM a one-time code; the code arrives in consumeIM().
    void startCodeFlow()
    {
        sPending = true;
        const U32 gen = ++sGeneration;
        FSCoreHttpUtil::callbackHttpGetRaw(authUrl("start"),
            [gen](const LLSD& result)
            {
                const LLSD body = parseBody(result);
                const std::string status = body["status"].asString();
                if (status != "sent" && status != "wait")
                {
                    LL_WARNS("MGPhone") << "sign-in code not sent: " << body["message"].asString() << LL_ENDL;
                    finish("");
                    return;
                }
                LL_INFOS("MGPhone") << "sign-in code requested (" << status << ")" << LL_ENDL;
                doAfterInterval([gen]()
                    {
                        if (sPending && gen == sGeneration)
                        {
                            LL_WARNS("MGPhone") << "sign-in code IM did not arrive in time" << LL_ENDL;
                            finish("");
                        }
                    }, CODE_WAIT_SECONDS);
            },
            [](const LLSD&) { finish(""); });
    }
}

void MGPhoneAuth::requestSso(const std::string& dest, sso_callback_t cb)
{
    if (gAgentID.isNull())
    {
        cb("");
        return;
    }
    sWaiters.push_back(cb);
    if (sPending)
    {
        return; // already waiting for a code IM; all waiters are answered together
    }
    sDest = dest.empty() ? std::string("app:home") : dest;

    std::string ts, sig;
    if (!getToken(ts, sig))
    {
        startCodeFlow();
        return;
    }
    sPending = true;
    ++sGeneration;
    FSCoreHttpUtil::callbackHttpGetRaw(authUrl("resume") + "&t=" + LLURI::escape(ts) + "&s=" + LLURI::escape(sig),
        [](const LLSD& result)
        {
            const LLSD body = parseBody(result);
            if (body["status"].asString() == "ok" && body.has("sso"))
            {
                finish(body["sso"].asString());
                return;
            }
            // token expired or revoked: get a new one through the in-world code
            clearToken();
            startCodeFlow();
        },
        [](const LLSD&) { finish(""); });
}

bool MGPhoneAuth::consumeIM(const LLUUID& from_id, const std::string& message)
{
    static const boost::regex code_re("MGV-AUTH:([0-9a-f]{24})");
    boost::smatch m;
    if (!boost::regex_search(message, m, code_re))
    {
        return false;
    }
    if (!sPending)
    {
        return true; // late or duplicate code: hide it, nothing to do
    }
    const std::string code = m[1].str();
    FSCoreHttpUtil::callbackHttpGetRaw(authUrl("verify") + "&code=" + code,
        [](const LLSD& result)
        {
            const LLSD body = parseBody(result);
            if (body["status"].asString() == "ok" && body.has("sso"))
            {
                saveToken(body["token"]);
                LL_INFOS("MGPhone") << "signed in to MasterGate" << LL_ENDL;
                finish(body["sso"].asString());
            }
            else
            {
                LL_WARNS("MGPhone") << "sign-in code rejected: " << body["message"].asString() << LL_ENDL;
                finish("");
            }
        },
        [](const LLSD&) { finish(""); });
    return true;
}

namespace
{
    // ---- unread badge on the MG Phone toolbar button -------------------------
    S32 sUnread = 0;
    LLHandle<LLView> sBadgedButton;
    LLSD sUnreadEvents = LLSD::emptyArray(); // full push events received while the phone was closed

    LLToolBarButton* findPhoneButton()
    {
        if (!gToolBarView)
        {
            return nullptr;
        }
        const LLCommandId id("mg_phone");
        for (S32 t = LLToolBarEnums::TOOLBAR_FIRST; t <= LLToolBarEnums::TOOLBAR_LAST; ++t)
        {
            LLToolBar* toolbar = gToolBarView->getToolbar((LLToolBarEnums::EToolBarLocation)t);
            if (!toolbar || !toolbar->hasCommand(id))
            {
                continue;
            }
            // toolbars keep their buttons private: walk the view tree
            std::vector<LLView*> todo{ toolbar };
            while (!todo.empty())
            {
                LLView* view = todo.back();
                todo.pop_back();
                LLToolBarButton* button = dynamic_cast<LLToolBarButton*>(view);
                if (button && button->getCommandId() == id)
                {
                    return button;
                }
                for (LLView* child : *view->getChildList())
                {
                    todo.push_back(child);
                }
            }
        }
        return nullptr;
    }

    void updateBadge()
    {
        if (gToolBarView)
        {
            gToolBarView->flashCommand(LLCommandId("mg_phone"), sUnread > 0);
        }
        LLToolBarButton* button = findPhoneButton();
        if (!button)
        {
            return; // the button is not on any toolbar
        }
        if (sBadgedButton.get() != button)
        {
            // toolbar buttons are rebuilt when toolbars change: attach a badge once per button
            LLBadge::Params p(LLUICtrlFactory::getDefaultParams<LLBadge>());
            p.image_color = LLUIColor(LLColor4(0.f, 0.831f, 1.f, 1.f));          // MasterGate cyan
            p.label_color = LLUIColor(LLColor4(0.016f, 0.078f, 0.102f, 1.f));    // dark text on cyan
            p.label = " ";
            // Default is top_left at 85% toward the corner: on the LEFT toolbar that put
            // the badge half off-screen. Top-right at 55% stays inside the button on any toolbar.
            p.location = LLRelPos::TOP_RIGHT;
            p.location_percent_hcenter = 55;
            p.location_percent_vcenter = 55;
            button->initBadgeParams(p);
            sBadgedButton = button->getHandle();
        }
        button->setBadgeLabel(LLStringExplicit(sUnread > 99 ? std::string("99+") : std::to_string(sUnread)));
        button->setBadgeVisibility(sUnread > 0);
    }

    void openPhoneFromNotification(const LLSD& notification, const LLSD& response)
    {
        if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
        {
            return;
        }
        // The phone runs this notification's own tap action (open the thread / post / app).
        LLSD key = LLSD::emptyMap();
        if (notification["payload"].has("event"))
        {
            key["open_event"] = notification["payload"]["event"];
        }
        LLFloaterReg::showInstance("mg_phone", key, true);
    }

    // Polls viewer_auth.php?a=notif and shows MasterGate notifications as viewer toasts
    // while the phone panel is closed. The server only peeks at the queue; the phone
    // still gets everything. To keep the server load low with many viewers online:
    //   - no checks while the phone is open (the phone shows notifications itself); when
    //     it closes, the next check starts from "now" (since=0: cursor only, missed calls
    //     excepted) so nothing already seen in the phone pops up again;
    //   - the interval comes from the server ("next", default 15 s), at least 60 s
    //     while the avatar is away, and 60 s after an error.
    class MGNotifyPoller : public LLEventTimer
    {
    public:
        MGNotifyPoller() : LLEventTimer(5.f), mCursor("0"), mBusy(false), mPhoneWasOpen(false), mNextDue(0.0) { }

        bool tick() override
        {
            LLFloater* phone = LLFloaterReg::findInstance("mg_phone");
            if (phone && phone->getVisible())
            {
                mPhoneWasOpen = true;   // the open phone shows its own notifications
                return false;
            }
            if (mPhoneWasOpen)
            {
                mPhoneWasOpen = false;
                mCursor = "0";          // resync: do not toast what the phone already showed
                mNextDue = 0.0;
            }
            const F64 now = LLTimer::getTotalSeconds();
            std::string ts, sig;
            if (mBusy || now < mNextDue || gAgentID.isNull() || !gSavedSettings.getBOOL("MGPhoneNativeNotifications")
                || !MGPhoneAuth::getToken(ts, sig))
            {
                return false;
            }
            mBusy = true;
            mNextDue = now + 60.0;      // until the answer says otherwise (errors: 60 s)
            std::string url = gSavedSettings.getString("MGPhoneAuthURL");
            url += (url.find('?') == std::string::npos) ? "?" : "&";
            url += "a=notif&u=" + gAgentID.asString() + "&t=" + LLURI::escape(ts) + "&s=" + LLURI::escape(sig)
                 + "&since=" + LLURI::escape(mCursor);
            FSCoreHttpUtil::callbackHttpGetRaw(url,
                [this](const LLSD& result)
                {
                    mBusy = false;
                    const LLSD body = parseBody(result);
                    if (body["status"].asString() != "ok")
                    {
                        return;
                    }
                    F64 every = body.has("next") ? llclamp(body["next"].asReal(), 10.0, 600.0) : 15.0;
                    if (gAgent.getAFK())
                    {
                        every = llmax(every, 60.0);   // away: nobody is looking
                    }
                    mNextDue = LLTimer::getTotalSeconds() + every;
                    mCursor = body["cursor"].asString();
                    LLFloater* phone = LLFloaterReg::findInstance("mg_phone");
                    if (phone && phone->getVisible())
                    {
                        return; // the open phone shows its own notifications
                    }
                    const S32 count = body["count"].asInteger();
                    if (count <= 0)
                    {
                        return;
                    }
                    LL_INFOS("MGPhone") << count << " new MasterGate notification(s)" << LL_ENDL;
                    sUnread += count;
                    updateBadge();
                    for (const LLSD& ev : llsd::inArray(body["events"]))
                    {
                        if (ev.has("event"))
                        {
                            sUnreadEvents.append(ev["event"]); // handed to the phone when it opens
                            while (sUnreadEvents.size() > 20)
                            {
                                sUnreadEvents.erase(0);
                            }
                        }
                        LLSD sub;
                        sub["TITLE"] = ev["title"].asString();
                        sub["MESSAGE"] = ev["body"].asString();
                        LLSD payload;
                        payload["dest"] = ev["dest"].asString();
                        if (ev.has("event"))
                        {
                            payload["event"] = ev["event"];
                        }
                        LLNotificationsUtil::add("MGPhoneNotification", sub, payload, &openPhoneFromNotification);
                    }
                },
                [this](const LLSD&) { mBusy = false; });
            return false;
        }

    private:
        std::string mCursor;
        bool mBusy;
        bool mPhoneWasOpen;
        F64 mNextDue;
    };

    MGNotifyPoller* sPoller = nullptr;
}

void MGPhoneAuth::onLogin()
{
    // MG Phone button first on the LEFT toolbar, once per account. Toolbars are saved per
    // account; an avatar's saved layout (or an older default with the button at the
    // bottom) would otherwise hide it. Done once, so a later removal by the user sticks.
    if (gToolBarView && gSavedPerAccountSettings.controlExists("MGPhoneToolbarSeeded")
        && !gSavedPerAccountSettings.getBOOL("MGPhoneToolbarSeeded"))
    {
        const LLCommandId id("mg_phone");
        const S32 where = gToolBarView->hasCommand(id);
        if (where != LLToolBarEnums::TOOLBAR_LEFT)
        {
            if (where != LLToolBarEnums::TOOLBAR_NONE)
            {
                int rank = LLToolBar::RANK_NONE;
                gToolBarView->removeCommand(id, rank);
            }
            gToolBarView->addCommand(id, LLToolBarEnums::TOOLBAR_LEFT, 0);
        }
        gSavedPerAccountSettings.setBOOL("MGPhoneToolbarSeeded", true);
    }

    std::string ts, sig;
    if (gSavedSettings.getBOOL("MGPhoneAutoSignIn") && !getToken(ts, sig))
    {
        // first time on this account: get a token now (one hidden code IM), so the
        // phone opens instantly and native notifications work from this session on
        requestSso("app:home", [](const std::string&) {});
    }
    if (!sPoller)
    {
        sPoller = new MGNotifyPoller(); // lives for the session (LLEventTimer list)
    }

    MGPhoneCalls::init(); // voice calls in the phone
}

void MGPhoneAuth::onPhoneOpened()
{
    sUnread = 0;
    updateBadge();
}

void MGPhoneAuth::addBadge(S32 count)
{
    sUnread += count;
    updateBadge();
}

LLSD MGPhoneAuth::takeUnreadEvents()
{
    LLSD events = sUnreadEvents;
    sUnreadEvents = LLSD::emptyArray();
    return events;
}

bool MGPhoneAuth::getToken(std::string& ts, std::string& sig)
{
    if (!gSavedPerAccountSettings.controlExists("MGPhoneTokenTS"))
    {
        return false;
    }
    ts = gSavedPerAccountSettings.getString("MGPhoneTokenTS");
    sig = gSavedPerAccountSettings.getString("MGPhoneTokenSig");
    return !ts.empty() && !sig.empty();
}
