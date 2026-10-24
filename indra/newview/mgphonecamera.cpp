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
        MGCameraGuides(const LLView::Params& p) : LLView(p), mAspect(0.f), mGrid(0) {}

        F32 mAspect;    // width / height, 0 = the window's
        S32 mGrid;      // 0 off, 1 thirds, 2 center, 3 golden

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
            LLView::draw();
        }
    };

    MGCameraGuides* sGuides = nullptr;
    std::string sGuideAspect = "native";
    std::string sGuideGrid = "off";

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
        const S32 g = (grid == "thirds") ? 1 : (grid == "center") ? 2 : (grid == "golden") ? 3 : 0;
        if (a <= 0.f && g == 0)
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
    };
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
        return LLSD().with("ok", false).with("error", "unknown command");
    }

    void restore()
    {
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
}
