/**
 * @file mgfloaterphone.cpp
 * @brief MasterGate Viewer: native "MG Phone" panel hosting the MGOS shell.
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

#include "mgfloaterphone.h"

#include <boost/json.hpp>

#include "llagent.h"
#include "llbase64.h"
#include "llcommandhandler.h"
#include "llfloaterreg.h"
#include "llgl.h" // <MG/> max texture size for pro shots
#include "llwindow.h" // <MG/> keep the cursor across shots
#include "llimagejpeg.h"
#include "llimview.h"
#include "llmediactrl.h"
#include "llnotificationsutil.h"
#include "llpluginclassmedia.h"
#include "llsdjson.h"
#include "llsdutil.h"
#include "llstatusbar.h"
#include "lltextbox.h"
#include "llui.h"
#include "lluri.h"
#include "llurldispatcher.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llweb.h"
#include "fsnearbychathub.h"
#include "mgphoneauth.h"
#include "mgphonecalls.h"
#include "mgphoneanims.h" // <MG/>
#include "mgphonecamera.h" // <MG/>
#include "mgphoneinventory.h" // <MG/>
#include "mgphonewardrobe.h" // <MG/>
#include "mgphoneupdate.h" // <MG/>
#include "mgphoneplaces.h" // <MG/>

namespace
{
    // Bezel around the screen (floater_mg_phone.xml must match).
    constexpr S32 BEZEL_SIDE = 12;
    constexpr S32 BEZEL_TOP = 30;    // includes the drag/close strip
    constexpr S32 BEZEL_BOTTOM = 14;

    // Logical page sizes and on-screen widths, as in
    // raw/my-website/modules/boonex/mastergateos/phone-launcher.js modeConfig().
    struct PhoneMode
    {
        S32 page_w;
        S32 page_h;
        S32 screen_w;
    };

    PhoneMode modeFor(const std::string& size, bool landscape, bool tablet)
    {
        const F32 mul = (size == "s") ? 0.82f : ((size == "l") ? 1.18f : 1.f);
        if (tablet)
        {
            return { 1024, 1024, ll_round(600.f * mul) };
        }
        if (landscape)
        {
            return { 1024, 512, ll_round(880.f * mul) };
        }
        return { 512, 1024, ll_round(380.f * mul) };
    }

    bool isOn(const LLSD& query, const char* key)
    {
        if (!query.has(key))
        {
            return false;
        }
        const std::string v = query[key].asString();
        return v == "1" || v == "true";
    }

    std::string toJson(const LLSD& sd)
    {
        return boost::json::serialize(LlsdToJson(sd));
    }
}

MGFloaterPhone::MGFloaterPhone(const LLSD& key)
:   LLFloater(key),
    mBrowser(nullptr),
    mStatus(nullptr),
    mLoaded(false),
    mApplyingMode(false),
    mPageW(512),
    mPageH(1024),
    mLastSeq(-1),
    mLastBalance(-1),
    mShellReady(false),
    mReplay(LLSD::emptyArray())
{
}

MGFloaterPhone::~MGFloaterPhone()
{
    if (mBrowser)
    {
        mBrowser->remObserver(this);
    }
}

bool MGFloaterPhone::postBuild()
{
    mBrowser = getChild<LLMediaCtrl>("phone_browser");
    mBrowser->addObserver(this);
    mStatus = getChild<LLTextBox>("phone_status");
    MGPhoneWardrobe::startWatching(); // <MG/> the Wardrobe reads the shapes of worn items
    return true;
}

void MGFloaterPhone::onOpen(const LLSD& key)
{
    MGPhoneAuth::onPhoneOpened(); // clears the toolbar badge

    // Notifications that arrived while the phone was closed, plus the one whose
    // "Open phone" button was used: handed to the shell once it is ready.
    for (const LLSD& ev : llsd::inArray(MGPhoneAuth::takeUnreadEvents()))
    {
        mReplay.append(ev);
    }
    if (key.isMap() && key.has("open_event"))
    {
        mOpenEvent = key["open_event"];
    }
    if (mLoaded && mShellReady)
    {
        deliverReplay(); // phone already running (e.g. toast clicked while it is open)
    }
    std::string dest = "app:home";
    if (key.isMap() && key.has("dest"))
    {
        dest = key["dest"].asString();
    }
    if (!mLoaded)
    {
        // Always open with the phone's proportions, fitted on screen (a saved rect
        // from an older layout may not match). The shell then sends its own size.
        LLSD state = LLSD().with("size", "m");
        applyShellState(state);
    }
    if (!mLoaded || (key.isMap() && key.has("dest")))
    {
        loadPhone(dest);
    }
}

void MGFloaterPhone::onClose(bool app_quitting)
{
    // Like the on-site popup: closing unloads the shell so nothing keeps polling.
    // Reopening signs in again silently (saved viewer token).
    if (mBrowser && !app_quitting)
    {
        mBrowser->navigateTo("about:blank", "text/html");
    }
    mLoaded = false;
    mShellReady = false;
    mLastSeq = -1;
    // <MG> the Camera's live changes (exposure, sky, freeze, guides) never outlive the phone
    if (!app_quitting)
    {
        MGPhoneAnims::onClose(); // <MG/> local test animations off
        MGPhoneWardrobe::onClose(); // <MG/> the 3D view off, a peek undone
        MGPhoneCamera::restore();
    }
    // </MG>
}

void MGFloaterPhone::deliverReplay()
{
    if (mReplay.size() == 0 && !mOpenEvent.isMap())
    {
        return;
    }
    LLSD msg;
    msg["type"] = "replay";
    msg["events"] = mReplay;
    if (mOpenEvent.isMap())
    {
        msg["open"] = mOpenEvent;
    }
    pushToPage(msg);
    mReplay = LLSD::emptyArray();
    mOpenEvent = LLSD();
}

void MGFloaterPhone::setStatus(const std::string& text)
{
    if (mStatus)
    {
        mStatus->setText(text);
        mStatus->setVisible(!text.empty());
    }
}

void MGFloaterPhone::loadPhone(const std::string& dest)
{
    if (!mBrowser)
    {
        return;
    }
    mLoaded = true;
    mShellReady = false; // wait for the new page's "hello" before handing over notifications
    mLastSeq = -1;
    setStatus(getString("signing_in"));

    LLHandle<LLFloater> handle = getHandle();
    MGPhoneAuth::requestSso(dest, [handle, dest](const std::string& sso)
        {
            MGFloaterPhone* self = handle.isDead() ? nullptr : dynamic_cast<MGFloaterPhone*>(handle.get());
            if (!self || !self->mBrowser || !self->mLoaded)
            {
                return;
            }
            std::string url = sso;
            if (url.empty())
            {
                // automatic sign-in unavailable: website login (viewer_launch.php)
                url = gSavedSettings.getString("MGPhoneURL");
                url += (url.find('?') == std::string::npos) ? "?" : "&";
                url += "dest=" + LLURI::escape(dest);
            }
            url += "&lang=" + LLURI::escape(LLUI::getLanguage());
            self->setStatus("");
            self->mBrowser->navigateTo(url, "text/html");
        });
}

void MGFloaterPhone::setMode(const std::string& size, bool landscape, bool tablet)
{
    PhoneMode m = modeFor(size, landscape, tablet);
    if (!landscape && !tablet)
    {
        // Portrait phone: lay the page out at MGPhoneCssWidth CSS px (default 360; lower =
        // bigger text). LLMediaCtrl applies the UI scale as page zoom, so the texture is
        // css width x UI scale, with the shell's 1:2 phone aspect.
        const F32 css_w = llclamp(gSavedSettings.getF32("MGPhoneCssWidth"), 300.f, 512.f);
        m.page_w = ll_round(css_w * LLUI::getScaleFactor().mV[VX]);
        m.page_h = m.page_w * 2;
    }
    mPageW = m.page_w;
    mPageH = m.page_h;

    // The page always lays out at its logical size; the screen shows it scaled.
    if (mBrowser)
    {
        mBrowser->setTextureSize(m.page_w, m.page_h);
    }

    S32 screen_w = m.screen_w;
    S32 screen_h = ll_round((F32)screen_w * (F32)m.page_h / (F32)m.page_w);

    // Scale the whole phone down (keeping its aspect) when it does not fit the space
    // available for floaters, e.g. a small or non-maximized viewer window.
    if (gFloaterView)
    {
        const LLRect avail = gFloaterView->getSnapRect();
        const S32 max_screen_h = avail.getHeight() - BEZEL_TOP - BEZEL_BOTTOM - 8;
        const S32 max_screen_w = avail.getWidth() - 2 * BEZEL_SIDE - 8;
        if (max_screen_h > 100 && screen_h > max_screen_h)
        {
            screen_h = max_screen_h;
            screen_w = ll_round((F32)screen_h * (F32)m.page_w / (F32)m.page_h);
        }
        if (max_screen_w > 100 && screen_w > max_screen_w)
        {
            screen_w = max_screen_w;
            screen_h = ll_round((F32)screen_w * (F32)m.page_h / (F32)m.page_w);
        }
    }

    LLRect r = getRect();
    r.setLeftTopAndSize(r.mLeft, r.mTop, screen_w + 2 * BEZEL_SIDE, screen_h + BEZEL_TOP + BEZEL_BOTTOM);
    if (gFloaterView)
    {
        // keep the whole phone inside the floater area (it grows downwards from its top)
        const LLRect avail = gFloaterView->getSnapRect();
        if (r.mBottom < avail.mBottom)
        {
            r.translate(0, avail.mBottom - r.mBottom);
        }
        if (r.mTop > avail.mTop)
        {
            r.translate(0, avail.mTop - r.mTop);
        }
        if (r.mRight > avail.mRight)
        {
            r.translate(avail.mRight - r.mRight, 0);
        }
        if (r.mLeft < avail.mLeft)
        {
            r.translate(avail.mLeft - r.mLeft, 0);
        }
    }
    mApplyingMode = true;
    setShape(r);
    mApplyingMode = false;
    LL_INFOS("MGPhone") << "phone mode " << size << (landscape ? " landscape" : "") << (tablet ? " tablet" : "")
                        << " rect " << getRect() << " avail " << (gFloaterView ? gFloaterView->getSnapRect() : LLRect())
                        << " floaterview " << (gFloaterView ? gFloaterView->getRect() : LLRect())
                        << " ui_scale " << LLUI::getScaleFactor() << LL_ENDL;
}

void MGFloaterPhone::handleReshape(const LLRect& new_rect, bool by_user)
{
    // A user resize keeps the phone's proportions: the screen width drives the height.
    if (by_user && !mApplyingMode && mPageW > 0)
    {
        const S32 screen_w = llmax(new_rect.getWidth() - 2 * BEZEL_SIDE, 120);
        const S32 screen_h = ll_round((F32)screen_w * (F32)mPageH / (F32)mPageW);
        LLRect r = new_rect;
        r.setLeftTopAndSize(new_rect.mLeft, new_rect.mTop, screen_w + 2 * BEZEL_SIDE, screen_h + BEZEL_TOP + BEZEL_BOTTOM);
        LLFloater::handleReshape(r, by_user);
        return;
    }
    LLFloater::handleReshape(new_rect, by_user);
}

void MGFloaterPhone::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLFloater::reshape(width, height, called_from_parent);
    // keep the grab pill centred in the top bezel
    if (LLView* grip = findChild<LLView>("phone_grip"))
    {
        const LLRect r = grip->getRect();
        grip->setOrigin((width - r.getWidth()) / 2, r.mBottom);
    }
}

void MGFloaterPhone::applyShellState(const LLSD& query)
{
    if (isOn(query, "minimized"))
    {
        // No docked bar in the viewer: minimize closes the panel, like the web popup.
        closeFloater();
        return;
    }
    const std::string size = query.has("size") ? query["size"].asString() : "m";
    setMode(size, isOn(query, "landscape"), isOn(query, "tablet"));
}

// ---------------------------------------------------------------- native bridge

void MGFloaterPhone::handleMediaEvent(LLPluginClassMedia* self, EMediaEvent event)
{
    if (event == MEDIA_EVENT_NAME_CHANGED && self)
    {
        handleBridgeTitle(self->getMediaName());
    }
    else if (event == MEDIA_EVENT_NAVIGATE_COMPLETE)
    {
        mLastSeq = -1;
        pushInfo(true);
    }
}

void MGFloaterPhone::handleBridgeTitle(const std::string& title)
{
    // "mgos:<seq>:<json>" set by MGOSNative.send() in the shell's top document.
    if (title.rfind("mgos:", 0) != 0)
    {
        return;
    }
    const size_t colon = title.find(':', 5);
    if (colon == std::string::npos)
    {
        return;
    }
    const S32 seq = atoi(title.substr(5, colon - 5).c_str());
    if (seq == mLastSeq)
    {
        return;
    }
    mLastSeq = seq;
    LLSD msg;
    try
    {
        msg = LlsdFromJson(boost::json::parse(title.substr(colon + 1)));
    }
    catch (...)
    {
        ackToPage(seq, LLSD().with("ok", false).with("error", "bad json"));
        return;
    }
    runCommand(msg["cmd"].asString(), msg["args"], seq);
}

void MGFloaterPhone::runCommand(const std::string& cmd, const LLSD& args, S32 seq)
{
    LLSD result = LLSD().with("ok", true);

    if (cmd == "hello")
    {
        pushInfo(true);
        mShellReady = true;
        deliverReplay();
        MGPhoneCalls::onShellReady(); // a call in progress or ringing
    }
    else if (MGPhoneCalls::isCommand(cmd))
    {
        result = MGPhoneCalls::runCommand(cmd, args);
    }
    // <MG> the Camera's pro capture controls (cam_*)
    else if (MGPhoneCamera::isCommand(cmd))
    {
        result = MGPhoneCamera::runCommand(cmd, args);
    }
    // the Textures app: browse / organize textures (inv_*)
    else if (MGPhoneInventory::isCommand(cmd))
    {
        result = MGPhoneInventory::runCommand(cmd, args);
    }
    // the Animations app: play / test animations, gestures, AO (anim_*, gest_*)
    else if (MGPhoneAnims::isCommand(cmd))
    {
        result = MGPhoneAnims::runCommand(cmd, args);
    }
    // the Wardrobe app: outfits, wear / take off, the 3D view (ward_*)
    else if (MGPhoneWardrobe::isCommand(cmd))
    {
        result = MGPhoneWardrobe::runCommand(cmd, args);
    }
    // the Places app: landmarks, favorites, teleport history, the place here (place_*)
    else if (MGPhonePlaces::isCommand(cmd))
    {
        result = MGPhonePlaces::runCommand(cmd, args);
    }
    // a new release: download, check, install over this one (update_*)
    else if (MGPhoneUpdate::isCommand(cmd))
    {
        result = MGPhoneUpdate::runCommand(cmd, args);
    }
    // </MG>
    else if (cmd == "state")
    {
        applyShellState(args);
    }
    else if (cmd == "say")
    {
        // Nearby chat as the avatar, exactly as if typed in the chat bar.
        const std::string text = args["text"].asString();
        if (!text.empty())
        {
            FSNearbyChat::instance().sendChatFromViewer(text, CHAT_TYPE_NORMAL, false);
        }
    }
    else if (cmd == "im")
    {
        LLUUID to;
        const std::string text = args["text"].asString();
        if (to.set(args["to"].asString(), false) && to.notNull() && !text.empty())
        {
            const LLUUID session = LLIMMgr::computeSessionID(IM_NOTHING_SPECIAL, to);
            LLIMModel::sendMessage(text, session, to, IM_NOTHING_SPECIAL);
        }
        else
        {
            result = LLSD().with("ok", false).with("error", "bad recipient or text");
        }
    }
    else if (cmd == "teleport")
    {
        // secondlife://Region/x/y/z or http(s)://maps.secondlife.com/secondlife/Region/x/y/z
        std::string slurl = args["slurl"].asString();
        const std::string maps = "maps.secondlife.com/secondlife/";
        const size_t at = slurl.find(maps);
        if (at != std::string::npos)
        {
            slurl = "secondlife:///app/teleport/" + slurl.substr(at + maps.size());
        }
        else if (slurl.rfind("secondlife://", 0) == 0 && slurl.find("/app/") == std::string::npos)
        {
            const size_t start = slurl.find_first_not_of('/', 13);
            slurl = (start == std::string::npos) ? std::string() : "secondlife:///app/teleport/" + slurl.substr(start);
        }
        if (slurl.rfind("secondlife:///app/teleport/", 0) == 0)
        {
            LLURLDispatcher::dispatch(slurl, LLCommandHandler::NAV_TYPE_CLICKED, nullptr, true);
        }
        else
        {
            result = LLSD().with("ok", false).with("error", "not a teleport address");
        }
    }
    else if (cmd == "snapshot")
    {
        sendSnapshot(seq, args);
        return; // acked with the image
    }
    else if (cmd == "camera_zoom")
    {
        // The Camera app's zoom: the 3D view's field of view, live (in degrees).
        // Not saved: camera_zoom_reset (or closing the capture screen) puts the
        // user's own view angle (CameraAngle) back.
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        if (args.has("fov"))
        {
            cam->setDefaultFOV((F32)args["fov"].asReal() * DEG_TO_RAD);
        }
        result["fov"] = cam->getDefaultFOV() * RAD_TO_DEG;
        result["min"] = cam->getMinView() * RAD_TO_DEG;
        result["max"] = cam->getMaxView() * RAD_TO_DEG;
        result["base"] = gSavedSettings.getF32("CameraAngle") * RAD_TO_DEG;
    }
    else if (cmd == "camera_zoom_reset")
    {
        LLViewerCamera::getInstance()->setDefaultFOV(gSavedSettings.getF32("CameraAngle"));
        result["fov"] = LLViewerCamera::getInstance()->getDefaultFOV() * RAD_TO_DEG;
    }
    else if (cmd == "camera_dof")
    {
        // depth of field ("portrait" blur) for the shot; the page restores it after
        if (args.has("on"))
        {
            gSavedSettings.setBOOL("RenderDepthOfField", args["on"].asBoolean());
        }
        result["on"] = gSavedSettings.getBOOL("RenderDepthOfField");
    }
    else if (cmd == "notify")
    {
        LLSD sub;
        sub["TITLE"] = args["title"].asString();
        sub["MESSAGE"] = args["body"].asString();
        LLSD payload;
        payload["dest"] = args["dest"].asString();
        LLNotificationsUtil::add("MGPhoneNotification", sub, payload, [](const LLSD& n, const LLSD& r)
            {
                if (LLNotificationsUtil::getSelectedOption(n, r) == 0)
                {
                    LLSD key = LLSD::emptyMap();
                    const std::string dest = n["payload"]["dest"].asString();
                    if (!dest.empty())
                    {
                        key["dest"] = dest;
                    }
                    LLFloaterReg::showInstance("mg_phone", key, true);
                }
                return false;
            });
    }
    else if (cmd == "openurl")
    {
        const std::string url = args["url"].asString();
        if (url.rfind("https://", 0) == 0)
        {
            LLWeb::loadURLExternal(url); // the viewer asks before opening the system browser
        }
    }
    else
    {
        result = LLSD().with("ok", false).with("error", "unknown command");
    }
    ackToPage(seq, result);
}

void MGFloaterPhone::ackToPage(S32 seq, const LLSD& result)
{
    if (mBrowser && mBrowser->getMediaPlugin())
    {
        mBrowser->getMediaPlugin()->executeJavaScript(
            "window.MGOSNative&&MGOSNative._ack(" + std::to_string(seq) + "," + toJson(result) + ");");
    }
}

void MGFloaterPhone::pushToPage(const LLSD& event)
{
    if (mBrowser && mBrowser->getMediaPlugin())
    {
        mBrowser->getMediaPlugin()->executeJavaScript(
            "window.MGOSNative&&MGOSNative._recv(" + toJson(event) + ");");
    }
}

void MGFloaterPhone::pushInfo(bool force)
{
    if (gAgentID.isNull())
    {
        return;
    }
    const S32 balance = gStatusBar ? gStatusBar->getBalance() : -1;
    LLViewerRegion* region = gAgent.getRegion();
    const std::string region_name = region ? region->getName() : "";
    const LLVector3 pos = gAgent.getPositionAgent();
    // position counts as a change after a 2 m move, so walking does not flood the page
    if (!force && balance == mLastBalance && region_name == mLastRegion && dist_vec(pos, mLastPos) < 2.f)
    {
        return;
    }
    mLastBalance = balance;
    mLastRegion = region_name;
    mLastPos = pos;

    LLSD info;
    info["type"] = "info";
    info["agent"] = gAgentID.asString();
    info["balance"] = balance;
    info["region"] = region_name;
    info["pos"] = LLSD().with("x", ll_round(pos.mV[VX])).with("y", ll_round(pos.mV[VY])).with("z", ll_round(pos.mV[VZ]));
    pushToPage(info);
}

void MGFloaterPhone::sendSnapshot(S32 seq, const LLSD& args)
{
    // The 3D view as a JPEG data URL. Options (Camera app):
    //   width   output width in px (64 up to the GPU's limit, default 1280; without a
    //           height the window aspect is kept; larger than the window renders off-screen)
    //   ui      include the viewer interface   hud  include HUDs (default: neither)
    //   balance show the L$ balance in the interface shot (default no)
    //   quality JPEG quality 60-95 (default 88)
    // <MG> pro capture (Camera app), all optional:
    //   height       with width: that exact size / aspect (a centered crop of the view,
    //                rendered at full resolution up to the GPU's limit, e.g. 4x the window)
    //   save         also write the full-size shot to the save folder
    //   format       png | jpg (the saved file)   name  file name pattern {date} {time} {n} {w} {h}
    //   send         return the image to the page (default yes)
    //   preview_max  longest side of the returned image (160-4096, default 2560)
    LLPointer<LLImageRaw> raw = new LLImageRaw;
    const S32 win_w = gViewerWindow->getWindowWidthRaw();
    const S32 win_h = gViewerWindow->getWindowHeightRaw();
    const S32 max_size = llmin((S32)MAX_SNAPSHOT_IMAGE_SIZE, (S32)gGLManager.mGLMaxTextureSize);
    S32 w = llclamp(args.has("width") ? args["width"].asInteger() : 1280, 64, max_size);
    S32 h;
    bool keep_aspect = true;
    if (args.has("height"))
    {
        h = llclamp(args["height"].asInteger(), 64, max_size);
        keep_aspect = false;
    }
    else
    {
        h = ll_round(win_h * (F32)w / (F32)llmax(1, win_w));
    }
    const bool show_ui = args["ui"].asBoolean();
    const bool show_hud = args["hud"].asBoolean();
    const bool show_balance = args["balance"].asBoolean();
    MGPhoneCamera::hideGuidesForShot(true);
    // rawSnapshot leaves an arrow cursor behind; the live view shoots every second
    const ECursorType cursor = gViewerWindow->getWindow() ? gViewerWindow->getWindow()->getCursor() : UI_CURSOR_ARROW;
    const bool shot = gViewerWindow->rawSnapshot(raw, w, h, keep_aspect, false, show_ui, show_hud, false, false, show_balance,
                                                 LLSnapshotModel::SNAPSHOT_TYPE_COLOR, max_size);
    gViewerWindow->setCursor(cursor);
    MGPhoneCamera::hideGuidesForShot(false);
    if (!shot)
    {
        ackToPage(seq, LLSD().with("ok", false).with("error", "snapshot failed"));
        return;
    }
    if (args.has("look"))
    {
        MGPhoneCamera::applyLook(raw, args["look"]);   // graded once: the saved file and the phone's copy match
    }
    LLSD result;
    result["ok"] = true;
    result["width"] = raw->getWidth();
    result["height"] = raw->getHeight();
    if (args["save"].asBoolean())
    {
        const std::string path = MGPhoneCamera::saveShot(raw, args["format"].asString(), args["name"].asString());
        if (!path.empty()) result["saved"] = path;
        else result["save_error"] = "could not write the file";
    }
    if (!args.has("send") || args["send"].asBoolean())
    {
        LLPointer<LLImageRaw> out = raw;
        const S32 pmax = llclamp(args.has("preview_max") ? args["preview_max"].asInteger() : 2560, 160, 4096);
        if (raw->getWidth() > pmax || raw->getHeight() > pmax)
        {
            const F32 k = (F32)pmax / (F32)llmax(raw->getWidth(), raw->getHeight());
            out = new LLImageRaw(raw->getData(), raw->getWidth(), raw->getHeight(), raw->getComponents());
            out->scale(llmax(1, ll_round(raw->getWidth() * k)), llmax(1, ll_round(raw->getHeight() * k)));
        }
        const S32 quality = llclamp(args.has("quality") ? args["quality"].asInteger() : 88, 50, 95);
        LLPointer<LLImageJPEG> jpeg = new LLImageJPEG(quality);
        if (!jpeg->encode(out, 0.f))
        {
            ackToPage(seq, LLSD().with("ok", false).with("error", "encode failed"));
            return;
        }
        result["image"] = "data:image/jpeg;base64," + LLBase64::encode(jpeg->getData(), jpeg->getDataSize());
    }
    // </MG>
    ackToPage(seq, result);
}

void MGFloaterPhone::draw()
{
    // the phone panel's media keeps its own volume and stays awake when the viewer is
    // minimized (llviewermedia.cpp asks MGPhoneCalls::isPhoneMedia)
    MGPhoneCalls::setPhoneMedia(mBrowser ? mBrowser->getMediaImpl() : nullptr);
    MGPhoneInventory::idle(); // <MG/> the Textures app's thumbnail jobs
    MGPhoneAnims::idle(); // <MG/> the Animations app's preview photos, gestures
    MGPhoneWardrobe::idle(); // <MG/> the Wardrobe app's peeks, cards, outfit photos
    MGPhonePlaces::idle(); // <MG/> the Places app's landmark / parcel lookups

    // live data for the shell: balance / region, checked once a second
    if (mLoaded && mInfoTimer.getElapsedTimeF32() > 1.f)
    {
        mInfoTimer.reset();
        pushInfo(false);
    }
    LLFloater::draw();
}

// secondlife:///app/mgphone/<verb>
//   open[?dest=app:xxx]   show the panel (optionally on an MGOS app route)
//   close                 hide it
//   state?...             window controls from the shell (see applyShellState)
//   openurl?url=https://  open the system browser (the viewer asks first)
// Every verb only affects the MG Phone panel or goes through the viewer's own
// confirmation, so untrusted pages may call it. Privileged actions (chat, IM,
// teleport, snapshot) use the title bridge, which only the phone page can reach.
class MGPhoneCommandHandler : public LLCommandHandler
{
public:
    MGPhoneCommandHandler() : LLCommandHandler("mgphone", UNTRUSTED_ALLOW) { }

    bool handle(const LLSD& params, const LLSD& query_map, const std::string& grid, LLMediaCtrl* web) override
    {
        const std::string verb = params.size() > 0 ? params[0].asString() : "open";

        if (verb == "open")
        {
            LLSD key = LLSD::emptyMap();
            if (query_map.has("dest"))
            {
                key["dest"] = query_map["dest"].asString();
            }
            LLFloaterReg::showInstance("mg_phone", key, true);
            return true;
        }
        if (verb == "close")
        {
            LLFloaterReg::hideInstance("mg_phone");
            return true;
        }
        if (verb == "state")
        {
            MGFloaterPhone* phone = LLFloaterReg::findTypedInstance<MGFloaterPhone>("mg_phone");
            if (phone && phone->getVisible())
            {
                phone->applyShellState(query_map);
            }
            return true;
        }
        if (verb == "openurl")
        {
            const std::string url = query_map["url"].asString();
            if (url.rfind("https://", 0) == 0)
            {
                LLWeb::loadURLExternal(url);
            }
            return true;
        }
        return false;
    }
};

MGPhoneCommandHandler gMGPhoneCommandHandler;
