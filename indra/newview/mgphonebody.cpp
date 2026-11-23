/**
 * @file mgphonebody.cpp
 * @brief <MG> MG Phone Body Studio (see mgphonebody.h): shape / physics sliders, bone morphs,
 *        big size and inflation as standard animations, visible on every viewer.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * MasterGate Viewer, based on the Firestorm Viewer and the Second Life Viewer.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "mgphonebody.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <vector>

#include "llagent.h"
#include "llagentbenefits.h"
#include "llagentcamera.h"
#include "llagentwearables.h"
#include "llanimationstates.h"
#include "llappearancemgr.h"
#include "lldatapacker.h"
#include "lldir.h"
#include "llfloaterperms.h"
#include "llfloaterreg.h"
#include "llinventorymodel.h"
#include "lljoint.h"
#include "llkeyframemotion.h"
#include "llmodel.h"
#include "llquantize.h"
#include "llsdserialize.h"
#include "llviewerassetupload.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewermenufile.h"
#include "llviewervisualparam.h"
#include "llviewerwearable.h"
#include "llvoavatarself.h"
#include "llvovolume.h"
#include "mgfloaterphone.h"

namespace
{
    void push(const LLSD& ev)
    {
        MGFloaterPhone* phone = LLFloaterReg::findTypedInstance<MGFloaterPhone>("mg_phone");
        if (phone) phone->pushToPage(ev);
    }

    // the bones Body Studio may move: collision volumes (fitted mesh bodies) and Bento face bones (heads)
    const char* const MORPH_JOINTS[] = {
        "PELVIS", "BUTT", "BELLY", "LEFT_HANDLE", "RIGHT_HANDLE", "LOWER_BACK", "CHEST", "LEFT_PEC", "RIGHT_PEC", "UPPER_BACK", "NECK",
        "L_CLAVICLE", "R_CLAVICLE", "L_UPPER_ARM", "R_UPPER_ARM", "L_LOWER_ARM", "R_LOWER_ARM",
        "L_UPPER_LEG", "R_UPPER_LEG", "L_LOWER_LEG", "R_LOWER_LEG",
        "mFaceEyebrowOuterLeft", "mFaceEyebrowCenterLeft", "mFaceEyebrowInnerLeft", "mFaceEyebrowOuterRight", "mFaceEyebrowCenterRight", "mFaceEyebrowInnerRight",
        "mFaceEar1Left", "mFaceEar2Left", "mFaceEar1Right", "mFaceEar2Right", "mFaceNoseLeft", "mFaceNoseCenter", "mFaceNoseRight", "mFaceNoseBase", "mFaceNoseBridge",
        "mFaceCheekLowerLeft", "mFaceCheekUpperLeft", "mFaceCheekLowerRight", "mFaceCheekUpperRight", "mFaceJaw", "mFaceChin", "mFaceJawShaper",
        "mFaceLipCornerLeft", "mFaceLipCornerRight" };

    struct State
    {
        // the shape's own bone positions (an animation's position replaces them, so each one carries them)
        std::map<std::string, LLVector3> base;
        bool baseOk = false;
        bool baseDirty = true;
        bool touched = false;                 // the skeleton was moved on this screen (restored on close)
        LLUUID local;                         // the morph previewed on this screen (a local animation)
        std::vector<LLUUID> oldLocals;        // stopped previews, removed once eased out
        LLFrameTimer cleanTimer;
        bool pose = false;
        // published (uploaded) animations
        std::string pubName;
        std::vector<LLUUID> assets;           // [0] neutral (the shape alone), [1] level 0 (the morph), [k+1] level k
        std::vector<std::string> names;
        S32 pending = 0;
        bool failed = false;
        S32 playing = -1;                     // index in assets
        LLUUID stopLater;                     // the neutral one, stopped once it has eased in
        LLFrameTimer stopTimer;
        F32 stopAfter = 0.f;
        LLSD file;                            // what is kept on this computer (snapshots, published)
        bool fileRead = false;
    } sB;

    std::string filePath() { return gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "mg_body_studio.xml"); }

    LLSD& stored()
    {
        if (!sB.fileRead)
        {
            sB.fileRead = true;
            std::ifstream in(filePath().c_str(), std::ios::binary);
            if (in.good()) LLSDSerialize::fromXMLDocument(sB.file, in);
            if (!sB.file.isMap()) sB.file = LLSD::emptyMap();
            const LLSD& p = sB.file["published"];
            if (p.isMap())
            {
                sB.pubName = p["name"].asString();
                for (LLSD::array_const_iterator it = p["assets"].beginArray(); it != p["assets"].endArray(); ++it) sB.assets.push_back(it->asUUID());
                for (LLSD::array_const_iterator it = p["names"].beginArray(); it != p["names"].endArray(); ++it) sB.names.push_back(it->asString());
            }
        }
        return sB.file;
    }

    void writeStored()
    {
        std::ofstream out(filePath().c_str(), std::ios::binary | std::ios::trunc);
        if (out.good()) LLSDSerialize::toPrettyXML(sB.file, out);
    }

    LLWearableType::EType typeOf(const std::string& s) { return s == "physics" ? LLWearableType::WT_PHYSICS : LLWearableType::WT_SHAPE; }
    LLViewerWearable* worn(LLWearableType::EType t) { return gAgentWearables.getViewerWearable(t, 0); }

    // the sliders of a worn wearable, as the appearance editor shows them (0..100 over each slider's range)
    LLSD paramsOf(LLViewerWearable* w)
    {
        LLSD out = LLSD::emptyArray();
        if (!w || !isAgentAvatarValid()) return out;
        const ESex sex = gAgentAvatarp->getSex();
        LLWearable::visual_param_vec_t list;
        w->getVisualParams(list);
        for (LLVisualParam* vp : list)
        {
            LLViewerVisualParam* p = dynamic_cast<LLViewerVisualParam*>(vp);
            if (!p || p->getGroup() != VISUAL_PARAM_GROUP_TWEAKABLE) continue;
            if (!(p->getSex() & sex)) continue;
            const F32 mn = p->getMinWeight(), mx = p->getMaxWeight();
            const F32 wgt = w->getVisualParamWeight(p->getID());
            const F32 pct = mx > mn ? (wgt - mn) / (mx - mn) * 100.f : 0.f;
            out.append(LLSD().with("id", p->getID()).with("label", p->getDisplayName()).with("name", p->getName()).with("group", p->getEditGroup())
                .with("value", ll_round(pct * 10.f) / 10.0).with("lo", p->getMinDisplayName()).with("hi", p->getMaxDisplayName()));
        }
        return out;
    }

    bool setValues(LLViewerWearable* w, const LLSD& values)
    {
        if (!w || !values.isMap() || !isAgentAvatarValid()) return false;
        bool any = false;
        for (LLSD::map_const_iterator it = values.beginMap(); it != values.endMap(); ++it)
        {
            const S32 id = std::atoi(it->first.c_str());
            LLViewerVisualParam* p = dynamic_cast<LLViewerVisualParam*>(w->getVisualParam(id));
            if (!p || p->getGroup() != VISUAL_PARAM_GROUP_TWEAKABLE) continue;
            const F32 pct = llclamp((F32)it->second.asReal(), 0.f, 100.f);
            const F32 wgt = p->getMinWeight() + (p->getMaxWeight() - p->getMinWeight()) * pct / 100.f;
            w->setVisualParamWeight(id, wgt, false);
            any = true;
        }
        if (any)
        {
            // as the appearance editor's sliders do
            w->writeToAvatar(gAgentAvatarp);
            gAgentAvatarp->updateVisualParams();
        }
        return any;
    }

    void reapplyWearables()
    {
        for (LLWearableType::EType t : { LLWearableType::WT_SHAPE, LLWearableType::WT_PHYSICS })
            if (LLViewerWearable* w = worn(t)) w->writeToAvatar(gAgentAvatarp);
        gAgentAvatarp->updateVisualParams();
    }

    F32 heightM() { return isAgentAvatarValid() ? gAgentAvatarp->mBodySize.mV[VZ] + 0.195f : 0.f; }

    // the bodies worn: a fitted mesh body (its collision volumes move it), a Bento head (face bones)
    void bodyFlags(bool& fitted, bool& bento)
    {
        fitted = bento = false;
        if (!isAgentAvatarValid()) return;
        for (const auto& kv : gAgentAvatarp->mAttachmentPoints)
        {
            LLViewerJointAttachment* att = kv.second;
            if (!att || att->getIsHUDAttachment()) continue;
            for (const auto& ao : att->mAttachedObjects)
            {
                LLViewerObject* root = ao.get();
                if (!root || root->isDead()) continue;
                std::vector<LLViewerObject*> prims{ root };
                for (LLViewerObject* c : root->getChildren()) prims.push_back(c);
                for (LLViewerObject* o : prims)
                {
                    if (!o || o->getPCode() != LL_PCODE_VOLUME) continue;
                    LLVOVolume* v = static_cast<LLVOVolume*>(o);
                    if (!v->isRiggedMesh()) continue;
                    const LLMeshSkinInfo* skin = v->getSkinInfo();
                    if (!skin) continue;
                    for (const auto& j : skin->mJointNames)
                    {
                        if (j == "LEFT_PEC" || j == "BELLY" || j == "BUTT") fitted = true;
                        if (j.rfind("mFace", 0) == 0) bento = true;
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // bone morphs as animations
    // ------------------------------------------------------------------
    void stopLocal()
    {
        if (sB.local.notNull() && isAgentAvatarValid())
        {
            gAgentAvatarp->stopMotion(sB.local, true);
            sB.oldLocals.push_back(sB.local);
        }
        sB.local.setNull();
    }

    // the shape's own bone positions: our preview off, the skeleton rebuilt (it comes back with the
    // last appearance the server sent, so the worn sliders are written back), then read
    void captureBase()
    {
        if (!isAgentAvatarValid()) return;
        stopLocal();
        gAgentAvatarp->resetSkeleton(false);
        reapplyWearables();
        sB.base.clear();
        for (const char* n : MORPH_JOINTS)
            if (LLJoint* j = gAgentAvatarp->getJoint(n)) sB.base[n] = j->getPosition();
        sB.baseOk = !sB.base.empty();
        sB.baseDirty = false;
    }

    std::map<std::string, LLVector3> vecs(const LLSD& m)
    {
        std::map<std::string, LLVector3> out;
        if (!m.isMap()) return out;
        for (LLSD::map_const_iterator it = m.beginMap(); it != m.endMap(); ++it)
        {
            const LLSD& v = it->second;
            if (!v.isArray() || v.size() < 3 || !sB.base.count(it->first)) continue;
            LLVector3 d((F32)v[0].asReal(), (F32)v[1].asReal(), (F32)v[2].asReal());
            for (S32 k = 0; k < 3; ++k) d.mV[k] = llclamp(d.mV[k], -0.6f, 0.6f);   // 60 cm at most per bone
            out[it->first] = d;
        }
        return out;
    }

    // one animation: every bone at base + morph + inflation x level; when it starts, the viewer blends
    // in over `speed` seconds (ease in) from wherever the bones are, with a little overshoot if bounce
    std::vector<U8> buildAnim(const std::map<std::string, LLVector3>& morph, const std::map<std::string, LLVector3>& inflate,
                              F32 level, F32 speed, bool bounce, bool neutral)
    {
        std::set<std::string> names;
        for (const auto& kv : morph) names.insert(kv.first);
        for (const auto& kv : inflate) names.insert(kv.first);
        speed = llclamp(speed, 0.2f, 10.f);
        struct K { F32 t, f; };
        std::vector<K> keys;
        F32 dur, loopIn;
        if (bounce && level > 0.f && !neutral)
        {
            dur = speed + 0.8f; loopIn = speed + 0.5f;
            keys = { { 0.f, 0.92f }, { speed, 1.10f }, { speed + 0.25f, 0.96f }, { speed + 0.5f, 1.f }, { dur, 1.f } };
        }
        else
        {
            dur = 1.f; loopIn = 0.f;
            keys = { { 0.f, 1.f }, { dur, 1.f } };
        }
        const S32 PRIORITY = 4;
        size_t size = 128;
        for (const auto& n : names) size += n.size() + 1 + 16 + keys.size() * 8;
        std::vector<U8> buf(size);
        LLDataPackerBinaryBuffer dp(buf.data(), (S32)buf.size());
        dp.packU16(KEYFRAME_MOTION_VERSION, "version");
        dp.packU16(KEYFRAME_MOTION_SUBVERSION, "sub_version");
        dp.packS32(PRIORITY, "base_priority");
        dp.packF32(dur, "duration");
        dp.packString(std::string(), "emote_name");
        dp.packF32(loopIn, "loop_in_point");
        dp.packF32(dur, "loop_out_point");
        dp.packS32(1, "loop");
        dp.packF32(speed, "ease_in_duration");
        dp.packF32(speed, "ease_out_duration");
        dp.packU32(0, "hand_pose");   // HAND_POSE_SPREAD: the hands are left alone
        dp.packU32((U32)names.size(), "num_joints");
        for (const auto& n : names)
        {
            const LLVector3 base = sB.base[n];
            const auto mi = morph.find(n);
            const auto ii = inflate.find(n);
            const LLVector3 m = (mi != morph.end() && !neutral) ? mi->second : LLVector3::zero;
            const LLVector3 inf = (ii != inflate.end() && !neutral) ? ii->second * level : LLVector3::zero;
            dp.packString(n, "joint_name");
            dp.packS32(PRIORITY, "joint_priority");
            dp.packS32(0, "num_rot_keys");
            dp.packS32((S32)keys.size(), "num_pos_keys");
            for (const K& k : keys)
            {
                const LLVector3 p = base + m + inf * k.f;
                dp.packU16(F32_to_U16(k.t, 0.f, dur), "time");
                dp.packU16(F32_to_U16(llclamp(p.mV[VX], -LL_MAX_PELVIS_OFFSET, LL_MAX_PELVIS_OFFSET), -LL_MAX_PELVIS_OFFSET, LL_MAX_PELVIS_OFFSET), "pos_x");
                dp.packU16(F32_to_U16(llclamp(p.mV[VY], -LL_MAX_PELVIS_OFFSET, LL_MAX_PELVIS_OFFSET), -LL_MAX_PELVIS_OFFSET, LL_MAX_PELVIS_OFFSET), "pos_y");
                dp.packU16(F32_to_U16(llclamp(p.mV[VZ], -LL_MAX_PELVIS_OFFSET, LL_MAX_PELVIS_OFFSET), -LL_MAX_PELVIS_OFFSET, LL_MAX_PELVIS_OFFSET), "pos_z");
            }
        }
        dp.packS32(0, "num_constraints");
        buf.resize(dp.getCurrentSize());
        return buf;
    }

    // on this screen only: the animation read from memory (as the uploader's preview does) and started
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
            return false;
        }
        if (sB.local.notNull())
        {
            gAgentAvatarp->stopMotion(sB.local);   // eases out while the new one eases in
            sB.oldLocals.push_back(sB.local);
        }
        gAgentAvatarp->startMotion(id);
        sB.local = id;
        sB.touched = true;
        return true;
    }

    // the skeleton as the worn shape makes it, on this screen (after previews)
    void restoreLocal()
    {
        if (!isAgentAvatarValid()) return;
        stopLocal();
        if (sB.touched)
        {
            gAgentAvatarp->resetSkeleton(false);
            reapplyWearables();
            sB.touched = false;
        }
    }

    LLSD publishedLsd()
    {
        LLSD a = LLSD::emptyArray();
        for (size_t i = 0; i < sB.names.size() && i < sB.assets.size(); ++i) a.append(LLSD().with("name", sB.names[i]).with("asset", sB.assets[i].asString()));
        return LLSD().with("name", sB.pubName).with("anims", a).with("playing", sB.playing).with("busy", sB.pending > 0);
    }

    void playPublished(S32 index)
    {
        if (index < 0 || index >= (S32)sB.assets.size()) return;
        restoreLocal();   // the preview gives way to what everyone sees
        gAgent.sendAnimationRequest(sB.assets[index], ANIM_REQUEST_START);
        if (sB.playing >= 0 && sB.playing < (S32)sB.assets.size() && sB.playing != index)
            gAgent.sendAnimationRequest(sB.assets[sB.playing], ANIM_REQUEST_STOP);
        sB.playing = index;
    }

    LLSD camera(const std::string& view, F32 turn)
    {
        if (!isAgentAvatarValid()) return LLSD().with("ok", false).with("error", "avatar");
        if (view == "reset") { gAgentCamera.resetView(); return LLSD().with("ok", true); }
        const char* jn = "mPelvis";
        F32 dist = 3.2f, up = 0.2f;
        if (view == "face") { jn = "mHead"; dist = 0.65f; up = 0.04f; }
        else if (view == "upper") { jn = "mChest"; dist = 1.3f; up = 0.1f; }
        else if (view == "legs") { jn = "mKneeLeft"; dist = 1.9f; up = 0.25f; }
        LLJoint* j = gAgentAvatarp->getJoint(jn);
        if (!j) return LLSD().with("ok", false).with("error", "joint");
        LLVector3 focus = j->getWorldPosition();
        if (view == "legs")
            if (LLJoint* r = gAgentAvatarp->getJoint("mKneeRight")) focus = (focus + r->getWorldPosition()) * 0.5f;
        LLVector3 fwd = LLVector3::x_axis * gAgentAvatarp->getRotation();
        fwd.mV[VZ] = 0.f;
        if (fwd.normVec() < 1e-4f) fwd = LLVector3::x_axis;
        const LLVector3 dir = fwd * LLQuaternion(turn * DEG_TO_RAD, LLVector3::z_axis);
        const LLVector3 cam = focus + dir * dist + LLVector3(0.f, 0.f, up);
        gAgentCamera.setCameraPosAndFocusGlobal(gAgent.getPosGlobalFromAgent(cam), gAgent.getPosGlobalFromAgent(focus), gAgent.getID());
        return LLSD().with("ok", true);
    }

    LLSD save(LLWearableType::EType t, bool asNew, const std::string& name)
    {
        LLSD r = LLSD().with("ok", false);
        LLViewerWearable* w = worn(t);
        if (!w || !isAgentAvatarValid()) return r.with("error", "not_worn");
        U32 index = 0;
        if (!gAgentWearables.getWearableIndex(w, index)) return r.with("error", "not_worn");
        if (!gAgentWearables.isWearableModifiable(t, index)) return r.with("error", "no_modify");
        // as the appearance editor saves (llpaneleditwearable.cpp)
        LLInventoryItem* link_item = nullptr;
        std::string description;
        LLInventoryModel::item_array_t links = LLAppearanceMgr::instance().findCOFItemLinks(w->getItemID());
        if (!links.empty())
        {
            link_item = links.at(0).get();
            if (link_item && link_item->getIsLinkType()) description = link_item->getActualDescription();
        }
        if (asNew)
        {
            const std::string nm = name.empty() ? std::string("Body Studio shape") : name.substr(0, 63);
            LLAppearanceMgr::instance().removeCOFItemLinks(w->getItemID(), gAgentAvatarp->mEndCustomizeCallback);
            gAgentWearables.saveWearableAs(t, index, nm, description, false);
        }
        else
        {
            if (link_item)
            {
                LLInventoryObject::const_object_list_t obj_array;
                obj_array.push_back(LLConstPointer<LLInventoryObject>(link_item));
                link_inventory_array(LLAppearanceMgr::instance().getCOF(), obj_array, gAgentAvatarp->mEndCustomizeCallback);
                remove_inventory_item(link_item->getUUID(), gAgentAvatarp->mEndCustomizeCallback);
            }
            gAgentWearables.saveWearable(t, index, true, name.substr(0, 63));
        }
        return r.with("ok", true);
    }

    LLSD wearableLsd(LLWearableType::EType t)
    {
        LLViewerWearable* w = worn(t);
        LLSD o = LLSD().with("worn", w != nullptr);
        if (!w) return o;
        U32 index = 0;
        const bool idx = gAgentWearables.getWearableIndex(w, index);
        LLViewerInventoryItem* item = gInventory.getItem(w->getItemID());
        o["name"] = item ? item->getName() : w->getName();
        o["editable"] = idx && gAgentWearables.isWearableModifiable(t, index);
        o["dirty"] = w->isDirty();
        o["params"] = paramsOf(w);
        return o;
    }
}

namespace MGPhoneBody
{
    bool isCommand(const std::string& cmd) { return cmd.rfind("body_", 0) == 0; }

    LLSD runCommand(const std::string& cmd, const LLSD& args)
    {
        LLSD result = LLSD().with("ok", true);
        if (!isAgentAvatarValid()) return result.with("ok", false).with("error", "avatar");
        if (cmd == "body_state")
        {
            stored();
            bool fitted = false, bento = false;
            bodyFlags(fitted, bento);
            result["shape"] = wearableLsd(LLWearableType::WT_SHAPE);
            result["physics"] = wearableLsd(LLWearableType::WT_PHYSICS);
            result["female"] = gAgentAvatarp->getSex() == SEX_FEMALE;
            result["height"] = heightM();
            result["fitted"] = fitted;
            result["bento"] = bento;
            result["published"] = publishedLsd();
            LLSD snaps = LLSD::emptyArray();
            const LLSD& s = stored()["snaps"];
            for (LLSD::map_const_iterator it = s.beginMap(); it != s.endMap(); ++it) snaps.append(LLSD().with("name", it->first).with("values", it->second));
            result["snaps"] = snaps;
            result["anim_cost"] = LLAgentBenefitsMgr::current().getAnimationUploadCost();
            result["pose"] = sB.pose;
            return result;
        }
        if (cmd == "body_set")
        {
            const LLWearableType::EType t = typeOf(args["type"].asString());
            LLViewerWearable* w = worn(t);
            if (!w) return result.with("ok", false).with("error", "not_worn");
            if (!setValues(w, args["values"])) return result.with("ok", false).with("error", "values");
            if (t == LLWearableType::WT_SHAPE) sB.baseDirty = true;   // the bones' own positions changed
            return result.with("height", heightM()).with("dirty", w->isDirty());
        }
        if (cmd == "body_save") return save(typeOf(args["type"].asString()), args["as_new"].asBoolean(), args["name"].asString());
        if (cmd == "body_revert")
        {
            const LLWearableType::EType t = typeOf(args["type"].asString());
            LLViewerWearable* w = worn(t);
            if (!w) return result.with("ok", false).with("error", "not_worn");
            w->revertValues();
            w->writeToAvatar(gAgentAvatarp);
            gAgentAvatarp->updateVisualParams();
            if (t == LLWearableType::WT_SHAPE) sB.baseDirty = true;
            return result.with("params", paramsOf(w)).with("height", heightM());
        }
        if (cmd == "body_camera") return camera(args["view"].asString(), (F32)args["turn"].asReal());
        if (cmd == "body_pose")
        {
            const bool on = args["on"].asBoolean();
            if (on && !sB.pose) gAgentAvatarp->startMotion(ANIM_AGENT_CUSTOMIZE);
            if (!on && sB.pose) gAgentAvatarp->stopMotion(ANIM_AGENT_CUSTOMIZE);
            sB.pose = on;
            return result.with("pose", sB.pose);
        }
        if (cmd == "body_morph")
        {
            // preview: the morph and the inflation at a level, on this screen only
            if (args["off"].asBoolean()) { restoreLocal(); return result; }
            if (!sB.baseOk || sB.baseDirty) captureBase();
            if (!sB.baseOk) return result.with("ok", false).with("error", "skeleton");
            const auto morph = vecs(args["bones"]);
            const auto inflate = vecs(args["inflate"]);
            if (morph.empty() && inflate.empty()) { restoreLocal(); return result; }
            const std::vector<U8> bytes = buildAnim(morph, inflate, llclamp((F32)args["level"].asReal(), 0.f, 1.f),
                                                    args.has("speed") ? (F32)args["speed"].asReal() : 1.5f, args["bounce"].asBoolean(), false);
            if (!playLocal(bytes)) return result.with("ok", false).with("error", "anim");
            return result.with("bytes", (S32)bytes.size());
        }
        if (cmd == "body_publish")
        {
            if (sB.pending > 0) return result.with("ok", false).with("error", "busy");
            if (!sB.baseOk || sB.baseDirty) captureBase();
            if (!sB.baseOk) return result.with("ok", false).with("error", "skeleton");
            const auto morph = vecs(args["bones"]);
            const auto inflate = vecs(args["inflate"]);
            if (morph.empty() && inflate.empty()) return result.with("ok", false).with("error", "nothing");
            const S32 levels = inflate.empty() ? 0 : llclamp(args["levels"].asInteger(), 1, 10);
            const F32 speed = args.has("speed") ? (F32)args["speed"].asReal() : 1.5f;
            const bool bounce = args["bounce"].asBoolean();
            const std::string base = args["name"].asString().empty() ? std::string("Body Studio") : args["name"].asString().substr(0, 40);
            // [0] neutral (back to the shape alone), [1] the morph (level 0), then levels 1..N
            std::vector<std::pair<std::string, std::vector<U8>>> anims;
            anims.push_back({ base + " - off", buildAnim(morph, inflate, 0.f, speed, false, true) });
            anims.push_back({ base + " - level 0", buildAnim(morph, inflate, 0.f, speed, false, false) });
            for (S32 k = 1; k <= levels; ++k)
                anims.push_back({ base + llformat(" - level %d", k), buildAnim(morph, inflate, (F32)k / (F32)levels, speed, bounce, false) });
            sB.pubName = base;
            sB.assets.assign(anims.size(), LLUUID::null);
            sB.names.clear();
            for (const auto& a : anims) sB.names.push_back(a.first);
            sB.pending = (S32)anims.size();
            sB.failed = false;
            sB.playing = -1;
            const S32 cost = LLAgentBenefitsMgr::current().getAnimationUploadCost();
            for (size_t i = 0; i < anims.size(); ++i)
            {
                const size_t idx = i;
                LLNewBufferedResourceUploadInfo::uploadFinish_f finish = [idx](LLUUID asset, LLSD)
                {
                    if (idx < sB.assets.size()) sB.assets[idx] = asset;
                    if (--sB.pending > 0 || sB.failed) return;
                    LLSD pub = LLSD().with("name", sB.pubName);
                    LLSD as = LLSD::emptyArray(), ns = LLSD::emptyArray();
                    for (size_t k = 0; k < sB.assets.size(); ++k) { as.append(sB.assets[k]); ns.append(k < sB.names.size() ? sB.names[k] : std::string()); }
                    pub["assets"] = as;
                    pub["names"] = ns;
                    stored()["published"] = pub;
                    writeStored();
                    push(LLSD().with("type", "body_published").with("ok", true).with("published", publishedLsd()));
                };
                LLNewBufferedResourceUploadInfo::uploadFailure_f failure = [](LLUUID, LLSD, std::string reason)
                {
                    sB.failed = true;
                    --sB.pending;
                    push(LLSD().with("type", "body_published").with("ok", false).with("error", reason.empty() ? std::string("upload") : reason));
                    return true;
                };
                const std::string data((const char*)anims[i].second.data(), anims[i].second.size());
                LLResourceUploadInfo::ptr_t info(std::make_shared<LLNewBufferedResourceUploadInfo>(
                    data, LLUUID::generateNewID(), anims[i].first, std::string("Made with Body Studio (MasterGate Viewer)"), 0,
                    LLFolderType::FT_ANIMATION, LLInventoryType::IT_ANIMATION, LLAssetType::AT_ANIMATION,
                    LLFloaterPerms::getNextOwnerPerms("Uploads"), LLFloaterPerms::getGroupPerms("Uploads"), LLFloaterPerms::getEveryonePerms("Uploads"),
                    cost, LLUUID::null, false, finish, failure));
                upload_new_resource(info);
            }
            LL_INFOS("MGBody") << "Body Studio: uploading " << anims.size() << " animations (" << levels << " levels) as \"" << base << "\"" << LL_ENDL;
            return result.with("uploads", (S32)anims.size()).with("cost", cost * (S32)anims.size());
        }
        if (cmd == "body_play")
        {
            stored();
            const S32 level = args["level"].asInteger();
            if (sB.assets.empty() || sB.pending > 0) return result.with("ok", false).with("error", "not_published");
            if (level < 0)
            {
                // off for everyone: the neutral one eases the bones back to the shape, then stops
                if (sB.assets[0].isNull()) return result.with("ok", false).with("error", "not_published");
                playPublished(0);
                sB.stopLater = sB.assets[0];
                sB.stopTimer.reset();
                sB.stopAfter = 4.f;
                sB.playing = -1;
                return result.with("playing", -1);
            }
            const S32 idx = level + 1;   // level 0 = [1]
            if (idx >= (S32)sB.assets.size() || sB.assets[idx].isNull()) return result.with("ok", false).with("error", "level");
            sB.stopLater.setNull();
            playPublished(idx);
            return result.with("playing", level);
        }
        if (cmd == "body_snaps")
        {
            LLSD& f = stored();
            const std::string what = args["what"].asString(), name = args["name"].asString().substr(0, 40);
            if (!f["snaps"].isMap()) f["snaps"] = LLSD::emptyMap();
            if (what == "save" && !name.empty()) { f["snaps"][name] = args["values"]; writeStored(); }
            else if (what == "delete" && !name.empty()) { f["snaps"].erase(name); writeStored(); }
            LLSD snaps = LLSD::emptyArray();
            for (LLSD::map_const_iterator it = f["snaps"].beginMap(); it != f["snaps"].endMap(); ++it) snaps.append(LLSD().with("name", it->first).with("values", it->second));
            return result.with("snaps", snaps);
        }
        return result.with("ok", false).with("error", "unknown");
    }

    void idle()
    {
        if (!isAgentAvatarValid()) return;
        if (sB.stopLater.notNull() && sB.stopTimer.getElapsedTimeF32() > sB.stopAfter)
        {
            gAgent.sendAnimationRequest(sB.stopLater, ANIM_REQUEST_STOP);
            sB.stopLater.setNull();
        }
        if (!sB.oldLocals.empty() && sB.cleanTimer.getElapsedTimeF32() > 2.f)
        {
            sB.cleanTimer.reset();
            for (auto it = sB.oldLocals.begin(); it != sB.oldLocals.end();)
            {
                if (gAgentAvatarp->isMotionActive(*it)) { ++it; continue; }
                gAgentAvatarp->removeMotion(*it);
                LLKeyframeDataCache::removeKeyframeData(*it);
                it = sB.oldLocals.erase(it);
            }
        }
    }

    void onClose()
    {
        if (!isAgentAvatarValid()) return;
        if (sB.pose) { gAgentAvatarp->stopMotion(ANIM_AGENT_CUSTOMIZE); sB.pose = false; }
        restoreLocal();   // previews are on this screen only: the skeleton as the worn shape makes it
    }
}
