/**
 * @file mgphoneanimstudio.cpp
 * @brief <MG> MG Phone Animation Studio (see mgphoneanimstudio.h).
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

#include "mgphoneanimstudio.h"
#include "mgmotionread.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <functional>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include "llagent.h"
#include "llagentbenefits.h"
#include "llagentcamera.h"
#include "llassetstorage.h"
#include "llfilesystem.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llviewerinventory.h"
#include "llbvhloader.h"
#include "lldatapacker.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "llfile.h"
#include "llfilepicker.h"
#include "llfloaterperms.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llkeyboard.h"
#include "llkeyframemotion.h"
#include "llquantize.h"
#include "llrender2dutils.h"
#include "llrootview.h"
#include "llsdserialize.h"
#include "lltool.h"
#include "lltoolcomp.h"
#include "lltoolfocus.h"
#include "lltoolmgr.h"
#include "llviewerassetupload.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewermenufile.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "pipeline.h"
#include "mgfloaterphone.h"
#include "mgphoneposer.h"

namespace
{
    void push(const LLSD& ev)
    {
        MGFloaterPhone* phone = LLFloaterReg::findTypedInstance<MGFloaterPhone>("mg_phone");
        if (phone) phone->pushToPage(ev);
    }

    LLSD fail(const std::string& why) { return LLSD().with("ok", false).with("error", why); }

    // ------------------------------------------------------------------
    // studio state
    // ------------------------------------------------------------------
    struct Studio
    {
        bool open = false;
        bool aoWas = false;
        bool tool = false;               // our transient tool has the mouse in the 3D view
        std::string gizmo = "rotate";    // rotate (FK) | move (IK: the poser turns the two bones above)
        std::string group = "body";      // which joint dots: body | hands | face
        LLUUID local;                    // the clip playing on this screen
        std::vector<LLUUID> oldLocals;   // stopped previews, removed once eased out
        LLFrameTimer cleanTimer;
        bool playing = false;
        bool wasDragging = false;
        bool picking = false;            // a file dialog is open
        S32 pending = 0;                 // uploads under way
        std::map<std::string, LLUUID> worldPlaying;   // asset id string -> played for everyone
        LLUUID invPreview;               // an inventory animation previewed on this screen
    } sS;

    // ------------------------------------------------------------------
    // the .anim format (llkeyframemotion.cpp, version 1.0)
    // ------------------------------------------------------------------
    // the emotes the uploader offers (llfloaterbvhpreview.cpp), by their animation names
    bool knownEmote(const std::string& e)
    {
        static const std::set<std::string> OK = { "express_open_mouth", "express_afraid_emote", "express_anger_emote", "express_toothsmile",
            "express_bored_emote", "express_cry_emote", "express_disdain", "express_embarrassed_emote", "express_frown", "express_kiss",
            "express_laugh_emote", "express_tongue_out", "express_repulsed_emote", "express_sad_emote", "express_shrug_emote",
            "express_smile", "express_surprise_emote", "express_wink_emote", "express_worry_emote" };
        return OK.count(e) > 0;
    }

    struct RotKey { F32 t; LLQuaternion q; };
    struct PosKey { F32 t; LLVector3 p; };
    struct JointTrack { std::string name; S32 pri = -1; std::vector<RotKey> rot; std::vector<PosKey> pos; };

    // the page's anim (LLSD) into bytes, checked; empty + err on a problem
    std::vector<U8> buildAnim(const LLSD& a, std::string& err)
    {
        std::vector<U8> none;
        if (!isAgentAvatarValid()) { err = "avatar"; return none; }
        const F32 dur = (F32)a["duration"].asReal();
        if (!(dur > 0.f) || dur > MAX_ANIM_DURATION) { err = "duration"; return none; }
        const S32 pri = llclamp((S32)a["priority"].asInteger(), 0, 6);
        const bool loop = a["loop"].asBoolean();
        F32 loopIn = llclamp((F32)a["loop_in"].asReal(), 0.f, dur);
        F32 loopOut = a.has("loop_out") ? llclamp((F32)a["loop_out"].asReal(), 0.f, dur) : dur;
        if (loopOut < loopIn) std::swap(loopIn, loopOut);
        if (!loop) { loopIn = 0.f; loopOut = dur; }
        const F32 easeIn = llclamp((F32)a["ease_in"].asReal(), 0.f, 10.f);
        const F32 easeOut = llclamp((F32)a["ease_out"].asReal(), 0.f, 10.f);
        const U32 hand = (U32)llclamp((S32)a["hand"].asInteger(), 0, 13);
        std::string emote = a["emote"].asString();
        if (!emote.empty() && !knownEmote(emote)) emote.clear();

        std::vector<JointTrack> tracks;
        const LLSD& js = a["joints"];
        for (LLSD::map_const_iterator it = js.beginMap(); it != js.endMap(); ++it)
        {
            if (!gAgentAvatarp->getJoint(it->first)) continue;   // not a joint of the skeleton
            JointTrack tr;
            tr.name = it->first;
            if (it->second.has("pri")) tr.pri = llclamp((S32)it->second["pri"].asInteger(), 0, 6);
            const LLSD& r = it->second["r"];
            for (S32 i = 0; i + 4 < (S32)r.size(); i += 5)
            {
                RotKey k;
                k.t = llclamp((F32)r[i].asReal(), 0.f, dur);
                k.q = LLQuaternion((F32)r[i + 1].asReal(), (F32)r[i + 2].asReal(), (F32)r[i + 3].asReal(), (F32)r[i + 4].asReal());
                if (!k.q.isFinite() || !llfinite(k.t)) continue;
                k.q.normalize();
                tr.rot.push_back(k);
            }
            const LLSD& p = it->second["p"];
            for (S32 i = 0; i + 3 < (S32)p.size(); i += 4)
            {
                PosKey k;
                k.t = llclamp((F32)p[i].asReal(), 0.f, dur);
                k.p.set((F32)p[i + 1].asReal(), (F32)p[i + 2].asReal(), (F32)p[i + 3].asReal());
                if (!k.p.isFinite() || !llfinite(k.t)) continue;
                for (S32 c = 0; c < 3; ++c) k.p.mV[c] = llclamp(k.p.mV[c], -LL_MAX_PELVIS_OFFSET, LL_MAX_PELVIS_OFFSET);
                tr.pos.push_back(k);
            }
            std::stable_sort(tr.rot.begin(), tr.rot.end(), [](const RotKey& x, const RotKey& y) { return x.t < y.t; });
            std::stable_sort(tr.pos.begin(), tr.pos.end(), [](const PosKey& x, const PosKey& y) { return x.t < y.t; });
            if (!tr.rot.empty() || !tr.pos.empty()) tracks.push_back(tr);
        }
        if (tracks.empty()) { err = "empty"; return none; }

        size_t size = 128 + emote.size();
        for (const JointTrack& tr : tracks) size += tr.name.size() + 1 + 12 + tr.rot.size() * 8 + tr.pos.size() * 8;
        std::vector<U8> buf(size);
        LLDataPackerBinaryBuffer dp(buf.data(), (S32)buf.size());
        dp.packU16(KEYFRAME_MOTION_VERSION, "version");
        dp.packU16(KEYFRAME_MOTION_SUBVERSION, "sub_version");
        dp.packS32(pri, "base_priority");
        dp.packF32(dur, "duration");
        dp.packString(emote, "emote_name");
        dp.packF32(loopIn, "loop_in_point");
        dp.packF32(loopOut, "loop_out_point");
        dp.packS32(loop ? 1 : 0, "loop");
        dp.packF32(easeIn, "ease_in_duration");
        dp.packF32(easeOut, "ease_out_duration");
        dp.packU32(hand, "hand_pose");
        dp.packU32((U32)tracks.size(), "num_joints");
        for (const JointTrack& tr : tracks)
        {
            dp.packString(tr.name, "joint_name");
            dp.packS32(tr.pri >= 0 ? tr.pri : pri, "joint_priority");
            dp.packS32((S32)tr.rot.size(), "num_rot_keys");
            for (const RotKey& k : tr.rot)
            {
                const LLVector3 v = k.q.packToVector3();   // the vector part, w >= 0 (as LLKeyframeMotion::serialize)
                dp.packU16(F32_to_U16(k.t, 0.f, dur), "time");
                dp.packU16(F32_to_U16(v.mV[VX], -1.f, 1.f), "rot_angle_x");
                dp.packU16(F32_to_U16(v.mV[VY], -1.f, 1.f), "rot_angle_y");
                dp.packU16(F32_to_U16(v.mV[VZ], -1.f, 1.f), "rot_angle_z");
            }
            dp.packS32((S32)tr.pos.size(), "num_pos_keys");
            for (const PosKey& k : tr.pos)
            {
                dp.packU16(F32_to_U16(k.t, 0.f, dur), "time");
                dp.packU16(F32_to_U16(k.p.mV[VX], -LL_MAX_PELVIS_OFFSET, LL_MAX_PELVIS_OFFSET), "pos_x");
                dp.packU16(F32_to_U16(k.p.mV[VY], -LL_MAX_PELVIS_OFFSET, LL_MAX_PELVIS_OFFSET), "pos_y");
                dp.packU16(F32_to_U16(k.p.mV[VZ], -LL_MAX_PELVIS_OFFSET, LL_MAX_PELVIS_OFFSET), "pos_z");
            }
        }
        dp.packS32(0, "num_constraints");
        buf.resize(dp.getCurrentSize());
        return buf;
    }

    // .anim bytes into the page's form (import, round trips)
    bool decodeAnim(const std::vector<U8>& bytes, LLSD& out, std::string& err)
    {
        if (bytes.size() < 32) { err = "short"; return false; }
        std::vector<U8> copy = bytes;
        LLDataPackerBinaryBuffer dp(copy.data(), (S32)copy.size());
        U16 ver = 0, sub = 0;
        S32 pri = 0, loop = 0;
        F32 dur = 0.f, loopIn = 0.f, loopOut = 0.f, easeIn = 0.f, easeOut = 0.f;
        U32 hand = 0, nj = 0;
        std::string emote;
        if (!dp.unpackU16(ver, "version") || !dp.unpackU16(sub, "sub_version")) { err = "header"; return false; }
        if (ver != KEYFRAME_MOTION_VERSION || sub != KEYFRAME_MOTION_SUBVERSION) { err = "old_format"; return false; }
        if (!dp.unpackS32(pri, "base_priority") || !dp.unpackF32(dur, "duration") || !dp.unpackString(emote, "emote_name") ||
            !dp.unpackF32(loopIn, "loop_in_point") || !dp.unpackF32(loopOut, "loop_out_point") || !dp.unpackS32(loop, "loop") ||
            !dp.unpackF32(easeIn, "ease_in_duration") || !dp.unpackF32(easeOut, "ease_out_duration") || !dp.unpackU32(hand, "hand_pose") ||
            !dp.unpackU32(nj, "num_joints"))
        {
            err = "header"; return false;
        }
        if (!(dur > 0.f) || dur > MAX_ANIM_DURATION || nj > 512) { err = "header"; return false; }
        LLSD joints = LLSD::emptyMap();
        for (U32 j = 0; j < nj; ++j)
        {
            std::string name;
            S32 jpri = 0, nr = 0, np = 0;
            if (!dp.unpackString(name, "joint_name") || !dp.unpackS32(jpri, "joint_priority") || !dp.unpackS32(nr, "num_rot_keys") || nr < 0 || nr > 100000)
            {
                err = "joint"; return false;
            }
            LLSD r = LLSD::emptyArray(), p = LLSD::emptyArray();
            for (S32 k = 0; k < nr; ++k)
            {
                U16 t, x, y, z;
                if (!dp.unpackU16(t, "time") || !dp.unpackU16(x, "x") || !dp.unpackU16(y, "y") || !dp.unpackU16(z, "z")) { err = "keys"; return false; }
                LLQuaternion q;
                q.unpackFromVector3(LLVector3(U16_to_F32(x, -1.f, 1.f), U16_to_F32(y, -1.f, 1.f), U16_to_F32(z, -1.f, 1.f)));
                r.append(U16_to_F32(t, 0.f, dur));
                for (S32 c = 0; c < 4; ++c) r.append(q.mQ[c]);
            }
            if (!dp.unpackS32(np, "num_pos_keys") || np < 0 || np > 100000) { err = "joint"; return false; }
            for (S32 k = 0; k < np; ++k)
            {
                U16 t, x, y, z;
                if (!dp.unpackU16(t, "time") || !dp.unpackU16(x, "x") || !dp.unpackU16(y, "y") || !dp.unpackU16(z, "z")) { err = "keys"; return false; }
                p.append(U16_to_F32(t, 0.f, dur));
                p.append(U16_to_F32(x, -LL_MAX_PELVIS_OFFSET, LL_MAX_PELVIS_OFFSET));
                p.append(U16_to_F32(y, -LL_MAX_PELVIS_OFFSET, LL_MAX_PELVIS_OFFSET));
                p.append(U16_to_F32(z, -LL_MAX_PELVIS_OFFSET, LL_MAX_PELVIS_OFFSET));
            }
            LLSD e = LLSD().with("pri", jpri);
            if (nr) e["r"] = r;
            if (np) e["p"] = p;
            joints[name] = e;
        }
        out = LLSD().with("duration", dur).with("priority", pri).with("loop", loop != 0).with("loop_in", loopIn).with("loop_out", loopOut)
                    .with("ease_in", easeIn).with("ease_out", easeOut).with("hand", (S32)hand).with("emote", emote).with("joints", joints);
        return true;
    }

    // the bytes read by the viewer's own reader: what everyone's viewer will do with them
    bool validate(const std::vector<U8>& bytes)
    {
        if (!isAgentAvatarValid() || bytes.empty()) return false;
        LLUUID id;
        id.generate();
        LLKeyframeMotion* m = dynamic_cast<LLKeyframeMotion*>(gAgentAvatarp->createMotion(id));
        if (!m) return false;
        std::vector<U8> copy = bytes;
        LLDataPackerBinaryBuffer dp(copy.data(), (S32)copy.size());
        const bool ok = m->deserialize(dp, id, false);
        gAgentAvatarp->removeMotion(id);
        LLKeyframeDataCache::removeKeyframeData(id);
        return ok;
    }

    // ------------------------------------------------------------------
    // posing (Firestorm's poser through mgphoneposer.cpp)
    // ------------------------------------------------------------------
    LLSD poser(const std::string& c, const LLSD& a = LLSD()) { return MGPhonePoser::runCommand(c, a); }

    void toolOn();
    void toolOff();

    // the joints the studio animates: every one of body, hands and face is held by the poser,
    // so nothing else (the AO, blinking, the default hands) moves them while you edit
    void holdAll()
    {
        LLSD groups = LLSD::emptyArray();
        groups.append("body"); groups.append("hands"); groups.append("face");
        MGPhonePoser::applyPose(MGPhonePoser::capturePose(groups), false);
    }

    // A clip the .anim format cannot hold (the body more than 5 m away, longer than 60 s: a long
    // route) is played by the poser itself, frame by frame, from the page's anim form. Only this
    // viewer shows it (it is the studio's preview); the export splits it into in-place animations
    // and a mover prim.
    struct FreeTrack { std::string joint; std::vector<F32> rt; std::vector<LLQuaternion> rq; std::vector<F32> pt; std::vector<LLVector3> pv; size_t ri = 0, pi = 0; };
    struct FreePlay { bool on = false; std::vector<FreeTrack> tracks; F32 dur = 1.f, loopIn = 0.f, loopOut = 1.f; bool loop = false; LLFrameTimer timer; } sFree;

    bool needsFree(const LLSD& a)
    {
        if ((F32)a["duration"].asReal() > MAX_ANIM_DURATION) return true;
        const LLSD& js = a["joints"];
        for (LLSD::map_const_iterator it = js.beginMap(); it != js.endMap(); ++it)
        {
            const LLSD& p = it->second["p"];
            for (S32 k = 0; k + 3 < (S32)p.size(); k += 4)
                for (S32 c = 1; c <= 3; ++c)
                    if (fabsf((F32)p[k + c].asReal()) > LL_MAX_PELVIS_OFFSET - 0.01f) return true;
        }
        return false;
    }

    bool startFree(const LLSD& a)
    {
        sFree = FreePlay();
        sFree.dur = llclamp((F32)a["duration"].asReal(), 0.05f, 900.f);
        sFree.loop = a["loop"].asBoolean();
        sFree.loopIn = llclamp((F32)a["loop_in"].asReal(), 0.f, sFree.dur);
        sFree.loopOut = llclamp((F32)a["loop_out"].asReal(), sFree.loopIn, sFree.dur);
        const LLSD& js = a["joints"];
        for (LLSD::map_const_iterator it = js.beginMap(); it != js.endMap(); ++it)
        {
            FreeTrack t;
            t.joint = it->first;
            const LLSD& r = it->second["r"];
            for (S32 k = 0; k + 4 < (S32)r.size(); k += 5)
            {
                t.rt.push_back((F32)r[k].asReal());
                LLQuaternion q((F32)r[k + 1].asReal(), (F32)r[k + 2].asReal(), (F32)r[k + 3].asReal(), (F32)r[k + 4].asReal());
                q.normalize();
                t.rq.push_back(q);
            }
            const LLSD& p = it->second["p"];
            for (S32 k = 0; k + 3 < (S32)p.size(); k += 4)
            {
                t.pt.push_back((F32)p[k].asReal());
                t.pv.push_back(LLVector3((F32)p[k + 1].asReal(), (F32)p[k + 2].asReal(), (F32)p[k + 3].asReal()));
            }
            if (!t.rt.empty() || !t.pt.empty()) sFree.tracks.push_back(t);
        }
        if (sFree.tracks.empty()) return false;
        sFree.on = true;
        sFree.timer.reset();
        return true;
    }

    // the segment holding t (searched from the last one: playback moves forward)
    S32 seg(const std::vector<F32>& ts, size_t& hint, F32 t)
    {
        if (ts.size() < 2) return 0;
        if (hint >= ts.size() - 1 || ts[hint] > t) hint = 0;
        while (hint + 2 < ts.size() && ts[hint + 1] < t) ++hint;
        return (S32)hint;
    }

    void stepFree()
    {
        F32 t = sFree.timer.getElapsedTimeF32();
        if (sFree.loop && sFree.loopOut - sFree.loopIn > 0.01f && t > sFree.loopOut) t = sFree.loopIn + fmodf(t - sFree.loopIn, sFree.loopOut - sFree.loopIn);
        t = llmin(t, sFree.dur);
        LLSD pose = LLSD::emptyMap();
        for (FreeTrack& tr : sFree.tracks)
        {
            LLSD j = LLSD::emptyMap();
            if (!tr.rt.empty())
            {
                const S32 i = seg(tr.rt, tr.ri, t);
                LLQuaternion q = tr.rq[i];
                if (i + 1 < (S32)tr.rt.size() && t > tr.rt[i])
                {
                    const F32 u = llclamp((t - tr.rt[i]) / llmax(1e-5f, tr.rt[i + 1] - tr.rt[i]), 0.f, 1.f);
                    q = slerp(u, tr.rq[i], tr.rq[i + 1]);
                }
                j["r"] = LLSD().with(0, q.mQ[VX]).with(1, q.mQ[VY]).with(2, q.mQ[VZ]).with(3, q.mQ[VW]);
            }
            if (!tr.pt.empty())
            {
                const S32 i = seg(tr.pt, tr.pi, t);
                LLVector3 v = tr.pv[i];
                if (i + 1 < (S32)tr.pt.size() && t > tr.pt[i])
                {
                    const F32 u = llclamp((t - tr.pt[i]) / llmax(1e-5f, tr.pt[i + 1] - tr.pt[i]), 0.f, 1.f);
                    v = lerp(tr.pv[i], tr.pv[i + 1], u);
                }
                j["p"] = LLSD().with(0, v.mV[0]).with(1, v.mV[1]).with(2, v.mV[2]);
            }
            pose[tr.joint] = j;
        }
        MGPhonePoser::applyPose(pose, true);
    }

    void stopLocal()
    {
        if (sS.invPreview.notNull() && isAgentAvatarValid()) gAgentAvatarp->stopMotion(sS.invPreview, true);
        sS.invPreview.setNull();
        sFree.on = false;
        sFree.tracks.clear();
        if (sS.local.notNull() && isAgentAvatarValid())
        {
            gAgentAvatarp->stopMotion(sS.local, true);
            sS.oldLocals.push_back(sS.local);
        }
        sS.local.setNull();
        sS.playing = false;
    }

    bool playLocal(const std::vector<U8>& bytes)
    {
        if (!isAgentAvatarValid() || bytes.empty()) return false;
        LLUUID id;
        id.generate();
        LLKeyframeMotion* m = dynamic_cast<LLKeyframeMotion*>(gAgentAvatarp->createMotion(id));
        if (!m) return false;
        std::vector<U8> copy = bytes;
        LLDataPackerBinaryBuffer dp(copy.data(), (S32)copy.size());
        if (!m->deserialize(dp, id, false))
        {
            gAgentAvatarp->removeMotion(id);
            LLKeyframeDataCache::removeKeyframeData(id);
            return false;
        }
        stopLocal();
        gAgentAvatarp->startMotion(id);
        sS.local = id;
        sS.playing = true;
        return true;
    }

    // ------------------------------------------------------------------
    // the timeline at the bottom of the 3D view (as Creator Lab's toolbar at the top): the page
    // says what is on it (studio_timeline, every 2 s at least) and hears the clicks (studio_tl)
    // ------------------------------------------------------------------
    struct Track { std::string id, label, kind; std::vector<S32> keys; std::set<S32> sel; };
    // a motion clip on the clip row: where it starts, how long it lasts on the timeline, the length
    // of one cycle (loop dividers), the blend at its head, its speed (shown as %), its colour
    struct ClipBlock { std::string id, name; S32 start = 0, len = 1, blend = 0; F32 cyc = 0.f, speed = 1.f; LLColor4 col = LLColor4(0.16f, 0.55f, 0.5f, 1.f); bool sel = false; };
    struct Timeline
    {
        S32 frames = 120;
        F32 fps = 30.f;
        F32 frame = 0.f;
        bool playing = false;
        F32 playFrom = 0.f;
        LLFrameTimer playTimer;
        bool loop = false;
        S32 loopIn = 0, loopOut = 120;
        bool autoKey = false;
        std::string hint, title, selTrack;
        std::vector<Track> tracks;
        LLFrameTimer beat;
        bool any = false;
        F32 v0 = 0.f, v1 = 120.f;   // the frames in view
        bool zoomed = false;
        std::vector<ClipBlock> clips;
        std::string edgeMode = "loop";   // what dragging a clip's end does: loop | speed | resize
    } sTL;

    bool tlLive() { return sTL.any && sTL.beat.getElapsedTimeF32() < 6.f; }

    F32 tlFrameNow()
    {
        if (!sTL.playing) return sTL.frame;
        F32 f = sTL.playFrom + sTL.playTimer.getElapsedTimeF32() * sTL.fps;
        if (sTL.loop && sTL.loopOut > sTL.loopIn && f > (F32)sTL.loopOut)
            f = (F32)sTL.loopIn + std::fmod(f - (F32)sTL.loopIn, (F32)(sTL.loopOut - sTL.loopIn));
        return llmin(f, (F32)sTL.frames);
    }

    const S32 HEAD_H = 26, RULER_H = 18, LAB_W = 96, PAD = 6, BTN = 22, CLIP_H = 22;
    S32 ROW_H = 17;   // the rows' height: the timeline's size, set by dragging its edge
    // the resident's layout: docked at the bottom or the top, row height, width (share of the space)
    struct TLLayout { bool top = false; S32 rowH = 17; F32 width = 1.f; bool loaded = false; } sTLL;
    void loadTLLayout();
    void saveTLLayout();
    // the shortcut bar at the top of the 3D view (items from the page)
    struct BarItem { std::string id, tip, label; bool on = false, sep = false; };
    std::vector<BarItem> sBar;
    const S32 BAR_TOP = 64, BAR_BW = 30;
    const LLColor4 C_BG(0.03f, 0.05f, 0.08f, 0.9f), C_LINE(1.f, 1.f, 1.f, 0.08f), C_TEXT(0.84f, 0.89f, 0.94f, 1.f),
                   C_DIM(0.55f, 0.62f, 0.7f, 1.f), C_CYAN(0.13f, 0.83f, 0.93f, 1.f), C_GREEN(0.36f, 0.85f, 0.45f, 1.f),
                   C_RED(1.f, 0.33f, 0.36f, 1.f), C_GOLD(1.f, 0.82f, 0.25f, 1.f), C_PINK(1.f, 0.48f, 0.72f, 1.f);

    struct Btn { std::string id, tip; LLRect r; bool on = false; };

    class MGStudioTimeline : public LLView
    {
    public:
        MGStudioTimeline(const LLView::Params& p) : LLView(p) {}

        // D_PRESS: pressed on empty track space (a click jumps there, a drag becomes a box)
        enum Drag { D_NONE, D_SCRUB, D_KEYS, D_LIN, D_LOUT, D_PRESS, D_BOX, D_PAN, D_THUMB, D_CMOVE, D_CEDGEL, D_CEDGER, D_SIZEH, D_SIZEW };
        S32 mSize0 = 0;
        std::string mClip;          // the clip being dragged
        S32 mClipDelta = 0;
        Drag mDrag = D_NONE;
        S32 mDragFrom = 0, mDragDelta = 0;
        S32 mDownX = 0, mDownY = 0, mCurX = 0, mCurY = 0;
        F32 mPanV0 = 0.f;
        bool mBoxAdd = false;
        S32 mLastPushed = -1;
        LLFrameTimer mPushTimer;
        std::string mHoverTip;

        static constexpr S32 SCROLL_H = 9;
        LLRect panel() const
        {
            const LLRect wv = gViewerWindow->getWorldViewRectScaled();
            S32 left = wv.mLeft + 14, right = wv.mRight - 14;
            if (!sTLL.loaded) loadTLLayout();
            ROW_H = llclamp(sTLL.rowH, 12, 40);
            const S32 rows = llmax(1, (S32)sTL.tracks.size());
            const S32 h = HEAD_H + RULER_H + CLIP_H + rows * ROW_H + SCROLL_H + PAD * 2;
            // docked at the bottom, or at the top under the shortcut bar
            S32 bottom = wv.mBottom + 10, top = bottom + h;
            if (sTLL.top) { top = wv.mTop - (sBar.empty() ? 12 : BAR_TOP + BAR_BW + 34); bottom = top - h; }
            // beside the phone, never under it
            if (LLFloater* ph = LLFloaterReg::findInstance("mg_phone"))
            {
                LLRect pr;
                if (ph->getVisible() && !ph->isMinimized() && ph->localRectToOtherView(ph->getLocalRect(), &pr, gViewerWindow->getRootView()) &&
                    pr.mBottom < top && pr.mTop > bottom && pr.mRight > left && pr.mLeft < right)
                {
                    if (pr.getCenterX() > wv.getCenterX()) right = llmin(right, pr.mLeft - 10);
                    else left = llmax(left, pr.mRight + 10);
                }
            }
            // the width the resident chose (centred in the free space)
            const S32 avail = right - left, w = llmax(360, ll_round((F32)avail * llclamp(sTLL.width, 0.3f, 1.f)));
            if (w < avail) { left += (avail - w) / 2; right = left + w; }
            if (right - left < 360) right = left + 360;
            return LLRect(left, top, right, bottom);
        }
        LLRect ruler(const LLRect& P) const { return LLRect(P.mLeft + LAB_W, P.mTop - HEAD_H, P.mRight - PAD, P.mTop - HEAD_H - RULER_H); }
        S32 x0(const LLRect& P) const { return ruler(P).mLeft + 6; }
        S32 x1(const LLRect& P) const { return ruler(P).mRight - 6; }
        S32 xOf(const LLRect& P, F32 f) const
        {
            const F32 span = llmax(1.f, sTL.v1 - sTL.v0);
            return x0(P) + ll_round((f - sTL.v0) / span * (F32)(x1(P) - x0(P)));
        }
        F32 frameAt(const LLRect& P, S32 x) const
        {
            const F32 span = llmax(1.f, sTL.v1 - sTL.v0);
            return sTL.v0 + (F32)(x - x0(P)) / (F32)llmax(1, x1(P) - x0(P)) * span;
        }
        S32 clipTop(const LLRect& P) const { return P.mTop - HEAD_H - RULER_H; }
        S32 rowTop(const LLRect& P, S32 i) const { return P.mTop - HEAD_H - RULER_H - CLIP_H - i * ROW_H; }
        S32 tracksBottom(const LLRect& P) const { return rowTop(P, llmax(1, (S32)sTL.tracks.size())); }
        // the scrollbar: the whole clip (and whatever is in view past it)
        F32 total() const { return llmax((F32)llmax(10, sTL.frames), sTL.v1); }
        LLRect scrollRect(const LLRect& P) const { const S32 t = tracksBottom(P) - 2; return LLRect(x0(P), t, x1(P), t - SCROLL_H + 2); }
        LLRect thumbRect(const LLRect& P) const
        {
            const LLRect s = scrollRect(P);
            const F32 T = total(), w = (F32)s.getWidth();
            S32 a = s.mLeft + ll_round(sTL.v0 / T * w), b = s.mLeft + ll_round(sTL.v1 / T * w);
            if (b - a < 14) b = a + 14;
            return LLRect(a, s.mTop, llmin(b, s.mRight), s.mBottom);
        }
        void setView(F32 v0, F32 span)
        {
            span = llclamp(span, 10.f, (F32)llmax(10, sTL.frames) * 1.5f);
            v0 = llclamp(v0, 0.f, llmax(0.f, (F32)llmax(10, sTL.frames) * 1.2f - span));
            sTL.v0 = v0; sTL.v1 = v0 + span;
            sTL.zoomed = true;
        }

        std::vector<Btn> buttons(const LLRect& P) const
        {
            std::vector<Btn> b;
            auto add = [&](const char* id, const std::string& tip, bool on = false, S32 gapBefore = 0)
            {
                const S32 x = (b.empty() ? P.mLeft + PAD : b.back().r.mRight + 3) + gapBefore;
                const S32 top = P.mTop - 2;
                Btn bt; bt.id = id; bt.tip = tip; bt.on = on; bt.r = LLRect(x, top, x + BTN, top - BTN);
                b.push_back(bt);
            };
            add("start", "First frame"); add("prev", "Previous key"); add("play", sTL.playing ? "Pause" : "Play", sTL.playing);
            add("next", "Next key"); add("end", "Last frame");
            add("loop", "Loop", sTL.loop, 10); add("auto", "Auto key: a key wherever you pose", sTL.autoKey);
            add("key", "Set key (the selected joints, this frame)", false, 10); add("delkey", "Delete the selected keys");
            add("selall", "Select every key of the row", false, 10); add("copy", "Copy the selected keys"); add("paste", "Paste the copied keys at the playhead");
            add("eloop", "Clip ends: drag to LOOP the motion", sTL.edgeMode == "loop", 10); add("espeed", "Clip ends: drag to change its SPEED", sTL.edgeMode == "speed");
            add("eresize", "Clip ends: drag to TRIM it (the cut part comes back if you drag again)", sTL.edgeMode == "resize"); add("break", "Break the selected clip at the playhead");
            add("fit", "Fit the timeline", false, 10);
            bool clipSel = false;
            for (const ClipBlock& c : sTL.clips) if (c.sel) clipSel = true;
            if (clipSel) add("delclip", "Delete the selected clip", false, 10);
            add("dock", sTLL.top ? "Move the timeline to the bottom" : "Move the timeline to the top", false, 10);
            return b;
        }

        // ---------------- drawing ----------------
        static void diamond(S32 x, S32 y, S32 r, const LLColor4& c, bool filled = true)
        {
            gl_triangle_2d(x - r, y, x, y + r, x + r, y, c, filled);
            gl_triangle_2d(x - r, y, x + r, y, x, y - r, c, filled);
        }
        static void icon(const std::string& id, const LLRect& r, const LLColor4& c)
        {
            const S32 cx = r.getCenterX(), cy = r.getCenterY();
            if (id == "play")
            {
                if (sTL.playing) { gl_rect_2d(cx - 5, cy + 6, cx - 2, cy - 6, c); gl_rect_2d(cx + 2, cy + 6, cx + 5, cy - 6, c); }
                else gl_triangle_2d(cx - 4, cy + 6, cx + 6, cy, cx - 4, cy - 6, c, true);
            }
            else if (id == "start") { gl_rect_2d(cx - 6, cy + 6, cx - 4, cy - 6, c); gl_triangle_2d(cx + 5, cy + 6, cx - 3, cy, cx + 5, cy - 6, c, true); }
            else if (id == "end") { gl_rect_2d(cx + 4, cy + 6, cx + 6, cy - 6, c); gl_triangle_2d(cx - 5, cy + 6, cx + 3, cy, cx - 5, cy - 6, c, true); }
            else if (id == "prev") { gl_triangle_2d(cx + 1, cy + 5, cx - 6, cy, cx + 1, cy - 5, c, true); diamond(cx + 4, cy, 3, c); }
            else if (id == "next") { gl_triangle_2d(cx - 1, cy + 5, cx + 6, cy, cx - 1, cy - 5, c, true); diamond(cx - 4, cy, 3, c); }
            else if (id == "loop")
            {
                gGL.color4fv(c.mV);
                gl_circle_2d((F32)cx, (F32)cy, 6.f, 20, false);
                gl_triangle_2d(cx + 3, cy + 3, cx + 9, cy + 3, cx + 6, cy - 1, c, true);
            }
            else if (id == "auto")
            {
                gGL.color4fv(c.mV);
                gl_circle_2d((F32)cx, (F32)cy, 6.f, 20, false);
                if (sTL.autoKey) { gGL.color4fv(C_RED.mV); gl_circle_2d((F32)cx, (F32)cy, 4.f, 16, true); }
            }
            else if (id == "key") { diamond(cx - 1, cy, 6, c); gl_rect_2d(cx + 4, cy + 7, cx + 10, cy + 5, c); gl_rect_2d(cx + 6, cy + 9, cx + 8, cy + 3, c); }
            else if (id == "delkey") { diamond(cx, cy, 6, c, false); gl_line_2d(cx - 4, cy - 4, cx + 4, cy + 4, c); gl_line_2d(cx - 4, cy + 4, cx + 4, cy - 4, c); }
            else if (id == "selall") { gl_rect_2d(cx - 7, cy + 6, cx + 7, cy - 6, c, false); diamond(cx - 3, cy, 2, c); diamond(cx + 3, cy, 2, c); }
            else if (id == "copy") { gl_rect_2d(cx - 6, cy + 6, cx + 2, cy - 2, c, false); gl_rect_2d(cx - 2, cy + 2, cx + 6, cy - 6, c, false); }
            else if (id == "paste") { gl_rect_2d(cx - 5, cy + 5, cx + 5, cy - 7, c, false); gl_rect_2d(cx - 2, cy + 7, cx + 2, cy + 4, c, true); diamond(cx, cy - 1, 2, c); }
            else if (id == "eloop") { gGL.color4fv(c.mV); gl_circle_2d((F32)cx, (F32)cy, 5.f, 16, false); gl_rect_2d(cx - 9, cy + 7, cx - 7, cy - 7, c); gl_rect_2d(cx + 7, cy + 7, cx + 9, cy - 7, c); }
            else if (id == "espeed") { gl_line_2d(cx - 8, cy, cx + 8, cy, c); gl_triangle_2d(cx + 8, cy + 4, cx + 8, cy - 4, cx + 11, cy, c, true); gl_triangle_2d(cx - 8, cy + 4, cx - 8, cy - 4, cx - 11, cy, c, true); gl_rect_2d(cx - 2, cy + 7, cx + 2, cy + 5, c); }
            else if (id == "eresize") { gl_rect_2d(cx - 8, cy + 6, cx + 3, cy - 6, c, false); gl_rect_2d(cx + 5, cy + 8, cx + 7, cy - 8, c); }
            else if (id == "break") { gGL.color4fv(c.mV); gl_circle_2d((F32)cx - 4, (F32)cy - 5, 2.5f, 10, false); gl_circle_2d((F32)cx + 4, (F32)cy - 5, 2.5f, 10, false); gl_line_2d(cx - 3, cy - 3, cx + 5, cy + 7, c); gl_line_2d(cx + 3, cy - 3, cx - 5, cy + 7, c); }
            else if (id == "delclip") { gl_rect_2d(cx - 9, cy + 5, cx + 9, cy - 5, c, false); gl_line_2d(cx - 4, cy - 3, cx + 4, cy + 3, C_RED); gl_line_2d(cx - 4, cy + 3, cx + 4, cy - 3, C_RED); }
            else if (id == "dock")
            {
                gl_rect_2d(cx - 8, cy + 7, cx + 8, cy - 7, c, false);
                if (sTLL.top) gl_rect_2d(cx - 6, cy - 2, cx + 6, cy - 5, c, true); else gl_rect_2d(cx - 6, cy + 5, cx + 6, cy + 2, c, true);
            }
            else if (id == "fit")
            {
                gl_line_2d(cx - 7, cy + 5, cx - 7, cy - 5, c); gl_line_2d(cx - 7, cy + 5, cx - 4, cy + 5, c); gl_line_2d(cx - 7, cy - 5, cx - 4, cy - 5, c);
                gl_line_2d(cx + 7, cy + 5, cx + 7, cy - 5, c); gl_line_2d(cx + 7, cy + 5, cx + 4, cy + 5, c); gl_line_2d(cx + 7, cy - 5, cx + 4, cy - 5, c);
            }
        }
        static LLColor4 kindColor(const std::string& k)
        {
            if (k == "hands") return C_GOLD;
            if (k == "face") return C_PINK;
            if (k == "all") return C_TEXT;
            return LLColor4(0.78f, 0.84f, 0.92f, 1.f);
        }
        static S32 tickStep(F32 pxPerFrame, F32 minPx)
        {
            static const S32 STEPS[] = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200, 1800 };
            for (S32 s : STEPS) if (s * pxPerFrame >= minPx) return s;
            return 1800;
        }
        S32 selectedCount() const { S32 n = 0; for (const Track& t : sTL.tracks) n += (S32)t.sel.size(); return n; }

        void draw() override
        {
            if (!tlLive()) return;
            const LLRect P = panel();
            const LLRect R = ruler(P);
            const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
            const F32 fnow = tlFrameNow();
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

            // panel
            gl_rect_2d(P, C_BG, true);
            gl_rect_2d(P.mLeft, P.mTop, P.mRight, P.mTop - 1, LLColor4(0.13f, 0.83f, 0.93f, 0.55f), true);
            {
                // the grips: drag to make it taller / shorter (the edge facing the 3D view) and wider / narrower
                const S32 gy = sTLL.top ? P.mBottom + 2 : P.mTop - 2, gx = P.getCenterX();
                const LLColor4 g = mDrag == D_SIZEH ? C_CYAN : LLColor4(1.f, 1.f, 1.f, 0.35f);
                gl_rect_2d(gx - 22, gy + 1, gx + 22, gy - 1, g, true);
                const LLColor4 gw = mDrag == D_SIZEW ? C_CYAN : LLColor4(1.f, 1.f, 1.f, 0.35f);
                gl_rect_2d(P.mRight - 3, P.getCenterY() + 14, P.mRight - 1, P.getCenterY() - 14, gw, true);
            }
            // buttons
            const std::vector<Btn> bts = buttons(P);
            for (const Btn& b : bts)
            {
                if (b.on) gl_rect_2d(b.r, LLColor4(0.07f, 0.24f, 0.3f, 1.f), true);
                icon(b.id, b.r, b.on ? C_CYAN : C_TEXT);
            }
            // ruler + track area backgrounds
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gl_rect_2d(R, LLColor4(1.f, 1.f, 1.f, 0.05f), true);
            const S32 rows = (S32)sTL.tracks.size();
            for (S32 i = 0; i < rows; ++i)
            {
                const S32 top = rowTop(P, i);
                const bool selRow = sTL.tracks[i].id == sTL.selTrack;
                if (selRow) gl_rect_2d(P.mLeft + 2, top, R.mRight, top - ROW_H, LLColor4(0.13f, 0.83f, 0.93f, 0.1f), true);
                else if (i % 2) gl_rect_2d(P.mLeft + 2, top, R.mRight, top - ROW_H, LLColor4(1.f, 1.f, 1.f, 0.025f), true);
            }
            const S32 tb = tracksBottom(P);
            // outside the loop: darker
            if (sTL.loop)
            {
                const S32 xi = llclamp(xOf(P, (F32)sTL.loopIn), x0(P), x1(P)), xo = llclamp(xOf(P, (F32)sTL.loopOut), x0(P), x1(P));
                if (xi > x0(P)) gl_rect_2d(x0(P), R.mTop, xi, tb, LLColor4(0.f, 0.f, 0.f, 0.35f), true);
                if (xo < x1(P)) gl_rect_2d(xo, R.mTop, x1(P), tb, LLColor4(0.f, 0.f, 0.f, 0.35f), true);
            }
            // the clip's end
            const S32 xe = llclamp(xOf(P, (F32)sTL.frames), x0(P), x1(P));
            if (xe < x1(P)) gl_rect_2d(xe, R.mTop, x1(P), tb, LLColor4(0.f, 0.f, 0.f, 0.5f), true);

            // ticks
            const F32 ppf = (F32)(x1(P) - x0(P)) / llmax(1.f, sTL.v1 - sTL.v0);
            const S32 major = tickStep(ppf, 46.f), minor = llmax(1, tickStep(ppf, 7.f));
            const S32 fStart = llmax(0, (S32)std::floor(sTL.v0)), fEnd = (S32)std::ceil(sTL.v1);
            for (S32 f = (fStart / minor) * minor; f <= fEnd; f += minor)
            {
                const S32 x = xOf(P, (F32)f);
                if (x < x0(P) || x > x1(P)) continue;
                const bool big = f % major == 0;
                gl_rect_2d(x, R.mBottom + (big ? 9 : 4), x + 1, R.mBottom, big ? LLColor4(1.f, 1.f, 1.f, 0.45f) : LLColor4(1.f, 1.f, 1.f, 0.2f), true);
                if (big) gl_rect_2d(x, R.mBottom, x + 1, tb, C_LINE, true);
            }
            for (S32 f = (fStart / major) * major; f <= fEnd; f += major)
            {
                const S32 x = xOf(P, (F32)f);
                if (x < x0(P) - 2 || x > x1(P) - 10) continue;
                font->renderUTF8(llformat("%d", f), 0, x + 3, R.mBottom + 7, C_DIM, LLFontGL::LEFT, LLFontGL::BASELINE);
            }
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

            // keys (the selection follows a drag, in every row)
            for (S32 i = 0; i < rows; ++i)
            {
                const Track& tr = sTL.tracks[i];
                const S32 cy = rowTop(P, i) - ROW_H / 2;
                const LLColor4 kc = kindColor(tr.kind);
                for (S32 k : tr.keys)
                {
                    const bool sel = tr.sel.count(k) > 0;
                    const S32 f = (mDrag == D_KEYS && sel) ? k + mDragDelta : k;
                    const S32 x = xOf(P, (F32)f);
                    if (x < x0(P) - 4 || x > x1(P) + 4) continue;
                    diamond(x, cy, sel ? 5 : 4, LLColor4(0.f, 0.f, 0.f, 0.7f));
                    diamond(x, cy, sel ? 4 : 3, sel ? C_CYAN : kc);
                }
            }
            // the clip row: motion blocks (name, speed %), loop dividers, the head blend
            {
                const S32 ct = clipTop(P), cb = ct - CLIP_H;
                gl_rect_2d(x0(P) - 6, ct, x1(P) + 6, cb, LLColor4(0.f, 0.f, 0.f, 0.25f), true);
                for (const ClipBlock& c : sTL.clips)
                {
                    S32 st = c.start, ln = c.len;
                    if (mClip == c.id)
                    {
                        if (mDrag == D_CMOVE) st += mClipDelta;
                        else if (mDrag == D_CEDGER) ln = llmax(1, ln + mClipDelta);
                        else if (mDrag == D_CEDGEL) { st += mClipDelta; ln = llmax(1, ln - mClipDelta); }
                    }
                    S32 xa = xOf(P, (F32)st), xb = xOf(P, (F32)(st + ln));
                    if (xb < x0(P) - 2 || xa > x1(P) + 2) continue;
                    xa = llmax(xa, x0(P) - 2); xb = llmin(xb, x1(P) + 2);
                    LLColor4 col = c.col; col.mV[VW] = 0.85f;
                    gl_rect_2d(xa, ct - 2, xb, cb + 2, col, true);
                    // the blend at the head: a darker wedge
                    if (c.blend > 0)
                    {
                        const S32 xh = llmin(xb, xOf(P, (F32)(st + c.blend)));
                        gl_triangle_2d(xa, cb + 2, xh, ct - 2, xa, ct - 2, LLColor4(0.f, 0.f, 0.f, 0.35f), true);
                    }
                    // a divider at each repeat of a looped motion
                    if (c.cyc > 0.5f && c.cyc < (F32)ln - 0.5f)
                        for (F32 f = (F32)st + c.cyc; f < (F32)(st + ln) - 0.5f; f += c.cyc)
                        {
                            const S32 xd = xOf(P, f);
                            if (xd > xa && xd < xb) gl_rect_2d(xd, ct - 3, xd + 1, cb + 3, LLColor4(1.f, 1.f, 1.f, 0.35f), true);
                        }
                    gl_rect_2d(xa, ct - 2, xb, cb + 2, c.sel ? C_CYAN : LLColor4(0.f, 0.f, 0.f, 0.6f), false);
                    if (c.sel) gl_rect_2d(xa + 1, ct - 3, xb - 1, cb + 3, C_CYAN, false);
                    std::string lab = c.name + llformat(" (%.0f%%)", c.speed * 100.f);
                    const S32 room = xb - xa - 8;
                    if (room > 18)
                    {
                        while (lab.size() > 3 && font->getWidth(lab) > room) lab = lab.substr(0, lab.size() - 4) + "\xE2\x80\xA6";
                        font->renderUTF8(lab, 0, xa + 4, cb + 7, LLColor4::white, LLFontGL::LEFT, LLFontGL::BASELINE);
                        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                    }
                }
                font->renderUTF8("Clips", 0, P.mLeft + PAD + 2, cb + 7, C_GOLD, LLFontGL::LEFT, LLFontGL::BASELINE);
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            }
            // the box being dragged
            if (mDrag == D_BOX)
            {
                const S32 l = llmin(mDownX, mCurX), r = llmax(mDownX, mCurX), t = llmin(llmax(mDownY, mCurY), R.mBottom - CLIP_H), b = llmax(llmin(mDownY, mCurY), tb);
                gl_rect_2d(l, t, r, b, LLColor4(0.13f, 0.83f, 0.93f, 0.12f), true);
                gl_rect_2d(l, t, r, b, LLColor4(0.13f, 0.83f, 0.93f, 0.8f), false);
            }
            // loop markers
            if (sTL.loop)
            {
                const S32 xi = xOf(P, (F32)sTL.loopIn), xo = xOf(P, (F32)sTL.loopOut);
                if (xi >= x0(P) - 6 && xi <= x1(P) + 6) gl_triangle_2d(xi, R.mTop, xi + 7, R.mTop, xi, R.mTop - 9, C_GREEN, true);
                if (xo >= x0(P) - 6 && xo <= x1(P) + 6) gl_triangle_2d(xo - 7, R.mTop, xo, R.mTop, xo, R.mTop - 9, C_GREEN, true);
            }
            // scrollbar
            const LLRect sr = scrollRect(P), th = thumbRect(P);
            gl_rect_2d(sr, LLColor4(1.f, 1.f, 1.f, 0.06f), true);
            gl_rect_2d(th, (mDrag == D_THUMB) ? LLColor4(0.13f, 0.83f, 0.93f, 0.85f) : LLColor4(0.6f, 0.68f, 0.76f, 0.55f), true);
            // playhead
            const S32 xp = xOf(P, fnow);
            if (xp >= x0(P) - 1 && xp <= x1(P) + 1)
            {
                gl_rect_2d(xp, R.mTop, xp + 1, tb, C_CYAN, true);
                gl_rect_2d(xp - 12, R.mTop, xp + 13, R.mTop - 12, C_CYAN, true);
                font->renderUTF8(llformat("%d", (S32)std::floor(fnow + 0.0001f)), 0, xp, R.mTop - 10, LLColor4(0.02f, 0.06f, 0.08f, 1.f),
                                 LLFontGL::HCENTER, LLFontGL::BASELINE);
            }
            // labels
            for (S32 i = 0; i < rows; ++i)
            {
                const Track& tr = sTL.tracks[i];
                const S32 cy = rowTop(P, i) - ROW_H + 5;
                std::string lab = tr.label;
                if (lab.size() > 15) lab = lab.substr(0, 14) + "\xE2\x80\xA6";
                font->renderUTF8(lab, 0, P.mLeft + PAD + 2, cy, tr.id == sTL.selTrack ? C_CYAN : C_TEXT, LLFontGL::LEFT, LLFontGL::BASELINE);
            }
            // header text: frame / length / speed, the selection, then the hint or the button under the mouse
            const S32 cur = (S32)std::floor(fnow + 0.0001f);
            const S32 nsel = selectedCount();
            const std::string info = (nsel ? llformat("%d selected  \xC2\xB7  ", nsel) : std::string()) +
                                     llformat("%d / %d  \xC2\xB7  %.0f fps  \xC2\xB7  %.2f s", cur, sTL.frames, sTL.fps, sTL.fps > 0.f ? fnow / sTL.fps : 0.f);
            font->renderUTF8(info, 0, P.mRight - PAD, P.mTop - 16, C_TEXT, LLFontGL::RIGHT, LLFontGL::BASELINE);
            const S32 tx = bts.empty() ? P.mLeft + PAD : bts.back().r.mRight + 14;
            std::string line = !mHoverTip.empty() ? mHoverTip : (!sTL.title.empty() ? sTL.title + (sTL.hint.empty() ? "" : "  \xC2\xB7  " + sTL.hint) : sTL.hint);
            const S32 room = (P.mRight - PAD - font->getWidth(info) - 16) - tx;
            if (room > 40 && !line.empty())
            {
                while (line.size() > 4 && font->getWidth(line) > room) line = line.substr(0, line.size() - 4) + "\xE2\x80\xA6";
                font->renderUTF8(line, 0, tx, P.mTop - 16, mHoverTip.empty() ? C_DIM : C_TEXT, LLFontGL::LEFT, LLFontGL::BASELINE);
            }
            LLView::draw();
        }

        // ---------------- mouse ----------------
        void pushScrub(F32 f, bool final)
        {
            const S32 fi = llclamp(ll_round(f), 0, sTL.frames);
            sTL.frame = (F32)fi;
            if (!final && (fi == mLastPushed || mPushTimer.getElapsedTimeF32() < 0.04f)) return;
            mPushTimer.reset();
            mLastPushed = fi;
            push(LLSD().with("type", "studio_tl").with("ev", "scrub").with("frame", fi).with("final", final));
        }
        bool keyAt(const LLRect& P, S32 x, S32 y, std::string& track, S32& frame) const
        {
            const S32 rows = (S32)sTL.tracks.size();
            const S32 i = (P.mTop - HEAD_H - RULER_H - CLIP_H - y) / ROW_H;
            if (y > P.mTop - HEAD_H - RULER_H - CLIP_H || i < 0 || i >= rows) return false;
            const Track& tr = sTL.tracks[i];
            S32 best = 7;
            bool hit = false;
            for (S32 k : tr.keys)
            {
                const S32 d = std::abs(xOf(P, (F32)k) - x);
                if (d <= best) { best = d; frame = k; hit = true; }
            }
            if (hit) track = tr.id;
            return hit;
        }
        std::string trackAt(const LLRect& P, S32 y) const
        {
            const S32 i = (P.mTop - HEAD_H - RULER_H - CLIP_H - y) / ROW_H;
            if (y > P.mTop - HEAD_H - RULER_H - CLIP_H || i < 0 || i >= (S32)sTL.tracks.size()) return std::string();
            return sTL.tracks[i].id;
        }
        void clearSel() { for (Track& t : sTL.tracks) t.sel.clear(); }

        bool handleMouseDown(S32 x, S32 y, MASK mask) override
        {
            if (!tlLive()) return false;
            const LLRect P = panel();
            if (!LLRect(P.mLeft, P.mTop + 4, P.mRight + 4, P.mBottom - 4).pointInRect(x, y)) return false;
            // the size grips
            if (std::abs(y - (sTLL.top ? P.mBottom : P.mTop)) <= 4) { mDrag = D_SIZEH; mDownY = y; mSize0 = sTLL.rowH; gFocusMgr.setMouseCapture(this); return true; }
            if (std::abs(x - P.mRight) <= 4) { mDrag = D_SIZEW; mDownX = x; mPanV0 = sTLL.width; gFocusMgr.setMouseCapture(this); return true; }
            if (!P.pointInRect(x, y)) return false;
            for (const Btn& b : buttons(P))
            {
                if (b.r.pointInRect(x, y))
                {
                    if (b.id == "fit") { sTL.v0 = 0.f; sTL.v1 = (F32)llmax(10, sTL.frames); sTL.zoomed = false; return true; }
                    if (b.id == "dock") { sTLL.top = !sTLL.top; saveTLLayout(); return true; }
                    if (b.id == "auto") sTL.autoKey = !sTL.autoKey;   // at once on screen; the page confirms
                    if (b.id == "eloop") sTL.edgeMode = "loop";
                    if (b.id == "espeed") sTL.edgeMode = "speed";
                    if (b.id == "eresize") sTL.edgeMode = "resize";
                    push(LLSD().with("type", "studio_tl").with("ev", "btn").with("id", b.id));
                    return true;
                }
            }
            mDownX = mCurX = x; mDownY = mCurY = y;
            const LLRect R = ruler(P);
            // scrollbar: drag the thumb, or click beside it to jump a page
            const LLRect sr = scrollRect(P);
            if (LLRect(sr.mLeft, sr.mTop + 2, sr.mRight, sr.mBottom - 2).pointInRect(x, y))
            {
                const LLRect th = thumbRect(P);
                const F32 span = sTL.v1 - sTL.v0;
                if (th.pointInRect(x, y)) { mDrag = D_THUMB; mPanV0 = sTL.v0; gFocusMgr.setMouseCapture(this); }
                else setView(sTL.v0 + (x < th.mLeft ? -span : span) * 0.9f, span);
                return true;
            }
            if (sTL.loop && R.pointInRect(x, y) && y > R.getCenterY())
            {
                if (std::abs(x - xOf(P, (F32)sTL.loopIn)) <= 6) { mDrag = D_LIN; gFocusMgr.setMouseCapture(this); return true; }
                if (std::abs(x - xOf(P, (F32)sTL.loopOut)) <= 6) { mDrag = D_LOUT; gFocusMgr.setMouseCapture(this); return true; }
            }
            // the ruler: scrub
            if (R.pointInRect(x, y) && x >= x0(P) - 6)
            {
                mDrag = D_SCRUB;
                gFocusMgr.setMouseCapture(this);
                pushScrub(frameAt(P, x), false);
                return true;
            }
            // the clip row: pick a clip, drag it, drag one of its ends
            if (y <= clipTop(P) && y > clipTop(P) - CLIP_H && x >= x0(P) - 6 && x <= x1(P) + 6)
            {
                std::string hit, side;
                for (auto it = sTL.clips.rbegin(); it != sTL.clips.rend() && hit.empty(); ++it)
                {
                    const S32 xa = xOf(P, (F32)it->start), xb = xOf(P, (F32)(it->start + it->len));
                    if (std::abs(x - xb) <= 5) { hit = it->id; side = "r"; }
                    else if (std::abs(x - xa) <= 5) { hit = it->id; side = "l"; }
                    else if (x > xa && x < xb) { hit = it->id; side = "m"; }
                }
                for (ClipBlock& c : sTL.clips) c.sel = (c.id == hit);
                push(LLSD().with("type", "studio_tl").with("ev", "clip_sel").with("id", hit));
                if (hit.empty()) { mDrag = D_SCRUB; gFocusMgr.setMouseCapture(this); pushScrub(frameAt(P, x), false); return true; }
                mClip = hit; mClipDelta = 0;
                mDrag = side == "r" ? D_CEDGER : side == "l" ? D_CEDGEL : D_CMOVE;
                gFocusMgr.setMouseCapture(this);
                return true;
            }
            std::string tr;
            S32 kf = 0;
            if (x >= x0(P) - 6 && keyAt(P, x, y, tr, kf))
            {
                // the selection on screen at once (the page keeps the real one)
                const bool add = (mask & (MASK_SHIFT | MASK_CONTROL)) != 0;
                Track* t = nullptr;
                for (Track& tt : sTL.tracks) if (tt.id == tr) t = &tt;
                if (t)
                {
                    if (add) { if (t->sel.count(kf)) t->sel.erase(kf); else t->sel.insert(kf); }
                    else if (!t->sel.count(kf)) { clearSel(); t->sel.insert(kf); }
                }
                sTL.selTrack = tr;
                push(LLSD().with("type", "studio_tl").with("ev", "key").with("track", tr).with("frame", kf).with("add", add));
                mDrag = D_KEYS; mDragFrom = kf; mDragDelta = 0;
                gFocusMgr.setMouseCapture(this);
                return true;
            }
            // empty track space: a click jumps there, a drag draws a selection box
            if (x >= x0(P) - 6 && x <= x1(P) + 6 && y <= R.mBottom - CLIP_H && y > tracksBottom(P))
            {
                const std::string row = trackAt(P, y);
                if (!row.empty() && row != sTL.selTrack)
                {
                    sTL.selTrack = row;
                    push(LLSD().with("type", "studio_tl").with("ev", "track").with("track", row));
                }
                mDrag = D_PRESS;
                mBoxAdd = (mask & (MASK_SHIFT | MASK_CONTROL)) != 0;
                gFocusMgr.setMouseCapture(this);
                return true;
            }
            if (x < x0(P))
            {
                const std::string row = trackAt(P, y);
                if (!row.empty())
                {
                    sTL.selTrack = row;
                    push(LLSD().with("type", "studio_tl").with("ev", "track").with("track", row));
                }
            }
            return true;   // the panel keeps its clicks
        }
        bool handleRightMouseDown(S32 x, S32 y, MASK mask) override
        {
            if (!tlLive()) return false;
            const LLRect P = panel();
            if (!P.pointInRect(x, y)) return false;
            if (y <= clipTop(P) && y > clipTop(P) - CLIP_H)
            {
                for (auto it = sTL.clips.rbegin(); it != sTL.clips.rend(); ++it)
                    if (x >= xOf(P, (F32)it->start) && x <= xOf(P, (F32)(it->start + it->len)))
                    {
                        for (ClipBlock& c : sTL.clips) c.sel = (c.id == it->id);
                        push(LLSD().with("type", "studio_tl").with("ev", "clip_menu").with("id", it->id));
                        return true;
                    }
            }
            mDrag = D_PAN; mDownX = x; mPanV0 = sTL.v0;
            gFocusMgr.setMouseCapture(this);
            return true;
        }
        bool handleRightMouseUp(S32 x, S32 y, MASK mask) override
        {
            if (mDrag != D_PAN) return tlLive() && panel().pointInRect(x, y);
            if (hasMouseCapture()) gFocusMgr.setMouseCapture(nullptr);
            mDrag = D_NONE;
            return true;
        }
        bool handleHover(S32 x, S32 y, MASK mask) override
        {
            if (!tlLive()) return false;
            const LLRect P = panel();
            if (hasMouseCapture())
            {
                mCurX = x; mCurY = y;
                const F32 f = frameAt(P, x);
                const F32 perPx = (sTL.v1 - sTL.v0) / (F32)llmax(1, x1(P) - x0(P));
                if (mDrag == D_SCRUB) pushScrub(f, false);
                else if (mDrag == D_SIZEH)
                {
                    // dragging away from the 3D view's edge makes the rows taller
                    const S32 rows = llmax(1, (S32)sTL.tracks.size()), dy = sTLL.top ? (mDownY - y) : (y - mDownY);
                    sTLL.rowH = llclamp(mSize0 + dy / rows, 12, 40);
                    gViewerWindow->setCursor(UI_CURSOR_SIZENS);
                    return true;
                }
                else if (mDrag == D_SIZEW)
                {
                    const LLRect wv = gViewerWindow->getWorldViewRectScaled();
                    sTLL.width = llclamp(mPanV0 + (F32)(x - mDownX) * 2.f / (F32)llmax(1, wv.getWidth() - 28), 0.3f, 1.f);
                    gViewerWindow->setCursor(UI_CURSOR_SIZEWE);
                    return true;
                }
                else if (mDrag == D_KEYS) mDragDelta = ll_round(f) - mDragFrom;
                else if (mDrag == D_LIN) sTL.loopIn = llclamp(ll_round(f), 0, sTL.loopOut - 1);
                else if (mDrag == D_LOUT) sTL.loopOut = llclamp(ll_round(f), sTL.loopIn + 1, sTL.frames);
                else if (mDrag == D_PRESS && (std::abs(x - mDownX) > 3 || std::abs(y - mDownY) > 3)) mDrag = D_BOX;
                else if (mDrag == D_CMOVE || mDrag == D_CEDGEL || mDrag == D_CEDGER)
                {
                    S32 d = ll_round(frameAt(P, x) - frameAt(P, mDownX));
                    const ClipBlock* c = nullptr;
                    for (const ClipBlock& cc : sTL.clips) if (cc.id == mClip) c = &cc;
                    if (c && !(mask & MASK_SHIFT))
                    {
                        // snap the moving edge to another clip's edges, the playhead or frame 0
                        std::vector<S32> marks = { 0, ll_round(tlFrameNow()) };
                        for (const ClipBlock& o : sTL.clips) if (o.id != mClip) { marks.push_back(o.start); marks.push_back(o.start + o.len); }
                        std::vector<S32> edges;
                        if (mDrag != D_CEDGER) edges.push_back(c->start + d);
                        if (mDrag != D_CEDGEL) edges.push_back(c->start + c->len + d);
                        S32 bestPx = 7, adj = 0;
                        for (S32 e : edges) for (S32 m : marks)
                        {
                            const S32 px = std::abs(xOf(P, (F32)e) - xOf(P, (F32)m));
                            if (px < bestPx) { bestPx = px; adj = m - e; }
                        }
                        d += adj;
                    }
                    if (c && mDrag == D_CMOVE) d = llmax(d, -c->start);
                    if (c && mDrag == D_CEDGEL) d = llclamp(d, -c->start, c->len - 1);
                    if (c && mDrag == D_CEDGER) d = llmax(d, 1 - c->len);
                    mClipDelta = d;
                }
                else if (mDrag == D_PAN) setView(mPanV0 - (F32)(x - mDownX) * perPx, sTL.v1 - sTL.v0);
                else if (mDrag == D_THUMB)
                {
                    const F32 perPxT = total() / (F32)llmax(1, scrollRect(P).getWidth());
                    setView(mPanV0 + (F32)(x - mDownX) * perPxT, sTL.v1 - sTL.v0);
                }
                gViewerWindow->setCursor(mDrag == D_KEYS ? UI_CURSOR_SIZEWE : mDrag == D_PAN || mDrag == D_THUMB ? UI_CURSOR_SIZEWE : UI_CURSOR_ARROW);
                return true;
            }
            mHoverTip.clear();
            if (std::abs(y - (sTLL.top ? P.mBottom : P.mTop)) <= 4 && x >= P.mLeft && x <= P.mRight) { mHoverTip = "Drag to make the timeline taller or shorter"; gViewerWindow->setCursor(UI_CURSOR_SIZENS); return true; }
            if (std::abs(x - P.mRight) <= 4 && y <= P.mTop && y >= P.mBottom) { mHoverTip = "Drag to make the timeline wider or narrower"; gViewerWindow->setCursor(UI_CURSOR_SIZEWE); return true; }
            if (!P.pointInRect(x, y)) return false;
            for (const Btn& b : buttons(P)) if (b.r.pointInRect(x, y)) { mHoverTip = b.tip; gViewerWindow->setCursor(UI_CURSOR_HAND); return true; }
            std::string tr;
            S32 kf;
            if (keyAt(P, x, y, tr, kf)) { mHoverTip = llformat("Key at frame %d: drag moves the selection, Shift+click adds", kf); gViewerWindow->setCursor(UI_CURSOR_HAND); }
            else if (y <= ruler(P).mBottom - CLIP_H && y > tracksBottom(P)) mHoverTip = "Drag a box to select keys (Shift adds) \xC2\xB7 wheel scrolls, Ctrl+wheel zooms, right-drag pans";
            if (!keyAt(P, x, y, tr, kf)) gViewerWindow->setCursor(UI_CURSOR_ARROW);
            return true;
        }
        bool handleMouseUp(S32 x, S32 y, MASK mask) override
        {
            if (!hasMouseCapture()) return tlLive() && panel().pointInRect(x, y);
            gFocusMgr.setMouseCapture(nullptr);
            const LLRect P = panel();
            if (mDrag == D_SIZEH || mDrag == D_SIZEW) { mDrag = D_NONE; saveTLLayout(); return true; }
            if (mDrag == D_SCRUB) pushScrub(frameAt(P, x), true);
            else if (mDrag == D_KEYS && mDragDelta != 0)
            {
                // the dragged keys stay on screen where they were dropped
                for (Track& t : sTL.tracks)
                {
                    std::set<S32> moved;
                    for (S32 k : t.sel) moved.insert(llmax(0, k + mDragDelta));
                    for (S32& k : t.keys) if (t.sel.count(k)) k = llmax(0, k + mDragDelta);
                    t.sel = moved;
                }
                push(LLSD().with("type", "studio_tl").with("ev", "move").with("delta", mDragDelta));
            }
            else if (mDrag == D_LIN || mDrag == D_LOUT)
                push(LLSD().with("type", "studio_tl").with("ev", "loop").with("in", sTL.loopIn).with("out", sTL.loopOut));
            else if ((mDrag == D_CMOVE || mDrag == D_CEDGEL || mDrag == D_CEDGER) && mClipDelta != 0)
            {
                // where it was dropped, at once on screen (the page sends the real result back)
                for (ClipBlock& c : sTL.clips)
                {
                    if (c.id != mClip) continue;
                    if (mDrag == D_CMOVE) { c.start += mClipDelta; push(LLSD().with("type", "studio_tl").with("ev", "clip_move").with("id", c.id).with("start", c.start)); }
                    else if (mDrag == D_CEDGER) { c.len = llmax(1, c.len + mClipDelta); push(LLSD().with("type", "studio_tl").with("ev", "clip_edge").with("id", c.id).with("side", "r").with("len", c.len).with("mode", sTL.edgeMode)); }
                    else { c.start += mClipDelta; c.len = llmax(1, c.len - mClipDelta); push(LLSD().with("type", "studio_tl").with("ev", "clip_edge").with("id", c.id).with("side", "l").with("start", c.start).with("len", c.len).with("mode", sTL.edgeMode)); }
                }
                mClipDelta = 0;
            }
            else if (mDrag == D_PRESS)
            {
                // a click on empty space: the playhead there, nothing selected
                clearSel();
                push(LLSD().with("type", "studio_tl").with("ev", "clear"));
                pushScrub(frameAt(P, x), true);
            }
            else if (mDrag == D_BOX)
            {
                // every key inside the box, in every row it covers
                const LLRect R = ruler(P);
                const F32 fa = frameAt(P, llmin(mDownX, x)), fb = frameAt(P, llmax(mDownX, x));
                const S32 yt = llmin(llmax(mDownY, y), R.mBottom - CLIP_H), yb = llmax(llmin(mDownY, y), tracksBottom(P));
                if (!mBoxAdd) clearSel();
                LLSD items = LLSD::emptyArray();
                for (S32 i = 0; i < (S32)sTL.tracks.size(); ++i)
                {
                    const S32 rt = rowTop(P, i), rb = rt - ROW_H;
                    if (rb >= yt || rt <= yb) continue;
                    Track& t = sTL.tracks[i];
                    for (S32 k : t.keys)
                    {
                        if (k < fa - 0.5f || k > fb + 0.5f) continue;
                        t.sel.insert(k);
                        items.append(LLSD().with(0, t.id).with(1, k));
                    }
                }
                push(LLSD().with("type", "studio_tl").with("ev", "box").with("items", items).with("add", mBoxAdd));
            }
            mDrag = D_NONE;
            mDragDelta = 0;
            return true;
        }
        bool handleScrollWheel(S32 x, S32 y, S32 clicks) override
        {
            if (!tlLive()) return false;
            const LLRect P = panel();
            if (!P.pointInRect(x, y)) return false;
            const F32 span = sTL.v1 - sTL.v0;
            if (gKeyboard && (gKeyboard->currentMask(false) & MASK_CONTROL))
            {
                // Ctrl + wheel: zoom around the mouse
                const F32 at = llclamp(frameAt(P, x), sTL.v0, sTL.v1);
                const F32 k = clicks > 0 ? 1.25f : 0.8f, ns = span * k, t = (at - sTL.v0) / llmax(1.f, span);
                setView(at - ns * t, ns);
            }
            else setView(sTL.v0 + (F32)clicks * span * 0.12f, span);   // wheel: scroll along the clip
            return true;
        }
        bool handleDoubleClick(S32 x, S32 y, MASK mask) override
        {
            if (!tlLive()) return false;
            const LLRect P = panel();
            if (!P.pointInRect(x, y)) return false;
            if (y <= clipTop(P) && y > clipTop(P) - CLIP_H)
            {
                for (const ClipBlock& c : sTL.clips)
                    if (x > xOf(P, (F32)c.start) && x < xOf(P, (F32)(c.start + c.len))) push(LLSD().with("type", "studio_tl").with("ev", "clip_dbl").with("id", c.id));
                return true;
            }
            if (x >= x0(P) && y < ruler(P).mBottom - CLIP_H && y > tracksBottom(P))
                push(LLSD().with("type", "studio_tl").with("ev", "dbl").with("track", trackAt(P, y)).with("frame", llclamp(ll_round(frameAt(P, x)), 0, sTL.frames)));
            return true;
        }
    };
    MGStudioTimeline* sTLView = nullptr;

    class MGStudioBar : public LLView
    {
    public:
        MGStudioBar(const LLView::Params& p) : LLView(p) {}
        S32 mHover = -1;
        static bool live() { return !sBar.empty() && tlLive(); }
        LLRect itemRect(S32 i) const
        {
            const LLRect r = gViewerWindow->getWorldViewRectScaled();
            const S32 gap = 4, sep = 10;
            S32 total = 0;
            for (size_t k = 0; k < sBar.size(); ++k) total += BAR_BW + gap + (k > 0 && sBar[k].sep ? sep : 0);
            S32 x = r.getCenterX() - total / 2;
            for (S32 k = 1; k <= i && k < (S32)sBar.size(); ++k) x += BAR_BW + gap + (sBar[k].sep ? sep : 0);
            const S32 top = r.mTop - BAR_TOP;
            return LLRect(x, top, x + BAR_BW, top - BAR_BW);
        }
        S32 itemAt(S32 x, S32 y) const
        {
            if (!live()) return -1;
            for (S32 i = 0; i < (S32)sBar.size(); ++i) if (itemRect(i).pointInRect(x, y)) return i;
            return -1;
        }
        bool handleMouseDown(S32 x, S32 y, MASK mask) override
        {
            const S32 i = itemAt(x, y);
            if (i < 0) return false;
            if (sBar[i].id == "tldock") { sTLL.top = !sTLL.top; saveTLLayout(); return true; }
            push(LLSD().with("type", "studio_bar_click").with("id", sBar[i].id));
            return true;
        }
        bool handleMouseUp(S32 x, S32 y, MASK mask) override { return itemAt(x, y) >= 0; }
        bool handleHover(S32 x, S32 y, MASK mask) override
        {
            mHover = itemAt(x, y);
            if (mHover < 0) return false;
            gViewerWindow->setCursor(UI_CURSOR_HAND);
            return true;
        }
        static void glyph(const std::string& id, S32 cx, S32 cy, const LLColor4& c)
        {
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gGL.color4fv(c.mV);
            if (id == "fk") { gl_circle_2d((F32)cx, (F32)cy, 8.f, 24, false); gl_circle_2d((F32)cx, (F32)cy, 2.f, 8, true); }
            else if (id == "ik") { gl_line_2d(cx - 8, cy, cx + 8, cy, c); gl_line_2d(cx, cy - 8, cx, cy + 8, c); gl_triangle_2d(cx + 8, cy + 3, cx + 8, cy - 3, cx + 11, cy, c, true); gl_triangle_2d(cx - 3, cy + 8, cx + 3, cy + 8, cx, cy + 11, c, true); }
            else if (id == "key") { MGStudioTimeline::icon("key", LLRect(cx - 11, cy + 11, cx + 11, cy - 11), c); }
            else if (id == "auto" || id == "play") { MGStudioTimeline::icon(id, LLRect(cx - 11, cy + 11, cx + 11, cy - 11), c); }
            else if (id == "undo" || id == "redo")
            {
                const S32 d = id == "undo" ? -1 : 1;
                gl_line_2d(cx - 6 * d, cy + 2, cx + 5 * d, cy + 2, c); gl_line_2d(cx + 5 * d, cy + 2, cx + 7 * d, cy - 3, c); gl_line_2d(cx + 7 * d, cy - 3, cx + 2 * d, cy - 7, c);
                gl_triangle_2d(cx - 6 * d, cy + 6, cx - 6 * d, cy - 2, cx - 10 * d, cy + 2, c, true);
            }
            else if (id == "trail") { gl_line_2d(cx - 9, cy - 6, cx - 3, cy + 2, c); gl_line_2d(cx - 3, cy + 2, cx + 3, cy - 1, c); gl_line_2d(cx + 3, cy - 1, cx + 9, cy + 7, c); gl_rect_2d(cx - 2, cy + 4, cx + 2, cy, c, true); }
            else if (id == "balance") { gl_line_2d(cx, cy + 8, cx, cy - 6, c); gl_line_2d(cx - 8, cy + 4, cx + 8, cy + 4, c); gl_triangle_2d(cx - 6, cy - 8, cx + 6, cy - 8, cx, cy - 4, c, true); }
            else if (id == "path") { gl_rect_2d(cx - 9, cy - 4, cx - 5, cy - 8, c, true); gl_rect_2d(cx + 5, cy + 8, cx + 9, cy + 4, c, true); gl_line_2d(cx - 7, cy - 6, cx, cy + 1, c); gl_line_2d(cx, cy + 1, cx + 7, cy + 6, c); }
            else if (id == "import") { gl_line_2d(cx, cy + 8, cx, cy - 3, c); gl_triangle_2d(cx - 4, cy - 1, cx + 4, cy - 1, cx, cy - 6, c, true); gl_rect_2d(cx - 8, cy - 6, cx + 8, cy - 9, c, true); }
            else if (id == "save") { gl_rect_2d(cx - 8, cy + 8, cx + 8, cy - 8, c, false); gl_rect_2d(cx - 5, cy + 8, cx + 4, cy + 3, c, true); gl_rect_2d(cx - 5, cy - 2, cx + 5, cy - 8, c, false); }
            else if (id == "camera") { gl_rect_2d(cx - 9, cy + 5, cx + 9, cy - 6, c, false); gl_circle_2d((F32)cx, (F32)cy, 3.5f, 12, false); gl_rect_2d(cx - 4, cy + 8, cx + 2, cy + 5, c, true); }
            else if (id == "tldock") { gl_rect_2d(cx - 9, cy + 7, cx + 9, cy - 7, c, false); if (sTLL.top) gl_rect_2d(cx - 7, cy + 5, cx + 7, cy + 2, c, true); else gl_rect_2d(cx - 7, cy - 2, cx + 7, cy - 5, c, true); }
            else if (id == "home") { gl_triangle_2d(cx - 9, cy + 1, cx + 9, cy + 1, cx, cy + 9, c, false); gl_rect_2d(cx - 6, cy + 1, cx + 6, cy - 8, c, false); }
            else { const LLFontGL* f = LLFontGL::getFontSansSerifSmall(); f->renderUTF8(id.substr(0, 2), 0, cx, cy - 4, c, LLFontGL::HCENTER, LLFontGL::BASELINE); }
        }
        void draw() override
        {
            if (!live()) return;
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            const LLRect a = itemRect(0), b = itemRect((S32)sBar.size() - 1);
            gl_rect_2d(a.mLeft - 6, a.mTop + 6, b.mRight + 6, b.mBottom - 6, LLColor4(0.03f, 0.06f, 0.09f, 0.86f), true);
            for (S32 i = 0; i < (S32)sBar.size(); ++i)
            {
                const LLRect r = itemRect(i);
                const bool on = sBar[i].on, hov = mHover == i;
                if (on) gl_rect_2d(r, LLColor4(0.07f, 0.19f, 0.24f, 1.f), true);
                else if (hov) gl_rect_2d(r, LLColor4(1.f, 1.f, 1.f, 0.08f), true);
                glyph(sBar[i].id, r.getCenterX(), r.getCenterY(), on ? LLColor4(0.13f, 0.78f, 0.9f, 1.f) : LLColor4(0.72f, 0.78f, 0.86f, 1.f));
                if (sBar[i].sep && i > 0) gl_rect_2d(r.mLeft - 8, r.mTop - 4, r.mLeft - 7, r.mBottom + 4, LLColor4(1.f, 1.f, 1.f, 0.15f), true);
            }
            if (mHover >= 0 && mHover < (S32)sBar.size())
            {
                const LLRect r = itemRect(mHover);
                const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
                font->renderUTF8(sBar[mHover].tip, 0, (F32)r.mLeft, (F32)(a.mBottom - 20), LLColor4::white, LLFontGL::LEFT, LLFontGL::BASELINE, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
            }
            LLView::draw();
        }
    };
    MGStudioBar* sBarView = nullptr;
    void showBar(bool on)
    {
        if (on && !sBarView && gViewerWindow && gViewerWindow->getRootView())
        {
            LLView::Params p;
            p.name = "mg_studio_bar";
            p.rect = gViewerWindow->getRootView()->getLocalRect();
            p.mouse_opaque = false;
            p.follows.flags = FOLLOWS_ALL;
            sBarView = new MGStudioBar(p);
            gViewerWindow->getRootView()->addChild(sBarView);
        }
        if (sBarView) sBarView->setVisible(on);
    }

    void showTimeline(bool on)
    {
        if (on && !sTLView && gViewerWindow && gViewerWindow->getRootView())
        {
            LLView::Params p;
            p.name = "mg_studio_timeline";
            p.rect = gViewerWindow->getRootView()->getLocalRect();
            p.mouse_opaque = false;
            p.follows.flags = FOLLOWS_ALL;
            sTLView = new MGStudioTimeline(p);
            gViewerWindow->getRootView()->addChild(sTLView);
        }
        if (sTLView) sTLView->setVisible(on);
        showBar(on);
        if (!on) sTL.any = false;
    }

    // ------------------------------------------------------------------
    // Motion along waypoints: the path is kept in the avatar's own frame where it stands (x
    // forward, y left, z up, the floor under its feet at z = 0), the frame an animation's pelvis
    // offsets live in. Waypoints are put on real surfaces (land, prims) with the mouse; the
    // page computes the motion and sends back the track to draw (like iClone's trajectory).
    // ------------------------------------------------------------------
    struct PathView
    {
        bool on = false, edit = false;
        std::vector<LLVector3> pts;
        std::vector<S32> frames;              // the frame each waypoint is reached at
        std::vector<LLVector4> track;         // x, y, z, gait code (0 walk, 1 run, 2 jump, 3 fly, 4 idle, 5 custom)
        std::vector<S32> gaits;               // per leg (waypoint i-1 -> i), the same codes
        S32 sel = -1, hover = -1, drag = -1;
        S32 downY = 0;
        F32 downZ = 0.f;
        S32 gizAxis = -1, gizHover = -1;   // the gizmo axis dragged / under the mouse (0 fwd, 1 left, 2 up)
        F32 gizT0 = 0.f;
        LLVector3 gizStart;
    } sPath;
    const F32 GIZ_LEN = 0.7f;

    bool pathFrame(LLVector3& o, LLQuaternion& r)
    {
        if (!isAgentAvatarValid()) return false;
        // the skeleton's root, not the avatar object: an animation's pelvis offsets live in the
        // root's frame, and for my own avatar the root's facing lags behind and may deviate from
        // the avatar's heading while standing (llvoavatar.cpp updateOrientation)
        LLJoint* root = gAgentAvatarp->getRootJoint();
        o = root ? root->getWorldPosition() : gAgentAvatarp->getPositionAgent();
        o.mV[VZ] -= gAgentAvatarp->getPelvisToFoot();
        LLVector3 fwd = LLVector3::x_axis * (root ? root->getWorldRotation() : gAgentAvatarp->getRotation());
        fwd.mV[VZ] = 0.f;
        if (fwd.normVec() < 1e-4f) fwd = LLVector3::x_axis;
        r = LLQuaternion(atan2f(fwd.mV[VY], fwd.mV[VX]), LLVector3::z_axis);
        return true;
    }
    LLVector3 toAgent(const LLVector3& l) { LLVector3 o; LLQuaternion r; return pathFrame(o, r) ? o + l * r : l; }
    LLVector3 toLocal(const LLVector3& a) { LLVector3 o; LLQuaternion r; return pathFrame(o, r) ? (a - o) * ~r : a; }

    bool notMine(LLViewerObject* obj)
    {
        if (!obj) return true;
        if (obj->isAvatar() || obj->isAttachment()) return false;
        LLViewerObject* root = obj->getRootEdit();
        return !(root && (root->isAvatar() || root->isAttachment()));
    }

    // the surface under the mouse (prims or land, never an avatar), in the path frame
    bool surfaceAt(S32 x, S32 y, LLVector3& local)
    {
        const LLPickInfo pick = gViewerWindow->pickImmediate(x, y, false, false);
        LLVector3d global;
        bool ok = false;
        if (pick.mPickType == LLPickInfo::PICK_LAND) { global = pick.mPosGlobal; ok = true; }
        else if (pick.mPickType == LLPickInfo::PICK_OBJECT && notMine(pick.getObject())) { global = pick.mPosGlobal; ok = true; }
        if (!ok) ok = gViewerWindow->mousePointOnLandGlobal(x, y, &global, true);
        if (!ok) return false;
        local = toLocal(gAgent.getPosAgentFromGlobal(global));
        return local.isFinite();
    }

    // the floor height (path frame) at a point: a ray down from above it, prims then land
    F32 groundAt(F32 lx, F32 ly, F32 zhint)
    {
        const F32 top = zhint + 2.5f;
        LLVector3 from = toAgent(LLVector3(lx, ly, top)), to = toAgent(LLVector3(lx, ly, zhint - 8.f));
        F32 best = -1000.f;
        for (S32 tries = 0; tries < 6; ++tries)
        {
            LLVector4a a, b, hit;
            a.load3(from.mV); b.load3(to.mV);
            S32 face = -1;
            LLViewerObject* obj = gPipeline.lineSegmentIntersectInWorld(a, b, false, false, false, false, &face, nullptr, nullptr, &hit);
            if (!obj) break;
            const LLVector3 h(hit.getF32ptr());
            if (notMine(obj)) { best = toLocal(h).mV[VZ]; break; }
            from = h - LLVector3(0.f, 0.f, 0.01f);   // through my own avatar
        }
        const F32 land = toLocal(LLVector3(toAgent(LLVector3(lx, ly, 0.f)).mV[VX], toAgent(LLVector3(lx, ly, 0.f)).mV[VY],
                                           LLWorld::getInstance()->resolveLandHeightAgent(toAgent(LLVector3(lx, ly, 0.f))))).mV[VZ];
        if (best <= -999.f || land > best) best = land;
        return llclamp(best, zhint - 8.f, zhint + 2.5f);
    }

    // the gizmo axis under the mouse (its line or its knob), -1 if none
    S32 gizmoAxisAt(S32 x, S32 y)
    {
        if (!sPath.edit || sPath.sel < 0 || sPath.sel >= (S32)sPath.pts.size()) return -1;
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        const LLVector3 p = sPath.pts[sPath.sel];
        LLCoordGL c0;
        if (!cam->projectPosAgentToScreen(toAgent(p), c0, false)) return -1;
        S32 best = -1;
        F32 bd = 7.f * 7.f;
        for (S32 a = 0; a < 3; ++a)
        {
            LLVector3 e; e.mV[a] = GIZ_LEN;
            LLCoordGL c1;
            if (!cam->projectPosAgentToScreen(toAgent(p + e), c1, false)) continue;
            const F32 ax = (F32)c0.mX, ay = (F32)c0.mY, dx = (F32)(c1.mX - c0.mX), dy = (F32)(c1.mY - c0.mY), L2 = dx * dx + dy * dy;
            if (L2 < 9.f) continue;
            const F32 u = llclamp(((x - ax) * dx + (y - ay) * dy) / L2, 0.f, 1.f);
            if (u < 0.25f) continue;   // near the square: the square itself (free drag)
            const F32 px = ax + dx * u - x, py = ay + dy * u - y, d = px * px + py * py;
            if (d < bd) { bd = d; best = a; }
        }
        return best;
    }
    // where the mouse ray passes closest to a gizmo axis: the parameter along the axis (metres)
    bool gizmoParam(S32 x, S32 y, S32 axis, const LLVector3& start, F32& t)
    {
        LLVector3 o; LLQuaternion r;
        if (!pathFrame(o, r)) return false;
        LLVector3 a; a.mV[axis] = 1.f;
        const LLVector3 A = a * r, P0 = toAgent(start);
        const LLVector3 C = LLViewerCamera::getInstance()->getOrigin(), D = gViewerWindow->mouseDirectionGlobal(x, y);
        const LLVector3 w = P0 - C;
        const F32 b = A * D, d = A * w, e = D * w, den = 1.f - b * b;
        if (std::fabs(den) < 1e-4f) return false;   // looking along the axis
        t = (b * e - d) / den;
        return true;
    }
    S32 waypointAt(S32 x, S32 y)
    {
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        S32 best = -1;
        F32 bd = 11.f * 11.f;
        for (S32 i = 0; i < (S32)sPath.pts.size(); ++i)
        {
            LLCoordGL c;
            if (!cam->projectPosAgentToScreen(toAgent(sPath.pts[i]), c, false)) continue;
            const F32 dx = (F32)(c.mX - x), dy = (F32)(c.mY - y), d = dx * dx + dy * dy;
            if (d < bd) { bd = d; best = i; }
        }
        return best;
    }

    void drawPath()
    {
        if (!sPath.on || !isAgentAvatarValid()) return;
        static const LLColor4 GAIT[] = { LLColor4(0.31f, 0.82f, 0.91f, 0.95f), LLColor4(1.f, 0.7f, 0.28f, 0.95f), LLColor4(1.f, 0.88f, 0.4f, 0.95f),
                                         LLColor4(0.71f, 0.55f, 1.f, 0.95f), LLColor4(0.6f, 0.6f, 0.6f, 0.8f), LLColor4(0.4f, 0.95f, 0.6f, 0.95f) };
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        // the legs between waypoints (straight, dimmer): click one to put a waypoint on it
        for (S32 i = 1; i < (S32)sPath.pts.size(); ++i)
        {
            LLCoordGL a, b;
            if (!cam->projectPosAgentToScreen(toAgent(sPath.pts[i - 1]), a, false) || !cam->projectPosAgentToScreen(toAgent(sPath.pts[i]), b, false)) continue;
            LLColor4 c = GAIT[llclamp(i - 1 < (S32)sPath.gaits.size() ? sPath.gaits[i - 1] : 0, 0, 5)];
            c.mV[VW] = 0.45f;
            gl_line_2d(a.mX, a.mY, b.mX, b.mY, c);
            gl_line_2d(a.mX, a.mY + 1, b.mX, b.mY + 1, c);
        }
        // the track
        LLCoordGL prev;
        bool havePrev = false;
        for (size_t i = 0; i < sPath.track.size(); ++i)
        {
            const LLVector4& t = sPath.track[i];
            LLCoordGL c;
            const bool ok = cam->projectPosAgentToScreen(toAgent(LLVector3(t.mV[0], t.mV[1], t.mV[2] + 0.03f)), c, false);
            if (ok && havePrev) gl_line_2d(prev.mX, prev.mY, c.mX, c.mY, GAIT[llclamp((S32)t.mV[3], 0, 5)]);
            if (ok && i % 6 == 0) { gGL.color4fv(GAIT[llclamp((S32)t.mV[3], 0, 5)].mV); gl_circle_2d((F32)c.mX, (F32)c.mY, 1.6f, 6, true); }
            prev = c; havePrev = ok;
        }
        // the move gizmo on the selected waypoint (edit mode): forward red, left green, up blue
        if (sPath.edit && sPath.sel >= 0 && sPath.sel < (S32)sPath.pts.size())
        {
            static const LLColor4 AX[] = { LLColor4(1.f, 0.35f, 0.35f, 1.f), LLColor4(0.4f, 0.95f, 0.45f, 1.f), LLColor4(0.4f, 0.65f, 1.f, 1.f) };
            const LLVector3 p = sPath.pts[sPath.sel];
            LLCoordGL c0;
            if (cam->projectPosAgentToScreen(toAgent(p), c0, false))
                for (S32 a = 0; a < 3; ++a)
                {
                    LLVector3 e; e.mV[a] = GIZ_LEN;
                    LLCoordGL c1;
                    if (!cam->projectPosAgentToScreen(toAgent(p + e), c1, false)) continue;
                    const bool hi = sPath.gizAxis == a || (sPath.gizAxis < 0 && sPath.gizHover == a);
                    const LLColor4 col = hi ? LLColor4::white : AX[a];
                    gl_line_2d(c0.mX, c0.mY, c1.mX, c1.mY, col);
                    gl_line_2d(c0.mX + 1, c0.mY, c1.mX + 1, c1.mY, col);
                    gGL.color4fv(col.mV);
                    gl_circle_2d((F32)c1.mX, (F32)c1.mY, hi ? 6.f : 5.f, 12, true);
                }
        }
        // waypoints: squares, the number and the frame reached
        const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
        for (S32 i = 0; i < (S32)sPath.pts.size(); ++i)
        {
            LLCoordGL c;
            if (!cam->projectPosAgentToScreen(toAgent(sPath.pts[i]), c, false)) continue;
            const bool sel = i == sPath.sel, hov = i == sPath.hover || i == sPath.drag;
            const S32 r = sel || hov ? 6 : 5;
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gl_rect_2d(c.mX - r - 1, c.mY + r + 1, c.mX + r + 1, c.mY - r - 1, LLColor4(0.f, 0.f, 0.f, 0.7f), true);
            gl_rect_2d(c.mX - r, c.mY + r, c.mX + r, c.mY - r, i == 0 ? C_GREEN : sel ? LLColor4::white : C_CYAN, true);
            std::string lab = i == 0 ? std::string("Start") : llformat("%d", i);
            if (i < (S32)sPath.frames.size() && i > 0) lab += llformat("  f%d", sPath.frames[i]);
            font->renderUTF8(lab, 0, c.mX + 9, c.mY + 4, LLColor4::white, LLFontGL::LEFT, LLFontGL::BASELINE, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
        }
    }

    // ------------------------------------------------------------------
    // motion trails: the path of chosen joints over time, computed by the page (frames around
    // the playhead, in the path frame), drawn here: the part already played darker, the part to
    // come lighter, a dot every N frames, squares on keys (with their frame number)
    // ------------------------------------------------------------------
    struct TrailPt { LLVector3 p; S32 f = 0; bool key = false; bool point = false; };   // key = a layer key, point = an edit point on the motion
    struct Trail { std::string joint; LLColor4 col; std::vector<TrailPt> pts; };
    std::vector<Trail> sTrails;
    struct TrailDrag { bool on = false, moved = false; std::string joint; S32 frame = 0; S32 x0 = 0, y0 = 0; LLVector3 start, cur; } sTrailDrag;
    S32 sTrailCur = 0, sTrailEvery = 1, sTrailLabels = 1;

    void drawTrails()
    {
        if (sTrails.empty() || !isAgentAvatarValid()) return;
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
        for (const Trail& t : sTrails)
        {
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            LLColor4 past = t.col * 0.55f; past.mV[VW] = 0.9f;
            LLCoordGL prev;
            bool havePrev = false;
            std::vector<std::pair<LLCoordGL, const TrailPt*>> keys;
            for (const TrailPt& q : t.pts)
            {
                LLCoordGL c;
                const bool ok = cam->projectPosAgentToScreen(toAgent(q.p), c, false);
                const LLColor4& col = q.f < sTrailCur ? past : t.col;
                if (ok && havePrev) gl_line_2d(prev.mX, prev.mY, c.mX, c.mY, col);
                if (ok && sTrailEvery > 0 && q.f % sTrailEvery == 0 && !q.key) { gGL.color4fv(col.mV); gl_circle_2d((F32)c.mX, (F32)c.mY, 1.7f, 6, true); }
                if (ok && q.key) keys.push_back({ c, &q });
                prev = c; havePrev = ok;
            }
            for (const auto& k : keys)
            {
                const S32 x = k.first.mX, y = k.first.mY;
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                gl_rect_2d(x - 4, y + 4, x + 4, y - 4, LLColor4(0.f, 0.f, 0.f, 0.7f), true);
                gl_rect_2d(x - 3, y + 3, x + 3, y - 3, k.second->f == sTrailCur ? LLColor4::white : t.col, !k.second->point);   // edit points hollow
                if (sTrailLabels > 0) font->renderUTF8(llformat("%d", k.second->f), 0, x + 6, y + 3, LLColor4::white, LLFontGL::LEFT, LLFontGL::BASELINE, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
            }
            if (sTrailDrag.on && sTrailDrag.moved && sTrailDrag.joint == t.joint)
            {
                LLCoordGL a, b;
                if (cam->projectPosAgentToScreen(sTrailDrag.start, a, false) && cam->projectPosAgentToScreen(sTrailDrag.cur, b, false))
                {
                    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                    gl_line_2d(a.mX, a.mY, b.mX, b.mY, LLColor4(1.f, 1.f, 1.f, 0.7f));
                    gl_rect_2d(b.mX - 5, b.mY + 5, b.mX + 5, b.mY - 5, LLColor4::white, true);
                    font->renderUTF8(llformat("%d", sTrailDrag.frame), 0, b.mX + 8, b.mY + 4, LLColor4::white, LLFontGL::LEFT, LLFontGL::BASELINE, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
                }
            }
            if (sTrailLabels > 1)
                for (const TrailPt& q : t.pts)
                {
                    if (q.key || (sTrailEvery > 0 && q.f % sTrailEvery)) continue;
                    LLCoordGL c;
                    if (cam->projectPosAgentToScreen(toAgent(q.p), c, false))
                        font->renderUTF8(llformat("%d", q.f), 0, c.mX + 4, c.mY + 2, LLColor4(0.8f, 0.86f, 0.92f, 0.9f), LLFontGL::LEFT, LLFontGL::BASELINE, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
                }
        }
    }
    // the key square of a trail under the mouse
    bool trailKeyAt(S32 x, S32 y, std::string& joint, S32& frame)
    {
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        F32 bd = 8.f * 8.f;
        bool hit = false;
        for (const Trail& t : sTrails)
            for (const TrailPt& q : t.pts)
            {
                if (!q.key) continue;
                LLCoordGL c;
                if (!cam->projectPosAgentToScreen(toAgent(q.p), c, false)) continue;
                const F32 dx = (F32)(c.mX - x), dy = (F32)(c.mY - y), d = dx * dx + dy * dy;
                if (d < bd) { bd = d; joint = t.joint; frame = q.f; hit = true; }
            }
        return hit;
    }

    // ------------------------------------------------------------------
    // recording a route: the resident walks, runs or flies with the usual keys; the root's place
    // and heading are sampled 10 times a second in the frame where the take started
    // ------------------------------------------------------------------
    struct Recording { bool on = false; LLVector3 o; LLQuaternion r; LLFrameTimer timer; F32 next = 0.f; std::vector<F32> samples; std::vector<F32> marks; bool keyWas[5] = { false, false, false, false, false }; } sRec;

    // ------------------------------------------------------------------
    // IK on hands and feet (our own solver: a straight arm or leg gets a natural bend hint, knees
    // forward, elbows back, so a sideways drag works at once; the hand / foot keeps its turn) and
    // locks (iClone): locked hands and feet stay where they are while anything else moves
    // ------------------------------------------------------------------
    std::set<std::string> sLocks;
    struct LockRef { std::string end; LLVector3 pos; LLQuaternion rot; };
    std::vector<LockRef> sLockRefs;
    S32 sRelock = 0;                     // frames left keeping the locks after a page-driven change
    struct LookAt { bool on = false, drag = false, hover = false; LLVector3 p; LLFrameTimer push; } sLook;   // agent frame
    struct IKDrag { bool on = false; std::string joint; S32 axis = -1; F32 t0 = 0.f; LLVector3 start, axisDir, planeN; LLQuaternion rot; S32 hover = -1; LLSD pelR; LLVector3 pelP; } sIK;
    const F32 IK_LEN = 0.25f;

    bool limbOf(const std::string& end, std::string& a, std::string& b, bool& leg)
    {
        const bool L = end.find("Left") != std::string::npos;
        if (end == "mWristLeft" || end == "mWristRight") { a = L ? "mShoulderLeft" : "mShoulderRight"; b = L ? "mElbowLeft" : "mElbowRight"; leg = false; return true; }
        if (end == "mAnkleLeft" || end == "mAnkleRight") { a = L ? "mHipLeft" : "mHipRight"; b = L ? "mKneeLeft" : "mKneeRight"; leg = true; return true; }
        return false;
    }
    // the bone that turns when a joint is dragged (the joint itself keeps its own turn relative to it)
    std::string aimParent(const std::string& j)
    {
        static const std::map<std::string, std::string> P = {
            { "mChest", "mTorso" }, { "mTorso", "mPelvis" }, { "mNeck", "mChest" }, { "mHead", "mNeck" },
            { "mCollarLeft", "mChest" }, { "mCollarRight", "mChest" }, { "mShoulderLeft", "mCollarLeft" }, { "mShoulderRight", "mCollarRight" },
            { "mElbowLeft", "mShoulderLeft" }, { "mElbowRight", "mShoulderRight" }, { "mKneeLeft", "mHipLeft" }, { "mKneeRight", "mHipRight" } };
        auto it = P.find(j);
        return it == P.end() ? std::string() : it->second;
    }
    bool ikJoint(const std::string& j) { std::string a, b; bool leg; return j == "mPelvis" || limbOf(j, a, b, leg) || !aimParent(j).empty(); }
    // the aim: the parent bone turned so `j` moves toward T (agent frame)
    bool solveAim(const std::string& j, const LLVector3& T, LLSD& pose)
    {
        const std::string pn = aimParent(j);
        if (pn.empty() || !isAgentAvatarValid()) return false;
        LLJoint *jp = gAgentAvatarp->getJoint(pn), *jj = gAgentAvatarp->getJoint(j);
        if (!jp || !jj || !jp->getParent()) return false;
        const LLVector3 P = jp->getWorldPosition(), J = jj->getWorldPosition();
        if ((J - P).length() < 1e-4f || (T - P).length() < 1e-4f) return false;
        LLQuaternion turn; turn.shortestArc(J - P, T - P);
        LLQuaternion q = (jp->getWorldRotation() * turn) * ~jp->getParent()->getWorldRotation();
        q.normalize();
        pose[pn] = LLSD().with("r", LLSD().with(0, q.mQ[VX]).with(1, q.mQ[VY]).with(2, q.mQ[VZ]).with(3, q.mQ[VW]));
        return true;
    }
    bool inLimb(const std::string& end, const std::string& joint)
    {
        if (joint.empty()) return false;
        std::string a, b; bool leg;
        return limbOf(end, a, b, leg) && (joint == end || joint == a || joint == b);
    }

    // the limb's local rotations that put `end` at T (agent frame) with world rotation R
    bool solveLimb(const std::string& end, const LLVector3& T, const LLQuaternion& R, LLSD& pose)
    {
        std::string an, bn; bool leg = false;
        if (!limbOf(end, an, bn, leg) || !isAgentAvatarValid()) return false;
        LLJoint *ja = gAgentAvatarp->getJoint(an), *jb = gAgentAvatarp->getJoint(bn), *jc = gAgentAvatarp->getJoint(end);
        if (!ja || !jb || !jc || !ja->getParent()) return false;
        const LLVector3 A = ja->getWorldPosition(), B = jb->getWorldPosition(), C = jc->getWorldPosition();
        const F32 L1 = (B - A).length(), L2 = (C - B).length();
        if (L1 < 1e-4f || L2 < 1e-4f) return false;
        const LLVector3 toT = T - A;
        const F32 d = llclamp(toT.length(), std::fabs(L1 - L2) + 1e-3f, L1 + L2 - 1e-4f);
        LLVector3 dir = toT;
        if (dir.normVec() < 1e-6f) return false;
        LLVector3 pole = B - A;
        pole -= dir * (pole * dir);
        if (pole.length() < 0.03f * L1)
        {
            // a straight limb has no bend plane: knees bend forward, elbows back and down (the body's frame)
            LLJoint* pj = gAgentAvatarp->getJoint("mPelvis");
            const LLQuaternion pr = pj ? pj->getWorldRotation() : gAgentAvatarp->getRotation();
            const LLVector3 hint = leg ? LLVector3::x_axis * pr : LLVector3(-1.f, 0.f, -0.6f) * pr;
            pole = hint - dir * (hint * dir);
            if (pole.length() < 1e-4f) { const LLVector3 up = LLVector3::z_axis * pr; pole = up - dir * (up * dir); }
        }
        pole.normVec();
        const F32 x = (L1 * L1 - L2 * L2 + d * d) / (2.f * d), h = sqrtf(llmax(0.f, L1 * L1 - x * x));
        const LLVector3 Bn = A + dir * x + pole * h, Cn = A + dir * d;
        LLQuaternion turnA; turnA.shortestArc(B - A, Bn - A);
        const LLQuaternion Wa = ja->getWorldRotation() * turnA, Wb1 = jb->getWorldRotation() * turnA;
        LLQuaternion turnB; turnB.shortestArc((C - B) * turnA, Cn - Bn);
        const LLQuaternion Wb = Wb1 * turnB, Wp = ja->getParent()->getWorldRotation();
        auto put = [&](const std::string& n, LLQuaternion q)
        {
            q.normalize();
            pose[n] = LLSD().with("r", LLSD().with(0, q.mQ[VX]).with(1, q.mQ[VY]).with(2, q.mQ[VZ]).with(3, q.mQ[VW]));
        };
        put(an, Wa * ~Wp);
        put(bn, Wb * ~Wa);
        put(end, R * ~Wb);
        return true;
    }
    void captureLockRefs(const std::string& movingJoint)
    {
        sLockRefs.clear();
        if (!isAgentAvatarValid()) return;
        for (const std::string& e : sLocks)
        {
            if (inLimb(e, movingJoint) || inLimb(e, aimParent(movingJoint))) continue;   // the limb being moved itself
            if (LLJoint* j = gAgentAvatarp->getJoint(e)) sLockRefs.push_back({ e, j->getWorldPosition(), j->getWorldRotation() });
        }
    }
    void solveLocks(LLSD& pose)
    {
        for (const LockRef& r : sLockRefs) solveLimb(r.end, r.pos, r.rot, pose);
    }
    void keepLocks()
    {
        if (sLockRefs.empty()) return;
        LLSD pose = LLSD::emptyMap();
        solveLocks(pose);
        if (pose.size()) MGPhonePoser::applyPose(pose, true);
    }
    void pushPoseBegin()
    {
        LLSD groups = LLSD::emptyArray();
        groups.append("body"); groups.append("hands"); groups.append("face");
        push(LLSD().with("type", "studio_pose_begin").with("pose", MGPhonePoser::capturePose(groups)));
    }
    bool ikGizmoOn()
    {
        return sS.open && !sS.playing && !sPath.edit && sS.gizmo == "move" && isAgentAvatarValid() && ikJoint(MGPhonePoser::selectedJoint());
    }
    LLVector3 ikAxis(S32 a)
    {
        LLVector3 e; e.mV[a] = 1.f;
        return e * gAgentAvatarp->getRotation();
    }
    // the gizmo part under the mouse: 0..2 an axis, 3 the centre ring (free move), -1 none
    S32 ikGizmoAt(S32 x, S32 y)
    {
        if (!ikGizmoOn()) return -1;
        LLJoint* j = gAgentAvatarp->getJoint(MGPhonePoser::selectedJoint());
        if (!j) return -1;
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        const LLVector3 p = j->getWorldPosition();
        LLCoordGL c0;
        if (!cam->projectPosAgentToScreen(p, c0, false)) return -1;
        S32 best = -1;
        F32 bd = 7.f * 7.f;
        for (S32 a = 0; a < 3; ++a)
        {
            LLCoordGL c1;
            if (!cam->projectPosAgentToScreen(p + ikAxis(a) * IK_LEN, c1, false)) continue;
            const F32 ax = (F32)c0.mX, ay = (F32)c0.mY, dx = (F32)(c1.mX - c0.mX), dy = (F32)(c1.mY - c0.mY), L2 = dx * dx + dy * dy;
            if (L2 < 9.f) continue;
            const F32 u = llclamp(((x - ax) * dx + (y - ay) * dy) / L2, 0.f, 1.f);
            if (u < 0.3f) continue;
            const F32 px = ax + dx * u - x, py = ay + dy * u - y, d = px * px + py * py;
            if (d < bd) { bd = d; best = a; }
        }
        if (best < 0)
        {
            const F32 dx = (F32)(c0.mX - x), dy = (F32)(c0.mY - y);
            if (dx * dx + dy * dy <= 11.f * 11.f) best = 3;
        }
        return best;
    }
    // the parameter along a line (P0, unit A) where the mouse ray passes closest
    bool lookAtHit(S32 x, S32 y)
    {
        if (!sLook.on) return false;
        LLCoordGL c;
        if (!LLViewerCamera::getInstance()->projectPosAgentToScreen(sLook.p, c, false)) return false;
        const F32 dx = (F32)(c.mX - x), dy = (F32)(c.mY - y);
        return dx * dx + dy * dy <= 16.f * 16.f;
    }
    void pushLookAt(bool final)
    {
        const LLVector3 l = toLocal(sLook.p);
        push(LLSD().with("type", "studio_lookat").with("p", LLSD().with(0, l.mV[0]).with(1, l.mV[1]).with(2, l.mV[2])).with("final", final));
    }
    bool lineParam(S32 x, S32 y, const LLVector3& P0, const LLVector3& A, F32& t)
    {
        const LLVector3 C = LLViewerCamera::getInstance()->getOrigin(), D = gViewerWindow->mouseDirectionGlobal(x, y), w = P0 - C;
        const F32 b = A * D, d = A * w, e = D * w, den = 1.f - b * b;
        if (std::fabs(den) < 1e-4f) return false;
        t = (b * e - d) / den;
        return true;
    }
    void drawIK()
    {
        if (!sS.open || !isAgentAvatarValid()) return;
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        // the locks: an orange padlock on each locked hand / foot
        for (const std::string& e : sLocks)
        {
            LLJoint* j = gAgentAvatarp->getJoint(e);
            LLCoordGL c;
            if (!j || !cam->projectPosAgentToScreen(j->getWorldPosition(), c, false)) continue;
            const S32 x = c.mX + 10, y = c.mY + 10;
            gl_rect_2d(x - 6, y + 2, x + 6, y - 8, LLColor4(0.f, 0.f, 0.f, 0.7f), true);
            gl_rect_2d(x - 5, y + 1, x + 5, y - 7, LLColor4(1.f, 0.62f, 0.15f, 1.f), true);
            gGL.color4f(1.f, 0.62f, 0.15f, 1.f);
            gl_circle_2d((F32)x, (F32)(y + 3), 4.f, 12, false);
        }
        if (sLook.on)
        {
            LLCoordGL c;
            if (cam->projectPosAgentToScreen(sLook.p, c, false))
            {
                const LLColor4 col = sLook.drag || sLook.hover ? LLColor4::white : LLColor4(1.f, 0.85f, 0.3f, 1.f);
                gGL.color4fv(col.mV);
                gl_circle_2d((F32)c.mX, (F32)c.mY, 11.f, 24, false);
                gl_circle_2d((F32)c.mX, (F32)c.mY, 4.f, 12, true);
                gl_line_2d(c.mX - 16, c.mY, c.mX - 8, c.mY, col); gl_line_2d(c.mX + 8, c.mY, c.mX + 16, c.mY, col);
                gl_line_2d(c.mX, c.mY - 16, c.mX, c.mY - 8, col); gl_line_2d(c.mX, c.mY + 8, c.mX, c.mY + 16, col);
                LLFontGL::getFontSansSerifSmall()->renderUTF8("Look at", 0, c.mX + 14, c.mY + 12, col, LLFontGL::LEFT, LLFontGL::BASELINE, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
            }
        }
        if (!ikGizmoOn()) return;
        LLJoint* j = gAgentAvatarp->getJoint(MGPhonePoser::selectedJoint());
        LLCoordGL c0;
        if (!j || !cam->projectPosAgentToScreen(j->getWorldPosition(), c0, false)) return;
        static const LLColor4 AX[] = { LLColor4(1.f, 0.35f, 0.35f, 1.f), LLColor4(0.4f, 0.95f, 0.45f, 1.f), LLColor4(0.4f, 0.65f, 1.f, 1.f) };
        const S32 hot = sIK.on ? sIK.axis : sIK.hover;
        for (S32 a = 0; a < 3; ++a)
        {
            LLCoordGL c1;
            if (!cam->projectPosAgentToScreen(j->getWorldPosition() + ikAxis(a) * IK_LEN, c1, false)) continue;
            const LLColor4 col = hot == a ? LLColor4::white : AX[a];
            gl_line_2d(c0.mX, c0.mY, c1.mX, c1.mY, col);
            gl_line_2d(c0.mX + 1, c0.mY, c1.mX + 1, c1.mY, col);
            gGL.color4fv(col.mV);
            gl_circle_2d((F32)c1.mX, (F32)c1.mY, hot == a ? 6.f : 5.f, 12, true);
        }
        gGL.color4fv((hot == 3 ? LLColor4::white : LLColor4(1.f, 0.85f, 0.3f, 1.f)).mV);
        gl_circle_2d((F32)c0.mX, (F32)c0.mY, 10.f, 20, false);
        gl_circle_2d((F32)c0.mX, (F32)c0.mY, 9.f, 20, false);
    }

    // the joint dots (drawn under the page and the phone) and the path
    class MGStudioDots : public LLView
    {
    public:
        MGStudioDots(const LLView::Params& p) : LLView(p) {}
        void draw() override
        {
            if (sS.open && !sS.playing && !sPath.edit) MGPhonePoser::drawMarkers();
            if (sS.open) { drawTrails(); drawPath(); drawIK(); }
            LLView::draw();
        }
    };
    MGStudioDots* sDotsView = nullptr;
    void showDots(bool on)
    {
        if (on && !sDotsView && gViewerWindow && gViewerWindow->getRootView())
        {
            LLView::Params p;
            p.name = "mg_studio_dots";
            p.rect = gViewerWindow->getRootView()->getLocalRect();
            p.mouse_opaque = false;
            p.follows.flags = FOLLOWS_ALL;
            sDotsView = new MGStudioDots(p);
            gViewerWindow->getRootView()->addChild(sDotsView);
            gViewerWindow->getRootView()->sendChildToBack(sDotsView);
        }
        if (sDotsView) sDotsView->setVisible(on);
    }

    // ------------------------------------------------------------------
    // the 3D view's mouse while posing: click a joint's dot to pose it, drag the poser's rings
    // (rotate = FK) or its arrows (move = IK on hands and feet: the two bones above follow),
    // Alt + drag turns the camera. The viewer draws a tool's 3D marks only for the current
    // tool, so this one forwards render() to the poser's tool.
    // ------------------------------------------------------------------
    class MGToolStudio : public LLTool
    {
    public:
        MGToolStudio() : LLTool(std::string("MGAnimStudio")) {}
        bool isAlwaysRendered() override { return true; }
        void render() override
        {
            if (sS.playing || sPath.edit) return;
            if (sS.gizmo == "move" && ikJoint(MGPhonePoser::selectedJoint())) return;   // our IK gizmo (drawn in 2D)
            if (sS.gizmo == "move") FSToolCompPoseTranslate::getInstance()->render();
            else FSToolCompPose::getInstance()->render();
        }
        static LLSD frac(S32 x, S32 y)
        {
            const LLRect r = gViewerWindow->getRootView()->getLocalRect();
            return LLSD().with("x", (F64)x / llmax(1, r.getWidth())).with("y", 1.0 - (F64)y / llmax(1, r.getHeight()));
        }
        // the joint of the current group drawn nearest the mouse
        static std::string jointAt(S32 x, S32 y)
        {
            if (!isAgentAvatarValid()) return std::string();
            static LLSD list;
            if (!list.size()) list = MGPhonePoser::jointList();
            LLViewerCamera* cam = LLViewerCamera::getInstance();
            const F32 rad = sS.group == "body" ? 14.f : 9.f;
            F32 bd = rad * rad;
            std::string best;
            for (LLSD::array_const_iterator it = list.beginArray(); it != list.endArray(); ++it)
            {
                if ((*it)["g"].asString() != sS.group) continue;
                LLJoint* j = gAgentAvatarp->getJoint((*it)["n"].asString());
                LLCoordGL c;
                if (!j || !cam->projectPosAgentToScreen(j->getWorldPosition(), c, false)) continue;
                const F32 dx = (F32)(c.mX - x), dy = (F32)(c.mY - y), d = dx * dx + dy * dy;
                if (d < bd) { bd = d; best = (*it)["n"].asString(); }
            }
            return best;
        }
        // placing waypoints: click a surface to add one, drag one to move it, right-click to remove it
        bool pathDown(S32 x, S32 y)
        {
            const S32 ga = gizmoAxisAt(x, y);
            if (ga >= 0)
            {
                F32 t0;
                if (gizmoParam(x, y, ga, sPath.pts[sPath.sel], t0))
                {
                    sPath.gizAxis = ga; sPath.gizT0 = t0; sPath.gizStart = sPath.pts[sPath.sel]; sPath.drag = sPath.sel;
                    setMouseCapture(true);
                    return true;
                }
            }
            const S32 w = waypointAt(x, y);
            if (w >= 0)
            {
                sPath.drag = w; sPath.sel = w;
                sPath.downY = y; sPath.downZ = sPath.pts[w].mV[VZ];
                setMouseCapture(true);
                push(LLSD().with("type", "studio_path_sel").with("i", w));
                return true;
            }
            LLVector3 l;
            // on a leg between two waypoints: a new waypoint there
            LLViewerCamera* cam = LLViewerCamera::getInstance();
            for (S32 i = 1; i < (S32)sPath.pts.size(); ++i)
            {
                LLCoordGL a, b;
                if (!cam->projectPosAgentToScreen(toAgent(sPath.pts[i - 1]), a, false) || !cam->projectPosAgentToScreen(toAgent(sPath.pts[i]), b, false)) continue;
                const F32 ax = (F32)a.mX, ay = (F32)a.mY, bx = (F32)b.mX, by = (F32)b.mY, dx = bx - ax, dy = by - ay, L2 = dx * dx + dy * dy;
                if (L2 < 4.f) continue;
                const F32 u = llclamp(((x - ax) * dx + (y - ay) * dy) / L2, 0.f, 1.f), px = ax + dx * u - x, py = ay + dy * u - y;
                if (px * px + py * py > 7.f * 7.f || u < 0.08f || u > 0.92f) continue;
                if (!surfaceAt(x, y, l)) l = lerp(sPath.pts[i - 1], sPath.pts[i], u);
                push(LLSD().with("type", "studio_path_ins").with("i", i).with("p", LLSD().with(0, l.mV[0]).with(1, l.mV[1]).with(2, l.mV[2])));
                return true;
            }
            if (!surfaceAt(x, y, l)) return true;
            push(LLSD().with("type", "studio_path_add").with("p", LLSD().with(0, l.mV[0]).with(1, l.mV[1]).with(2, l.mV[2])));
            return true;
        }
        bool handleMouseUp(S32 x, S32 y, MASK mask) override
        {
            if (sLook.drag) { setMouseCapture(false); sLook.drag = false; pushLookAt(true); return true; }
            if (sIK.on)
            {
                setMouseCapture(false);
                sIK.on = false;
                sLockRefs.clear();
                push(LLSD().with("type", "studio_posed").with("joint", sIK.joint));
                return true;
            }
            if (sTrailDrag.on)
            {
                setMouseCapture(false);
                sTrailDrag.on = false;
                if (sTrailDrag.moved)
                {
                    const LLVector3 l = toLocal(sTrailDrag.cur);
                    push(LLSD().with("type", "studio_trail_drag").with("joint", sTrailDrag.joint).with("frame", sTrailDrag.frame).with("p", LLSD().with(0, l.mV[0]).with(1, l.mV[1]).with(2, l.mV[2])));
                }
                else push(LLSD().with("type", "studio_trail_key").with("joint", sTrailDrag.joint).with("frame", sTrailDrag.frame));
                return true;
            }
            if (sPath.drag >= 0)
            {
                setMouseCapture(false);
                const S32 i = sPath.drag;
                sPath.drag = -1;
                sPath.gizAxis = -1;
                if (i < (S32)sPath.pts.size())
                {
                    const LLVector3& l = sPath.pts[i];
                    push(LLSD().with("type", "studio_path_move").with("i", i).with("p", LLSD().with(0, l.mV[0]).with(1, l.mV[1]).with(2, l.mV[2])));
                }
                return true;
            }
            return false;
        }
        bool handleRightMouseDown(S32 x, S32 y, MASK mask) override
        {
            if (!sPath.edit)
            {
                std::string tj;
                S32 tf = 0;
                if (!trailKeyAt(x, y, tj, tf)) return false;
                push(LLSD().with("type", "studio_trail_menu").with("joint", tj).with("frame", tf));
                return true;
            }
            const S32 w = waypointAt(x, y);
            if (w > 0) push(LLSD().with("type", "studio_path_del").with("i", w));
            return true;
        }
        bool handleMouseDown(S32 x, S32 y, MASK mask) override
        {
            if (mask & MASK_ALT) return LLToolCamera::getInstance()->handleMouseDown(x, y, mask);
            if (sPath.edit) return pathDown(x, y);
            if (sS.playing || sRec.on) return true;
            if (lookAtHit(x, y)) { sLook.drag = true; setMouseCapture(true); return true; }
            const S32 ga = ikGizmoAt(x, y);
            if (ga >= 0)
            {
                LLJoint* jj = gAgentAvatarp->getJoint(MGPhonePoser::selectedJoint());
                if (jj)
                {
                    sIK.joint = MGPhonePoser::selectedJoint();
                    sIK.axis = ga;
                    sIK.start = jj->getWorldPosition();
                    sIK.rot = jj->getWorldRotation();
                    sIK.planeN = LLViewerCamera::getInstance()->getAtAxis();
                    sIK.axisDir = ga < 3 ? ikAxis(ga) : LLVector3::zero;
                    if (ga < 3 && !lineParam(x, y, sIK.start, sIK.axisDir, sIK.t0)) sIK.t0 = 0.f;
                    sIK.on = true;
                    if (sIK.joint == "mPelvis")
                    {
                        // where the hips are now (the poser's offset and turn)
                        LLSD groups = LLSD::emptyArray(); groups.append("body");
                        const LLSD cur = MGPhonePoser::capturePose(groups)["mPelvis"];
                        sIK.pelR = cur["r"];
                        sIK.pelP.setZero();
                        if (cur["p"].size() >= 3) sIK.pelP.set((F32)cur["p"][0].asReal(), (F32)cur["p"][1].asReal(), (F32)cur["p"][2].asReal());
                    }
                    pushPoseBegin();
                    captureLockRefs(sIK.joint);
                    setMouseCapture(true);
                    return true;
                }
            }
            std::string tj;
            S32 tf = 0;
            if (trailKeyAt(x, y, tj, tf))
            {
                // a click goes to that key; a drag moves the joint there (on a plane facing the camera)
                for (const Trail& t : sTrails)
                    if (t.joint == tj)
                        for (const TrailPt& q : t.pts)
                            if (q.f == tf && q.key) { sTrailDrag.start = sTrailDrag.cur = toAgent(q.p); }
                sTrailDrag.on = true; sTrailDrag.moved = false; sTrailDrag.joint = tj; sTrailDrag.frame = tf; sTrailDrag.x0 = x; sTrailDrag.y0 = y;
                setMouseCapture(true);
                return true;
            }
            const std::string j = jointAt(x, y);
            if (!j.empty() && j != MGPhonePoser::selectedJoint())
            {
                poser("pose_markers", LLSD().with("on", true).with("group", sS.group).with("sel", j));
                push(LLSD().with("type", "studio_joint").with("joint", j));
                return true;
            }
            LLSD a = frac(x, y);
            a["ev"] = "down";
            if (poser("pose_gizmo", a)["grabbed"].asBoolean()) return true;
            return true;   // the studio keeps the mouse (no object selection while posing)
        }
        bool handleHover(S32 x, S32 y, MASK mask) override
        {
            if (sPath.edit)
            {
                if (sPath.drag >= 0 && hasMouseCapture())
                {
                    LLVector3 l;
                    F32 tn;
                    if (sPath.gizAxis >= 0 && sPath.drag < (S32)sPath.pts.size())
                    {
                        // along one axis only (Ctrl: 5 cm steps)
                        if (gizmoParam(x, y, sPath.gizAxis, sPath.gizStart, tn))
                        {
                            F32 dt = llclamp(tn - sPath.gizT0, -50.f, 50.f);
                            if (mask & MASK_CONTROL) dt = ll_round(dt / 0.05f) * 0.05f;
                            LLVector3 np = sPath.gizStart;
                            np.mV[sPath.gizAxis] += dt;
                            sPath.pts[sPath.drag] = np;
                        }
                        gViewerWindow->setCursor(UI_CURSOR_SIZEALL);
                        return true;
                    }
                    if (sPath.drag < (S32)sPath.pts.size() && (mask & MASK_SHIFT))
                        sPath.pts[sPath.drag].mV[VZ] = llclamp(sPath.downZ + (F32)(y - sPath.downY) * 0.01f, -50.f, 50.f);   // Shift: up / down
                    else if (surfaceAt(x, y, l) && sPath.drag < (S32)sPath.pts.size()) sPath.pts[sPath.drag] = l;
                    gViewerWindow->setCursor(UI_CURSOR_CROSS);
                    return true;
                }
                sPath.gizHover = gizmoAxisAt(x, y);
                sPath.hover = sPath.gizHover >= 0 ? -1 : waypointAt(x, y);
                gViewerWindow->setCursor(sPath.gizHover >= 0 ? UI_CURSOR_SIZEALL : sPath.hover > 0 ? UI_CURSOR_HAND : UI_CURSOR_CROSS);
                return true;
            }
            if (sLook.drag && hasMouseCapture())
            {
                // on a plane facing the camera through the target
                LLViewerCamera* cam = LLViewerCamera::getInstance();
                const LLVector3 C = cam->getOrigin(), D = gViewerWindow->mouseDirectionGlobal(x, y), N = cam->getAtAxis();
                const F32 den = D * N;
                if (std::fabs(den) > 1e-4f) { const F32 tt = ((sLook.p - C) * N) / den; if (tt > 0.f) sLook.p = C + D * tt; }
                if (sLook.push.getElapsedTimeF32() > 0.05f) { sLook.push.reset(); pushLookAt(false); }
                gViewerWindow->setCursor(UI_CURSOR_SIZEALL);
                return true;
            }
            if (!sPath.edit && !sS.playing && sLook.on)
            {
                sLook.hover = lookAtHit(x, y);
                if (sLook.hover) { gViewerWindow->setCursor(UI_CURSOR_HAND); return true; }
            }
            if (sIK.on && hasMouseCapture())
            {
                // the target: along one axis (Ctrl: 1 cm steps), or on a plane facing the camera (the ring)
                LLVector3 target = sIK.start;
                if (sIK.axis < 3)
                {
                    F32 t;
                    if (lineParam(x, y, sIK.start, sIK.axisDir, t))
                    {
                        F32 dt = llclamp(t - sIK.t0, -2.f, 2.f);
                        if (mask & MASK_CONTROL) dt = ll_round(dt / 0.01f) * 0.01f;
                        target = sIK.start + sIK.axisDir * dt;
                    }
                }
                else
                {
                    const LLVector3 C = LLViewerCamera::getInstance()->getOrigin(), D = gViewerWindow->mouseDirectionGlobal(x, y);
                    const F32 den = D * sIK.planeN;
                    if (std::fabs(den) > 1e-4f) { const F32 tt = ((sIK.start - C) * sIK.planeN) / den; if (tt > 0.f) target = C + D * tt; }
                }
                LLSD pose = LLSD::emptyMap();
                std::string la, lb; bool lleg;
                if (sIK.joint == "mPelvis")
                {
                    // the offset lives in the root's frame (the root may face a little off the avatar)
                    LLJoint* rj = gAgentAvatarp->getRootJoint();
                    const LLQuaternion rr = rj ? rj->getWorldRotation() : gAgentAvatarp->getRotation();
                    const LLVector3 np = sIK.pelP + (target - sIK.start) * ~rr;
                    LLSD pe = LLSD().with("p", LLSD().with(0, np.mV[0]).with(1, np.mV[1]).with(2, np.mV[2]));
                    if (sIK.pelR.isArray()) pe["r"] = sIK.pelR;
                    pose["mPelvis"] = pe;
                }
                else if (limbOf(sIK.joint, la, lb, lleg)) solveLimb(sIK.joint, target, sIK.rot, pose);
                else solveAim(sIK.joint, target, pose);
                solveLocks(pose);
                if (pose.size()) MGPhonePoser::applyPose(pose, true);
                gViewerWindow->setCursor(UI_CURSOR_SIZEALL);
                return true;
            }
            if (!sPath.edit && !sS.playing)
            {
                sIK.hover = ikGizmoAt(x, y);
                if (sIK.hover >= 0) { gViewerWindow->setCursor(UI_CURSOR_SIZEALL); return true; }
            }
            if (sTrailDrag.on && hasMouseCapture())
            {
                if (std::abs(x - sTrailDrag.x0) + std::abs(y - sTrailDrag.y0) > 3) sTrailDrag.moved = true;
                // the plane through the key point, facing the camera
                LLViewerCamera* cam = LLViewerCamera::getInstance();
                const LLVector3 C = cam->getOrigin(), D = gViewerWindow->mouseDirectionGlobal(x, y), N = cam->getAtAxis();
                const F32 den = D * N;
                if (std::fabs(den) > 1e-4f)
                {
                    const F32 tt = ((sTrailDrag.start - C) * N) / den;
                    if (tt > 0.f) sTrailDrag.cur = C + D * tt;
                }
                gViewerWindow->setCursor(UI_CURSOR_SIZEALL);
                return true;
            }
            if (sS.playing) { gViewerWindow->setCursor(UI_CURSOR_ARROW); return true; }
            {
                std::string tj; S32 tf;
                if (trailKeyAt(x, y, tj, tf)) { gViewerWindow->setCursor(UI_CURSOR_HAND); return true; }
            }
            LLSD a = frac(x, y);
            a["ev"] = "hover";
            poser("pose_gizmo", a);
            gViewerWindow->setCursor(!jointAt(x, y).empty() ? UI_CURSOR_HAND : UI_CURSOR_TOOLROTATE);
            return true;
        }
    };
    MGToolStudio* sTool = nullptr;

    void toolOn()
    {
        if (!sTool) sTool = new MGToolStudio();
        LLToolMgr::getInstance()->setTransientTool(sTool);
        sS.tool = true;
    }
    void toolOff()
    {
        if (sTool && LLToolMgr::getInstance()->getCurrentTool() == sTool) LLToolMgr::getInstance()->clearTransientTool();
        sS.tool = false;
    }

    // the gizmo on a joint: a mode change goes through the poser's tool (which drops ours), so
    // ours is put back after it
    void setGizmo(const std::string& mode, const std::string& joint)
    {
        sS.gizmo = (mode == "move") ? "move" : "rotate";
        poser("pose_gizmo_mode", LLSD().with("mode", sS.gizmo));
        poser("pose_markers", LLSD().with("on", true).with("group", sS.group).with("sel", joint));
        toolOn();
    }

    // ------------------------------------------------------------------
    // files: projects (per account) and the .anim the user saves
    // ------------------------------------------------------------------
    std::string projectsDir()
    {
        const std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "mg_anim_studio");
        if (!LLFile::isdir(dir)) LLFile::mkdir(dir);
        return dir;
    }

    std::string cleanName(std::string s)
    {
        static const std::string bad = "\\/:*?\"<>|";
        for (char& c : s) if (bad.find(c) != std::string::npos || (unsigned char)c < 32) c = '_';
        LLStringUtil::trim(s);
        while (!s.empty() && s[0] == '.') s.erase(0, 1);
        return s.substr(0, 64);
    }

    void loadTLLayout()
    {
        sTLL.loaded = true;
        llifstream in(gDirUtilp->add(projectsDir(), "studio_ui.xml").c_str());
        if (!in.good()) return;
        LLSD d;
        if (LLSDSerialize::fromXMLDocument(d, in) <= 0) return;
        sTLL.top = d["top"].asBoolean();
        if (d.has("row_h")) sTLL.rowH = llclamp((S32)d["row_h"].asInteger(), 12, 40);
        if (d.has("width")) sTLL.width = llclamp((F32)d["width"].asReal(), 0.3f, 1.f);
    }
    void saveTLLayout()
    {
        llofstream out(gDirUtilp->add(projectsDir(), "studio_ui.xml").c_str());
        if (!out.good()) return;
        LLSDSerialize::toPrettyXML(LLSD().with("top", sTLL.top).with("row_h", sTLL.rowH).with("width", sTLL.width), out);
    }

    // the resident's own animations: only items they may do anything with (copy, modify, transfer)
    std::vector<LLUUID> sInvAnims;                   // list index -> inventory item (the page never sees ids)
    std::map<LLUUID, std::string> sInvWaiting;       // asset -> name, while it downloads
    struct FullPermAnim : public LLInventoryCollectFunctor
    {
        bool operator()(LLInventoryCategory*, LLInventoryItem* item) override
        {
            if (!item || item->getType() != LLAssetType::AT_ANIMATION) return false;
            const LLPermissions& p = item->getPermissions();
            return p.allowOperationBy(PERM_COPY, gAgentID) && p.allowOperationBy(PERM_MODIFY, gAgentID) && p.allowOperationBy(PERM_TRANSFER, gAgentID);
        }
    };
    void pushInvAnim(const LLUUID& asset, const std::string& name, const std::vector<U8>& bytes)
    {
        LLSD r = LLSD().with("type", "studio_imported").with("name", name).with("source", "inventory");
        LLSD anim;
        std::string err;
        if (bytes.empty()) r.with("ok", false).with("error", "read");
        else if (!decodeAnim(bytes, anim, err)) r.with("ok", false).with("error", err);
        else r.with("ok", true).with("anim", anim);
        push(r);
    }
    void onInvAnimLoaded(const LLUUID& asset, LLAssetType::EType type, void*, S32 status, LLExtStat)
    {
        auto it = sInvWaiting.find(asset);
        if (it == sInvWaiting.end()) return;
        const std::string name = it->second;
        sInvWaiting.erase(it);
        std::vector<U8> buf;
        if (status == 0)
        {
            LLFileSystem file(asset, type, LLFileSystem::READ);
            const S32 size = file.getSize();
            if (size > 0) { buf.resize(size); if (!file.read(buf.data(), size)) buf.clear(); }
        }
        pushInvAnim(asset, name, buf);
    }

    LLSD listProjects()
    {
        LLSD out = LLSD::emptyArray();
        const std::string dir = projectsDir();
        std::string file;
        LLDirIterator it(dir, "*.mgas");
        while (it.next(file))
        {
            const std::string path = gDirUtilp->add(dir, file);
            llstat st;
            const bool okst = LLFile::stat(path, &st) == 0;
            out.append(LLSD().with("name", gDirUtilp->getBaseFileName(path, true)).with("mtime", okst ? (F64)st.st_mtime : 0.0)
                             .with("size", okst ? (S32)st.st_size : 0));
        }
        return out;
    }

    LLSD& uploads()
    {
        static LLSD u;
        static bool read = false;
        if (!read)
        {
            read = true;
            std::ifstream in(gDirUtilp->add(projectsDir(), "uploads.xml").c_str(), std::ios::binary);
            if (in.good()) LLSDSerialize::fromXMLDocument(u, in);
            if (!u.isArray()) u = LLSD::emptyArray();
        }
        return u;
    }
    void writeUploads()
    {
        std::ofstream out(gDirUtilp->add(projectsDir(), "uploads.xml").c_str(), std::ios::binary | std::ios::trunc);
        if (out.good()) LLSDSerialize::toPrettyXML(uploads(), out);
    }

    std::string readFile(const std::string& path, size_t maxBytes)
    {
        std::ifstream in(path.c_str(), std::ios::binary);
        if (!in.good()) return std::string();
        in.seekg(0, std::ios::end);
        const std::streamoff n = in.tellg();
        if (n <= 0 || (size_t)n > maxBytes) return std::string();
        in.seekg(0, std::ios::beg);
        std::string s((size_t)n, '\0');
        in.read(&s[0], n);
        return s;
    }

    // a .bvh (through the uploader's own reader) or an .anim file into the page's form
    std::string motionText(const MGMotion& m)
    {
        std::ostringstream o;
        o.imbue(std::locale::classic());
        auto num = [&](float v, int prec) { char b[32]; snprintf(b, sizeof(b), "%.*g", prec, (double)v); o << b; };
        auto str = [&](const std::string& v) { o << '"'; for (char c : v) { if (c == '"' || c == '\\') o << '\\'; if ((unsigned char)c >= 32) o << c; } o << '"'; };
        o << "{\"fps\":"; num(m.fps, 6); o << ",\"frames\":" << m.frames << ",\"unit\":"; num(m.unit, 6);
        o << ",\"kind\":"; str(m.kind); o << ",\"take\":"; str(m.take); o << ",\"takes\":" << m.takes << ",\"bones\":[";
        for (size_t i = 0; i < m.bones.size(); ++i)
        {
            const MGMotionBone& b = m.bones[i];
            if (i) o << ',';
            o << "{\"n\":"; str(b.name); o << ",\"p\":" << b.parent << ",\"t\":[";
            for (int c = 0; c < 3; ++c) { if (c) o << ','; num(b.t[c], 7); }
            o << "],\"r\":["; for (int c = 0; c < 4; ++c) { if (c) o << ','; num(b.r[c], 7); }
            o << "],\"rz\":["; for (int c = 0; c < 4; ++c) { if (c) o << ','; num(b.rz[c], 7); }
            o << "],\"s\":"; num(b.s, 6);
            if (b.hasBind) { o << ",\"bind\":["; for (int k = 0; k < 16; ++k) { if (k) o << ','; num(b.bind[k], 7); } o << ']'; }
            o << '}';
        }
        // per-bone tracks as comma strings (rounded: 5 digits for rotations)
        o << "],\"rot\":[";
        for (size_t i = 0; i < m.bones.size(); ++i)
        {
            if (i) o << ',';
            const auto& v = m.bones[i].rot;
            if (v.empty()) { o << "null"; continue; }
            o << '"';
            for (size_t k = 0; k < v.size(); ++k) { if (k) o << ','; num(v[k], 5); }
            o << '"';
        }
        o << "],\"pos\":[";
        for (size_t i = 0; i < m.bones.size(); ++i)
        {
            if (i) o << ',';
            const auto& v = m.bones[i].pos;
            if (v.empty()) { o << "null"; continue; }
            o << '"';
            for (size_t k = 0; k < v.size(); ++k) { if (k) o << ','; num(v[k], 6); }
            o << '"';
        }
        o << "]}";
        return o.str();
    }

    LLSD importFile(const std::string& path)
    {
        const std::string ext = gDirUtilp->getExtension(path);
        const std::string name = gDirUtilp->getBaseFileName(path, true);
        LLSD r = LLSD().with("type", "studio_imported").with("name", name).with("ext", ext);
        if (ext == "imotion" || ext == "rlmotion" || ext == "imotionplus" || ext == "rlmotionplus")
            return r.with("ok", false).with("error", "iclone_format");
        // any rig, any tool: FBX and BVH go through the converter (the page maps the bones onto SL)
        if (ext == "fbx" || ext == "bvh")
        {
            MGMotion m;
            std::string err;
            bool ok = mgReadMotion(path, m, err, 30.f, 300.f);
            size_t samples = 0;
            for (const MGMotionBone& b : m.bones) samples += b.rot.size() / 4;
            // a long take on a big rig: fewer frames a second (the page resamples to the clip's rate anyway)
            if (ok && samples > 400000) ok = mgReadMotion(path, m, err, samples > 1200000 ? 10.f : 15.f, 300.f);
            if (ok)
            {
                static S32 sMotionId = 0;
                const std::string id = llformat("m%d", ++sMotionId);
                const std::string text = motionText(m);
                const size_t PART = 200000;
                const S32 n = (S32)((text.size() + PART - 1) / PART);
                for (S32 i = 0; i < n; ++i)
                    push(LLSD().with("type", "studio_motion_part").with("id", id).with("i", i).with("n", n).with("data", text.substr((size_t)i * PART, PART)));
                LL_INFOS("MGStudio") << "Animation Studio: " << path << " read: " << m.bones.size() << " bones, " << m.frames << " frames at " << m.fps
                                     << " fps, " << text.size() << " chars in " << n << " parts" << LL_ENDL;
                return r.with("ok", true).with("motion_id", id).with("parts", n).with("source", ext).with("bones", (S32)m.bones.size()).with("frames", m.frames);
            }
            if (ext == "fbx") return r.with("ok", false).with("error", err);
            // an SL BVH the uploader reads: the old path below
            LL_INFOS("MGStudio") << "Animation Studio: BVH reader: " << err << ", trying the uploader's reader" << LL_ENDL;
        }
        const std::string data = readFile(path, 16 * 1024 * 1024);
        if (data.empty()) return r.with("ok", false).with("error", "read");
        std::vector<U8> bytes;
        if (ext == "bvh")
        {
            if (!isAgentAvatarValid()) return r.with("ok", false).with("error", "avatar");
            ELoadStatus status = E_ST_OK;
            S32 line = 0;
            auto aliases = gAgentAvatarp->getJointAliases();
            LLBVHLoader loader(data.c_str(), status, line, aliases);
            if (!loader.isInitialized())
                return r.with("ok", false).with("error", "bvh").with("line", line).with("status", (S32)status);
            if (loader.getDuration() > MAX_ANIM_DURATION) return r.with("ok", false).with("error", "too_long");
            const U32 size = loader.getOutputSize();
            bytes.assign(size, 0);
            LLDataPackerBinaryBuffer dp(bytes.data(), (S32)size);
            if (!loader.serialize(dp)) return r.with("ok", false).with("error", "bvh");
            bytes.resize(dp.getCurrentSize());
            r["frames"] = loader.getNumFrames();
        }
        else
        {
            bytes.assign(data.begin(), data.end());
        }
        LLSD anim;
        std::string err;
        if (!decodeAnim(bytes, anim, err)) return r.with("ok", false).with("error", err);
        return r.with("ok", true).with("anim", anim).with("source", ext);
    }

    // ------------------------------------------------------------------
    // camera framings for posing
    // ------------------------------------------------------------------
    LLSD camera(const std::string& view, F32 turn)
    {
        if (!isAgentAvatarValid()) return fail("avatar");
        if (view == "reset") { gAgentCamera.resetView(); return LLSD().with("ok", true); }
        const char* jn = "mPelvis";
        F32 dist = 3.4f, up = 0.25f;
        if (view == "face") { jn = "mHead"; dist = 0.7f; up = 0.04f; }
        else if (view == "upper") { jn = "mChest"; dist = 1.5f; up = 0.1f; }
        else if (view == "hands") { jn = "mWristLeft"; dist = 0.75f; up = 0.1f; }
        else if (view == "feet") { jn = "mAnkleLeft"; dist = 1.6f; up = 0.35f; }
        LLJoint* j = gAgentAvatarp->getJoint(jn);
        if (!j) return fail("joint");
        LLVector3 focus = j->getWorldPosition();
        if (view == "feet")
            if (LLJoint* r = gAgentAvatarp->getJoint("mAnkleRight")) focus = (focus + r->getWorldPosition()) * 0.5f;
        LLVector3 fwd = LLVector3::x_axis * gAgentAvatarp->getRotation();
        fwd.mV[VZ] = 0.f;
        if (fwd.normVec() < 1e-4f) fwd = LLVector3::x_axis;
        const LLVector3 dir = fwd * LLQuaternion(turn * DEG_TO_RAD, LLVector3::z_axis);
        const LLVector3 cam = focus + dir * dist + LLVector3(0.f, 0.f, up);
        gAgentCamera.setCameraPosAndFocusGlobal(gAgent.getPosGlobalFromAgent(cam), gAgent.getPosGlobalFromAgent(focus), gAgent.getID());
        return LLSD().with("ok", true);
    }

    void closeAll(bool notify)
    {
        if (!sS.open) return;
        stopLocal();
        toolOff();
        poser("pose_gizmo_mode", LLSD().with("mode", "off"));
        poser("pose_markers", LLSD().with("on", false));
        poser("pose_stop");
        showTimeline(false);
        showDots(false);
        sPath = PathView();
        sTrails.clear();
        sRec.on = false;
        if (sS.aoWas) gSavedPerAccountSettings.setBOOL("UseAO", true);
        sS.aoWas = false;
        sS.open = false;
        sS.wasDragging = false;
        if (notify) push(LLSD().with("type", "studio_closed"));
    }
}

namespace MGPhoneAnimStudio
{
    bool isCommand(const std::string& cmd) { return cmd.rfind("studio_", 0) == 0; }

    LLSD runCommand(const std::string& cmd, const LLSD& args)
    {
        LLSD result = LLSD().with("ok", true);
        if (!isAgentAvatarValid()) return fail("avatar");

        if (cmd == "studio_open")
        {
            if (!sS.open)
            {
                sS.aoWas = gSavedPerAccountSettings.getBOOL("UseAO");
                if (sS.aoWas) gSavedPerAccountSettings.setBOOL("UseAO", false);
            }
            const std::string start = args["start"].asString();
            const LLSD r = (start == "t" || start == "a") ? poser("pose_preset", LLSD().with("name", start)) : poser("pose_start");
            if (!r["ok"].asBoolean() && !MGPhonePoser::isPosing())
            {
                if (sS.aoWas && !sS.open) { gSavedPerAccountSettings.setBOOL("UseAO", true); sS.aoWas = false; }
                return fail("cannot_pose");
            }
            sS.open = true;
            holdAll();
            sS.group = "body";
            showDots(true);
            setGizmo("rotate", args.has("joint") ? args["joint"].asString() : std::string("mChest"));
            sTL.beat.reset();
            LLSD groups = LLSD::emptyArray();
            groups.append("body"); groups.append("hands"); groups.append("face");
            result["joints"] = MGPhonePoser::jointList();
            result["pose"] = MGPhonePoser::capturePose(groups);
            result["cost"] = LLAgentBenefitsMgr::current().getAnimationUploadCost();
            result["female"] = gAgentAvatarp->getSex() == SEX_FEMALE;
            result["uploads"] = uploads();
            result["projects"] = listProjects();
            return result;
        }
        if (cmd == "studio_close") { closeAll(false); return result; }
        if (cmd == "studio_camera") return camera(args["view"].asString(), (F32)args["turn"].asReal());
        if (cmd == "studio_library")
        {
            const std::string path = gDirUtilp->add(projectsDir(), "library.mgas");
            const std::string what = args["what"].asString();
            if (what == "save")
            {
                std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
                if (!out.good()) return fail("write");
                LLSDSerialize::toBinary(args["data"], out);
                return result;
            }
            LLSD data;
            std::ifstream in(path.c_str(), std::ios::binary);
            if (!in.good()) return result.with("data", LLSD::emptyMap());
            if (LLSDSerialize::fromBinary(data, in, 96 * 1024 * 1024) == LLSDParser::PARSE_FAILURE || !data.isMap()) return result.with("data", LLSD::emptyMap());
            return result.with("data", data);
        }
        if (cmd == "studio_projects")
        {
            const std::string what = args["what"].asString();
            const std::string name = cleanName(args["name"].asString());
            const std::string path = gDirUtilp->add(projectsDir(), name + ".mgas");
            if (what == "save")
            {
                if (name.empty() || !args["data"].isMap()) return fail("name");
                std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
                if (!out.good()) return fail("write");
                LLSDSerialize::toBinary(args["data"], out);
                out.close();
                result["name"] = name;
            }
            else if (what == "load")
            {
                std::ifstream in(path.c_str(), std::ios::binary);
                LLSD data;
                if (name.empty() || !in.good() || LLSDSerialize::fromBinary(data, in, 64 * 1024 * 1024) == LLSDParser::PARSE_FAILURE || !data.isMap())
                    return fail("read");
                result["name"] = name;
                result["data"] = data;
            }
            else if (what == "delete")
            {
                if (name.empty() || !LLFile::isfile(path) || LLFile::remove(path) != 0) return fail("delete");
            }
            result["projects"] = listProjects();
            return result;
        }

        if (!sS.open) return fail("closed");

        if (cmd == "studio_capture")
        {
            result["pose"] = MGPhonePoser::capturePose(args["groups"]);
            return result;
        }
        if (cmd == "studio_lookat_mode")
        {
            sLook.on = args["on"].asBoolean();
            const LLSD& q = args["p"];
            if (q.size() >= 3) sLook.p = toAgent(LLVector3((F32)q[0].asReal(), (F32)q[1].asReal(), (F32)q[2].asReal()));
            return result.with("on", sLook.on);
        }
        if (cmd == "studio_locks")
        {
            sLocks.clear();
            const LLSD& js = args["joints"];
            for (LLSD::array_const_iterator it = js.beginArray(); it != js.endArray(); ++it)
                if (ikJoint(it->asString())) sLocks.insert(it->asString());
            return result.with("locks", (S32)sLocks.size());
        }
        if (cmd == "studio_set")
        {
            if (sS.playing) return fail("playing");
            if (args["keep_locks"].asBoolean() && !sLocks.empty())
            {
                if (sRelock <= 0) captureLockRefs(args["moving"].asString());   // where they are before this change
                sRelock = 5;
            }
            result["joints"] = MGPhonePoser::applyPose(args["pose"], args["snap"].asBoolean());
            return result;
        }
        if (cmd == "studio_tool")
        {
            const std::string mode = args["mode"].asString();
            if (args.has("group")) sS.group = args["group"].asString() == "hands" ? "hands" : args["group"].asString() == "face" ? "face" : "body";
            const std::string joint = args.has("joint") ? args["joint"].asString() : MGPhonePoser::selectedJoint();
            if (mode == "off")
            {
                toolOff();
                poser("pose_gizmo_mode", LLSD().with("mode", "off"));
                poser("pose_markers", LLSD().with("on", false));
            }
            else if (sS.playing)
            {
                sS.gizmo = (mode == "move") ? "move" : "rotate";   // used again when playback stops
            }
            else
            {
                setGizmo(mode.empty() ? sS.gizmo : mode, joint);
            }
            result["gizmo"] = sS.gizmo;
            result["group"] = sS.group;
            result["joint"] = MGPhonePoser::selectedJoint();
            return result;
        }
        if (cmd == "studio_play")
        {
            if (needsFree(args["anim"]))
            {
                // past the format's limits: the poser plays it (the studio's own preview)
                stopLocal();
                toolOff();
                poser("pose_gizmo_mode", LLSD().with("mode", "off"));
                poser("pose_markers", LLSD().with("on", false));
                if (!MGPhonePoser::isPosing()) poser("pose_start");
                if (!startFree(args["anim"])) return fail("anim");
                sS.playing = true;
                return result.with("free", true);
            }
            std::string err;
            const std::vector<U8> bytes = buildAnim(args["anim"], err);
            if (bytes.empty()) return fail(err);
            // the poser lets go (it would hold every joint), the clip plays as everyone would see it
            poser("pose_gizmo_mode", LLSD().with("mode", "off"));
            poser("pose_stop");
            if (!playLocal(bytes))
            {
                poser("pose_start");
                holdAll();
                setGizmo(sS.gizmo, MGPhonePoser::selectedJoint());
                return fail("anim");
            }
            return result.with("bytes", (S32)bytes.size());
        }
        if (cmd == "studio_stop")
        {
            stopLocal();
            if (!MGPhonePoser::isPosing()) poser("pose_start");
            holdAll();
            setGizmo(sS.gizmo, MGPhonePoser::selectedJoint());
            return result;
        }
        if (cmd == "studio_path")
        {
            // the waypoints (page order), the computed track, edit mode (the mouse places waypoints)
            if (args.has("on")) sPath.on = args["on"].asBoolean();
            if (args.has("edit"))
            {
                const bool edit = args["edit"].asBoolean() && sPath.on;
                if (edit != sPath.edit)
                {
                    sPath.edit = edit;
                    if (edit) { poser("pose_gizmo_mode", LLSD().with("mode", "off")); toolOn(); }
                    else setGizmo(sS.gizmo, MGPhonePoser::selectedJoint());
                }
            }
            if (!sPath.on && sPath.edit) { sPath.edit = false; setGizmo(sS.gizmo, MGPhonePoser::selectedJoint()); }
            if (args.has("pts"))
            {
                sPath.pts.clear();
                const LLSD& p = args["pts"];
                for (S32 i = 0; i < (S32)p.size(); ++i) sPath.pts.push_back(LLVector3((F32)p[i][0].asReal(), (F32)p[i][1].asReal(), (F32)p[i][2].asReal()));
            }
            if (args.has("frames"))
            {
                sPath.frames.clear();
                for (LLSD::array_const_iterator it = args["frames"].beginArray(); it != args["frames"].endArray(); ++it) sPath.frames.push_back((S32)it->asInteger());
            }
            if (args.has("track"))
            {
                sPath.track.clear();
                const LLSD& t = args["track"];
                for (S32 i = 0; i + 3 < (S32)t.size(); i += 4)
                    sPath.track.push_back(LLVector4((F32)t[i].asReal(), (F32)t[i + 1].asReal(), (F32)t[i + 2].asReal(), (F32)t[i + 3].asReal()));
            }
            if (args.has("gaits"))
            {
                sPath.gaits.clear();
                for (LLSD::array_const_iterator it = args["gaits"].beginArray(); it != args["gaits"].endArray(); ++it) sPath.gaits.push_back((S32)it->asInteger());
            }
            if (args.has("sel")) sPath.sel = (S32)args["sel"].asInteger();
            return result.with("edit", sPath.edit);
        }
        if (cmd == "studio_trails")
        {
            sTrails.clear();
            sTrailCur = (S32)args["cur"].asInteger();
            sTrailEvery = llclamp((S32)args["every"].asInteger(), 0, 30);
            sTrailLabels = llclamp((S32)args["labels"].asInteger(), 0, 2);
            const LLSD& ts = args["trails"];
            for (S32 i = 0; i < (S32)ts.size() && i < 16; ++i)
            {
                Trail t;
                t.joint = ts[i]["joint"].asString();
                const LLSD& c = ts[i]["col"];
                t.col = c.size() >= 3 ? LLColor4((F32)c[0].asReal(), (F32)c[1].asReal(), (F32)c[2].asReal(), 1.f) : LLColor4(0.31f, 0.82f, 0.91f, 1.f);
                const LLSD& q = ts[i]["pts"];
                for (S32 k = 0; k + 4 < (S32)q.size(); k += 5)
                {
                    TrailPt pt;
                    pt.p.set((F32)q[k].asReal(), (F32)q[k + 1].asReal(), (F32)q[k + 2].asReal());
                    pt.f = (S32)q[k + 3].asInteger();
                    pt.key = q[k + 4].asInteger() != 0;
                    pt.point = q[k + 4].asInteger() == 2;
                    t.pts.push_back(pt);
                }
                sTrails.push_back(t);
            }
            return result;
        }
        if (cmd == "studio_skel")
        {
            // the avatar's own skeleton at rest (its shape, its mesh body's joint positions): every
            // bone's place relative to the pelvis, chained as the viewer chains joints (an offset is
            // scaled by its parent's scale), no turns
            LLSD out = LLSD::emptyMap();
            LLJoint* pel = gAgentAvatarp->getJoint("mPelvis");
            if (!pel) return fail("skeleton");
            std::function<void(LLJoint*, const LLVector3&, S32)> walk = [&](LLJoint* j, const LLVector3& wp, S32 depth)
            {
                const std::string& n = j->getName();
                if (n.empty() || n[0] != 'm' || depth > 40) return;
                out[n] = LLSD().with(0, wp.mV[0]).with(1, wp.mV[1]).with(2, wp.mV[2]);
                const LLVector3 sc = j->getScale();
                for (LLJoint* c : j->mChildren)
                {
                    const LLVector3 cp = c->getPosition();
                    walk(c, wp + LLVector3(cp.mV[0] * sc.mV[0], cp.mV[1] * sc.mV[1], cp.mV[2] * sc.mV[2]), depth + 1);
                }
            };
            walk(pel, LLVector3::zero, 0);
            return result.with("joints", out).with("pelvis_to_foot", gAgentAvatarp->getPelvisToFoot());
        }
        if (cmd == "studio_rec")
        {
            if (args["on"].asBoolean())
            {
                if (sRec.on) return fail("recording");
                // the poser lets go: the avatar walks with its own animations while the take runs
                stopLocal();
                toolOff();
                poser("pose_gizmo_mode", LLSD().with("mode", "off"));
                poser("pose_markers", LLSD().with("on", false));
                poser("pose_stop");
                if (!pathFrame(sRec.o, sRec.r)) return fail("avatar");
                sRec.samples.clear();
                sRec.marks.clear();
                for (bool& k : sRec.keyWas) k = false;
                sRec.timer.reset();
                sRec.next = 0.f;
                sRec.on = true;
                return result;
            }
            if (!sRec.on) return fail("not_recording");
            sRec.on = false;
            if (!MGPhonePoser::isPosing()) poser("pose_start");
            holdAll();
            setGizmo(sS.gizmo, MGPhonePoser::selectedJoint());
            LLSD sm = LLSD::emptyArray();
            for (F32 v : sRec.samples) sm.append(ll_round(v * 1000.f) / 1000.0);
            sRec.samples.clear();
            LLSD mk = LLSD::emptyArray();
            for (F32 v : sRec.marks) mk.append(ll_round(v * 1000.f) / 1000.0);
            sRec.marks.clear();
            return result.with("samples", sm).with("marks", mk);
        }
        if (cmd == "studio_ground")
        {
            // floor heights under points of the path frame: [x, y, zhint, ...] -> z[]
            const LLSD& p = args["pts"];
            LLSD z = LLSD::emptyArray();
            for (S32 i = 0; i + 2 < (S32)p.size() && i < 3 * 2000; i += 3)
                z.append(ll_round(groundAt((F32)p[i].asReal(), (F32)p[i + 1].asReal(), (F32)p[i + 2].asReal()) * 1000.f) / 1000.0);
            result["z"] = z;
            LLVector3 o; LLQuaternion r;
            if (gAgent.getRegion() && pathFrame(o, r))
            {
                const LLVector3 rp = gAgent.getRegion()->getPosRegionFromAgent(o);
                const LLVector3 fx = LLVector3::x_axis * r;
                result["region"] = LLSD().with(0, rp.mV[0]).with(1, rp.mV[1]).with(2, rp.mV[2]);
                result["yaw"] = atan2f(fx.mV[VY], fx.mV[VX]) * RAD_TO_DEG;
                result["width"] = gAgent.getRegion()->getWidth();
            }
            return result;
        }
        if (cmd == "studio_timeline" && args.has("beat") && !args.has("tracks"))
        {
            // the page is still there: only the clock (cheap, every 2 s)
            sTL.beat.reset();
            return result;
        }
        if (cmd == "studio_timeline")
        {
            const S32 frames = llclamp((S32)args["frames"].asInteger(), 1, 9000);
            if (!sTL.zoomed) { sTL.v0 = 0.f; sTL.v1 = (F32)llmax(10, frames); }
            sTL.frames = frames;
            sTL.fps = llclamp((F32)args["fps"].asReal(), 1.f, 60.f);
            const bool playing = args["playing"].asBoolean();
            if (playing && (!sTL.playing || args.has("play_from")))
            {
                sTL.playFrom = (F32)args["play_from"].asReal();
                sTL.playTimer.reset();
            }
            sTL.playing = playing;
            if (!playing) sTL.frame = (F32)args["frame"].asReal();
            sTL.loop = args["loop"].asBoolean();
            sTL.loopIn = llclamp((S32)args["loop_in"].asInteger(), 0, frames);
            sTL.loopOut = llclamp((S32)(args.has("loop_out") ? args["loop_out"].asInteger() : frames), 0, frames);
            sTL.autoKey = args["auto"].asBoolean();
            if (args.has("bar"))
            {
                sBar.clear();
                const LLSD& bl = args["bar"];
                for (LLSD::array_const_iterator bi = bl.beginArray(); bi != bl.endArray(); ++bi)
                {
                    const LLSD& it = *bi;
                    BarItem b; b.id = it["id"].asString(); b.tip = it["tip"].asString(); b.on = it["on"].asBoolean(); b.sep = it["sep"].asBoolean();
                    if (!b.id.empty() && sBar.size() < 24) sBar.push_back(b);
                }
            }
            sTL.hint = args["hint"].asString();
            sTL.title = args["title"].asString();
            if (args.has("sel_track")) sTL.selTrack = args["sel_track"].asString();
            if (args.has("edge_mode")) sTL.edgeMode = args["edge_mode"].asString();
            if (args.has("clips"))
            {
                sTL.clips.clear();
                const LLSD& cs = args["clips"];
                for (S32 i = 0; i < (S32)cs.size() && i < 64; ++i)
                {
                    ClipBlock c;
                    c.id = cs[i]["id"].asString(); c.name = cs[i]["name"].asString();
                    c.start = (S32)cs[i]["start"].asInteger(); c.len = llmax(1, (S32)cs[i]["len"].asInteger());
                    c.cyc = (F32)cs[i]["cyc"].asReal(); c.blend = (S32)cs[i]["blend"].asInteger(); c.speed = (F32)cs[i]["speed"].asReal();
                    c.sel = cs[i]["sel"].asBoolean();
                    if (cs[i]["col"].size() >= 3) c.col = LLColor4((F32)cs[i]["col"][0].asReal(), (F32)cs[i]["col"][1].asReal(), (F32)cs[i]["col"][2].asReal(), 1.f);
                    sTL.clips.push_back(c);
                }
            }
            if (args.has("tracks"))
            {
                sTL.tracks.clear();
                const LLSD& ts = args["tracks"];
                for (S32 i = 0; i < (S32)ts.size() && i < 12; ++i)
                {
                    Track t;
                    t.id = ts[i]["id"].asString();
                    t.label = ts[i]["label"].asString();
                    t.kind = ts[i]["kind"].asString();
                    for (LLSD::array_const_iterator k = ts[i]["keys"].beginArray(); k != ts[i]["keys"].endArray(); ++k) t.keys.push_back((S32)k->asInteger());
                    for (LLSD::array_const_iterator k = ts[i]["sel"].beginArray(); k != ts[i]["sel"].endArray(); ++k) t.sel.insert((S32)k->asInteger());
                    sTL.tracks.push_back(t);
                }
            }
            sTL.any = true;
            sTL.beat.reset();
            showTimeline(true);
            return result;
        }
        if (cmd == "studio_upload")
        {
            if (sS.pending > 0) return fail("busy");
            std::string err;
            const std::vector<U8> bytes = buildAnim(args["anim"], err);
            if (bytes.empty()) return fail(err);
            if (!validate(bytes)) return fail("invalid");
            const std::string name = args["name"].asString().empty() ? std::string("Motion Lab") : args["name"].asString().substr(0, 63);
            const std::string desc = args["desc"].asString().empty() ? std::string("Made with Motion Lab (MasterGate Viewer)") : args["desc"].asString().substr(0, 127);
            const S32 cost = LLAgentBenefitsMgr::current().getAnimationUploadCost();
            sS.pending = 1;
            LLNewBufferedResourceUploadInfo::uploadFinish_f finish = [name](LLUUID asset, LLSD)
            {
                sS.pending = 0;
                uploads().append(LLSD().with("name", name).with("asset", asset).with("date", (F64)time(nullptr)));
                writeUploads();
                push(LLSD().with("type", "studio_uploaded").with("ok", true).with("asset", asset).with("name", name).with("uploads", uploads()));
            };
            LLNewBufferedResourceUploadInfo::uploadFailure_f failure = [](LLUUID, LLSD, std::string reason)
            {
                sS.pending = 0;
                push(LLSD().with("type", "studio_uploaded").with("ok", false).with("error", reason.empty() ? std::string("upload") : reason));
                return true;
            };
            const std::string data((const char*)bytes.data(), bytes.size());
            LLResourceUploadInfo::ptr_t info(std::make_shared<LLNewBufferedResourceUploadInfo>(
                data, LLUUID::generateNewID(), name, desc, 0,
                LLFolderType::FT_ANIMATION, LLInventoryType::IT_ANIMATION, LLAssetType::AT_ANIMATION,
                LLFloaterPerms::getNextOwnerPerms("Uploads"), LLFloaterPerms::getGroupPerms("Uploads"), LLFloaterPerms::getEveryonePerms("Uploads"),
                cost, LLUUID::null, false, finish, failure));
            upload_new_resource(info);
            LL_INFOS("MGStudio") << "Animation Studio: uploading \"" << name << "\" (" << bytes.size() << " bytes)" << LL_ENDL;
            return result.with("bytes", (S32)bytes.size()).with("cost", cost);
        }
        if (cmd == "studio_save")
        {
            if (sS.picking) return fail("busy");
            std::string err;
            auto bytes = std::make_shared<std::vector<U8>>(buildAnim(args["anim"], err));
            if (bytes->empty()) return fail(err);
            if (!validate(*bytes)) return fail("invalid");
            std::string name = cleanName(args["name"].asString());
            if (name.empty()) name = "animation";
            sS.picking = true;
            LLFilePickerReplyThread::startPicker(
                [bytes](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
                {
                    sS.picking = false;
                    if (files.empty()) return;
                    std::string path = files[0];
                    if (gDirUtilp->getExtension(path) != "anim") path += ".anim";
                    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
                    bool ok = out.good();
                    if (ok) { out.write((const char*)bytes->data(), bytes->size()); ok = out.good(); }
                    push(LLSD().with("type", "studio_saved").with("ok", ok).with("path", path).with("name", gDirUtilp->getBaseFileName(path, false)));
                },
                LLFilePicker::FFSAVE_ALL, name + ".anim",
                [](const std::vector<std::string>&, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter) { sS.picking = false; });
            return result.with("bytes", (S32)bytes->size());
        }
        if (cmd == "studio_inv_anims")
        {
            LLInventoryModel::cat_array_t cats;
            LLInventoryModel::item_array_t items;
            FullPermAnim want;
            gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, items, LLInventoryModel::EXCLUDE_TRASH, want);
            std::sort(items.begin(), items.end(), [](const LLPointer<LLViewerInventoryItem>& a, const LLPointer<LLViewerInventoryItem>& b) { return LLStringUtil::compareDict(a->getName(), b->getName()) < 0; });
            sInvAnims.clear();
            LLSD list = LLSD::emptyArray();
            for (const auto& it : items)
            {
                if (sInvAnims.size() >= 1000) break;
                sInvAnims.push_back(it->getUUID());
                list.append(LLSD().with("i", (S32)sInvAnims.size() - 1).with("name", it->getName()));
            }
            return result.with("anims", list);
        }
        if (cmd == "studio_inv_preview")
        {
            // plays it on the avatar, only on this screen (the poser lets go meanwhile); studio_stop ends it
            const S32 i = args["i"].asInteger();
            if (i < 0 || i >= (S32)sInvAnims.size() || !isAgentAvatarValid()) return fail("not_found");
            LLViewerInventoryItem* item = gInventory.getItem(sInvAnims[i]);
            FullPermAnim ok;
            if (!item || !ok(nullptr, item) || item->getAssetUUID().isNull()) return fail("not_found");
            stopLocal();
            toolOff();
            poser("pose_gizmo_mode", LLSD().with("mode", "off"));
            poser("pose_markers", LLSD().with("on", false));
            poser("pose_stop");
            sS.invPreview = item->getAssetUUID();
            gAgentAvatarp->startMotion(sS.invPreview);
            sS.playing = true;
            return result;
        }
        if (cmd == "studio_inv_load")
        {
            const S32 i = args["i"].asInteger();
            if (i < 0 || i >= (S32)sInvAnims.size()) return fail("not_found");
            LLViewerInventoryItem* item = gInventory.getItem(sInvAnims[i]);
            if (!item || item->getType() != LLAssetType::AT_ANIMATION) return fail("not_found");
            FullPermAnim ok;
            if (!ok(nullptr, item)) return fail("perms");   // full permission only
            const LLUUID asset = item->getAssetUUID();
            if (asset.isNull()) return fail("not_found");
            const bool first = sInvWaiting.find(asset) == sInvWaiting.end();
            sInvWaiting[asset] = item->getName();
            if (first) gAssetStorage->getAssetData(asset, LLAssetType::AT_ANIMATION, onInvAnimLoaded, nullptr, true);
            return result.with("pending", true);
        }
        if (cmd == "studio_save_route")
        {
            if (sS.picking) return fail("busy");
            struct OutFile { std::string name; std::vector<U8> bytes; };
            auto files = std::make_shared<std::vector<OutFile>>();
            const LLSD& fs = args["files"];
            for (S32 i = 0; i < (S32)fs.size() && i < 64; ++i)
            {
                OutFile f;
                const std::string n = cleanName(fs[i]["name"].asString());
                if (n.empty()) return fail("name");
                if (fs[i].has("anim"))
                {
                    std::string err;
                    f.bytes = buildAnim(fs[i]["anim"], err);
                    if (f.bytes.empty()) return fail(err + " (" + n + ")");
                    if (!validate(f.bytes)) return fail("invalid (" + n + ")");
                    f.name = n + ".anim";
                }
                else
                {
                    const std::string txt = fs[i]["text"].asString();
                    if (txt.size() > 256 * 1024) return fail("text");
                    f.bytes.assign(txt.begin(), txt.end());
                    f.name = n + (fs[i]["ext"].asString() == "txt" ? ".txt" : ".lsl");
                }
                files->push_back(f);
            }
            if (files->empty()) return fail("files");
            std::string first = cleanName(args["name"].asString());
            if (first.empty()) first = "route";
            sS.picking = true;
            LLFilePickerReplyThread::startPicker(
                [files](const std::vector<std::string>& picked, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
                {
                    sS.picking = false;
                    if (picked.empty()) return;
                    // the folder of the file chosen: every file of the route goes there
                    const std::string dir = gDirUtilp->getDirName(picked[0]);
                    S32 n = 0;
                    for (const OutFile& f : *files)
                    {
                        const std::string path = dir + gDirUtilp->getDirDelimiter() + f.name;
                        std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
                        if (!out.good()) continue;
                        out.write((const char*)f.bytes.data(), f.bytes.size());
                        if (out.good()) ++n;
                    }
                    push(LLSD().with("type", "studio_route_saved").with("ok", n == (S32)files->size()).with("dir", dir).with("n", n));
                },
                LLFilePicker::FFSAVE_ALL, first + " mover.lsl",
                [](const std::vector<std::string>&, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter) { sS.picking = false; });
            return result.with("files", (S32)files->size());
        }
        if (cmd == "studio_import")
        {
            if (sS.picking) return fail("busy");
            sS.picking = true;
            LLFilePickerReplyThread::startPicker(
                [](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
                {
                    sS.picking = false;
                    if (files.empty()) return;
                    push(importFile(files[0]));
                },
                LLFilePicker::FFLOAD_ALL, false,
                [](const std::vector<std::string>&, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter) { sS.picking = false; });
            return result;
        }
        if (cmd == "studio_world")
        {
            const LLUUID asset = args["asset"].asUUID();
            if (asset.isNull()) return fail("asset");
            const bool on = args["on"].asBoolean();
            gAgent.sendAnimationRequest(asset, on ? ANIM_REQUEST_START : ANIM_REQUEST_STOP);
            if (on) sS.worldPlaying[asset.asString()] = asset; else sS.worldPlaying.erase(asset.asString());
            return result.with("on", on);
        }
        return fail("unknown");
    }

    void idle()
    {
        if (sRec.on && isAgentAvatarValid())
        {
            const F32 t = sRec.timer.getElapsedTimeF32();
            // 1..5 pressed during the take: an action there (the page turns it into a clip)
            if (gKeyboard)
                for (S32 k = 0; k < 5; ++k)
                {
                    const bool dn = gKeyboard->getKeyDown((KEY)('1' + k));
                    if (dn && !sRec.keyWas[k] && sRec.marks.size() < 400) { sRec.marks.push_back(t); sRec.marks.push_back((F32)(k + 1)); }
                    sRec.keyWas[k] = dn;
                }
            if (t >= sRec.next && sRec.samples.size() < 6 * 1200)
            {
                sRec.next = t + 0.1f;
                LLJoint* root = gAgentAvatarp->getRootJoint();
                LLVector3 o = root ? root->getWorldPosition() : gAgentAvatarp->getPositionAgent();
                o.mV[VZ] -= gAgentAvatarp->getPelvisToFoot();
                const LLVector3 l = (o - sRec.o) * ~sRec.r;
                LLVector3 fwd = LLVector3::x_axis * ((root ? root->getWorldRotation() : gAgentAvatarp->getRotation()) * ~sRec.r);
                const F32 yaw = atan2f(fwd.mV[VY], fwd.mV[VX]) * RAD_TO_DEG;
                sRec.samples.insert(sRec.samples.end(), { t, l.mV[0], l.mV[1], l.mV[2], yaw, gAgent.getFlying() ? 1.f : 0.f });
            }
        }
        if (!sS.open || !isAgentAvatarValid()) return;
        if (sFree.on) stepFree();
        // the page stopped talking (another app, a reload): let the avatar go
        if (sTL.beat.getElapsedTimeF32() > 10.f) { LL_INFOS("MGStudio") << "Animation Studio: no page for 10 s, closing" << LL_ENDL; closeAll(true); return; }
        // a ring or an arrow let go: the page reads the pose (and keys it when Auto key is on)
        const bool d = MGPhonePoser::isDragging();
        if (d && !sS.wasDragging) { pushPoseBegin(); captureLockRefs(MGPhonePoser::selectedJoint()); }
        if (d) keepLocks();
        if (sS.wasDragging && !d) { sLockRefs.clear(); push(LLSD().with("type", "studio_posed").with("joint", MGPhonePoser::selectedJoint())); }
        sS.wasDragging = d;
        // a change from the page (body position, a joint's sliders): the locked hands / feet kept a few frames
        if (sRelock > 0 && !d && !sIK.on)
        {
            keepLocks();
            if (--sRelock == 0) { sLockRefs.clear(); push(LLSD().with("type", "studio_posed").with("joint", "mPelvis").with("relock", true)); }
        }
        if (!sS.oldLocals.empty() && sS.cleanTimer.getElapsedTimeF32() > 2.f)
        {
            sS.cleanTimer.reset();
            for (auto it = sS.oldLocals.begin(); it != sS.oldLocals.end();)
            {
                if (gAgentAvatarp->isMotionActive(*it)) { ++it; continue; }
                gAgentAvatarp->removeMotion(*it);
                LLKeyframeDataCache::removeKeyframeData(*it);
                it = sS.oldLocals.erase(it);
            }
        }
    }

    void onClose()
    {
        if (!isAgentAvatarValid()) return;
        // uploaded animations played for everyone stop with the phone
        for (const auto& kv : sS.worldPlaying) gAgent.sendAnimationRequest(kv.second, ANIM_REQUEST_STOP);
        sS.worldPlaying.clear();
        closeAll(false);
    }
}
