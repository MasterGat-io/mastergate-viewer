/**
 * @file mgfloaterphone.h
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

#ifndef MG_FLOATERPHONE_H
#define MG_FLOATERPHONE_H

#include "llfloater.h"
#include "llframetimer.h"
#include "llviewermediaobserver.h"

class LLMediaCtrl;
class LLTextBox;

// Hosts the MGOS web shell (the MG Phone page, opened in viewer mode).
//  * Phone-shaped window: a bezel around a screen with the page's exact aspect; the
//    page lays out at its logical size (512x1024 phone, 1024x512 landscape,
//    1024x1024 tablet) and is scaled into the screen.
//  * Signs in automatically (MGPhoneAuth), falling back to the website login.
//  * Native bridge (only the phone's own top document can use it):
//      page -> viewer: document.title = "mgos:<seq>:<json {cmd,args}>"
//      viewer -> page: MGOSNative._recv({...}) / MGOSNative._ack(seq) via executeJavaScript
//    The page side is window.MGOSNative in the MGOS shell script.
class MGFloaterPhone : public LLFloater, public LLViewerMediaObserver
{
public:
    MGFloaterPhone(const LLSD& key);
    ~MGFloaterPhone();

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;
    void handleReshape(const LLRect& new_rect, bool by_user) override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;

    // LLViewerMediaObserver
    void handleMediaEvent(LLPluginClassMedia* self, EMediaEvent event) override;

    // query: minimized=0|1, size=s|m|l, landscape=0|1, tablet=0|1
    void applyShellState(const LLSD& query);

    // native -> page event
    void pushToPage(const LLSD& event);

    // the shell is loaded and listening (it said "hello")
    bool isShellReady() const { return mLoaded && mShellReady; }

private:
    void loadPhone(const std::string& dest);
    void setMode(const std::string& size, bool landscape, bool tablet);
    void setStatus(const std::string& text);
    void handleBridgeTitle(const std::string& title);
    void runCommand(const std::string& cmd, const LLSD& args, S32 seq);
    void pushInfo(bool force);
    void sendSnapshot(S32 seq, const LLSD& args);
    void ackToPage(S32 seq, const LLSD& result);
    void deliverReplay();

    LLMediaCtrl* mBrowser;
    LLTextBox*   mStatus;
    bool         mLoaded;
    bool         mApplyingMode;
    S32          mPageW;
    S32          mPageH;
    S32          mLastSeq;
    S32          mLastBalance;
    std::string  mLastRegion;
    LLVector3    mLastPos;
    bool         mShellReady;   // the shell said "hello" (MGOSNative listeners are in place)
    LLSD         mReplay;       // notifications to hand over (arrived while the phone was closed)
    LLSD         mOpenEvent;    // notification whose "Open phone" button was used
    LLFrameTimer mInfoTimer;
};

#endif // MG_FLOATERPHONE_H
