/**
 * @file mgphoneposer.cpp
 * @brief MasterGate Viewer: the MG Phone Camera's posing panel, driving Firestorm's poser.
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

#include "mgphoneposer.h"

#include "fsfloaterposer.h"
#include "fsposeranimator.h"
#include "fsposingmotion.h" // <MG/> Animation Studio: the joints' targets
#include "fsjointpose.h"    // <MG/>
#include <set>              // <MG/>
#include "lldir.h"
#include "lldiriterator.h"
#include "llframetimer.h"
#include "llfile.h"
#include "llfloaterreg.h"
#include "llrender2dutils.h"
#include "llrootview.h"
#include "llsdserialize.h"
#include "llviewercamera.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "lltoolcomp.h"
#include "lltoolmgr.h"

// FSFloaterPoser lets this class use its pose engine, axis settings and pose files
class MGPoserBridge
{
public:
    static FSFloaterPoser* poser()
    {
        // built once, never shown: its strings carry each joint's axis mapping
        return LLFloaterReg::getTypedInstance<FSFloaterPoser>("fs_poser");
    }
    static FSPoserAnimator& anim(FSFloaterPoser* f) { return f->mPoserAnimator; }
    static E_BoneAxisTranslation translation(FSFloaterPoser* f, E_PoserReferenceFrame frame, const std::string& joint)
    {
        return f->getJointTranslation(frame, joint);
    }
    static S32 negation(FSFloaterPoser* f, E_PoserReferenceFrame frame, const std::string& joint)
    {
        return f->getJointNegation(frame, joint);
    }
    static E_RotationStyle rotationStyle(FSFloaterPoser* f, const std::string& joint) { return f->getUiSelectedBoneRotationStyle(joint); }
    static bool couldAnimate(FSFloaterPoser* f, LLVOAvatar* av) { return f->couldAnimateAvatar(av); }
    static void ensureDirs(FSFloaterPoser* f) { f->createUserPoseDirectoryIfNeeded(); }
    static bool save(FSFloaterPoser* f, LLVOAvatar* av, const std::string& name) { return f->savePoseToXml(av, name); }
    static bool load(FSFloaterPoser* f, LLVOAvatar* av, const std::string& name, E_LoadPoseMethods how) { return f->loadPoseFromXml(av, name, how); }
    // the manip writes through the floater (updatePosedBones), which works on the avatar
    // selected in its list; a floater never shown has an empty list: fill it, then select
    static bool selectAvatar(FSFloaterPoser* f, const LLUUID& id)
    {
        // only when needed: a new selection makes the floater rebuild the gizmo from ITS
        // joint selection (setVisualManipulators), which would drop ours
        LLVOAvatar* sel = f->getUiSelectedAvatar();
        if (sel && sel->getID() == id) return true;
        f->onAvatarsRefresh();
        f->setUiSelectedAvatar(id);
        sel = f->getUiSelectedAvatar();
        return sel && sel->getID() == id;
    }
    // (the floater's own joint lists are only filled when it is shown: never drive them)
    template <class T> static bool dragging(T* tool) { return tool->mMouseDown && tool->mManip && tool->mManip->hasMouseCapture(); }

    // the gizmo: the page forwards the pointer (the viewfinder covers the window, so the
    // tool never sees the mouse); a press on a ring starts the drag the way the tool's own
    // pick callback does, and the manip then holds the mouse until it is released
    template <class T> static void hover(T* tool, S32 x, S32 y)
    {
        if (tool->mManip && !tool->mMouseDown) tool->mManip->highlightManipulators(x, y);
    }
    template <class T> static bool grab(T* tool, S32 x, S32 y)
    {
        if (!tool->mManip) return false;
        tool->mManip->highlightManipulators(x, y);
        if (tool->mManip->getHighlightedPart() == LLManip::LL_NO_PART) return false;
        tool->mMouseDown = true;
        tool->setCurrentTool(tool->mManip);
        tool->mManip->handleMouseDownOnPart(x, y, MASK_NONE);
        return true;
    }
};

namespace
{
    std::string sGizmo = "off";        // off | rotate | move
    LLToolset* sGizmoLastToolset = nullptr;
    void gizmoJoint();
    void setGizmo(const std::string& mode, E_PoserReferenceFrame frame);

    bool sMarkers = false;
    bool sCameraPosed = false;   // the camera started the posing: closing it lets the avatar go
    std::string sMarkerSel;
    S32 sMarkerGroup = BODY;

    LLVOAvatar* me()
    {
        return isAgentAvatarValid() ? (LLVOAvatar*)gAgentAvatarp.get() : nullptr;
    }

    E_PoserReferenceFrame frameOf(const LLSD& args)
    {
        const std::string f = args["frame"].asString();
        if (f == "world") return POSER_FRAME_WORLD;
        if (f == "avatar") return POSER_FRAME_AVATAR;
        if (f == "camera") return POSER_FRAME_CAMERA;
        return POSER_FRAME_BONE;
    }

    E_BoneDeflectionStyles styleOf(const LLSD& args)
    {
        const std::string s = args["style"].asString();
        if (s == "mirror") return MIRROR;
        if (s == "sym") return SYMPATHETIC;
        return NONE;
    }

    const char* groupName(E_BoneTypes t)
    {
        switch (t)
        {
            case WHOLEAVATAR: return "whole";
            case BODY: return "body";
            case FACE: return "face";
            case HANDS: return "hands";
            case MISC: return "misc";
            case COL_VOLUMES: return "volumes";
        }
        return "misc";
    }

    LLSD vec(const LLVector3& v, F32 k = 1.f)
    {
        LLSD a = LLSD::emptyArray();
        a.append(v.mV[VX] * k); a.append(v.mV[VY] * k); a.append(v.mV[VZ] * k);
        return a;
    }

    LLVector3 vecOf(const LLSD& a, F32 k = 1.f)
    {
        return LLVector3((F32)a[0].asReal() * k, (F32)a[1].asReal() * k, (F32)a[2].asReal() * k);
    }

    std::string posesDir(bool hands)
    {
        std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "poses");
        if (hands) dir = gDirUtilp->add(dir, "hand_presets");
        return dir;
    }

    LLSD listPoses(bool hands)
    {
        LLSD out = LLSD::emptyArray();
        const std::string dir = posesDir(hands);
        if (!LLFile::isdir(dir)) return out;
        std::string file;
        LLDirIterator it(dir, "*.xml");
        while (it.next(file))
        {
            const std::string path = gDirUtilp->add(dir, file);
            llstat st;
            const F64 mtime = LLFile::stat(path, &st) ? 0.0 : (F64)st.st_mtime;
            out.append(LLSD().with("name", gDirUtilp->getBaseFileName(path, true)).with("mtime", mtime));
        }
        return out;
    }

    std::string cleanName(std::string s)
    {
        static const std::string bad = "\\/:*?\"<>|";
        for (char& c : s)
        {
            if (bad.find(c) != std::string::npos || (unsigned char)c < 32) c = '_';
        }
        LLStringUtil::trim(s);
        return s.substr(0, 80);
    }

    // the gizmo follows the joint chosen in the panel or on the avatar
    void gizmoJoint()
    {
        if (sGizmo == "off" || !isAgentAvatarValid()) return;
        LLJoint* j = sMarkerSel.empty() ? nullptr : gAgentAvatarp->getJoint(sMarkerSel);
        FSToolCompPose::getInstance()->setAvatar(gAgentAvatarp);
        FSToolCompPoseTranslate::getInstance()->setAvatar(gAgentAvatarp);
        FSToolCompPose::getInstance()->setJoint(j);
        FSToolCompPoseTranslate::getInstance()->setJoint(j);
    }

    void setGizmo(const std::string& mode, E_PoserReferenceFrame frame)
    {
        LLToolMgr* tm = LLToolMgr::getInstance();
        if (mode != "rotate" && mode != "move")
        {
            if (sGizmo != "off")
            {
                FSToolCompPose::getInstance()->setAvatar(nullptr);
                FSToolCompPoseTranslate::getInstance()->setAvatar(nullptr);
                if (sGizmoLastToolset) tm->setCurrentToolset(sGizmoLastToolset);
                sGizmoLastToolset = nullptr;
            }
            sGizmo = "off";
            return;
        }
        LLVOAvatar* av = me();
        FSFloaterPoser* f = MGPoserBridge::poser();
        if (!av || !f) return;
        if (tm->getCurrentToolset() != gPoserToolset)
        {
            sGizmoLastToolset = tm->getCurrentToolset();
            tm->setCurrentToolset(gPoserToolset);
        }
        if (!MGPoserBridge::selectAvatar(f, av->getID())) // the manip writes through the floater's avatar
        {
            LL_WARNS("MGPhone") << "poser gizmo: the poser could not select my avatar" << LL_ENDL;
        }
        FSToolCompPose::getInstance()->setAvatar(av);
        FSToolCompPoseTranslate::getInstance()->setAvatar(av);
        FSToolCompPose::getInstance()->setReferenceFrame(frame);
        FSToolCompPoseTranslate::getInstance()->setReferenceFrame(frame);
        tm->getCurrentToolset()->selectTool(mode == "move" ? (LLTool*)FSToolCompPoseTranslate::getInstance() : (LLTool*)FSToolCompPose::getInstance());
        sGizmo = mode;
        gizmoJoint();
    }

    LLSD jointState(FSFloaterPoser* f, LLVOAvatar* av, const FSPoserAnimator::FSPoserJoint& j, E_PoserReferenceFrame frame)
    {
        FSPoserAnimator& a = MGPoserBridge::anim(f);
        const std::string name = j.jointName();
        LLSD s;
        s["ok"] = true;
        s["joint"] = name;
        s["rot"] = vec(a.getJointRotation(av, j, MGPoserBridge::translation(f, frame, name), MGPoserBridge::negation(f, frame, name)), RAD_TO_DEG);
        s["pos"] = vec(a.getJointPosition(av, j));
        s["scale"] = vec(a.getJointScale(av, j));
        s["posed"] = a.isPosingAvatarJoint(av, j);
        s["changed"] = a.hasJointBeenChanged(av, j);
        s["undo"] = a.canRedoOrUndoJointChange(av, j, true);
        s["redo"] = a.canRedoOrUndoJointChange(av, j, false);
        s["mirror"] = j.mirrorJointName();
        return s;
    }
}

namespace MGPhonePoser
{
    bool isPosing()
    {
        FSFloaterPoser* f = LLFloaterReg::findTypedInstance<FSFloaterPoser>("fs_poser");
        LLVOAvatar* av = me();
        return f && av && MGPoserBridge::anim(f).isPosingAvatar(av);
    }

    bool isCommand(const std::string& cmd)
    {
        return cmd.rfind("pose_", 0) == 0;
    }

    LLSD runCommand(const std::string& cmd, const LLSD& args)
    {
        LLSD result = LLSD().with("ok", true);
        FSFloaterPoser* f = MGPoserBridge::poser();
        LLVOAvatar* av = me();
        if (!f || !av)
        {
            return LLSD().with("ok", false).with("error", "no poser");
        }
        FSPoserAnimator& a = MGPoserBridge::anim(f);
        const E_PoserReferenceFrame frame = frameOf(args);

        if (cmd == "pose_state")
        {
            MGPoserBridge::ensureDirs(f);
            result["posing"] = a.isPosingAvatar(av);
            LLSD joints = LLSD::emptyArray();
            for (const FSPoserAnimator::FSPoserJoint& j : a.PoserJoints)
            {
                const std::string name = j.jointName();
                const std::string title = std::string("title_") + name;
                LLSD e;
                e["n"] = name;
                e["label"] = f->hasString(title) ? f->getString(title) : name;
                e["g"] = groupName(j.boneType());
                e["mirror"] = j.mirrorJointName();
                if (a.isPosingAvatar(av))
                {
                    e["changed"] = a.hasJointBeenChanged(av, j);
                }
                joints.append(e);
            }
            result["joints"] = joints;
            result["poses"] = listPoses(false);
            result["hands"] = listPoses(true);
            return result;
        }
        if (cmd == "pose_start")
        {
            if (!a.isPosingAvatar(av))
            {
                if (!MGPoserBridge::couldAnimate(f, av) || !a.tryPosingAvatar(av))
                {
                    return LLSD().with("ok", false).with("error", "cannot pose");
                }
                sCameraPosed = true;
            }
            result["posing"] = true;
            return result;
        }
        if (cmd == "pose_preset")
        {
            // <MG> Creator Lab: a T pose (every joint at the skeleton's rest, as rigged mesh is bound)
            // or an A pose (the same, arms down 45 degrees), to match a garment made in that pose
            if (!a.isPosingAvatar(av))
            {
                if (!MGPoserBridge::couldAnimate(f, av) || !a.tryPosingAvatar(av))
                {
                    return LLSD().with("ok", false).with("error", "cannot pose");
                }
                sCameraPosed = true;
            }
            for (const FSPoserAnimator::FSPoserJoint& j : a.PoserJoints)
            {
                if (!a.isPosingAvatarJoint(av, j)) a.setPosingAvatarJoint(av, j, true);
                a.resetJoint(av, j, NONE);
            }
            a.setAllAvatarStartingRotationsToZero(av);
            if (args["name"].asString() == "a")
            {
                // about the joint's own X axis (forward): the left arm (along +Y) down, the right one (along -Y) down
                const struct { const char* joint; F32 roll; } ARMS[] = { { "mShoulderLeft", -45.f }, { "mShoulderRight", 45.f } };
                for (const auto& arm : ARMS)
                {
                    const FSPoserAnimator::FSPoserJoint* pj = a.getPoserJointByName(arm.joint);
                    if (pj) a.loadJointRotation(av, pj, false, false, LLVector3(arm.roll * DEG_TO_RAD, 0.f, 0.f));
                }
            }
            result["posing"] = true;
            return result;
        }
        if (cmd == "pose_stop")
        {
            a.stopPosingAvatar(av);
            sCameraPosed = false;
            result["posing"] = false;
            return result;
        }
        if (cmd == "pose_files")
        {
            result["poses"] = listPoses(false);
            result["hands"] = listPoses(true);
            return result;
        }
        if (cmd == "pose_flip")
        {
            if (a.isPosingAvatar(av)) a.flipEntirePose(av);
            return result;
        }
        if (cmd == "pose_sym")
        {
            if (a.isPosingAvatar(av)) a.symmetrizeLeftToRightOrRightToLeft(av, args["dir"].asString() == "rl");
            return result;
        }
        if (cmd == "pose_save")
        {
            const std::string name = cleanName(args["name"].asString());
            if (name.empty() || !a.isPosingAvatar(av)) return LLSD().with("ok", false).with("error", "nothing to save");
            MGPoserBridge::ensureDirs(f);
            if (!MGPoserBridge::save(f, av, name)) return LLSD().with("ok", false).with("error", "save failed");
            result["name"] = name;
            result["poses"] = listPoses(false);
            return result;
        }
        if (cmd == "pose_delete")
        {
            const std::string name = cleanName(args["name"].asString());
            const std::string path = gDirUtilp->add(posesDir(false), name + ".xml");
            if (name.empty() || !LLFile::isfile(path) || LLFile::remove(path) != 0) return LLSD().with("ok", false).with("error", "not deleted");
            result["poses"] = listPoses(false);
            return result;
        }
        if (cmd == "pose_load")
        {
            const std::string name = cleanName(args["name"].asString());
            const std::string part = args["part"].asString();
            if (name.empty()) return LLSD().with("ok", false).with("error", "no name");
            if (!a.isPosingAvatar(av) && !(MGPoserBridge::couldAnimate(f, av) && a.tryPosingAvatar(av)))
            {
                return LLSD().with("ok", false).with("error", "cannot pose");
            }
            E_LoadPoseMethods how = ROT_POS_AND_SCALES;
            std::string file = name;
            if (part == "rot") how = ROTATIONS;
            else if (part == "face") how = FACE_ONLY;
            else if (part == "hand_l" || part == "hand_r")
            {
                how = (part == "hand_l") ? HAND_LEFT : HAND_RIGHT;
                file = std::string("hand_presets") + gDirUtilp->getDirDelimiter() + name;
            }
            if (!MGPoserBridge::load(f, av, file, how)) return LLSD().with("ok", false).with("error", "load failed");
            return result;
        }
        if (cmd == "pose_hand")
        {
            // a hand shape on one hand, as Firestorm's poser applies it (FSFloaterPoser::
            // onClickLoadHandPose): only that hand's finger rotations, the rest of the pose kept
            if (!a.isPosingAvatar(av)) return LLSD().with("ok", false).with("error", "not posing");
            const std::string name = cleanName(args["name"].asString());
            const bool right = args["side"].asString() == "r";
            const std::string path = gDirUtilp->add(posesDir(true), name + ".xml");
            llifstream in(path.c_str());
            if (name.empty() || !in.is_open()) return LLSD().with("ok", false).with("error", "no such hand shape");
            LLSD pose;
            if (LLSDSerialize::fromXML(pose, in) == LLSDParser::PARSE_FAILURE) return LLSD().with("ok", false).with("error", "bad file");
            S32 applied = 0;
            for (LLSD::map_const_iterator itr = pose.beginMap(); itr != pose.endMap(); ++itr)
            {
                const std::string& jn = itr->first;
                if (jn.find("Hand") == std::string::npos) continue;
                if (right != (jn.find("Left") == std::string::npos)) continue;
                const FSPoserAnimator::FSPoserJoint* pj = a.getPoserJointByName(jn);
                if (!pj || !itr->second.has("rotation")) continue;
                LLVector3 rot;
                rot.setValue(itr->second["rotation"]);
                a.loadJointRotation(av, pj, true, true, rot);
                ++applied;
            }
            result["applied"] = applied;
            return result;
        }
        if (cmd == "pose_multi")
        {
            // several joints at once (the face's expression sliders): {ops: [{joint, pos?, rot?}]}
            // rot = degrees about the joint's own axes (x forward, y left, z up: roll, pitch, yaw),
            // without the poser window's per-joint axis swaps; pos = offset in metres
            if (!a.isPosingAvatar(av)) return LLSD().with("ok", false).with("error", "not posing");
            const LLSD& ops = args["ops"];
            for (S32 i = 0; i < (S32)ops.size(); ++i)
            {
                const LLSD& op = ops[i];
                const FSPoserAnimator::FSPoserJoint* pj = a.getPoserJointByName(op["joint"].asString());
                if (!pj) continue;
                if (!a.isPosingAvatarJoint(av, *pj)) a.setPosingAvatarJoint(av, *pj, true);
                if (op.has("pos")) a.setJointPosition(av, pj, vecOf(op["pos"]), frame, NONE);
                if (op.has("rot")) a.loadJointRotation(av, pj, false, false, vecOf(op["rot"], DEG_TO_RAD));
            }
            return result;
        }
        if (cmd == "pose_base")
        {
            // the current position and rotation of several joints (the expression sliders' start)
            LLSD out = LLSD::emptyMap();
            const LLSD& joints = args["joints"];
            for (S32 i = 0; i < (S32)joints.size(); ++i)
            {
                const FSPoserAnimator::FSPoserJoint* pj = a.getPoserJointByName(joints[i].asString());
                if (!pj) continue;
                out[pj->jointName()] = LLSD().with("pos", vec(a.getJointPosition(av, *pj)))
                                             .with("rot", vec(a.getJointRotation(av, *pj, SWAP_NOTHING, NEGATE_NOTHING), RAD_TO_DEG));
            }
            result["joints"] = out;
            return result;
        }
        if (cmd == "pose_pick")
        {
            // the joint whose screen position is nearest the click (fractions of the window)
            LLView* root = gViewerWindow ? gViewerWindow->getRootView() : nullptr;
            if (!root) return LLSD().with("ok", false);
            const LLRect r = root->getLocalRect();
            const F32 px = (F32)args["x"].asReal() * r.getWidth();
            const F32 py = r.getHeight() - (F32)args["y"].asReal() * r.getHeight();
            const std::string group = args.has("group") ? args["group"].asString() : "body";
            LLViewerCamera* cam = LLViewerCamera::getInstance();
            F32 best = 48.f * 48.f;
            std::string pick;
            for (const FSPoserAnimator::FSPoserJoint& j : a.PoserJoints)
            {
                if (group != groupName(j.boneType())) continue;
                LLJoint* lj = av->getJoint(j.jointName());
                if (!lj) continue;
                LLCoordGL c;
                if (!cam->projectPosAgentToScreen(lj->getWorldPosition(), c, false)) continue;
                const F32 dx = c.mX - px, dy = c.mY - py, d2 = dx * dx + dy * dy;
                if (d2 < best) { best = d2; pick = j.jointName(); }
            }
            if (pick.empty()) return LLSD().with("ok", false).with("error", "no joint here");
            result["joint"] = pick;
            sMarkerSel = pick;
            gizmoJoint();
            return result;
        }
        if (cmd == "pose_gizmo_mode")
        {
            setGizmo(args["mode"].asString(), frame);
            result["mode"] = sGizmo;
            return result;
        }
        if (cmd == "pose_gizmo")
        {
            // the pointer from the page, in fractions of the window
            LLView* root = gViewerWindow ? gViewerWindow->getRootView() : nullptr;
            if (!root || sGizmo == "off") return LLSD().with("ok", false).with("grabbed", false);
            const LLRect r = root->getLocalRect();
            const S32 x = ll_round((F32)args["x"].asReal() * r.getWidth());
            const S32 y = ll_round((1.f - (F32)args["y"].asReal()) * r.getHeight());
            const std::string ev = args["ev"].asString();
            const bool move = (sGizmo == "move");
            bool grabbed = false;
            if (ev == "hover")
            {
                if (move) MGPoserBridge::hover(FSToolCompPoseTranslate::getInstance(), x, y);
                else MGPoserBridge::hover(FSToolCompPose::getInstance(), x, y);
            }
            else if (ev == "down")
            {
                // the poser's list again (an avatar arriving or a relog rebuilds it)
                if (FSFloaterPoser* f2 = MGPoserBridge::poser()) MGPoserBridge::selectAvatar(f2, av->getID());
                gizmoJoint(); // on the panel's joint, whatever the floater did meanwhile
                grabbed = move ? MGPoserBridge::grab(FSToolCompPoseTranslate::getInstance(), x, y)
                               : MGPoserBridge::grab(FSToolCompPose::getInstance(), x, y);
                LL_INFOS("MGPhone") << "poser gizmo: press at " << x << "," << y << (grabbed ? " grabbed a ring" : " missed the rings") << LL_ENDL;
            }
            else if (ev == "up")
            {
                LLTool* tool = move ? (LLTool*)FSToolCompPoseTranslate::getInstance() : (LLTool*)FSToolCompPose::getInstance();
                tool->handleMouseUp(x, y, MASK_NONE);
            }
            result["grabbed"] = grabbed;
            return result;
        }
        if (cmd == "pose_markers")
        {
            sMarkers = args["on"].asBoolean();
            if (args.has("sel"))
            {
                sMarkerSel = args["sel"].asString();
                gizmoJoint();   // <MG> the rings onto that joint (no tool change: a tool change drops a caller's transient tool)
            }
            const std::string g = args["group"].asString();
            sMarkerGroup = (g == "face") ? FACE : (g == "hands") ? HANDS : (g == "misc") ? MISC : BODY;
            return result;
        }

        // ---- one joint
        const FSPoserAnimator::FSPoserJoint* j = a.getPoserJointByName(args["joint"].asString());
        if (!j)
        {
            return LLSD().with("ok", false).with("error", "unknown joint");
        }
        if (!a.isPosingAvatar(av))
        {
            return LLSD().with("ok", false).with("error", "not posing");
        }
        if (sMarkerSel != j->jointName())
        {
            sMarkerSel = j->jointName();
            gizmoJoint();
        }
        if (cmd == "pose_get")
        {
            return jointState(f, av, *j, frame);
        }
        if (cmd == "pose_rot")
        {
            const std::string name = j->jointName();
            const E_BoneAxisTranslation tr = MGPoserBridge::translation(f, frame, name);
            const S32 ng = MGPoserBridge::negation(f, frame, name);
            const LLVector3 abs_rot = vecOf(args["rot"], DEG_TO_RAD);
            const LLVector3 delta = abs_rot - a.getJointRotation(av, *j, tr, ng);
            if (!a.isPosingAvatarJoint(av, *j)) a.setPosingAvatarJoint(av, *j, true);
            a.setJointRotation(av, j, abs_rot, delta, styleOf(args), frame, tr, ng, false, MGPoserBridge::rotationStyle(f, name));
            return jointState(f, av, *j, frame);
        }
        if (cmd == "pose_pos")
        {
            if (!a.isPosingAvatarJoint(av, *j)) a.setPosingAvatarJoint(av, *j, true);
            a.setJointPosition(av, j, vecOf(args["pos"]), frame, styleOf(args));
            return jointState(f, av, *j, frame);
        }
        if (cmd == "pose_scale")
        {
            if (!a.isPosingAvatarJoint(av, *j)) a.setPosingAvatarJoint(av, *j, true);
            a.setJointScale(av, j, vecOf(args["scale"]), frame, styleOf(args));
            return jointState(f, av, *j, frame);
        }
        if (cmd == "pose_reset")
        {
            a.resetJoint(av, *j, styleOf(args));
            return jointState(f, av, *j, frame);
        }
        if (cmd == "pose_undo")
        {
            a.undoLastJointChange(av, *j, styleOf(args));
            return jointState(f, av, *j, frame);
        }
        if (cmd == "pose_redo")
        {
            a.redoLastJointChange(av, *j, styleOf(args));
            return jointState(f, av, *j, frame);
        }
        if (cmd == "pose_reflect")
        {
            a.reflectJoint(av, j);
            return jointState(f, av, *j, frame);
        }
        if (cmd == "pose_enable")
        {
            a.setPosingAvatarJoint(av, *j, args["on"].asBoolean());
            return jointState(f, av, *j, frame);
        }
        return LLSD().with("ok", false).with("error", "unknown command");
    }

    void drawMarkers()
    {
        LLVOAvatar* av = me();
        if (!sMarkers || !av)
        {
            return;
        }
        FSFloaterPoser* f = LLFloaterReg::findTypedInstance<FSFloaterPoser>("fs_poser");
        if (!f)
        {
            return;
        }
        FSPoserAnimator& a = MGPoserBridge::anim(f);
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        for (const FSPoserAnimator::FSPoserJoint& j : a.PoserJoints)
        {
            if (j.boneType() != sMarkerGroup) continue;
            LLJoint* lj = av->getJoint(j.jointName());
            if (!lj) continue;
            LLCoordGL c;
            if (!cam->projectPosAgentToScreen(lj->getWorldPosition(), c, false)) continue;
            const bool sel = j.jointName() == sMarkerSel;
            const bool changed = a.isPosingAvatar(av) && a.hasJointBeenChanged(av, j);
            const F32 r = sel ? 7.f : (sMarkerGroup == BODY ? 5.f : 3.5f);
            gGL.color4f(0.f, 0.f, 0.f, 0.55f);
            gl_circle_2d((F32)c.mX, (F32)c.mY, r + 1.5f, 16, true);
            if (sel) gGL.color4f(0.16f, 0.83f, 0.91f, 1.f);
            else if (changed) gGL.color4f(1.f, 0.84f, 0.04f, 0.95f);
            else gGL.color4f(1.f, 1.f, 1.f, 0.9f);
            gl_circle_2d((F32)c.mX, (F32)c.mY, r, 16, true);
        }
    }

    void idle()
    {
        // the hidden floater may point the gizmo elsewhere (its selection is empty): keep it
        // on the panel's joint, never in the middle of a drag
        static LLFrameTimer timer;
        if (sGizmo == "off" || timer.getElapsedTimeF32() < 0.25f) return;
        timer.reset();
        if (MGPoserBridge::dragging(FSToolCompPose::getInstance()) || MGPoserBridge::dragging(FSToolCompPoseTranslate::getInstance())) return;
        gizmoJoint();
    }

    void restore()
    {
        setGizmo("off", POSER_FRAME_BONE);
        sMarkers = false;
        sMarkerSel.clear();
        // the camera closing: a pose it started ends, the animations play again (a pose
        // started in Firestorm's own poser window before the camera is left as it is)
        if (sCameraPosed)
        {
            sCameraPosed = false;
            FSFloaterPoser* f = MGPoserBridge::poser();
            LLVOAvatar* av = me();
            if (f && av && MGPoserBridge::anim(f).isPosingAvatar(av))
            {
                MGPoserBridge::anim(f).stopPosingAvatar(av);
            }
        }
    }

    // <MG> Animation Studio ------------------------------------------------------------
    // the pelvis (WHOLEAVATAR) counts as body; collision volumes are never animated here
    static std::string studioGroup(E_BoneTypes t)
    {
        const std::string g = groupName(t);
        return g == "whole" ? std::string("body") : g;
    }

    LLSD capturePose(const LLSD& groups)
    {
        LLSD out = LLSD::emptyMap();
        FSFloaterPoser* f = MGPoserBridge::poser();
        LLVOAvatar* av = me();
        if (!f || !av) return out;
        std::set<std::string> want;
        for (LLSD::array_const_iterator it = groups.beginArray(); it != groups.endArray(); ++it) want.insert(it->asString());
        if (want.empty()) want = { "body", "hands", "face" };
        FSPoserAnimator& a = MGPoserBridge::anim(f);
        FSPosingMotion* m = a.isPosingAvatar(av) ? a.mgPosingMotion(av) : nullptr;
        for (const FSPoserAnimator::FSPoserJoint& j : a.PoserJoints)
        {
            if (j.boneType() == COL_VOLUMES || !want.count(studioGroup(j.boneType()))) continue;
            LLJoint* lj = av->getJoint(j.jointName());
            if (!lj) continue;
            LLQuaternion q = lj->getRotation();
            LLVector3 p = lj->getPosition();
            FSJointPose* jp = m ? m->getJointPoseByJointName(j.jointName()) : nullptr;
            if (jp && a.isPosingAvatarJoint(av, j))
            {
                // the target, not the joint (the poser blends towards it over a few frames)
                q = jp->getTargetRotation();
                p = jp->getTargetPosition();
            }
            q.normalize();
            if (q.mQ[VW] < 0.f) q = -q;   // one sign for each rotation: keys compare and blend cleanly
            LLSD r = LLSD::emptyArray();
            for (S32 k = 0; k < 4; ++k) r.append(q.mQ[k]);
            out[j.jointName()] = LLSD().with("r", r).with("p", vec(p));
        }
        return out;
    }

    S32 applyPose(const LLSD& pose, bool snap)
    {
        FSFloaterPoser* f = MGPoserBridge::poser();
        LLVOAvatar* av = me();
        if (!f || !av || !pose.isMap()) return 0;
        FSPoserAnimator& a = MGPoserBridge::anim(f);
        if (!a.isPosingAvatar(av)) return 0;
        FSPosingMotion* m = a.mgPosingMotion(av);
        if (!m) return 0;
        S32 n = 0;
        for (LLSD::map_const_iterator it = pose.beginMap(); it != pose.endMap(); ++it)
        {
            const FSPoserAnimator::FSPoserJoint* pj = a.getPoserJointByName(it->first);
            if (!pj || pj->boneType() == COL_VOLUMES) continue;
            if (!a.isPosingAvatarJoint(av, *pj)) a.setPosingAvatarJoint(av, *pj, true);
            FSJointPose* jp = m->getJointPoseByJointName(it->first);
            if (!jp) continue;
            const LLSD& v = it->second;
            if (v.has("r") && v["r"].size() >= 4)
            {
                LLQuaternion q((F32)v["r"][0].asReal(), (F32)v["r"][1].asReal(), (F32)v["r"][2].asReal(), (F32)v["r"][3].asReal());
                if (!q.isFinite()) continue;
                q.normalize();
                // base zeroed: the target is this rotation itself (target = public x base)
                jp->setPublicRotation(true, false, POSER_CHANGE_DEFAULT, q);
                if (snap) jp->getJointState()->setRotation(q);
            }
            if (v.has("p") && v["p"].size() >= 3)
            {
                const LLVector3 want = vecOf(v["p"]);
                if (!want.isFinite()) continue;
                const LLVector3 base = jp->getTargetPosition() - jp->getPublicPosition();
                jp->setPublicPosition(want - base);
                if (snap) jp->getJointState()->setPosition(want);
            }
            jp->purgeUndoQueue();   // scrubbing is not an edit: the poser's undo stays the user's own
            ++n;
        }
        return n;
    }

    bool isDragging()
    {
        return MGPoserBridge::dragging(FSToolCompPose::getInstance()) || MGPoserBridge::dragging(FSToolCompPoseTranslate::getInstance());
    }

    std::string selectedJoint() { return sMarkerSel; }

    LLSD jointList()
    {
        LLSD list = LLSD::emptyArray();
        FSFloaterPoser* f = MGPoserBridge::poser();
        LLVOAvatar* av = me();
        if (!f) return list;
        FSPoserAnimator& a = MGPoserBridge::anim(f);
        for (const FSPoserAnimator::FSPoserJoint& j : a.PoserJoints)
        {
            if (j.boneType() == COL_VOLUMES) continue;
            const std::string name = j.jointName();
            const std::string title = std::string("title_") + name;
            LLJoint* lj = av ? av->getJoint(name) : nullptr;
            LLSD e;
            e["n"] = name;
            e["label"] = f->hasString(title) ? f->getString(title) : name;
            e["g"] = studioGroup(j.boneType());
            e["mirror"] = j.mirrorJointName();
            e["parent"] = (lj && lj->getParent()) ? lj->getParent()->getName() : std::string();
            e["has"] = lj != nullptr;
            list.append(e);
        }
        return list;
    }
}
