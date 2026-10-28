/**
 * @file mgphoneanims.cpp
 * @brief MasterGate Viewer: the MG Phone "Animations" app (animations + gestures).
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

#include "mgphoneanims.h"

#include <deque>
#include <map>
#include <set>

#include "aoengine.h"
#include "aoset.h"
#include "llagent.h"
#include "llanimationstates.h"
#include "llassetstorage.h"
#include "llbase64.h"
#include "llcallbacklist.h"
#include "lldatapacker.h"
#include "lldir.h"
#include "llfile.h"
#include "llfilesystem.h"
#include "llfloaterreg.h"
#include "llgesturemgr.h"
#include "llhandmotion.h"
#include "llimagejpeg.h"
#include "llinventorymodel.h"
#include "llkeyboard.h"
#include "llkeyframemotion.h"
#include "llmultigesture.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "lldrawpoolavatar.h"
#include "lldynamictexture.h"
#include "llface.h"
#include "llrender.h"
#include "llviewercamera.h"
#include "llviewerobjectlist.h"
#include "llviewershadermgr.h"
#include "pipeline.h"
#include "llwindow.h"
#include "mgfloaterphone.h"
#include "mgphonecamera.h"

namespace
{
    void push(const LLSD& ev)
    {
        MGFloaterPhone* phone = LLFloaterReg::findTypedInstance<MGFloaterPhone>("mg_phone");
        if (phone) phone->pushToPage(ev);
    }

    // an animation / gesture the avatar owns (not a link, not in the trash, not the Library)
    LLViewerInventoryItem* ownItem(const LLUUID& id, LLAssetType::EType type)
    {
        LLViewerInventoryItem* item = gInventory.getItem(id);
        if (!item || item->getIsLinkType() || item->getType() != type || item->getAssetUUID().isNull()) return nullptr;
        if (!gInventory.isObjectDescendentOf(id, gInventory.getRootFolderID())) return nullptr;
        return item;
    }

    // ---------------------------------------------------------------------
    // animation facts, read from the keyframe data
    // ---------------------------------------------------------------------
    std::map<LLUUID, LLSD> sAnimInfo;                       // by asset
    std::map<LLUUID, std::set<LLUUID>> sAnimWaiting;        // asset -> items asking

    LLSD infoFrom(const LLKeyframeMotion::JointMotionList* l, S32 size)
    {
        LLSD i;
        i["dur"] = l->mDuration;
        i["loop"] = l->mLoop;
        i["loopIn"] = l->mLoopInPoint;
        i["loopOut"] = l->mLoopOutPoint;
        i["easeIn"] = l->mEaseInDuration;
        i["easeOut"] = l->mEaseOutDuration;
        i["prio"] = (S32)l->mBasePriority;
        i["maxPrio"] = (S32)l->mMaxPriority;
        i["hand"] = LLHandMotion::getHandPoseName(l->mHandPose);
        i["emote"] = l->mEmoteName;
        i["joints"] = (S32)l->getNumJointMotions();
        if (size > 0) i["size"] = size;
        // which parts of the body it moves (Bento names: mFace*, mHand*, mTail*, mWing*, mHind*)
        bool face = false, hands = false, body = false, legs = false, tail = false, wings = false, moves = false;
        for (U32 j = 0; j < l->getNumJointMotions(); ++j)
        {
            const LLKeyframeMotion::JointMotion* jm = l->getJointMotion(j);
            const std::string& n = jm->mJointName;
            if (n.rfind("mFace", 0) == 0) face = true;
            else if (n.rfind("mHand", 0) == 0) hands = true;
            else if (n.rfind("mTail", 0) == 0) tail = true;
            else if (n.rfind("mWing", 0) == 0) wings = true;
            else
            {
                body = true;
                if (n.rfind("mHip", 0) == 0 || n.rfind("mKnee", 0) == 0 || n.rfind("mAnkle", 0) == 0 || n.rfind("mHind", 0) == 0) legs = true;
            }
            if (n == "mPelvis" && jm->mPositionCurve.mNumKeys > 1) moves = true;
        }
        i["parts"] = LLSD().with("face", face).with("hands", hands).with("body", body).with("legs", legs)
                           .with("tail", tail).with("wings", wings).with("moves", moves);
        return i;
    }

    void answerAnim(const LLUUID& asset, const LLSD& info, const std::string& error)
    {
        auto it = sAnimWaiting.find(asset);
        if (it == sAnimWaiting.end()) return;
        for (const LLUUID& item : it->second)
        {
            LLSD ev = LLSD().with("type", "anim_info").with("id", item.asString());
            if (error.empty()) ev["info"] = info;
            else ev["error"] = error;
            push(ev);
        }
        sAnimWaiting.erase(it);
    }

    void onAnimLoaded(const LLUUID& asset, LLAssetType::EType type, void*, S32 status, LLExtStat)
    {
        if (status != 0 || !isAgentAvatarValid())
        {
            answerAnim(asset, LLSD(), "could not load");
            return;
        }
        const LLKeyframeMotion::JointMotionList* list = LLKeyframeDataCache::getKeyframeData(asset);
        S32 size = 0;
        if (!list)
        {
            LLFileSystem file(asset, type, LLFileSystem::READ);
            size = file.getSize();
            if (size > 0)
            {
                std::vector<U8> buf(size);
                if (file.read(buf.data(), size))
                {
                    // parsed once into the shared keyframe cache (where playing it looks first)
                    LLDataPackerBinaryBuffer dp(buf.data(), size);
                    LLKeyframeMotion* motion = new LLKeyframeMotion(asset);
                    motion->setCharacter(gAgentAvatarp);
                    if (motion->deserialize(dp, asset)) list = LLKeyframeDataCache::getKeyframeData(asset);
                    delete motion;
                }
            }
        }
        if (!list)
        {
            answerAnim(asset, LLSD(), "not a valid animation");
            return;
        }
        LLSD info = infoFrom(list, size);
        sAnimInfo[asset] = info;
        answerAnim(asset, info, "");
    }

    // ---------------------------------------------------------------------
    // playing: what the app started (local = only me, world = everyone)
    // ---------------------------------------------------------------------
    struct Playing { LLUUID item; bool world = false; F64 started = 0.0; };
    std::map<LLUUID, Playing> sPlaying;   // by asset

    void stopOne(const LLUUID& asset, const Playing& p)
    {
        if (p.world) gAgent.sendAnimationRequest(asset, ANIM_REQUEST_STOP);
        if (isAgentAvatarValid()) gAgentAvatarp->stopMotion(asset);
    }

    void stopAll(bool local_only)
    {
        for (auto it = sPlaying.begin(); it != sPlaying.end();)
        {
            if (local_only && it->second.world) { ++it; continue; }
            stopOne(it->first, it->second);
            it = sPlaying.erase(it);
        }
    }

    // ---------------------------------------------------------------------
    // preview photos: play locally, camera in front, a square shot
    // ---------------------------------------------------------------------
    struct Shot { LLUUID item, asset; };
    std::deque<Shot> sShots;
    S32 sShotPhase = 0;          // 0 next, 1 waiting for the animation, 2 shooting
    F64 sShotT = 0.0;
    F64 sShotLoadedAt = 0.0;
    bool sShotCam = false;
    constexpr S32 SHOT_SIZE = 192;

    void finishShots()
    {
        if (sShotCam)
        {
            MGPhoneCamera::runCommand("cam_view", LLSD().with("preset", "reset"));
            sShotCam = false;
        }
        push(LLSD().with("type", "anim_shots_done"));
    }

    void takeShot()
    {
        if (sShots.empty() || !isAgentAvatarValid())
        {
            sShotPhase = 0;
            return;
        }
        const Shot s = sShots.front();
        sShots.pop_front();
        LLSD ev = LLSD().with("type", "inv_thumb").with("id", s.item.asString()).with("anim", true);
        LLPointer<LLImageRaw> raw = new LLImageRaw;
        MGPhoneCamera::hideGuidesForShot(true);
        const ECursorType cursor = gViewerWindow->getWindow() ? gViewerWindow->getWindow()->getCursor() : UI_CURSOR_ARROW;
        const bool ok = gViewerWindow->rawSnapshot(raw, SHOT_SIZE * 2, SHOT_SIZE * 2, false, false, false, false, false, false, false);
        gViewerWindow->setCursor(cursor);
        MGPhoneCamera::hideGuidesForShot(false);
        gAgentAvatarp->stopMotion(s.asset);
        LLPointer<LLImageJPEG> jpeg = new LLImageJPEG(85);
        if (ok && raw->getData())
        {
            raw->scale(SHOT_SIZE, SHOT_SIZE);
            if (jpeg->encode(raw, 0.f))
            {
                ev["image"] = "data:image/jpeg;base64," + LLBase64::encode(jpeg->getData(), jpeg->getDataSize());
                llofstream out(MGPhoneAnims::shotPath(s.asset.asString(), SHOT_SIZE).c_str(), std::ios::out | std::ios::binary);
                if (out.is_open()) out.write((const char*)jpeg->getData(), jpeg->getDataSize());
            }
        }
        if (!ev.has("image")) ev["error"] = "snapshot failed";
        push(ev);
        sShotPhase = 0;
        if (sShots.empty()) finishShots();
    }

    // ---------------------------------------------------------------------
    // gestures
    // ---------------------------------------------------------------------
    LLSD gestureInfo(const LLMultiGesture& g)
    {
        LLSD i;
        i["trigger"] = g.mTrigger;
        i["replace"] = g.mReplaceText;
        i["key"] = g.mKey ? LLKeyboard::stringFromAccelerator(g.mMask, g.mKey) : std::string();
        LLSD steps = LLSD::emptyArray();
        for (const LLGestureStep* step : g.mSteps)
        {
            if (!step) continue;
            std::string label;
            for (const std::string& part : step->getLabel()) label += part;
            steps.append(label);
        }
        i["steps"] = steps;
        return i;
    }

    std::map<LLUUID, std::set<LLUUID>> sGestWaiting;   // gesture asset -> items asking

    void onGestureLoaded(const LLUUID& asset, LLAssetType::EType type, void*, S32 status, LLExtStat)
    {
        auto it = sGestWaiting.find(asset);
        if (it == sGestWaiting.end()) return;
        LLSD info;
        std::string error;
        if (status == 0)
        {
            LLFileSystem file(asset, type, LLFileSystem::READ);
            const S32 size = file.getSize();
            std::vector<char> buf(size + 1);
            if (size > 0 && file.read((U8*)buf.data(), size))
            {
                buf[size] = '\0';
                LLMultiGesture g;
                LLDataPackerAsciiBuffer dp(buf.data(), size + 1);
                if (g.deserialize(dp)) info = gestureInfo(g);
                else error = "not a valid gesture";
            }
            else error = "empty";
        }
        else error = "could not load";
        for (const LLUUID& item : it->second)
        {
            LLSD ev = LLSD().with("type", "gest_info").with("id", item.asString());
            if (error.empty()) ev["info"] = info;
            else ev["error"] = error;
            push(ev);
        }
        sGestWaiting.erase(it);
    }

    // ---------------------------------------------------------------------
    // The stage: a stand-in avatar (woman / man) plays the animation in an
    // off-screen render (the animation upload preview's technique); each
    // frame is read back and sent to the phone as a small JPEG. Only pictures
    // leave the viewer, never the animation data or its UUID.
    // ---------------------------------------------------------------------
    class MGAnimStage : public LLViewerDynamicTexture
    {
    public:
        MGAnimStage(S32 size)
            : LLViewerDynamicTexture(size, size, 3, ORDER_MIDDLE, false)
        {
            mAvatar = (LLVOAvatar*)gObjectList.createObjectViewer(LL_PCODE_LEGACY_AVATAR, gAgent.getRegion(), LLViewerObject::CO_FLAG_UI_AVATAR);
            mAvatar->mSpecialRenderMode = 1;
            mAvatar->startMotion(ANIM_AGENT_STAND, 5.f);   // like the upload preview: a settled stand under it
            mAvatar->updateOverallAppearance();
            mAvatar->hideHair();
            mAvatar->hideSkirt();
            mAvatar->stopMotion(ANIM_AGENT_HEAD_ROT, true);
            mAvatar->stopMotion(ANIM_AGENT_EYE, true);
            mAvatar->stopMotion(ANIM_AGENT_BODY_NOISE, true);
            mAvatar->stopMotion(ANIM_AGENT_BREATHE_ROT, true);
        }

        S8 getType() const override { return LLViewerDynamicTexture::LL_PREVIEW_ANIMATION; }

        // a frame when it is time (fps), or once after a change while paused
        bool needsRender() override
        {
            if (!mAvatar || mAvatar->isDead()) return false;
            if (mPauseIn > 0 && --mPauseIn == 0) mPauseReq = mAvatar->requestPause();
            const F64 now = LLTimer::getTotalSeconds();
            if (mPauseReq.notNull() && !mDirty && !mSpin) return false;
            return now - mLastFrame >= 1.0 / llmax(1, mFps);
        }

        void play(const LLUUID& asset, F32 offset = 0.f)
        {
            if (mAsset.notNull()) mAvatar->stopMotion(mAsset, true);
            mAsset = asset;
            mPauseReq = nullptr;
            mPauseIn = 0;
            if (asset.notNull()) mAvatar->startMotion(asset, offset);
            mDirty = true;
        }

        // the body shape follows the "male" parameter (as the image upload preview does it)
        void setSex(bool male)
        {
            mAvatar->setVisualParamWeight("male", male ? 1.f : 0.f);
            mAvatar->updateVisualParams();
            if (mAvatar->mDrawable.notNull()) mAvatar->updateGeometry(mAvatar->mDrawable);
            mAvatar->dirtyMesh();
            mMale = male;
            mDirty = true;
        }

        void pause(bool on)
        {
            mPauseIn = 0;
            mPauseReq = on ? mAvatar->requestPause() : nullptr;
            mDirty = true;
        }
        bool paused() const { return mPauseReq.notNull() || mPauseIn > 0; }

        // seek: started again "t" seconds ago, frozen there when paused
        void seek(F32 t)
        {
            const bool paused = this->paused();
            mPauseReq = nullptr;
            if (mAsset.notNull())
            {
                mAvatar->stopMotion(mAsset, true);
                mAvatar->startMotion(mAsset, t);
            }
            if (paused) mPauseIn = 3;   // let it pose, then hold
            mDirty = true;
        }

        void setSpeed(F32 f) { mAvatar->setAnimTimeFactor(llclamp(f, 0.1f, 3.f)); }

        bool render() override
        {
            mLastFrame = LLTimer::getTotalSeconds();
            mDirty = false;
            if (mSpin) mYaw += 0.025f;
            LLVOAvatar* av = mAvatar;

            // backdrop: a soft studio gradient
            gGL.matrixMode(LLRender::MM_PROJECTION);
            gGL.pushMatrix();
            gGL.loadIdentity();
            gGL.ortho(0.0f, (F32)mFullWidth, 0.0f, (F32)mFullHeight, -1.0f, 1.0f);
            gGL.matrixMode(LLRender::MM_MODELVIEW);
            gGL.pushMatrix();
            gGL.loadIdentity();
            gUIProgram.bind();
            {
                LLGLSUIDefault def;
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                const F32 w = (F32)mFullWidth, h = (F32)mFullHeight;
                // 0 light studio (the grey body stands out), 1 dark, 2 blue
                static const F32 BG[3][2][3] = { { { 0.62f, 0.66f, 0.72f }, { 0.93f, 0.94f, 0.96f } },
                                                 { { 0.03f, 0.035f, 0.05f }, { 0.13f, 0.15f, 0.2f } },
                                                 { { 0.05f, 0.12f, 0.28f }, { 0.25f, 0.5f, 0.85f } } };
                const F32* lo = BG[mBg][0];
                const F32* hi = BG[mBg][1];
                gGL.begin(LLRender::TRIANGLES);
                gGL.color4f(lo[0], lo[1], lo[2], 1.f); gGL.vertex2f(0.f, 0.f);
                gGL.color4f(lo[0], lo[1], lo[2], 1.f); gGL.vertex2f(w, 0.f);
                gGL.color4f(hi[0], hi[1], hi[2], 1.f); gGL.vertex2f(w, h);
                gGL.color4f(lo[0], lo[1], lo[2], 1.f); gGL.vertex2f(0.f, 0.f);
                gGL.color4f(hi[0], hi[1], hi[2], 1.f); gGL.vertex2f(w, h);
                gGL.color4f(hi[0], hi[1], hi[2], 1.f); gGL.vertex2f(0.f, h);
                gGL.end();
            }
            gGL.matrixMode(LLRender::MM_PROJECTION);
            gGL.popMatrix();
            gGL.matrixMode(LLRender::MM_MODELVIEW);
            gGL.popMatrix();
            gGL.flush();

            // camera: around the avatar (following the pelvis when it travels)
            const LLVector3 root = av->mRoot->getWorldPosition();
            LLVector3 target = root;
            if (mFollow && av->mPelvisp)
            {
                const LLVector3 pelvis = av->mPelvisp->getWorldPosition();
                target.mV[VX] = pelvis.mV[VX];
                target.mV[VY] = pelvis.mV[VY];
            }
            target.mV[VZ] += mLift;
            const LLQuaternion cam_rot = LLQuaternion(mPitch, LLVector3::y_axis) * LLQuaternion(mYaw, LLVector3::z_axis);
            const LLQuaternion av_rot = av->mRoot->getWorldRotation() * cam_rot;
            LLViewerCamera* camera = LLViewerCamera::getInstance();
            camera->setOriginAndLookAt(target + LLVector3(4.f, 0.f, 0.f) * av_rot, LLVector3::z_axis, target);
            camera->setViewNoBroadcast(camera->getDefaultFOV() / mZoom);
            camera->setAspect((F32)mFullWidth / (F32)mFullHeight);
            camera->setPerspective(false, mOrigin.mX, mOrigin.mY, mFullWidth, mFullHeight, false);

            // floor grid under the feet
            if (mGrid)
            {
                gUIProgram.bind();
                LLGLDepthTest depth(GL_TRUE, GL_FALSE);
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                const F32 z = root.mV[VZ] - av->getPelvisToFoot();
                const F32 cx = ll_round(target.mV[VX] * 2.f) * 0.5f, cy = ll_round(target.mV[VY] * 2.f) * 0.5f;
                gGL.begin(LLRender::LINES);
                for (S32 i = -8; i <= 8; ++i)
                {
                    const F32 a = (i == 0) ? 0.45f : 0.2f;
                    const F32 g = mBg == 0 ? 0.25f : 0.8f;   // dark lines on the light studio
                    gGL.color4f(g, g + 0.04f, g + 0.12f, a);
                    gGL.vertex3f(cx + i * 0.25f, cy - 2.f, z); gGL.vertex3f(cx + i * 0.25f, cy + 2.f, z);
                    gGL.color4f(g, g + 0.04f, g + 0.12f, a);
                    gGL.vertex3f(cx - 2.f, cy + i * 0.25f, z); gGL.vertex3f(cx + 2.f, cy + i * 0.25f, z);
                }
                gGL.end();
                gGL.flush();
            }

            if (av->mDrawable.notNull())
            {
                av->updateLOD();
                LLVertexBuffer::unbind();
                LLGLDepthTest gls_depth(GL_TRUE);
                LLFace* face = av->mDrawable->getFace(0);
                if (face)
                {
                    LLDrawPoolAvatar* pool = (LLDrawPoolAvatar*)face->getPool();
                    av->dirtyMesh();
                    gPipeline.enableLightsPreview();
                    const LLColor4 ambient(0.42f, 0.42f, 0.46f, 1.f);
                    gGL.setAmbientLightColor(ambient);
                    const LLColor4 key(1.35f, 1.28f, 1.18f, 1.f), fill(0.55f, 0.62f, 0.78f, 1.f), rim(0.9f, 0.95f, 1.1f, 1.f);
                    const LLColor4* cols[3] = { &key, &fill, &rim };
                    for (S32 l = 1; l <= 3; ++l)
                    {
                        LLLightState* light = gGL.getLight(l);
                        light->setDiffuse(*cols[l - 1]);
                        light->setAmbient(ambient);
                    }
                    pool->renderAvatars(av);
                }
            }

            // the skeleton over the body (X-ray)
            if (mBones)
            {
                gUIProgram.bind();
                LLGLDepthTest no_depth(GL_FALSE);
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                gGL.begin(LLRender::LINES);
                for (LLAvatarJoint* j : av->getSkeleton())
                {
                    if (!j) continue;
                    LLJoint* p = j->getParent();
                    const std::string& n = j->getName();
                    if (!p || n.empty() || n[0] != 'm' || p->getName().empty() || p->getName()[0] != 'm') continue;
                    if (n.rfind("mFace", 0) == 0 || n.rfind("mEye", 0) == 0 || n == "mScreen" || n == "mRoot") continue;
                    const bool hand = n.rfind("mHand", 0) == 0;
                    gGL.color4f(hand ? 1.f : 0.25f, hand ? 0.7f : 0.95f, hand ? 0.3f : 1.f, 0.95f);
                    gGL.vertex3fv(p->getWorldPosition().mV);
                    gGL.vertex3fv(j->getWorldPosition().mV);
                }
                gGL.end();
                gGL.flush();
            }
            gGL.color4f(1, 1, 1, 1);

            // read the frame back and send it
            const S32 w = mFullWidth, h = mFullHeight;
            std::vector<U8> px((size_t)w * h * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(mOrigin.mX, mOrigin.mY, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            LLPointer<LLImageRaw> raw = new LLImageRaw((U16)w, (U16)h, 3);
            U8* d = raw->getData();
            for (S32 i = 0; i < w * h; ++i)
            {
                d[i * 3] = px[i * 4]; d[i * 3 + 1] = px[i * 4 + 1]; d[i * 3 + 2] = px[i * 4 + 2];
            }
            LLPointer<LLImageJPEG> jpeg = new LLImageJPEG(80);
            if (jpeg->encode(raw, 0.f))
            {
                push(LLSD().with("type", "anim_stage").with("id", mItem.asString())
                           .with("image", "data:image/jpeg;base64," + LLBase64::encode(jpeg->getData(), jpeg->getDataSize()))
                           .with("paused", paused()));
            }
            return true;
        }

        LLPointer<LLVOAvatar> mAvatar;
        LLAnimPauseRequest mPauseReq;
        LLUUID mAsset, mItem;
        S32 mFps = 12;
        S32 mBg = 0;
        bool mMale = false;
        S32 mPauseIn = 0;
        F64 mLastFrame = 0.0;
        bool mDirty = true, mGrid = true, mBones = false, mSpin = false, mFollow = true;
        F32 mYaw = 0.f, mPitch = 0.12f, mZoom = 1.1f, mLift = 0.f;

    protected:
        ~MGAnimStage() override
        {
            if (mAvatar) mAvatar->markDead();
        }
    };
    LLPointer<MGAnimStage> sStage;

    void closeStage()
    {
        sStage = nullptr;
    }

    // gestures asked to play while they were still loading
    std::map<LLUUID, F64> sGestToPlay;

    LLMultiGesture* activeGesture(const LLUUID& item_id)
    {
        const LLGestureMgr::item_map_t& active = LLGestureMgr::instance().getActiveGestures();
        auto it = active.find(item_id);
        return it == active.end() ? nullptr : it->second;
    }
}

namespace MGPhoneAnims
{
    bool isCommand(const std::string& cmd)
    {
        return cmd.rfind("anim_", 0) == 0 || cmd.rfind("gest_", 0) == 0;
    }

    std::string shotPath(const std::string& asset, S32 size)
    {
        const std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "mgthumbs");
        if (!LLFile::isdir(dir)) LLFile::mkdir(dir);
        return dir + gDirUtilp->getDirDelimiter() + asset + "_" + std::to_string(size) + "_a1.jpg";
    }

    void idle()
    {
        const F64 now = LLTimer::getTotalSeconds();
        // preview photos, one at a time
        if (!sShots.empty() && isAgentAvatarValid())
        {
            if (sShotPhase == 0)
            {
                if (!sShotCam)
                {
                    MGPhoneCamera::runCommand("cam_view", LLSD().with("preset", "person").with("dist", 2.9));
                    sShotCam = true;
                }
                gAgentAvatarp->startMotion(sShots.front().asset);
                sShotPhase = 1;
                sShotT = now;
                sShotLoadedAt = 0.0;
            }
            else if (sShotPhase == 1)
            {
                // wait for the keyframes, then a moment into the motion
                const LLUUID& asset = sShots.front().asset;
                if (sShotLoadedAt == 0.0 && (LLKeyframeDataCache::getKeyframeData(asset) || gAgentAvatarp->isMotionActive(asset)))
                {
                    sShotLoadedAt = now;
                }
                const bool ready = sShotLoadedAt > 0.0 && now - sShotLoadedAt > 1.1;
                if (ready || now - sShotT > 8.0)
                {
                    sShotPhase = 2;
                    doOnIdleOneTime(takeShot);   // render outside the UI draw
                }
            }
        }
        // gestures waiting to be loaded before playing
        for (auto it = sGestToPlay.begin(); it != sGestToPlay.end();)
        {
            if (activeGesture(it->first))
            {
                LLGestureMgr::instance().playGesture(it->first);
                it = sGestToPlay.erase(it);
            }
            else if (now - it->second > 15.0) it = sGestToPlay.erase(it);
            else ++it;
        }
    }

    void onClose()
    {
        closeStage();
        stopAll(true);
        if (!sShots.empty())
        {
            if (isAgentAvatarValid()) gAgentAvatarp->stopMotion(sShots.front().asset);
            sShots.clear();
            sShotPhase = 0;
        }
        sShotCam = false;   // MGPhoneCamera::restore puts the camera back
    }

    LLSD runCommand(const std::string& cmd, const LLSD& args)
    {
        LLSD result = LLSD().with("ok", true);
        const LLUUID id(args["id"].asString());

        if (cmd == "anim_info")
        {
            LLViewerInventoryItem* item = ownItem(id, LLAssetType::AT_ANIMATION);
            if (!item) return LLSD().with("ok", false).with("error", "not found");
            const LLUUID asset = item->getAssetUUID();
            auto it = sAnimInfo.find(asset);
            if (it != sAnimInfo.end())
            {
                result["info"] = it->second;
                return result;
            }
            if (const LLKeyframeMotion::JointMotionList* list = LLKeyframeDataCache::getKeyframeData(asset))
            {
                sAnimInfo[asset] = infoFrom(list, 0);
                result["info"] = sAnimInfo[asset];
                return result;
            }
            const bool first = sAnimWaiting.find(asset) == sAnimWaiting.end();
            sAnimWaiting[asset].insert(item->getUUID());
            if (first) gAssetStorage->getAssetData(asset, LLAssetType::AT_ANIMATION, onAnimLoaded, nullptr, true);
            result["pending"] = true;
            return result;
        }

        if (cmd == "anim_play")
        {
            LLViewerInventoryItem* item = ownItem(id, LLAssetType::AT_ANIMATION);
            if (!item || !isAgentAvatarValid()) return LLSD().with("ok", false).with("error", "not found");
            if (args["replace"].asBoolean()) stopAll(false);   // one at a time
            const LLUUID asset = item->getAssetUUID();
            Playing p;
            p.item = item->getUUID();
            p.world = args["world"].asBoolean();
            p.started = LLTimer::getTotalSeconds();
            if (p.world) gAgent.sendAnimationRequest(asset, ANIM_REQUEST_START);
            else gAgentAvatarp->startMotion(asset);
            sPlaying[asset] = p;
            return result;
        }

        if (cmd == "anim_stop")
        {
            if (args["everything"].asBoolean())
            {
                stopAll(false);
                gAgent.stopCurrentAnimations();
                return result;
            }
            if (args["all"].asBoolean())
            {
                stopAll(false);
                return result;
            }
            LLViewerInventoryItem* item = ownItem(id, LLAssetType::AT_ANIMATION);
            if (!item) return LLSD().with("ok", false).with("error", "not found");
            auto it = sPlaying.find(item->getAssetUUID());
            if (it != sPlaying.end())
            {
                stopOne(it->first, it->second);
                sPlaying.erase(it);
            }
            else if (isAgentAvatarValid())
            {
                gAgentAvatarp->stopMotion(item->getAssetUUID());
            }
            return result;
        }

        if (cmd == "anim_state")
        {
            LLSD list = LLSD::emptyArray();
            const F64 now = LLTimer::getTotalSeconds();
            for (auto it = sPlaying.begin(); it != sPlaying.end();)
            {
                const bool active = isAgentAvatarValid() && gAgentAvatarp->isMotionActive(it->first);
                // finished (a non-looping one ran out): forget it after a grace time for loading
                if (!active && now - it->second.started > 6.0)
                {
                    it = sPlaying.erase(it);
                    continue;
                }
                list.append(LLSD().with("id", it->second.item.asString()).with("world", it->second.world).with("active", active));
                ++it;
            }
            result["playing"] = list;
            result["shots"] = (S32)sShots.size();
            return result;
        }

        if (cmd == "anim_watch")
        {
            return MGPhoneCamera::runCommand("cam_view", LLSD().with("preset", args["on"].asBoolean() ? "person" : "reset").with("dist", 3.0));
        }

        if (cmd == "anim_shot")
        {
            S32 n = 0;
            for (const auto& v : llsd::inArray(args["ids"]))
            {
                if (sShots.size() >= 60) break;
                LLViewerInventoryItem* item = ownItem(LLUUID(v.asString()), LLAssetType::AT_ANIMATION);
                if (!item) continue;
                sShots.push_back({ item->getUUID(), item->getAssetUUID() });
                ++n;
            }
            if (!n) return LLSD().with("ok", false).with("error", "nothing to photograph");
            stopAll(true);
            result["queued"] = n;
            return result;
        }

        if (cmd == "anim_shot_cancel")
        {
            if (!sShots.empty() && isAgentAvatarValid()) gAgentAvatarp->stopMotion(sShots.front().asset);
            sShots.clear();
            sShotPhase = 0;
            finishShots();
            return result;
        }

        if (cmd == "anim_stage")
        {
            // {id, size, sex: "f"|"m", bones, grid, fps, speed}: the stand-in avatar plays this animation
            LLViewerInventoryItem* item = ownItem(id, LLAssetType::AT_ANIMATION);
            if (!item) return LLSD().with("ok", false).with("error", "not found");
            if (!gAgent.getRegion()) return LLSD().with("ok", false).with("error", "not in a region");
            const S32 size = llclamp(args.has("size") ? args["size"].asInteger() : 360, 160, 512);
            if (sStage.isNull() || sStage->getFullWidth() != size || sStage->mAvatar.isNull() || sStage->mAvatar->isDead())
            {
                sStage = new MGAnimStage(size);
                sStage->setSex(args["sex"].asString() == "m");
            }
            else if (args.has("sex"))
            {
                const bool male = args["sex"].asString() == "m";
                if (sStage->mMale != male) sStage->setSex(male);
            }
            if (args.has("bones")) sStage->mBones = args["bones"].asBoolean();
            if (args.has("grid")) sStage->mGrid = args["grid"].asBoolean();
            if (args.has("fps")) sStage->mFps = llclamp(args["fps"].asInteger(), 4, 24);
            if (args.has("bg")) sStage->mBg = llclamp(args["bg"].asInteger(), 0, 2);
            sStage->setSpeed(args.has("speed") ? (F32)args["speed"].asReal() : 1.f);
            sStage->mItem = item->getUUID();
            sStage->play(item->getAssetUUID());
            if (const LLKeyframeMotion::JointMotionList* list = LLKeyframeDataCache::getKeyframeData(item->getAssetUUID()))
            {
                result["dur"] = list->mDuration;
                result["loop"] = list->mLoop;
            }
            return result;
        }

        if (cmd == "anim_stage_set")
        {
            // camera {dyaw, dpitch, dzoom, view: front|side|back|top|reset, spin, follow} and playback {pause, speed, seek, restart, sex, bones, grid}
            if (sStage.isNull()) return LLSD().with("ok", false).with("error", "no stage");
            MGAnimStage* st = sStage;
            if (args.has("dyaw")) st->mYaw += (F32)args["dyaw"].asReal();
            if (args.has("dpitch")) st->mPitch = llclamp(st->mPitch + (F32)args["dpitch"].asReal(), -1.2f, 1.2f);
            if (args.has("dzoom")) st->mZoom = llclamp(st->mZoom * (F32)args["dzoom"].asReal(), 0.6f, 4.f);
            if (args.has("lift")) st->mLift = llclamp(st->mLift + (F32)args["lift"].asReal(), -1.f, 1.f);
            if (args.has("view"))
            {
                const std::string v = args["view"].asString();
                st->mPitch = v == "top" ? 1.1f : 0.12f;
                st->mYaw = v == "side" ? F_PI_BY_TWO : (v == "back" ? F_PI : (v == "three" ? F_PI / 4.f : 0.f));
                if (v == "reset") { st->mZoom = 1.1f; st->mLift = 0.f; }
            }
            if (args.has("spin")) st->mSpin = args["spin"].asBoolean();
            if (args.has("follow")) st->mFollow = args["follow"].asBoolean();
            if (args.has("bones")) st->mBones = args["bones"].asBoolean();
            if (args.has("grid")) st->mGrid = args["grid"].asBoolean();
            if (args.has("sex")) st->setSex(args["sex"].asString() == "m");
            if (args.has("bg")) st->mBg = llclamp(args["bg"].asInteger(), 0, 2);
            if (args.has("speed")) st->setSpeed((F32)args["speed"].asReal());
            if (args.has("pause")) st->pause(args["pause"].asBoolean());
            if (args.has("seek")) st->seek((F32)args["seek"].asReal());
            if (args["restart"].asBoolean()) st->play(st->mAsset);
            st->mDirty = true;
            result["paused"] = st->paused();
            return result;
        }

        if (cmd == "anim_stage_close")
        {
            closeStage();
            return result;
        }

        if (cmd == "anim_ao")
        {
            result["on"] = gSavedPerAccountSettings.getBOOL("UseAO");
            const AOSet* current = AOEngine::instance().getCurrentSet();
            LLSD sets = LLSD::emptyArray();
            for (AOSet* set : AOEngine::instance().getSetList())
            {
                if (!set) continue;
                LLSD states = LLSD::emptyArray();
                for (S32 i = 0; i < AOSet::AOSTATES_MAX; ++i)
                {
                    const AOSet::AOState* state = set->getState(i);
                    if (!state) continue;
                    // 7.2.5: a state is a list of steps, each one animation or a group of them
                    S32 n = 0;
                    for (const auto& step : state->mSteps) n += (S32)step.mMembers.size();
                    states.append(LLSD().with("i", i).with("name", state->mName).with("n", n));
                }
                sets.append(LLSD().with("name", set->getName()).with("current", set == current).with("states", states));
            }
            result["sets"] = sets;
            return result;
        }

        if (cmd == "anim_ao_add")
        {
            LLViewerInventoryItem* item = ownItem(id, LLAssetType::AT_ANIMATION);
            if (!item) return LLSD().with("ok", false).with("error", "not found");
            AOSet* set = AOEngine::instance().getSetByName(args["set"].asString());
            AOSet::AOState* state = set ? set->getState(args["state"].asInteger()) : nullptr;
            if (!set || !state) return LLSD().with("ok", false).with("error", "no such AO set or state");
            AOEngine::instance().addAnimation(set, state, item);
            return result;
        }

        // <MG> a first AO set from the phone (the AO engine then makes its state folders and
        // loads it on its reload timer; the page asks anim_ao again until it shows up)
        if (cmd == "anim_ao_newset")
        {
            std::string name = args["name"].asString();
            LLStringUtil::trim(name);
            if (name.empty()) name = "My AO";
            if (name.size() > 63) name.resize(63);
            if (AOEngine::instance().getSetByName(name)) return LLSD().with("ok", false).with("error", "a set has this name");
            AOEngine::instance().addSet(name, [name](const LLUUID&)
                {
                    push(LLSD().with("type", "anim_ao_changed").with("name", name));
                });
            result["pending"] = true;
            return result;
        }

        if (cmd == "anim_ao_enable")
        {
            gSavedPerAccountSettings.setBOOL("UseAO", args["on"].asBoolean());
            return result;
        }

        if (cmd == "gest_info")
        {
            LLViewerInventoryItem* item = ownItem(id, LLAssetType::AT_GESTURE);
            if (!item) return LLSD().with("ok", false).with("error", "not found");
            result["active"] = LLGestureMgr::instance().isGestureActive(item->getUUID());
            result["playing"] = LLGestureMgr::instance().isGesturePlaying(item->getUUID());
            if (LLMultiGesture* g = activeGesture(item->getUUID()))
            {
                result["info"] = gestureInfo(*g);
                return result;
            }
            const LLUUID asset = item->getAssetUUID();
            const bool first = sGestWaiting.find(asset) == sGestWaiting.end();
            sGestWaiting[asset].insert(item->getUUID());
            if (first) gAssetStorage->getAssetData(asset, LLAssetType::AT_GESTURE, onGestureLoaded, nullptr, true);
            result["pending"] = true;
            return result;
        }

        if (cmd == "gest_play")
        {
            LLViewerInventoryItem* item = ownItem(id, LLAssetType::AT_GESTURE);
            if (!item) return LLSD().with("ok", false).with("error", "not found");
            if (activeGesture(item->getUUID()))
            {
                LLGestureMgr::instance().playGesture(item->getUUID());
                return result;
            }
            // not active (or still loading): activate it, it plays once loaded
            if (!LLGestureMgr::instance().isGestureActive(item->getUUID())) LLGestureMgr::instance().activateGesture(item->getUUID());
            sGestToPlay[item->getUUID()] = LLTimer::getTotalSeconds();
            result["activated"] = true;
            return result;
        }

        if (cmd == "gest_active")
        {
            LLViewerInventoryItem* item = ownItem(id, LLAssetType::AT_GESTURE);
            if (!item) return LLSD().with("ok", false).with("error", "not found");
            if (args["on"].asBoolean()) LLGestureMgr::instance().activateGesture(item->getUUID());
            else LLGestureMgr::instance().deactivateGesture(item->getUUID());
            return result;
        }

        return LLSD().with("ok", false).with("error", "unknown command");
    }
}
