/**
 * @file mgphonecamera.cpp
 * @brief MasterGate Viewer: the MG Phone Camera's pro capture controls.
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

#include "mgphonecamera.h"

#include <array>
#include <ctime>
#include <map>
#include <set>

#include "llagent.h"
#include "llagentcamera.h"
#include "lldir.h"
#include "lldirpicker.h"
#include "llenvironment.h"
#include "llfile.h"
#include "llfloaterreg.h"
#include "llgl.h"
#include "llimagejpeg.h"
#include "llimagepng.h"
#include "llrender2dutils.h"
#include "llrootview.h"
#include "llsettingssky.h"
#include "llsettingswater.h"
#include "llview.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "mgfloaterphone.h"
#include "mgphoneposer.h"
// <MG> Camera Studio
#include "llappviewer.h"
#include "llcontrol.h"
#include "llframetimer.h"
#include "lltexturefetch.h"
#include "llviewercamera.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llspatialpartition.h"
#include "llfontgl.h" // <MG/> the AF distance
#include "llinventoryfunctions.h"
#include "llsdutil.h"
#include "llinventorymodel.h"
#include "llsettingsvo.h"
#include "llviewerinventory.h"
#include "llrendertarget.h"
#include "llvertexbuffer.h"
#include "llviewershadermgr.h"
#include "llfloater.h"
#include "pipeline.h"

extern bool gSnapshot;
extern bool gCubeSnapshot;
// </MG>

namespace
{
    // ---------------------------------------------------------------------
    // Framing guides: the shot's aspect (the rest of the window dimmed) and a
    // composition grid, drawn under every other window. Not in the photo: a
    // world-only shot renders without the interface, and an interface shot
    // hides them (hideGuidesForShot).
    // ---------------------------------------------------------------------
    class MGCameraGuides : public LLView
    {
    public:
        MGCameraGuides(const LLView::Params& p) : LLView(p), mAspect(0.f), mGrid(0), mAfShow(false), mAfHunt(false), mAfT(0.f), mAfDist(0.f), mFailT(-1.f) {}

        F32 mAspect;    // width / height, 0 = the window's
        S32 mGrid;      // 0 off, 1 thirds, 2 center, 3 golden, 4 diagonals
        // <MG> the autofocus box (UI coordinates): a face, or brackets on a point
        bool   mAfShow;
        bool   mAfHunt;     // the lens hunts first (not in manual focus)
        F32    mAfT;        // seconds since the focus was asked
        F32    mAfDist;     // metres, shown under the box once locked
        LLRect mAfBox;
        F32    mFailT;      // seconds since a focus failed (< 0: none)
        LLCoordGL mFailAt;
        // </MG>

        void draw() override
        {
            const S32 W = getRect().getWidth();
            const S32 H = getRect().getHeight();
            if (W <= 0 || H <= 0)
            {
                return;
            }
            S32 x0 = 0, y0 = 0, x1 = W, y1 = H;
            if (mAspect > 0.f)
            {
                if ((F32)W / (F32)H > mAspect)
                {
                    const S32 fw = ll_round(H * mAspect);
                    x0 = (W - fw) / 2; x1 = x0 + fw;
                }
                else
                {
                    const S32 fh = ll_round(W / mAspect);
                    y0 = (H - fh) / 2; y1 = y0 + fh;
                }
            }
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            const LLColor4 shade(0.f, 0.f, 0.f, 0.55f);
            if (x0 > 0) { gl_rect_2d(0, H, x0, 0, shade, true); gl_rect_2d(x1, H, W, 0, shade, true); }
            if (y0 > 0) { gl_rect_2d(0, y0, W, 0, shade, true); gl_rect_2d(0, H, W, y1, shade, true); }
            if (mAspect > 0.f)
            {
                gl_rect_2d(x0, y1, x1, y0, LLColor4(1.f, 1.f, 1.f, 0.6f), false);
            }
            const LLColor4 line(1.f, 1.f, 1.f, 0.38f);
            const S32 fw = x1 - x0, fh = y1 - y0;
            if (mGrid == 1 || mGrid == 3)
            {
                const F32 a = (mGrid == 1) ? (1.f / 3.f) : 0.382f;
                const F32 b = 1.f - a;
                gl_line_2d(x0 + ll_round(fw * a), y0, x0 + ll_round(fw * a), y1, line);
                gl_line_2d(x0 + ll_round(fw * b), y0, x0 + ll_round(fw * b), y1, line);
                gl_line_2d(x0, y0 + ll_round(fh * a), x1, y0 + ll_round(fh * a), line);
                gl_line_2d(x0, y0 + ll_round(fh * b), x1, y0 + ll_round(fh * b), line);
            }
            else if (mGrid == 2)
            {
                const S32 cx = x0 + fw / 2, cy = y0 + fh / 2, arm = llmin(fw, fh) / 14;
                gl_line_2d(cx - arm, cy, cx + arm, cy, line);
                gl_line_2d(cx, cy - arm, cx, cy + arm, line);
                gl_line_2d(x0 + fw / 2, y0, x0 + fw / 2, y1, LLColor4(1.f, 1.f, 1.f, 0.16f));
                gl_line_2d(x0, y0 + fh / 2, x1, y0 + fh / 2, LLColor4(1.f, 1.f, 1.f, 0.16f));
            }
            // <MG> diagonals: corner to corner, and the 45-degree lines of the frame's ends
            else if (mGrid == 4)
            {
                gl_line_2d(x0, y0, x1, y1, line);
                gl_line_2d(x0, y1, x1, y0, line);
                const S32 d = llmin(fw, fh);
                const LLColor4 faint(1.f, 1.f, 1.f, 0.2f);
                gl_line_2d(x0, y0, x0 + d, y0 + d, faint);
                gl_line_2d(x0, y1, x0 + d, y1 - d, faint);
                gl_line_2d(x1, y0, x1 - d, y0 + d, faint);
                gl_line_2d(x1, y1, x1 - d, y1 - d, faint);
            }
            // the AF box: white brackets that breathe while the lens hunts, then a snap and a
            // ping outwards as it locks (green), the distance under it; red blinks when it fails
            if (mAfShow && mAfBox.getWidth() > 2)
            {
                const F32 lock_at = mAfHunt ? HUNT_TIME : 0.f;
                const bool hunting = mAfT < lock_at;
                const F32 u = mAfT - lock_at;   // since the lock
                F32 scale = 1.f;
                if (hunting)
                {
                    scale = 1.f - 0.13f * (0.5f - 0.5f * cosf(mAfT / 0.18f * F_TWO_PI));
                }
                else if (u < 0.24f)
                {
                    const F32 v = u / 0.24f;
                    scale = (v < 0.5f) ? lerp(1.22f, 0.92f, v / 0.5f) : lerp(0.92f, 1.f, (v - 0.5f) / 0.5f);
                }
                const LLColor4 green(0.30f, 0.92f, 0.52f, 0.95f);
                const LLColor4 c = hunting ? LLColor4(1.f, 1.f, 1.f, 0.9f) : green;
                brackets(mAfBox, scale, c, 2);
                if (!hunting && u < 0.45f)
                {
                    // the ping: a ghost of the box flying outwards
                    const F32 g = u / 0.45f;
                    brackets(mAfBox, 1.f + 0.55f * g, LLColor4(0.45f, 1.f, 0.75f, 0.7f * (1.f - g) * (1.f - g)), 1);
                }
                if (!hunting && mAfDist > 0.f && u < 2.2f)
                {
                    const F32 a = (u < 1.6f) ? 1.f : 1.f - (u - 1.6f) / 0.6f;
                    const std::string txt = mAfDist >= 100.f ? std::string("\xE2\x88\x9E") : llformat(mAfDist < 10.f ? "%.1f m" : "%.0f m", mAfDist);
                    LLFontGL::getFontSansSerifSmall()->renderUTF8(txt, 0, (F32)mAfBox.getCenterX(), (F32)(mAfBox.mBottom - 6),
                        LLColor4(green.mV[0], green.mV[1], green.mV[2], a), LLFontGL::HCENTER, LLFontGL::TOP, LLFontGL::BOLD, LLFontGL::DROP_SHADOW_SOFT);
                }
            }
            if (mFailT >= 0.f && mFailT < 1.1f && fmodf(mFailT, 0.22f) < 0.13f)
            {
                const LLRect r(mFailAt.mX - 22, mFailAt.mY + 22, mFailAt.mX + 22, mFailAt.mY - 22);
                brackets(r, 1.f, LLColor4(1.f, 0.30f, 0.30f, 0.95f), 2);
            }
            // </MG>
            MGPhonePoser::drawMarkers(); // <MG/> the posing panel's joint dots
            LLView::draw();
        }

        static constexpr F32 HUNT_TIME = 0.36f;   // the lens hunts this long before it locks

    private:
        // four corner brackets around the box, scaled from its centre
        static void brackets(const LLRect& box, F32 scale, const LLColor4& c, S32 thick)
        {
            const F32 cx = (F32)box.getCenterX(), cy = (F32)box.getCenterY();
            const S32 hw = ll_round(box.getWidth() * 0.5f * scale), hh = ll_round(box.getHeight() * 0.5f * scale);
            const S32 l = ll_round(cx) - hw, r = ll_round(cx) + hw, t = ll_round(cy) + hh, b = ll_round(cy) - hh;
            const S32 k = llmax(6, llmin(r - l, t - b) / 4);
            for (S32 o = 0; o < thick; ++o)
            {
                gl_line_2d(l + o, t, l + o, t - k, c); gl_line_2d(l, t - o, l + k, t - o, c);
                gl_line_2d(r - o, t, r - o, t - k, c); gl_line_2d(r, t - o, r - k, t - o, c);
                gl_line_2d(l + o, b, l + o, b + k, c); gl_line_2d(l, b + o, l + k, b + o, c);
                gl_line_2d(r - o, b, r - o, b + k, c); gl_line_2d(r, b + o, r - k, b + o, c);
            }
        }
    };

    MGCameraGuides* sGuides = nullptr;
    std::string sGuideAspect = "native";
    std::string sGuideGrid = "off";
    LLRect sViewRect;   // <MG/> the viewfinder's area (UI coordinates), empty when off

    F32 aspectOf(const std::string& a)
    {
        const size_t c = a.find(':');
        if (c == std::string::npos)
        {
            return 0.f;
        }
        const F32 w = (F32)atof(a.substr(0, c).c_str());
        const F32 h = (F32)atof(a.substr(c + 1).c_str());
        return (w > 0.f && h > 0.f) ? w / h : 0.f;
    }

    void setGuides(const std::string& aspect, const std::string& grid)
    {
        sGuideAspect = aspect;
        sGuideGrid = grid;
        const F32 a = aspectOf(aspect);
        const S32 g = (grid == "thirds") ? 1 : (grid == "center") ? 2 : (grid == "golden") ? 3 : (grid == "diagonals") ? 4 : 0;
        if (a <= 0.f && g == 0 && sViewRect.isEmpty()) // <MG/> the viewfinder keeps them for the AF box
        {
            if (sGuides) sGuides->setVisible(false);
            return;
        }
        if (!sGuides && gViewerWindow && gViewerWindow->getRootView())
        {
            LLView::Params p;
            p.name = "mg_camera_guides";
            p.rect = gViewerWindow->getRootView()->getLocalRect();
            p.mouse_opaque = false;
            p.follows.flags = FOLLOWS_ALL;
            sGuides = new MGCameraGuides(p);
            gViewerWindow->getRootView()->addChild(sGuides);
            gViewerWindow->getRootView()->sendChildToBack(sGuides);
        }
        if (sGuides)
        {
            sGuides->mAspect = a;
            sGuides->mGrid = g;
            sGuides->setVisible(true);
        }
    }

    // ---------------------------------------------------------------------
    // Exposure / freeze: remembered once, put back by restore()
    // ---------------------------------------------------------------------
    bool sExposureSaved = false;
    F32  sExposureBase = 1.f;
    bool sFreezeSaved = false;
    bool sFreezePrev = false;
    bool sViewMoved = false;

    // ---------------------------------------------------------------------
    // Light: a local copy of the sky the user sees, changed live (like the
    // Personal Lighting floater), dropped by restore() / cam_env_reset
    // ---------------------------------------------------------------------
    struct EnvState
    {
        bool active = false;
        LLEnvironment::EnvSelection_t prevSelected = LLEnvironment::ENV_DEFAULT;
        bool prevLocal = false;
        LLSettingsDay::ptr_t prevDay;
        LLSettingsDay::Seconds prevDayLength;
        LLSettingsDay::Seconds prevDayOffset;
        LLSettingsSky::ptr_t prevSky;
        LLSettingsWater::ptr_t prevWater;
        LLSettingsSky::ptr_t live;
        LLSettingsWater::ptr_t liveWater;
        F32 hazeDensity = 0.f, hazeHorizon = 0.f, densityMult = 0.f;
        F32 hour = -1.f;
        std::string air = "original";
        bool dayMode = false;           // <MG/> a day cycle preset runs: the sliders re-clone the sky
        bool cloudsFrozen = false;      // <MG/>
        LLVector2 cloudRate;            // <MG/> before the freeze
    };
    bool sEnvKeep = false;              // <MG/> "Keep this light": not undone when the camera closes
    EnvState sEnv;

    bool captureEnvironment()
    {
        if (sEnv.active)
        {
            return true;
        }
        LLEnvironment& env = LLEnvironment::instance();
        LLSettingsSky::ptr_t sky = env.getCurrentSky();
        LLSettingsWater::ptr_t water = env.getCurrentWater();
        if (!sky || !water)
        {
            return false;
        }
        sEnv = EnvState();
        sEnv.prevSelected = env.getSelectedEnvironment();
        sEnv.prevLocal = env.hasEnvironment(LLEnvironment::ENV_LOCAL);
        if (sEnv.prevLocal)
        {
            sEnv.prevDay = env.getEnvironmentDay(LLEnvironment::ENV_LOCAL);
            sEnv.prevDayLength = env.getEnvironmentDayLength(LLEnvironment::ENV_LOCAL);
            sEnv.prevDayOffset = env.getEnvironmentDayOffset(LLEnvironment::ENV_LOCAL);
            sEnv.prevSky = env.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL);
            sEnv.prevWater = env.getEnvironmentFixedWater(LLEnvironment::ENV_LOCAL);
        }
        sEnv.live = sky->buildClone();
        sEnv.liveWater = water->buildClone();
        if (!sEnv.live || !sEnv.liveWater)
        {
            return false;
        }
        sEnv.hazeDensity = sEnv.live->getHazeDensity();
        sEnv.hazeHorizon = sEnv.live->getHazeHorizon();
        sEnv.densityMult = sEnv.live->getDensityMultiplier();
        env.setEnvironment(LLEnvironment::ENV_LOCAL, sEnv.live, sEnv.liveWater);
        env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
        sEnv.active = true;
        return true;
    }

    void releaseEnvironment()
    {
        if (!sEnv.active)
        {
            return;
        }
        if (sEnvKeep)
        {
            // <MG> the user kept this light: it stays as the local environment
            sEnvKeep = false;
            sEnv = EnvState();
            return;
        }
        LLEnvironment& env = LLEnvironment::instance();
        if (sEnv.prevLocal && sEnv.prevDay)
        {
            env.setEnvironment(LLEnvironment::ENV_LOCAL, sEnv.prevDay, sEnv.prevDayLength, sEnv.prevDayOffset);
        }
        else if (sEnv.prevLocal && (sEnv.prevSky || sEnv.prevWater))
        {
            env.setEnvironment(LLEnvironment::ENV_LOCAL, sEnv.prevSky, sEnv.prevWater);
        }
        else
        {
            env.clearEnvironment(LLEnvironment::ENV_LOCAL);
        }
        env.setSelectedEnvironment(sEnv.prevSelected, LLEnvironment::TRANSITION_INSTANT);
        env.updateEnvironment(LLEnvironment::TRANSITION_INSTANT);
        sEnv = EnvState();
    }

    // time of day -> the sun on an east-to-west arc tilted 25 degrees south;
    // the moon opposite. 6:00 sunrise, 12:00 highest, 18:00 sunset.
    void applyHour(F32 hour)
    {
        hour = fmodf(fmodf(hour, 24.f) + 24.f, 24.f);
        const F32 theta = (hour - 6.f) / 12.f * F_PI;
        LLQuaternion arc, tilt, moonArc;
        arc.setAngleAxis(-theta, 0.f, 1.f, 0.f);
        tilt.setAngleAxis(25.f * DEG_TO_RAD, 1.f, 0.f, 0.f);
        moonArc.setAngleAxis(-(theta + F_PI), 0.f, 1.f, 0.f);
        sEnv.live->setSunRotation(arc * tilt);
        sEnv.live->setMoonRotation(moonArc * tilt);
        sEnv.live->update();
        sEnv.hour = hour;
    }

    F32 currentHour()
    {
        if (sEnv.active && sEnv.hour >= 0.f)
        {
            return sEnv.hour;
        }
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        if (!sky)
        {
            return 12.f;
        }
        const LLVector3 d = LLVector3::x_axis * sky->getSunRotation();
        F32 hour = 6.f + atan2f(d.mV[VZ], d.mV[VX]) / F_PI * 12.f;
        return fmodf(hour + 24.f, 24.f);
    }

    void applyAir(const std::string& mode)
    {
        F32 hd = 1.f, hh = 1.f, dm = 1.f;
        if (mode == "clear")      { hd = 0.35f; hh = 0.6f; dm = 0.7f; }
        else if (mode == "hazy")  { hd = 1.9f;  hh = 1.7f; dm = 1.4f; }
        else if (mode == "misty") { hd = 3.2f;  hh = 2.6f; dm = 2.4f; }
        sEnv.live->setHazeDensity(llclamp(sEnv.hazeDensity * hd, 0.f, 10.f));
        sEnv.live->setHazeHorizon(llclamp(sEnv.hazeHorizon * hh, 0.f, 10.f));
        sEnv.live->setDensityMultiplier(llclamp(sEnv.densityMult * dm, 0.0001f, 2.f));
        sEnv.live->update();
        sEnv.air = (mode == "clear" || mode == "hazy" || mode == "misty") ? mode : "original";
    }

    // ---- <MG> world light: presets from the inventory, the sun, the atmosphere
    void envReclone()
    {
        // after a day cycle preset, the sliders work on a copy of the sky shown now
        if (!sEnv.dayMode) return;
        LLEnvironment& env = LLEnvironment::instance();
        LLSettingsSky::ptr_t sky = env.getCurrentSky();
        LLSettingsWater::ptr_t water = env.getCurrentWater();
        if (!sky || !water) return;
        sEnv.live = sky->buildClone();
        sEnv.liveWater = water->buildClone();
        sEnv.hazeDensity = sEnv.live->getHazeDensity();
        sEnv.hazeHorizon = sEnv.live->getHazeHorizon();
        sEnv.densityMult = sEnv.live->getDensityMultiplier();
        env.setEnvironment(LLEnvironment::ENV_LOCAL, sEnv.live, sEnv.liveWater);
        env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
        sEnv.dayMode = false;
    }

    LLSD envList()
    {
        LLSD out;
        out["skies"] = LLSD::emptyArray();
        out["waters"] = LLSD::emptyArray();
        out["days"] = LLSD::emptyArray();
        const LLUUID roots[2] = { gInventory.getRootFolderID(), gInventory.getLibraryRootFolderID() };
        for (S32 r = 0; r < 2; ++r)
        {
            if (roots[r].isNull()) continue;
            LLInventoryModel::cat_array_t cats;
            LLInventoryModel::item_array_t items;
            LLIsType is_settings(LLAssetType::AT_SETTINGS);
            gInventory.collectDescendentsIf(roots[r], cats, items, LLInventoryModel::EXCLUDE_TRASH, is_settings);
            for (const LLPointer<LLViewerInventoryItem>& item : items)
            {
                if (!item || item->getAssetUUID().isNull()) continue;
                const LLSettingsType::type_e t = item->getSettingsType();
                const char* key = (t == LLSettingsType::ST_SKY) ? "skies" : (t == LLSettingsType::ST_WATER) ? "waters" : (t == LLSettingsType::ST_DAYCYCLE) ? "days" : nullptr;
                if (!key || out[key].size() >= 400) continue;
                out[key].append(LLSD().with("id", item->getAssetUUID()).with("name", item->getName()).with("lib", r == 1));
            }
        }
        return out;
    }

    LLSD envState()
    {
        LLSD s;
        LLSettingsSky::ptr_t sky = (sEnv.active && sEnv.live && !sEnv.dayMode) ? sEnv.live : LLEnvironment::instance().getCurrentSky();
        if (sky)
        {
            const LLVector3 d = LLVector3::x_axis * sky->getSunRotation();
            s["az"] = fmodf(atan2f(d.mV[VY], d.mV[VX]) * RAD_TO_DEG + 360.f, 360.f);
            s["el"] = asinf(llclamp(d.mV[VZ], -1.f, 1.f)) * RAD_TO_DEG;
            s["haze"] = sky->getHazeDensity();
            s["horizon"] = sky->getHazeHorizon();
            s["density"] = sky->getDensityMultiplier() * 1000.f;
            s["distance"] = sky->getDistanceMultiplier();
            s["gamma"] = sky->getGamma();
            s["clouds"] = sky->getCloudShadow();
            s["stars"] = sky->getStarBrightness();
        }
        s["clouds_frozen"] = sEnv.cloudsFrozen;
        s["facelights"] = gSavedSettings.getBOOL("RenderAttachedLights");
        s["local"] = sEnv.active;
        s["day"] = sEnv.dayMode;
        return s;
    }

    void pushEnv(const LLSD& extra = LLSD())
    {
        MGFloaterPhone* phone = LLFloaterReg::findTypedInstance<MGFloaterPhone>("mg_phone");
        if (!phone) return;
        LLSD ev = envState();
        ev["type"] = "cam_env";
        if (extra.isMap()) for (const auto& kv : llsd::inMap(extra)) ev[kv.first] = kv.second;
        phone->pushToPage(ev);
    }

    F32 currentEv()
    {
        const F32 cur = gSavedSettings.getF32("RenderExposure");
        const F32 base = sExposureSaved ? sExposureBase : cur;
        return (base > 0.f && cur > 0.f) ? log2f(cur / base) : 0.f;
    }

    S32 maxShotSize()
    {
        return llmin((S32)MAX_SNAPSHOT_IMAGE_SIZE, (S32)gGLManager.mGLMaxTextureSize);
    }

    LLSD stateOf()
    {
        LLSD s;
        s["ok"] = true;
        s["window"] = LLSD().with("w", gViewerWindow ? gViewerWindow->getWindowWidthRaw() : 0)
                            .with("h", gViewerWindow ? gViewerWindow->getWindowHeightRaw() : 0);
        s["max"] = maxShotSize();
        s["ev"] = currentEv();
        s["freeze"] = gSavedSettings.getBOOL("FreezeTime");
        s["hour"] = currentHour();
        s["air"] = sEnv.air;
        s["env_local"] = sEnv.active;
        s["dof"] = gSavedSettings.getBOOL("RenderDepthOfField");
        s["guides"] = LLSD().with("aspect", sGuideAspect).with("grid", sGuideGrid);
        s["folder"] = MGPhoneCamera::saveFolder();
        return s;
    }

    std::string sanitizeName(std::string s)
    {
        static const std::string bad = "\\/:*?\"<>|";
        for (char& c : s)
        {
            if (bad.find(c) != std::string::npos || (unsigned char)c < 32) c = '_';
        }
        while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
        return s.substr(0, 120);
    }

    void replaceAll(std::string& s, const std::string& from, const std::string& to)
    {
        size_t at = 0;
        while ((at = s.find(from, at)) != std::string::npos)
        {
            s.replace(at, from.size(), to);
            at += to.size();
        }
    }
}

// <MG> ================================================================ Camera Studio
namespace
{
    // Settings changed for the viewfinder, remembered once and put back by
    // restore() / restoreSettings(): the user's own values are never lost.
    std::map<std::string, LLSD> sSaved;

    void tempSet(const std::string& name, const LLSD& value)
    {
        LLControlVariable* c = gSavedSettings.getControl(name);
        if (!c)
        {
            return;
        }
        if (sSaved.find(name) == sSaved.end())
        {
            sSaved[name] = c->getValue();
        }
        c->setValue(value);
    }

    void tempRestore(const std::string& name)
    {
        auto it = sSaved.find(name);
        if (it == sSaved.end())
        {
            return;
        }
        if (LLControlVariable* c = gSavedSettings.getControl(name))
        {
            c->setValue(it->second);
        }
        sSaved.erase(it);
    }

    // the photo quality boost (Aurora's "Photo quality"): only ever raises a value
    struct Boost { const char* name; F32 value; };
    const Boost QUALITY[] = {
        { "RenderFarClip", 512.f },
        { "RenderVolumeLODFactor", 4.f },
        { "RenderTerrainLODFactor", 2.f },
        { "RenderTreeLODFactor", 1.f },
        { "RenderShadowResolutionScale", 1.5f },
        { "RenderReflectionProbeDetail", 2.f },
        { "RenderLocalLightCount", 512.f },
        { "RenderMaxPartCount", 8192.f },
        { "RenderAvatarMaxNonImpostors", 50.f },
    };
    bool sQuality = false;

    void setQuality(bool on)
    {
        if (on == sQuality)
        {
            return;
        }
        sQuality = on;
        for (const Boost& b : QUALITY)
        {
            if (!on)
            {
                tempRestore(b.name);
                continue;
            }
            LLControlVariable* c = gSavedSettings.getControl(b.name);
            if (!c) continue;
            const F32 cur = (F32)c->getValue().asReal();
            if (cur < b.value)
            {
                if (c->type() == TYPE_F32) tempSet(b.name, LLSD((F64)b.value));
                else tempSet(b.name, LLSD((S32)b.value));
            }
        }
        const char* flags[] = { "RenderScreenSpaceReflections", "RenderDeferredSSAO" };
        for (const char* f : flags)
        {
            if (on) tempSet(f, LLSD(true)); else tempRestore(f);
        }
        if (on)
        {
            tempSet("RenderAvatarMaxComplexity", LLSD(0));   // every avatar in full
            if (gSavedSettings.getS32("RenderShadowDetail") < 2) tempSet("RenderShadowDetail", LLSD(2));
            if (gSavedSettings.getU32("RenderMaxTextureResolution") < 2048) tempSet("RenderMaxTextureResolution", LLSD(2048));
        }
        else
        {
            tempRestore("RenderAvatarMaxComplexity");
            tempRestore("RenderShadowDetail");
            tempRestore("RenderMaxTextureResolution");
        }
    }

    // ---- the lens: Firestorm's depth of field takes the focal length from the
    // field of view against a reference (CameraFocalLength at CameraFieldOfView).
    // A 50 mm lens at 26.99 degrees is a 24 mm high (full frame) sensor, so the
    // focal length chosen here is the one the blur is computed with.
    constexpr F32 SENSOR_HALF_H = 12.f;   // mm
    bool sFovChanged = false;

    void lensOn()
    {
        tempSet("CameraFocalLength", LLSD(50.0));
        tempSet("CameraFieldOfView", LLSD(26.99));
        // <MG> depth of field at full resolution for photos: at the default 0.7 the blur buffer
        // mixes the subject's edge pixels into the background, which the blur then spreads as a
        // soft glow around hair and shoulders
        tempSet("CameraDoFResScale", LLSD(1.0));
    }

    F32 focalOfFov(F32 fov_rad) { return SENSOR_HALF_H / tanf(llmax(0.01f, fov_rad) * 0.5f); }
    F32 fovOfFocal(F32 focal_mm) { return 2.f * atanf(SENSOR_HALF_H / llmax(1.f, focal_mm)); }

    // ---- autofocus
    struct AfState
    {
        bool active = false;
        std::string mode = "s";   // s single, c continuous, m manual
        bool face = false;
        LLUUID avatar;            // the face held
        LLVector3 point;          // agent coordinates
        F32 sx = 0.5f, sy = 0.5f; // the AF point (fractions of the view), AF-C on a point
        LLFrameTimer since;
        LLFrameTimer track;
        bool hunt = false;        // <MG/> the lens hunts (overshoot, back, lock) before holding
        F32 fromDist = 16.f;      // <MG/> where the focus was (m)
    };
    AfState sAf;
    LLFrameTimer sAfFail;         // <MG/> a focus that failed: red brackets there
    bool sAfFailOn = false;
    F32 sAfFailX = 0.5f, sAfFailY = 0.5f;
    S32 sLensDof = -1;            // <MG/> the studio's depth of field (-1: never set)

    // the lens hunting: through the target, back a little, then on it (in dioptres, like a lens)
    LLVector3 huntPoint(const LLVector3& target, F32 from_dist, F32 t)
    {
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        const F32 dt = (target - cam->getOrigin()) * cam->getAtAxis();
        if (dt <= 0.05f)
        {
            return target;
        }
        const F32 Dt = 1.f / dt;
        F32 Df = 1.f / llmax(from_dist, 0.3f);
        if (fabsf(Df - Dt) < 0.2f * Dt)
        {
            Df = Dt * 1.35f;   // already close: it still searches a little
        }
        const F32 Dpast = llmax(0.001f, Dt - 0.35f * (Df - Dt));
        const F32 Dback = Dt + 0.1f * (Df - Dt);
        auto ease = [](F32 x) { x = llclamp(x, 0.f, 1.f); return x * x * (3.f - 2.f * x); };
        F32 D;
        if (t < 0.15f) D = lerp(Df, Dpast, ease(t / 0.15f));
        else if (t < 0.28f) D = lerp(Dpast, Dback, ease((t - 0.15f) / 0.13f));
        else D = lerp(Dback, Dt, ease((t - 0.28f) / 0.08f));
        return cam->getOrigin() + (target - cam->getOrigin()) * ((1.f / D) / dt);
    }

    LLVOAvatar* avatarOf(LLViewerObject* obj)
    {
        if (!obj) return nullptr;
        if (obj->isAvatar()) return (LLVOAvatar*)obj;
        return obj->getAvatar();
    }

    LLVOAvatar* findAvatar(const LLUUID& id)
    {
        if (id.isNull()) return nullptr;
        LLViewerObject* obj = gObjectList.findObject(id);
        return (obj && obj->isAvatar() && !obj->isDead()) ? (LLVOAvatar*)obj : nullptr;
    }

    // between the eyes (the head when an avatar has no eye joints)
    LLVector3 eyesOf(LLVOAvatar* av)
    {
        if (av->mEyeLeftp && av->mEyeRightp)
        {
            return (av->mEyeLeftp->getWorldPosition() + av->mEyeRightp->getWorldPosition()) * 0.5f;
        }
        return av->mHeadp ? av->mHeadp->getWorldPosition() + LLVector3(0.f, 0.f, 0.08f) : av->getPositionAgent();
    }

    void setFocusPoint(const LLVector3& p)
    {
        LLPipeline::sLastFocusPoint = p;
        tempSet("FSFocusPointFollowsPointer", LLSD(false));
        tempSet("FSFocusPointLocked", LLSD(true));
    }

    F32 distanceTo(const LLVector3& p)
    {
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        return (p - cam->getOrigin()) * cam->getAtAxis();
    }

    // a pick at fractions of the view; true when something was hit
    bool pickAt(F32 fx, F32 fy, LLVector3& point, LLVOAvatar*& avatar)
    {
        avatar = nullptr;
        LLRect r = sViewRect;
        if (r.isEmpty() && gViewerWindow) r = gViewerWindow->getWorldViewRectScaled();
        const S32 x = r.mLeft + ll_round(llclamp(fx, 0.f, 1.f) * r.getWidth());
        const S32 y = r.mTop - ll_round(llclamp(fy, 0.f, 1.f) * r.getHeight());
        LLPickInfo pick = gViewerWindow->pickImmediate(x, y, false, true, false);
        if (pick.mPickType == LLPickInfo::PICK_INVALID || pick.mPosGlobal.isExactlyZero())
        {
            return false;
        }
        point = gAgent.getPosAgentFromGlobal(pick.mPosGlobal);
        avatar = avatarOf(pick.getObject().get());
        return true;
    }

    // ---- meter (histogram): a 160x90 copy of the finished scene, read back a
    // quarter of a second later through a pixel buffer (no stall)
    constexpr S32 METER_W = 160, METER_H = 90;
    bool sMeterOn = false;
    bool sMeterPending = false;
    GLuint sMeterFbo = 0, sMeterRbo = 0, sMeterPbo = 0;
    LLFrameTimer sMeterTimer;
    LLSD sMeterResult;
    bool sMeterFresh = false;
    LLFrameTimer sPushTimer;

    void meterFree()
    {
        if (sMeterPbo) glDeleteBuffers(1, &sMeterPbo);
        if (sMeterFbo) glDeleteFramebuffers(1, &sMeterFbo);
        if (sMeterRbo) glDeleteRenderbuffers(1, &sMeterRbo);
        sMeterPbo = sMeterFbo = sMeterRbo = 0;
        sMeterPending = false;
    }

    void meterRead(const U8* px)
    {
        LLSD hist = LLSD::emptyArray();
        S32 bins[64] = { 0 };
        S32 hi = 0, lo = 0;
        F64 sum = 0.0;
        const S32 n = METER_W * METER_H;
        for (S32 i = 0; i < n; ++i)
        {
            const U8* p = px + i * 4;
            const S32 l = (S32)(0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2] + 0.5f);
            bins[llclamp(l >> 2, 0, 63)]++;
            if (p[0] >= 250 || p[1] >= 250 || p[2] >= 250) ++hi;
            if (l <= 5) ++lo;
            sum += l;
        }
        for (S32 b = 0; b < 64; ++b) hist.append(bins[b]);
        sMeterResult = LLSD::emptyMap();
        sMeterResult["hist"] = hist;
        sMeterResult["clip_hi"] = (F64)hi / n;
        sMeterResult["clip_lo"] = (F64)lo / n;
        sMeterResult["mean"] = sum / n / 255.0;
        sMeterFresh = true;
    }

    // ---- downloads the page announced (edited photos to the PC)
    std::string sArmName;
    LLFrameTimer sArmTimer;

    bool sHudsHidden = false;
    bool sHudsWere = true;
    bool sRolled = false; // cam_move tilted the camera

    // ---- live looks (mgLookF.glsl)
    struct LookState
    {
        S32 layers = 0;
        F32 a[16] = { 0 }, b[16] = { 0 }, c[16] = { 0 }, d[16] = { 0 }, e[16] = { 0 };
        F32 fx[4] = { 0 }, fx2[4] = { 0, 0, -1.f, 0 };
        bool any = false;
    };
    LookState sLook;
    bool sLookSuspended = false;
    bool sMatteShot = false;   // <MG/> the cut-out's two background renders: no look in them
    bool sShotTile = false;
    F32 sTile[4] = { 0, 0, 1, 1 };
    LLFrameTimer sLookClock;

    void readVec4(const LLSD& v, F32* out)
    {
        for (S32 k = 0; k < 4; ++k) out[k] = (F32)v[k].asReal();
    }

    void setLook(const LLSD& args)
    {
        sLook = LookState();
        const LLSD& layers = args["layers"];
        for (S32 i = 0; i < 4 && i < (S32)layers.size(); ++i)
        {
            readVec4(layers[i]["a"], sLook.a + i * 4);
            readVec4(layers[i]["b"], sLook.b + i * 4);
            readVec4(layers[i]["c"], sLook.c + i * 4);
            readVec4(layers[i]["d"], sLook.d + i * 4);
            readVec4(layers[i]["e"], sLook.e + i * 4);
            sLook.layers = i + 1;
        }
        if (args.has("fx")) readVec4(args["fx"], sLook.fx);
        if (args.has("fx2")) readVec4(args["fx2"], sLook.fx2);
        if (!args.has("fx2")) sLook.fx2[2] = -1.f;
        sLook.any = sLook.layers > 0 || sLook.fx[0] > 0.f || sLook.fx[1] > 0.f || sLook.fx[2] > 0.f || sLook.fx[3] > 0.f
                    || sLook.fx2[0] > 0.f || sLook.fx2[1] > 0.f || sLook.fx2[2] >= 0.f;
    }

    // the photo's rectangle in the window (uv): the guides' aspect, centred
    void liveFrame(F32 out[4])
    {
        out[0] = 0.f; out[1] = 0.f; out[2] = 1.f; out[3] = 1.f;
        const F32 a = aspectOf(sGuideAspect);
        if (a <= 0.f || !gViewerWindow) return;
        const F32 W = (F32)llmax(1, gViewerWindow->getWindowWidthRaw()), H = (F32)llmax(1, gViewerWindow->getWindowHeightRaw());
        if (W / H > a) { out[2] = (H * a) / W; out[0] = (1.f - out[2]) * 0.5f; }
        else { out[3] = (W / a) / H; out[1] = (1.f - out[3]) * 0.5f; }
    }

    // ---- isolation: the subject (my avatar and what it wears) alone
    bool sIsolate = false;
    bool sIsoWhite = false;
    std::set<LLUUID> sIsoHidden;          // objects we hid (not those already derendered)
    std::vector<U32> sIsoTypesOff;        // render types we switched off
    bool sIsoTerrain = false;
    LLFrameTimer sIsoTimer;

    const U32 ISO_TYPES[] = { LLPipeline::RENDER_TYPE_SKY, LLPipeline::RENDER_TYPE_WL_SKY, LLPipeline::RENDER_TYPE_CLOUDS,
                              LLPipeline::RENDER_TYPE_WATER, LLPipeline::RENDER_TYPE_VOIDWATER, LLPipeline::RENDER_TYPE_PARTICLES };

    bool isSubject(LLViewerObject* o)
    {
        if (o->isHUDAttachment()) return true;
        LLVOAvatar* av = o->isAvatar() ? (LLVOAvatar*)o : o->getAvatarAncestor();
        return av && av->isSelf();
    }

    // hide / show one object for the isolation: the flag alone is not enough, the
    // geometry is batched per spatial group (LLVolumeGeometryManager::rebuildGeom skips
    // FORCE_INVISIBLE drawables only when the group is rebuilt)
    void isoSetHidden(LLDrawable* d, bool hide)
    {
        if (!d || d->isDead())
        {
            return;
        }
        if (hide) d->setState(LLDrawable::FORCE_INVISIBLE);
        else d->clearState(LLDrawable::FORCE_INVISIBLE);
        gPipeline.markRebuild(d, LLDrawable::REBUILD_ALL);
        if (LLSpatialGroup* g = d->getSpatialGroup())
        {
            g->dirtyGeom();
            gPipeline.markRebuild(g);
        }
    }

    // hide everything that is not the subject (run again every second: objects stream in,
    // and an object update clears the flag unless the object is ours, see isIsoHidden)
    void isoSweep()
    {
        const S32 n = gObjectList.getNumObjects();
        for (S32 i = 0; i < n; ++i)
        {
            LLViewerObject* o = gObjectList.getObject(i);
            if (!o || o->isDead() || !o->mDrawable || isSubject(o))
            {
                continue;
            }
            LLDrawable* d = o->mDrawable;
            const bool ours = sIsoHidden.count(o->getID()) > 0;
            if (d->isState(LLDrawable::FORCE_INVISIBLE))
            {
                continue; // hidden (by us, or derendered by the user: left as it is)
            }
            isoSetHidden(d, true);
            if (!ours) sIsoHidden.insert(o->getID());
        }
    }

    void setIsolate(bool on)
    {
        if (on == sIsolate)
        {
            return;
        }
        sIsolate = on;
        sIsoWhite = false;
        if (on)
        {
            for (U32 t : ISO_TYPES)
            {
                if (gPipeline.hasRenderType(t))
                {
                    LLPipeline::toggleRenderType(t);
                    sIsoTypesOff.push_back(t);
                }
            }
            gPipeline.skipRenderingOfTerrain(true);
            sIsoTerrain = true;
            // clean edges and the same exposure for the two renders of a transparent shot
            tempSet("RenderDynamicExposureEnabled", LLSD(false));
            tempSet("RenderGlow", LLSD(false));
            tempSet("RenderDepthOfField", LLSD(false));
            isoSweep();
            sIsoTimer.reset();
            return;
        }
        for (U32 t : sIsoTypesOff)
        {
            if (!gPipeline.hasRenderType(t)) LLPipeline::toggleRenderType(t);
        }
        sIsoTypesOff.clear();
        if (sIsoTerrain)
        {
            gPipeline.skipRenderingOfTerrain(false);
            sIsoTerrain = false;
        }
        for (const LLUUID& id : sIsoHidden)
        {
            LLViewerObject* o = gObjectList.findObject(id);
            if (o && o->mDrawable)
            {
                isoSetHidden(o->mDrawable, false);
            }
        }
        sIsoHidden.clear();
        tempRestore("RenderDynamicExposureEnabled");
        tempRestore("RenderGlow");
        // <MG> the studio's depth of field again (not the one from before the camera)
        if (sLensDof >= 0) tempSet("RenderDepthOfField", LLSD(sLensDof == 1));
        else tempRestore("RenderDepthOfField");
    }

    std::string prefsPath()
    {
        std::string p = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "mg_camera.json");
        if (gDirUtilp->getLindenUserDir().empty() || p.empty())
        {
            p = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "mg_camera.json");
        }
        return p;
    }

    S32 texturesLoading()
    {
        LLTextureFetch* fetch = LLAppViewer::getTextureFetch();
        return fetch ? fetch->getNumRequests() : 0;
    }

    void stopAf()
    {
        sAf = AfState();
        if (sGuides) sGuides->mAfShow = false;
    }
}
// </MG>

namespace MGPhoneCamera
{
    bool isCommand(const std::string& cmd)
    {
        return cmd.rfind("cam_", 0) == 0;
    }

    LLSD runCommand(const std::string& cmd, const LLSD& args)
    {
        LLSD result = LLSD().with("ok", true);

        if (cmd == "cam_state")
        {
            return stateOf();
        }
        if (cmd == "cam_guides")
        {
            setGuides(args.has("aspect") ? args["aspect"].asString() : sGuideAspect,
                      args.has("grid") ? args["grid"].asString() : sGuideGrid);
            return stateOf();
        }
        if (cmd == "cam_exposure")
        {
            if (!sExposureSaved)
            {
                sExposureBase = gSavedSettings.getF32("RenderExposure");
                if (sExposureBase <= 0.f) sExposureBase = 1.f;
                sExposureSaved = true;
            }
            const F32 ev = llclamp((F32)args["ev"].asReal(), -4.f, 4.f);
            gSavedSettings.setF32("RenderExposure", sExposureBase * powf(2.f, ev));
            result["ev"] = currentEv();
            return result;
        }
        if (cmd == "cam_time" || cmd == "cam_air")
        {
            if (!captureEnvironment())
            {
                return LLSD().with("ok", false).with("error", "no sky yet");
            }
            if (cmd == "cam_time")
            {
                applyHour((F32)args["hour"].asReal());
            }
            else
            {
                applyAir(args["mode"].asString());
            }
            return stateOf();
        }
        if (cmd == "cam_env_reset")
        {
            releaseEnvironment();
            return stateOf();
        }
        if (cmd == "cam_freeze")
        {
            if (!sFreezeSaved)
            {
                sFreezePrev = gSavedSettings.getBOOL("FreezeTime");
                sFreezeSaved = true;
            }
            gSavedSettings.setBOOL("FreezeTime", args["on"].asBoolean());
            result["freeze"] = gSavedSettings.getBOOL("FreezeTime");
            return result;
        }
        if (cmd == "cam_view")
        {
            const std::string preset = args["preset"].asString();
            if (preset == "reset")
            {
                gAgentCamera.resetView(true, true);
                sViewMoved = false;
                return result;
            }
            if (!isAgentAvatarValid() || !gAgentAvatarp->mHeadp || !gAgentAvatarp->mPelvisp)
            {
                return LLSD().with("ok", false).with("error", "no avatar");
            }
            LLVector3 at = gAgent.getAtAxis();
            at.mV[VZ] = 0.f;
            if (at.normVec() < 0.01f) at = LLVector3::x_axis;
            const LLVector3 head = gAgentAvatarp->mHeadp->getWorldPosition();
            const LLVector3 pelvis = gAgentAvatarp->mPelvisp->getWorldPosition();
            LLVector3 focus, eye;
            F32 dist;
            if (preset == "face")
            {
                dist = args.has("dist") ? (F32)args["dist"].asReal() : 0.9f;
                focus = head + LLVector3(0.f, 0.f, 0.02f);
                eye = focus + at * dist + LLVector3(0.f, 0.f, 0.04f);
            }
            else if (preset == "room")
            {
                dist = args.has("dist") ? (F32)args["dist"].asReal() : 7.5f;
                focus = pelvis + LLVector3(0.f, 0.f, 0.3f);
                eye = focus + at * dist + LLVector3(0.f, 0.f, dist * 0.33f);
            }
            else
            {
                dist = args.has("dist") ? (F32)args["dist"].asReal() : 3.2f;
                focus = (head + pelvis) * 0.5f;
                eye = focus + at * dist + LLVector3(0.f, 0.f, 0.25f);
            }
            gAgentCamera.setCameraPosAndFocusGlobal(gAgent.getPosGlobalFromAgent(eye), gAgent.getPosGlobalFromAgent(focus), gAgentID);
            sViewMoved = true;
            result["dist"] = dist;
            return result;
        }
        if (cmd == "cam_folder")
        {
            if (args["pick"].asBoolean())
            {
                (new LLDirPickerThread([](const std::vector<std::string>& dirs, std::string)
                    {
                        if (dirs.empty() || dirs[0].empty()) return;
                        gSavedSettings.setString("MGCameraFolder", dirs[0]);
                        MGFloaterPhone* phone = LLFloaterReg::findTypedInstance<MGFloaterPhone>("mg_phone");
                        if (phone)
                        {
                            phone->pushToPage(LLSD().with("type", "cam_folder").with("path", dirs[0]));
                        }
                    }, saveFolder()))->getFile();
                result["picking"] = true;
            }
            else if (args.has("reset"))
            {
                gSavedSettings.setString("MGCameraFolder", "");
            }
            result["folder"] = saveFolder();
            return result;
        }
        if (cmd == "cam_restore")
        {
            restore();
            return stateOf();
        }
        // <MG> ------------------------------------------------ Camera Studio
        if (cmd == "cam_af")
        {
            if (args["clear"].asBoolean())
            {
                stopAf();
                tempRestore("FSFocusPointLocked");
                tempRestore("FSFocusPointFollowsPointer");
                return result;
            }
            lensOn();
            // <MG> the focus never switches the depth of field on by itself (the studio's DoF
            // switch does: cam_lens); the pipeline's own ease is shortened so the hunt shows
            tempSet("CameraFocusTransitionTime", LLSD(0.08));
            const std::string mode = args.has("mode") ? args["mode"].asString() : "s";
            const bool face_area = !args.has("area") || args["area"].asString() == "face";
            LLViewerCamera* cam = LLViewerCamera::getInstance();
            AfState af;
            af.active = true;
            af.mode = (mode == "c" || mode == "m") ? mode : "s";
            af.sx = (F32)(args.has("x") ? args["x"].asReal() : 0.5);
            af.sy = (F32)(args.has("y") ? args["y"].asReal() : 0.5);
            if (af.mode == "m")
            {
                const F32 d = llclamp((F32)(args.has("dist") ? args["dist"].asReal() : 3.0), 0.3f, 1000.f);
                af.point = cam->getOrigin() + cam->getAtAxis() * d;
            }
            else
            {
                LLVector3 hit;
                LLVOAvatar* av = nullptr;
                if (!pickAt(af.sx, af.sy, hit, av))
                {
                    hit = cam->getOrigin() + cam->getAtAxis() * 1000.f; // the sky: infinity
                }
                if (av && face_area)
                {
                    af.face = true;
                    af.avatar = av->getID();
                    af.point = eyesOf(av);
                    result["name"] = av->getFullname();
                }
                else
                {
                    af.point = hit;
                }
            }
            const F32 dist = distanceTo(af.point);
            if (dist < 0.3f && af.mode != "m")
            {
                sAfFailOn = true;
                sAfFailX = af.sx;
                sAfFailY = af.sy;
                sAfFail.reset();
                return LLSD().with("ok", false).with("error", "too_close").with("dist", dist);
            }
            // where the lens was: it hunts from there (manual focus just turns)
            const LLVector3 was = sAf.active ? LLPipeline::sLastFocusPoint : LLVector3::zero;
            af.fromDist = was.isExactlyZero() ? 16.f : llmax(0.3f, distanceTo(was));
            af.hunt = af.mode != "m";
            sAf = af;
            sAf.since.reset();
            setFocusPoint(af.hunt ? huntPoint(sAf.point, sAf.fromDist, 0.f) : sAf.point);
            result["dist"] = dist;
            result["face"] = sAf.face;
            result["hunt"] = sAf.hunt;
            result["lock_ms"] = sAf.hunt ? (S32)(MGCameraGuides::HUNT_TIME * 1000.f) : 0;
            return result;
        }
        if (cmd == "cam_lens")
        {
            LLViewerCamera* cam = LLViewerCamera::getInstance();
            lensOn();
            if (args.has("focal"))
            {
                const F32 fov = llclamp(fovOfFocal((F32)args["focal"].asReal()), cam->getMinView(), cam->getMaxView());
                cam->setDefaultFOV(fov);
                sFovChanged = true;
            }
            if (args.has("fnum"))
            {
                tempSet("CameraFNumber", LLSD((F64)llclamp((F32)args["fnum"].asReal(), 0.7f, 64.f)));
            }
            if (args.has("dof"))
            {
                sLensDof = args["dof"].asBoolean() ? 1 : 0;   // <MG/> (the cut-out gives it back)
                if (!sIsolate) tempSet("RenderDepthOfField", LLSD(args["dof"].asBoolean()));
            }
            result["focal"] = focalOfFov(cam->getDefaultFOV());
            result["fnum"] = gSavedSettings.getF32("CameraFNumber");
            result["dof"] = gSavedSettings.getBOOL("RenderDepthOfField");
            result["min_focal"] = focalOfFov(cam->getMaxView());
            result["max_focal"] = focalOfFov(cam->getMinView());
            return result;
        }
        if (cmd == "cam_quality")
        {
            setQuality(args["on"].asBoolean());
            result["on"] = sQuality;
            return result;
        }
        if (cmd == "cam_clean")
        {
            if (args.has("huds"))
            {
                const bool hide = args["huds"].asBoolean();
                if (hide && !sHudsHidden)
                {
                    sHudsWere = LLPipeline::sShowHUDAttachments;
                    LLPipeline::sShowHUDAttachments = false;
                    sHudsHidden = true;
                }
                else if (!hide && sHudsHidden)
                {
                    LLPipeline::sShowHUDAttachments = sHudsWere;
                    sHudsHidden = false;
                }
            }
            if (args.has("names"))
            {
                if (args["names"].asBoolean()) tempSet("AvatarNameTagMode", LLSD(0));
                else tempRestore("AvatarNameTagMode");
            }
            result["huds"] = sHudsHidden;
            result["names"] = gSavedSettings.getS32("AvatarNameTagMode") == 0;
            return result;
        }
        if (cmd == "cam_meter")
        {
            sMeterOn = args["on"].asBoolean();
            if (!sMeterOn) meterFree();
            result["on"] = sMeterOn;
            return result;
        }
        if (cmd == "cam_textures")
        {
            result["loading"] = texturesLoading();
            return result;
        }
        if (cmd == "cam_prefs")
        {
            const std::string path = prefsPath();
            if (args.has("json"))
            {
                const std::string json = args["json"].asString();
                if (json.size() > 65536)
                {
                    return LLSD().with("ok", false).with("error", "too large");
                }
                llofstream out(path.c_str(), std::ios::out | std::ios::trunc);
                if (!out.is_open())
                {
                    return LLSD().with("ok", false).with("error", "cannot write");
                }
                out << json;
                return result;
            }
            std::string json;
            llifstream in(path.c_str());
            if (in.is_open())
            {
                std::stringstream buf;
                buf << in.rdbuf();
                json = buf.str();
            }
            result["json"] = json;
            return result;
        }
        if (cmd == "cam_move")
        {
            // the camera, free around its focus (like Alt + drag): orbit / pan in pixels
            // of the view, zoom in steps (+ in), roll in degrees, reset
            if (args["reset"].asBoolean())
            {
                gAgentCamera.resetView(true, true);
                gAgentCamera.resetCameraRoll();
                sViewMoved = false;
                sRolled = false;
                return result;
            }
            if (gAgentCamera.getFocusOnAvatar())
            {
                gAgentCamera.setFocusOnAvatar(false, false);
            }
            sViewMoved = true;
            const F32 width = (F32)llmax(1, gViewerWindow->getWorldViewWidthScaled());
            const F32 rad_per_px = F_TWO_PI / width;
            if (args.has("orbit"))
            {
                const F32 dx = (F32)args["orbit"][0].asReal(), dy = (F32)args["orbit"][1].asReal();
                if (dx != 0.f) gAgentCamera.cameraOrbitAround(-dx * rad_per_px);
                if (dy != 0.f) gAgentCamera.cameraOrbitOver(-dy * rad_per_px);
            }
            if (args.has("pan"))
            {
                LLVector3d to_focus = gAgentCamera.getCameraPositionGlobal() - gAgentCamera.getFocusGlobal();
                const F32 m_per_px = 3.f * (F32)to_focus.normVec() / width;
                const F32 dx = (F32)args["pan"][0].asReal(), dy = (F32)args["pan"][1].asReal();
                if (dx != 0.f) gAgentCamera.cameraPanLeft(dx * m_per_px);
                if (dy != 0.f) gAgentCamera.cameraPanUp(-dy * m_per_px);
            }
            if (args.has("zoom"))
            {
                gAgentCamera.cameraZoomIn(powf(0.92f, (F32)args["zoom"].asReal()));
            }
            if (args.has("roll"))
            {
                gAgentCamera.cameraRollOver((F32)args["roll"].asReal() * DEG_TO_RAD);
                sRolled = true;
            }
            result["roll"] = gAgentCamera.getRollAngle() * RAD_TO_DEG;
            return result;
        }
        if (cmd == "cam_env_list")
        {
            LLSD l = envList();
            l["ok"] = true;
            l["state"] = envState();
            return l;
        }
        if (cmd == "cam_env_state")
        {
            LLSD s = envState();
            s["ok"] = true;
            return s;
        }
        if (cmd == "cam_env_apply")
        {
            // a sky, water or day cycle from the inventory, for this view only (async load)
            const LLUUID asset = args["id"].asUUID();
            if (asset.isNull() || !captureEnvironment()) return LLSD().with("ok", false).with("error", "no sky yet");
            LLSettingsVOBase::getSettingsAsset(asset, [](LLUUID, LLSettingsBase::ptr_t settings, S32 status, LLExtStat)
                {
                    if (!settings || status || !sEnv.active)
                    {
                        pushEnv(LLSD().with("error", "load"));
                        return;
                    }
                    LLEnvironment& env = LLEnvironment::instance();
                    const std::string type = settings->getSettingsType();
                    if (type == "sky")
                    {
                        envReclone();
                        sEnv.live = std::static_pointer_cast<LLSettingsSky>(settings)->buildClone();
                        sEnv.hazeDensity = sEnv.live->getHazeDensity();
                        sEnv.hazeHorizon = sEnv.live->getHazeHorizon();
                        sEnv.densityMult = sEnv.live->getDensityMultiplier();
                        sEnv.hour = -1.f;
                        sEnv.air = "original";
                        env.setEnvironment(LLEnvironment::ENV_LOCAL, sEnv.live, sEnv.liveWater);
                    }
                    else if (type == "water")
                    {
                        envReclone();
                        sEnv.liveWater = std::static_pointer_cast<LLSettingsWater>(settings)->buildClone();
                        env.setEnvironment(LLEnvironment::ENV_LOCAL, sEnv.live, sEnv.liveWater);
                    }
                    else
                    {
                        env.setEnvironment(LLEnvironment::ENV_LOCAL, std::static_pointer_cast<LLSettingsDay>(settings),
                                           env.getEnvironmentDayLength(LLEnvironment::ENV_REGION), env.getEnvironmentDayOffset(LLEnvironment::ENV_REGION));
                        sEnv.dayMode = true;
                        sEnv.hour = -1.f;
                    }
                    env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
                    env.updateEnvironment(LLEnvironment::TRANSITION_INSTANT);
                    pushEnv(LLSD().with("applied", type));
                });
            result["loading"] = true;
            return result;
        }
        if (cmd == "cam_env_set")
        {
            if (!captureEnvironment()) return LLSD().with("ok", false).with("error", "no sky yet");
            envReclone();
            LLSettingsSky::ptr_t sky = sEnv.live;
            if (args.has("az") || args.has("el"))
            {
                const LLSD cur = envState();
                const F32 az = (F32)(args.has("az") ? args["az"].asReal() : cur["az"].asReal()) * DEG_TO_RAD;
                const F32 el = (F32)(args.has("el") ? args["el"].asReal() : cur["el"].asReal()) * DEG_TO_RAD;
                const LLVector3 dir(cosf(el) * cosf(az), cosf(el) * sinf(az), sinf(el));
                LLQuaternion q;
                q.shortestArc(LLVector3::x_axis, dir);
                LLQuaternion qm;
                qm.shortestArc(LLVector3::x_axis, -dir);
                sky->setSunRotation(q);
                sky->setMoonRotation(qm);
                sEnv.hour = -1.f;
            }
            if (args.has("haze")) sky->setHazeDensity(llclamp((F32)args["haze"].asReal(), 0.f, 10.f));
            if (args.has("horizon")) sky->setHazeHorizon(llclamp((F32)args["horizon"].asReal(), 0.f, 10.f));
            if (args.has("density")) sky->setDensityMultiplier(llclamp((F32)args["density"].asReal() / 1000.f, 0.0001f, 2.f));
            if (args.has("distance")) sky->setDistanceMultiplier(llclamp((F32)args["distance"].asReal(), 0.f, 100.f));
            if (args.has("gamma")) sky->setGamma(llclamp((F32)args["gamma"].asReal(), 0.1f, 10.f));
            if (args.has("clouds")) sky->setCloudShadow(llclamp((F32)args["clouds"].asReal(), 0.f, 1.f));
            if (args.has("stars")) sky->setStarBrightness(llclamp((F32)args["stars"].asReal(), 0.f, 500.f));
            if (args.has("clouds_frozen"))
            {
                const bool freeze = args["clouds_frozen"].asBoolean();
                if (freeze && !sEnv.cloudsFrozen) { sEnv.cloudRate = sky->getCloudScrollRate(); sky->setCloudScrollRate(LLVector2(0.f, 0.f)); }
                else if (!freeze && sEnv.cloudsFrozen) sky->setCloudScrollRate(sEnv.cloudRate);
                sEnv.cloudsFrozen = freeze;
            }
            if (args.has("facelights"))
            {
                if (args["facelights"].asBoolean()) tempRestore("RenderAttachedLights");
                else tempSet("RenderAttachedLights", LLSD(false));
            }
            sky->update();
            LLSD s = envState();
            s["ok"] = true;
            return s;
        }
        if (cmd == "cam_env_keep")
        {
            sEnvKeep = sEnv.active;
            result["kept"] = sEnvKeep;
            return result;
        }
        if (cmd == "cam_look")
        {
            setLook(args);
            result["on"] = sLook.any;
            result["shader"] = gMGLookProgram.isComplete();
            return result;
        }
        if (cmd == "cam_isolate")
        {
            setIsolate(args["on"].asBoolean());
            result["on"] = sIsolate;
            result["hidden"] = (S32)sIsoHidden.size();
            return result;
        }
        if (cmd == "cam_pose")
        {
            // Firestorm's poser (body, face, hands, mirror, BVH) over the viewfinder
            if (args.has("open") && !args["open"].asBoolean())
            {
                LLFloaterReg::hideInstance("fs_poser");
            }
            else
            {
                // the viewfinder hides the floaters like mouselook, which also blocks any
                // floater from showing (LLFloaterView::pushVisibleAll): lift it for the poser
                MGFloaterPhone* phone = LLFloaterReg::findTypedInstance<MGFloaterPhone>("mg_phone");
                const bool vf = phone && phone->isViewfinder();
                if (vf) LLFloaterReg::blockShowFloaters(false);
                LLFloater* poser = LLFloaterReg::showInstance("fs_poser", LLSD(), true);
                if (vf) LLFloaterReg::blockShowFloaters(true);
                if (poser && gFloaterView)
                {
                    gFloaterView->adjustToFitScreen(poser, false);
                    gFloaterView->bringToFront(poser, true);
                }
            }
            result["open"] = LLFloaterReg::instanceVisible("fs_poser");
            return result;
        }
        if (cmd == "cam_save_arm")
        {
            sArmName = args["name"].asString();
            sArmTimer.reset();
            result["folder"] = saveFolder();
            return result;
        }
        // </MG>
        return LLSD().with("ok", false).with("error", "unknown command");
    }

    // <MG>
    void restoreSettings()
    {
        setIsolate(false);
        sQuality = false;
        for (const auto& kv : sSaved)
        {
            if (LLControlVariable* c = gSavedSettings.getControl(kv.first))
            {
                c->setValue(kv.second);
            }
        }
        sSaved.clear();
        if (sHudsHidden)
        {
            LLPipeline::sShowHUDAttachments = sHudsWere;
            sHudsHidden = false;
        }
        if (sExposureSaved)
        {
            gSavedSettings.setF32("RenderExposure", sExposureBase);
            sExposureSaved = false;
        }
        if (sFreezeSaved)
        {
            gSavedSettings.setBOOL("FreezeTime", sFreezePrev);
            sFreezeSaved = false;
        }
    }
    // </MG>

    void restore()
    {
        // <MG> Camera Studio
        setIsolate(false);
        sLensDof = -1;
        sAfFailOn = false;
        sLook = LookState();
        MGPhonePoser::restore();
        stopAf();
        sMeterOn = false;
        meterFree();
        sArmName.clear();
        restoreSettings();
        if (sFovChanged)
        {
            LLViewerCamera::getInstance()->setDefaultFOV(gSavedSettings.getF32("CameraAngle"));
            sFovChanged = false;
        }
        if (sRolled)
        {
            gAgentCamera.resetCameraRoll();
            sRolled = false;
        }
        // </MG>
        setGuides("native", "off");
        if (sExposureSaved)
        {
            gSavedSettings.setF32("RenderExposure", sExposureBase);
            sExposureSaved = false;
        }
        if (sFreezeSaved)
        {
            gSavedSettings.setBOOL("FreezeTime", sFreezePrev);
            sFreezeSaved = false;
        }
        releaseEnvironment();
        if (sViewMoved)
        {
            gAgentCamera.resetView(true, true);
            sViewMoved = false;
        }
    }

    void hideGuidesForShot(bool hide)
    {
        if (sGuides && (sGuides->mAspect > 0.f || sGuides->mGrid != 0))
        {
            sGuides->setVisible(!hide);
        }
    }

    std::string saveFolder()
    {
        std::string dir = gSavedSettings.getString("MGCameraFolder");
        if (dir.empty())
        {
            const char* home = getenv("USERPROFILE");
            if (!home) home = getenv("HOME");
            const std::string base = home ? std::string(home) : gDirUtilp->getOSUserDir();
            dir = base + gDirUtilp->getDirDelimiter() + "Pictures" + gDirUtilp->getDirDelimiter() + "MasterGate";
        }
        return dir;
    }

    std::string saveShot(LLImageRaw* raw, const std::string& format, const std::string& pattern)
    {
        if (!raw)
        {
            return "";
        }
        const std::string dir = saveFolder();
        if (!LLFile::isdir(dir))
        {
            LLFile::mkdir(gDirUtilp->getDirName(dir));
            LLFile::mkdir(dir);
            if (!LLFile::isdir(dir)) return "";
        }
        static S32 sCount = 0;
        ++sCount;
        char date[16] = { 0 }, tim[16] = { 0 };
        const std::time_t now = std::time(nullptr);
        std::tm lt = {};
#if LL_WINDOWS
        localtime_s(&lt, &now);
#else
        localtime_r(&now, &lt);
#endif
        std::strftime(date, sizeof(date), "%Y%m%d", &lt);
        std::strftime(tim, sizeof(tim), "%H%M%S", &lt);
        std::string name = pattern.empty() ? std::string("MasterGate_{date}_{time}") : pattern;
        replaceAll(name, "{date}", date);
        replaceAll(name, "{time}", tim);
        replaceAll(name, "{n}", llformat("%03d", sCount));
        replaceAll(name, "{w}", std::to_string(raw->getWidth()));
        replaceAll(name, "{h}", std::to_string(raw->getHeight()));
        name = sanitizeName(name);
        if (name.empty()) name = "MasterGate";

        const bool png = (format == "png");
        const std::string ext = png ? ".png" : ".jpg";
        std::string path = dir + gDirUtilp->getDirDelimiter() + name + ext;
        for (S32 i = 2; LLFile::isfile(path) && i < 1000; ++i)
        {
            path = dir + gDirUtilp->getDirDelimiter() + name + "_" + std::to_string(i) + ext;
        }

        LLPointer<LLImageFormatted> img;
        if (png)
        {
            img = new LLImagePNG();
        }
        else
        {
            img = new LLImageJPEG(95);
        }
        if (!img->encode(raw, 0.f) || !img->save(path))
        {
            return "";
        }
        return path;
    }

    void applyLook(LLImageRaw* raw, const LLSD& look)
    {
        if (!raw || !raw->getData() || raw->getComponents() < 3)
        {
            return;
        }
        // [sat, contrast, gain r,g,b, shadow tint r,g,b, highlight tint r,g,b, lift, swap R/G]
        static const std::map<std::string, std::array<F32, 13>> presets = {
            { "kodachrome", { 1.28f, 1.12f, 1.05f, 1.00f, 0.93f, 0.00f, 0.01f, 0.05f, 0.05f, 0.02f, -0.03f, 0.00f, 0.f } },
            { "cyan_night", { 0.85f, 1.10f, 0.84f, 0.98f, 1.12f, 0.00f, 0.05f, 0.09f, 0.00f, 0.03f, 0.05f, 0.02f, 0.f } },
            { "bleach",     { 0.42f, 1.38f, 1.00f, 1.00f, 1.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.f } },
            { "infra",      { 1.15f, 1.08f, 1.06f, 0.98f, 1.00f, 0.00f, 0.00f, 0.00f, 0.03f, 0.00f, 0.02f, 0.00f, 1.f } },
            { "golden",     { 1.12f, 1.05f, 1.09f, 1.01f, 0.85f, 0.02f, 0.00f, -0.02f, 0.06f, 0.03f, 0.00f, 0.01f, 0.f } },
        };
        const std::array<F32, 13>* P = nullptr;
        const auto it = presets.find(look["preset"].asString());
        if (it != presets.end()) P = &it->second;
        const F32 s = (look.has("strength") ? (F32)look["strength"].asReal() : 60.f) / 100.f;
        const F32 con = (F32)look["contrast"].asReal() / 100.f;
        const F32 hi = (F32)look["highlights"].asReal() / 100.f;
        const F32 sh = (F32)look["shadows"].asReal() / 100.f;
        const F32 wh = (F32)look["whites"].asReal() / 100.f;
        const F32 bl = (F32)look["blacks"].asReal() / 100.f;
        const F32 grain = (F32)look["grain"].asReal() / 100.f;
        const F32 vig = (F32)look["vignette"].asReal() / 100.f;
        const F32 hal = (F32)look["halation"].asReal() / 100.f;
        if (!P && con == 0.f && hi == 0.f && sh == 0.f && wh == 0.f && bl == 0.f && grain == 0.f && vig == 0.f && hal == 0.f)
        {
            return;
        }

        const S32 W = raw->getWidth(), H = raw->getHeight(), C = raw->getComponents();
        U8* data = raw->getData();
        auto sm = [](F32 a, F32 b, F32 x) { F32 t = llclamp((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); };
        auto noise = [](U32 x, U32 y)
        {
            U32 h = x * 374761393u + y * 668265263u;
            h = (h ^ (h >> 13)) * 1274126177u;
            h ^= (h >> 16);
            return (F32)(h & 0xffff) / 65535.f;
        };

        // halation: the bright parts, blurred at 1/8 size
        std::vector<F32> halo;
        S32 hw = 0, hh = 0;
        if (hal > 0.f)
        {
            hw = llmax(1, W >> 3); hh = llmax(1, H >> 3);
            halo.assign(hw * hh, 0.f);
            for (S32 cy = 0; cy < hh; ++cy) for (S32 cx = 0; cx < hw; ++cx)
            {
                const S32 px = llmin(W - 1, cx * 8 + 4), py = llmin(H - 1, cy * 8 + 4);
                const U8* p = data + ((size_t)py * W + px) * C;
                const F32 lum = (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.f;
                halo[cy * hw + cx] = llmax(0.f, (lum - 0.7f) / 0.3f);
            }
            std::vector<F32> tmp(hw * hh);
            for (S32 pass = 0; pass < 3; ++pass)
            {
                for (S32 y = 0; y < hh; ++y) for (S32 x = 0; x < hw; ++x)
                {
                    F32 acc = 0.f; S32 n = 0;
                    for (S32 o = -2; o <= 2; ++o) { const S32 xx = x + o; if (xx >= 0 && xx < hw) { acc += halo[y * hw + xx]; ++n; } }
                    tmp[y * hw + x] = acc / n;
                }
                for (S32 y = 0; y < hh; ++y) for (S32 x = 0; x < hw; ++x)
                {
                    F32 acc = 0.f; S32 n = 0;
                    for (S32 o = -2; o <= 2; ++o) { const S32 yy = y + o; if (yy >= 0 && yy < hh) { acc += tmp[yy * hw + x]; ++n; } }
                    halo[y * hw + x] = acc / n;
                }
            }
        }

        for (S32 row = 0; row < H; ++row)
        {
            // LLImageRaw rows run bottom-up; the page's canvas runs top-down
            const S32 y = H - 1 - row;
            for (S32 x = 0; x < W; ++x)
            {
                U8* p = data + ((size_t)row * W + x) * C;
                F32 r = p[0] / 255.f, g = p[1] / 255.f, b = p[2] / 255.f;
                if (P)
                {
                    const std::array<F32, 13>& k = *P;
                    const F32 r0 = r, g0 = g, b0 = b;
                    if (k[12] > 0.f) std::swap(r, g);
                    const F32 Lp = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    r = Lp + (r - Lp) * k[0]; g = Lp + (g - Lp) * k[0]; b = Lp + (b - Lp) * k[0];
                    r = (r - 0.5f) * k[1] + 0.5f; g = (g - 0.5f) * k[1] + 0.5f; b = (b - 0.5f) * k[1] + 0.5f;
                    r *= k[2]; g *= k[3]; b *= k[4];
                    const F32 ws = (1.f - Lp) * (1.f - Lp), wl = Lp * Lp;
                    r += k[5] * ws + k[8] * wl; g += k[6] * ws + k[9] * wl; b += k[7] * ws + k[10] * wl;
                    r = r * (1.f - k[11]) + k[11]; g = g * (1.f - k[11]) + k[11]; b = b * (1.f - k[11]) + k[11];
                    r = r0 + (r - r0) * s; g = g0 + (g - g0) * s; b = b0 + (b - b0) * s;
                }
                if (con != 0.f)
                {
                    const F32 kc = 1.f + con * 0.6f;
                    r = (r - 0.5f) * kc + 0.5f; g = (g - 0.5f) * kc + 0.5f; b = (b - 0.5f) * kc + 0.5f;
                }
                const F32 Lt = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                const F32 off = hi * 0.35f * sm(0.5f, 1.f, Lt) + sh * 0.35f * (1.f - sm(0.f, 0.5f, Lt))
                              + wh * 0.25f * sm(0.75f, 1.f, Lt) + bl * 0.25f * (1.f - sm(0.f, 0.25f, Lt));
                r += off; g += off; b += off;
                if (!halo.empty())
                {
                    // the halo buffer was built in this buffer's row order: sample by row
                    const F32 fx = llclamp((F32)x / W * hw - 0.5f, 0.f, (F32)(hw - 1));
                    const F32 fy = llclamp((F32)row / H * hh - 0.5f, 0.f, (F32)(hh - 1));
                    const S32 ix = (S32)fx, iy = (S32)fy, ix2 = llmin(hw - 1, ix + 1), iy2 = llmin(hh - 1, iy + 1);
                    const F32 ax = fx - ix, ay = fy - iy;
                    const F32 v = (halo[iy * hw + ix] * (1.f - ax) + halo[iy * hw + ix2] * ax) * (1.f - ay)
                                + (halo[iy2 * hw + ix] * (1.f - ax) + halo[iy2 * hw + ix2] * ax) * ay;
                    r += hal * v * 0.9f; g += hal * v * 0.32f; b += hal * v * 0.12f;
                }
                if (vig > 0.f)
                {
                    const F32 dx = (W > 1 ? (F32)x / (W - 1) : 0.5f) * 2.f - 1.f;
                    const F32 dy = (H > 1 ? (F32)y / (H - 1) : 0.5f) * 2.f - 1.f;
                    const F32 f = 1.f - vig * 0.75f * sm(0.35f, 1.f, sqrtf(dx * dx + dy * dy) / 1.41421f);
                    r *= f; g *= f; b *= f;
                }
                if (grain > 0.f)
                {
                    const F32 nz = (noise((U32)x, (U32)y) - 0.5f) * grain * 0.14f;
                    r += nz; g += nz; b += nz;
                }
                p[0] = (U8)llclamp(ll_round(r * 255.f), 0, 255);
                p[1] = (U8)llclamp(ll_round(g * 255.f), 0, 255);
                p[2] = (U8)llclamp(ll_round(b * 255.f), 0, 255);
            }
        }
    }

    // <MG> ================================================================ Camera Studio

    void downsample(LLImageRaw* raw, S32 f)
    {
        if (!raw || f < 2 || !raw->getData())
        {
            return;
        }
        const S32 W = raw->getWidth(), H = raw->getHeight(), C = raw->getComponents();
        const S32 w = W / f, h = H / f;
        if (w < 1 || h < 1)
        {
            return;
        }
        LLPointer<LLImageRaw> out = new LLImageRaw((U16)w, (U16)h, (S8)C);
        const U8* src = raw->getData();
        U8* dst = out->getData();
        const S32 n = f * f;
        for (S32 y = 0; y < h; ++y)
        {
            for (S32 x = 0; x < w; ++x)
            {
                S32 acc[4] = { 0, 0, 0, 0 };
                for (S32 dy = 0; dy < f; ++dy)
                {
                    const U8* row = src + ((size_t)(y * f + dy) * W + (size_t)x * f) * C;
                    for (S32 dx = 0; dx < f; ++dx)
                    {
                        for (S32 c = 0; c < C; ++c) acc[c] += row[dx * C + c];
                    }
                }
                U8* o = dst + ((size_t)y * w + x) * C;
                for (S32 c = 0; c < C; ++c) o[c] = (U8)((acc[c] + n / 2) / n);
            }
        }
        raw->resize((U16)w, (U16)h, (S8)C);
        memcpy(raw->getData(), out->getData(), (size_t)w * h * C);
    }

    void setViewRect(const LLRect& rect)
    {
        const bool was = !sViewRect.isEmpty();
        sViewRect = rect;
        if (was != !rect.isEmpty())
        {
            setGuides(sGuideAspect, sGuideGrid);
        }
    }

    void idle()
    {
        MGPhonePoser::idle(); // <MG/> the posing gizmo
        // isolation: objects that streamed in since are hidden too
        if (sIsolate && sIsoTimer.getElapsedTimeF32() > 1.f)
        {
            sIsoTimer.reset();
            isoSweep();
        }
        // <MG> a failed focus: its red brackets blink for a moment
        if (sGuides)
        {
            if (sAfFailOn && sAfFail.getElapsedTimeF32() < 1.2f && !sViewRect.isEmpty())
            {
                sGuides->mFailT = sAfFail.getElapsedTimeF32();
                sGuides->mFailAt.mX = sViewRect.mLeft + ll_round(sAfFailX * sViewRect.getWidth());
                sGuides->mFailAt.mY = sViewRect.mTop - ll_round(sAfFailY * sViewRect.getHeight());
            }
            else
            {
                sAfFailOn = false;
                sGuides->mFailT = -1.f;
            }
        }
        // the face held: the focus follows it, the box with it
        if (!sAf.active)
        {
            return;
        }
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        if (sAf.face)
        {
            LLVOAvatar* av = findAvatar(sAf.avatar);
            if (!av)
            {
                stopAf();
                return;
            }
            sAf.point = eyesOf(av);
        }
        else if (sAf.mode == "c" && sAf.track.getElapsedTimeF32() > 0.1f)
        {
            // AF-C on a point: what is under the AF point now
            sAf.track.reset();
            LLVector3 hit;
            LLVOAvatar* av = nullptr;
            if (pickAt(sAf.sx, sAf.sy, hit, av))
            {
                sAf.point = hit;
            }
        }
        // <MG> the lens: hunting at first, then on the subject
        const F32 af_t = sAf.since.getElapsedTimeF32();
        const bool hunting = sAf.hunt && af_t < MGCameraGuides::HUNT_TIME;
        LLPipeline::sLastFocusPoint = hunting ? huntPoint(sAf.point, sAf.fromDist, af_t) : sAf.point;
        if (!sGuides || sViewRect.isEmpty())
        {
            return;
        }
        LLCoordGL c;
        if (!cam->projectPosAgentToScreen(sAf.point, c, false) || distanceTo(sAf.point) <= 0.f)
        {
            sGuides->mAfShow = false;
            return;
        }
        S32 half = 22;
        if (sAf.face)
        {
            // a head is about 0.32 m tall: its size on screen
            LLCoordGL top;
            if (cam->projectPosAgentToScreen(sAf.point + cam->getUpAxis() * 0.16f, top, false))
            {
                half = llclamp(abs(top.mY - c.mY), 10, 600);
            }
        }
        const S32 hw = sAf.face ? ll_round(half * 0.8f) : half;
        sGuides->mAfBox = LLRect(c.mX - hw, c.mY + half, c.mX + hw, c.mY - half);
        sGuides->mAfHunt = sAf.hunt;
        sGuides->mAfT = af_t;
        sGuides->mAfDist = distanceTo(sAf.point);
        sGuides->mAfShow = true;
    }

    void onSceneFinal()
    {
        if (!sMeterOn || gSnapshot || gCubeSnapshot || !gViewerWindow)
        {
            return;
        }
        if (sMeterTimer.getElapsedTimeF32() < 0.25f)
        {
            return;
        }
        sMeterTimer.reset();

        GLint prev_read = 0, prev_draw = 0, prev_pack = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pack);

        if (!sMeterFbo)
        {
            glGenRenderbuffers(1, &sMeterRbo);
            glBindRenderbuffer(GL_RENDERBUFFER, sMeterRbo);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, METER_W, METER_H);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            glGenFramebuffers(1, &sMeterFbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sMeterFbo);
            glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, sMeterRbo);
            glGenBuffers(1, &sMeterPbo);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, sMeterPbo);
            glBufferData(GL_PIXEL_PACK_BUFFER, METER_W * METER_H * 4, nullptr, GL_STREAM_READ);
            sMeterPending = false;
        }

        // the copy asked a quarter of a second ago is long finished
        glBindBuffer(GL_PIXEL_PACK_BUFFER, sMeterPbo);
        if (sMeterPending)
        {
            const U8* px = (const U8*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, METER_W * METER_H * 4, GL_MAP_READ_BIT);
            if (px)
            {
                meterRead(px);
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            }
            sMeterPending = false;
        }

        // the finished scene (no interface yet), scaled down on the GPU
        const S32 W = gViewerWindow->getWindowWidthRaw(), H = gViewerWindow->getWindowHeightRaw();
        glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_draw);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sMeterFbo);
        glBlitFramebuffer(0, 0, W, H, 0, 0, METER_W, METER_H, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sMeterFbo);
        glReadPixels(0, 0, METER_W, METER_H, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        sMeterPending = true;

        glBindBuffer(GL_PIXEL_PACK_BUFFER, prev_pack);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);

        // readouts with each histogram: the page shows them in its bars
        if (sMeterFresh && sPushTimer.getElapsedTimeF32() > 0.2f)
        {
            sPushTimer.reset();
            sMeterFresh = false;
            LLViewerCamera* cam = LLViewerCamera::getInstance();
            LLSD ev = sMeterResult;
            ev["type"] = "cam_meter";
            ev["focal"] = focalOfFov(cam->getDefaultFOV());
            ev["fnum"] = gSavedSettings.getF32("CameraFNumber");
            ev["pitch"] = asinf(llclamp(cam->getAtAxis().mV[VZ], -1.f, 1.f)) * RAD_TO_DEG;
            ev["roll"] = gAgentCamera.getRollAngle() * RAD_TO_DEG;
            ev["loading"] = texturesLoading();
            if (sAf.active)
            {
                ev["af"] = LLSD().with("dist", distanceTo(sAf.point)).with("face", sAf.face).with("mode", sAf.mode);
            }
            MGFloaterPhone* phone = LLFloaterReg::findTypedInstance<MGFloaterPhone>("mg_phone");
            if (phone)
            {
                phone->pushToPage(ev);
            }
        }
    }

    bool isIsolating() { return sIsolate; }

    bool renderLook(LLRenderTarget* src, LLRenderTarget* dst, LLVertexBuffer* triangle)
    {
        // not in the cut-out's two background renders: they must differ only by the background
        // (the look is seen live, and a third render brings it into the photo: lookIntoMatte)
        if (!sLook.any || sLookSuspended || (sIsolate && sMatteShot) || !gMGLookProgram.isComplete() || !src || !dst || !triangle)
        {
            return false;
        }
        static LLStaticHashedString u_layers("mg_layers"), u_a("mg_a"), u_b("mg_b"), u_c("mg_c"), u_d("mg_d"), u_e("mg_e"),
            u_fx("mg_fx"), u_fx2("mg_fx2"), u_frame("mg_frame");
        LLGLSLShader* shader = &gMGLookProgram;
        dst->bindTarget();
        shader->bind();
        S32 channel = shader->enableTexture(LLShaderMgr::DEFERRED_DIFFUSE, src->getUsage());
        if (channel > -1)
        {
            src->bindTexture(0, channel, LLTexUnit::TFO_BILINEAR);
        }
        shader->uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (GLfloat)dst->getWidth(), (GLfloat)dst->getHeight());
        shader->uniform1i(u_layers, sLook.layers);
        shader->uniform4fv(u_a, 4, sLook.a);
        shader->uniform4fv(u_b, 4, sLook.b);
        shader->uniform4fv(u_c, 4, sLook.c);
        shader->uniform4fv(u_d, 4, sLook.d);
        shader->uniform4fv(u_e, 4, sLook.e);
        shader->uniform4fv(u_fx, 1, sLook.fx);
        F32 fx2[4] = { sLook.fx2[0], sLook.fx2[1], sLook.fx2[2], (F32)fmod(sLookClock.getElapsedTimeF32(), 100.f) };
        if (sShotTile) fx2[2] = -1.f; // no before / after line in the photo
        if (sIsolate) fx2[1] = 0.f;   // <MG/> no cinema bars on a transparent picture
        shader->uniform4fv(u_fx2, 1, fx2);
        F32 frame[4];
        if (sShotTile) { frame[0] = sTile[0]; frame[1] = sTile[1]; frame[2] = sTile[2]; frame[3] = sTile[3]; }
        else liveFrame(frame);
        shader->uniform4fv(u_frame, 1, frame);
        triangle->setBuffer();
        triangle->drawArrays(LLRender::TRIANGLES, 0, 3);
        shader->disableTexture(LLShaderMgr::DEFERRED_DIFFUSE, src->getUsage());
        shader->unbind();
        dst->flush();
        return true;
    }

    void setShotTile(F32 x0, F32 y0, F32 w, F32 h)
    {
        sShotTile = true;
        sTile[0] = x0; sTile[1] = y0; sTile[2] = llmax(0.0001f, w); sTile[3] = llmax(0.0001f, h);
    }

    void clearShotTile() { sShotTile = false; }

    void suspendLook(bool suspend) { sLookSuspended = suspend; }

    void setMatteShot(bool on) { sMatteShot = on; }

    bool lookActive() { return sLook.any && !sLookSuspended && gMGLookProgram.isComplete(); }

    void lookIntoMatte(LLImageRaw* rgba, LLImageRaw* looked)
    {
        // looked = the subject over black with the look. Where it is opaque, that is the colour.
        // At soft edges (hair) the black behind is part of the pixel, and the look changed it too
        // (fade lifts it): take that share away, as for the matte, so the fringes stay clean.
        if (!rgba || !looked || rgba->getComponents() != 4 || looked->getComponents() < 3
            || rgba->getWidth() != looked->getWidth() || rgba->getHeight() != looked->getHeight())
        {
            return;
        }
        const S32 n = rgba->getWidth() * rgba->getHeight(), cl = looked->getComponents();
        U8* po = rgba->getData();
        const U8* pl = looked->getData();
        // what the look made of the black background: its average where nothing is
        F64 sum[3] = { 0.0, 0.0, 0.0 };
        S64 cnt = 0;
        for (S32 i = 0; i < n; ++i)
        {
            if (po[(size_t)i * 4 + 3] == 0)
            {
                const U8* l = pl + (size_t)i * cl;
                sum[0] += l[0]; sum[1] += l[1]; sum[2] += l[2];
                ++cnt;
            }
        }
        F32 lb[3] = { 0.f, 0.f, 0.f };
        if (cnt > 0)
        {
            for (S32 c = 0; c < 3; ++c) lb[c] = (F32)(sum[c] / (F64)cnt);
        }
        for (S32 i = 0; i < n; ++i)
        {
            U8* o = po + (size_t)i * 4;
            if (o[3] == 0) continue;
            const F32 a = o[3] / 255.f;
            const U8* l = pl + (size_t)i * cl;
            for (S32 c = 0; c < 3; ++c)
            {
                o[c] = (U8)llclamp(ll_round(((F32)l[c] - (1.f - a) * lb[c]) / a), 0, 255);
            }
        }
        LL_INFOS("MGPhone") << "camera: transparent shot with the look, its black was " << lb[0] << "," << lb[1] << "," << lb[2] << LL_ENDL;
    }

    bool isIsoHidden(const LLUUID& id) { return sIsolate && sIsoHidden.count(id) > 0; }

    void gbufferClear(LLColor4& color)
    {
        if (sIsolate)
        {
            const F32 v = sIsoWhite ? 1.f : 0.f;
            color.set(v, v, v, color.mV[VALPHA]);
        }
    }

    void setIsolateWhite(bool white) { sIsoWhite = white; }

    bool isolating() { return sIsolate; }

    LLPointer<LLImageRaw> matte(LLImageRaw* a, LLImageRaw* b)
    {
        // a = the subject over black, b = over white: per pixel, b - a = (1 - alpha) * W,
        // W being what the white background became after lighting and tone mapping
        // (one value for the whole frame, measured where a is black and b is brightest)
        if (!a || !b || a->getWidth() != b->getWidth() || a->getHeight() != b->getHeight() || a->getComponents() < 3)
        {
            return nullptr;
        }
        const S32 W = a->getWidth(), H = a->getHeight(), ca = a->getComponents(), cb = b->getComponents();
        const U8* pa = a->getData();
        const U8* pb = b->getData();
        S32 hist[3][256] = { { 0 } };
        S32 nbg = 0;
        for (S32 i = 0; i < W * H; ++i)
        {
            const U8* x = pa + (size_t)i * ca;
            if (x[0] + x[1] + x[2] <= 3)
            {
                const U8* y = pb + (size_t)i * cb;
                hist[0][y[0]]++; hist[1][y[1]]++; hist[2][y[2]]++;
                ++nbg;
            }
        }
        F32 bg[3] = { 255.f, 255.f, 255.f };
        if (nbg > 0)
        {
            for (S32 c = 0; c < 3; ++c)
            {
                // the median of the background pixels: robust to the subject's own black parts
                S32 acc = 0, v = 255;
                for (S32 k = 0; k < 256; ++k) { acc += hist[c][k]; if (acc * 2 >= nbg) { v = k; break; } }
                bg[c] = (F32)llmax(16, v);
            }
        }
        LLPointer<LLImageRaw> out = new LLImageRaw((U16)W, (U16)H, (S8)4);
        U8* po = out->getData();
        for (S32 i = 0; i < W * H; ++i)
        {
            const U8* x = pa + (size_t)i * ca;
            const U8* y = pb + (size_t)i * cb;
            F32 al = 0.f;
            for (S32 c = 0; c < 3; ++c)
            {
                al += 1.f - llclamp(((F32)y[c] - (F32)x[c]) / bg[c], 0.f, 1.f);
            }
            al = llclamp(al / 3.f, 0.f, 1.f);
            U8* o = po + (size_t)i * 4;
            if (al < 0.004f)
            {
                o[0] = o[1] = o[2] = o[3] = 0;
                continue;
            }
            // over black the colour is premultiplied: divide it back
            for (S32 c = 0; c < 3; ++c)
            {
                o[c] = (U8)llclamp(ll_round((F32)x[c] / al), 0, 255);
            }
            o[3] = (U8)llclamp(ll_round(al * 255.f), 0, 255);
        }
        LL_INFOS("MGPhone") << "camera: transparent shot, background measured " << bg[0] << "," << bg[1] << "," << bg[2]
                            << " over " << nbg << " px" << LL_ENDL;
        return out;
    }

    std::string takeDownloadPath(const std::string& suggested)
    {
        if (sArmName.empty() || sArmTimer.getElapsedTimeF32() > 30.f)
        {
            sArmName.clear();
            return "";
        }
        std::string name = sanitizeName(sArmName);
        sArmName.clear();
        std::string lower = name;
        LLStringUtil::toLower(lower);
        const auto ends = [&lower](const char* e) { const size_t n = strlen(e); return lower.size() > n && lower.compare(lower.size() - n, n, e) == 0; };
        if (!ends(".png") && !ends(".jpg") && !ends(".jpeg"))
        {
            return "";
        }
        const std::string dir = saveFolder();
        if (!LLFile::isdir(dir))
        {
            LLFile::mkdir(gDirUtilp->getDirName(dir));
            LLFile::mkdir(dir);
            if (!LLFile::isdir(dir)) return "";
        }
        const size_t dot = name.rfind('.');
        const std::string stem = name.substr(0, dot), ext = name.substr(dot);
        std::string path = dir + gDirUtilp->getDirDelimiter() + name;
        for (S32 i = 2; LLFile::isfile(path) && i < 1000; ++i)
        {
            path = dir + gDirUtilp->getDirDelimiter() + stem + "_" + std::to_string(i) + ext;
        }
        LL_INFOS("MGPhone") << "camera: the page's photo goes to " << path << " (offered " << suggested << ")" << LL_ENDL;
        MGFloaterPhone* phone = LLFloaterReg::findTypedInstance<MGFloaterPhone>("mg_phone");
        if (phone)
        {
            phone->pushToPage(LLSD().with("type", "cam_download").with("path", path));
        }
        return path;
    }
    // </MG>
}
