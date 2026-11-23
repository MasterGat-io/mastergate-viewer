/**
 * @file mgphonecreator.cpp
 * @brief MasterGate Viewer: the MG Phone "Creator Lab" app: a garment from Blender
 *        worn on your own avatar, checked before upload, stress-tested for clipping.
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

#include "mgphonecreator.h"
#include "mgphoneposer.h" // <MG/> pose the avatar to the garment (the Camera's poser bridge)
#include "lltoolcomp.h" // <MG/> the poser's rotate rings (FSToolCompPose), drawn by Creator Lab's posing tool

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <functional>
#include <atomic>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <chrono>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "indra_constants.h"
#include "llagent.h"
#include "llanimationstates.h"
#include "llappearancemgr.h"
#include "lldrawable.h"
#include "llface.h"
#include "llfilepicker.h"
#include "lljoint.h"
#include "llrender2dutils.h"
#include "llfontgl.h" // <MG/> bone names on the skeleton overlay
#include "llrootview.h"
#include "llview.h"
#include "llviewercamera.h"
#include "llviewerwindow.h"
#include "llfloaterreg.h"
#include "llskinningutil.h"
#include "llmeshoptimizer.h" // <MG/> LOD preview (the uploader's simplifier)
#include "llviewertexture.h" // <MG/> weight view (a local colour texture on the garment)
#include "llimage.h"
#include "llmeshrepository.h" // <MG/> skin guard (gMeshRepo.mSkinMap)
#include "lltool.h"
#include "lltoolfocus.h"
#include "lltoolmgr.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llmodel.h"
#include "lltextureentry.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewermenufile.h"
#include "llviewerobjectlist.h"
#include "llviewervisualparam.h"
#include "llvoavatarself.h"
#include "llvolume.h"
#include "llvovolume.h"
#include "pipeline.h"
#include "vjlocalmesh.h"
#include "lllocalbitmaps.h"        // <MG/> Look: local textures on the garment
#include "lllocalgltfmaterials.h"  // <MG/> Look: PBR materials written as .gltf, shown as local materials
#include "llmaterial.h"
#include "llsdserialize.h"
#include "llwindow.h"             // <MG/> Look: open the material folder
#include "llimagepng.h"          // <MG/> Look: packed maps and thumbnails
#include "llbase64.h"
#include "llviewerassetupload.h"   // <MG/> Alpha: the masks uploaded
#include "llagentbenefits.h"
#include "llfloaterperms.h"
#include "llviewertexturelist.h"
#include "llwearablelist.h"        // <MG/> Alpha: the wearable made
#include "llviewerwearable.h"
#include "lllocaltextureobject.h"
#include "llavatarappearancedefines.h"
#include "llimagej2c.h"
#include <filesystem>
#include "mgfloaterphone.h"
#include "mgcreatordae.h"
#include "mgcreatorrefit.h"
#include "mgcreatorweights.h"
#include "mgrig.h"
#include "mgrigcloth.h"
#include "mgdrape.h"
#include "mgfitcore.h" // <MG/> Make it fit: the geometry
#include "lldir.h"
#include "llfloatermodelpreview.h"

#include "glm/mat3x3.hpp"
#include "glm/mat4x4.hpp"
#include "glm/matrix.hpp"
#include "glm/geometric.hpp" // <MG/> cross / dot (Make it fit: flipped triangles)
#include "glm/gtc/type_ptr.hpp"
#include "glm/gtc/matrix_transform.hpp" // <MG/> translate / scale (the pose's rest chain)

namespace
{
    void push(const LLSD& ev)
    {
        MGFloaterPhone* phone = LLFloaterReg::findTypedInstance<MGFloaterPhone>("mg_phone");
        if (phone) phone->pushToPage(ev);
    }

    LLLocalMeshSystem* lms() { return LLLocalMeshSystem::getInstance(); }

    std::string lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    std::string baseName(const std::string& path)
    {
        const size_t p = path.find_last_of("/\\");
        return p == std::string::npos ? path : path.substr(p + 1);
    }

    // ------------------------------------------------------------------
    // body regions and fitted-mesh bones
    // ------------------------------------------------------------------

    // a joint name -> the region a creator talks about (order matters: HANDLE before HAND)
    std::string regionOf(const std::string& j)
    {
        static const std::vector<std::pair<const char*, const char*>> MAP = {
            { "HANDLE", "belly" }, { "mHand", "hands" }, { "HAND", "hands" }, { "mWrist", "hands" },
            { "mElbow", "arms" }, { "LOWER_ARM", "arms" }, { "mShoulder", "arms" }, { "UPPER_ARM", "arms" },
            { "mCollar", "shoulders" }, { "CLAVICLE", "shoulders" },
            { "PEC", "chest" }, { "mChest", "chest" }, { "CHEST", "chest" }, { "UPPER_BACK", "chest" },
            { "mSpine3", "chest" }, { "mSpine4", "chest" },
            { "mTorso", "belly" }, { "BELLY", "belly" }, { "LOWER_BACK", "belly" }, { "mSpine1", "belly" }, { "mSpine2", "belly" },
            { "mPelvis", "hips" }, { "PELVIS", "hips" }, { "BUTT", "hips" }, { "mGroin", "hips" },
            { "mHip", "thighs" }, { "UPPER_LEG", "thighs" },
            { "mKnee", "legs" }, { "LOWER_LEG", "legs" },
            { "mAnkle", "feet" }, { "mFoot", "feet" }, { "mToe", "feet" }, { "FOOT", "feet" },
            { "mNeck", "neck" }, { "NECK", "neck" },
            { "mHead", "head" }, { "HEAD", "head" }, { "mFace", "head" }, { "mEye", "head" }, { "mSkull", "head" },
            { "mTail", "tail" }, { "mWing", "wings" },
        };
        for (const auto& kv : MAP)
        {
            if (j.find(kv.first) != std::string::npos) return kv.second;
        }
        return "other";
    }

    // the collision volumes: weights on them make a garment follow the shape sliders
    bool isFittedBone(const std::string& j)
    {
        static const std::set<std::string> CV = {
            "PELVIS", "BUTT", "BELLY", "LEFT_HANDLE", "RIGHT_HANDLE", "LOWER_BACK", "UPPER_BACK", "CHEST",
            "LEFT_PEC", "RIGHT_PEC", "NECK", "HEAD", "L_CLAVICLE", "R_CLAVICLE", "L_UPPER_ARM", "R_UPPER_ARM",
            "L_LOWER_ARM", "R_LOWER_ARM", "L_HAND", "R_HAND", "L_UPPER_LEG", "R_UPPER_LEG", "L_LOWER_LEG",
            "R_LOWER_LEG", "L_FOOT", "R_FOOT" };
        return CV.count(j) > 0;
    }

    // ------------------------------------------------------------------
    // files opened from the app (they live in Local Mesh's list)
    // ------------------------------------------------------------------
    struct FileWatch
    {
        LLUUID      id;
        std::string path;
        const void* gen = nullptr;   // first loaded object: changes on every reload
        S32         status = -1;
        S32         loads = 0;
    };
    std::vector<FileWatch> sFiles;
    bool sPicking = false;
    std::string sDevkitPath;   // the body devkit weights are copied from (and versions are made from)
    // <MG> that body, loaded once (a devkit .dae, or the body out of a project .glb / .fbx), shared
    // with the worker threads; loading it runs on a thread too (a project file takes seconds)
    std::shared_ptr<const MGCreatorDae::File> sBody;
    std::vector<std::string> sBodyPieces;
    struct BodyJob
    {
        std::mutex lock;
        bool done = false;              // guarded
        std::string path, error;
        std::shared_ptr<MGCreatorDae::File> body;
        std::vector<std::string> pieces;
    };
    std::shared_ptr<BodyJob> sBodyJob;

    // <MG> Versions for all bodies: the garment refitted to other bodies' devkits, on a
    // worker thread (about 3 s a body). The thread only reads and writes files and this job.
    struct VersionDone
    {
        std::string body, out, error;
        S32 surface = 0, followed = 0, bonesOnly = 0, added = 0;
        F64 meanMm = 0.0, maxMm = 0.0;
    };
    struct VersionJob
    {
        std::mutex lock;
        std::vector<VersionDone> done;   // guarded by lock
        bool finished = false;           // guarded by lock
        std::atomic<bool> cancel{ false };
        size_t total = 0;
    };
    std::vector<std::string> sVersionTargets;     // the other bodies' devkits
    std::shared_ptr<VersionJob> sVersionJob;
    size_t sVersionReported = 0;
    std::vector<std::string> sVersionOut;          // the versions made (try on)

    std::string samePathKey(std::string p)
    {
        for (auto& c : p) c = (c == '\\') ? '/' : (char)std::tolower((unsigned char)c);
        return p;
    }

    // <MG> Convert to SL: a character from DAZ, VRoid, Mixamo, CC, Unreal... re-rigged to SL's
    // skeleton on a worker thread (the thread only reads and writes files and this job)
    struct ConvertJob
    {
        std::mutex lock;
        bool finished = false;          // guarded by lock
        std::string source, out, error;
        MGRig::Detection detection;
        MGRig::Report report;
        std::vector<std::string> notes;
    };
    std::shared_ptr<ConvertJob> sConvertJob;

    // <MG> Rig clothes & accessories: a file analysed (items, suggestions), reviewed in the app,
    // then rigged; the model stays here so the creator can change the choices and rig again
    struct RigJob
    {
        std::mutex lock;
        int phase = 1;                  // 1 analysing, 2 ready to review, 3 rigging, 4 rigged (guarded)
        std::string source, out, error; // guarded while a thread runs
        MGRig::Model model;
        std::shared_ptr<const MGCreatorDae::File> body;   // the body to skin to (or none)
        std::vector<MGRig::SlBone> sl;
        std::vector<MGRigCloth::Item> items;
        MGRigCloth::Result result;
        int reportedPhase = 0;          // main thread only
        LLUUID standin;                 // the worn stand-in to show it on (main thread only)
        bool preview = false;           // a preview to place (placement applied live, not baked)
        F32 pmove[3] = { 0.f, 0.f, 0.f };
        F32 pturn = 0.f, pscale = 1.f;
    };
    std::shared_ptr<RigJob> sRigJob;

    // the converted file's name: <name>_sl.dae, unless a file of that name is not one of ours
    std::string convertOutPath(const std::string& source, const std::string& wanted = std::string())
    {
        const std::string base = wanted.empty() ? MGRig::outPathFor(source) : wanted;
        for (int i = 1; i < 100; ++i)
        {
            std::string p = base;
            if (i > 1) p = base.substr(0, base.size() - 4) + std::to_string(i) + ".dae";
            if (samePathKey(p) == samePathKey(source)) continue;
            std::ifstream in(p, std::ios::binary);
            if (!in) return p;   // free
            char head[600] = { 0 };
            in.read(head, sizeof(head) - 1);
            if (std::string(head).find("Creator Lab (rig converter)") != std::string::npos) return p;   // ours: replaced
        }
        return std::string();
    }

    FileWatch* watchOf(const LLUUID& id)
    {
        for (auto& w : sFiles) if (w.id == id) return &w;
        return nullptr;
    }

    const char* statusName(S32 s)
    {
        switch (s)
        {
            case LLLocalMeshFile::STATUS_LOADING: return "loading";
            case LLLocalMeshFile::STATUS_ACTIVE:  return "ready";
            case LLLocalMeshFile::STATUS_ERROR:   return "error";
            default:                              return "none";
        }
    }

    // more than 4 weights on a vertex: Local Mesh and the uploader keep the 4 biggest,
    // so this is read from the file itself. DAE: <vcount> entries above 4. glTF: a
    // second JOINTS set means up to 8. Returns -1 when unknown.
    S32 countOverFour(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return -1;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string ext = lower(path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.')));
        if (ext == ".gltf") return text.find("\"JOINTS_1\"") != std::string::npos ? 1 : 0;
        if (ext != ".dae") return -1;
        S32 over = 0;
        size_t pos = 0;
        while ((pos = text.find("<vcount>", pos)) != std::string::npos)
        {
            const size_t end = text.find("</vcount>", pos);
            if (end == std::string::npos) break;
            const char* p = text.c_str() + pos + 8;
            const char* e = text.c_str() + end;
            while (p < e)
            {
                while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
                if (p >= e) break;
                const long v = strtol(p, nullptr, 10);
                if (v > 4) ++over;
                while (p < e && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') ++p;
            }
            pos = end;
        }
        return over;
    }

    // the upload checks, on what Local Mesh loaded (+ the raw file for weights > 4)
    LLSD runChecks(LLLocalMeshFile* f, const std::string& path)
    {
        LLSD out = LLSD::emptyMap();
        if (!f) return out;
        auto& objs = f->getObjectVector();
        S32 maxJoints = 0, maxFaces = 0, unweighted = 0, verts = 0, thin = 0, riggedObjs = 0, fittedObjs = 0;
        S32 rotated = 0, boneCount = 0;   // bones whose bind matrix turns them (SL expects none)
        F32 pelvisZ = -1000.f;            // where the file puts mPelvis (SL: 1.067 m)
        std::array<S32, LOCAL_NUM_LODS> tris{ 0, 0, 0, 0 };
        std::array<bool, LOCAL_NUM_LODS> hasLod{ false, false, false, false };
        std::set<std::string> regions;
        LLSD objects = LLSD::emptyArray();

        for (auto& o : objs)
        {
            if (!o) continue;
            LLPointer<LLMeshSkinInfo> skin = o->getObjectMeshSkinInfo();
            const bool rigged = o->getIsRiggedObject() && skin.notNull();
            const S32 nj = rigged ? (S32)skin->mJointNames.size() : 0;
            bool fitted = false;
            if (rigged)
            {
                ++riggedObjs;
                for (const auto& j : skin->mJointNames)
                {
                    if (isFittedBone(j)) fitted = true;
                    regions.insert(regionOf(j));
                }
                // Second Life's skeleton has no rotations in its rest pose, so a file rigged for
                // it has bind matrices that only move (and scale) bones. Maya / DAZ / game rigs
                // keep "joint orients": the garment then twists away from the body.
                for (size_t ji = 0; ji < skin->mJointNames.size() && ji < skin->mInvBindMatrix.size(); ++ji)
                {
                    // collision volumes rest a few degrees turned in SL itself (CHEST -10, PELVIS 8):
                    // only the regular m* bones, all unrotated in SL, are judged
                    const std::string& jn = skin->mJointNames[ji];
                    if (jn.empty() || jn[0] != 'm') continue;
                    const F32* m = skin->mInvBindMatrix[ji].getF32ptr();
                    LLVector3 ax(m[0], m[1], m[2]), ay(m[4], m[5], m[6]), az(m[8], m[9], m[10]);
                    if (ax.normVec() < 1e-6f || ay.normVec() < 1e-6f || az.normVec() < 1e-6f) continue;
                    const F32 tr = llclamp((ax.mV[0] + ay.mV[1] + az.mV[2] - 1.f) * 0.5f, -1.f, 1.f);
                    ++boneCount;
                    if (std::acos(tr) * RAD_TO_DEG > 10.f) ++rotated;
                    if (jn == "mPelvis")
                    {
                        LLMatrix4 bind(m);
                        bind.invert();
                        pelvisZ = bind.getTranslation().mV[VZ];
                    }
                }
                if (fitted) ++fittedObjs;
            }
            maxJoints = llmax(maxJoints, nj);

            S32 best = -1;
            for (S32 lod = 0; lod < LOCAL_NUM_LODS; ++lod)
            {
                auto& faces = o->getFaces((LLLocalMeshFileLOD)lod);
                if (faces.empty()) continue;
                hasLod[lod] = true;
                best = lod;
                for (auto& fc : faces) if (fc) tris[lod] += fc->getNumIndices() / 3;
            }
            if (best < 0) continue;

            auto& hi = o->getFaces((LLLocalMeshFileLOD)best);
            maxFaces = llmax(maxFaces, (S32)hi.size());
            const LLVector4 size = o->getObjectSize();
            S32 objUnweighted = 0, objThin = 0;
            for (auto& fc : hi)
            {
                if (!fc) continue;
                auto& pos = fc->getPositions();
                auto& idx = fc->getIndices();
                verts += (S32)pos.size();
                if (rigged)
                {
                    for (const auto& u : fc->getSkin())
                    {
                        F32 sum = 0.f;
                        for (F32 w : u.mJointWeights) sum += w;
                        if (sum <= 1e-4f) ++objUnweighted;
                    }
                }
                for (size_t t = 0; t + 2 < idx.size(); t += 3)
                {
                    const S32 ia = idx[t], ib = idx[t + 1], ic = idx[t + 2];
                    if (ia < 0 || ib < 0 || ic < 0 || ia >= (S32)pos.size() || ib >= (S32)pos.size() || ic >= (S32)pos.size()) continue;
                    // Local Mesh keeps positions in a unit box: back to meters
                    LLVector3 a(pos[ia].mV[0] * size.mV[0], pos[ia].mV[1] * size.mV[1], pos[ia].mV[2] * size.mV[2]);
                    LLVector3 b(pos[ib].mV[0] * size.mV[0], pos[ib].mV[1] * size.mV[1], pos[ib].mV[2] * size.mV[2]);
                    LLVector3 c(pos[ic].mV[0] * size.mV[0], pos[ic].mV[1] * size.mV[1], pos[ic].mV[2] * size.mV[2]);
                    const F32 area = 0.5f * ((b - a) % (c - a)).length();
                    const F32 edge = llmin((b - a).length(), llmin((c - b).length(), (a - c).length()));
                    if (area < 1e-10f || edge < 5e-5f) ++objThin;
                }
            }
            unweighted += objUnweighted;
            thin += objThin;
            objects.append(LLSD().with("name", o->getObjectName()).with("rigged", rigged).with("bones", nj)
                .with("fitted", fitted).with("materials", (S32)hi.size()).with("unweighted", objUnweighted).with("thin", objThin));
        }

        const S32 over = countOverFour(path);
        LLSD rows = LLSD::emptyArray();
        auto row = [&rows](const char* k, const char* state, const std::string& val)
        {
            rows.append(LLSD().with("k", k).with("state", state).with("val", val));
        };
        {
            std::ostringstream s; s << maxJoints << " of 110";
            row("bones", maxJoints <= (S32)LL_MAX_JOINTS_PER_MESH_OBJECT ? "ok" : "bad", s.str());
        }
        {
            std::ostringstream s;
            if (over < 0) s << "not read from this file type";
            else if (over == 0) s << "4 at most";
            else s << over << " vertices have more than 4";
            row("weights4", over == 0 ? "ok" : (over < 0 ? "info" : "warn"), s.str());
        }
        if (riggedObjs > 0)
        {
            std::ostringstream s;
            if (unweighted) s << unweighted << " vertices have no weights"; else s << "none";
            row("unweighted", unweighted ? "bad" : "ok", s.str());
        }
        {
            std::ostringstream s;
            if (thin) s << thin << " thin or zero-size triangles"; else s << "none";
            row("thin", thin ? "bad" : "ok", s.str());
        }
        {
            std::ostringstream s; s << maxFaces << " of 8";
            row("materials", maxFaces <= 8 ? "ok" : "bad", s.str());
        }
        {
            static const char* LN[] = { "lowest", "low", "medium", "high" };
            std::ostringstream s;
            bool first = true, missing = false;
            for (S32 lod = LOCAL_NUM_LODS - 1; lod >= 0; --lod)
            {
                if (!hasLod[lod]) { missing = true; continue; }
                if (!first) s << " · ";
                s << LN[lod] << " " << tris[lod];
                first = false;
            }
            if (missing) s << " (the uploader makes the others)";
            row("lod", "ok", s.str());
        }
        if (riggedObjs > 0 && boneCount > 0)
        {
            std::ostringstream s;
            if (rotated) s << rotated << " of " << boneCount << " bones are turned in the file";
            else s << "matches the Second Life skeleton";
            row("rig", rotated ? "bad" : "ok", s.str());
        }
        if (riggedObjs > 0 && pelvisZ > -999.f)
        {
            std::ostringstream s;
            s.setf(std::ios::fixed);
            s.precision(2);
            s << "pelvis at " << pelvisZ << " m (Second Life: 1.07 m)";
            row("size", std::fabs(pelvisZ - 1.067f) <= 0.3f ? "ok" : "bad", s.str());
        }
        if (riggedObjs == 0) row("fitted", "warn", "not rigged: it won't follow the body");
        else if (fittedObjs == riggedObjs) row("fitted", "ok", "yes: shape sliders reshape it");
        else row("fitted", "warn", "no: shape sliders won't reshape it");

        LLSD reg = LLSD::emptyArray();
        for (const auto& r : regions) reg.append(r);
        out["rows"] = rows;
        out["objects"] = objects;
        out["regions"] = reg;
        out["verts"] = verts;
        out["thin"] = thin;
        out["rigged"] = riggedObjs > 0;
        out["rotated"] = rotated;
        return out;
    }

    LLSD fileRow(FileWatch& w)
    {
        LLSD r = LLSD().with("id", w.id.asString()).with("name", baseName(w.path)).with("path", w.path);
        LLLocalMeshFile* f = lms()->getFile(w.id);
        if (!f)
        {
            r["status"] = "gone";
            return r;
        }
        auto info = f->getFileInfo();
        r["status"] = statusName(info.mStatus);
        LLSD objs = LLSD::emptyArray();
        for (const auto& n : info.mObjectList) objs.append(n);
        r["objects"] = objs;
        r["loads"] = w.loads;
        LLSD log = LLSD::emptyArray();
        if (info.mStatus == LLLocalMeshFile::STATUS_ERROR)
        {
            auto lines = f->getFileLog();
            const size_t from = lines.size() > 6 ? lines.size() - 6 : 0;
            for (size_t i = from; i < lines.size(); ++i) log.append(lines[i]);
        }
        r["log"] = log;
        return r;
    }

    LLSD openPath(const std::string& path)
    {
        const std::string ext = lower(path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.')));
        if (ext != ".dae" && ext != ".gltf" && ext != ".glb")
        {
            return LLSD().with("ok", false).with("error", "type");
        }
        for (auto& w : sFiles)
        {
            if (w.path == path && lms()->getFile(w.id))
            {
                lms()->reloadFile(w.id);
                return LLSD().with("ok", true).with("id", w.id.asString()).with("again", true);
            }
        }
        std::set<LLUUID> before;
        for (const auto& i : lms()->getFileInfoVector()) before.insert(i.mLocalID);
        lms()->addFile(path, true);
        LLUUID added;
        for (const auto& i : lms()->getFileInfoVector())
        {
            if (!before.count(i.mLocalID)) added = i.mLocalID;
        }
        if (added.isNull()) return LLSD().with("ok", false).with("error", "load");
        FileWatch w;
        w.id = added;
        w.path = path;
        sFiles.push_back(w);
        // a creator saves again and again: the file follows every save
        if (!gSavedSettings.getBOOL("FSLocalMeshAutoReload"))
        {
            gSavedSettings.setBOOL("FSLocalMeshAutoReload", true);
            lms()->refreshAutoReloadTimer();
        }
        return LLSD().with("ok", true).with("id", added.asString());
    }

    // ------------------------------------------------------------------
    // the stand-in: a worn object Local Mesh draws the file on
    // ------------------------------------------------------------------
    struct Applied
    {
        LLUUID         prim;
        LLVolumeParams orig;
        struct Face { LLUUID tex; LLColor4 color; F32 glow; };
        std::vector<Face> faces;   // the prim's own look, put back on unwear
        bool           madeMesh = false;   // a local "mesh" sculpt entry was added (plain prim)
    };
    LLUUID sStandIn;   // root object of the worn stand-in
    LLUUID sWornFile;  // the file shown on it
    std::vector<Applied> sApplied;

    std::string itemName(const LLViewerObject* obj)
    {
        const LLViewerInventoryItem* item = obj ? gInventory.getItem(obj->getAttachmentItemID()) : nullptr;
        return item ? item->getName() : std::string();
    }

    bool looksLikeStandIn(const std::string& name)
    {
        const std::string n = lower(name);
        return n.find("stand-in") != std::string::npos || n.find("stand in") != std::string::npos ||
               n.find("standin") != std::string::npos || n.find("mg fit") != std::string::npos ||
               n.find("placeholder") != std::string::npos;
    }

    std::vector<LLViewerObject*> primsOf(LLViewerObject* root)
    {
        std::vector<LLViewerObject*> out;
        if (!root) return out;
        out.push_back(root);
        for (const auto& c : root->getChildren())
        {
            if (c.notNull() && !c->isDead()) out.push_back(c.get());
        }
        return out;
    }

    bool isMyAttachment(LLViewerObject* obj)
    {
        return obj && !obj->isDead() && obj->isAttachment() && isAgentAvatarValid() &&
               obj->getAvatar() == gAgentAvatarp.get() && !obj->isHUDAttachment();
    }

    // <MG> Level-of-detail preview: the worn garment shown as the uploader's default levels would
    // make it (meshoptimizer, a third of the triangles per level), in this viewer only. The points
    // stay; only each face's triangle list is swapped (the original is kept and put back).
    struct LodPreview
    {
        S32 level = 0;                      // 0 high .. 3 lowest
        struct Face { LLUUID prim; S32 face = 0; LLVolume* vol = nullptr; std::vector<U16> idx; };
        std::vector<Face> faces;
    } sLod;

    LLVOVolume* lodVolume(const LLUUID& prim)
    {
        LLViewerObject* o = gObjectList.findObject(prim);
        if (!o || o->isDead() || o->getPCode() != LL_PCODE_VOLUME) return nullptr;
        return static_cast<LLVOVolume*>(o);
    }

    void lodRestore()
    {
        std::set<LLVOVolume*> touched;
        for (auto& lf : sLod.faces)
        {
            LLVOVolume* v = lodVolume(lf.prim);
            LLVolume* vol = v ? v->getVolume() : nullptr;
            if (!vol || vol != lf.vol || lf.face >= vol->getNumVolumeFaces()) continue;   // reloaded: nothing to put back
            LLVolumeFace& f = vol->getVolumeFace(lf.face);
            f.resizeIndices((S32)lf.idx.size());
            if (!lf.idx.empty() && f.mIndices) memcpy(f.mIndices, lf.idx.data(), lf.idx.size() * sizeof(U16));
            f.destroyOctree();
            touched.insert(v);
        }
        for (LLVOVolume* v : touched) if (v->mDrawable.notNull()) gPipeline.markRebuild(v->mDrawable, LLDrawable::REBUILD_ALL);
        sLod.faces.clear();
        sLod.level = 0;
    }

    // show a level; the answer has the triangles and where your viewer switches between levels
    LLSD lodShow(S32 level)
    {
        LLSD r = LLSD().with("ok", true);
        level = llclamp(level, 0, 3);
        // a face from before a reload is not ours to keep
        for (const auto& lf : sLod.faces)
        {
            LLVOVolume* v = lodVolume(lf.prim);
            if (!v || v->getVolume() != lf.vol) { sLod.faces.clear(); sLod.level = 0; break; }
        }
        if (sLod.faces.empty())
            for (const auto& a : sApplied)
            {
                LLVOVolume* v = lodVolume(a.prim);
                LLVolume* vol = v && v->mIsLocalMesh ? v->getVolume() : nullptr;
                if (!vol) continue;
                for (S32 fi = 0; fi < vol->getNumVolumeFaces(); ++fi)
                {
                    const LLVolumeFace& f = vol->getVolumeFace(fi);
                    if (!f.mIndices || f.mNumIndices < 3) continue;
                    LodPreview::Face lf;
                    lf.prim = a.prim; lf.face = fi; lf.vol = vol;
                    lf.idx.assign(f.mIndices, f.mIndices + f.mNumIndices);
                    sLod.faces.push_back(lf);
                }
            }
        if (sLod.faces.empty()) return r.with("ok", false).with("error", "not_worn");
        S32 trisHigh = 0, trisShown = 0;
        std::set<LLVOVolume*> touched;
        for (auto& lf : sLod.faces)
        {
            LLVOVolume* v = lodVolume(lf.prim);
            LLVolume* vol = v ? v->getVolume() : nullptr;
            if (!vol || vol != lf.vol || lf.face >= vol->getNumVolumeFaces()) continue;
            LLVolumeFace& f = vol->getVolumeFace(lf.face);
            const U64 n0 = lf.idx.size();
            trisHigh += (S32)(n0 / 3);
            std::vector<U16> out(lf.idx);
            U64 n = n0;
            if (level > 0)
            {
                // the uploader's default: a third of the triangles per level
                U64 tris = (n0 / 3) / (U64)pow(3.0, level);
                const U64 target = llmax<U64>(1, tris) * 3;
                F32 err = 0.f;
                n = LLMeshOptimizer::simplify(out.data(), lf.idx.data(), n0, f.mPositions, (U64)f.mNumVertices, sizeof(LLVector4a), target, 1.f, false, &err);
                if (n > target + target / 4)   // seams held it back: as the uploader then does, the sloppy way
                    n = LLMeshOptimizer::simplify(out.data(), lf.idx.data(), n0, f.mPositions, (U64)f.mNumVertices, sizeof(LLVector4a), target, 1.f, true, &err);
                if (n < 3) { out.assign(lf.idx.begin(), lf.idx.begin() + 3); n = 3; }   // a face never vanishes entirely
            }
            f.resizeIndices((S32)n);
            if (f.mIndices) memcpy(f.mIndices, out.data(), (size_t)n * sizeof(U16));
            f.destroyOctree();
            trisShown += (S32)(n / 3);
            touched.insert(v);
        }
        for (LLVOVolume* v : touched) if (v->mDrawable.notNull()) gPipeline.markRebuild(v->mDrawable, LLDrawable::REBUILD_ALL);
        sLod.level = level;
        if (level == 0) sLod.faces.clear();
        // where your viewer switches (rigged on an avatar: the avatar's box, your LOD factor)
        r["level"] = level;
        r["tris"] = trisShown;
        r["tris_high"] = trisHigh;
        if (isAgentAvatarValid())
        {
            const LLVector3* box = gAgentAvatarp->getLastAnimExtents();
            const F32 radius = (box[1] - box[0]).magVec();
            const F32 factor = LLVOVolume::sLODFactor;
            const F32 k = radius * factor / ((F_PI / 3.f) * llmax(0.01f, LLVOVolume::sDistanceFactor));
            r["d_medium"] = k / 0.24f;
            r["d_low"] = k / 0.06f;
            r["d_lowest"] = k / 0.03f;
            r["factor"] = factor;
        }
        return r;
    }

    void weightViewOff();
    void unwear()
    {
        lodRestore();
        weightViewOff();   // the garment's own look back before the stand-in's is restored
        for (const auto& a : sApplied)
        {
            LLViewerObject* o = gObjectList.findObject(a.prim);
            if (!o || o->isDead() || o->getPCode() != LL_PCODE_VOLUME) continue;
            LLVOVolume* v = static_cast<LLVOVolume*>(o);
            v->mIsLocalMesh = false;
            if (a.madeMesh)
            {
                // back to a plain prim: no sculpt entry, no skin (local only, nothing sent)
                v->setParameterEntryInUse(LLNetworkData::PARAMS_SCULPT, false, false);
                v->notifySkinInfoUnavailable();
            }
            v->setVolume(a.orig, 3);
            for (size_t i = 0; i < a.faces.size() && i < (size_t)v->getNumTEs(); ++i)
            {
                v->setTETexture((U8)i, a.faces[i].tex);
                v->setTEColor((U8)i, a.faces[i].color);
                v->setTEGlow((U8)i, a.faces[i].glow);
                // <MG> what Look set on this screen goes too (materials, PBR, shine, full bright, repeats)
                v->setRenderMaterialID((S32)i, LLUUID::null, false, true);
                v->setTEMaterialParams((U8)i, nullptr);
                v->setTEShiny((U8)i, 0);
                v->setTEFullbright((U8)i, 0);
                v->setTEScale((U8)i, 1.f, 1.f);
            }
            v->markForUpdate();
            if (v->mDrawable.notNull()) gPipeline.markRebuild(v->mDrawable, LLDrawable::REBUILD_ALL);
        }
        sApplied.clear();
        sStandIn.setNull();
        sWornFile.setNull();
    }

    // what the renderer thinks of the stand-in's prims, in the viewer log (#MGCreator)
    void logRenderState(const char* when)
    {
        for (const auto& a : sApplied)
        {
            LLViewerObject* o = gObjectList.findObject(a.prim);
            if (!o || o->isDead() || o->getPCode() != LL_PCODE_VOLUME) continue;
            LLVOVolume* v = static_cast<LLVOVolume*>(o);
            LLVolume* vol = v->getVolume();
            S32 faces = 0, riggedFaces = 0;
            bool drawRigged = false;
            if (v->mDrawable.notNull())
            {
                drawRigged = v->mDrawable->isState(LLDrawable::RIGGED);
                faces = v->mDrawable->getNumFaces();
                for (S32 i = 0; i < faces; ++i)
                {
                    LLFace* fc = v->mDrawable->getFace(i);
                    if (fc && fc->isState(LLFace::RIGGED)) ++riggedFaces;
                }
            }
            LL_INFOS("MGCreator") << when << " prim " << a.prim
                << " localMesh=" << v->mIsLocalMesh << " madeMesh=" << a.madeMesh
                << " isSculpted=" << v->isSculpted() << " isMesh=" << v->isMesh()
                << " skin=" << (v->getSkinInfo() != nullptr) << " attach=" << v->isAttachment()
                << " animated=" << v->isAnimatedObject()
                << " volMesh=" << (vol ? vol->getParams().isMeshSculpt() : false)
                << " loaded=" << (vol ? vol->isMeshAssetLoaded() : false)
                << " lod=" << v->getLOD() << " volFaces=" << (vol ? vol->getNumVolumeFaces() : -1)
                << " drawRigged=" << drawRigged << " faces=" << faces << " riggedFaces=" << riggedFaces
                << " sculptId=" << (vol ? vol->getParams().getSculptID() : LLUUID::null) << LL_ENDL;
        }
    }
    LLFrameTimer sDiagTimer;
    bool sDiagPending = false;

    struct StandInFinder : public LLInventoryCollectFunctor
    {
        bool operator()(LLInventoryCategory* cat, LLInventoryItem* item) override
        {
            return item && item->getType() == LLAssetType::AT_OBJECT && looksLikeStandIn(item->getName());
        }
    };

    // ------------------------------------------------------------------
    // clip finder
    // ------------------------------------------------------------------
    std::set<LLUUID> sBodyOff;   // worn items left out of the body (by item id)

    bool bodyDefaultOff(const std::string& name)
    {
        const std::string n = lower(name);
        if (n.find("hair") != std::string::npos || n.find("lash") != std::string::npos) return true;
        static const char* BODY[] = { "body", "head", "hand", "feet", "foot", "skin", "torso", "legs" };
        for (const char* w : BODY) if (n.find(w) != std::string::npos) return false;
        return true;   // clothes and accessories: left out until the creator adds them
    }

    // ------------------------------------------------------------------
    // clip markers: red dots where the body poked through at the last check. Each point
    // is kept in its body bone's frame, so the dots follow the avatar and the camera.
    // ------------------------------------------------------------------
    struct Marker { std::string joint; LLVector3 local; };
    std::vector<Marker> sMarkers;
    bool sMarkersOn = true;

    bool sPoseBones = false;   // posing bones to fit a garment: the poser's gizmo has the mouse

    class MGCreatorMarkers : public LLView
    {
    public:
        MGCreatorMarkers(const LLView::Params& p) : LLView(p) {}
        // posing bones: Ctrl+click on a joint dot picks it (anything else goes on to the poser's gizmo)
        bool handleMouseDown(S32 x, S32 y, MASK mask) override
        {
            if (!sPoseBones || !(mask & MASK_CONTROL) || !gViewerWindow) return false;
            const LLRect r = gViewerWindow->getRootView()->getLocalRect();
            LLSD a = LLSD().with("x", (F64)x / llmax(1, r.getWidth())).with("y", 1.0 - (F64)y / llmax(1, r.getHeight())).with("group", "body");
            const LLSD res = MGPhonePoser::runCommand("pose_pick", a);
            if (!res["ok"].asBoolean()) return false;
            push(LLSD().with("type", "lab_pose_joint").with("joint", res["joint"]));
            return true;
        }
        void draw() override
        {
            if (sPoseBones) MGPhonePoser::drawMarkers();
            if (sMarkersOn && !sMarkers.empty() && isAgentAvatarValid())
            {
                LLViewerCamera* cam = LLViewerCamera::getInstance();
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                for (const Marker& m : sMarkers)
                {
                    LLJoint* j = gAgentAvatarp->getJoint(m.joint);
                    if (!j) continue;
                    const LLVector3 w = j->getWorldPosition() + m.local * j->getWorldRotation();
                    LLCoordGL c;
                    if (!cam->projectPosAgentToScreen(w, c, false)) continue;
                    gGL.color4f(0.f, 0.f, 0.f, 0.6f);
                    gl_circle_2d((F32)c.mX, (F32)c.mY, 4.5f, 12, true);
                    gGL.color4f(1.f, 0.3f, 0.33f, 1.f);
                    gl_circle_2d((F32)c.mX, (F32)c.mY, 3.f, 12, true);
                }
            }
            LLView::draw();
        }
    };
    MGCreatorMarkers* sMarkerView = nullptr;

    void showMarkers()
    {
        if (!sMarkerView && gViewerWindow && gViewerWindow->getRootView())
        {
            LLView::Params p;
            p.name = "mg_creator_markers";
            p.rect = gViewerWindow->getRootView()->getLocalRect();
            p.mouse_opaque = false;
            p.follows.flags = FOLLOWS_ALL;
            sMarkerView = new MGCreatorMarkers(p);
            gViewerWindow->getRootView()->addChild(sMarkerView);
            gViewerWindow->getRootView()->sendChildToBack(sMarkerView);
        }
        if (sMarkerView) sMarkerView->setVisible((sMarkersOn && !sMarkers.empty()) || sPoseBones);
    }

    void clearMarkers()
    {
        sMarkers.clear();
        if (sMarkerView) sMarkerView->setVisible(false);
    }

    // ------------------------------------------------------------------
    // <MG> the workspace's toolbar in the 3D view (top middle, as Mesh's): the page says which
    // buttons (lab_bar) and hears the clicks (lab_bar_click). It refreshes it every 2 s; a bar not
    // refreshed for 6 s (the phone closed) goes away.
    // ------------------------------------------------------------------
    struct BarItem { std::string id, icon, tip; bool on = false, sep = false; };
    std::vector<BarItem> sBar;
    std::string sBarHint;
    LLFrameTimer sBarTimer;
    void editIcon(const char* id, S32 icx, S32 icy, const LLColor4& col);

    class MGCreatorBar : public LLView
    {
    public:
        MGCreatorBar(const LLView::Params& p) : LLView(p) {}
        S32 mHover = -1;
        static bool live() { return !sBar.empty() && sBarTimer.getElapsedTimeF32() < 6.f; }
        LLRect itemRect(S32 i) const
        {
            const LLRect r = gViewerWindow->getWorldViewRectScaled();
            const S32 bw = 30, gap = 4, sep = 10;
            // a separator (a gap) before an item marked sep
            S32 total = 0;
            for (size_t k = 0; k < sBar.size(); ++k) total += bw + gap + (k > 0 && sBar[k].sep ? sep : 0);
            S32 x = r.getCenterX() - total / 2;
            for (S32 k = 1; k <= i && k < (S32)sBar.size(); ++k) x += bw + gap + (sBar[k].sep ? sep : 0);
            const S32 top = r.mTop - 70;
            return LLRect(x, top, x + bw, top - bw);
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
            push(LLSD().with("type", "lab_bar_click").with("id", sBar[i].id));
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
        void draw() override
        {
            if (!live()) return;
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            const LLRect a = itemRect(0), b = itemRect((S32)sBar.size() - 1);
            gGL.color4f(0.03f, 0.06f, 0.09f, 0.86f);
            gl_rect_2d(a.mLeft - 6, a.mTop + 6, b.mRight + 6, b.mBottom - 6, true);
            for (S32 i = 0; i < (S32)sBar.size(); ++i)
            {
                const LLRect r = itemRect(i);
                const bool on = sBar[i].on, hov = mHover == i;
                if (on) { gGL.color4f(0.07f, 0.19f, 0.24f, 1.f); gl_rect_2d(r, true); }
                else if (hov) { gGL.color4f(1.f, 1.f, 1.f, 0.08f); gl_rect_2d(r, true); }
                editIcon(sBar[i].icon.c_str(), r.getCenterX(), r.getCenterY(), on ? LLColor4(0.13f, 0.78f, 0.9f, 1.f) : LLColor4(0.62f, 0.68f, 0.76f, 1.f));
            }
            const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
            S32 line = a.mBottom - 18;
            if (mHover >= 0 && mHover < (S32)sBar.size())
            {
                const LLRect r = itemRect(mHover);
                font->renderUTF8(sBar[mHover].tip, 0, (F32)r.mLeft, (F32)line, LLColor4::white, LLFontGL::LEFT, LLFontGL::BASELINE, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
                line -= 16;
            }
            if (!sBarHint.empty())
                font->renderUTF8(sBarHint, 0, (F32)a.mLeft, (F32)line, LLColor4(0.8f, 0.86f, 0.92f, 1.f), LLFontGL::LEFT, LLFontGL::BASELINE, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
            LLView::draw();
        }
    };
    MGCreatorBar* sBarView = nullptr;

    void showBar()
    {
        if (!sBarView && gViewerWindow && gViewerWindow->getRootView())
        {
            LLView::Params p;
            p.name = "mg_creator_bar";
            p.rect = gViewerWindow->getRootView()->getLocalRect();
            p.mouse_opaque = false;
            p.follows.flags = FOLLOWS_ALL;
            sBarView = new MGCreatorBar(p);
            gViewerWindow->getRootView()->addChild(sBarView);
        }
        if (sBarView) sBarView->setVisible(!sBar.empty());
    }

    struct Tri { LLVector3 a, b, c; };

    struct Grid
    {
        F32 cell = 0.04f;
        std::unordered_map<U64, std::vector<S32>> map;
        static U64 key(S32 x, S32 y, S32 z)
        {
            const U64 o = 1u << 20;
            return ((U64)(x + o) << 42) | ((U64)(y + o) << 21) | (U64)(z + o);
        }
        S32 c(F32 v) const { return (S32)std::floor(v / cell); }
    };

    // +1 when (b-a)x(c-a) points the way the mesh's own normals do, -1 when the maker's
    // triangle order is the other way round (seen on a body: every point counted as clipping)
    F32 windingSign(const LLVolumeFace& sf)
    {
        if (!sf.mPositions || !sf.mNormals || !sf.mIndices) return 1.f;
        S32 agree = 0;
        const S32 step = llmax(3, (sf.mNumIndices / 3 / 2000) * 3);   // ~2000 triangles are plenty
        for (S32 i = 0; i + 2 < sf.mNumIndices; i += step)
        {
            const U16 ia = sf.mIndices[i], ib = sf.mIndices[i + 1], ic = sf.mIndices[i + 2];
            if (ia >= sf.mNumVertices || ib >= sf.mNumVertices || ic >= sf.mNumVertices) continue;
            LLVector3 a(sf.mPositions[ia].getF32ptr()), b(sf.mPositions[ib].getF32ptr()), c(sf.mPositions[ic].getF32ptr());
            LLVector3 n(sf.mNormals[ia].getF32ptr());
            n += LLVector3(sf.mNormals[ib].getF32ptr());
            n += LLVector3(sf.mNormals[ic].getF32ptr());
            const F32 d = ((b - a) % (c - a)) * n;
            if (d > 0.f) ++agree; else if (d < 0.f) --agree;
        }
        return agree < 0 ? -1.f : 1.f;
    }

    // ray (two-sided) against a triangle: the signed distance along dir, or false
    bool rayTri(const LLVector3& o, const LLVector3& d, const Tri& t, F32& out)
    {
        const LLVector3 e1 = t.b - t.a, e2 = t.c - t.a;
        const LLVector3 p = d % e2;
        const F32 det = e1 * p;
        if (std::fabs(det) < 1e-12f) return false;
        const F32 inv = 1.f / det;
        const LLVector3 s = o - t.a;
        const F32 u = (s * p) * inv;
        if (u < 0.f || u > 1.f) return false;
        const LLVector3 q = s % e1;
        const F32 v = (d * q) * inv;
        if (v < 0.f || u + v > 1.f) return false;
        out = (e2 * q) * inv;
        return true;
    }

    // <MG> Alpha (2026-11-14): while the stress test runs, the body points the garment covers are
    // counted on the body faces that show the Bakes on Mesh skin (their UVs are Second Life's own
    // upper / lower layout). What is covered in every pose becomes the alpha mask (see alphaMake).
    struct AlphaFace
    {
        S32 channel = 0;                 // 0 upper body, 1 lower body
        S32 samples = 0;                 // test samples this face was looked at in
        std::vector<LLVector2> uv;
        std::vector<U16> idx;
        std::vector<U16> covered;        // per point: samples it had the garment over it
    };
    struct AlphaState
    {
        bool collect = false;            // a stress test runs: count
        bool complete = false;           // ... and went through every pose
        S32 samples = 0;
        std::map<std::pair<LLUUID, S32>, AlphaFace> faces;
        LLPointer<LLImageRaw> raw[2];    // the masks made (white = shown, transparent black = hidden)
        F32 hidden[2] = { 0.f, 0.f };    // share of each layout hidden
        std::string files[2];
        S32 pending = 0;                 // uploads still running
        bool failed = false;
        LLUUID asset[2];
        std::string name;
        bool wear = true;
    } sAlpha;

    // the garment (the stand-in's prims) and the body (other worn rigged objects) are
    // skinned in the current pose, in agent space. A body point pokes through when the
    // garment surface nearest along the body's outward normal lies UNDER the skin.
    LLSD clipCheck()
    {
        const F32 REACH = 0.025f;   // look 2.5 cm in and out of the skin
        const F32 DEPTH = 0.0008f;  // under the skin by more than 0.8 mm = clipping
        LLSD out = LLSD().with("ok", false);
        if (!isAgentAvatarValid()) return out.with("error", "avatar");

        std::vector<Tri> tris;
        std::set<LLUUID> garment;
        // the regions the garment is rigged to: a hand passing through pants is the pose,
        // not the pants, so only body points in these regions are judged
        std::set<std::string> garmentRegions;
        for (const auto& a : sApplied)
        {
            LLViewerObject* o = gObjectList.findObject(a.prim);
            if (!o || o->isDead() || o->getPCode() != LL_PCODE_VOLUME) continue;
            LLVOVolume* v = static_cast<LLVOVolume*>(o);
            garment.insert(v->getID());
            if (!v->isRiggedMesh()) continue;
            if (const LLMeshSkinInfo* gs = v->getSkinInfo())
            {
                for (const auto& j : gs->mJointNames) garmentRegions.insert(regionOf(j));
            }
            v->updateRiggedVolume(true, LLRiggedVolume::UPDATE_ALL_FACES, false);
            LLRiggedVolume* rv = v->getRiggedVolume();
            if (!rv) continue;
            for (S32 f = 0; f < rv->getNumVolumeFaces(); ++f)
            {
                const LLVolumeFace& vf = rv->getVolumeFace(f);
                if (!vf.mPositions || !vf.mIndices) continue;
                for (S32 i = 0; i + 2 < vf.mNumIndices; i += 3)
                {
                    const U16 ia = vf.mIndices[i], ib = vf.mIndices[i + 1], ic = vf.mIndices[i + 2];
                    if (ia >= vf.mNumVertices || ib >= vf.mNumVertices || ic >= vf.mNumVertices) continue;
                    Tri t;
                    t.a.set(vf.mPositions[ia].getF32ptr());
                    t.b.set(vf.mPositions[ib].getF32ptr());
                    t.c.set(vf.mPositions[ic].getF32ptr());
                    tris.push_back(t);
                }
            }
        }
        if (tris.empty()) return out.with("error", sApplied.empty() ? "not_worn" : "not_rigged");

        LLVector3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
        Grid grid;
        for (S32 i = 0; i < (S32)tris.size(); ++i)
        {
            const Tri& t = tris[i];
            LLVector3 tl, th;
            for (S32 k = 0; k < 3; ++k)
            {
                tl.mV[k] = llmin(t.a.mV[k], llmin(t.b.mV[k], t.c.mV[k]));
                th.mV[k] = llmax(t.a.mV[k], llmax(t.b.mV[k], t.c.mV[k]));
                lo.mV[k] = llmin(lo.mV[k], tl.mV[k]);
                hi.mV[k] = llmax(hi.mV[k], th.mV[k]);
            }
            for (S32 x = grid.c(tl.mV[0]); x <= grid.c(th.mV[0]); ++x)
                for (S32 y = grid.c(tl.mV[1]); y <= grid.c(th.mV[1]); ++y)
                    for (S32 z = grid.c(tl.mV[2]); z <= grid.c(th.mV[2]); ++z)
                        grid.map[Grid::key(x, y, z)].push_back(i);
        }
        for (S32 k = 0; k < 3; ++k) { lo.mV[k] -= REACH; hi.mV[k] += REACH; }

        std::vector<Marker> found;   // every poking point, in its bone's frame
        if (sAlpha.collect) ++sAlpha.samples;
        std::vector<U32> stamp(tris.size(), 0);
        U32 tick = 0;
        // <MG> Alpha: is there garment anywhere up to 15 cm out along the skin's normal? (cells walked along the line)
        const F32 ALPHA_OUT = 0.15f;
        auto outwardHit = [&](const LLVector3& p, const LLVector3& nb) -> bool
        {
            ++tick;
            for (F32 s = REACH; s <= ALPHA_OUT + grid.cell * 0.5f; s += grid.cell * 0.5f)
            {
                const LLVector3 q = p + nb * s;
                auto it = grid.map.find(Grid::key(grid.c(q.mV[0]), grid.c(q.mV[1]), grid.c(q.mV[2])));
                if (it == grid.map.end()) continue;
                for (S32 ti : it->second)
                {
                    if (stamp[ti] == tick) continue;
                    stamp[ti] = tick;
                    F32 d;
                    if (rayTri(p, nb, tris[ti], d) && d > 0.f && d <= ALPHA_OUT) return true;
                }
            }
            return false;
        };
        S32 covered = 0, poke = 0, bodies = 0;
        std::map<std::string, S32> byRegion;

        for (const auto& kv : gAgentAvatarp->mAttachmentPoints)
        {
            LLViewerJointAttachment* att = kv.second;
            if (!att || att->getIsHUDAttachment()) continue;
            for (const auto& ao : att->mAttachedObjects)
            {
                LLViewerObject* root = ao.get();
                if (!root || root->isDead()) continue;
                if (sBodyOff.count(root->getAttachmentItemID())) continue;
                for (LLViewerObject* o : primsOf(root))
                {
                    if (!o || o->getPCode() != LL_PCODE_VOLUME || garment.count(o->getID())) continue;
                    LLVOVolume* v = static_cast<LLVOVolume*>(o);
                    if (v->mIsLocalMesh || !v->isRiggedMesh()) continue;
                    const LLMeshSkinInfo* skin = v->getSkinInfo();
                    LLVolume* src = v->getVolume();
                    if (!skin || !src) continue;
                    v->updateRiggedVolume(true, LLRiggedVolume::UPDATE_ALL_FACES, false);
                    LLRiggedVolume* rv = v->getRiggedVolume();
                    if (!rv || rv->getNumVolumeFaces() != src->getNumVolumeFaces()) continue;
                    bool counted = false;
                    for (S32 f = 0; f < rv->getNumVolumeFaces(); ++f)
                    {
                        // hidden faces (alpha cuts, invisible) are not part of the visible body
                        const LLTextureEntry* te = v->getTE((U8)f);
                        if (te && (te->getColor().mV[VALPHA] < 0.05f || te->getID() == IMG_INVISIBLE)) continue;
                        const LLVolumeFace& sf = src->getVolumeFace(f);
                        const LLVolumeFace& df = rv->getVolumeFace(f);
                        if (!df.mPositions || !df.mIndices || df.mNumVertices != sf.mNumVertices) continue;
                        const S32 n = df.mNumVertices;
                        // <MG> Alpha: a face showing the Bakes on Mesh skin counts what the garment covers
                        AlphaFace* af = nullptr;
                        if (sAlpha.collect && te && sf.mTexCoords && sf.mIndices)
                        {
                            const LLUUID& tid = te->getID();
                            const S32 ch = tid == IMG_USE_BAKED_UPPER ? 0 : tid == IMG_USE_BAKED_LOWER ? 1 : -1;
                            if (ch >= 0)
                            {
                                AlphaFace& a = sAlpha.faces[{ v->getID(), f }];
                                if ((S32)a.uv.size() != n || a.channel != ch)
                                {
                                    a = AlphaFace();
                                    a.channel = ch;
                                    a.uv.resize(n);
                                    for (S32 j = 0; j < n; ++j) a.uv[j].set(sf.mTexCoords[j].mV[0], sf.mTexCoords[j].mV[1]);
                                    a.idx.assign(sf.mIndices, sf.mIndices + sf.mNumIndices);
                                    a.covered.assign(n, 0);
                                }
                                ++a.samples;
                                af = &a;
                            }
                        }
                        // outward normals from the skinned triangles, turned the way the mesh says is out
                        const F32 wind = windingSign(sf);
                        std::vector<LLVector3> nrm(n, LLVector3::zero);
                        for (S32 i = 0; i + 2 < df.mNumIndices; i += 3)
                        {
                            const U16 ia = df.mIndices[i], ib = df.mIndices[i + 1], ic = df.mIndices[i + 2];
                            if (ia >= n || ib >= n || ic >= n) continue;
                            LLVector3 a(df.mPositions[ia].getF32ptr()), b(df.mPositions[ib].getF32ptr()), c(df.mPositions[ic].getF32ptr());
                            const LLVector3 fn = ((b - a) % (c - a)) * wind;
                            nrm[ia] += fn; nrm[ib] += fn; nrm[ic] += fn;
                        }
                        for (S32 j = 0; j < n; ++j)
                        {
                            const LLVector3 p(df.mPositions[j].getF32ptr());
                            if (p.mV[0] < lo.mV[0] || p.mV[1] < lo.mV[1] || p.mV[2] < lo.mV[2] ||
                                p.mV[0] > hi.mV[0] || p.mV[1] > hi.mV[1] || p.mV[2] > hi.mV[2]) continue;
                            LLVector3 nb = nrm[j];
                            if (nb.normVec() < 1e-12f) continue;
                            const LLVector3 s0 = p - nb * REACH, s1 = p + nb * REACH;
                            ++tick;
                            bool hit = false;
                            F32 best = 0.f;
                            for (S32 x = grid.c(llmin(s0.mV[0], s1.mV[0])); x <= grid.c(llmax(s0.mV[0], s1.mV[0])); ++x)
                                for (S32 y = grid.c(llmin(s0.mV[1], s1.mV[1])); y <= grid.c(llmax(s0.mV[1], s1.mV[1])); ++y)
                                    for (S32 z = grid.c(llmin(s0.mV[2], s1.mV[2])); z <= grid.c(llmax(s0.mV[2], s1.mV[2])); ++z)
                                    {
                                        auto it = grid.map.find(Grid::key(x, y, z));
                                        if (it == grid.map.end()) continue;
                                        for (S32 ti : it->second)
                                        {
                                            if (stamp[ti] == tick) continue;
                                            stamp[ti] = tick;
                                            F32 d;
                                            if (!rayTri(p, nb, tris[ti], d) || std::fabs(d) > REACH) continue;
                                            if (!hit || std::fabs(d) < std::fabs(best)) { best = d; hit = true; }
                                        }
                                    }
                            if (!hit)
                            {
                                // <MG> Alpha: a loose garment (a coat, the thin shape) is farther than the clip reach but still over the skin
                                if (af && af->covered[j] < 65535 && outwardHit(p, nb)) ++af->covered[j];
                                continue;   // no garment over this point (for the clipping)
                            }
                            if (af && af->covered[j] < 65535) ++af->covered[j];
                            // the region: the vertex's strongest joint
                            std::string region = "other";
                            std::string strongest = "mPelvis";
                            if (sf.mWeights)
                            {
                                const F32* w = sf.mWeights[j].getF32ptr();
                                S32 bi = -1;
                                F32 bw = -1.f;
                                for (S32 k = 0; k < 4; ++k)
                                {
                                    const S32 idx = (S32)std::floor(w[k]);
                                    const F32 wt = w[k] - std::floor(w[k]);
                                    if (wt > bw) { bw = wt; bi = idx; }
                                }
                                if (bi >= 0 && bi < (S32)skin->mJointNames.size())
                                {
                                    strongest = skin->mJointNames[bi];
                                    region = regionOf(strongest);
                                }
                            }
                            if (!garmentRegions.empty() && !garmentRegions.count(region)) continue;
                            ++covered;
                            counted = true;
                            if (best >= -DEPTH) continue;
                            ++poke;
                            ++byRegion[region];
                            LLJoint* jt = gAgentAvatarp->getJoint(strongest);
                            if (!jt) { strongest = "mPelvis"; jt = gAgentAvatarp->getJoint(strongest); }
                            if (jt) found.push_back({ strongest, (p - jt->getWorldPosition()) * ~jt->getWorldRotation() });
                        }
                    }
                    if (counted) ++bodies;
                }
            }
        }

        // the dots: at most 400, spread evenly over what was found
        sMarkers.clear();
        const size_t step = found.size() > 400 ? found.size() / 400 + 1 : 1;
        for (size_t i = 0; i < found.size(); i += step) sMarkers.push_back(found[i]);
        showMarkers();

        LLSD regions = LLSD::emptyMap();
        for (const auto& kv : byRegion) regions[kv.first] = kv.second;
        out["ok"] = true;
        out["covered"] = covered;
        out["poke"] = poke;
        out["regions"] = regions;
        out["bodies"] = bodies;
        out["tris"] = (S32)tris.size();
        return out;
    }

    // ------------------------------------------------------------------
    // stress test and hold
    // ------------------------------------------------------------------
    struct Step
    {
        const char*   key;
        const char*   name;
        const LLUUID* anim;
        S32           shape;   // 0 as is, 1 heavy, 2 thin
        F32           settle;
        S32           samples;
        F32           every;
    };
    const Step STEPS[] = {
        { "stand",  "Stand",             nullptr,                              0, 0.3f, 1, 0.f },
        { "walk",   "Walk",              &ANIM_AGENT_WALK,                     0, 0.8f, 4, 0.25f },
        { "sit",    "Sit on the ground", &ANIM_AGENT_SIT_GROUND_CONSTRAINED,   0, 1.6f, 1, 0.f },
        { "crouch", "Crouch",            &ANIM_AGENT_CROUCH,                   0, 1.0f, 1, 0.f },
        { "armsup", "Arms up",           &ANIM_AGENT_STRETCH,                  0, 1.0f, 3, 0.4f },
        { "heavy",  "Heavy shape",       nullptr,                              1, 0.6f, 1, 0.f },
        { "thin",   "Thin shape",        nullptr,                              2, 0.6f, 1, 0.f },
    };
    const S32 NSTEPS = (S32)(sizeof(STEPS) / sizeof(STEPS[0]));

    // the sliders fitted mesh follows; heavy / thin = this fraction of each slider's range
    struct Slider { S32 id; F32 heavy; F32 thin; };
    const Slider SLIDERS[] = {
        { 637, 1.0f, 0.0f },   // Body Fat
        { 157, 1.0f, 0.0f },   // Belly Size
        { 795, 1.0f, 0.0f },   // Butt Size
        { 105, 0.9f, 0.1f },   // Breast Size
        { 753, 0.6f, 0.0f },   // Saddle Bags
        { 676, 0.8f, 0.2f },   // Love Handles
        { 37,  0.75f, 0.3f },  // Hip Width
    };

    struct Run
    {
        bool  active = false;  // a test runs
        bool  hold = false;    // one pose / shape held
        S32   step = -1;
        S32   sampled = 0;
        LLFrameTimer timer;
        bool  aoWas = false;
        bool  aoPaused = false;
        const LLUUID* anim = nullptr;
        std::map<S32, F32> saved;   // slider weights before the test
        std::map<std::string, S32> regions;
        S32   poke = 0;
        S32   covered = 0;
        bool  fit = false;     // "Make it fit" runs the same poses
        S32   pushed = 0;      // garment points pushed out in this step (fit)
        S32   left = 0;        // <MG> clipping still there after the fit corrected this step (the stress test's rule)
        S32   deep = 0;        // ... of it, deeper than Max push (the fit leaves it to the creator)
        S32   squashed = 0;    // pushes skipped: the skinning squashes moves there (weights, not shape)
    } sRun;

    void pauseAO()
    {
        if (sRun.aoPaused) return;
        sRun.aoWas = gSavedPerAccountSettings.getBOOL("UseAO");
        if (sRun.aoWas) gSavedPerAccountSettings.setBOOL("UseAO", false);
        sRun.aoPaused = true;
    }

    void resumeAO()
    {
        if (!sRun.aoPaused) return;
        if (sRun.aoWas) gSavedPerAccountSettings.setBOOL("UseAO", true);
        sRun.aoPaused = false;
    }

    void applyShape(S32 kind)
    {
        if (!isAgentAvatarValid() || kind == 0) return;
        for (const auto& s : SLIDERS)
        {
            LLViewerVisualParam* p = (LLViewerVisualParam*)gAgentAvatarp->getVisualParam(s.id);
            if (!p || p->getCrossWearable()) continue;   // never touch a wearable
            if (!sRun.saved.count(s.id)) sRun.saved[s.id] = p->getWeight();
            const F32 frac = kind == 1 ? s.heavy : s.thin;
            const F32 w = lerp(p->getMinWeight(), p->getMaxWeight(), frac);
            // the base setter: this screen only, nothing saved, nothing baked
            gAgentAvatarp->LLCharacter::setVisualParamWeight(p, w, false);
        }
        gAgentAvatarp->updateVisualParams();
    }

    void restoreShape()
    {
        if (!isAgentAvatarValid() || sRun.saved.empty()) { sRun.saved.clear(); return; }
        for (const auto& kv : sRun.saved)
        {
            LLVisualParam* p = gAgentAvatarp->getVisualParam(kv.first);
            if (p) gAgentAvatarp->LLCharacter::setVisualParamWeight(p, kv.second, false);
        }
        sRun.saved.clear();
        gAgentAvatarp->updateVisualParams();
    }

    void stopAnim()
    {
        if (sRun.anim && isAgentAvatarValid()) gAgentAvatarp->stopMotion(*sRun.anim);
        sRun.anim = nullptr;
    }

    void startStep(S32 i)
    {
        sRun.step = i;
        sRun.sampled = 0;
        sRun.regions.clear();
        sRun.poke = 0;
        sRun.covered = 0;
        const Step& s = STEPS[i];
        stopAnim();
        restoreShape();
        if (s.anim && isAgentAvatarValid())
        {
            gAgentAvatarp->startMotion(*s.anim);
            sRun.anim = s.anim;
        }
        applyShape(s.shape);
        sRun.timer.reset();
        sRun.pushed = 0;
        sRun.left = 0;
        sRun.deep = 0;
        sRun.squashed = 0;
        if (sRun.active) push(LLSD().with("type", sRun.fit ? "lab_fit" : "lab_test").with("i", i).with("key", s.key).with("name", s.name).with("state", "run"));
    }

    void endRun(bool stopped)
    {
        sAlpha.collect = false;   // <MG> Alpha: counted only while a test runs
        const bool was = sRun.active;
        const bool fit = sRun.fit;
        stopAnim();
        restoreShape();
        resumeAO();
        sRun.active = false;
        sRun.hold = false;
        sRun.fit = false;
        sRun.step = -1;
        if (was && !fit) push(LLSD().with("type", "lab_test_done").with("stopped", stopped));
        if (was && fit && stopped) push(LLSD().with("type", "lab_fit_done").with("ok", false).with("stopped", true));
    }

    // ------------------------------------------------------------------
    // wear a loaded file on a worn stand-in (lab_wear, and the fitted file)
    // ------------------------------------------------------------------
    LLSD wearFile(const LLUUID& file, const LLUUID& standin, bool force)
    {
        LLSD result = LLSD().with("ok", true);
        LLLocalMeshFile* f = lms()->getFile(file);
        if (!f || f->getFileInfo().mStatus != LLLocalMeshFile::STATUS_ACTIVE) return result.with("ok", false).with("error", "file");
        LLViewerObject* root = gObjectList.findObject(standin);
        if (!isMyAttachment(root)) return result.with("ok", false).with("error", "standin");
        if (!force)
        {
            // Local Mesh: degenerate triangles can crash the renderer when applied
            const FileWatch* w = watchOf(file);
            LLSD c = runChecks(f, w ? w->path : std::string());
            if (c["thin"].asInteger() > 0) return result.with("ok", false).with("error", "thin").with("thin", c["thin"]);
        }
        endRun(true);
        unwear();
        clearMarkers();
        const auto prims = primsOf(root);
        const S32 nobj = (S32)f->getObjectVector().size();
        const S32 n = llmin(nobj, (S32)prims.size());
        for (S32 i = 0; i < n; ++i)
        {
            LLViewerObject* o = prims[i];
            if (!o || o->getPCode() != LL_PCODE_VOLUME) continue;
            LLVOVolume* v = static_cast<LLVOVolume*>(o);
            if (!v->getVolume()) continue;
            Applied a;
            a.prim = v->getID();
            a.orig = v->getVolume()->getParams();
            // Local Mesh draws the file with the prim's own textures (a plywood cube gives
            // wooden pants): show it plain white instead, on this screen only
            for (U8 te = 0; te < v->getNumTEs(); ++te)
            {
                const LLTextureEntry* e = v->getTE(te);
                if (!e) continue;
                a.faces.push_back({ e->getID(), e->getColor(), e->getGlow() });
                v->setTETexture(te, IMG_WHITE);
                v->setTEColor(te, LLColor4::white);
                v->setTEGlow(te, 0.f);
            }
            // The renderer skins an attachment only when the OBJECT says it is a mesh
            // (LLVOVolume::isMesh reads its sculpt entry); a plain box says no, and a
            // rigged file was drawn squeezed into the box. Give it a local mesh sculpt
            // entry first (local_origin false: never sent), the way a worn mesh would.
            if (!v->isMesh())
            {
                LLSculptParams sp;
                sp.setSculptTexture(f->getObjectVector()[i]->getVolumeParams().getSculptID(), LL_SCULPT_TYPE_MESH);
                v->setParameterEntry(LLNetworkData::PARAMS_SCULPT, sp, false);
                a.madeMesh = true;
            }
            sApplied.push_back(a);
            lms()->applyVObject(v->getID(), file, i, false);
        }
        sStandIn = standin;
        sWornFile = file;
        logRenderState("wear");
        sDiagPending = true;   // and again once the renderer has rebuilt it
        sDiagTimer.reset();
        result["shown"] = n;
        result["objects"] = nobj;
        result["prims"] = (S32)prims.size();
        return result;
    }

    const std::vector<MGRig::SlBone>& slSkeleton();
    S32 slIndex(const std::string& name);

    // the avatar's pose now as each bone's move from its rest place, in bind space (COLLADA layout):
    // posed world x rest world^-1 in the agent's space, brought to bind space through the pelvis
    std::map<std::string, MGCreatorWeights::Mat> poseMoves()
    {
        std::map<std::string, MGCreatorWeights::Mat> out;
        if (!isAgentAvatarValid()) return out;
        auto mat = [](const LLMatrix4& m) { return glm::make_mat4((const F32*)m.mMatrix); };
        std::map<LLJoint*, glm::mat4> rest;
        std::function<glm::mat4(LLJoint*)> restOf = [&](LLJoint* j) -> glm::mat4
        {
            auto it = rest.find(j);
            if (it != rest.end()) return it->second;
            LLJoint* p = j->getParent();
            glm::mat4 r;
            if (!p) r = mat(j->getWorldMatrix());   // the skeleton's root: where the avatar stands
            else
            {
                // no rotation at rest (Second Life's bones are bound unrotated): offset and scale only
                const LLVector3 pos = j->getPosition(), scl = j->getScale();
                r = restOf(p) * glm::translate(glm::mat4(1.f), glm::vec3(pos.mV[0], pos.mV[1], pos.mV[2])) * glm::scale(glm::mat4(1.f), glm::vec3(scl.mV[0], scl.mV[1], scl.mV[2]));
            }
            rest[j] = r;
            return r;
        };
        LLJoint* pel = gAgentAvatarp->getJoint("mPelvis");
        if (!pel) return out;
        const auto& sl = slSkeleton();
        glm::vec3 pb(0.f, 0.f, 1.067f);
        const S32 pi = slIndex("mPelvis");
        if (pi >= 0) pb = glm::vec3((F32)sl[pi].world[0], (F32)sl[pi].world[1], (F32)sl[pi].world[2]);
        const glm::mat4 K = restOf(pel) * glm::translate(glm::mat4(1.f), -pb);   // bind space -> agent space, at rest
        const glm::mat4 Ki = glm::inverse(K);
        for (const auto& b : sl)
        {
            LLJoint* j = gAgentAvatarp->getJoint(b.name);
            if (!j) continue;
            const glm::mat4 D = Ki * (mat(j->getWorldMatrix()) * glm::inverse(restOf(j))) * K;
            MGCreatorWeights::Mat m;
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) m[r * 4 + c] = D[c][r];
            out[b.name] = m;
        }
        return out;
    }

    // What you see is what you get: while placing, the garment hangs from the pelvis (as modelled);
    // at Rig it, each point is put where it shows now, through its new bones: point = (the blend of
    // its bones' skinning)^-1 x (the pelvis's skinning) x point. The skeleton used is the avatar's own
    // (its shape), at rest (T) unless the user posed it to the garment (then as posed).
    struct ShownBake
    {
        std::map<std::string, glm::mat4> world;   // joint -> its world matrix (rest or posed)
        glm::mat4 ibPel{ 1.f };                   // the preview's pelvis inverse bind (it hangs from it)
        glm::mat4 pelvisSkin() const { return world.at("mPelvis") * ibPel; }   // garment space -> world
    };

    // Posing bones: click a joint (its dot) to pose it, drag the poser's rings to turn it, click the
    // garment to move the garment again. Creator Lab keeps the mouse and draws the poser's rings
    // itself (the viewer only draws a tool's 3D marks for the current tool).
    void placeToolOn();
    bool placeActive();
    std::string sPoseSel;   // the joint the poser's rings are on
    // a joint chosen: the rings go onto it. Only the selection changes: switching the poser's tool
    // (pose_gizmo_mode) makes the tool manager drop our transient tool, and Firestorm's tool would
    // then take the clicks (no other joint, no garment could be clicked again)
    void poseSelect(const std::string& j)
    {
        sPoseSel = j;
        MGPhonePoser::runCommand("pose_markers", LLSD().with("on", true).with("group", "body").with("sel", j));
    }
    class MGToolPose : public LLTool
    {
    public:
        MGToolPose() : LLTool(std::string("MGPoseFit")) {}
        bool isAlwaysRendered() override { return true; }
        void render() override { FSToolCompPose::getInstance()->render(); }
        static LLSD frac(S32 x, S32 y)
        {
            const LLRect r = gViewerWindow->getRootView()->getLocalRect();
            return LLSD().with("x", (F64)x / llmax(1, r.getWidth())).with("y", 1.0 - (F64)y / llmax(1, r.getHeight()));
        }
        // the body joint drawn nearest the mouse, within 14 px
        static std::string jointAt(S32 x, S32 y)
        {
            static const char* BODY[] = { "mPelvis", "mTorso", "mChest", "mNeck", "mHead", "mCollarLeft", "mCollarRight", "mShoulderLeft", "mShoulderRight",
                "mElbowLeft", "mElbowRight", "mWristLeft", "mWristRight", "mHipLeft", "mHipRight", "mKneeLeft", "mKneeRight", "mAnkleLeft", "mAnkleRight", "mFootLeft", "mFootRight" };
            if (!isAgentAvatarValid()) return std::string();
            LLViewerCamera* cam = LLViewerCamera::getInstance();
            std::string best;
            F32 bd = 14.f * 14.f;
            for (const char* n : BODY)
            {
                LLJoint* j = gAgentAvatarp->getJoint(n);
                LLCoordGL c;
                if (!j || !cam->projectPosAgentToScreen(j->getWorldPosition(), c, false)) continue;
                const F32 dx = (F32)(c.mX - x), dy = (F32)(c.mY - y), d = dx * dx + dy * dy;
                if (d < bd) { bd = d; best = n; }
            }
            return best;
        }
        bool handleMouseDown(S32 x, S32 y, MASK mask) override
        {
            if (mask & MASK_ALT) return LLToolCamera::getInstance()->handleMouseDown(x, y, mask);
            // another joint's dot: pose that one (before the rings: a ring often passes over a dot)
            const std::string j = jointAt(x, y);
            if (!j.empty() && j != sPoseSel)
            {
                poseSelect(j);
                push(LLSD().with("type", "lab_pose_joint").with("joint", j));
                return true;
            }
            // a ring of the poser's gizmo: it turns the bone (the manip then holds the mouse)
            LLSD a = frac(x, y);
            a["ev"] = "down";
            if (MGPhonePoser::runCommand("pose_gizmo", a)["grabbed"].asBoolean()) return true;
            if (!j.empty()) return true;   // the selected joint's own dot
            // the garment: move it again
            const LLPickInfo pick = gViewerWindow->pickImmediate(x, y, false, true);
            LLViewerObject* o = pick.getObject();
            bool garment = false;
            if (o) for (const auto& ap : sApplied) if (ap.prim == o->getID() || (o->getRootEdit() && ap.prim == o->getRootEdit()->getID())) garment = true;
            if (garment && placeActive())
            {
                MGPhonePoser::runCommand("pose_gizmo_mode", LLSD().with("mode", "off"));
                MGPhonePoser::runCommand("pose_markers", LLSD().with("on", false));
                sPoseBones = false;
                showMarkers();
                placeToolOn();
                push(LLSD().with("type", "lab_pose_state").with("posing", MGPhonePoser::isPosing()).with("bones", false));
            }
            return true;
        }
        bool handleHover(S32 x, S32 y, MASK mask) override
        {
            LLSD a = frac(x, y);
            a["ev"] = "hover";
            MGPhonePoser::runCommand("pose_gizmo", a);
            gViewerWindow->setCursor(!jointAt(x, y).empty() ? UI_CURSOR_HAND : UI_CURSOR_TOOLROTATE);
            return true;
        }
    };
    MGToolPose* sPoseTool = nullptr;
    void poseToolOn()
    {
        if (!sPoseTool) sPoseTool = new MGToolPose();
        LLToolMgr::getInstance()->setTransientTool(sPoseTool);
    }
    void poseToolOff()
    {
        if (sPoseTool && LLToolMgr::getInstance()->getCurrentTool() == sPoseTool) LLToolMgr::getInstance()->clearTransientTool();
    }

    // the avatar let go by the poser (its animations again) when a test needs to animate it
    void poseRelease()
    {
        if (!MGPhonePoser::isPosing() && !sPoseBones) return;
        poseToolOff();
        MGPhonePoser::runCommand("pose_gizmo_mode", LLSD().with("mode", "off"));
        MGPhonePoser::runCommand("pose_markers", LLSD().with("on", false));
        sPoseBones = false;
        MGPhonePoser::runCommand("pose_stop", LLSD());
        push(LLSD().with("type", "lab_pose_state").with("posing", false).with("released", true));
    }

    bool previewPelvisBind(glm::mat4& out);
    std::shared_ptr<ShownBake> shownBake(bool posed)
    {
        if (!isAgentAvatarValid()) return nullptr;
        auto mat = [](const LLMatrix4& m) { return glm::make_mat4((const F32*)m.mMatrix); };
        // <MG> the rest skeleton composed as the viewer composes joints (LLXformMatrix): a joint's
        // matrix holds only its OWN scale; its offset is scaled by its parent's scale; no turn at rest
        struct RestJ { LLVector3 pos; };
        std::map<LLJoint*, RestJ> rest;
        LLQuaternion R0;
        std::function<LLVector3(LLJoint*)> restPos = [&](LLJoint* j) -> LLVector3
        {
            auto it = rest.find(j);
            if (it != rest.end()) return it->second.pos;
            LLJoint* p = j->getParent();
            LLVector3 wp;
            if (!p) { wp = j->getWorldPosition(); R0 = j->getWorldRotation(); }
            else
            {
                const LLVector3 pp = restPos(p);
                LLVector3 off = j->getPosition();
                off.scaleVec(p->getScale());
                wp = pp + off * R0;
            }
            rest[j] = { wp };
            return wp;
        };
        auto restOf = [&](LLJoint* j)
        {
            const LLVector3 wp = restPos(j);
            LLMatrix4 m;
            m.initAll(j->getScale(), R0, wp);
            return mat(m);
        };
        auto b = std::make_shared<ShownBake>();
        const auto& sl = slSkeleton();
        // the preview's own pelvis bind (the garment is shown through it while placed)
        const S32 pi = slIndex("mPelvis");
        if (pi >= 0) b->ibPel = glm::translate(glm::mat4(1.f), -glm::vec3((F32)sl[pi].world[0], (F32)sl[pi].world[1], (F32)sl[pi].world[2]));
        previewPelvisBind(b->ibPel);
        for (const auto& bone : sl)
        {
            LLJoint* j = gAgentAvatarp->getJoint(bone.name);
            if (!j) continue;
            b->world[bone.name] = posed ? mat(j->getWorldMatrix()) : restOf(j);
        }
        return b->world.count("mPelvis") ? b : nullptr;
    }

    // the skeleton the rig weighs in: each bone as it shows, seen from the garment's space (the
    // preview's pelvis skinning), so the weights come from where the garment sits on screen
    std::map<std::string, MGCreatorWeights::Mat> shownInGarment(const ShownBake& b)
    {
        std::map<std::string, MGCreatorWeights::Mat> out;
        if (!b.world.count("mPelvis")) return out;
        const glm::mat4 Pinv = glm::inverse(b.pelvisSkin());
        for (const auto& kv : b.world)
        {
            const glm::mat4 D = Pinv * kv.second;
            MGCreatorWeights::Mat m;
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) m[r * 4 + c] = D[c][r];
            out[kv.first] = m;
        }
        return out;
    }

    // (worker thread: plain maths on copies)
    void bakeShown(MGRig::Model& out, const ShownBake& b)
    {
        auto fromCollada = [](const MGCreatorWeights::Mat& m) { glm::mat4 g; for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) g[c][r] = (F32)m[r * 4 + c]; return g; };
        const glm::mat4 Wpel = b.world.at("mPelvis");
        const glm::mat4 Ppel = b.pelvisSkin();   // how the preview showed each point
        for (auto& m : out.meshes)
        {
            const size_t nj = m.skinJoints.size();
            std::vector<glm::mat4> pal(nj);
            for (size_t j = 0; j < nj; ++j)
            {
                auto it = b.world.find(m.skinJoints[j]);
                const glm::mat4 W = it != b.world.end() ? it->second : Wpel;
                const glm::mat4 IB = j < m.skinInvBind.size() ? fromCollada(m.skinInvBind[j]) : glm::mat4(1.f);
                pal[j] = W * IB;
            }
            for (size_t v = 0; v < m.vertexCount() && v < m.weights.size(); ++v)
            {
                glm::mat4 F(0.f);
                double sum = 0;
                for (const auto& jw : m.weights[v])
                {
                    if (jw.first < 0 || jw.first >= (int)nj || jw.second <= 0) continue;
                    F += pal[jw.first] * (F32)jw.second;
                    sum += jw.second;
                }
                if (sum <= 0) continue;
                F *= (F32)(1.0 / sum);
                const glm::mat4 M = glm::inverse(F) * Ppel;
                const glm::vec4 q = M * glm::vec4((F32)m.pos[v * 3], (F32)m.pos[v * 3 + 1], (F32)m.pos[v * 3 + 2], 1.f);
                if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) continue;
                m.pos[v * 3] = q.x; m.pos[v * 3 + 1] = q.y; m.pos[v * 3 + 2] = q.z;
                if (m.nrm.size() >= (v + 1) * 3)
                {
                    const glm::vec3 n = glm::transpose(glm::inverse(glm::mat3(M))) * glm::vec3((F32)m.nrm[v * 3], (F32)m.nrm[v * 3 + 1], (F32)m.nrm[v * 3 + 2]);
                    const F32 l = glm::length(n);
                    if (l > 1e-8f) { m.nrm[v * 3] = n.x / l; m.nrm[v * 3 + 1] = n.y / l; m.nrm[v * 3 + 2] = n.z / l; }
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // <MG> Place on the body, live. The rig preview is shown on the stand-in; moving,
    // turning and resizing it changes only the stand-in's copy of the skin (its bind shape,
    // which the renderer bakes into the vertices) and rebuilds that object: no file is written,
    // nothing is sent. "Rig it" then bakes the same placement into the file, once.
    // ------------------------------------------------------------------
    struct PlaceLive
    {
        bool on = false;
        std::map<LLUUID, LLConstPointer<LLMeshSkinInfo>> base;   // prim -> the skin Local Mesh gave it
        std::map<LLUUID, LLConstPointer<LLMeshSkinInfo>> ours;   // prim -> the copy shown now
        glm::vec3 centre{ 0.f };                                 // the pieces' centre (bind space)
        F32 move[3] = { 0.f, 0.f, 0.f };                         // m: forward, left, up
        F32 turn = 0.f;                                          // degrees about the vertical (Z)
        F32 scale = 1.f;                                         // all axes
        F32 tilt[2] = { 0.f, 0.f };                              // degrees about X (sideways) and Y (forward / back)
        F32 axis[3] = { 1.f, 1.f, 1.f };                         // per axis, times scale (stretch)
        S32 mode = 0;                                            // the gizmo: 0 move, 1 rotate, 2 scale
    } sPlace;
    bool placeActive() { return sPlace.on; }

    // the pelvis inverse bind of the preview being placed (the garment is shown through it)
    bool previewPelvisBind(glm::mat4& out)
    {
        for (const auto& kv : sPlace.base)
        {
            const LLMeshSkinInfo* s = kv.second.get();
            if (!s) continue;
            for (size_t i = 0; i < s->mJointNames.size() && i < s->mInvBindMatrix.size(); ++i)
                if (s->mJointNames[i] == "mPelvis") { out = glm::make_mat4(s->mInvBindMatrix[i].getF32ptr()); return true; }
        }
        return false;
    }

    glm::mat4 placeMatrix()
    {
        const glm::vec3 c = sPlace.centre;
        auto rot = [](int axis, F32 deg)
        {
            const F32 a = deg * F_PI / 180.f, cs = cosf(a), sn = sinf(a);
            glm::mat4 M(1.f);   // column-major
            const int i = (axis + 1) % 3, j = (axis + 2) % 3;
            M[i][i] = cs; M[i][j] = sn; M[j][i] = -sn; M[j][j] = cs;
            return M;
        };
        const glm::mat4 R = rot(2, sPlace.turn) * rot(1, sPlace.tilt[1]) * rot(0, sPlace.tilt[0]);
        glm::mat4 S(1.f);
        for (int k = 0; k < 3; ++k) S[k][k] = sPlace.scale * sPlace.axis[k];
        glm::mat4 T1(1.f), T0(1.f);
        T1[3] = glm::vec4(c.x + sPlace.move[0], c.y + sPlace.move[1], c.z + sPlace.move[2], 1.f);
        T0[3] = glm::vec4(-c.x, -c.y, -c.z, 1.f);
        return T1 * R * S * T0;
    }

    // the stand-in's prims with the placement (a prim whose skin is not our copy got a new one
    // from Local Mesh, e.g. after a reload: it becomes the base)
    void placeApply()
    {
        if (!sPlace.on) return;
        const glm::mat4 P = placeMatrix();
        for (const Applied& a : sApplied)
        {
            LLVOVolume* v = dynamic_cast<LLVOVolume*>(gObjectList.findObject(a.prim));
            if (!v || v->isDead()) continue;
            const LLMeshSkinInfo* cur = v->getSkinInfo();
            if (!cur) continue;
            auto o = sPlace.ours.find(a.prim);
            if (o == sPlace.ours.end() || o->second.get() != cur) sPlace.base[a.prim] = cur;
            const LLMeshSkinInfo* base = sPlace.base[a.prim].get();
            if (!base) continue;
            LLPointer<LLMeshSkinInfo> c = new LLMeshSkinInfo(*base);
            const glm::mat4 M = P * glm::make_mat4(base->mBindShapeMatrix.getF32ptr());
            c->mBindShapeMatrix.loadu(glm::value_ptr(M));
            c->mBindPoseMatrix.resize(c->mInvBindMatrix.size());
            for (size_t i = 0; i < c->mInvBindMatrix.size(); ++i) matMul(c->mBindShapeMatrix, c->mInvBindMatrix[i], c->mBindPoseMatrix[i]);
            sPlace.ours[a.prim] = c.get();
            v->notifySkinInfoLoaded(c);
            if (v->mDrawable.notNull()) gPipeline.markRebuild(v->mDrawable, LLDrawable::REBUILD_ALL);
        }
    }

    // the worn garment must keep its skin: a refused server fetch of a new local mesh's skin
    // used to clear it (the object then counts as "not rigged": no live preview, no Drape match)
    void skinGuard()
    {
        static LLFrameTimer t;
        if (t.getElapsedTimeF32() < 0.5f || sPlace.on) return;
        t.reset();
        for (const Applied& a : sApplied)
        {
            LLVOVolume* v = dynamic_cast<LLVOVolume*>(gObjectList.findObject(a.prim));
            if (!v || v->isDead() || !v->mIsLocalMesh || !v->getVolume() || v->getSkinInfo()) continue;
            const LLSculptParams* sp = v->getSculptParams();
            if (!sp) continue;
            auto it = gMeshRepo.mSkinMap.find(sp->getSculptTexture());
            if (it == gMeshRepo.mSkinMap.end() || !it->second) continue;
            v->notifySkinInfoLoaded(it->second);
            if (v->mDrawable.notNull()) gPipeline.markRebuild(v->mDrawable, LLDrawable::REBUILD_ALL);
            LL_INFOS("MGCreator") << "Skin guard: the garment's skin was put back on prim " << a.prim << LL_ENDL;
        }
    }

    // a reload or a re-wear gave the stand-in a new skin: the placement again
    void placeIdle()
    {
        if (!sPlace.on) return;
        for (const Applied& a : sApplied)
        {
            LLVOVolume* v = dynamic_cast<LLVOVolume*>(gObjectList.findObject(a.prim));
            if (!v || v->isDead() || !v->getSkinInfo()) continue;
            auto o = sPlace.ours.find(a.prim);
            if (o == sPlace.ours.end() || o->second.get() != v->getSkinInfo()) { placeApply(); return; }
        }
    }

    class MGToolPlace;
    MGToolPlace* sPlaceTool = nullptr;
    void placeToolOff();

    // done placing: the stand-in keeps what it shows (restore = put the file's own skin back)
    void placeEnd(bool restore)
    {
        if (restore)
            for (const auto& kv : sPlace.base)
            {
                LLVOVolume* v = dynamic_cast<LLVOVolume*>(gObjectList.findObject(kv.first));
                if (!v || v->isDead() || kv.second.isNull()) continue;
                v->notifySkinInfoLoaded(kv.second.get());
                if (v->mDrawable.notNull()) gPipeline.markRebuild(v->mDrawable, LLDrawable::REBUILD_ALL);
            }
        sPlace.on = false;
        sPlace.base.clear();
        sPlace.ours.clear();
        placeToolOff();
    }

    void pushPlace()
    {
        push(LLSD().with("type", "lab_place").with("fwd", sPlace.move[0] * 100.f).with("left", sPlace.move[1] * 100.f)
            .with("up", sPlace.move[2] * 100.f).with("turn", sPlace.turn).with("size", sPlace.scale * 100.f)
            .with("tiltx", sPlace.tilt[0]).with("tilty", sPlace.tilt[1])
            .with("sx", sPlace.axis[0] * 100.f).with("sy", sPlace.axis[1] * 100.f).with("sz", sPlace.axis[2] * 100.f).with("mode", sPlace.mode));
    }

    // drag in the 3D view: move it (in the camera's plane); Shift: turn; Ctrl: resize; Alt: camera
    const std::vector<MGRig::SlBone>& slSkeleton();
    S32 slIndex(const std::string& name);

    // the placed garment's middle, in the agent's space, and the avatar's turn (bind space -> agent)
    bool placeOrigin(LLVector3& O, LLQuaternion& R)
    {
        if (!isAgentAvatarValid()) return false;
        LLJoint* pel = gAgentAvatarp->getJoint("mPelvis");
        if (!pel) return false;
        // <MG> the frame the garment is shown in: the pelvis joint's (its own turn, not the avatar
        // object's, which lags behind the body), through the preview's pelvis bind
        R = pel->getWorldRotation();
        LLVector3 pb(0.f, 0.f, 1.067f);
        const S32 pi = slIndex("mPelvis");
        if (pi >= 0) pb.set((F32)slSkeleton()[pi].world[0], (F32)slSkeleton()[pi].world[1], (F32)slSkeleton()[pi].world[2]);
        glm::mat4 ib = glm::translate(glm::mat4(1.f), -glm::vec3(pb.mV[0], pb.mV[1], pb.mV[2]));
        previewPelvisBind(ib);
        const glm::mat4 P = glm::make_mat4((const F32*)pel->getWorldMatrix().mMatrix) * ib;
        const glm::vec4 o = P * glm::vec4(sPlace.centre.x + sPlace.move[0], sPlace.centre.y + sPlace.move[1], sPlace.centre.z + sPlace.move[2], 1.f);
        O.set(o.x, o.y, o.z);
        return true;
    }

    class MGToolPlace : public LLTool
    {
    public:
        MGToolPlace() : LLTool(std::string("MGPlace")) {}
        // the gizmo, as Second Life's and Blender's: Move (three arrows, the middle square free),
        // Rotate (three rings, one per axis), Scale (three handles, the middle square: all axes).
        // Parts: 1-3 the X / Y / Z handle (forward, left, up), 4 the middle.
        S32 mX0 = 0, mY0 = 0;
        MASK mMask = 0;
        F32 mMove0[3] = { 0.f, 0.f, 0.f }, mTurn0 = 0.f, mScale0 = 1.f;
        LLVector3 mAnchor, mPlaneN;
        bool mDragging = false;
        LLFrameTimer mPushTimer;
        S32 mPart = 0, mHover = 0;
        LLCoordGL mO2, mTip2[3];
        std::vector<LLCoordGL> mRing2[3];
        LLVector3 mAx[3];
        bool mGizmo = false;
        F32 mTilt0[2] = { 0.f, 0.f }, mAxis0[3] = { 1.f, 1.f, 1.f };
        static constexpr F32 ARROW = 0.30f, RING = 0.32f;

        void buildGizmo()
        {
            mGizmo = false;
            LLVector3 O; LLQuaternion R;
            if (!sPlace.on || !placeOrigin(O, R)) return;
            LLViewerCamera* cam = LLViewerCamera::getInstance();
            if (!cam->projectPosAgentToScreen(O, mO2, false)) return;
            mAx[0] = LLVector3(1, 0, 0) * R; mAx[1] = LLVector3(0, 1, 0) * R; mAx[2] = LLVector3(0, 0, 1) * R;
            const F32 len = sPlace.mode == 2 ? ARROW * 0.85f : ARROW;
            for (int k = 0; k < 3; ++k) if (!cam->projectPosAgentToScreen(O + mAx[k] * len, mTip2[k], false)) return;
            for (int k = 0; k < 3; ++k)
            {
                mRing2[k].clear();
                if (sPlace.mode != 1) continue;
                const LLVector3& u = mAx[(k + 1) % 3];
                const LLVector3& w = mAx[(k + 2) % 3];
                for (int i = 0; i <= 64; ++i)
                {
                    const F32 a = (F32)i / 64.f * F_TWO_PI;
                    LLCoordGL c;
                    if (cam->projectPosAgentToScreen(O + (u * cosf(a) + w * sinf(a)) * RING, c, false)) mRing2[k].push_back(c);
                }
            }
            mGizmo = true;
        }
        static F32 segD2(S32 x, S32 y, const LLCoordGL& a, const LLCoordGL& b)
        {
            const F32 dx = (F32)(b.mX - a.mX), dy = (F32)(b.mY - a.mY), l2 = dx * dx + dy * dy;
            F32 t = l2 > 1e-6f ? (((F32)(x - a.mX)) * dx + ((F32)(y - a.mY)) * dy) / l2 : 0.f;
            t = llclamp(t, 0.f, 1.f);
            const F32 px = a.mX + dx * t - x, py = a.mY + dy * t - y;
            return px * px + py * py;
        }
        S32 partAt(S32 x, S32 y)
        {
            if (!mGizmo) return 0;
            auto near2 = [&](const LLCoordGL& c, F32 r) { const F32 dx = (F32)(c.mX - x), dy = (F32)(c.mY - y); return dx * dx + dy * dy <= r * r; };
            if (sPlace.mode != 1 && near2(mO2, 10.f)) return 4;
            if (sPlace.mode == 1)
            {
                S32 best = 0; F32 bd = 49.f;
                for (int k = 0; k < 3; ++k)
                    for (size_t i = 1; i < mRing2[k].size(); ++i) { const F32 d = segD2(x, y, mRing2[k][i - 1], mRing2[k][i]); if (d < bd) { bd = d; best = k + 1; } }
                return best;
            }
            for (int k = 0; k < 3; ++k) if (near2(mTip2[k], 10.f) || segD2(x, y, mO2, mTip2[k]) <= 36.f) return k + 1;
            return 0;
        }
        void draw() override
        {
            buildGizmo();
            if (!mGizmo) return;
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            const S32 active = mDragging ? mPart : mHover;
            auto thick = [](const LLCoordGL& a, const LLCoordGL& b, F32 w)
            {
                for (F32 o = -w; o <= w + 0.01f; o += 1.f) { gl_line_2d(a.mX + (S32)o, a.mY, b.mX + (S32)o, b.mY); gl_line_2d(a.mX, a.mY + (S32)o, b.mX, b.mY + (S32)o); }
            };
            static const F32 COL[3][3] = { { 0.95f, 0.2f, 0.2f }, { 0.3f, 0.9f, 0.3f }, { 0.25f, 0.45f, 1.f } };
            auto colour = [&](int k, bool on) { if (on) gGL.color4f(1.f, 0.95f, 0.3f, 1.f); else gGL.color4f(COL[k][0], COL[k][1], COL[k][2], 1.f); };
            if (sPlace.mode == 1)
            {
                // three rings: red turns about X (side tilt), green about Y (forward tilt), blue about Z (turn)
                for (int k = 0; k < 3; ++k)
                {
                    colour(k, active == k + 1);
                    for (size_t i = 1; i < mRing2[k].size(); ++i) thick(mRing2[k][i - 1], mRing2[k][i], active == k + 1 ? 1.5f : 0.5f);
                }
            }
            else
            {
                for (int k = 0; k < 3; ++k)
                {
                    const bool on = active == k + 1;
                    colour(k, on);
                    thick(mO2, mTip2[k], on ? 1.5f : 1.f);
                    if (sPlace.mode == 0)
                    {
                        // an arrow head: a triangle along the axis on screen
                        const F32 dx = (F32)(mTip2[k].mX - mO2.mX), dy = (F32)(mTip2[k].mY - mO2.mY), l = sqrtf(dx * dx + dy * dy);
                        if (l > 1.f)
                        {
                            const F32 ux = dx / l, uy = dy / l, hl = 14.f, hw = 6.f;
                            const F32 tx = mTip2[k].mX + ux * hl, ty = mTip2[k].mY + uy * hl;
                            gl_triangle_2d((S32)tx, (S32)ty, (S32)(mTip2[k].mX - uy * hw), (S32)(mTip2[k].mY + ux * hw),
                                           (S32)(mTip2[k].mX + uy * hw), (S32)(mTip2[k].mY - ux * hw), LLColor4(on ? 1.f : COL[k][0], on ? 0.95f : COL[k][1], on ? 0.3f : COL[k][2], 1.f), true);
                        }
                    }
                    else gl_rect_2d(mTip2[k].mX - 6, mTip2[k].mY + 6, mTip2[k].mX + 6, mTip2[k].mY - 6, true);   // scale: a cube
                }
                // the middle: free move (Move), all axes (Scale)
                if (active == 4) gGL.color4f(1.f, 0.95f, 0.3f, 1.f); else gGL.color4f(sPlace.mode == 2 ? 0.9f : 1.f, sPlace.mode == 2 ? 0.9f : 0.85f, sPlace.mode == 2 ? 0.9f : 0.f, 0.95f);
                gl_rect_2d(mO2.mX - 7, mO2.mY + 7, mO2.mX + 7, mO2.mY - 7, true);
            }
            if (!mDragging && mHover)
            {
                static const char* WHAT[3][5] = {
                    { "", "forward / back", "left / right", "up / down", "move freely" },
                    { "", "tilt sideways (X)", "tilt forward (Y)", "turn (Z)", "" },
                    { "", "stretch forward / back", "stretch left / right", "stretch up / down", "size (all)" } };
                LLFontGL::getFontSansSerifSmall()->renderUTF8(std::string(WHAT[sPlace.mode][mHover]), 0, (F32)mO2.mX + 16.f, (F32)mO2.mY - 20.f, LLColor4::white,
                    LLFontGL::LEFT, LLFontGL::BASELINE, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
            }
        }
        bool handleMouseDown(S32 x, S32 y, MASK mask) override
        {
            if (mask & MASK_ALT) return LLToolCamera::getInstance()->handleMouseDown(x, y, mask);
            if (!sPlace.on || !isAgentAvatarValid()) return false;
            mX0 = x; mY0 = y; mMask = mask;
            buildGizmo();
            mPart = partAt(x, y);
            for (int k = 0; k < 3; ++k) mMove0[k] = sPlace.move[k];
            mTurn0 = sPlace.turn;
            mScale0 = sPlace.scale;
            for (int k = 0; k < 2; ++k) mTilt0[k] = sPlace.tilt[k];
            for (int k = 0; k < 3; ++k) mAxis0[k] = sPlace.axis[k];
            // the plane through the avatar's middle, facing the camera
            mAnchor = gAgentAvatarp->getPositionAgent();
            mPlaneN = -LLViewerCamera::getInstance()->getAtAxis();
            mDragging = true;
            setMouseCapture(true);
            return true;
        }
        bool hit(S32 x, S32 y, LLVector3& out)
        {
            const LLVector3 o = LLViewerCamera::getInstance()->getOrigin();
            LLVector3 d = gViewerWindow->mouseDirectionGlobal(x, y);
            d.normVec();
            const F32 den = d * mPlaneN;
            if (fabsf(den) < 1e-6f) return false;
            const F32 t = ((mAnchor - o) * mPlaneN) / den;
            if (t <= 0.f) return false;
            out = o + d * t;
            return true;
        }
        bool handleHover(S32 x, S32 y, MASK mask) override
        {
            if (!mDragging)
            {
                mHover = partAt(x, y);
                gViewerWindow->setCursor(!sPlace.on ? UI_CURSOR_ARROW : sPlace.mode == 1 ? UI_CURSOR_TOOLROTATE : sPlace.mode == 2 ? UI_CURSOR_TOOLSCALE : UI_CURSOR_TOOLTRANSLATE);
                return sPlace.on;
            }
            auto along = [&](int k, F32& lenPx)
            {
                const F32 sx = (F32)(mTip2[k].mX - mO2.mX), sy = (F32)(mTip2[k].mY - mO2.mY);
                lenPx = sqrtf(sx * sx + sy * sy);
                return lenPx > 4.f ? (((F32)(x - mX0)) * sx + ((F32)(y - mY0)) * sy) / lenPx : 0.f;
            };
            if (sPlace.mode == 0 && mPart >= 1 && mPart <= 3)
            {
                // Move along one arrow: the mouse's move along it on screen, in metres
                F32 lp; const F32 a = along(mPart - 1, lp);
                if (lp > 4.f) sPlace.move[mPart - 1] = llclamp(mMove0[mPart - 1] + a / lp * ARROW, -3.f, 3.f);
            }
            else if (sPlace.mode == 1 && mPart >= 1 && mPart <= 3)
            {
                // Rotate on one ring: the angle swept around the middle on screen (the ring seen from its back turns the other way)
                const F32 a0 = atan2f((F32)(mY0 - mO2.mY), (F32)(mX0 - mO2.mX)), a1 = atan2f((F32)(y - mO2.mY), (F32)(x - mO2.mX));
                F32 d = (a1 - a0) * RAD_TO_DEG;
                while (d > 180.f) d -= 360.f;
                while (d < -180.f) d += 360.f;
                const bool fromBack = (LLViewerCamera::getInstance()->getAtAxis() * mAx[mPart - 1]) > 0.f;
                if (fromBack) d = -d;
                auto wrap = [](F32 t) { while (t > 180.f) t -= 360.f; while (t < -180.f) t += 360.f; return t; };
                if (mPart == 3) sPlace.turn = wrap(mTurn0 + d);
                else sPlace.tilt[mPart - 1] = wrap(mTilt0[mPart - 1] + d);
            }
            else if (sPlace.mode == 2 && mPart >= 1 && mPart <= 3)
            {
                // Scale one axis: the handle pulled along its axis
                F32 lp; const F32 a = along(mPart - 1, lp);
                if (lp > 4.f) sPlace.axis[mPart - 1] = llclamp(mAxis0[mPart - 1] * (1.f + a / lp), 0.05f, 20.f);
            }
            else if (sPlace.mode == 2 && mPart == 4)
            {
                // Scale all: dragged up / right bigger
                sPlace.scale = llclamp(mScale0 * powf(2.f, ((F32)(x - mX0) + (F32)(y - mY0)) / 200.f), 0.05f, 20.f);
            }            else if (mMask & MASK_SHIFT) sPlace.turn = llclamp(mTurn0 + (F32)(x - mX0) * 0.4f, -180.f, 180.f);
            else if (mMask & MASK_CONTROL) sPlace.scale = llclamp(mScale0 * (1.f + (F32)(y - mY0) * 0.003f), 0.05f, 20.f);
            else
            {
                LLVector3 a, b;
                if (hit(mX0, mY0, a) && hit(x, y, b))
                {
                    // the move in the frame the garment is shown in (the pelvis's; bind space: X forward, Y left, Z up)
                    LLJoint* pel = gAgentAvatarp->getJoint("mPelvis");
                    LLVector3 dl = (b - a) * ~(pel ? pel->getWorldRotation() : gAgentAvatarp->getRotation());
                    for (int k = 0; k < 3; ++k) sPlace.move[k] = llclamp(mMove0[k] + dl.mV[k], -3.f, 3.f);
                }
            }
            placeApply();
            if (mPushTimer.getElapsedTimeF32() > 0.1f) { pushPlace(); mPushTimer.reset(); }
            gViewerWindow->setCursor(UI_CURSOR_TOOLTRANSLATE);
            return true;
        }
        bool handleMouseUp(S32 x, S32 y, MASK mask) override
        {
            if (!mDragging) return false;
            mDragging = false;
            setMouseCapture(false);
            pushPlace();
            return true;
        }
        void onMouseCaptureLost() override { if (mDragging) { mDragging = false; pushPlace(); } }
    };

    void placeToolOn()
    {
        if (!sPlaceTool) sPlaceTool = new MGToolPlace();
        LLToolMgr::getInstance()->setTransientTool(sPlaceTool);
    }
    void placeToolOff()
    {
        if (sPlaceTool && LLToolMgr::getInstance()->getCurrentTool() == sPlaceTool) LLToolMgr::getInstance()->clearTransientTool();
    }

    // ------------------------------------------------------------------
    // Make it fit: push the garment out of the body, save a copy of the file.
    // The file is skinned here exactly as Second Life will draw it once uploaded
    // (LLSkinningUtil, the avatar's bones, the file's own bind matrices), in every test
    // pose, and compared with the body's skin triangles (mgfitcore). Each correction is
    // turned back into the file's own space (inverse of the skinning at that point) and
    // kept, so later poses start from the corrected garment (2026-11-14, was: one push
    // per point, the largest of any pose, against the nearest body POINT, which left skin
    // showing through sleeves). Only positions change; the file keeps everything else.
    // ------------------------------------------------------------------
    struct FitMesh
    {
        LLPointer<LLMeshSkinInfo> skin;
        std::vector<std::array<F32, 4>> packed;   // per vertex: joint + weight (4 strongest)
        std::vector<F32> push;                    // its total move on the avatar (m)
        std::vector<double> work;                 // the file's positions as corrected so far (each pose starts from them)
        std::vector<std::vector<S32>> nb;         // <MG> neighbours: triangle edges + points at one place (seams)
    };
    struct Fit
    {
        bool ready = false;
        MGCreatorDae::File dae;
        std::vector<FitMesh> meshes;
        std::set<std::string> regions;
        F32 margin = 0.003f;
        S32 smooth = 4;                 // how far a push spreads to the neighbours (passes)
        F32 maxPush = 0.02f;            // deeper clipping is left to the creator (m)
        bool shapes = false;            // also fit the heavy / thin shapes
        std::vector<std::vector<U8>> protect;   // Touch up's protected points: never moved
        std::set<std::string> only;     // fit these regions only (empty: all of the garment's)
        LLUUID standin;
        LLUUID pendingFile;    // the fitted file, worn once Local Mesh has loaded it
        LLFrameTimer pendingTimer;
    } sFit;

    LLMatrix4a fromCollada(const std::array<double, 16>& a)
    {
        // as Local Mesh's importer: COLLADA row-major -> LL row-vector matrices
        LLMatrix4 m;
        for (S32 i = 0; i < 4; ++i)
            for (S32 j = 0; j < 4; ++j)
                m.mMatrix[i][j] = (F32)a[i + j * 4];
        LLMatrix4a r;
        r.loadu(m);
        return r;
    }

    std::string fitPrepare(const std::string& path)
    {
        sFit.ready = false;
        sFit.meshes.clear();
        sFit.regions.clear();
        if (!MGCreatorDae::read(path, sFit.dae)) return sFit.dae.error.empty() ? std::string("read") : sFit.dae.error;
        if (!sFit.dae.upAxis.empty() && sFit.dae.upAxis != "Z_UP") return "axis";
        if (std::fabs(sFit.dae.meter - 1.0) > 1e-6) return "unit";
        for (const auto& m : sFit.dae.meshes)
        {
            if (m.joints.size() > LL_MAX_JOINTS_PER_MESH_OBJECT) return "bones";
            FitMesh fm;
            fm.skin = new LLMeshSkinInfo();
            fm.skin->mJointNames = m.joints;
            // LLSkinningUtil::initJointNums reads and fills mJointNums[j]: it must hold one
            // -1 per joint first, as the mesh repository's skin loader leaves it (an empty
            // list here crashed the viewer on the first pose)
            fm.skin->mJointNums.assign(m.joints.size(), -1);
            for (const auto& ib : m.invBind) fm.skin->mInvBindMatrix.push_back(fromCollada(ib));
            fm.skin->mBindShapeMatrix = fromCollada(m.bindShape);
            fm.skin->mBindPoseMatrix.resize(fm.skin->mInvBindMatrix.size());
            for (size_t i = 0; i < fm.skin->mInvBindMatrix.size(); ++i)
            {
                matMul(fm.skin->mBindShapeMatrix, fm.skin->mInvBindMatrix[i], fm.skin->mBindPoseMatrix[i]);
            }
            for (const auto& j : m.joints) sFit.regions.insert(regionOf(j));
            const size_t n = m.vertexCount();
            fm.packed.resize(n);
            for (size_t v = 0; v < n; ++v)
            {
                std::vector<std::pair<int, double>> w = m.weights[v];
                std::sort(w.begin(), w.end(), [](const std::pair<int, double>& a, const std::pair<int, double>& b) { return a.second > b.second; });
                if (w.size() > 4) w.resize(4);
                double sum = 0.0;
                for (const auto& jw : w) sum += jw.second;
                std::array<F32, 4> pk{ 0.f, 0.f, 0.f, 0.f };
                if (w.empty() || sum <= 0.0)
                {
                    pk[0] = 0.9999f;   // no weights: the first bone (the uploader would refuse it anyway)
                }
                for (size_t k = 0; k < w.size() && sum > 0.0; ++k)
                {
                    const F32 wt = llclamp((F32)(w[k].second / sum), 0.f, 0.9999f);
                    pk[k] = (F32)w[k].first + wt;
                }
                fm.packed[v] = pk;
            }
            fm.push.assign(n, 0.f);
            fm.work = m.positions;
            fm.nb.assign(n, {});
            for (const auto& t : m.triangles)
                for (S32 k = 0; k < 3; ++k)
                {
                    const S32 a = t[k], b = t[(k + 1) % 3];
                    if (a < 0 || b < 0 || a >= (S32)n || b >= (S32)n || a == b) continue;
                    fm.nb[a].push_back(b);
                    fm.nb[b].push_back(a);
                }
            {
                std::map<std::array<long long, 3>, std::vector<S32>> at;
                for (size_t v = 0; v < n; ++v) at[{ std::llround(m.positions[v * 3] * 1e5), std::llround(m.positions[v * 3 + 1] * 1e5), std::llround(m.positions[v * 3 + 2] * 1e5) }].push_back((S32)v);
                for (const auto& kv : at)
                    for (S32 u : kv.second) for (S32 w : kv.second) if (u != w) fm.nb[u].push_back(w);
            }
            sFit.meshes.push_back(fm);
        }
        // points the creator protected with Touch up (next to the _fit copy)
        sFit.protect.assign(sFit.meshes.size(), {});
        for (size_t mi = 0; mi < sFit.meshes.size(); ++mi) sFit.protect[mi].assign(sFit.dae.meshes[mi].vertexCount(), 0);
        {
            std::ifstream pin(MGCreatorDae::fitPathFor(path) + ".protect");
            size_t pmi, pv;
            while (pin && (pin >> pmi >> pv))
            {
                if (pmi < sFit.protect.size() && pv < sFit.protect[pmi].size()) sFit.protect[pmi][pv] = 1;
            }
        }
        if (!sFit.only.empty())
        {
            std::set<std::string> kept;
            for (const auto& r : sFit.regions) if (sFit.only.count(r)) kept.insert(r);
            sFit.regions = kept;
        }
        if (sFit.regions.empty()) return "regions";
        sFit.ready = true;
        return std::string();
    }

    // one pose sample: the garment as corrected so far (sFit work positions), skinned as Second
    // Life will draw it, against the body's skin TRIANGLES in this pose (MGFitCore: points under
    // the skin, skin through a face, layers together, pushes spread in 3D). Each move goes back
    // to the file through the inverse of the skinning at that point and is kept at once, so the
    // next sample and the next pose start from the corrected garment: the poses add up instead
    // of fighting (the old one kept, per point, only the largest push of any pose).
    // Returns how many places needed a push in this pose (before correcting it).
    S32 fitSample()
    {
        if (!sFit.ready || !isAgentAvatarValid()) return 0;
        MGFitCore::Params prm;
        prm.margin = sFit.margin;
        prm.maxPush = sFit.maxPush;
        prm.radius = 0.004f + 0.003f * (F32)sFit.smooth;
        prm.plateau = llmin(0.005f, prm.radius);
        const F32 REACH = prm.maxPush + prm.margin + 0.002f;
        const F32 CAP = prm.maxPush + prm.margin;   // a point never ends further than this from where it was (world)

        // 1. the garment, skinned
        MGFitCore::Garment G;
        std::vector<std::vector<LLMatrix4a>> finals(sFit.meshes.size());
        std::vector<size_t> base(sFit.meshes.size(), 0);
        LLVector3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
        auto skinPoint = [](const FitMesh& fm, const LLMatrix4a& fin, size_t v) -> LLVector3
        {
            LLVector4a raw((F32)fm.work[v * 3], (F32)fm.work[v * 3 + 1], (F32)fm.work[v * 3 + 2]);
            LLVector4a t, dst;
            fm.skin->mBindShapeMatrix.affineTransform(raw, t);
            fin.affineTransform(t, dst);
            return LLVector3(dst.getF32ptr());
        };
        for (size_t mi = 0; mi < sFit.meshes.size(); ++mi)
        {
            const MGCreatorDae::SkinnedMesh& m = sFit.dae.meshes[mi];
            FitMesh& fm = sFit.meshes[mi];
            const S32 count = (S32)fm.skin->mJointNames.size();
            LLMatrix4a mat[LL_MAX_JOINTS_PER_MESH_OBJECT];
            LLSkinningUtil::initSkinningMatrixPalette(mat, count, fm.skin.get(), gAgentAvatarp);
            const size_t n = m.vertexCount();
            base[mi] = G.pos.size();
            finals[mi].resize(n);
            for (size_t v = 0; v < n; ++v)
            {
                std::array<F32, 4> pk = fm.packed[v];
                LLSkinningUtil::getPerVertexSkinMatrix(pk.data(), mat, true, finals[mi][v], count);
                const LLVector3 w = skinPoint(fm, finals[mi][v], v);
                G.pos.emplace_back(w.mV[0], w.mV[1], w.mV[2]);
                G.fixed.push_back((mi < sFit.protect.size() && v < sFit.protect[mi].size() && sFit.protect[mi][v]) ? 1 : 0);
                for (S32 k = 0; k < 3; ++k)
                {
                    lo.mV[k] = llmin(lo.mV[k], w.mV[k]);
                    hi.mV[k] = llmax(hi.mV[k], w.mV[k]);
                }
            }
            for (const auto& t : m.triangles)
            {
                if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= (S32)n || t[1] >= (S32)n || t[2] >= (S32)n) continue;
                G.tris.push_back({ (int)(base[mi] + t[0]), (int)(base[mi] + t[1]), (int)(base[mi] + t[2]) });
            }
        }
        for (S32 k = 0; k < 3; ++k) { lo.mV[k] -= 2.f * REACH; hi.mV[k] += 2.f * REACH; }
        auto inBox = [&](const LLVector3& p)
        {
            return p.mV[0] >= lo.mV[0] && p.mV[1] >= lo.mV[1] && p.mV[2] >= lo.mV[2] && p.mV[0] <= hi.mV[0] && p.mV[1] <= hi.mV[1] && p.mV[2] <= hi.mV[2];
        };

        // 2. the body near it: visible skin triangles in the garment's own regions, wound outward
        MGFitCore::Body B;
        std::set<LLUUID> garment;
        for (const auto& a : sApplied) garment.insert(a.prim);
        for (const auto& kv : gAgentAvatarp->mAttachmentPoints)
        {
            LLViewerJointAttachment* att = kv.second;
            if (!att || att->getIsHUDAttachment()) continue;
            for (const auto& ao : att->mAttachedObjects)
            {
                LLViewerObject* root = ao.get();
                if (!root || root->isDead() || sBodyOff.count(root->getAttachmentItemID())) continue;
                for (LLViewerObject* o : primsOf(root))
                {
                    if (!o || o->getPCode() != LL_PCODE_VOLUME || garment.count(o->getID())) continue;
                    LLVOVolume* vo = static_cast<LLVOVolume*>(o);
                    if (vo->mIsLocalMesh || !vo->isRiggedMesh()) continue;
                    const LLMeshSkinInfo* skin = vo->getSkinInfo();
                    LLVolume* src = vo->getVolume();
                    if (!skin || !src) continue;
                    vo->updateRiggedVolume(true, LLRiggedVolume::UPDATE_ALL_FACES, false);
                    LLRiggedVolume* rv = vo->getRiggedVolume();
                    if (!rv || rv->getNumVolumeFaces() != src->getNumVolumeFaces()) continue;
                    for (S32 f = 0; f < rv->getNumVolumeFaces(); ++f)
                    {
                        const LLTextureEntry* te = vo->getTE((U8)f);
                        if (te && (te->getColor().mV[VALPHA] < 0.05f || te->getID() == IMG_INVISIBLE)) continue;
                        const LLVolumeFace& sf = src->getVolumeFace(f);
                        const LLVolumeFace& df = rv->getVolumeFace(f);
                        if (!df.mPositions || !df.mIndices || df.mNumVertices != sf.mNumVertices) continue;
                        const S32 n = df.mNumVertices;
                        const bool flip = windingSign(sf) < 0.f;
                        // per point: kept (in the garment's regions) and its body index once used
                        std::vector<S8> keep(n, -1);
                        std::vector<S32> idx(n, -1);
                        auto kept = [&](S32 j) -> bool
                        {
                            if (keep[j] >= 0) return keep[j] != 0;
                            bool ok = true;
                            if (sf.mWeights)
                            {
                                const F32* w = sf.mWeights[j].getF32ptr();
                                S32 bi = -1;
                                F32 bw = -1.f;
                                for (S32 k = 0; k < 4; ++k)
                                {
                                    const F32 wt = w[k] - std::floor(w[k]);
                                    if (wt > bw) { bw = wt; bi = (S32)std::floor(w[k]); }
                                }
                                if (bi >= 0 && bi < (S32)skin->mJointNames.size() && !sFit.regions.count(regionOf(skin->mJointNames[bi]))) ok = false;
                            }
                            keep[j] = ok ? 1 : 0;
                            return ok;
                        };
                        auto index = [&](S32 j) -> int
                        {
                            if (idx[j] < 0)
                            {
                                idx[j] = (S32)B.pos.size();
                                const F32* p = df.mPositions[j].getF32ptr();
                                B.pos.emplace_back(p[0], p[1], p[2]);
                            }
                            return idx[j];
                        };
                        for (S32 i = 0; i + 2 < df.mNumIndices; i += 3)
                        {
                            const U16 ia = df.mIndices[i], ib = df.mIndices[i + 1], ic = df.mIndices[i + 2];
                            if (ia >= n || ib >= n || ic >= n) continue;
                            const LLVector3 a(df.mPositions[ia].getF32ptr()), b(df.mPositions[ib].getF32ptr()), c(df.mPositions[ic].getF32ptr());
                            if (!inBox(a) && !inBox(b) && !inBox(c)) continue;
                            if (!kept(ia) && !kept(ib) && !kept(ic)) continue;
                            const int A = index(ia), Bi = index(ib), C = index(ic);
                            B.tris.push_back(flip ? std::array<int, 3>{ A, C, Bi } : std::array<int, 3>{ A, Bi, C });
                        }
                    }
                }
            }
        }
        if (B.tris.empty()) return 0;

        // 3. solve, carry the moves to the file, again (2 rounds: a 90k-point garment costs ~1 s a round; the next sample goes on)
        S32 first = -1;
        S32 squashed = 0;   // pushes skipped where the skinning squashes moves
        for (S32 round = 0; round < 2; ++round)
        {
            MGFitCore::Result r;
            MGFitCore::solve(B, G, prm, r);
            if (first < 0) first = r.inside + r.pokes;
            bool any = false;
            for (size_t mi = 0; mi < sFit.meshes.size(); ++mi)
            {
                FitMesh& fm = sFit.meshes[mi];
                const MGCreatorDae::SkinnedMesh& m = sFit.dae.meshes[mi];
                const glm::mat4 gbs = glm::make_mat4(fm.skin->mBindShapeMatrix.getF32ptr());
                const size_t n = m.vertexCount();
                for (size_t v = 0; v < n; ++v)
                {
                    const size_t gi = base[mi] + v;
                    const MGFitCore::V3& pw = r.push[gi];
                    const F32 len = std::sqrt(pw.x * pw.x + pw.y * pw.y + pw.z * pw.z);
                    if (len < 1e-5f) continue;
                    // back to file space: the skinning's linear part at this point, inverted
                    const glm::mat3 L(glm::make_mat4(finals[mi][v].getF32ptr()) * gbs);
                    if (std::fabs(glm::determinant(L)) < 1e-10f) continue;
                    glm::vec3 d = glm::inverse(L) * glm::vec3(pw.x, pw.y, pw.z);
                    // <MG> 2026-11-14 (3): where the skinning squashes moves (blended bones at a bent elbow or
                    // shoulder: candy-wrapper), a small push on the avatar is a big one in the file, and each
                    // pose pumped it further (cuffs moved 166 mm, 1,371 triangles flipped). Such a point is not
                    // pushed in this pose: that clipping is the weights', not the shape's
                    const F32 dl = glm::length(d);
                    if (dl > len * 2.0f) { ++squashed; continue; }
                    if (dl > len * 1.5f && dl > 0.f) d *= (len * 1.5f) / dl;
                    double* wp = &fm.work[v * 3];
                    wp[0] += d.x; wp[1] += d.y; wp[2] += d.z;
                    // the total move stays within the largest push: measured on the avatar in this pose AND in
                    // the file (bind space), whichever is larger
                    const LLVector3 now = skinPoint(fm, finals[mi][v], v);
                    LLVector4a raw0((F32)m.positions[v * 3], (F32)m.positions[v * 3 + 1], (F32)m.positions[v * 3 + 2]);
                    LLVector4a t0, w0;
                    fm.skin->mBindShapeMatrix.affineTransform(raw0, t0);
                    finals[mi][v].affineTransform(t0, w0);
                    const LLVector3 was(w0.getF32ptr());
                    const F32 onAvatar = (now - was).length();
                    const glm::vec3 inFile = glm::mat3(gbs) * glm::vec3((F32)(wp[0] - m.positions[v * 3]), (F32)(wp[1] - m.positions[v * 3 + 1]), (F32)(wp[2] - m.positions[v * 3 + 2]));
                    const F32 total = llmax(onAvatar, glm::length(inFile));
                    if (total > CAP)
                    {
                        const double s = CAP / total;
                        for (S32 k = 0; k < 3; ++k) wp[k] = m.positions[v * 3 + k] + (wp[k] - m.positions[v * 3 + k]) * s;
                    }
                    const LLVector3 fin = total > CAP ? skinPoint(fm, finals[mi][v], v) : now;
                    fm.push[v] = llmax(fm.push[v], llmin(total, CAP));
                    G.pos[gi] = MGFitCore::V3(fin.mV[0], fin.mV[1], fin.mV[2]);
                    any = true;
                }
            }
            if (!any) break;
        }
        // <MG> did it fix what the stress test finds? The corrected garment checked by the test's own rule
        // (skin more than 0.8 mm through it, looked for within 2.5 cm); what is deeper than Max push is the
        // part the fit leaves to the creator
        {
            MGFitCore::Params chk = prm;
            chk.margin = -0.0006f;      // flagged under -0.8 mm (solve's own 0.2 mm tolerance)
            chk.maxPush = 0.025f;       // the stress test's reach
            chk.deepAt = sFit.maxPush;
            MGFitCore::Result r;
            MGFitCore::solve(B, G, chk, r);
            sRun.left = llmax(sRun.left, r.inside + r.pokes);
            sRun.squashed = llmax(sRun.squashed, squashed);
            sRun.deep = llmax(sRun.deep, r.overDeep);
            if (r.thinParts) LL_DEBUGS("MGCreator") << "Make it fit: " << r.thinParts << " thin worn pieces left out" << LL_ENDL;
        }
        return llmax(first, 0);
    }

    // <MG> 2026-11-14 (4): the fit's MOVE smoothed over the garment (the shape's own details untouched):
    // each push is spread with its own direction and goes back through each point's own skinning, and
    // the poses add up, so neighbours ended with very different moves and small folded triangles (a
    // rolled cuff) crumpled: 737 flipped, 3,326 torn on the owner's coat. Eight passes: 0 flipped, 18
    // torn, 90 % of the move kept. Run after every pose (the next poses fix what it reopens) and before
    // writing; more passes while any triangle is still turned over.
    void fitSmooth(S32 passes)
    {
        for (size_t mi = 0; mi < sFit.meshes.size(); ++mi)
        {
            FitMesh& fm = sFit.meshes[mi];
            const MGCreatorDae::SkinnedMesh& m = sFit.dae.meshes[mi];
            const size_t n = m.vertexCount();
            if (fm.work.size() != n * 3 || fm.nb.size() != n) continue;
            std::vector<double> d(n * 3), next;
            for (size_t i = 0; i < n * 3; ++i) d[i] = fm.work[i] - m.positions[i];
            auto isProtected = [&](size_t v) { return mi < sFit.protect.size() && v < sFit.protect[mi].size() && sFit.protect[mi][v]; };
            auto flipped = [&]() -> S32
            {
                S32 bad = 0;
                for (const auto& t : m.triangles)
                {
                    if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= (S32)n || t[1] >= (S32)n || t[2] >= (S32)n) continue;
                    glm::dvec3 p[3], q[3];
                    for (S32 k = 0; k < 3; ++k)
                    {
                        p[k] = glm::dvec3(m.positions[t[k] * 3], m.positions[t[k] * 3 + 1], m.positions[t[k] * 3 + 2]);
                        q[k] = p[k] + glm::dvec3(d[t[k] * 3], d[t[k] * 3 + 1], d[t[k] * 3 + 2]);
                    }
                    const glm::dvec3 n0 = glm::cross(p[1] - p[0], p[2] - p[0]), n1 = glm::cross(q[1] - q[0], q[2] - q[0]);
                    if (glm::dot(n0, n0) > 1e-24 && glm::dot(n0, n1) <= 0.0) ++bad;
                }
                return bad;
            };
            S32 total = 0;
            for (S32 round = 0; round < 4; ++round)
            {
                const S32 todo = round == 0 ? passes : 4;
                for (S32 it = 0; it < todo; ++it)
                {
                    next = d;
                    for (size_t v = 0; v < n; ++v)
                    {
                        if (fm.nb[v].empty() || isProtected(v)) continue;
                        double s[3] = { 0.0, 0.0, 0.0 };
                        for (S32 u : fm.nb[v]) for (S32 k = 0; k < 3; ++k) s[k] += d[u * 3 + k];
                        const double inv = 1.0 / (double)fm.nb[v].size();
                        for (S32 k = 0; k < 3; ++k) next[v * 3 + k] = 0.5 * d[v * 3 + k] + 0.5 * s[k] * inv;
                    }
                    d.swap(next);
                    ++total;
                }
                if (flipped() == 0) break;
            }
            for (size_t i = 0; i < n * 3; ++i) fm.work[i] = m.positions[i] + d[i];
            LL_DEBUGS("MGCreator") << "Make it fit: move smoothed (" << total << " passes)" << LL_ENDL;
        }
    }

    // all poses done: write the copy (the corrected positions), show it
    void finishFit()
    {
        if (!sFit.ready) return;
        fitSmooth(4 + sFit.smooth);
        S32 moved = 0;
        F32 maxPush = 0.f;
        std::vector<std::vector<double>> newPos;
        for (size_t mi = 0; mi < sFit.meshes.size(); ++mi)
        {
            FitMesh& fm = sFit.meshes[mi];
            const MGCreatorDae::SkinnedMesh& m = sFit.dae.meshes[mi];
            const size_t n = m.vertexCount();
            std::vector<double> np = fm.work;
            // points that were at one place (a seam split by UVs or normals) keep moving as one:
            // the longest of their moves for all of them, so no seam opens
            std::unordered_map<U64, std::vector<size_t>> at;
            for (size_t v = 0; v < n; ++v)
            {
                const U64 k = Grid::key((S32)std::llround(m.positions[v * 3] / 0.00001) & 0xFFFFF, (S32)std::llround(m.positions[v * 3 + 1] / 0.00001) & 0xFFFFF,
                                        (S32)std::llround(m.positions[v * 3 + 2] / 0.00001) & 0xFFFFF);
                at[k].push_back(v);
            }
            for (const auto& kv : at)
            {
                if (kv.second.size() < 2) continue;
                size_t best = kv.second[0];
                double bl = -1.0;
                for (size_t v : kv.second)
                {
                    const double dx = np[v * 3] - m.positions[v * 3], dy = np[v * 3 + 1] - m.positions[v * 3 + 1], dz = np[v * 3 + 2] - m.positions[v * 3 + 2];
                    const double l = dx * dx + dy * dy + dz * dz;
                    if (l > bl) { bl = l; best = v; }
                }
                for (size_t v : kv.second)
                {
                    if (mi < sFit.protect.size() && v < sFit.protect[mi].size() && sFit.protect[mi][v]) continue;
                    for (S32 k = 0; k < 3; ++k) np[v * 3 + k] = m.positions[v * 3 + k] + (np[best * 3 + k] - m.positions[best * 3 + k]);
                }
            }
            for (size_t v = 0; v < n; ++v)
            {
                const double dx = np[v * 3] - m.positions[v * 3], dy = np[v * 3 + 1] - m.positions[v * 3 + 1], dz = np[v * 3 + 2] - m.positions[v * 3 + 2];
                if (dx * dx + dy * dy + dz * dz > 1e-14) ++moved;
            }
            for (F32 pl : fm.push) maxPush = llmax(maxPush, pl);
            newPos.push_back(np);
        }
        const std::string out = MGCreatorDae::fitPathFor(sFit.dae.path);
        std::string err;
        const bool ok = MGCreatorDae::write(sFit.dae, newPos, {}, out, err);
        LLSD ev = LLSD().with("type", "lab_fit_done").with("ok", ok).with("moved", moved)
            .with("max_mm", (F32)ll_round(maxPush * 10000.f) / 10.f).with("path", out).with("name", baseName(out));
        if (!ok)
        {
            push(ev.with("error", err));
            sFit.ready = false;
            return;
        }
        // show it: the same file again is reloaded by Local Mesh; a new one is opened and worn
        LLSD r = openPath(out);
        if (r["ok"].asBoolean())
        {
            const LLUUID id(r["id"].asString());
            if (id != sWornFile)
            {
                sFit.pendingFile = id;
                sFit.standin = sStandIn;
                sFit.pendingTimer.reset();
            }
            else
            {
                // already worn: Local Mesh reloads it in place
                push(ev);
                push(LLSD().with("type", "lab_fit_worn").with("ok", true).with("file", id.asString()));
                sFit.ready = false;
                sFit.dae = MGCreatorDae::File();
                sFit.meshes.clear();
                return;
            }
        }
        push(ev);
        sFit.ready = false;
        sFit.dae = MGCreatorDae::File();
        sFit.meshes.clear();
    }


    // ------------------------------------------------------------------
    // Touch up: brushes on the worn garment, in the 3D view.
    // The garment file is skinned here as Second Life draws it (the same skin as Make
    // it fit); the mouse ray picks it; each dab moves the points under the brush, in
    // world terms, and turns that into the file's own space (inverse skinning), so a
    // correction made in one pose holds in all. A stroke ends on mouse up: the _fit copy
    // is written and Local Mesh reloads it. Protected points (a mask kept next to the
    // file) are left alone by the brushes and by Make it fit.
    // ------------------------------------------------------------------
    enum TouchBrush { TB_PUSH = 0, TB_SMOOTH, TB_INFLATE, TB_GRAB, TB_PROTECT, TB_WEIGHT, TB_WSMOOTH, TB_STIFF, TB_LEGS, TB_ERASE, TB_SELECT };
    bool weightBrush(S32 b) { return b == TB_WEIGHT || b == TB_WSMOOTH || b == TB_STIFF || b == TB_LEGS || b == TB_ERASE; }

    // one step of the undo history: the shape and the weights
    struct TouchSnap
    {
        std::vector<std::vector<double>> pos;
        std::vector<MGCreatorDae::NewSkin> skin;
    };

    struct Touch
    {
        bool active = false;
        MGCreatorDae::File dae;
        std::vector<FitMesh> meshes;                       // skins (packed weights)
        std::vector<std::vector<double>> pos;              // current positions (file space)
        std::vector<std::vector<std::vector<S32>>> nb;     // neighbours
        std::vector<std::vector<U8>> mask;                 // 1 = protected
        std::vector<std::vector<S32>> mirror;              // left/right partner or -1
        std::vector<std::vector<LLVector3>> world;         // skinned, current pose
        std::vector<std::vector<LLVector3>> wnorm;         // skinned normals (raw winding)
        std::vector<std::vector<LLMatrix4a>> finals;
        LLFrameTimer worldTimer;
        S32 brush = TB_PUSH;
        F32 radius = 0.04f;        // m
        F32 strength = 0.5f;       // 0..1
        bool symmetry = false;
        std::string outPath, maskPath;
        std::vector<TouchSnap> undo, redo;
        // <MG> weights: the skin as painted (file bone indices), the bone being painted
        std::vector<MGCreatorDae::NewSkin> skin;
        bool skinEdited = false;
        std::string bone;
        std::string stiffBone;     // Stiff: the steady bone this stroke's area follows (picked where it began)
        bool stiffManual = false;  // Stiff: the bone was chosen on the skeleton (Ctrl+click), not picked per stroke
        // skeleton overlay (weight brushes): each bone as drawn, to hover and Ctrl+click it
        struct BoneSeg { std::string name; F32 x0, y0, x1, y1; bool mine, volume; };
        std::vector<BoneSeg> boneSegs;
        std::string hoverBone;
        bool showBones = true;
        S32 edits = 0;
        // the stroke
        bool stroking = false;
        bool invert = false;       // shift: pull / shrink / unprotect
        F32 outward = 1.f;         // the mesh's winding: raw normals * outward = away from the body
        LLVector3 hit, hitN;
        bool hasHit = false;
        TouchSnap before;                                  // shape and weights when the stroke began
        // grab
        std::vector<std::tuple<S32, S32, F32>> grabSet;
        std::vector<std::vector<double>> grabBase;
        LLVector3 grabStart, grabPlaneN;
        LLFrameTimer dabTimer;
        std::vector<std::pair<S32, S32>> touched;          // shown while stroking
        // live: the worn garment follows the brush (shape and weights) while it is used
        bool live = false, liveDirty = false;
        LLFrameTimer liveTimer;
        LLUUID pendingFile;
        LLUUID standin;
        LLFrameTimer pendingTimer;
        // <MG> Edit Mesh (brush TB_SELECT): what is selected and hidden, and how it is changed
        S32 selMode = 0;           // 0 point, 1 edge, 2 face, 3 piece
        S32 gizmo = 0;             // 0 move, 1 rotate, 2 scale
        bool softOn = false;
        F32 softR = 0.04f;         // m
        bool xray = false, showEdges = true, showPoints = true;
        std::vector<std::vector<U8>> sel, hidden;
        S32 lastA = -1, lastB = -1;
        size_t lastMesh = 0;
        bool hideDirty = false;
        F32 editOut = 0.f;         // the mesh's winding (raw normals * this = outward), 0 = not measured
    } sTouch;

    // Weight view state (see weightViewDraw, below)
    struct WeightView
    {
        bool on = false, dirty = false;
        std::string bone;
        struct Face
        {
            LLUUID prim; S32 te = 0;
            LLUUID tex; LLColor4 color; U8 fullbright = 0; F32 ss = 1.f, st = 1.f, os = 0.f, ot = 0.f, rot = 0.f;
            LLPointer<LLImageRaw> raw;
            LLPointer<LLViewerTexture> image;
            // a mirrored UV layout (both halves on the same part of the image): while the view is on,
            // each half gets its own half of the image; the face's own UVs are kept to put back
            LLVolume* uvVol = nullptr;
            std::vector<F32> uv0;
            S32 split = -1;            // -1 not checked, 0 no, 1 mirrored
            bool uvApplied = false;
        };
        std::vector<Face> faces;
        LLFrameTimer timer;
    } sWV;
    const S32 WV_SIZE = 1024;

    bool makeSkin(const MGCreatorDae::SkinnedMesh& m, FitMesh& fm)
    {
        if (m.joints.size() > LL_MAX_JOINTS_PER_MESH_OBJECT) return false;
        fm.skin = new LLMeshSkinInfo();
        fm.skin->mJointNames = m.joints;
        fm.skin->mJointNums.assign(m.joints.size(), -1);   // see fitPrepare
        for (const auto& ib : m.invBind) fm.skin->mInvBindMatrix.push_back(fromCollada(ib));
        fm.skin->mBindShapeMatrix = fromCollada(m.bindShape);
        const size_t n = m.vertexCount();
        fm.packed.resize(n);
        for (size_t v = 0; v < n; ++v)
        {
            std::vector<std::pair<int, double>> w = m.weights[v];
            std::sort(w.begin(), w.end(), [](const std::pair<int, double>& a, const std::pair<int, double>& b) { return a.second > b.second; });
            if (w.size() > 4) w.resize(4);
            double sum = 0.0;
            for (const auto& jw : w) sum += jw.second;
            std::array<F32, 4> pk{ 0.f, 0.f, 0.f, 0.f };
            if (w.empty() || sum <= 0.0) pk[0] = 0.9999f;
            for (size_t k = 0; k < w.size() && sum > 0.0; ++k)
            {
                pk[k] = (F32)w[k].first + llclamp((F32)(w[k].second / sum), 0.f, 0.9999f);
            }
            fm.packed[v] = pk;
        }
        return true;
    }

    // <MG> Weight brush --------------------------------------------------
    void touchSkin();
    F32 falloff(F32 d, F32 r);
    // the live skin of a mesh from the painted weights (4 strongest per point, as SL draws it)
    void repack(size_t mi)
    {
        const MGCreatorDae::NewSkin& sk = sTouch.skin[mi];
        FitMesh& fm = sTouch.meshes[mi];
        LLPointer<LLMeshSkinInfo> info = new LLMeshSkinInfo();
        info->mJointNames = sk.joints;
        info->mJointNums.assign(sk.joints.size(), -1);
        for (const auto& ib : sk.invBind) info->mInvBindMatrix.push_back(fromCollada(ib));
        info->mBindShapeMatrix = fromCollada(sTouch.dae.meshes[mi].bindShape);
        fm.skin = info;
        const size_t n = sk.weights.size();
        fm.packed.resize(n);
        for (size_t v = 0; v < n; ++v)
        {
            std::vector<std::pair<int, double>> w = sk.weights[v];
            std::sort(w.begin(), w.end(), [](const std::pair<int, double>& a, const std::pair<int, double>& b) { return a.second > b.second; });
            if (w.size() > 4) w.resize(4);
            double sum = 0.0;
            for (const auto& jw : w) sum += jw.second;
            std::array<F32, 4> pk{ 0.f, 0.f, 0.f, 0.f };
            if (w.empty() || sum <= 0.0) pk[0] = 0.9999f;
            for (size_t k = 0; k < w.size() && sum > 0.0; ++k) pk[k] = (F32)w[k].first + llclamp((F32)(w[k].second / sum), 0.f, 0.9999f);
            fm.packed[v] = pk;
        }
    }

    // SL's skeleton (bones and volumes, default places), read once
    const std::vector<MGRig::SlBone>& slSkeleton()
    {
        static std::vector<MGRig::SlBone> sl;
        if (sl.empty()) MGRig::loadSlSkeleton(gDirUtilp->getExpandedFilename(LL_PATH_CHARACTER, "avatar_skeleton.xml"), sl);
        return sl;
    }
    S32 slIndex(const std::string& name)
    {
        const auto& sl = slSkeleton();
        for (size_t i = 0; i < sl.size(); ++i) if (sl[i].name == name) return (S32)i;
        return -1;
    }

    // the mesh's index of a bone; a bone it does not have yet is added: the body's bind if the
    // body has it, else placed from its nearest parent in this mesh plus SL's offsets
    S32 jointFor(size_t mi, const std::string& name)
    {
        MGCreatorDae::NewSkin& sk = sTouch.skin[mi];
        for (size_t j = 0; j < sk.joints.size(); ++j) if (sk.joints[j] == name) return (S32)j;
        if (sk.joints.size() >= LL_MAX_JOINTS_PER_MESH_OBJECT) return -1;
        const auto& sl = slSkeleton();
        const S32 bi = slIndex(name);
        if (bi < 0) return -1;
        std::array<double, 16> ib = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        bool done = false;
        if (sBody)
            for (const auto& bm : sBody->meshes)
                for (size_t j = 0; j < bm.joints.size() && !done; ++j)
                    if (bm.joints[j] == name && j < bm.invBind.size()) { ib = bm.invBind[j]; done = true; }
        if (!done)
        {
            // walk up to a parent this mesh has; its place plus SL's offsets down to the bone
            double off[3] = { sl[bi].local[0], sl[bi].local[1], sl[bi].local[2] };
            double base[3] = { sl[bi].world[0] - off[0], sl[bi].world[1] - off[1], sl[bi].world[2] - off[2] };
            for (S32 p = sl[bi].parent; p >= 0; p = sl[p].parent)
            {
                bool found = false;
                for (size_t j = 0; j < sk.joints.size(); ++j)
                {
                    MGCreatorWeights::Mat w;
                    if (sk.joints[j] != sl[p].name || !MGCreatorWeights::invert(sk.invBind[j], w)) continue;
                    base[0] = w[3]; base[1] = w[7]; base[2] = w[11];
                    off[0] = off[1] = off[2] = 0.0;
                    for (S32 q = bi; q >= 0 && q != p; q = sl[q].parent) for (int k = 0; k < 3; ++k) off[k] += sl[q].local[k];
                    found = true;
                    break;
                }
                if (found) break;
            }
            ib[3] = -(base[0] + off[0]); ib[7] = -(base[1] + off[1]); ib[11] = -(base[2] + off[2]);
        }
        sk.joints.push_back(name);
        sk.invBind.push_back(ib);
        return (S32)sk.joints.size() - 1;
    }

    std::string mirrorBone(const std::string& n)
    {
        auto swapIn = [&](const std::string& a, const std::string& b) -> std::string
        {
            const size_t p = n.find(a);
            if (p != std::string::npos) return n.substr(0, p) + b + n.substr(p + a.size());
            return std::string();
        };
        std::string m;
        if (!(m = swapIn("Left", "Right")).empty()) return m;
        if (!(m = swapIn("Right", "Left")).empty()) return m;
        if (n.compare(0, 2, "L_") == 0) return "R_" + n.substr(2);
        if (n.compare(0, 2, "R_") == 0) return "L_" + n.substr(2);
        if (n.compare(0, 5, "LEFT_") == 0) return "RIGHT_" + n.substr(5);
        if (n.compare(0, 6, "RIGHT_") == 0) return "LEFT_" + n.substr(6);
        return n;
    }

    // more (amount > 0) or less (< 0) of bone j on one point; the rest scaled so they add up to 1;
    // weight taken from a point's only bone goes to that bone's parent
    void paintPoint(size_t mi, size_t v, S32 j, double amount)
    {
        auto& w = sTouch.skin[mi].weights[v];
        double cur = 0.0, others = 0.0;
        for (const auto& jw : w) { if (jw.first == j) cur += jw.second; else others += jw.second; }
        const double total = cur + others;
        if (total > 0.0) { cur /= total; others /= total; }
        double nw = amount >= 0.0 ? cur + amount * (1.0 - cur) : cur * (1.0 + amount);
        nw = llclamp(nw, 0.0, 1.0);
        std::map<int, double> acc;
        if (others > 1e-9)
        {
            for (const auto& jw : w) if (jw.first != j) acc[jw.first] += (jw.second / total) * (1.0 - nw) / others;
        }
        else if (nw < 1.0)
        {
            const auto& sl = slSkeleton();
            const S32 bi = slIndex(sTouch.skin[mi].joints[j]);
            const S32 pj = bi >= 0 && sl[bi].parent >= 0 ? jointFor(mi, sl[sl[bi].parent].name) : -1;
            if (pj < 0) return;   // nothing to give it to
            acc[pj] += 1.0 - nw;
        }
        if (nw > 1e-6) acc[j] += nw;
        w.clear();
        for (const auto& kv : acc) if (kv.second > 1e-6) w.push_back(kv);
    }

    // one dab of the weight brushes at sTouch.hit
    // the steady bone an area should follow to be stiff, from the bones that move it now: the
    // pelvis below the waist (skirts, coat tails), the chest for the torso and capes, the head
    // for hair and hoods (as Second Life creators rig them: the top of the hair on the head)
    std::string stiffAnchorFor(size_t mi, size_t v)
    {
        const MGCreatorDae::NewSkin& sk = sTouch.skin[mi];
        double pelvis = 0, chest = 0, head = 0;
        for (const auto& jw : sk.weights[v])
        {
            if (jw.first < 0 || jw.first >= (int)sk.joints.size()) continue;
            const std::string& b = sk.joints[jw.first];
            auto has = [&](const char* s) { return b.find(s) != std::string::npos; };
            if (has("Neck") || has("NECK") || has("Head") || has("HEAD") || has("Skull") || has("Face") || has("Eye") || has("Jaw")) head += jw.second;
            else if (has("Pelvis") || has("PELVIS") || has("BUTT") || has("Hip") || has("Knee") || has("Ankle") || has("Foot") || has("Toe") || has("LEG") || has("HindLimb")) pelvis += jw.second;
            else chest += jw.second;
        }
        if (head >= pelvis && head >= chest) return "mHead";
        if (pelvis >= chest) return "mPelvis";
        return "mChest";
    }

    // Stiff: the area follows one steady bone, like a long skirt hung from the hips (it no longer
    // stretches with the legs). Follow legs: the area shared between the legs as Second Life's own
    // skirt does (left side the left leg, right side the right one, the middle half and half, part
    // on the pelvis), so the legs no longer walk through it. Shift: less of it.
    void weightDabShape()
    {
        const F32 r = sTouch.radius;
        const double k0 = (0.04 + 0.3 * sTouch.strength) * (sTouch.invert ? -1.0 : 1.0);
        const auto& sl = slSkeleton();
        F32 kneeZ = 0.f;
        { const S32 k = slIndex("mKneeLeft"); if (k >= 0) kneeZ = (F32)sl[k].world[2]; }
        for (size_t mi = 0; mi < sTouch.meshes.size(); ++mi)
        {
            const auto& W = sTouch.world[mi];
            const MGCreatorDae::SkinnedMesh& m = sTouch.dae.meshes[mi];
            bool changed = false;
            if (sTouch.brush == TB_STIFF)
            {
                if (sTouch.stiffBone.empty()) continue;
                const S32 j = jointFor(mi, sTouch.stiffBone);
                if (j < 0) continue;
                for (size_t v = 0; v < W.size(); ++v)
                {
                    const F32 f = falloff((W[v] - sTouch.hit).length(), r);
                    if (f <= 0.f || sTouch.mask[mi][v]) continue;
                    paintPoint(mi, v, j, k0 * f);
                    if (sTouch.symmetry)
                    {
                        const S32 m2 = sTouch.mirror[mi][v];
                        if (m2 >= 0 && m2 != (S32)v && !sTouch.mask[mi][m2]) paintPoint(mi, (size_t)m2, j, k0 * f);
                    }
                    sTouch.touched.push_back({ (S32)mi, (S32)v });
                    changed = true;
                }
            }
            else
            {
                const S32 jp = jointFor(mi, "mPelvis");
                const S32 jhl = jointFor(mi, "mHipLeft"), jhr = jointFor(mi, "mHipRight");
                const S32 jkl = jointFor(mi, "mKneeLeft"), jkr = jointFor(mi, "mKneeRight");
                if (jp < 0 || jhl < 0 || jhr < 0) continue;
                for (size_t v = 0; v < W.size() && v < sTouch.skin[mi].weights.size(); ++v)
                {
                    const F32 f = falloff((W[v] - sTouch.hit).length(), r);
                    if (f <= 0.f || sTouch.mask[mi][v]) continue;
                    // where it sits in the garment's own space (Y: left)
                    const auto p = MGCreatorWeights::transformPoint(m.bindShape, &sTouch.pos[mi][v * 3]);
                    const double left = llclamp(0.5 + p[1] / 0.12, 0.0, 1.0);   // blended over 12 cm in the middle
                    const bool low = kneeZ > 0.f && p[2] < kneeZ && jkl >= 0 && jkr >= 0;
                    const S32 jl = low ? jkl : jhl, jr = low ? jkr : jhr;
                    const double legs = 0.7;   // the rest stays on the pelvis, so it still hangs from the hips
                    std::map<int, double> target;
                    target[jp] += 1.0 - legs;
                    target[jl] += legs * left;
                    target[jr] += legs * (1.0 - left);
                    const double k = llclamp(k0 * f, -1.0, 1.0);
                    auto& w = sTouch.skin[mi].weights[v];
                    std::map<int, double> acc;
                    double sum = 0.0;
                    for (const auto& jw : w) sum += jw.second;
                    for (const auto& jw : w) acc[jw.first] += (sum > 0 ? jw.second / sum : 0.0) * (1.0 - std::fabs(k));
                    if (k > 0) for (const auto& t : target) acc[t.first] += k * t.second;
                    else
                    {
                        // Shift: back towards the pelvis alone (stiffer)
                        acc[jp] += -k;
                    }
                    w.clear();
                    for (const auto& kv : acc) if (kv.second > 1e-6) w.push_back(kv);
                    sTouch.touched.push_back({ (S32)mi, (S32)v });
                    changed = true;
                }
            }
            if (changed) { repack(mi); sTouch.skinEdited = true; ++sTouch.edits; sWV.dirty = true; }
        }
        touchSkin();
    }

    // Make it a skirt: everything below the hips rigged the way Second Life skirts are. At the
    // hips it hangs from the pelvis; towards the hem more and more of it follows the leg it is in
    // front of or behind (left side the left hip, right side the right hip, a soft middle), up to
    // `legs` at the hem. Protected points keep theirs. legs 0 = stiff.
    S32 touchSkirt(double legs)
    {
        legs = llclamp(legs, 0.0, 0.9);
        const auto& sl = slSkeleton();
        F32 hipZ = 0.f, kneeZ = 0.f;
        { const S32 h = slIndex("mHipLeft"); if (h >= 0) hipZ = (F32)sl[h].world[2]; }
        { const S32 k = slIndex("mKneeLeft"); if (k >= 0) kneeZ = (F32)sl[k].world[2]; }
        if (hipZ <= kneeZ + 0.05f) return 0;
        S32 changed = 0;
        for (size_t mi = 0; mi < sTouch.meshes.size(); ++mi)
        {
            const MGCreatorDae::SkinnedMesh& m = sTouch.dae.meshes[mi];
            const S32 jp = jointFor(mi, "mPelvis"), jl = jointFor(mi, "mHipLeft"), jr = jointFor(mi, "mHipRight");
            if (jp < 0 || jl < 0 || jr < 0) continue;
            bool any = false;
            for (size_t v = 0; v < m.vertexCount() && v < sTouch.skin[mi].weights.size(); ++v)
            {
                if (sTouch.mask[mi][v]) continue;
                const auto p = MGCreatorWeights::transformPoint(m.bindShape, &sTouch.pos[mi][v * 3]);
                if (p[2] >= hipZ) continue;   // above the hips: as it was
                // 0 at the hips .. 1 at the knees (and below), eased
                double t = llclamp((hipZ - p[2]) / (hipZ - kneeZ), 0.0, 1.0);
                t = t * t * (3.0 - 2.0 * t);
                const double share = legs * t;
                const double left = llclamp(0.5 + p[1] / 0.12, 0.0, 1.0);
                // the parts of its weights that are not body (a sleeve end, a strap) are kept in scale
                auto& w = sTouch.skin[mi].weights[v];
                w.clear();
                w.push_back({ jp, 1.0 - share });
                if (share * left > 1e-4) w.push_back({ jl, share * left });
                if (share * (1.0 - left) > 1e-4) w.push_back({ jr, share * (1.0 - left) });
                ++changed;
                any = true;
            }
            if (any) { repack(mi); sTouch.skinEdited = true; ++sTouch.edits; }
        }
        return changed;
    }

    void weightDab()
    {
        const F32 r = sTouch.radius;
        sTouch.touched.clear();
        if ((sTouch.brush == TB_WEIGHT || sTouch.brush == TB_ERASE) && sTouch.bone.empty()) return;
        if (sTouch.brush == TB_STIFF || sTouch.brush == TB_LEGS) { weightDabShape(); return; }
        for (size_t mi = 0; mi < sTouch.meshes.size(); ++mi)
        {
            const auto& W = sTouch.world[mi];
            bool changed = false;
            if (sTouch.brush == TB_WEIGHT || sTouch.brush == TB_ERASE)
            {
                const S32 j = jointFor(mi, sTouch.bone);
                if (j < 0) continue;
                const std::string mb = mirrorBone(sTouch.bone);
                const S32 jm = sTouch.symmetry && mb != sTouch.bone ? jointFor(mi, mb) : j;
                const double base = 0.03 + 0.22 * sTouch.strength;   // per dab, at the centre
                for (size_t v = 0; v < W.size(); ++v)
                {
                    const F32 f = falloff((W[v] - sTouch.hit).length(), r);
                    if (f <= 0.f || sTouch.mask[mi][v]) continue;
                    // Erase: the chosen bone lets go of this part (the other bones take over); Shift: the opposite
                    const double a = base * f * ((sTouch.invert != (sTouch.brush == TB_ERASE)) ? -1.0 : 1.0);
                    paintPoint(mi, v, j, a);
                    if (sTouch.symmetry)
                    {
                        const S32 m2 = sTouch.mirror[mi][v];
                        if (m2 >= 0 && m2 != (S32)v && !sTouch.mask[mi][m2] && jm >= 0) paintPoint(mi, (size_t)m2, jm, a);
                    }
                    sTouch.touched.push_back({ (S32)mi, (S32)v });
                    changed = true;
                }
            }
            else
            {
                // smooth: each point towards its neighbours' weights
                const auto src = sTouch.skin[mi].weights;
                const double k0 = 0.1 + 0.5 * sTouch.strength;
                for (size_t v = 0; v < W.size() && v < src.size(); ++v)
                {
                    const F32 f = falloff((W[v] - sTouch.hit).length(), r);
                    if (f <= 0.f || sTouch.mask[mi][v] || sTouch.nb[mi][v].empty()) continue;
                    std::map<int, double> avg;
                    for (S32 u : sTouch.nb[mi][v]) for (const auto& jw : src[u]) avg[jw.first] += jw.second / (double)sTouch.nb[mi][v].size();
                    const double k = k0 * f;
                    std::map<int, double> acc;
                    for (const auto& jw : src[v]) acc[jw.first] += (1.0 - k) * jw.second;
                    for (const auto& kv : avg) acc[kv.first] += k * kv.second;
                    auto& w = sTouch.skin[mi].weights[v];
                    w.clear();
                    for (const auto& kv : acc) if (kv.second > 1e-6) w.push_back(kv);
                    sTouch.touched.push_back({ (S32)mi, (S32)v });
                    changed = true;
                }
            }
            if (changed) { repack(mi); sTouch.skinEdited = true; ++sTouch.edits; }
        }
        touchSkin();
    }

    // Ctrl+click: the bone that moves the point under the mouse most
    std::string boneAt(const LLVector3& hit)
    {
        F32 best = 1e9f;
        std::string bone;
        for (size_t mi = 0; mi < sTouch.world.size(); ++mi)
        {
            const auto& W = sTouch.world[mi];
            for (size_t v = 0; v < W.size() && v < sTouch.skin[mi].weights.size(); ++v)
            {
                const F32 d = (W[v] - hit).lengthSquared();
                if (d >= best) continue;
                double bw = -1.0;
                std::string b;
                for (const auto& jw : sTouch.skin[mi].weights[v])
                    if (jw.second > bw && jw.first >= 0 && jw.first < (int)sTouch.skin[mi].joints.size()) { bw = jw.second; b = sTouch.skin[mi].joints[jw.first]; }
                if (!b.empty()) { best = d; bone = b; }
            }
        }
        return bone;
    }

    // the skin to write: 4 strongest per point, adding up to 1
    std::vector<MGCreatorDae::NewSkin> skinToWrite()
    {
        std::vector<MGCreatorDae::NewSkin> out = sTouch.skin;
        for (auto& sk : out)
            for (auto& w : sk.weights)
            {
                std::sort(w.begin(), w.end(), [](const std::pair<int, double>& a, const std::pair<int, double>& b) { return a.second > b.second; });
                if (w.size() > 4) w.resize(4);
                double sum = 0.0;
                for (const auto& jw : w) sum += jw.second;
                if (sum > 0.0) for (auto& jw : w) jw.second /= sum;
            }
        return out;
    }

    std::string maskPathFor(const std::string& fitPath)
    {
        return fitPath + ".protect";
    }

    void loadMask(const std::string& path, std::vector<std::vector<U8>>& mask)
    {
        std::ifstream in(path);
        if (!in) return;
        size_t mi, v;
        while (in >> mi >> v)
        {
            if (mi < mask.size() && v < mask[mi].size()) mask[mi][v] = 1;
        }
    }

    bool saveMask(const std::string& path, const std::vector<std::vector<U8>>& mask)
    {
        std::ofstream out(path, std::ios::trunc);
        if (!out) return false;
        for (size_t mi = 0; mi < mask.size(); ++mi)
            for (size_t v = 0; v < mask[mi].size(); ++v)
                if (mask[mi][v]) out << mi << ' ' << v << '\n';
        return true;
    }

    // the garment in the current pose (positions + normals), as Second Life draws it
    void touchSkin()
    {
        if (!sTouch.active || !isAgentAvatarValid()) return;
        for (size_t mi = 0; mi < sTouch.meshes.size(); ++mi)
        {
            FitMesh& fm = sTouch.meshes[mi];
            const MGCreatorDae::SkinnedMesh& m = sTouch.dae.meshes[mi];
            const std::vector<double>& p = sTouch.pos[mi];
            const S32 count = (S32)fm.skin->mJointNames.size();
            LLMatrix4a mat[LL_MAX_JOINTS_PER_MESH_OBJECT];
            LLSkinningUtil::initSkinningMatrixPalette(mat, count, fm.skin.get(), gAgentAvatarp);
            const LLMatrix4a& bs = fm.skin->mBindShapeMatrix;
            const size_t n = m.vertexCount();
            sTouch.world[mi].resize(n);
            sTouch.finals[mi].resize(n);
            for (size_t v = 0; v < n; ++v)
            {
                LLVector4a raw((F32)p[v * 3], (F32)p[v * 3 + 1], (F32)p[v * 3 + 2]);
                LLVector4a t, dst;
                bs.affineTransform(raw, t);
                std::array<F32, 4> pk = fm.packed[v];
                LLSkinningUtil::getPerVertexSkinMatrix(pk.data(), mat, true, sTouch.finals[mi][v], count);
                sTouch.finals[mi][v].affineTransform(t, dst);
                sTouch.world[mi][v].set(dst.getF32ptr());
            }
            std::vector<LLVector3>& nr = sTouch.wnorm[mi];
            nr.assign(n, LLVector3::zero);
            for (const auto& tr : m.triangles)
            {
                if (tr[0] < 0 || tr[1] < 0 || tr[2] < 0 || tr[0] >= (S32)n || tr[1] >= (S32)n || tr[2] >= (S32)n) continue;
                const LLVector3& a = sTouch.world[mi][tr[0]];
                const LLVector3 fn = (sTouch.world[mi][tr[1]] - a) % (sTouch.world[mi][tr[2]] - a);
                nr[tr[0]] += fn; nr[tr[1]] += fn; nr[tr[2]] += fn;
            }
            for (auto& x : nr) x.normVec();
        }
        sTouch.worldTimer.reset();
    }

    // the mouse ray against the garment: the nearest hit, its normal facing the camera
    bool touchPick(S32 x, S32 y, LLVector3& hit, LLVector3& n, LLVector3* rayDir = nullptr)
    {
        if (!sTouch.active) return false;
        const LLVector3 o = LLViewerCamera::getInstance()->getOrigin();
        LLVector3 d = gViewerWindow->mouseDirectionGlobal(x, y);
        d.normVec();
        if (rayDir) *rayDir = d;
        F32 bestT = 1e9f;
        bool found = false;
        LLVector3 rawN;
        for (size_t mi = 0; mi < sTouch.meshes.size(); ++mi)
        {
            const auto& W = sTouch.world[mi];
            for (const auto& tr : sTouch.dae.meshes[mi].triangles)
            {
                if (tr[0] < 0 || tr[1] < 0 || tr[2] < 0 || tr[0] >= (S32)W.size() || tr[1] >= (S32)W.size() || tr[2] >= (S32)W.size()) continue;
                Tri t{ W[tr[0]], W[tr[1]], W[tr[2]] };
                F32 dist;
                if (!rayTri(o, d, t, dist) || dist <= 0.f || dist >= bestT) continue;
                bestT = dist;
                rawN = (t.b - t.a) % (t.c - t.a);
                found = true;
            }
        }
        if (!found) return false;
        hit = o + d * bestT;
        rawN.normVec();
        // the side facing the camera is the outside; the mesh's winding follows from it
        sTouch.outward = (rawN * d < 0.f) ? 1.f : -1.f;
        n = rawN * sTouch.outward;
        return true;
    }

    // world move of one point -> file space (the skinning's linear part, inverted, capped)
    bool worldToFile(size_t mi, size_t v, const LLVector3& dw, LLVector3& df)
    {
        const glm::mat3 L(glm::make_mat4(sTouch.finals[mi][v].getF32ptr()) * glm::make_mat4(sTouch.meshes[mi].skin->mBindShapeMatrix.getF32ptr()));
        if (std::fabs(glm::determinant(L)) < 1e-10f) return false;
        glm::vec3 d = glm::inverse(L) * glm::vec3(dw.mV[0], dw.mV[1], dw.mV[2]);
        const F32 want = dw.length(), dl = glm::length(d);
        if (dl > want * 1.5f && dl > 0.f) d *= (want * 1.5f) / dl;
        df.set(d.x, d.y, d.z);
        return true;
    }

    void moveFile(size_t mi, size_t v, const LLVector3& df)
    {
        std::vector<double>& p = sTouch.pos[mi];
        p[v * 3] += df.mV[0];
        p[v * 3 + 1] += df.mV[1];
        p[v * 3 + 2] += df.mV[2];
        // symmetry: the left/right partner gets the mirrored move (Y is left in SL's frame)
        if (sTouch.symmetry)
        {
            const S32 m2 = sTouch.mirror[mi][v];
            if (m2 >= 0 && m2 != (S32)v && !sTouch.mask[mi][m2])
            {
                p[m2 * 3] += df.mV[0];
                p[m2 * 3 + 1] -= df.mV[1];
                p[m2 * 3 + 2] += df.mV[2];
            }
        }
    }

    F32 falloff(F32 d, F32 r)
    {
        if (d >= r) return 0.f;
        const F32 u = 1.f - (d * d) / (r * r);
        return u * u;
    }

    // one dab of the current brush at sTouch.hit
    void touchDab()
    {
        sTouch.liveDirty = true;
        sWV.dirty = true;
        if (weightBrush(sTouch.brush)) { weightDab(); return; }
        const F32 r = sTouch.radius;
        const F32 r2 = r * r;
        const F32 step = 0.0002f + 0.0018f * sTouch.strength;   // 0.2 .. 2 mm per dab at the centre
        sTouch.touched.clear();
        for (size_t mi = 0; mi < sTouch.meshes.size(); ++mi)
        {
            const auto& W = sTouch.world[mi];
            std::vector<double> smoothSrc;
            if (sTouch.brush == TB_SMOOTH) smoothSrc = sTouch.pos[mi];
            for (size_t v = 0; v < W.size(); ++v)
            {
                const F32 d2 = (W[v] - sTouch.hit).lengthSquared();
                if (d2 >= r2) continue;
                const F32 f = falloff(std::sqrt(d2), r);
                if (sTouch.brush == TB_PROTECT)
                {
                    const U8 val = sTouch.invert ? 0 : 1;
                    sTouch.mask[mi][v] = val;
                    if (sTouch.symmetry && sTouch.mirror[mi][v] >= 0) sTouch.mask[mi][sTouch.mirror[mi][v]] = val;
                    sTouch.touched.push_back({ (S32)mi, (S32)v });
                    continue;
                }
                if (sTouch.mask[mi][v]) continue;
                LLVector3 df;
                if (sTouch.brush == TB_SMOOTH)
                {
                    const auto& nbs = sTouch.nb[mi][v];
                    if (nbs.empty()) continue;
                    double ax = 0, ay = 0, az = 0;
                    for (S32 u : nbs) { ax += smoothSrc[u * 3]; ay += smoothSrc[u * 3 + 1]; az += smoothSrc[u * 3 + 2]; }
                    const double inv = 1.0 / (double)nbs.size();
                    const F32 k = llclamp(0.15f + 0.6f * sTouch.strength, 0.f, 0.9f) * f;
                    df.set((F32)((ax * inv - smoothSrc[v * 3]) * k), (F32)((ay * inv - smoothSrc[v * 3 + 1]) * k), (F32)((az * inv - smoothSrc[v * 3 + 2]) * k));
                }
                else
                {
                    const LLVector3 dirW = (sTouch.brush == TB_INFLATE) ? sTouch.wnorm[mi][v] * sTouch.outward : sTouch.hitN;
                    const LLVector3 dw = dirW * (step * f * (sTouch.invert ? -1.f : 1.f));
                    if (!worldToFile(mi, v, dw, df)) continue;
                }
                moveFile(mi, v, df);
                sTouch.touched.push_back({ (S32)mi, (S32)v });
                ++sTouch.edits;
            }
        }
        touchSkin();
    }

    void touchGrabBegin()
    {
        sTouch.grabSet.clear();
        sTouch.grabBase = sTouch.pos;
        sTouch.grabStart = sTouch.hit;
        const F32 r = sTouch.radius;
        for (size_t mi = 0; mi < sTouch.meshes.size(); ++mi)
        {
            const auto& W = sTouch.world[mi];
            for (size_t v = 0; v < W.size(); ++v)
            {
                if (sTouch.mask[mi][v]) continue;
                const F32 f = falloff((W[v] - sTouch.hit).length(), r);
                if (f > 0.f) sTouch.grabSet.push_back(std::make_tuple((S32)mi, (S32)v, f));
            }
        }
    }

    void touchGrabMove(S32 x, S32 y)
    {
        sTouch.liveDirty = true;
        const LLVector3 o = LLViewerCamera::getInstance()->getOrigin();
        LLVector3 d = gViewerWindow->mouseDirectionGlobal(x, y);
        d.normVec();
        const F32 den = d * sTouch.grabPlaneN;
        if (std::fabs(den) < 1e-6f) return;
        const F32 t = ((sTouch.grabStart - o) * sTouch.grabPlaneN) / den;
        if (t <= 0.f) return;
        const LLVector3 cur = o + d * t;
        LLVector3 dw = cur - sTouch.grabStart;
        const F32 maxMove = 0.05f;   // one grab moves cloth 5 cm at most
        if (dw.length() > maxMove) dw *= maxMove / dw.length();
        sTouch.pos = sTouch.grabBase;
        sTouch.touched.clear();
        for (const auto& g : sTouch.grabSet)
        {
            const size_t mi = (size_t)std::get<0>(g), v = (size_t)std::get<1>(g);
            LLVector3 df;
            if (!worldToFile(mi, v, dw * std::get<2>(g), df)) continue;
            moveFile(mi, v, df);
            sTouch.touched.push_back({ (S32)mi, (S32)v });
        }
        ++sTouch.edits;
        touchSkin();
    }

    // the stroke is over: write the _fit copy and show it
    void touchSave()
    {
        std::string err;
        const bool ok = sTouch.skinEdited ? MGCreatorDae::writeSkinned(sTouch.dae, sTouch.pos, skinToWrite(), sTouch.outPath, err)
                                          : MGCreatorDae::write(sTouch.dae, sTouch.pos, {}, sTouch.outPath, err);
        saveMask(sTouch.maskPath, sTouch.mask);
        LLSD ev = LLSD().with("type", "lab_touch_saved").with("ok", ok).with("name", baseName(sTouch.outPath))
            .with("undo", (S32)sTouch.undo.size()).with("redo", (S32)sTouch.redo.size());
        if (!ok) { push(ev.with("error", err)); return; }
        LLSD r = openPath(sTouch.outPath);
        if (r["ok"].asBoolean())
        {
            const LLUUID id(r["id"].asString());
            if (id != sWornFile)
            {
                sTouch.pendingFile = id;
                sTouch.standin = sStandIn;
                sTouch.pendingTimer.reset();
            }
        }
        push(ev);
    }

    void touchStrokeEnd()
    {
        if (!sTouch.stroking) return;
        sTouch.stroking = false;
        const bool painted = !sTouch.touched.empty();
        sTouch.touched.clear();
        if (weightBrush(sTouch.brush) && !painted) return;
        if (sTouch.brush == TB_PROTECT)
        {
            saveMask(sTouch.maskPath, sTouch.mask);
            push(LLSD().with("type", "lab_touch_saved").with("ok", true).with("mask", true)
                .with("undo", (S32)sTouch.undo.size()).with("redo", (S32)sTouch.redo.size()));
            return;
        }
        if (!weightBrush(sTouch.brush) && sTouch.pos == sTouch.before.pos) return;   // nothing moved
        sTouch.undo.push_back(sTouch.before);
        if (sTouch.undo.size() > 20) sTouch.undo.erase(sTouch.undo.begin());
        sTouch.redo.clear();
        touchSave();
    }

    // the bone drawn under the mouse (within 7 px of its line or its joint), "" if none
    std::string boneAtScreen(S32 x, S32 y)
    {
        std::string best;
        F32 bestD = 7.f * 7.f;
        for (const auto& b : sTouch.boneSegs)
        {
            const F32 dx = b.x1 - b.x0, dy = b.y1 - b.y0, l2 = dx * dx + dy * dy;
            F32 t = l2 > 1e-6f ? ((x - b.x0) * dx + (y - b.y0) * dy) / l2 : 1.f;
            t = llclamp(t, 0.f, 1.f);
            const F32 px = b.x0 + dx * t - x, py = b.y0 + dy * t - y;
            F32 d = px * px + py * py;
            if (!b.mine) d += 4.f;   // the garment's own bones win a tie
            if (d < bestD) { bestD = d; best = b.name; }
        }
        return best;
    }

    // the skeleton as the avatar stands now: the garment's bones and volumes, and the body's main bones
    void drawBones()
    {
        sTouch.boneSegs.clear();
        if (!sTouch.showBones || !weightBrush(sTouch.brush) || !isAgentAvatarValid()) return;
        static const char* CORE[] = { "mPelvis", "mTorso", "mChest", "mNeck", "mHead", "mCollarLeft", "mCollarRight", "mShoulderLeft", "mShoulderRight",
            "mElbowLeft", "mElbowRight", "mWristLeft", "mWristRight", "mHipLeft", "mHipRight", "mKneeLeft", "mKneeRight", "mAnkleLeft", "mAnkleRight" };
        std::set<std::string> mine;
        for (const auto& sk : sTouch.skin) for (const auto& j : sk.joints) mine.insert(j);
        std::set<std::string> names(mine.begin(), mine.end());
        for (const char* c : CORE) names.insert(c);
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        const std::string& sel = sTouch.brush == TB_STIFF && !sTouch.stiffBone.empty() ? sTouch.stiffBone : sTouch.bone;
        for (const auto& n : names)
        {
            LLJoint* j = gAgentAvatarp->getJoint(n);
            if (!j) continue;
            LLJoint* p = j->getParent();
            const LLVector3 wj = j->getWorldPosition();
            const LLVector3 wp = p ? p->getWorldPosition() : wj;
            LLCoordGL a, b;
            if (!cam->projectPosAgentToScreen(wp, a, false) || !cam->projectPosAgentToScreen(wj, b, false)) continue;
            const bool volume = !n.empty() && n[0] != 'm';
            sTouch.boneSegs.push_back({ n, (F32)a.mX, (F32)a.mY, (F32)b.mX, (F32)b.mY, mine.count(n) > 0, volume });
        }
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        // dim first, then the garment's, then hovered and chosen on top
        for (int pass = 0; pass < 3; ++pass)
            for (const auto& b : sTouch.boneSegs)
            {
                const bool chosen = b.name == sel, hover = b.name == sTouch.hoverBone;
                const int want = chosen || hover ? 2 : b.mine ? 1 : 0;
                if (want != pass) continue;
                if (chosen) gGL.color4f(0.f, 0.83f, 1.f, 1.f);
                else if (hover) gGL.color4f(1.f, 1.f, 1.f, 1.f);
                else if (b.mine) gGL.color4f(0.9f, 0.92f, 0.95f, 0.75f);
                else gGL.color4f(0.6f, 0.62f, 0.66f, 0.35f);
                const F32 w = chosen ? 1.5f : hover ? 1.f : 0.f;
                for (F32 o = -w; o <= w + 0.01f; o += 1.f)
                {
                    gl_line_2d((S32)(b.x0 + o), (S32)b.y0, (S32)(b.x1 + o), (S32)b.y1);
                    gl_line_2d((S32)b.x0, (S32)(b.y0 + o), (S32)b.x1, (S32)(b.y1 + o));
                }
                gl_circle_2d(b.x1, b.y1, b.volume ? (chosen ? 5.f : 3.5f) : (chosen ? 4.f : 2.5f), b.volume ? 4 : 10, true);
            }
        // names: the chosen bone, and the one under the mouse
        const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
        for (const auto& b : sTouch.boneSegs)
        {
            const bool chosen = b.name == sel, hover = b.name == sTouch.hoverBone;
            if (!chosen && !hover) continue;
            std::string label = b.name;
            if (hover && !chosen) label += "   (Ctrl+click)";
            font->renderUTF8(label, 0, b.x1 + 8.f, b.y1 + 4.f, chosen ? LLColor4(0.f, 0.83f, 1.f, 1.f) : LLColor4::white,
                             LLFontGL::LEFT, LLFontGL::BASELINE, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
        }
    }

    // ------------------------------------------------------------------
    // <MG> Edit Mesh (Touch up's session, brush TB_SELECT), like Character Creator's: the garment's
    // points, edges and faces over the worn garment; select them (point, edge, face, piece: click,
    // Shift adds, Ctrl removes, drag a box), then move, turn or resize them with a gizmo, with a
    // soft falloff and the left / right mirror. Each drag is one undo step and writes the _fit
    // copy, like a brush stroke. A toolbar is drawn in the 3D view (letters would walk the avatar).
    // ------------------------------------------------------------------
    void editApplyHidden();   // (below, with the live preview: hidden faces are left out of the worn garment)
    void editPushState();
    void drapeBody(std::vector<float>& P, std::vector<std::array<int, 3>>& T, const std::set<LLUUID>& garment, std::vector<std::string>* names);

    void editEnsure()
    {
        const size_t nm = sTouch.world.size();
        if (sTouch.sel.size() != nm) sTouch.sel.assign(nm, {});
        if (sTouch.hidden.size() != nm) sTouch.hidden.assign(nm, {});
        for (size_t mi = 0; mi < nm; ++mi)
        {
            const size_t n = sTouch.world[mi].size();
            if (sTouch.sel[mi].size() != n) sTouch.sel[mi].assign(n, 0);
            if (sTouch.hidden[mi].size() != n) sTouch.hidden[mi].assign(n, 0);
        }
        if (sTouch.editOut == 0.f)
        {
            // the triangles' winding: their normals away from the garment's middle, or towards it
            LLVector3 c; size_t cn = 0;
            for (const auto& W : sTouch.world) for (const auto& p : W) { c += p; ++cn; }
            if (cn) c *= 1.f / (F32)cn;
            F64 s = 0;
            for (size_t mi = 0; mi < sTouch.world.size() && mi < sTouch.wnorm.size(); ++mi)
                for (size_t v = 0; v < sTouch.world[mi].size() && v < sTouch.wnorm[mi].size(); ++v) s += sTouch.wnorm[mi][v] * (sTouch.world[mi][v] - c);
            sTouch.editOut = s < 0 ? -1.f : 1.f;
        }
    }
    bool editFacing(size_t mi, size_t v, const LLVector3& eye)
    {
        if (sTouch.xray) return true;
        if (mi >= sTouch.wnorm.size() || v >= sTouch.wnorm[mi].size()) return true;
        const LLVector3& n = sTouch.wnorm[mi][v];
        if (n.lengthSquared() < 1e-12f) return true;
        return (n * (eye - sTouch.world[mi][v])) * sTouch.editOut >= 0.f;
    }
    S32 editCount()
    {
        S32 c = 0;
        for (const auto& s : sTouch.sel) for (U8 b : s) c += b;
        return c;
    }
    void editClear() { for (auto& s : sTouch.sel) std::fill(s.begin(), s.end(), (U8)0); }

    // the mouse ray against the garment's visible triangles: mesh, triangle, hit point
    bool editPick(S32 x, S32 y, size_t& mo, S32& to, LLVector3& hit)
    {
        const LLVector3 o = LLViewerCamera::getInstance()->getOrigin();
        LLVector3 d = gViewerWindow->mouseDirectionGlobal(x, y);
        d.normVec();
        F32 bestT = 1e9f;
        bool found = false;
        for (size_t mi = 0; mi < sTouch.world.size(); ++mi)
        {
            const auto& W = sTouch.world[mi];
            const auto& H = sTouch.hidden[mi];
            const auto& tris = sTouch.dae.meshes[mi].triangles;
            for (size_t ti = 0; ti < tris.size(); ++ti)
            {
                const auto& tr = tris[ti];
                if (tr[0] < 0 || tr[1] < 0 || tr[2] < 0 || tr[0] >= (S32)W.size() || tr[1] >= (S32)W.size() || tr[2] >= (S32)W.size()) continue;
                if (H[tr[0]] && H[tr[1]] && H[tr[2]]) continue;
                Tri t{ W[tr[0]], W[tr[1]], W[tr[2]] };
                F32 dist;
                if (!rayTri(o, d, t, dist) || dist <= 0.f || dist >= bestT) continue;
                bestT = dist; mo = mi; to = (S32)ti; found = true;
            }
        }
        if (found) hit = o + d * bestT;
        return found;
    }

    // replace (plain click), add (Shift) or take away (Ctrl)
    void editApplyOp(const std::vector<std::pair<size_t, S32>>& pts, MASK mask)
    {
        const bool add = (mask & MASK_SHIFT) != 0, sub = (mask & MASK_CONTROL) != 0;
        if (!add && !sub) editClear();
        for (const auto& p : pts)
        {
            if (p.first >= sTouch.sel.size() || p.second < 0 || p.second >= (S32)sTouch.sel[p.first].size()) continue;
            if (sTouch.hidden[p.first][p.second]) continue;
            sTouch.sel[p.first][p.second] = sub ? 0 : 1;
        }
    }

    // the edge loop through a - b: at each end, the neighbour that goes on straightest (within 35°);
    // on a triangulated quad mesh the diagonals are 45° off, so the loop follows the quads' rows
    std::vector<S32> editLoop(size_t mi, S32 a, S32 b)
    {
        std::vector<S32> out{ a, b };
        if (mi >= sTouch.world.size()) return out;
        const auto& W = sTouch.world[mi];
        const auto& nb = sTouch.nb[mi];
        std::set<S32> seen{ a, b };
        auto walk = [&](S32 prev, S32 cur)
        {
            for (int step = 0; step < 4000; ++step)
            {
                LLVector3 dir = W[cur] - W[prev];
                if (dir.normVec() < 1e-9f) return;
                S32 best = -1;
                F32 bestDot = cosf(35.f * DEG_TO_RAD);
                for (S32 u : nb[cur])
                {
                    if (u == prev) continue;
                    LLVector3 e = W[u] - W[cur];
                    if (e.normVec() < 1e-9f) continue;
                    const F32 dd = dir * e;
                    if (dd > bestDot) { bestDot = dd; best = u; }
                }
                if (best < 0 || seen.count(best)) return;
                seen.insert(best);
                out.push_back(best);
                prev = cur; cur = best;
            }
        };
        walk(a, b);
        walk(b, a);
        return out;
    }

    void editClick(S32 x, S32 y, MASK mask)
    {
        size_t mi = 0; S32 ti = -1; LLVector3 hit;
        if (!editPick(x, y, mi, ti, hit))
        {
            if (!(mask & (MASK_SHIFT | MASK_CONTROL))) editClear();   // a click on nothing: none selected
            return;
        }
        const auto& tr = sTouch.dae.meshes[mi].triangles[ti];
        const auto& W = sTouch.world[mi];
        std::vector<std::pair<size_t, S32>> pts;
        if (sTouch.selMode == 0)
        {
            S32 best = tr[0];
            for (int k = 1; k < 3; ++k) if ((W[tr[k]] - hit).lengthSquared() < (W[best] - hit).lengthSquared()) best = tr[k];
            pts.push_back({ mi, best });
        }
        else if (sTouch.selMode == 1)
        {
            // the triangle's edge nearest the hit
            F32 bd = 1e9f; S32 ea = tr[0], eb = tr[1];
            for (int k = 0; k < 3; ++k)
            {
                const LLVector3 a = W[tr[k]], b = W[tr[(k + 1) % 3]];
                LLVector3 ab = b - a;
                F32 t = ab.lengthSquared() > 1e-12f ? ((hit - a) * ab) / ab.lengthSquared() : 0.f;
                t = llclamp(t, 0.f, 1.f);
                const F32 dd = (a + ab * t - hit).lengthSquared();
                if (dd < bd) { bd = dd; ea = tr[k]; eb = tr[(k + 1) % 3]; }
            }
            pts.push_back({ mi, ea }); pts.push_back({ mi, eb });
            sTouch.lastMesh = mi; sTouch.lastA = ea; sTouch.lastB = eb;
        }
        else if (sTouch.selMode == 2)
        {
            for (int k = 0; k < 3; ++k) pts.push_back({ mi, tr[k] });
        }
        else
        {
            // the piece: everything joined to this triangle
            std::vector<U8> seen(W.size(), 0);
            std::vector<S32> todo{ tr[0] };
            seen[tr[0]] = 1;
            while (!todo.empty())
            {
                const S32 v = todo.back(); todo.pop_back();
                pts.push_back({ mi, v });
                for (S32 u : sTouch.nb[mi][v]) if (!seen[u]) { seen[u] = 1; todo.push_back(u); }
            }
        }
        editApplyOp(pts, mask);
    }

    void editBox(S32 x0, S32 y0, S32 x1, S32 y1, MASK mask)
    {
        const S32 lx = llmin(x0, x1), hx = llmax(x0, x1), ly = llmin(y0, y1), hy = llmax(y0, y1);
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        const LLVector3 eye = cam->getOrigin();
        std::vector<std::pair<size_t, S32>> pts;
        for (size_t mi = 0; mi < sTouch.world.size(); ++mi)
            for (size_t v = 0; v < sTouch.world[mi].size(); ++v)
            {
                if (sTouch.hidden[mi][v] || !editFacing(mi, v, eye)) continue;
                LLCoordGL c;
                if (!cam->projectPosAgentToScreen(sTouch.world[mi][v], c, false)) continue;
                if (c.mX >= lx && c.mX <= hx && c.mY >= ly && c.mY <= hy) pts.push_back({ mi, (S32)v });
            }
        editApplyOp(pts, mask);
    }

    void editGrow(bool grow)
    {
        for (size_t mi = 0; mi < sTouch.sel.size(); ++mi)
        {
            const auto src = sTouch.sel[mi];
            for (size_t v = 0; v < src.size(); ++v)
            {
                if (grow) { if (src[v]) for (S32 u : sTouch.nb[mi][v]) if (!sTouch.hidden[mi][u]) sTouch.sel[mi][u] = 1; }
                else if (src[v]) { for (S32 u : sTouch.nb[mi][v]) if (!src[u]) { sTouch.sel[mi][v] = 0; break; } }
            }
        }
    }

    // the garment's points under the skin of the worn body (as it stands now)
    S32 editUnderSkin(MASK mask)
    {
        std::vector<float> P;
        std::vector<std::array<int, 3>> T;
        std::set<LLUUID> garment;
        for (const auto& a : sApplied) garment.insert(a.prim);
        drapeBody(P, T, garment, nullptr);
        if (T.empty()) return -1;
        // the body's triangles by cell (2 cm)
        Grid g; g.cell = 0.02f;
        std::unordered_map<uint64_t, std::vector<S32>> cells;
        for (size_t ti = 0; ti < T.size(); ++ti)
        {
            const float* a = &P[T[ti][0] * 3];
            cells[Grid::key(g.c(a[0]), g.c(a[1]), g.c(a[2]))].push_back((S32)ti);
        }
        std::vector<std::pair<size_t, S32>> pts;
        for (size_t mi = 0; mi < sTouch.world.size(); ++mi)
            for (size_t v = 0; v < sTouch.world[mi].size(); ++v)
            {
                if (sTouch.hidden[mi][v]) continue;
                const LLVector3& p = sTouch.world[mi][v];
                F32 best = 0.03f * 0.03f, side = 1.f;
                const S32 cx = g.c(p.mV[0]), cy = g.c(p.mV[1]), cz = g.c(p.mV[2]);
                for (S32 xx = cx - 1; xx <= cx + 1; ++xx) for (S32 yy = cy - 1; yy <= cy + 1; ++yy) for (S32 zz = cz - 1; zz <= cz + 1; ++zz)
                {
                    auto it = cells.find(Grid::key(xx, yy, zz));
                    if (it == cells.end()) continue;
                    for (S32 ti : it->second)
                    {
                        const LLVector3 a(&P[T[ti][0] * 3]), b(&P[T[ti][1] * 3]), c(&P[T[ti][2] * 3]);
                        // nearest point on the triangle (barycentric, clamped)
                        const LLVector3 v0 = b - a, v1 = c - a, v2 = p - a;
                        const F32 d00 = v0 * v0, d01 = v0 * v1, d11 = v1 * v1, d20 = v2 * v0, d21 = v2 * v1, den = d00 * d11 - d01 * d01;
                        if (std::fabs(den) < 1e-20f) continue;
                        F32 bv = (d11 * d20 - d01 * d21) / den, bw = (d00 * d21 - d01 * d20) / den;
                        bv = llclamp(bv, 0.f, 1.f); bw = llclamp(bw, 0.f, 1.f - bv);
                        const LLVector3 q = a + v0 * bv + v1 * bw;
                        const F32 dd = (p - q).lengthSquared();
                        if (dd >= best) continue;
                        best = dd;
                        LLVector3 n = v0 % v1; n.normVec();
                        side = (p - q) * n;
                    }
                }
                if (best < 0.03f * 0.03f && side < -0.001f) pts.push_back({ mi, (S32)v });
            }
        editApplyOp(pts, mask);
        return (S32)pts.size();
    }

    // each point moved by an edit and how much: the selection 1, the soft falloff around it less
    void editWeights(std::vector<std::vector<F32>>& wt)
    {
        wt.assign(sTouch.world.size(), {});
        for (size_t mi = 0; mi < sTouch.world.size(); ++mi) wt[mi].assign(sTouch.world[mi].size(), 0.f);
        std::vector<LLVector3> selPts;
        for (size_t mi = 0; mi < sTouch.sel.size(); ++mi)
            for (size_t v = 0; v < sTouch.sel[mi].size(); ++v)
                if (sTouch.sel[mi][v] && !sTouch.mask[mi][v]) { wt[mi][v] = 1.f; selPts.push_back(sTouch.world[mi][v]); }
        if (!sTouch.softOn || sTouch.softR <= 0.f || selPts.empty()) return;
        const F32 r = sTouch.softR;
        Grid g; g.cell = r;
        std::unordered_map<uint64_t, std::vector<S32>> cells;
        for (size_t i = 0; i < selPts.size(); ++i) cells[Grid::key(g.c(selPts[i].mV[0]), g.c(selPts[i].mV[1]), g.c(selPts[i].mV[2]))].push_back((S32)i);
        for (size_t mi = 0; mi < sTouch.world.size(); ++mi)
            for (size_t v = 0; v < sTouch.world[mi].size(); ++v)
            {
                if (wt[mi][v] > 0.f || sTouch.mask[mi][v] || sTouch.hidden[mi][v]) continue;
                const LLVector3& p = sTouch.world[mi][v];
                F32 best = r * r;
                const S32 cx = g.c(p.mV[0]), cy = g.c(p.mV[1]), cz = g.c(p.mV[2]);
                for (S32 xx = cx - 1; xx <= cx + 1; ++xx) for (S32 yy = cy - 1; yy <= cy + 1; ++yy) for (S32 zz = cz - 1; zz <= cz + 1; ++zz)
                {
                    auto it = cells.find(Grid::key(xx, yy, zz));
                    if (it == cells.end()) continue;
                    for (S32 i : it->second) best = llmin(best, (selPts[i] - p).lengthSquared());
                }
                if (best < r * r)
                {
                    // smooth: 1 at the selection .. 0 at the radius
                    const F32 t = 1.f - std::sqrt(best) / r;
                    wt[mi][v] = t * t * (3.f - 2.f * t);
                }
            }
    }

    // with the mirror on, a point whose partner moves too is moved by the partner (mirrored)
    bool editMirrorSkip(size_t mi, size_t v, const std::vector<std::vector<F32>>& wt)
    {
        if (!sTouch.symmetry) return false;
        const S32 p = sTouch.mirror[mi][v];
        if (p < 0 || p == (S32)v || wt[mi][p] <= 0.f) return false;
        return sTouch.pos[mi][v * 3 + 1] < 0.0;   // the right side (-Y) follows the left
    }

    // ---- the gizmo (as placing: arrows, rings, cubes), at the selection's middle
    struct EditDrag
    {
        bool down = false, box = false, dragging = false;
        S32 x0 = 0, y0 = 0, x = 0, y = 0;
        MASK mask = 0;
        S32 part = 0, hover = 0;
        bool gizmo = false;
        LLVector3 O, ax[3];
        F32 len = 0.2f;
        LLCoordGL o2, tip2[3];
        std::vector<LLCoordGL> ring2[3];
        std::vector<std::vector<F32>> wt;
        std::vector<std::vector<LLVector3>> world0;
        std::vector<std::vector<double>> base;
        LLVector3 anchor, planeN;
        LLFrameTimer timer;
        S32 tbHover = -1;
    } sEdit;

    bool editCentre(LLVector3& O)
    {
        LLVector3 c; size_t n = 0;
        for (size_t mi = 0; mi < sTouch.sel.size(); ++mi)
            for (size_t v = 0; v < sTouch.sel[mi].size(); ++v)
                if (sTouch.sel[mi][v]) { c += sTouch.world[mi][v]; ++n; }
        if (!n) return false;
        O = c * (1.f / (F32)n);
        return true;
    }

    void editBuildGizmo()
    {
        sEdit.gizmo = false;
        if (sEdit.dragging) { /* keep the centre where the drag began */ }
        else if (!editCentre(sEdit.O)) return;
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        if (!cam->projectPosAgentToScreen(sEdit.O, sEdit.o2, false)) return;
        LLQuaternion R;
        if (isAgentAvatarValid()) { LLJoint* pel = gAgentAvatarp->getJoint("mPelvis"); R = pel ? pel->getWorldRotation() : gAgentAvatarp->getRotation(); }
        sEdit.ax[0] = LLVector3(1, 0, 0) * R; sEdit.ax[1] = LLVector3(0, 1, 0) * R; sEdit.ax[2] = LLVector3(0, 0, 1) * R;
        sEdit.len = llclamp((cam->getOrigin() - sEdit.O).length() * 0.14f, 0.03f, 0.6f);   // about the same size on screen
        const F32 len = sTouch.gizmo == 2 ? sEdit.len * 0.85f : sEdit.len;
        for (int k = 0; k < 3; ++k) if (!cam->projectPosAgentToScreen(sEdit.O + sEdit.ax[k] * len, sEdit.tip2[k], false)) return;
        for (int k = 0; k < 3; ++k)
        {
            sEdit.ring2[k].clear();
            if (sTouch.gizmo != 1) continue;
            const LLVector3& u = sEdit.ax[(k + 1) % 3];
            const LLVector3& w = sEdit.ax[(k + 2) % 3];
            for (int i = 0; i <= 64; ++i)
            {
                const F32 a = (F32)i / 64.f * F_TWO_PI;
                LLCoordGL c;
                if (cam->projectPosAgentToScreen(sEdit.O + (u * cosf(a) + w * sinf(a)) * sEdit.len, c, false)) sEdit.ring2[k].push_back(c);
            }
        }
        sEdit.gizmo = true;
    }
    F32 segDist2(S32 x, S32 y, const LLCoordGL& a, const LLCoordGL& b)
    {
        const F32 dx = (F32)(b.mX - a.mX), dy = (F32)(b.mY - a.mY), l2 = dx * dx + dy * dy;
        F32 t = l2 > 1e-6f ? (((F32)(x - a.mX)) * dx + ((F32)(y - a.mY)) * dy) / l2 : 0.f;
        t = llclamp(t, 0.f, 1.f);
        const F32 px = a.mX + dx * t - x, py = a.mY + dy * t - y;
        return px * px + py * py;
    }
    S32 editPartAt(S32 x, S32 y)
    {
        if (!sEdit.gizmo) return 0;
        auto near2 = [&](const LLCoordGL& c, F32 r) { const F32 dx = (F32)(c.mX - x), dy = (F32)(c.mY - y); return dx * dx + dy * dy <= r * r; };
        if (sTouch.gizmo != 1 && near2(sEdit.o2, 10.f)) return 4;
        if (sTouch.gizmo == 1)
        {
            S32 best = 0; F32 bd = 49.f;
            for (int k = 0; k < 3; ++k)
                for (size_t i = 1; i < sEdit.ring2[k].size(); ++i) { const F32 d = segDist2(x, y, sEdit.ring2[k][i - 1], sEdit.ring2[k][i]); if (d < bd) { bd = d; best = k + 1; } }
            return best;
        }
        for (int k = 0; k < 3; ++k) if (near2(sEdit.tip2[k], 10.f) || segDist2(x, y, sEdit.o2, sEdit.tip2[k]) <= 36.f) return k + 1;
        return 0;
    }
    bool editPlaneHit(S32 x, S32 y, LLVector3& out)
    {
        const LLVector3 o = LLViewerCamera::getInstance()->getOrigin();
        LLVector3 d = gViewerWindow->mouseDirectionGlobal(x, y);
        d.normVec();
        const F32 den = d * sEdit.planeN;
        if (std::fabs(den) < 1e-6f) return false;
        const F32 t = ((sEdit.anchor - o) * sEdit.planeN) / den;
        if (t <= 0.f) return false;
        out = o + d * t;
        return true;
    }

    void editBegin()
    {
        sEdit.dragging = true;
        sTouch.before.pos = sTouch.pos;
        sTouch.before.skin = sTouch.skin;
        editWeights(sEdit.wt);
        sEdit.world0 = sTouch.world;
        sEdit.base = sTouch.pos;
        sEdit.anchor = sEdit.O;
        sEdit.planeN = -LLViewerCamera::getInstance()->getAtAxis();
    }

    // the drag so far as one world transform, applied to each point by its weight
    void editDragTo(S32 x, S32 y)
    {
        glm::mat4 M(1.f);
        const glm::vec3 O(sEdit.O.mV[0], sEdit.O.mV[1], sEdit.O.mV[2]);
        auto along = [&](int k, F32& lenPx)
        {
            const F32 sx = (F32)(sEdit.tip2[k].mX - sEdit.o2.mX), sy = (F32)(sEdit.tip2[k].mY - sEdit.o2.mY);
            lenPx = sqrtf(sx * sx + sy * sy);
            return lenPx > 4.f ? (((F32)(x - sEdit.x0)) * sx + ((F32)(y - sEdit.y0)) * sy) / lenPx : 0.f;
        };
        const S32 part = sEdit.part;
        if (sTouch.gizmo == 0)
        {
            LLVector3 dw;
            if (part >= 1 && part <= 3)
            {
                F32 lp; const F32 a = along(part - 1, lp);
                if (lp > 4.f) dw = sEdit.ax[part - 1] * (a / lp * sEdit.len);
            }
            else
            {
                LLVector3 a0, a1;
                if (editPlaneHit(sEdit.x0, sEdit.y0, a0) && editPlaneHit(x, y, a1)) dw = a1 - a0;
            }
            M = glm::translate(glm::mat4(1.f), glm::vec3(dw.mV[0], dw.mV[1], dw.mV[2]));
        }
        else if (sTouch.gizmo == 1 && part >= 1 && part <= 3)
        {
            const F32 a0 = atan2f((F32)(sEdit.y0 - sEdit.o2.mY), (F32)(sEdit.x0 - sEdit.o2.mX)), a1 = atan2f((F32)(y - sEdit.o2.mY), (F32)(x - sEdit.o2.mX));
            F32 d = a1 - a0;
            while (d > F_PI) d -= F_TWO_PI;
            while (d < -F_PI) d += F_TWO_PI;
            const LLVector3& axv = sEdit.ax[part - 1];
            if ((LLViewerCamera::getInstance()->getAtAxis() * axv) > 0.f) d = -d;   // the ring seen from its back turns the other way
            M = glm::translate(glm::mat4(1.f), O) * glm::rotate(glm::mat4(1.f), d, glm::vec3(axv.mV[0], axv.mV[1], axv.mV[2])) * glm::translate(glm::mat4(1.f), -O);
        }
        else if (sTouch.gizmo == 2)
        {
            if (part >= 1 && part <= 3)
            {
                F32 lp; const F32 a = along(part - 1, lp);
                const F32 f = lp > 4.f ? llclamp(1.f + a / lp, 0.05f, 20.f) : 1.f;
                const LLVector3& axv = sEdit.ax[part - 1];
                const glm::vec3 u(axv.mV[0], axv.mV[1], axv.mV[2]);
                glm::mat3 S(1.f);
                S += (f - 1.f) * glm::outerProduct(u, u);
                M = glm::translate(glm::mat4(1.f), O) * glm::mat4(S) * glm::translate(glm::mat4(1.f), -O);
            }
            else
            {
                const F32 f = llclamp(powf(2.f, ((F32)(x - sEdit.x0) + (F32)(y - sEdit.y0)) / 200.f), 0.05f, 20.f);
                M = glm::translate(glm::mat4(1.f), O) * glm::scale(glm::mat4(1.f), glm::vec3(f)) * glm::translate(glm::mat4(1.f), -O);
            }
        }
        sTouch.pos = sEdit.base;
        for (size_t mi = 0; mi < sEdit.wt.size(); ++mi)
            for (size_t v = 0; v < sEdit.wt[mi].size(); ++v)
            {
                const F32 w = sEdit.wt[mi][v];
                if (w <= 0.f || editMirrorSkip(mi, v, sEdit.wt)) continue;
                const LLVector3& p0 = sEdit.world0[mi][v];
                const glm::vec4 q = M * glm::vec4(p0.mV[0], p0.mV[1], p0.mV[2], 1.f);
                const LLVector3 dw = (LLVector3(q.x, q.y, q.z) - p0) * w;
                LLVector3 df;
                if (!worldToFile(mi, v, dw, df)) continue;
                moveFile(mi, v, df);
            }
        ++sTouch.edits;
        touchSkin();
        sTouch.liveDirty = true;
    }

    // one change done: an undo step, the _fit copy written
    void editCommit()
    {
        if (sTouch.pos == sTouch.before.pos && !sTouch.skinEdited) return;
        sTouch.undo.push_back(sTouch.before);
        if (sTouch.undo.size() > 20) sTouch.undo.erase(sTouch.undo.begin());
        sTouch.redo.clear();
        touchSave();
    }
    void editSnapshot() { sTouch.before.pos = sTouch.pos; sTouch.before.skin = sTouch.skin; }

    // push out (+) / pull in (-) along the surface, mm
    void editPush(F32 mm)
    {
        editSnapshot();
        std::vector<std::vector<F32>> wt;
        editWeights(wt);
        for (size_t mi = 0; mi < wt.size(); ++mi)
            for (size_t v = 0; v < wt[mi].size(); ++v)
            {
                if (wt[mi][v] <= 0.f || editMirrorSkip(mi, v, wt)) continue;
                const LLVector3 dw = sTouch.wnorm[mi][v] * (sTouch.editOut * mm / 1000.f * wt[mi][v]);
                LLVector3 df;
                if (worldToFile(mi, v, dw, df)) moveFile(mi, v, df);
            }
        touchSkin();
        sTouch.liveDirty = true;
        editCommit();
    }

    // smooth: each point towards its neighbours' middle, a few passes
    void editSmooth(F32 k)
    {
        editSnapshot();
        std::vector<std::vector<F32>> wt;
        editWeights(wt);
        for (int pass = 0; pass < 3; ++pass)
            for (size_t mi = 0; mi < wt.size(); ++mi)
            {
                const std::vector<double> src = sTouch.pos[mi];
                for (size_t v = 0; v < wt[mi].size(); ++v)
                {
                    const auto& nbs = sTouch.nb[mi][v];
                    if (wt[mi][v] <= 0.f || nbs.empty()) continue;
                    double a[3] = { 0, 0, 0 };
                    for (S32 u : nbs) for (int q = 0; q < 3; ++q) a[q] += src[u * 3 + q];
                    for (int q = 0; q < 3; ++q) sTouch.pos[mi][v * 3 + q] += (a[q] * (1.0 / nbs.size()) - src[v * 3 + q]) * k * wt[mi][v];
                }
            }
        touchSkin();
        sTouch.liveDirty = true;
        editCommit();
    }

    // the selected points 100 % to one bone (Character Creator's quick set)
    S32 editSetWeights(const std::string& bone)
    {
        editSnapshot();
        S32 n = 0;
        for (size_t mi = 0; mi < sTouch.sel.size(); ++mi)
        {
            bool any = false;
            for (size_t v = 0; v < sTouch.sel[mi].size(); ++v) if (sTouch.sel[mi][v] && !sTouch.mask[mi][v]) { any = true; break; }
            if (!any) continue;
            const S32 j = jointFor(mi, bone);
            if (j < 0) continue;
            for (size_t v = 0; v < sTouch.sel[mi].size() && v < sTouch.skin[mi].weights.size(); ++v)
            {
                if (!sTouch.sel[mi][v] || sTouch.mask[mi][v]) continue;
                sTouch.skin[mi].weights[v].assign(1, { j, 1.0 });
                ++n;
            }
            repack(mi);
            sTouch.skinEdited = true;
        }
        if (!n) return 0;
        touchSkin();
        sTouch.liveDirty = true;
        sWV.dirty = true;
        editCommit();
        return n;
    }

    // ---- the toolbar in the 3D view (top middle): selection mode, gizmo, soft, mirror, x-ray, hide
    struct EditButton { const char* id; const char* tip; };
    const EditButton EDIT_TB[] = {
        { "point", "Points" }, { "edge", "Edges (Loop: on the phone)" }, { "face", "Faces" }, { "piece", "Pieces" },
        { "move", "Move" }, { "rotate", "Rotate" }, { "scale", "Scale" },
        { "soft", "Soft selection" }, { "mirror", "Mirror left / right" }, { "xray", "X-ray: see and select through" },
        { "hide", "Hide the selection" }, { "unhide", "Show everything" } };
    const S32 EDIT_TB_N = 12;
    LLRect editTbRect(S32 i)
    {
        const LLRect r = gViewerWindow->getWorldViewRectScaled();
        const S32 bw = 30, gap = 4, sep = 10;
        S32 total = EDIT_TB_N * (bw + gap) + 3 * sep;
        S32 x = r.getCenterX() - total / 2;
        for (S32 k = 0; k < i; ++k) x += bw + gap + ((k == 3 || k == 6 || k == 9) ? sep : 0);
        const S32 top = r.mTop - 70;
        return LLRect(x, top, x + bw, top - bw);
    }
    bool editTbOn(S32 i)
    {
        if (i <= 3) return sTouch.selMode == i;
        if (i <= 6) return sTouch.gizmo == i - 4;
        if (i == 7) return sTouch.softOn;
        if (i == 8) return sTouch.symmetry;
        if (i == 9) return sTouch.xray;
        return false;
    }
    void editUiPush()
    {
        push(LLSD().with("type", "lab_edit_ui").with("mode", sTouch.selMode).with("gizmo", sTouch.gizmo).with("soft", sTouch.softOn)
            .with("soft_mm", sTouch.softR * 1000.f).with("mirror", sTouch.symmetry).with("xray", sTouch.xray));
    }
    void editHide(bool hideSel);
    void editTbClick(S32 i)
    {
        if (i <= 3) sTouch.selMode = i;
        else if (i <= 6) sTouch.gizmo = i - 4;
        else if (i == 7) sTouch.softOn = !sTouch.softOn;
        else if (i == 8) sTouch.symmetry = !sTouch.symmetry;
        else if (i == 9) sTouch.xray = !sTouch.xray;
        else if (i == 10) editHide(true);
        else editHide(false);
        editUiPush();
        editPushState();
    }
    S32 editTbAt(S32 x, S32 y)
    {
        for (S32 i = 0; i < EDIT_TB_N; ++i) if (editTbRect(i).pointInRect(x, y)) return i;
        return -1;
    }
    void editIcon(const char* id, S32 icx, S32 icy, const LLColor4& col)
    {
        gGL.color4fv(col.mV);
        // all in floats; the 2D calls take whole pixels
        const F32 cx = (F32)icx, cy = (F32)icy, s = 8.f;
        auto line = [](F32 x0, F32 y0, F32 x1, F32 y1) { gl_line_2d((S32)x0, (S32)y0, (S32)x1, (S32)y1); };
        auto rect = [](F32 l, F32 t, F32 r, F32 b) { gl_rect_2d((S32)l, (S32)t, (S32)r, (S32)b, false); };
        auto trif = [&](F32 x1, F32 y1, F32 x2, F32 y2, F32 x3, F32 y3, bool fill) { gl_triangle_2d((S32)x1, (S32)y1, (S32)x2, (S32)y2, (S32)x3, (S32)y3, col, fill); };
        auto circ = [&](F32 r, S32 steps, bool fill) { gl_circle_2d(cx, cy, r, steps, fill); };
        auto eye = [&]() { line(cx - s, cy, cx - 3.f, cy + 4.f); line(cx - 3.f, cy + 4.f, cx + 3.f, cy + 4.f); line(cx + 3.f, cy + 4.f, cx + s, cy);
                           line(cx - s, cy, cx - 3.f, cy - 4.f); line(cx - 3.f, cy - 4.f, cx + 3.f, cy - 4.f); line(cx + 3.f, cy - 4.f, cx + s, cy); circ(3.f, 10, true); };
        const std::string n(id);
        auto tri = [&](F32 ox, F32 oy, F32 k, bool fill)
        {
            const F32 ax = cx + ox, ay = cy + oy + s * k, bx = cx + ox - s * k, by = cy + oy - s * 0.8f * k, qx = cx + ox + s * k, qy = by;
            if (fill) trif(ax, ay, bx, by, qx, qy, true);
            line(ax, ay, bx, by); line(bx, by, qx, qy); line(qx, qy, ax, ay);
        };
        if (n == "point") { tri(0.f, 0.f, 1.f, false); gl_circle_2d(cx, cy + s, 3.f, 10, true); }
        else if (n == "edge") { tri(0.f, 0.f, 1.f, false); for (F32 o = -1.f; o <= 1.f; o += 1.f) line(cx - s + o, cy - s * 0.8f, cx + o, cy + s); }
        else if (n == "face") tri(0.f, 0.f, 1.f, true);
        else if (n == "piece") { tri(-4.f, 2.f, 0.7f, true); tri(4.f, -3.f, 0.7f, false); }
        else if (n == "move") { line(cx - s, cy, cx + s, cy); line(cx, cy - s, cx, cy + s); line(cx + s, cy, cx + s - 3.f, cy + 3.f); line(cx + s, cy, cx + s - 3.f, cy - 3.f); line(cx, cy + s, cx - 3.f, cy + s - 3.f); line(cx, cy + s, cx + 3.f, cy + s - 3.f); }
        else if (n == "rotate") { circ(s, 20, false); line(cx + s, cy, cx + s - 4.f, cy + 3.f); line(cx + s, cy, cx + s + 3.f, cy + 4.f); }
        else if (n == "scale") { rect(cx - s, cy + 1.f, cx + 1.f, cy - s); line(cx, cy, cx + s, cy + s); line(cx + s, cy + s, cx + s - 4.f, cy + s); line(cx + s, cy + s, cx + s, cy + s - 4.f); }
        else if (n == "soft") { circ(s, 16, false); circ(s * 0.5f, 12, false); circ(2.f, 8, true); }
        else if (n == "mirror") { for (F32 k = -8.f; k < 8.f; k += 4.f) line(cx, cy + k, cx, cy + k + 2.f); trif(cx - 3.f, cy + 5.f, cx - 3.f, cy - 5.f, cx - s, cy, true); trif(cx + 3.f, cy + 5.f, cx + 3.f, cy - 5.f, cx + s, cy, false); }
        else if (n == "xray") { rect(cx - s, cy + s, cx + s - 3.f, cy - s + 3.f); rect(cx - s + 3.f, cy + s - 3.f, cx + s, cy - s); }
        else if (n == "hide") { eye(); line(cx - s, cy - s, cx + s, cy + s); }
        else if (n == "unhide" || n == "eye") eye();
        // <MG> the workspace bars' icons
        else if (n == "pin") { line(cx - 4.f, cy + s, cx + 4.f, cy + s); line(cx - 4.f, cy + s, cx - 2.f, cy + 2.f); line(cx + 4.f, cy + s, cx + 2.f, cy + 2.f); line(cx - 6.f, cy + 1.f, cx + 6.f, cy + 1.f); line(cx, cy + 1.f, cx, cy - s); }
        else if (n == "hand") { for (F32 k = -4.5f; k <= 4.6f; k += 3.f) line(cx + k, cy - 2.f, cx + k, cy + (k > 3.f ? 3.f : 7.f)); rect(cx - 6.f, cy - 2.f, cx + 6.f, cy - s); }
        else if (n == "tighten") { line(cx - s, cy, cx - 2.f, cy); line(cx + 2.f, cy, cx + s, cy); line(cx - 5.f, cy + 4.f, cx - 2.f, cy); line(cx - 5.f, cy - 4.f, cx - 2.f, cy); line(cx + 5.f, cy + 4.f, cx + 2.f, cy); line(cx + 5.f, cy - 4.f, cx + 2.f, cy); }
        else if (n == "smooth") { line(cx - s, cy - 2.f, cx - 4.f, cy + 3.f); line(cx - 4.f, cy + 3.f, cx, cy - 2.f); line(cx, cy - 2.f, cx + 4.f, cy + 3.f); line(cx + 4.f, cy + 3.f, cx + s, cy - 2.f); line(cx - s, cy - 6.f, cx + s, cy - 6.f); }
        else if (n == "pause") { rect(cx - 5.f, cy + 6.f, cx - 2.f, cy - 6.f); rect(cx + 2.f, cy + 6.f, cx + 5.f, cy - 6.f); }
        else if (n == "play") trif(cx - 5.f, cy + 7.f, cx - 5.f, cy - 7.f, cx + 7.f, cy, true);
        else if (n == "reset") { circ(s - 1.f, 16, false); trif(cx + s - 4.f, cy + 3.f, cx + s + 2.f, cy + 3.f, cx + s - 1.f, cy - 2.f, true); }
        else if (n == "check") { line(cx - 6.f, cy, cx - 2.f, cy - 5.f); line(cx - 2.f, cy - 5.f, cx + 7.f, cy + 6.f); line(cx - 6.f, cy + 1.f, cx - 2.f, cy - 4.f); line(cx - 2.f, cy - 4.f, cx + 7.f, cy + 7.f); }
        else if (n == "cross") { line(cx - 6.f, cy - 6.f, cx + 6.f, cy + 6.f); line(cx - 6.f, cy + 6.f, cx + 6.f, cy - 6.f); }
        else if (n == "tpose" || n == "apose" || n == "letgo" || n == "bones")
        {
            // a little figure: arms out (T), down (A), free (let go); bones: its joints as dots
            gl_circle_2d(cx, cy + 6.f, 2.f, 8, true);
            line(cx, cy + 4.f, cx, cy - 3.f); line(cx, cy - 3.f, cx - 4.f, cy - s); line(cx, cy - 3.f, cx + 4.f, cy - s);
            if (n == "apose") { line(cx, cy + 2.f, cx - 6.f, cy - 3.f); line(cx, cy + 2.f, cx + 6.f, cy - 3.f); }
            else if (n == "letgo") { line(cx, cy + 2.f, cx - 6.f, cy + 5.f); line(cx, cy + 2.f, cx + 5.f, cy - 4.f); }
            else line(cx - s, cy + 2.f, cx + s, cy + 2.f);
            if (n == "bones") { gl_circle_2d(cx - s, cy + 2.f, 2.f, 6, true); gl_circle_2d(cx + s, cy + 2.f, 2.f, 6, true); gl_circle_2d(cx, cy - 3.f, 2.f, 6, true); }
        }
        else if (n == "white") circ(s - 1.f, 18, false);
        else if (n == "clay") { circ(s - 1.f, 18, true); }
        else if (n == "look") { circ(s - 1.f, 18, false); gl_circle_2d(cx - 3.f, cy + 3.f, 1.8f, 6, true); gl_circle_2d(cx + 3.f, cy + 3.f, 1.8f, 6, true); gl_circle_2d(cx - 4.f, cy - 2.f, 1.8f, 6, true); gl_circle_2d(cx + 2.f, cy - 4.f, 1.8f, 6, true); }
        else if (n == "flash") { line(cx + 2.f, cy + s, cx - 4.f, cy); line(cx - 4.f, cy, cx + 3.f, cy); line(cx + 3.f, cy, cx - 2.f, cy - s); }
        else if (n == "brush" || n == "paint") { line(cx + 6.f, cy + 7.f, cx - 1.f, cy); line(cx + 7.f, cy + 6.f, cx, cy - 1.f); gl_circle_2d(cx - 3.f, cy - 3.f, 3.5f, 10, true); }
        else if (n == "erase") { rect(cx - 6.f, cy + 2.f, cx + 6.f, cy - 4.f); line(cx - 2.f, cy + 2.f, cx - 2.f, cy - 4.f); }
        else if (n == "stiff") { line(cx, cy + s, cx, cy - s); line(cx - 1.f, cy + s, cx - 1.f, cy - s); line(cx - 5.f, cy + s, cx + 5.f, cy + s); }
        else if (n == "legs") { line(cx - 2.f, cy + s, cx - 5.f, cy - s); line(cx + 2.f, cy + s, cx + 5.f, cy - s); line(cx - 4.f, cy + s, cx + 4.f, cy + s); }
        else if (n == "inflate") { circ(4.f, 12, false); circ(s, 16, false); }
        else if (n == "protect") { line(cx - 6.f, cy + 5.f, cx, cy + s); line(cx, cy + s, cx + 6.f, cy + 5.f); line(cx - 6.f, cy + 5.f, cx - 5.f, cy - 2.f); line(cx + 6.f, cy + 5.f, cx + 5.f, cy - 2.f); line(cx - 5.f, cy - 2.f, cx, cy - s); line(cx + 5.f, cy - 2.f, cx, cy - s); }
        else if (n == "push") { line(cx, cy - 6.f, cx, cy + 6.f); line(cx, cy + 6.f, cx - 4.f, cy + 2.f); line(cx, cy + 6.f, cx + 4.f, cy + 2.f); line(cx - s, cy - 6.f, cx + s, cy - 6.f); }
        else if (n == "layers") { trif(cx, cy + 6.f, cx - s, cy + 2.f, cx + s, cy + 2.f, false); line(cx - s, cy - 1.f, cx, cy - 5.f); line(cx, cy - 5.f, cx + s, cy - 1.f); line(cx - s, cy - 4.f, cx, cy - s); line(cx, cy - s, cx + s, cy - 4.f); }
        else if (n == "upload") { line(cx, cy - 4.f, cx, cy + 7.f); line(cx, cy + 7.f, cx - 4.f, cy + 3.f); line(cx, cy + 7.f, cx + 4.f, cy + 3.f); line(cx - s, cy - 4.f, cx - s, cy - s); line(cx - s, cy - s, cx + s, cy - s); line(cx + s, cy - s, cx + s, cy - 4.f); }
        else if (n == "undo") { line(cx - 6.f, cy + 2.f, cx + 3.f, cy + 2.f); line(cx - 6.f, cy + 2.f, cx - 3.f, cy + 5.f); line(cx - 6.f, cy + 2.f, cx - 3.f, cy - 1.f); line(cx + 3.f, cy + 2.f, cx + 6.f, cy - 2.f); line(cx + 6.f, cy - 2.f, cx + 3.f, cy - 6.f); }
        else if (n == "redo") { line(cx + 6.f, cy + 2.f, cx - 3.f, cy + 2.f); line(cx + 6.f, cy + 2.f, cx + 3.f, cy + 5.f); line(cx + 6.f, cy + 2.f, cx + 3.f, cy - 1.f); line(cx - 3.f, cy + 2.f, cx - 6.f, cy - 2.f); line(cx - 6.f, cy - 2.f, cx - 3.f, cy - 6.f); }
        else if (n == "fit") { circ(s - 1.f, 16, false); circ(3.f, 8, false); }
        else if (n == "dots") { gl_circle_2d(cx - 4.f, cy, 2.f, 6, true); gl_circle_2d(cx + 4.f, cy, 2.f, 6, true); gl_circle_2d(cx, cy + 4.f, 2.f, 6, true); }
        else circ(3.f, 8, true);
    }
    void editDrawToolbar()
    {
        const LLRect a = editTbRect(0), b = editTbRect(EDIT_TB_N - 1);
        gGL.color4f(0.03f, 0.06f, 0.09f, 0.86f);
        gl_rect_2d(a.mLeft - 6, a.mTop + 6, b.mRight + 6, b.mBottom - 6, true);
        for (S32 i = 0; i < EDIT_TB_N; ++i)
        {
            const LLRect r = editTbRect(i);
            const bool on = editTbOn(i), hov = sEdit.tbHover == i;
            if (on) { gGL.color4f(0.07f, 0.19f, 0.24f, 1.f); gl_rect_2d(r, true); }
            else if (hov) { gGL.color4f(1.f, 1.f, 1.f, 0.08f); gl_rect_2d(r, true); }
            editIcon(EDIT_TB[i].id, r.getCenterX(), r.getCenterY(), on ? LLColor4(0.13f, 0.78f, 0.9f, 1.f) : LLColor4(0.62f, 0.68f, 0.76f, 1.f));
        }
        if (sEdit.tbHover >= 0)
        {
            const LLRect r = editTbRect(sEdit.tbHover);
            LLFontGL::getFontSansSerifSmall()->renderUTF8(std::string(EDIT_TB[sEdit.tbHover].tip), 0, (F32)r.mLeft, (F32)(r.mBottom - 18), LLColor4::white,
                LLFontGL::LEFT, LLFontGL::BASELINE, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
        }
        // the count, under the bar
        const S32 n = editCount();
        LLFontGL::getFontSansSerifSmall()->renderUTF8(n ? llformat("%d selected", n) : std::string("Click the garment to select; Shift adds, Ctrl takes away, drag a box"), 0,
            (F32)a.mLeft, (F32)(a.mBottom - (sEdit.tbHover >= 0 ? 34 : 18)), LLColor4(0.8f, 0.86f, 0.92f, 1.f), LLFontGL::LEFT, LLFontGL::BASELINE, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
    }

    // the 2D marks: points, the box, the gizmo, the toolbar
    void editDraw()
    {
        editEnsure();
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        const LLVector3 eye = cam->getOrigin();
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        // points: plain ones small (fewer on big meshes), selected orange, the soft falloff fading
        size_t total = 0;
        for (const auto& W : sTouch.world) total += W.size();
        const size_t step = total > 20000 ? total / 20000 + 1 : 1;
        std::vector<std::vector<F32>> wt;
        const bool soft = sTouch.softOn && editCount() > 0;
        if (soft) editWeights(wt);
        for (size_t mi = 0; mi < sTouch.world.size(); ++mi)
            for (size_t v = 0; v < sTouch.world[mi].size(); ++v)
            {
                if (sTouch.hidden[mi][v]) continue;
                const bool s = sTouch.sel[mi][v] != 0;
                const bool kept = mi < sTouch.mask.size() && v < sTouch.mask[mi].size() && sTouch.mask[mi][v];   // kept out of Drape: green
                const F32 w = soft ? wt[mi][v] : (s ? 1.f : 0.f);
                if (!s && w <= 0.f && ((!sTouch.showPoints && !kept) || v % step)) continue;
                if (!editFacing(mi, v, eye)) continue;
                LLCoordGL c;
                if (!cam->projectPosAgentToScreen(sTouch.world[mi][v], c, false)) continue;
                if (s) gGL.color4f(1.f, 0.81f, 0.35f, 1.f);
                else if (kept && w <= 0.f) gGL.color4f(0.24f, 0.86f, 0.52f, 0.95f);
                else if (w > 0.f) gGL.color4f(1.f, 0.6f + 0.2f * w, 0.25f, 0.25f + 0.65f * w);
                else gGL.color4f(0.62f, 0.72f, 0.82f, 0.75f);
                gl_circle_2d((F32)c.mX, (F32)c.mY, s ? 2.6f : (w > 0.f ? 2.f : 1.4f), 6, true);
            }
        // the box being dragged
        if (sEdit.box)
        {
            gGL.color4f(0.13f, 0.78f, 0.9f, 0.15f);
            gl_rect_2d(llmin(sEdit.x0, sEdit.x), llmax(sEdit.y0, sEdit.y), llmax(sEdit.x0, sEdit.x), llmin(sEdit.y0, sEdit.y), true);
            gGL.color4f(0.13f, 0.78f, 0.9f, 0.9f);
            gl_rect_2d(llmin(sEdit.x0, sEdit.x), llmax(sEdit.y0, sEdit.y), llmax(sEdit.x0, sEdit.x), llmin(sEdit.y0, sEdit.y), false);
        }
        // the gizmo
        editBuildGizmo();
        if (sEdit.gizmo)
        {
            const S32 active = sEdit.dragging ? sEdit.part : sEdit.hover;
            static const F32 COL[3][3] = { { 0.95f, 0.2f, 0.2f }, { 0.3f, 0.9f, 0.3f }, { 0.25f, 0.45f, 1.f } };
            auto colour = [&](int k, bool on) { if (on) gGL.color4f(1.f, 0.95f, 0.3f, 1.f); else gGL.color4f(COL[k][0], COL[k][1], COL[k][2], 1.f); };
            auto thick = [](const LLCoordGL& a, const LLCoordGL& b, F32 w)
            {
                for (F32 o = -w; o <= w + 0.01f; o += 1.f) { gl_line_2d(a.mX + (S32)o, a.mY, b.mX + (S32)o, b.mY); gl_line_2d(a.mX, a.mY + (S32)o, b.mX, b.mY + (S32)o); }
            };
            if (sTouch.gizmo == 1)
            {
                for (int k = 0; k < 3; ++k)
                {
                    colour(k, active == k + 1);
                    for (size_t i = 1; i < sEdit.ring2[k].size(); ++i) thick(sEdit.ring2[k][i - 1], sEdit.ring2[k][i], active == k + 1 ? 1.5f : 0.5f);
                }
            }
            else
            {
                for (int k = 0; k < 3; ++k)
                {
                    const bool on = active == k + 1;
                    colour(k, on);
                    thick(sEdit.o2, sEdit.tip2[k], on ? 1.5f : 1.f);
                    if (sTouch.gizmo == 0)
                    {
                        const F32 dx = (F32)(sEdit.tip2[k].mX - sEdit.o2.mX), dy = (F32)(sEdit.tip2[k].mY - sEdit.o2.mY), l = sqrtf(dx * dx + dy * dy);
                        if (l > 1.f)
                        {
                            const F32 ux = dx / l, uy = dy / l, hl = 14.f, hw = 6.f;
                            gl_triangle_2d((S32)(sEdit.tip2[k].mX + ux * hl), (S32)(sEdit.tip2[k].mY + uy * hl), (S32)(sEdit.tip2[k].mX - uy * hw), (S32)(sEdit.tip2[k].mY + ux * hw),
                                           (S32)(sEdit.tip2[k].mX + uy * hw), (S32)(sEdit.tip2[k].mY - ux * hw), LLColor4(on ? 1.f : COL[k][0], on ? 0.95f : COL[k][1], on ? 0.3f : COL[k][2], 1.f), true);
                        }
                    }
                    else gl_rect_2d(sEdit.tip2[k].mX - 6, sEdit.tip2[k].mY + 6, sEdit.tip2[k].mX + 6, sEdit.tip2[k].mY - 6, true);
                }
                if (active == 4) gGL.color4f(1.f, 0.95f, 0.3f, 1.f); else gGL.color4f(1.f, 0.85f, 0.f, 0.95f);
                gl_rect_2d(sEdit.o2.mX - 7, sEdit.o2.mY + 7, sEdit.o2.mX + 7, sEdit.o2.mY - 7, true);
            }
        }
        editDrawToolbar();
    }

    // the 3D marks (depth tested, so the garment and the body hide what is behind them; X-ray: not)
    void editRender()
    {
        if (!sTouch.active || sTouch.brush != TB_SELECT) return;
        editEnsure();
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        LLGLDepthTest depth(sTouch.xray ? GL_FALSE : GL_TRUE, GL_FALSE);
        LLGLEnable blend(GL_BLEND);
        const LLVector3 eye = LLViewerCamera::getInstance()->getOrigin();
        // a hair towards the eye: the lines are not lost in the surface they lie on
        auto lift = [&](const LLVector3& p) { LLVector3 d = eye - p; const F32 l = d.length(); return l > 1e-4f ? p + d * (0.0015f / l) : p; };
        if (sTouch.showEdges)
        {
            gGL.begin(LLRender::LINES);
            for (size_t mi = 0; mi < sTouch.world.size(); ++mi)
            {
                const auto& W = sTouch.world[mi];
                const auto& selv = sTouch.sel[mi];
                const auto& hidv = sTouch.hidden[mi];
                for (const auto& tr : sTouch.dae.meshes[mi].triangles)
                {
                    if (tr[0] < 0 || tr[1] < 0 || tr[2] < 0 || tr[0] >= (S32)W.size() || tr[1] >= (S32)W.size() || tr[2] >= (S32)W.size()) continue;
                    if (hidv[tr[0]] && hidv[tr[1]] && hidv[tr[2]]) continue;
                    if (!sTouch.xray)
                    {
                        // the far side is not drawn (its lines would show through thin cloth)
                        const LLVector3 fn = (W[tr[1]] - W[tr[0]]) % (W[tr[2]] - W[tr[0]]);
                        if ((fn * (eye - W[tr[0]])) * sTouch.editOut < 0.f) continue;
                    }
                    for (int k = 0; k < 3; ++k)
                    {
                        const S32 a = tr[k], b = tr[(k + 1) % 3];
                        if (selv[a] && selv[b]) gGL.color4f(1.f, 0.72f, 0.2f, 1.f);
                        else gGL.color4f(0.55f, 0.7f, 0.85f, 0.5f);
                        const LLVector3 pa = lift(W[a]), pb = lift(W[b]);
                        gGL.vertex3fv(pa.mV);
                        gGL.vertex3fv(pb.mV);
                    }
                }
            }
            gGL.end();
        }
        // the selected faces, tinted
        gGL.begin(LLRender::TRIANGLES);
        gGL.color4f(1.f, 0.62f, 0.15f, 0.22f);
        for (size_t mi = 0; mi < sTouch.world.size(); ++mi)
        {
            const auto& W = sTouch.world[mi];
            const auto& selv = sTouch.sel[mi];
            for (const auto& tr : sTouch.dae.meshes[mi].triangles)
            {
                if (tr[0] < 0 || tr[1] < 0 || tr[2] < 0 || tr[0] >= (S32)W.size() || tr[1] >= (S32)W.size() || tr[2] >= (S32)W.size()) continue;
                if (!selv[tr[0]] || !selv[tr[1]] || !selv[tr[2]]) continue;
                for (int k = 0; k < 3; ++k) { const LLVector3 p = lift(W[tr[k]]); gGL.vertex3fv(p.mV); }
            }
        }
        gGL.end();
    }

    class MGToolTouch : public LLTool
    {
    public:
        MGToolTouch() : LLTool(std::string("MGTouchUp")) {}
        // Edit Mesh draws its lines in the 3D view too (only while selecting)
        bool isAlwaysRendered() override { return true; }
        void render() override { editRender(); }

        bool handleMouseDown(S32 x, S32 y, MASK mask) override
        {
            // Alt: the camera, as everywhere else in the viewer
            if (mask & MASK_ALT) return LLToolCamera::getInstance()->handleMouseDown(x, y, mask);
            if (!sTouch.active) return false;
            if (sTouch.worldTimer.getElapsedTimeF32() > 0.1f) touchSkin();
            if (sTouch.brush == TB_SELECT)
            {
                // Edit Mesh: the toolbar, then the gizmo, else a click or a box
                editEnsure();
                const S32 tb = editTbAt(x, y);
                if (tb >= 0) { editTbClick(tb); return true; }
                editBuildGizmo();
                sEdit.down = true; sEdit.box = false;
                sEdit.x0 = sEdit.x = x; sEdit.y0 = sEdit.y = y; sEdit.mask = mask;
                sEdit.part = editPartAt(x, y);
                if (sEdit.part) editBegin();
                setMouseCapture(true);
                return true;
            }
            if (weightBrush(sTouch.brush) && (mask & MASK_CONTROL))
            {
                const std::string sb = boneAtScreen(x, y);
                if (!sb.empty())
                {
                    // Ctrl+click on the skeleton: that bone (Stiff: the bone the area follows)
                    if (sTouch.brush == TB_STIFF) { sTouch.stiffBone = sb; sTouch.stiffManual = true; push(LLSD().with("type", "lab_touch_stiff").with("bone", sb)); }
                    else { sTouch.bone = sb; push(LLSD().with("type", "lab_touch_bone").with("bone", sb)); }
                    sWV.dirty = true;
                    return true;
                }
            }
            LLVector3 dir;
            if (!touchPick(x, y, sTouch.hit, sTouch.hitN, &dir)) return true;   // empty sky: nothing
            if (weightBrush(sTouch.brush) && (mask & MASK_CONTROL))
            {
                // Ctrl+click on the garment: the bone that moves it there becomes the one painted
                const std::string b = boneAt(sTouch.hit);
                if (!b.empty()) { sTouch.bone = b; push(LLSD().with("type", "lab_touch_bone").with("bone", b)); }
                return true;
            }
            if (sTouch.brush == TB_STIFF && !sTouch.stiffManual)
            {
                // the bone this area follows: from the point under the brush
                F32 best = 1e9f; size_t bm = 0, bv = 0; bool found = false;
                for (size_t mi = 0; mi < sTouch.world.size(); ++mi)
                    for (size_t v = 0; v < sTouch.world[mi].size(); ++v)
                    {
                        const F32 d = (sTouch.world[mi][v] - sTouch.hit).lengthSquared();
                        if (d < best) { best = d; bm = mi; bv = v; found = true; }
                    }
                sTouch.stiffBone = found ? stiffAnchorFor(bm, bv) : std::string("mPelvis");
                push(LLSD().with("type", "lab_touch_stiff").with("bone", sTouch.stiffBone));
            }
            sTouch.stroking = true;
            sTouch.invert = (mask & MASK_SHIFT) != 0;
            sTouch.before.pos = sTouch.pos;
            sTouch.before.skin = sTouch.skin;
            setMouseCapture(true);
            if (sTouch.brush == TB_GRAB)
            {
                sTouch.grabPlaneN = -dir;
                touchGrabBegin();
            }
            else
            {
                touchDab();
            }
            sTouch.dabTimer.reset();
            return true;
        }

        bool handleHover(S32 x, S32 y, MASK mask) override
        {
            if (!sTouch.active) return false;
            if (sTouch.brush == TB_SELECT)
            {
                if (sTouch.worldTimer.getElapsedTimeF32() > 0.15f && !sEdit.dragging) touchSkin();
                if (sEdit.dragging)
                {
                    // the gizmo: the selection follows, 25 times a second (the garment is re-skinned each time)
                    sEdit.x = x; sEdit.y = y;
                    if (sEdit.timer.getElapsedTimeF32() > 0.04f) { editDragTo(x, y); sEdit.timer.reset(); }
                }
                else if (sEdit.down)
                {
                    sEdit.x = x; sEdit.y = y;
                    if (std::abs(x - sEdit.x0) + std::abs(y - sEdit.y0) > 4) sEdit.box = true;
                }
                else
                {
                    sEdit.tbHover = editTbAt(x, y);
                    editBuildGizmo();
                    sEdit.hover = sEdit.tbHover < 0 ? editPartAt(x, y) : 0;
                }
                gViewerWindow->setCursor(sEdit.tbHover >= 0 || sEdit.hover ? UI_CURSOR_HAND : sEdit.dragging ? UI_CURSOR_TOOLTRANSLATE : UI_CURSOR_ARROW);
                return true;
            }
            if (!sTouch.stroking)
            {
                if (sTouch.worldTimer.getElapsedTimeF32() > 0.15f) touchSkin();
                sTouch.hoverBone = weightBrush(sTouch.brush) ? boneAtScreen(x, y) : std::string();
                LLVector3 h, n;
                sTouch.hasHit = touchPick(x, y, h, n);
                if (sTouch.hasHit) { sTouch.hit = h; sTouch.hitN = n; }
                gViewerWindow->setCursor(!sTouch.hoverBone.empty() && (mask & MASK_CONTROL) ? UI_CURSOR_HAND : sTouch.hasHit ? UI_CURSOR_CROSS : UI_CURSOR_ARROW);
                return true;
            }
            if (sTouch.brush == TB_GRAB)
            {
                touchGrabMove(x, y);
            }
            else if (sTouch.dabTimer.getElapsedTimeF32() > 0.04f)
            {
                LLVector3 h, n;
                if (touchPick(x, y, h, n))
                {
                    sTouch.hit = h;
                    if (sTouch.brush != TB_PUSH) sTouch.hitN = n;   // push keeps the stroke's direction
                    touchDab();
                }
                sTouch.dabTimer.reset();
            }
            gViewerWindow->setCursor(UI_CURSOR_CROSS);
            return true;
        }

        bool handleMouseUp(S32 x, S32 y, MASK mask) override
        {
            if (sTouch.brush == TB_SELECT && sEdit.down)
            {
                editMouseUp(x, y);          // first: releasing the capture calls onMouseCaptureLost
                setMouseCapture(false);
                return true;
            }
            if (!sTouch.stroking) return false;
            setMouseCapture(false);
            touchStrokeEnd();
            return true;
        }

        static void editMouseUp(S32 x, S32 y)
        {
            if (!sEdit.down) return;
            sEdit.down = false;
            if (sEdit.dragging)
            {
                editDragTo(x, y);
                sEdit.dragging = false;
                editCommit();
            }
            else if (sEdit.box)
            {
                sEdit.box = false;
                editBox(sEdit.x0, sEdit.y0, x, y, sEdit.mask);
            }
            else editClick(x, y, sEdit.mask);
            editPushState();
        }

        void onMouseCaptureLost() override
        {
            if (sTouch.brush == TB_SELECT) { if (sEdit.down) editMouseUp(sEdit.x, sEdit.y); return; }
            touchStrokeEnd();
        }

        void draw() override
        {
            if (!sTouch.active) return;
            if (sTouch.brush == TB_SELECT) { editDraw(); return; }
            LLViewerCamera* cam = LLViewerCamera::getInstance();
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            const LLVector3 eye = cam->getOrigin();
            // the far side of the garment is not drawn (else it looks see-through)
            auto facing = [&](size_t mi, size_t v)
            {
                if (mi >= sTouch.wnorm.size() || v >= sTouch.wnorm[mi].size()) return true;
                const LLVector3& n = sTouch.wnorm[mi][v];
                if (n.lengthSquared() < 1e-12f) return true;
                return (n * (eye - sTouch.world[mi][v])) * sTouch.outward >= 0.f;
            };
            // protected points (protect brush): green
            if (sTouch.brush == TB_PROTECT)
            {
                gGL.color4f(0.24f, 0.86f, 0.52f, 0.9f);
                for (size_t mi = 0; mi < sTouch.mask.size(); ++mi)
                {
                    const auto& W = sTouch.world[mi];
                    for (size_t v = 0; v < sTouch.mask[mi].size() && v < W.size(); v += 3)
                    {
                        if (!sTouch.mask[mi][v] || !facing(mi, v)) continue;
                        LLCoordGL c;
                        if (cam->projectPosAgentToScreen(W[v], c, false)) gl_circle_2d((F32)c.mX, (F32)c.mY, 2.f, 6, true);
                    }
                }
            }
            // weight view: how much the chosen bone moves each point (blue none .. red all)
            if (weightBrush(sTouch.brush) && !sTouch.bone.empty() && !sWV.on)
            {
                size_t total = 0;
                for (const auto& W : sTouch.world) total += W.size();
                const size_t stepW = total > 7000 ? total / 7000 + 1 : 1;
                for (size_t mi = 0; mi < sTouch.world.size() && mi < sTouch.skin.size(); ++mi)
                {
                    const auto& W = sTouch.world[mi];
                    S32 j = -1;
                    for (size_t k = 0; k < sTouch.skin[mi].joints.size(); ++k) if (sTouch.skin[mi].joints[k] == sTouch.bone) j = (S32)k;
                    for (size_t v = 0; v < W.size() && v < sTouch.skin[mi].weights.size(); v += stepW)
                    {
                        double w = 0.0, sum = 0.0;
                        for (const auto& jw : sTouch.skin[mi].weights[v]) { sum += jw.second; if (jw.first == j) w += jw.second; }
                        if (sum > 0.0) w /= sum;
                        // only where this bone acts, on the side facing the camera: the rest of
                        // the garment stays clean (the live mesh shows it)
                        if (w < 0.02 || !facing(mi, v)) continue;
                        // blue -> cyan -> green -> yellow -> red
                        const F32 t = (F32)llclamp(w, 0.0, 1.0);
                        F32 rr, gg, bb;
                        if (t < 0.25f) { rr = 0.f; gg = t * 4.f; bb = 1.f; }
                        else if (t < 0.5f) { rr = 0.f; gg = 1.f; bb = 1.f - (t - 0.25f) * 4.f; }
                        else if (t < 0.75f) { rr = (t - 0.5f) * 4.f; gg = 1.f; bb = 0.f; }
                        else { rr = 1.f; gg = 1.f - (t - 0.75f) * 4.f; bb = 0.f; }
                        gGL.color4f(rr, gg, bb, t > 0.01f ? 0.95f : 0.35f);
                        LLCoordGL c;
                        if (cam->projectPosAgentToScreen(W[v], c, false)) gl_circle_2d((F32)c.mX, (F32)c.mY, t > 0.01f ? 2.2f : 1.4f, 6, true);
                    }
                }
            }
            drawBones();
            // the points the stroke moves, where they are now: cyan
            if (sTouch.stroking && !sTouch.touched.empty() && !weightBrush(sTouch.brush) && !sTouch.live)
            {
                gGL.color4f(0.f, 0.83f, 1.f, 0.85f);
                const size_t stepN = sTouch.touched.size() > 500 ? sTouch.touched.size() / 500 + 1 : 1;
                for (size_t i = 0; i < sTouch.touched.size(); i += stepN)
                {
                    const auto& tv = sTouch.touched[i];
                    LLCoordGL c;
                    if (cam->projectPosAgentToScreen(sTouch.world[tv.first][tv.second], c, false)) gl_circle_2d((F32)c.mX, (F32)c.mY, 1.8f, 6, true);
                }
            }
            // the brush ring
            if (sTouch.hasHit || sTouch.stroking)
            {
                LLCoordGL c, e;
                const LLVector3 edge = sTouch.hit + cam->getLeftAxis() * sTouch.radius;
                if (cam->projectPosAgentToScreen(sTouch.hit, c, false) && cam->projectPosAgentToScreen(edge, e, false))
                {
                    const F32 rpx = llmax(4.f, std::sqrt((F32)((e.mX - c.mX) * (e.mX - c.mX) + (e.mY - c.mY) * (e.mY - c.mY))));
                    gGL.color4f(0.f, 0.f, 0.f, 0.5f);
                    gl_circle_2d((F32)c.mX, (F32)c.mY, rpx + 1.f, 48, false);
                    if (sTouch.brush == TB_PROTECT) gGL.color4f(0.24f, 0.86f, 0.52f, 1.f);
                    else gGL.color4f(0.f, 0.83f, 1.f, 1.f);
                    gl_circle_2d((F32)c.mX, (F32)c.mY, rpx, 48, false);
                    gl_circle_2d((F32)c.mX, (F32)c.mY, 2.f, 8, true);
                }
            }
        }
    };
    MGToolTouch* sTouchTool = nullptr;

    // ------------------------------------------------------------------
    // <MG> Drape: the worn garment, skinned in the current pose, settles on the worn body as
    // cloth (MGDrape) on a worker thread. The thread only touches the job; the page and the 3D
    // tool send it commands; dots show the cloth; Apply turns the result back into the file's
    // space (each point's own skinning, inverted) and writes the _fit copy.
    // ------------------------------------------------------------------
    void touchStop();

    struct DrapeCmd
    {
        enum Kind { PIN, UNPIN, GRAB, RELEASE, TIGHTEN, FREE, REBASE, SETTINGS, PAUSE, RESUME, RESET, CLEARPINS, SMOOTH, BODY, CARRY, MOVEPINS } kind;
        std::vector<std::array<int, 3>> tris;   // body: its triangles
        std::vector<int> pts;
        std::vector<float> targets;   // grab: x y z per point
        float value = 0.f;            // grab: weight; tighten: factor
        MGDrape::Settings settings;
    };
    struct DrapeJob
    {
        std::mutex lock;
        std::vector<DrapeCmd> cmds;           // guarded
        std::vector<float> snap;              // guarded: the cloth now
        std::vector<uint8_t> pins;            // guarded
        float motion = 0.f;                   // guarded
        size_t contacts = 0, steps = 0;       // guarded
        bool paused = false;                  // guarded
        std::atomic<bool> stop{ false };
        // set before the thread starts, read-only after
        std::vector<float> start;
        std::vector<std::array<int, 3>> tris;
        std::vector<float> body;
        std::vector<std::array<int, 3>> bodyTris;
        MGDrape::Settings settings;
        std::vector<uint8_t> protect;
        std::thread worker;
        // drapeStop joins first; never let a joinable thread be destroyed (std::terminate)
        ~DrapeJob() { if (worker.joinable()) worker.detach(); }
    };
    struct Drape
    {
        bool active = false;
        std::shared_ptr<DrapeJob> job;
        MGCreatorDae::File dae;
        std::string outPath;
        std::vector<size_t> base;                         // first particle of each mesh
        std::vector<glm::mat4> toFile;                    // per particle: world -> file space
        std::vector<std::vector<int>> nb;                 // particle neighbours (the brush spreads along the cloth)
        F32 outward = 1.f;                                // +1: the triangles' normals point away from the body
        int hitTri = -1;                                  // the cloth triangle under the mouse
        std::vector<std::string> bodyNames;               // the worn items it drapes on
        // follow the pose: the garment's skinning (file -> agent per point) and the body, kept current
        std::vector<FitMesh> skins;                       // per mesh, as Second Life skins it
        std::vector<glm::mat4> mNow;                      // per point: file -> agent space, the pose now
        std::vector<float> skinnedNow;                    // per point: where the skin puts it now
        bool follow = true;
        LLFrameTimer followTimer;
        S32 tool = 0;                                     // 0 pin, 1 grab, 2 tighten, 3 smooth, 4 move pins
        F32 radius = 0.04f;
        // the stroke
        bool stroking = false, invert = false;
        LLVector3 hit, planeN;
        bool hasHit = false;
        std::vector<int> grabSet;
        std::vector<float> grabOff;                       // each grabbed point's offset from the hit
        std::vector<int> painted;
        LLFrameTimer dabTimer, stateTimer;
        LLUUID pendingFile, standin;
        LLFrameTimer pendingTimer;
    } sDrape;

    void drapeSend(const DrapeCmd& c)
    {
        if (!sDrape.job) return;
        std::lock_guard<std::mutex> g(sDrape.job->lock);
        sDrape.job->cmds.push_back(c);
    }

    void drapeRun(std::shared_ptr<DrapeJob> job)
    {
        MGDrape::Sim sim;
        if (!sim.init(job->start, job->tris, job->body, job->bodyTris, job->settings)) { job->stop = true; return; }
        for (size_t i = 0; i < job->protect.size(); ++i) if (job->protect[i]) sim.pin((int)i, true);
        bool paused = false;
        while (!job->stop)
        {
            std::vector<DrapeCmd> cmds;
            {
                std::lock_guard<std::mutex> g(job->lock);
                cmds.swap(job->cmds);
            }
            for (const DrapeCmd& c : cmds)
            {
                switch (c.kind)
                {
                case DrapeCmd::PIN: for (int i : c.pts) sim.pin(i, true); break;
                case DrapeCmd::UNPIN: for (int i : c.pts) sim.pin(i, false); break;
                case DrapeCmd::MOVEPINS:
                    for (size_t k = 0; k < c.pts.size(); ++k) sim.movePin(c.pts[k], c.targets[k * 3], c.targets[k * 3 + 1], c.targets[k * 3 + 2]);
                    break;
                case DrapeCmd::GRAB:
                    for (size_t k = 0; k < c.pts.size(); ++k) sim.grab(c.pts[k], c.targets[k * 3], c.targets[k * 3 + 1], c.targets[k * 3 + 2], c.value);
                    break;
                case DrapeCmd::RELEASE: sim.releaseGrabs(); for (int i : c.pts) sim.rebase(i); break;
                case DrapeCmd::TIGHTEN: for (int i : c.pts) { sim.setFree(i, true); sim.scaleRest(i, c.value); } break;
                case DrapeCmd::FREE: for (int i : c.pts) sim.setFree(i, true); break;
                case DrapeCmd::REBASE: for (int i : c.pts) sim.rebase(i); break;
                case DrapeCmd::SETTINGS: sim.setSettings(c.settings); break;
                case DrapeCmd::PAUSE: paused = true; break;
                case DrapeCmd::RESUME: paused = false; break;
                case DrapeCmd::RESET:
                    sim.init(job->start, job->tris, job->body, job->bodyTris, sim.settings());
                    for (size_t i = 0; i < job->protect.size(); ++i) if (job->protect[i]) sim.pin((int)i, true);
                    break;
                case DrapeCmd::CLEARPINS: sim.clearPins(); break;
                case DrapeCmd::SMOOTH: for (int i : c.pts) { sim.setFree(i, true); sim.relax(i, c.value); } break;
                case DrapeCmd::BODY: sim.setBody(c.targets, c.tris); break;
                case DrapeCmd::CARRY: sim.carry(c.targets); break;
                }
            }
            const auto t0 = std::chrono::steady_clock::now();
            if (!paused) sim.step(1.f / 60.f);
            {
                std::lock_guard<std::mutex> g(job->lock);
                job->snap = sim.positions();
                job->pins = sim.pins();
                job->motion = sim.motion();
                job->contacts = sim.contacts();
                job->paused = paused;
                if (!paused) ++job->steps;
            }
            // 60 steps a second at most; settled or paused: a calm pace
            const auto spent = std::chrono::steady_clock::now() - t0;
            const auto want = std::chrono::milliseconds(paused || sim.motion() < 0.008f ? 50 : 16);   // Drape v4 rests at ~0.006 m/s
            if (spent < want) std::this_thread::sleep_for(want - spent);
        }
    }

    // <MG> Drape live preview: the worn garment itself takes the cloth's shape while it settles.
    // Each of its volume vertices is matched (same pose, same place) to a cloth point; the
    // inverse of that vertex's skinning (bind shape included) turns the cloth point back into
    // volume space. Main thread only; the originals are put back when Drape stops. If the
    // volume is replaced (reload, level of detail) the preview stops and the dots take over.
    struct PreviewFace
    {
        LLUUID prim;
        S32 face = 0;
        LLVolume* vol = nullptr;        // compared only, never dereferenced unless it is still the object's
        S32 count = 0;
        std::vector<int> map;           // volume vertex -> cloth point (-1: none)
        std::vector<glm::mat4> inv;     // agent space -> volume space
        std::vector<glm::mat4> f2v;     // Touch up: file space -> volume space (does not depend on the pose)
        std::vector<F32> pos, nrm;      // the original positions and normals (x y z w)
        F32 ext[8] = { 0 };             // the original extents (min, max)
        std::vector<U16> idx;           // <MG> Edit Mesh: the original triangles, while some are hidden
    };
    struct Preview
    {
        bool on = false;
        bool touch = false;             // Touch up's (file space positions and weights) or Drape's (cloth)
        std::vector<PreviewFace> faces;
        std::vector<size_t> base;       // Touch up: first point of each file mesh in the flat list
        S32 retries = 0;
        LLFrameTimer timer;
    } sPreview;

    LLVOVolume* previewVolume(const LLUUID& prim)
    {
        LLViewerObject* o = gObjectList.findObject(prim);
        if (!o || o->isDead() || o->getPCode() != LL_PCODE_VOLUME) return nullptr;
        return static_cast<LLVOVolume*>(o);
    }

    // the face, if it is still the one that was matched
    LLVolumeFace* previewFace(PreviewFace& pf, LLVOVolume** vout)
    {
        LLVOVolume* v = previewVolume(pf.prim);
        if (!v) return nullptr;
        LLVolume* vol = v->getVolume();
        if (!vol || vol != pf.vol || pf.face >= vol->getNumVolumeFaces()) return nullptr;
        LLVolumeFace& f = vol->getVolumeFace(pf.face);
        if (f.mNumVertices != pf.count || !f.mPositions) return nullptr;
        if (vout) *vout = v;
        return &f;
    }

    // normals and extents from the positions
    void previewFaceDone(LLVolumeFace& f)
    {
        const S32 n = f.mNumVertices;
        if (f.mNormals && f.mIndices && f.mNumIndices >= 3)
        {
            std::vector<F32> acc((size_t)n * 3, 0.f);
            for (S32 t = 0; t + 2 < f.mNumIndices; t += 3)
            {
                const S32 ia = f.mIndices[t], ib = f.mIndices[t + 1], ic = f.mIndices[t + 2];
                if (ia >= n || ib >= n || ic >= n) continue;
                const F32* a = f.mPositions[ia].getF32ptr(); const F32* b = f.mPositions[ib].getF32ptr(); const F32* c = f.mPositions[ic].getF32ptr();
                const F32 e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
                const F32 nn[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                for (int q = 0; q < 3; ++q) { acc[ia * 3 + q] += nn[q]; acc[ib * 3 + q] += nn[q]; acc[ic * 3 + q] += nn[q]; }
            }
            for (S32 i = 0; i < n; ++i)
            {
                const F32* nn = &acc[i * 3];
                const F32 l = sqrtf(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
                if (l > 1e-12f) f.mNormals[i].set(nn[0] / l, nn[1] / l, nn[2] / l, 0.f);
            }
        }
        if (n <= 0) return;
        F32 lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        for (S32 i = 0; i < n; ++i)
        {
            const F32* p = f.mPositions[i].getF32ptr();
            for (int q = 0; q < 3; ++q) { lo[q] = llmin(lo[q], p[q]); hi[q] = llmax(hi[q], p[q]); }
        }
        if (f.mExtents) { f.mExtents[0].set(lo[0], lo[1], lo[2], 0.f); f.mExtents[1].set(hi[0], hi[1], hi[2], 0.f); }
        if (f.mCenter) f.mCenter->set((lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f, 0.f);
    }

    void previewRebuild(const std::set<LLVOVolume*>& touched)
    {
        for (LLVOVolume* v : touched)
            if (v && v->mDrawable.notNull()) gPipeline.markRebuild(v->mDrawable, LLDrawable::REBUILD_POSITION);
    }

    void previewRestore()
    {
        if (sPreview.faces.empty()) { sPreview.on = false; return; }
        std::set<LLVOVolume*> touched;
        for (auto& pf : sPreview.faces)
        {
            LLVOVolume* v = nullptr;
            LLVolumeFace* f = previewFace(pf, &v);
            if (!f) continue;
            for (S32 i = 0; i < pf.count; ++i)
            {
                f->mPositions[i].loadua(&pf.pos[i * 4]);
                if (f->mNormals) f->mNormals[i].loadua(&pf.nrm[i * 4]);
            }
            if (!pf.idx.empty() && f->mIndices && pf.idx.size() == (size_t)f->mNumIndices) std::copy(pf.idx.begin(), pf.idx.end(), f->mIndices);
            if (f->mExtents) { f->mExtents[0].loadua(pf.ext); f->mExtents[1].loadua(pf.ext + 4); }
            if (f->mCenter) f->mCenter->set((pf.ext[0] + pf.ext[4]) * 0.5f, (pf.ext[1] + pf.ext[5]) * 0.5f, (pf.ext[2] + pf.ext[6]) * 0.5f, 0.f);
            touched.insert(v);
        }
        sPreview.faces.clear();
        sPreview.on = false;
        previewRebuild(touched);
    }

    void previewKeep(PreviewFace& pf, const LLVolumeFace& f)
    {
        pf.pos.resize((size_t)pf.count * 4);
        pf.nrm.assign((size_t)pf.count * 4, 0.f);
        for (S32 i = 0; i < pf.count; ++i)
        {
            const F32* p = f.mPositions[i].getF32ptr();
            for (int q = 0; q < 4; ++q) pf.pos[i * 4 + q] = p[q];
            if (f.mNormals) { const F32* nn = f.mNormals[i].getF32ptr(); for (int q = 0; q < 4; ++q) pf.nrm[i * 4 + q] = nn[q]; }
        }
        if (f.mExtents)
        {
            const F32* lo = f.mExtents[0].getF32ptr(); const F32* hi = f.mExtents[1].getF32ptr();
            for (int q = 0; q < 4; ++q) { pf.ext[q] = lo[q]; pf.ext[4 + q] = hi[q]; }
        }
    }

    // every face still the one matched; a volume the viewer replaced (a reload after a save, another
    // level of detail) with the same vertices is taken over: the matching holds
    bool previewEnsure()
    {
        for (auto& pf : sPreview.faces)
        {
            if (previewFace(pf, nullptr)) continue;
            LLVOVolume* v = previewVolume(pf.prim);
            LLVolume* vol = v ? v->getVolume() : nullptr;
            if (!vol || pf.face >= vol->getNumVolumeFaces() || vol->getVolumeFace(pf.face).mNumVertices != pf.count || !vol->getVolumeFace(pf.face).mPositions)
            {
                LL_INFOS("MGCreator") << "Live preview stopped: the garment's mesh changed (face " << pf.face << ", "
                                      << (vol && pf.face < vol->getNumVolumeFaces() ? vol->getVolumeFace(pf.face).mNumVertices : -1) << " vertices, was " << pf.count << ")" << LL_ENDL;
                return false;
            }
            pf.vol = vol;
            previewKeep(pf, vol->getVolumeFace(pf.face));
            pf.idx.clear();               // a new volume: its own triangles (hidden ones are hidden again)
            sTouch.hideDirty = true;
        }
        return true;
    }

    // forget the preview without putting anything back (Touch up: the file holds the same shape)
    void previewDrop()
    {
        sPreview.faces.clear();
        sPreview.on = false;
        sPreview.touch = false;
    }

    // match the worn garment's vertices to the cloth's starting points (agent space, this pose)
    // fileToWorld (Touch up): each point's file -> agent matrix now, to write file positions later
    bool previewBuild(const std::vector<float>& start, const std::vector<glm::mat4>* fileToWorld = nullptr, bool forTouch = false)
    {
        sPreview.faces.clear();
        sPreview.on = false;
        sPreview.touch = false;
        if (!isAgentAvatarValid()) return false;
        const size_t n = start.size() / 3;
        const F32 cellSize = 0.002f;
        auto keyOf = [&](F32 x, F32 y, F32 z, int dx, int dy, int dz)
        {
            const int64_t cx = (int64_t)floorf(x / cellSize) + dx, cy = (int64_t)floorf(y / cellSize) + dy, cz = (int64_t)floorf(z / cellSize) + dz;
            return ((uint64_t)(cx + (1 << 20)) << 42) | ((uint64_t)(cy + (1 << 20)) << 21) | (uint64_t)(cz + (1 << 20));
        };
        std::unordered_map<uint64_t, std::vector<int>> grid;
        grid.reserve(n);
        for (size_t i = 0; i < n; ++i) grid[keyOf(start[i * 3], start[i * 3 + 1], start[i * 3 + 2], 0, 0, 0)].push_back((int)i);
        size_t total = 0, matched = 0, bad = 0;
        // the garment's prims: those recorded at wear time, and any rigged local mesh worn now
        std::vector<LLUUID> prims;
        for (const auto& a : sApplied) prims.push_back(a.prim);
        for (const auto& kv : gAgentAvatarp->mAttachmentPoints)
        {
            LLViewerJointAttachment* att = kv.second;
            if (!att || att->getIsHUDAttachment()) continue;
            for (const auto& ao : att->mAttachedObjects)
            {
                LLViewerObject* root = ao.get();
                if (!root || root->isDead()) continue;
                for (LLViewerObject* o : primsOf(root))
                {
                    if (!o || o->getPCode() != LL_PCODE_VOLUME || !static_cast<LLVOVolume*>(o)->mIsLocalMesh) continue;
                    if (std::find(prims.begin(), prims.end(), o->getID()) == prims.end()) prims.push_back(o->getID());
                }
            }
        }
        std::string why;   // each prim left out, and why (in the log)
        for (const LLUUID& primId : prims)
        {
            struct { LLUUID prim; } a{ primId };
            LLVOVolume* v = previewVolume(a.prim);
            const std::string tag = a.prim.asString().substr(0, 8);
            if (!v) { why += " " + tag + ":gone"; continue; }
            if (!v->mIsLocalMesh) { why += " " + tag + ":not-local-mesh"; continue; }
            if (!v->isRiggedMesh()) { why += " " + tag + ":not-rigged"; continue; }
            LLVolume* vol = v->getVolume();
            const LLMeshSkinInfo* skin = v->getSkinInfo();
            if (!vol || !skin || skin->mJointNames.empty()) { why += " " + tag + (vol ? ":no-skin" : ":no-volume"); continue; }
            v->updateRiggedVolume(true, LLRiggedVolume::UPDATE_ALL_FACES, false);
            LLRiggedVolume* rv = v->getRiggedVolume();
            if (!rv || rv->getNumVolumeFaces() != vol->getNumVolumeFaces()) { why += " " + tag + (rv ? ":posed-faces-differ" : ":no-posed-copy"); continue; }
            const S32 count = llmin((S32)skin->mJointNames.size(), (S32)LL_MAX_JOINTS_PER_MESH_OBJECT);
            LLMatrix4a mat[LL_MAX_JOINTS_PER_MESH_OBJECT];
            LLSkinningUtil::initSkinningMatrixPalette(mat, count, skin, gAgentAvatarp);
            const glm::mat4 bs = glm::make_mat4(skin->mBindShapeMatrix.getF32ptr());
            for (S32 fi = 0; fi < vol->getNumVolumeFaces(); ++fi)
            {
                LLVolumeFace& sf = vol->getVolumeFace(fi);
                const LLVolumeFace& df = rv->getVolumeFace(fi);
                if (!sf.mPositions || !sf.mWeights || !df.mPositions || sf.mNumVertices <= 0 || df.mNumVertices != sf.mNumVertices)
                {
                    why += " " + tag + "/face" + std::to_string(fi) + (!sf.mPositions ? ":no-positions" : !sf.mWeights ? ":no-weights" : !df.mPositions ? ":no-posed-positions" : ":vertex-count");
                    continue;
                }
                PreviewFace pf;
                pf.prim = a.prim;
                pf.face = fi;
                pf.vol = vol;
                pf.count = sf.mNumVertices;
                pf.map.assign(pf.count, -1);
                pf.inv.resize(pf.count);
                if (fileToWorld) pf.f2v.assign(pf.count, glm::mat4(1.f));
                pf.pos.resize((size_t)pf.count * 4);
                pf.nrm.assign((size_t)pf.count * 4, 0.f);
                for (S32 i = 0; i < pf.count; ++i)
                {
                    const F32* p = sf.mPositions[i].getF32ptr();
                    for (int q = 0; q < 4; ++q) pf.pos[i * 4 + q] = p[q];
                    if (sf.mNormals) { const F32* nn = sf.mNormals[i].getF32ptr(); for (int q = 0; q < 4; ++q) pf.nrm[i * 4 + q] = nn[q]; }
                    ++total;
                    // the nearest cloth point, within 2 mm
                    const F32* r = df.mPositions[i].getF32ptr();
                    int best = -1;
                    F32 bestD = 0.002f * 0.002f;
                    for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) for (int dz = -1; dz <= 1; ++dz)
                    {
                        auto it = grid.find(keyOf(r[0], r[1], r[2], dx, dy, dz));
                        if (it == grid.end()) continue;
                        for (int c : it->second)
                        {
                            const F32 ex = start[c * 3] - r[0], ey = start[c * 3 + 1] - r[1], ez = start[c * 3 + 2] - r[2];
                            const F32 d = ex * ex + ey * ey + ez * ez;
                            if (d < bestD) { bestD = d; best = c; }
                        }
                    }
                    if (best < 0) continue;
                    // this vertex's skinning: it must put the volume point where the viewer draws it
                    std::array<F32, 4> w;
                    const F32* sw = sf.mWeights[i].getF32ptr();
                    for (int q = 0; q < 4; ++q) w[q] = sw[q];
                    LLMatrix4a fin;
                    LLSkinningUtil::getPerVertexSkinMatrix(w.data(), mat, true, fin, count);
                    const glm::mat4 M = glm::make_mat4(fin.getF32ptr()) * bs;
                    const glm::vec4 wp = M * glm::vec4(p[0], p[1], p[2], 1.f);
                    const F32 ex = wp.x - r[0], ey = wp.y - r[1], ez = wp.z - r[2];
                    if (ex * ex + ey * ey + ez * ez > 0.002f * 0.002f) { ++bad; continue; }
                    pf.inv[i] = glm::inverse(M);
                    if (fileToWorld && (size_t)best < fileToWorld->size()) pf.f2v[i] = pf.inv[i] * (*fileToWorld)[best];
                    pf.map[i] = best;
                    ++matched;
                }
                if (sf.mExtents)
                {
                    const F32* lo = sf.mExtents[0].getF32ptr(); const F32* hi = sf.mExtents[1].getF32ptr();
                    for (int q = 0; q < 4; ++q) { pf.ext[q] = lo[q]; pf.ext[4 + q] = hi[q]; }
                }
                sPreview.faces.push_back(std::move(pf));
            }
        }
        LL_INFOS("MGCreator") << (fileToWorld ? "Touch up live: " : "Drape preview: ") << matched << " of " << total << " garment vertices matched, " << bad
                              << " skinning mismatches; " << prims.size() << " prims" << (why.empty() ? std::string() : ", left out:" + why) << LL_ENDL;
        if (total == 0 || matched < total * 8 / 10 || bad > total / 20) { sPreview.faces.clear(); return false; }
        sPreview.on = true;
        sPreview.touch = forTouch;
        sPreview.timer.reset();
        return true;
    }

    LLTool* sDrapeTool = nullptr;   // the 3D tool (MGToolDrape, below)

    void drapeStop()
    {
        if (!sDrape.active) return;
        previewRestore();
        // the mouse back to normal: a stroke in progress ends, the tool goes
        if (sDrapeTool && sDrapeTool->hasMouseCapture()) sDrapeTool->setMouseCapture(false);
        sDrape.stroking = false;
        if (sDrapeTool && LLToolMgr::getInstance()->getCurrentTool() == sDrapeTool) LLToolMgr::getInstance()->clearTransientTool();
        sDrape.active = false;
        if (sDrape.job)
        {
            sDrape.job->stop = true;
            if (sDrape.job->worker.joinable()) sDrape.job->worker.join();   // a step is short: this returns quickly
            sDrape.job.reset();
        }
        sDrape.dae = MGCreatorDae::File();
        sDrape.toFile.clear();
        sDrape.base.clear();
        sDrape.nb.clear();
    }

    // the cloth now (a copy), and its pins
    bool drapeSnap(std::vector<float>& x, std::vector<uint8_t>* pins = nullptr)
    {
        if (!sDrape.job) return false;
        std::lock_guard<std::mutex> g(sDrape.job->lock);
        if (sDrape.job->snap.empty()) return false;
        x = sDrape.job->snap;
        if (pins) *pins = sDrape.job->pins;
        return true;
    }

    void drapeBody(std::vector<float>& P, std::vector<std::array<int, 3>>& T, const std::set<LLUUID>& garment, std::vector<std::string>* names);

    // the garment as Second Life skins it in the pose now: per point file -> agent matrices
    void drapeSkinNow(std::vector<glm::mat4>& M, std::vector<float>& P)
    {
        M.clear();
        P.clear();
        if (!isAgentAvatarValid()) return;
        for (size_t mi = 0; mi < sDrape.dae.meshes.size() && mi < sDrape.skins.size(); ++mi)
        {
            const MGCreatorDae::SkinnedMesh& m = sDrape.dae.meshes[mi];
            FitMesh& fm = sDrape.skins[mi];
            if (fm.skin.isNull()) return;
            const S32 count = (S32)fm.skin->mJointNames.size();
            LLMatrix4a mat[LL_MAX_JOINTS_PER_MESH_OBJECT];
            LLSkinningUtil::initSkinningMatrixPalette(mat, count, fm.skin.get(), gAgentAvatarp);
            const glm::mat4 bs = glm::make_mat4(fm.skin->mBindShapeMatrix.getF32ptr());
            for (size_t v = 0; v < m.vertexCount() && v < fm.packed.size(); ++v)
            {
                LLMatrix4a fin;
                std::array<F32, 4> pk = fm.packed[v];
                LLSkinningUtil::getPerVertexSkinMatrix(pk.data(), mat, true, fin, count);
                const glm::mat4 Mv = glm::make_mat4(fin.getF32ptr()) * bs;
                const glm::vec4 w = Mv * glm::vec4((F32)m.positions[v * 3], (F32)m.positions[v * 3 + 1], (F32)m.positions[v * 3 + 2], 1.f);
                M.push_back(Mv);
                P.push_back(w.x); P.push_back(w.y); P.push_back(w.z);
            }
        }
    }

    // Drape follows the pose (a held pose, an animation): when the skin moved the garment, the
    // shape it keeps and its pins are carried along, the body is read again; the cloth then
    // moves as cloth would. force: now (Apply), whatever the timer.
    void drapeFollow(bool force)
    {
        if (!sDrape.active || !sDrape.job) return;
        if (!force && (!sDrape.follow || sDrape.followTimer.getElapsedTimeF32() < 0.2f)) return;
        sDrape.followTimer.reset();
        std::vector<glm::mat4> M;
        std::vector<float> P;
        drapeSkinNow(M, P);
        if (M.size() != sDrape.toFile.size() || P.size() != sDrape.skinnedNow.size()) return;
        // moved? (a few points are enough to tell)
        F32 moved = 0.f;
        const size_t n = M.size(), stride = llmax<size_t>(1, n / 400);
        for (size_t i = 0; i < n; i += stride)
        {
            const F32 dx = P[i * 3] - sDrape.skinnedNow[i * 3], dy = P[i * 3 + 1] - sDrape.skinnedNow[i * 3 + 1], dz = P[i * 3 + 2] - sDrape.skinnedNow[i * 3 + 2];
            moved = llmax(moved, dx * dx + dy * dy + dz * dz);
        }
        if (moved < 0.0005f * 0.0005f) return;
        // each point's carry: the pose now x the pose before (inverse)
        DrapeCmd carry;
        carry.kind = DrapeCmd::CARRY;
        carry.targets.resize(n * 16);
        for (size_t i = 0; i < n; ++i)
        {
            const glm::mat4 D = M[i] * sDrape.toFile[i];
            memcpy(&carry.targets[i * 16], glm::value_ptr(D), 16 * sizeof(F32));
            sDrape.toFile[i] = glm::inverse(M[i]);
        }
        sDrape.mNow = M;
        sDrape.skinnedNow = P;
        drapeSend(carry);
        // the body where it is now
        std::set<LLUUID> garment;
        for (const auto& a : sApplied) garment.insert(a.prim);
        DrapeCmd body;
        body.kind = DrapeCmd::BODY;
        drapeBody(body.targets, body.tris, garment, nullptr);
        if (!body.tris.empty()) drapeSend(body);
    }

    // the preview: the garment's vertices follow the cloth, 10 times a second
    void previewTick()
    {
        if (!sDrape.active || sPreview.touch) return;
        if (!sPreview.on)
        {
            // not matched (or lost): a few more tries, the garment may have just been reloaded
            if (sPreview.retries < 6 && sDrape.job && sPreview.timer.getElapsedTimeF32() > 2.f)
            {
                ++sPreview.retries;
                sPreview.timer.reset();
                if (sDrape.skinnedNow.size() == sDrape.mNow.size() * 3 && !sDrape.mNow.empty()) previewBuild(sDrape.skinnedNow, &sDrape.mNow);
                else previewBuild(sDrape.job->start);
            }
            return;
        }
        if (sPreview.timer.getElapsedTimeF32() < 0.1f) return;
        sPreview.timer.reset();
        std::vector<float> X;
        if (!drapeSnap(X)) return;
        // every face still the one matched, or taken over? (else: back to the dots)
        if (!previewEnsure()) { previewRestore(); return; }
        std::set<LLVOVolume*> touched;
        const size_t np = X.size() / 3;
        for (auto& pf : sPreview.faces)
        {
            LLVOVolume* v = nullptr;
            LLVolumeFace* f = previewFace(pf, &v);
            if (!f) continue;
            for (S32 i = 0; i < pf.count; ++i)
            {
                const int c = pf.map[i];
                if (c < 0 || (size_t)c >= np) continue;
                // through the file's space and the pose now when known (the avatar may have moved)
                const glm::vec4 xa(X[c * 3], X[c * 3 + 1], X[c * 3 + 2], 1.f);
                const glm::vec4 q = (!pf.f2v.empty() && (size_t)c < sDrape.toFile.size()) ? pf.f2v[i] * (sDrape.toFile[c] * xa) : pf.inv[i] * xa;
                if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) continue;
                f->mPositions[i].set(q.x, q.y, q.z, pf.pos[i * 4 + 3]);
            }
            previewFaceDone(*f);
            touched.insert(v);
        }
        previewRebuild(touched);
    }

    // Touch up live: the worn garment's vertices matched to the file's points; the brushes then
    // write shape (file space -> volume space, one matrix per vertex, whatever the pose) and
    // weights (the file's bones -> the worn mesh's, by name) straight into it
    void touchPreviewStart()
    {
        sTouch.live = false;
        sTouch.liveDirty = false;
        std::vector<float> start;
        std::vector<glm::mat4> f2w;
        sPreview.base.clear();
        for (size_t mi = 0; mi < sTouch.world.size() && mi < sTouch.meshes.size(); ++mi)
        {
            sPreview.base.push_back(start.size() / 3);
            if (sTouch.meshes[mi].skin.isNull()) continue;
            const glm::mat4 bs = glm::make_mat4(sTouch.meshes[mi].skin->mBindShapeMatrix.getF32ptr());
            for (size_t v = 0; v < sTouch.world[mi].size() && v < sTouch.finals[mi].size(); ++v)
            {
                const LLVector3& w = sTouch.world[mi][v];
                start.push_back(w.mV[0]); start.push_back(w.mV[1]); start.push_back(w.mV[2]);
                f2w.push_back(glm::make_mat4(sTouch.finals[mi][v].getF32ptr()) * bs);
            }
        }
        sTouch.live = previewBuild(start, &f2w, true);
    }

    // <MG> Edit Mesh: is this point of the flat list (all the file's meshes, in order) hidden?
    bool editHiddenFlat(int c)
    {
        if (c < 0 || sPreview.base.empty()) return false;
        size_t mi = 0;
        while (mi + 1 < sPreview.base.size() && (size_t)c >= sPreview.base[mi + 1]) ++mi;
        const size_t v = (size_t)c - sPreview.base[mi];
        return mi < sTouch.hidden.size() && v < sTouch.hidden[mi].size() && sTouch.hidden[mi][v];
    }

    // hidden faces left out of the worn garment (its triangles made empty; the originals kept to
    // put back): only on your screen, the file keeps every face
    void editApplyHidden()
    {
        sTouch.hideDirty = false;
        if (!sTouch.active || !sTouch.live || !sPreview.on || !sPreview.touch) return;
        bool any = false;
        for (const auto& h : sTouch.hidden) for (U8 b : h) if (b) { any = true; break; }
        std::set<LLVOVolume*> touched;
        for (auto& pf : sPreview.faces)
        {
            LLVOVolume* v = nullptr;
            LLVolumeFace* f = previewFace(pf, &v);
            if (!f || !f->mIndices || f->mNumIndices < 3) continue;
            if (pf.idx.empty())
            {
                if (!any) continue;
                pf.idx.assign(f->mIndices, f->mIndices + f->mNumIndices);
            }
            if (pf.idx.size() != (size_t)f->mNumIndices) continue;
            for (S32 t = 0; t + 2 < f->mNumIndices; t += 3)
            {
                bool hid = any;
                for (int k = 0; k < 3 && hid; ++k)
                {
                    const S32 vi = pf.idx[t + k];
                    const int c = vi < pf.count ? pf.map[vi] : -1;
                    if (!editHiddenFlat(c)) hid = false;
                }
                f->mIndices[t] = pf.idx[t];
                f->mIndices[t + 1] = hid ? pf.idx[t] : pf.idx[t + 1];
                f->mIndices[t + 2] = hid ? pf.idx[t] : pf.idx[t + 2];
            }
            if (!any) pf.idx.clear();
            touched.insert(v);
        }
        for (LLVOVolume* v : touched) if (v && v->mDrawable.notNull()) gPipeline.markRebuild(v->mDrawable, LLDrawable::REBUILD_ALL);
    }

    void editHide(bool hideSel)
    {
        editEnsure();
        for (size_t mi = 0; mi < sTouch.sel.size(); ++mi)
            for (size_t v = 0; v < sTouch.sel[mi].size(); ++v)
            {
                if (hideSel) { if (sTouch.sel[mi][v]) { sTouch.hidden[mi][v] = 1; sTouch.sel[mi][v] = 0; } }
                else sTouch.hidden[mi][v] = 0;
            }
        editApplyHidden();
    }

    // the page: how much is selected and hidden, per piece too
    void editPushState()
    {
        S32 count = 0, hid = 0;
        LLSD meshes = LLSD::emptyArray();
        for (size_t mi = 0; mi < sTouch.sel.size() && mi < sTouch.hidden.size(); ++mi)
        {
            S32 s = 0, h = 0;
            for (U8 b : sTouch.sel[mi]) s += b;
            for (U8 b : sTouch.hidden[mi]) h += b;
            count += s; hid += h;
            meshes.append(LLSD().with("sel", s).with("hidden", h).with("points", (S32)sTouch.sel[mi].size()));
        }
        push(LLSD().with("type", "lab_edit_sel").with("count", count).with("hidden", hid).with("meshes", meshes)
            .with("undo", (S32)sTouch.undo.size()).with("redo", (S32)sTouch.redo.size()));
    }

    void touchPreviewApply()
    {
        if (!sTouch.active || !sPreview.on || !sPreview.touch || !previewEnsure())
        {
            if (sTouch.live) previewDrop();
            sTouch.live = false;
            return;
        }
        std::set<LLVOVolume*> touched;
        for (auto& pf : sPreview.faces)
        {
            LLVOVolume* v = nullptr;
            LLVolumeFace* f = previewFace(pf, &v);
            if (!f) continue;
            const LLMeshSkinInfo* skin = v->getSkinInfo();
            std::map<size_t, std::vector<S32>> boneMap;   // file mesh -> its bones' places in the worn mesh's skin
            for (S32 i = 0; i < pf.count; ++i)
            {
                const int c = pf.map[i];
                if (c < 0) continue;
                // which file mesh and point
                size_t mi = 0;
                while (mi + 1 < sPreview.base.size() && (size_t)c >= sPreview.base[mi + 1]) ++mi;
                const size_t vv = (size_t)c - sPreview.base[mi];
                if (mi >= sTouch.pos.size() || vv * 3 + 2 >= sTouch.pos[mi].size()) continue;
                const double* p = &sTouch.pos[mi][vv * 3];
                const glm::vec4 q = pf.f2v[i] * glm::vec4((F32)p[0], (F32)p[1], (F32)p[2], 1.f);
                if (std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z)) f->mPositions[i].set(q.x, q.y, q.z, pf.pos[i * 4 + 3]);
                // the painted weights (a bone the worn mesh does not have yet waits for the reload)
                if (!sTouch.skinEdited || !f->mWeights || !skin || mi >= sTouch.skin.size() || vv >= sTouch.meshes[mi].packed.size()) continue;
                auto bm = boneMap.find(mi);
                if (bm == boneMap.end())
                {
                    std::vector<S32> m2;
                    for (const auto& name : sTouch.skin[mi].joints)
                    {
                        S32 at = -1;
                        for (size_t j = 0; j < skin->mJointNames.size(); ++j) if (skin->mJointNames[j] == name) { at = (S32)j; break; }
                        m2.push_back(at);
                    }
                    bm = boneMap.emplace(mi, m2).first;
                }
                const std::array<F32, 4>& pk = sTouch.meshes[mi].packed[vv];
                F32 out[4] = { 0.f, 0.f, 0.f, 0.f };
                bool ok = true;
                for (int k = 0; k < 4; ++k)
                {
                    const S32 j = (S32)floorf(pk[k]);
                    const F32 w = pk[k] - (F32)j;
                    if (w <= 0.f) continue;
                    if (j < 0 || j >= (S32)bm->second.size() || bm->second[j] < 0) { ok = false; break; }
                    out[k] = (F32)bm->second[j] + w;
                }
                if (ok) f->mWeights[i].set(out[0], out[1], out[2], out[3]);
            }
            previewFaceDone(*f);
            touched.insert(v);
        }
        previewRebuild(touched);
    }

    // <MG> Weight view, like Blender's Weight Paint mode: while a weight brush is on, the worn
    // garment shows the chosen bone's weights as colours (blue none .. red all). A colour image
    // is drawn in the garment's own UV layout and shown on it, in this viewer only (the face's
    // look is kept and put back; nothing is sent, nothing is written to the file).

    void weightRamp(F32 t, U8* out)
    {
        F32 r, g, b;
        if (t < 0.25f) { r = 0.f; g = t * 4.f; b = 1.f; }
        else if (t < 0.5f) { r = 0.f; g = 1.f; b = 1.f - (t - 0.25f) * 4.f; }
        else if (t < 0.75f) { r = (t - 0.5f) * 4.f; g = 1.f; b = 0.f; }
        else { r = 1.f; g = 1.f - (t - 0.75f) * 4.f; b = 0.f; }
        out[0] = (U8)llclamp(r * 255.f, 0.f, 255.f); out[1] = (U8)llclamp(g * 255.f, 0.f, 255.f); out[2] = (U8)llclamp(b * 255.f, 0.f, 255.f);
    }

    void weightViewOff()
    {
        if (!sWV.on && sWV.faces.empty()) return;
        std::set<LLVOVolume*> touched;
        for (auto& wf : sWV.faces)
        {
            LLVOVolume* v = previewVolume(wf.prim);
            if (!v || wf.te >= v->getNumTEs()) continue;
            v->setTEColor((U8)wf.te, wf.color);
            v->setTEFullbright((U8)wf.te, wf.fullbright);
            v->setTEScale((U8)wf.te, wf.ss, wf.st);
            v->setTEOffset((U8)wf.te, wf.os, wf.ot);
            v->setTERotation((U8)wf.te, wf.rot);
            v->setTEImage((U8)wf.te, LLViewerTextureManager::getFetchedTexture(wf.tex));
            // the face's own UVs back (if it is still the same volume)
            LLVolume* vol = v->getVolume();
            if (wf.uvApplied && vol && vol == wf.uvVol && wf.te < vol->getNumVolumeFaces())
            {
                LLVolumeFace& f = vol->getVolumeFace(wf.te);
                if (f.mTexCoords && (size_t)f.mNumVertices * 2 == wf.uv0.size())
                    for (S32 i = 0; i < f.mNumVertices; ++i) f.mTexCoords[i].set(wf.uv0[i * 2], wf.uv0[i * 2 + 1]);
            }
            touched.insert(v);
        }
        for (LLVOVolume* v : touched) if (v->mDrawable.notNull()) gPipeline.markRebuild(v->mDrawable, LLDrawable::REBUILD_ALL);
        sWV.faces.clear();
        sWV.on = false;
        sWV.bone.clear();
    }

    // the chosen bone's weight at each worn vertex, drawn into each face's image
    void weightViewDraw()
    {
        const auto& sl = sTouch.skin;
        for (auto& pf : sPreview.faces)
        {
            LLVOVolume* v = nullptr;
            LLVolumeFace* f = previewFace(pf, &v);
            if (!f || !f->mTexCoords || !f->mIndices || pf.face >= v->getNumTEs()) continue;
            // this face's entry (made on first use, the face's look kept)
            WeightView::Face* wf = nullptr;
            for (auto& x : sWV.faces) if (x.prim == pf.prim && x.te == pf.face) wf = &x;
            if (!wf)
            {
                WeightView::Face nf;
                nf.prim = pf.prim; nf.te = pf.face;
                const LLTextureEntry* te = v->getTE((U8)pf.face);
                if (!te) continue;
                nf.tex = te->getID(); nf.color = te->getColor(); nf.fullbright = te->getFullbright();
                te->getScale(&nf.ss, &nf.st); te->getOffset(&nf.os, &nf.ot); nf.rot = te->getRotation();
                nf.raw = new LLImageRaw(WV_SIZE, WV_SIZE, 3);
                sWV.faces.push_back(nf);
                wf = &sWV.faces.back();
            }
            // weight per vertex (the file's points through the preview's matching)
            std::vector<F32> w((size_t)pf.count, 0.f);
            std::map<size_t, S32> boneOf;
            for (S32 i = 0; i < pf.count; ++i)
            {
                const int c = pf.map[i];
                if (c < 0) continue;
                size_t mi = 0;
                while (mi + 1 < sPreview.base.size() && (size_t)c >= sPreview.base[mi + 1]) ++mi;
                const size_t vv = (size_t)c - sPreview.base[mi];
                if (mi >= sl.size() || vv >= sl[mi].weights.size()) continue;
                auto b = boneOf.find(mi);
                if (b == boneOf.end())
                {
                    S32 j = -1;
                    for (size_t k = 0; k < sl[mi].joints.size(); ++k) if (sl[mi].joints[k] == sWV.bone) { j = (S32)k; break; }
                    b = boneOf.emplace(mi, j).first;
                }
                double ww = 0.0, sum = 0.0;
                for (const auto& jw : sl[mi].weights[vv]) { sum += jw.second; if (jw.first == b->second) ww += jw.second; }
                w[i] = sum > 0.0 ? (F32)llclamp(ww / sum, 0.0, 1.0) : 0.f;
            }
            // the face's own UVs (again after a reload: a new volume)
            LLVolume* fvol = v->getVolume();
            if (wf->uvVol != fvol || wf->uv0.size() != (size_t)pf.count * 2)
            {
                wf->uvVol = fvol;
                wf->uv0.resize((size_t)pf.count * 2);
                for (S32 i = 0; i < pf.count; ++i) { wf->uv0[i * 2] = f->mTexCoords[i].mV[0]; wf->uv0[i * 2 + 1] = f->mTexCoords[i].mV[1]; }
                wf->split = -1;
                wf->uvApplied = false;
            }
            // the garment's own UVs (each triangle drawn in the 0..1 tile its middle is in)
            std::vector<F32> U((size_t)pf.count), V((size_t)pf.count);
            for (S32 i = 0; i < pf.count; ++i) { U[i] = wf->uv0[i * 2]; V[i] = wf->uv0[i * 2 + 1]; }
            // raster in UV space: weights blended across each triangle, then coloured (as Blender)
            U8* data = wf->raw->getData();
            const S32 N = WV_SIZE;
            std::vector<U8> cover((size_t)N * N, 0);
            for (S32 p = 0; p < N * N; ++p) weightRamp(0.f, &data[p * 3]);
            for (S32 t = 0; t + 2 < f->mNumIndices; t += 3)
            {
                const S32 ia = f->mIndices[t], ib = f->mIndices[t + 1], ic = f->mIndices[t + 2];
                if (ia >= pf.count || ib >= pf.count || ic >= pf.count) continue;
                F32 u[3] = { U[ia], U[ib], U[ic] };
                F32 q[3] = { V[ia], V[ib], V[ic] };
                // tiled UVs: the whole triangle moved into the 0..1 tile its middle is in
                const F32 du = floorf((u[0] + u[1] + u[2]) / 3.f), dq = floorf((q[0] + q[1] + q[2]) / 3.f);
                F32 x[3], y[3];
                for (int k = 0; k < 3; ++k) { x[k] = (u[k] - du) * N - 0.5f; y[k] = (q[k] - dq) * N - 0.5f; }
                const F32 wt[3] = { w[ia], w[ib], w[ic] };
                const F32 den = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2]);
                if (fabsf(den) < 1e-9f) continue;
                const S32 x0 = llmax(0, (S32)floorf(llmin(x[0], llmin(x[1], x[2])))), x1 = llmin(N - 1, (S32)ceilf(llmax(x[0], llmax(x[1], x[2]))));
                const S32 y0 = llmax(0, (S32)floorf(llmin(y[0], llmin(y[1], y[2])))), y1 = llmin(N - 1, (S32)ceilf(llmax(y[0], llmax(y[1], y[2]))));
                if (x1 - x0 > N || y1 - y0 > N) continue;
                for (S32 py = y0; py <= y1; ++py)
                    for (S32 px = x0; px <= x1; ++px)
                    {
                        const F32 a = ((y[1] - y[2]) * (px - x[2]) + (x[2] - x[1]) * (py - y[2])) / den;
                        const F32 bb = ((y[2] - y[0]) * (px - x[2]) + (x[0] - x[2]) * (py - y[2])) / den;
                        const F32 cc = 1.f - a - bb;
                        if (a < -0.02f || bb < -0.02f || cc < -0.02f) continue;
                        const size_t at = (size_t)py * N + px;
                        weightRamp(llclamp(a * wt[0] + bb * wt[1] + cc * wt[2], 0.f, 1.f), &data[at * 3]);
                        cover[at] = 1;
                    }
            }
            // a few pixels around each UV island, so the island edges do not bleed blue
            for (int pass = 0; pass < 3; ++pass)
            {
                std::vector<U8> next = cover;
                for (S32 py = 0; py < N; ++py)
                    for (S32 px = 0; px < N; ++px)
                    {
                        const size_t at = (size_t)py * N + px;
                        if (cover[at]) continue;
                        static const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
                        for (int k = 0; k < 4; ++k)
                        {
                            const S32 nx = px + DX[k], ny = py + DY[k];
                            if (nx < 0 || ny < 0 || nx >= N || ny >= N) continue;
                            const size_t nb = (size_t)ny * N + nx;
                            if (!cover[nb]) continue;
                            memcpy(&data[at * 3], &data[nb * 3], 3);
                            next[at] = 1;
                            break;
                        }
                    }
                cover.swap(next);
            }
            // shown on the face (the first time: plain white, full bright, no texture offsets)
            if (wf->image.isNull())
            {
                wf->image = LLViewerTextureManager::getLocalTexture(wf->raw.get(), false);
                v->setTEColor((U8)pf.face, LLColor4::white);
                v->setTEFullbright((U8)pf.face, 1);
                v->setTEScale((U8)pf.face, 1.f, 1.f);
                v->setTEOffset((U8)pf.face, 0.f, 0.f);
                v->setTERotation((U8)pf.face, 0.f);
                v->setTEImage((U8)pf.face, wf->image);
                if (v->mDrawable.notNull()) gPipeline.markRebuild(v->mDrawable, LLDrawable::REBUILD_ALL);
            }
            else
            {
                wf->image->createGLTexture(0, wf->raw.get());
                if (v->getTEImage((U8)pf.face) != wf->image.get()) v->setTEImage((U8)pf.face, wf->image);
            }
        }
    }

    void weightViewTick()
    {
        // the bone shown: Paint / Smooth weights the one chosen; Stiff the one its area follows
        const std::string showBone = sTouch.brush == TB_STIFF && !sTouch.stiffBone.empty() ? sTouch.stiffBone : sTouch.bone;
        const bool want = sTouch.active && weightBrush(sTouch.brush) && sTouch.live && sPreview.on && sPreview.touch && !showBone.empty();
        if (!want) { if (sWV.on) weightViewOff(); return; }
        if (sWV.on && !sWV.dirty && sWV.bone == showBone) return;
        if (sWV.timer.getElapsedTimeF32() < 0.1f) return;
        sWV.timer.reset();
        if (!previewEnsure()) { weightViewOff(); return; }
        sWV.bone = showBone;
        sWV.dirty = false;
        weightViewDraw();
        sWV.on = !sWV.faces.empty();
    }

    // the mouse ray against the cloth as it is now
    // which points face the camera: the cloth's far side is neither drawn nor painted
    void drapeFacing(const std::vector<float>& X, std::vector<uint8_t>& vis)
    {
        const size_t n = X.size() / 3;
        vis.assign(n, 1);
        if (!sDrape.job) return;
        std::vector<F32> N(n * 3, 0.f);
        for (const auto& t : sDrape.job->tris)
        {
            const F32* a = &X[t[0] * 3]; const F32* b = &X[t[1] * 3]; const F32* c = &X[t[2] * 3];
            const F32 e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            const F32 nn[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            for (int k = 0; k < 3; ++k) for (int q = 0; q < 3; ++q) N[t[k] * 3 + q] += nn[q];
        }
        const LLVector3 o = LLViewerCamera::getInstance()->getOrigin();
        for (size_t i = 0; i < n; ++i)
        {
            const F32* nn = &N[i * 3];
            if (nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2] < 1e-20f) continue;
            const F32 s = nn[0] * (o.mV[0] - X[i * 3]) + nn[1] * (o.mV[1] - X[i * 3 + 1]) + nn[2] * (o.mV[2] - X[i * 3 + 2]);
            vis[i] = s * sDrape.outward >= 0.f ? 1 : 0;
        }
    }

    bool drapePick(S32 x, S32 y, LLVector3& hit, LLVector3* dirOut = nullptr, int* triOut = nullptr)
    {
        std::vector<float> X;
        if (!drapeSnap(X)) return false;
        const LLVector3 o = LLViewerCamera::getInstance()->getOrigin();
        LLVector3 d = gViewerWindow->mouseDirectionGlobal(x, y);
        d.normVec();
        if (dirOut) *dirOut = d;
        F32 bestT = 1e9f;
        int bestTri = -1;
        const auto& tris = sDrape.job->tris;
        for (size_t ti = 0; ti < tris.size(); ++ti)
        {
            const auto& tr = tris[ti];
            Tri t{ LLVector3(&X[tr[0] * 3]), LLVector3(&X[tr[1] * 3]), LLVector3(&X[tr[2] * 3]) };
            F32 dist;
            if (!rayTri(o, d, t, dist) || dist <= 0.f || dist >= bestT) continue;
            bestT = dist;
            bestTri = (int)ti;
        }
        if (bestTri < 0) return false;
        hit = o + d * bestT;
        if (triOut) *triOut = bestTri;
        return true;
    }

    // the points under the brush: spread along the cloth from the triangle hit (the back panel,
    // a few cm behind, is not reached) and facing the camera
    std::vector<int> drapeNear(const LLVector3& at, F32 r, std::vector<float>* offsets = nullptr, int seedTri = -1)
    {
        std::vector<int> out;
        std::vector<float> X;
        if (!drapeSnap(X) || !sDrape.job) return out;
        const size_t n = X.size() / 3;
        std::vector<uint8_t> vis;
        drapeFacing(X, vis);
        const F32 r2 = r * r;
        auto take = [&](size_t i)
        {
            const F32 dx = X[i * 3] - at.mV[0], dy = X[i * 3 + 1] - at.mV[1], dz = X[i * 3 + 2] - at.mV[2];
            if (dx * dx + dy * dy + dz * dz >= r2) return false;
            out.push_back((int)i);
            // <MG> the point's offset from the hit (target = mouse + offset); it was the other way round,
            // which mirrored the grabbed patch through the mouse on the first move (a fold, a tear)
            if (offsets) { offsets->push_back(dx); offsets->push_back(dy); offsets->push_back(dz); }
            return true;
        };
        const auto& tris = sDrape.job->tris;
        if (seedTri >= 0 && seedTri < (int)tris.size() && sDrape.nb.size() == n)
        {
            std::vector<uint8_t> seen(n, 0);
            std::vector<int> todo;
            for (int k = 0; k < 3; ++k) { const int v = tris[seedTri][k]; if (!seen[v]) { seen[v] = 1; todo.push_back(v); } }
            for (size_t q = 0; q < todo.size(); ++q)
            {
                const int i = todo[q];
                const bool seed = q < 3;
                if (!seed && !vis[i]) continue;
                if (!take((size_t)i) && !seed) continue;
                for (int j : sDrape.nb[i]) if (!seen[j]) { seen[j] = 1; todo.push_back(j); }
            }
            return out;
        }
        for (size_t i = 0; i < n; ++i) if (vis[i]) take(i);
        return out;
    }

    class MGToolDrape : public LLTool
    {
    public:
        MGToolDrape() : LLTool(std::string("MGDrape")) {}
        bool handleMouseDown(S32 x, S32 y, MASK mask) override
        {
            if (mask & MASK_ALT) return LLToolCamera::getInstance()->handleMouseDown(x, y, mask);
            if (!sDrape.active) return false;
            LLVector3 dir;
            if (!drapePick(x, y, sDrape.hit, &dir, &sDrape.hitTri)) return true;
            sDrape.stroking = true;
            sDrape.invert = (mask & MASK_SHIFT) != 0;
            sDrape.painted.clear();
            setMouseCapture(true);
            if (sDrape.tool == 1)
            {
                // grab: the points under the brush follow the mouse in the camera's plane
                sDrape.planeN = -dir;
                sDrape.grabOff.clear();
                sDrape.grabSet = drapeNear(sDrape.hit, sDrape.radius, &sDrape.grabOff, sDrape.hitTri);
            }
            else if (sDrape.tool == 4)
            {
                // <MG> move pins: only the pinned points under the brush (shown yellow while dragged) follow
                // the mouse in the camera's plane, 10 cm per drag at most, and stay pinned there; the cloth
                // hangs from them. (Taking pins farther away, with no limit, tore the cloth into spikes.)
                sDrape.planeN = -dir;
                std::vector<float> X;
                std::vector<uint8_t> P;
                sDrape.grabSet.clear(); sDrape.grabOff.clear();
                if (drapeSnap(X, &P))
                {
                    const F32 r = sDrape.radius;
                    for (size_t i = 0; i < P.size(); ++i)
                    {
                        if (!P[i]) continue;
                        const LLVector3 p(&X[i * 3]);
                        if ((p - sDrape.hit).lengthSquared() >= r * r) continue;
                        sDrape.grabSet.push_back((int)i);
                        for (int q = 0; q < 3; ++q) sDrape.grabOff.push_back(p.mV[q] - sDrape.hit.mV[q]);
                    }
                }
                if (sDrape.grabSet.empty()) push(LLSD().with("type", "lab_drape_hint").with("hint", "no_pins"));
            }
            else dab();
            sDrape.dabTimer.reset();
            return true;
        }
        void dab()
        {
            std::vector<int> pts = drapeNear(sDrape.hit, sDrape.radius, nullptr, sDrape.hitTri);
            if (pts.empty()) return;
            DrapeCmd c;
            if (sDrape.tool == 0) c.kind = sDrape.invert ? DrapeCmd::UNPIN : DrapeCmd::PIN;
            else if (sDrape.tool == 3) { c.kind = DrapeCmd::SMOOTH; c.value = sDrape.invert ? 0.15f : 0.4f; }
            else { c.kind = DrapeCmd::TIGHTEN; c.value = sDrape.invert ? 1.015f : 0.985f; }
            c.pts = pts;
            drapeSend(c);
            sDrape.painted.insert(sDrape.painted.end(), pts.begin(), pts.end());
        }
        bool handleHover(S32 x, S32 y, MASK mask) override
        {
            if (!sDrape.active) return false;
            if (!sDrape.stroking)
            {
                LLVector3 h;
                sDrape.hasHit = drapePick(x, y, h, nullptr, &sDrape.hitTri);
                if (sDrape.hasHit) sDrape.hit = h;
                gViewerWindow->setCursor(sDrape.hasHit ? UI_CURSOR_CROSS : UI_CURSOR_ARROW);
                return true;
            }
            if (sDrape.tool == 1 || sDrape.tool == 4)
            {
                const LLVector3 o = LLViewerCamera::getInstance()->getOrigin();
                LLVector3 d = gViewerWindow->mouseDirectionGlobal(x, y);
                d.normVec();
                const F32 den = d * sDrape.planeN;
                if (fabsf(den) > 1e-6f && !sDrape.grabSet.empty())
                {
                    const F32 t = ((sDrape.hit - o) * sDrape.planeN) / den;
                    if (t > 0.f)
                    {
                        LLVector3 cur = o + d * t;
                        if (sDrape.tool == 4)
                        {
                            // pins: 10 cm per drag at most (they never give way: the cloth would be torn)
                            LLVector3 dv = cur - sDrape.hit;
                            if (dv.length() > 0.10f) dv *= 0.10f / dv.length();
                            cur = sDrape.hit + dv;
                        }
                        DrapeCmd c;
                        c.kind = sDrape.tool == 4 ? DrapeCmd::MOVEPINS : DrapeCmd::GRAB;
                        c.value = 0.5f;
                        c.pts = sDrape.grabSet;
                        for (size_t k = 0; k < sDrape.grabSet.size(); ++k)
                            for (int q = 0; q < 3; ++q) c.targets.push_back(cur.mV[q] + sDrape.grabOff[k * 3 + q]);
                        drapeSend(c);
                    }
                }
            }
            else if (sDrape.dabTimer.getElapsedTimeF32() > 0.05f)
            {
                LLVector3 h;
                if (drapePick(x, y, h, nullptr, &sDrape.hitTri)) { sDrape.hit = h; dab(); }
                sDrape.dabTimer.reset();
            }
            gViewerWindow->setCursor(UI_CURSOR_CROSS);
            return true;
        }
        void endStroke()
        {
            if (!sDrape.stroking) return;
            sDrape.stroking = false;
            DrapeCmd c;
            if (sDrape.tool == 1) { c.kind = DrapeCmd::RELEASE; c.pts = sDrape.grabSet; }   // it keeps where it was put
            else if (sDrape.tool == 2 || sDrape.tool == 3) { c.kind = DrapeCmd::REBASE; c.pts = sDrape.painted; }   // tightened / smoothed: kept so
            else return;
            drapeSend(c);
            sDrape.grabSet.clear();
        }
        bool handleMouseUp(S32 x, S32 y, MASK mask) override
        {
            if (!sDrape.stroking) return false;
            setMouseCapture(false);
            endStroke();
            return true;
        }
        void onMouseCaptureLost() override { endStroke(); }
        void draw() override
        {
            if (!sDrape.active) return;
            std::vector<float> X;
            std::vector<uint8_t> P;
            if (!drapeSnap(X, &P)) return;
            LLViewerCamera* cam = LLViewerCamera::getInstance();
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            const size_t n = X.size() / 3;
            const size_t stepN = n > 8000 ? n / 8000 + 1 : 1;
            std::vector<uint8_t> vis;
            drapeFacing(X, vis);
            // <MG> the pins being moved: yellow
            std::vector<uint8_t> moving(n, 0);
            if (sDrape.stroking && sDrape.tool == 4) for (int g : sDrape.grabSet) if (g >= 0 && (size_t)g < n) moving[g] = 1;
            for (size_t i = 0; i < n; ++i)
            {
                const bool pinned = i < P.size() && P[i];
                if (!vis[i] && !moving[i]) continue;   // the far side: hidden (else the cloth looks see-through)
                if (!pinned && sPreview.on) continue;   // the garment itself shows the cloth
                if (!pinned && i % stepN) continue;
                LLCoordGL c;
                if (!cam->projectPosAgentToScreen(LLVector3(&X[i * 3]), c, false)) continue;
                if (moving[i]) gGL.color4f(1.f, 0.95f, 0.3f, 1.f);
                else if (pinned) gGL.color4f(1.f, 0.55f, 0.1f, 1.f);
                else gGL.color4f(0.92f, 0.97f, 1.f, 0.8f);
                gl_circle_2d((F32)c.mX, (F32)c.mY, pinned ? 2.4f : 1.6f, 6, true);
            }
            if (sDrape.hasHit || sDrape.stroking)
            {
                LLCoordGL c, e;
                const LLVector3 edge = sDrape.hit + cam->getLeftAxis() * sDrape.radius;
                if (cam->projectPosAgentToScreen(sDrape.hit, c, false) && cam->projectPosAgentToScreen(edge, e, false))
                {
                    const F32 rpx = llmax(4.f, sqrtf((F32)((e.mX - c.mX) * (e.mX - c.mX) + (e.mY - c.mY) * (e.mY - c.mY))));
                    gGL.color4f(0.f, 0.f, 0.f, 0.5f);
                    gl_circle_2d((F32)c.mX, (F32)c.mY, rpx + 1.f, 48, false);
                    if (sDrape.tool == 0 || sDrape.tool == 4) gGL.color4f(1.f, 0.55f, 0.1f, 1.f);
                    else if (sDrape.tool == 2) gGL.color4f(0.75f, 0.45f, 1.f, 1.f);
                    else if (sDrape.tool == 3) gGL.color4f(0.3f, 0.95f, 0.55f, 1.f);
                    else gGL.color4f(0.f, 0.83f, 1.f, 1.f);
                    gl_circle_2d((F32)c.mX, (F32)c.mY, rpx, 48, false);
                }
            }
        }
    };


    // the worn body in the current pose (agent space), triangles turned outward
    void drapeBody(std::vector<float>& P, std::vector<std::array<int, 3>>& T, const std::set<LLUUID>& garment, std::vector<std::string>* names = nullptr)
    {
        for (const auto& kv : gAgentAvatarp->mAttachmentPoints)
        {
            LLViewerJointAttachment* att = kv.second;
            if (!att || att->getIsHUDAttachment()) continue;
            for (const auto& ao : att->mAttachedObjects)
            {
                LLViewerObject* root = ao.get();
                if (!root || root->isDead() || sBodyOff.count(root->getAttachmentItemID())) continue;
                const size_t before = T.size();
                for (LLViewerObject* o : primsOf(root))
                {
                    if (!o || o->getPCode() != LL_PCODE_VOLUME || garment.count(o->getID())) continue;
                    LLVOVolume* v = static_cast<LLVOVolume*>(o);
                    if (v->mIsLocalMesh || !v->isRiggedMesh()) continue;
                    LLVolume* src = v->getVolume();
                    if (!v->getSkinInfo() || !src) continue;
                    v->updateRiggedVolume(true, LLRiggedVolume::UPDATE_ALL_FACES, false);
                    LLRiggedVolume* rv = v->getRiggedVolume();
                    if (!rv || rv->getNumVolumeFaces() != src->getNumVolumeFaces()) continue;
                    for (S32 f = 0; f < rv->getNumVolumeFaces(); ++f)
                    {
                        const LLTextureEntry* te = v->getTE((U8)f);
                        if (te && (te->getColor().mV[VALPHA] < 0.05f || te->getID() == IMG_INVISIBLE)) continue;
                        const LLVolumeFace& sf = src->getVolumeFace(f);
                        const LLVolumeFace& df = rv->getVolumeFace(f);
                        if (!df.mPositions || !df.mIndices || df.mNumVertices != sf.mNumVertices) continue;
                        const F32 wind = windingSign(sf);
                        const int base = (int)(P.size() / 3);
                        for (S32 i = 0; i < df.mNumVertices; ++i)
                        {
                            const F32* q = df.mPositions[i].getF32ptr();
                            P.push_back(q[0]); P.push_back(q[1]); P.push_back(q[2]);
                        }
                        for (S32 i = 0; i + 2 < df.mNumIndices; i += 3)
                        {
                            const int a = base + df.mIndices[i], b = base + df.mIndices[i + 1], c = base + df.mIndices[i + 2];
                            T.push_back(wind < 0.f ? std::array<int, 3>{ a, c, b } : std::array<int, 3>{ a, b, c });
                        }
                    }
                }
                if (names && T.size() > before) names->push_back(itemName(root));
            }
        }
    }

    // Skin to my body: the worn body (the pieces left on in the Stress test, visible faces) as
    // a weight source in memory: bind-space points (bind shape applied), its weights and bones.
    // Only weights are copied from it; its geometry is never written anywhere.
    std::string myBodyFile(MGCreatorDae::File& out, const std::set<LLUUID>& garment)
    {
        out = MGCreatorDae::File();
        if (!isAgentAvatarValid()) return "avatar";
        out.path = "(worn body)";
        out.upAxis = "Z_UP";
        out.meter = 1.0;
        static const std::array<double, 16> IDENT_M = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        size_t total = 0;
        for (const auto& kv : gAgentAvatarp->mAttachmentPoints)
        {
            LLViewerJointAttachment* att = kv.second;
            if (!att || att->getIsHUDAttachment()) continue;
            for (const auto& ao : att->mAttachedObjects)
            {
                LLViewerObject* root = ao.get();
                if (!root || root->isDead() || sBodyOff.count(root->getAttachmentItemID())) continue;
                for (LLViewerObject* o : primsOf(root))
                {
                    if (!o || o->getPCode() != LL_PCODE_VOLUME || garment.count(o->getID())) continue;
                    LLVOVolume* v = static_cast<LLVOVolume*>(o);
                    if (v->mIsLocalMesh || !v->isRiggedMesh()) continue;
                    const LLMeshSkinInfo* skin = v->getSkinInfo();
                    LLVolume* src = v->getVolume();
                    if (!skin || !src || skin->mJointNames.empty()) continue;
                    const size_t nj = skin->mJointNames.size();
                    const glm::mat4 bs = glm::make_mat4(skin->mBindShapeMatrix.getF32ptr());
                    for (S32 f = 0; f < src->getNumVolumeFaces(); ++f)
                    {
                        const LLTextureEntry* te = v->getTE((U8)f);
                        if (te && (te->getColor().mV[VALPHA] < 0.05f || te->getID() == IMG_INVISIBLE)) continue;
                        const LLVolumeFace& sf = src->getVolumeFace(f);
                        if (!sf.mPositions || !sf.mIndices || !sf.mWeights || sf.mNumVertices <= 0) continue;
                        MGCreatorDae::SkinnedMesh sm;
                        sm.name = o->getID().asString() + "/" + std::to_string(f);
                        sm.bindShape = IDENT_M;
                        sm.joints = skin->mJointNames;
                        sm.invBind.resize(nj);
                        for (size_t j = 0; j < nj; ++j)
                        {
                            // LL keeps matrices column-major; the COLLADA layout is row-major
                            const F32* p = j < skin->mInvBindMatrix.size() ? skin->mInvBindMatrix[j].getF32ptr() : nullptr;
                            for (int r = 0; r < 4; ++r) for (int k = 0; k < 4; ++k) sm.invBind[j][r * 4 + k] = p ? (double)p[k * 4 + r] : (r == k ? 1.0 : 0.0);
                        }
                        sm.positions.resize((size_t)sf.mNumVertices * 3);
                        sm.weights.resize((size_t)sf.mNumVertices);
                        for (S32 i = 0; i < sf.mNumVertices; ++i)
                        {
                            const F32* q = sf.mPositions[i].getF32ptr();
                            const glm::vec4 b = bs * glm::vec4(q[0], q[1], q[2], 1.f);
                            sm.positions[i * 3] = b.x; sm.positions[i * 3 + 1] = b.y; sm.positions[i * 3 + 2] = b.z;
                            // packed: joint index + weight per influence
                            const F32* w = sf.mWeights[i].getF32ptr();
                            double sum = 0;
                            for (int k = 0; k < 4; ++k)
                            {
                                const F32 jf = floorf(w[k]);
                                const double wt = (double)(w[k] - jf);
                                const int j = (int)jf;
                                if (wt <= 0.0 || j < 0 || j >= (int)nj) continue;
                                sm.weights[i].push_back({ j, wt });
                                sum += wt;
                            }
                            if (sum > 0) for (auto& jw : sm.weights[i]) jw.second /= sum;
                        }
                        for (S32 i = 0; i + 2 < sf.mNumIndices; i += 3)
                        {
                            const int a = sf.mIndices[i], b = sf.mIndices[i + 1], cc = sf.mIndices[i + 2];
                            if (a >= sf.mNumVertices || b >= sf.mNumVertices || cc >= sf.mNumVertices) continue;
                            sm.triangles.push_back({ a, b, cc });
                        }
                        total += (size_t)sf.mNumVertices;
                        out.meshes.push_back(std::move(sm));
                    }
                }
            }
        }
        if (out.meshes.empty() || total == 0) return "my_body";
        {
            double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
            size_t tris = 0;
            for (const auto& m : out.meshes)
            {
                tris += m.triangles.size();
                for (size_t v = 0; v < m.vertexCount(); ++v) for (int q = 0; q < 3; ++q) { lo[q] = std::min(lo[q], m.positions[v * 3 + q]); hi[q] = std::max(hi[q], m.positions[v * 3 + q]); }
            }
            LL_INFOS("MGCreator") << "Skin to my body: " << out.meshes.size() << " faces, " << total << " points, " << tris << " triangles; bind space "
                                  << lo[0] << "," << lo[1] << "," << lo[2] << " .. " << hi[0] << "," << hi[1] << "," << hi[2] << LL_ENDL;
        }
        return std::string();
    }

    // the worn body as a weight source on SL's skeleton as it stands (bones: SL's rest positions)
    std::string myBodyOnSkeleton(MGCreatorDae::File& out, std::map<std::string, std::array<double, 3>>& bones,
                                 std::vector<MGCreatorWeights::CollisionVolume>& cvs)
    {
        std::set<LLUUID> garment;
        for (const auto& a : sApplied) garment.insert(a.prim);
        const std::string e = myBodyFile(out, garment);
        if (!e.empty()) return e;
        const std::string skel = gDirUtilp->getExpandedFilename(LL_PATH_CHARACTER, "avatar_skeleton.xml");
        std::vector<MGRig::SlBone> sl;
        if (!MGRig::loadSlSkeleton(skel, sl)) return "skeleton";
        bones.clear();
        for (const auto& b : sl) if (!b.volume) bones[b.name] = { b.world[0], b.world[1], b.world[2] };
        if (cvs.empty()) MGCreatorWeights::loadCollisionVolumes(skel, cvs);
        MGCreatorWeights::toSkeletonSpace(out, bones, cvs);
        return std::string();
    }

    // start: the worn file skinned as SL draws it now, the body around it, the thread
    std::string drapeStart(const std::string& wornPath, const MGDrape::Settings& settings)
    {
        lodRestore();
        drapeStop();
        touchStop();
        placeEnd(true);
        if (!isAgentAvatarValid()) return "avatar";
        std::string orig = wornPath;
        const size_t dot = orig.find_last_of('.');
        if (dot != std::string::npos && dot >= 4 && orig.compare(dot - 4, 4, "_fit") == 0) orig = orig.substr(0, dot - 4) + orig.substr(dot);
        sDrape.outPath = MGCreatorDae::fitPathFor(orig);
        if (!MGCreatorDae::read(wornPath, sDrape.dae)) return sDrape.dae.error.empty() ? std::string("read") : sDrape.dae.error;
        auto job = std::make_shared<DrapeJob>();
        job->settings = settings;
        size_t total = 0;
        for (const auto& m : sDrape.dae.meshes) total += m.vertexCount();
        if (total == 0) return "no_rigged_mesh";
        if (total > 60000) return "too_big";
        std::vector<std::vector<U8>> mask(sDrape.dae.meshes.size());
        for (size_t mi = 0; mi < mask.size(); ++mi) mask[mi].assign(sDrape.dae.meshes[mi].vertexCount(), 0);
        loadMask(maskPathFor(sDrape.outPath), mask);
        sDrape.base.clear();
        sDrape.toFile.clear();
        sDrape.skins.assign(sDrape.dae.meshes.size(), FitMesh());
        sDrape.mNow.clear();
        for (size_t mi = 0; mi < sDrape.dae.meshes.size(); ++mi)
        {
            const MGCreatorDae::SkinnedMesh& m = sDrape.dae.meshes[mi];
            FitMesh& fm = sDrape.skins[mi];
            if (!makeSkin(m, fm)) return "bones";
            const S32 count = (S32)fm.skin->mJointNames.size();
            LLMatrix4a mat[LL_MAX_JOINTS_PER_MESH_OBJECT];
            LLSkinningUtil::initSkinningMatrixPalette(mat, count, fm.skin.get(), gAgentAvatarp);
            const glm::mat4 bs = glm::make_mat4(fm.skin->mBindShapeMatrix.getF32ptr());
            const size_t base = job->start.size() / 3;
            sDrape.base.push_back(base);
            for (size_t v = 0; v < m.vertexCount(); ++v)
            {
                LLMatrix4a fin;
                std::array<F32, 4> pk = fm.packed[v];
                LLSkinningUtil::getPerVertexSkinMatrix(pk.data(), mat, true, fin, count);
                const glm::mat4 M = glm::make_mat4(fin.getF32ptr()) * bs;
                const glm::vec4 w = M * glm::vec4((F32)m.positions[v * 3], (F32)m.positions[v * 3 + 1], (F32)m.positions[v * 3 + 2], 1.f);
                job->start.push_back(w.x); job->start.push_back(w.y); job->start.push_back(w.z);
                sDrape.toFile.push_back(glm::inverse(M));
                sDrape.mNow.push_back(M);
                job->protect.push_back(mask[mi][v]);
            }
            for (const auto& t : m.triangles)
            {
                if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= (int)m.vertexCount() || t[1] >= (int)m.vertexCount() || t[2] >= (int)m.vertexCount()) continue;
                job->tris.push_back({ (int)base + t[0], (int)base + t[1], (int)base + t[2] });
            }
        }
        std::set<LLUUID> garment;
        for (const auto& a : sApplied) garment.insert(a.prim);
        sDrape.bodyNames.clear();
        drapeBody(job->body, job->bodyTris, garment, &sDrape.bodyNames);
        if (job->bodyTris.empty()) return "no_body";
        // the brush spreads along the cloth: each point's neighbours
        const size_t np = job->start.size() / 3;
        sDrape.nb.assign(np, {});
        for (const auto& t : job->tris)
            for (int k = 0; k < 3; ++k) { sDrape.nb[t[k]].push_back(t[(k + 1) % 3]); sDrape.nb[t[(k + 1) % 3]].push_back(t[k]); }
        for (auto& l : sDrape.nb) { std::sort(l.begin(), l.end()); l.erase(std::unique(l.begin(), l.end()), l.end()); }
        // which way the triangles face: away from the garment's middle (around the body) or towards it
        {
            F64 cx = 0, cy = 0, cz = 0;
            for (size_t i = 0; i < np; ++i) { cx += job->start[i * 3]; cy += job->start[i * 3 + 1]; cz += job->start[i * 3 + 2]; }
            cx /= (F64)np; cy /= (F64)np; cz /= (F64)np;
            std::vector<F32> N(np * 3, 0.f);
            for (const auto& t : job->tris)
            {
                const F32* a = &job->start[t[0] * 3]; const F32* b = &job->start[t[1] * 3]; const F32* c = &job->start[t[2] * 3];
                const F32 e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
                const F32 nn[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                for (int k = 0; k < 3; ++k) for (int q = 0; q < 3; ++q) N[t[k] * 3 + q] += nn[q];
            }
            F64 sum = 0;
            for (size_t i = 0; i < np; ++i)
                sum += N[i * 3] * (job->start[i * 3] - cx) + N[i * 3 + 1] * (job->start[i * 3 + 1] - cy) + N[i * 3 + 2] * (job->start[i * 3 + 2] - cz);
            sDrape.outward = sum < 0 ? -1.f : 1.f;
        }
        sDrape.hitTri = -1;
        {
            std::lock_guard<std::mutex> g(job->lock);
            job->snap = job->start;
        }
        sDrape.skinnedNow = job->start;
        sDrape.followTimer.reset();
        sPreview.retries = 0;
        previewBuild(job->start, &sDrape.mNow);
        job->worker = std::thread(drapeRun, job);
        sDrape.job = job;
        sDrape.active = true;
        if (!sDrapeTool) sDrapeTool = new MGToolDrape();
        LLToolMgr::getInstance()->setTransientTool(sDrapeTool);
        return std::string();
    }

    // Apply: the cloth back in the file's space, written to the _fit copy and worn
    LLSD drapeApply()
    {
        std::vector<float> X;
        drapeFollow(true);   // the file's space through the pose now
        if (!sDrape.active || !drapeSnap(X)) return LLSD().with("ok", false).with("error", "inactive");
        std::vector<std::vector<double>> pos(sDrape.dae.meshes.size());
        for (size_t mi = 0; mi < sDrape.dae.meshes.size(); ++mi)
        {
            const size_t n = sDrape.dae.meshes[mi].vertexCount(), base = sDrape.base[mi];
            pos[mi].resize(n * 3);
            for (size_t v = 0; v < n; ++v)
            {
                const glm::vec4 f = sDrape.toFile[base + v] * glm::vec4(X[(base + v) * 3], X[(base + v) * 3 + 1], X[(base + v) * 3 + 2], 1.f);
                // a point whose skinning cannot be undone keeps where it was
                if (!std::isfinite(f.x) || !std::isfinite(f.y) || !std::isfinite(f.z))
                    for (int q = 0; q < 3; ++q) pos[mi][v * 3 + q] = sDrape.dae.meshes[mi].positions[v * 3 + q];
                else { pos[mi][v * 3] = f.x; pos[mi][v * 3 + 1] = f.y; pos[mi][v * 3 + 2] = f.z; }
            }
        }
        std::string err;
        if (!MGCreatorDae::write(sDrape.dae, pos, {}, sDrape.outPath, err)) return LLSD().with("ok", false).with("error", err.empty() ? std::string("write") : err);
        const std::string out = sDrape.outPath;
        const LLUUID standin = sStandIn;
        drapeStop();
        LLSD r = openPath(out);
        if (r["ok"].asBoolean())
        {
            const LLUUID id(r["id"].asString());
            if (id != sWornFile && standin.notNull()) { sFit.pendingFile = id; sFit.standin = standin; sFit.pendingTimer.reset(); }
        }
        return LLSD().with("ok", true).with("name", baseName(out));
    }

    MGDrape::Settings drapeSettings(const LLSD& a, MGDrape::Settings s)
    {
        if (a.has("weight")) s.gravity = llclamp((F32)a["weight"].asReal(), 0.f, 200.f) / 100.f;
        if (a.has("hold")) s.hold = llclamp((F32)a["hold"].asReal(), 0.f, 100.f) / 200.f;
        if (a.has("stretch")) s.stretch = 0.5f + llclamp((F32)a["stretch"].asReal(), 0.f, 100.f) / 100.f * 0.499f;
        if (a.has("bend")) s.bend = llclamp((F32)a["bend"].asReal(), 0.f, 100.f) / 100.f * 0.6f;
        if (a.has("thickness_mm")) s.thickness = llclamp((F32)a["thickness_mm"].asReal(), 0.5f, 15.f) / 1000.f;
        if (a.has("friction")) s.friction = llclamp((F32)a["friction"].asReal(), 0.f, 100.f) / 100.f * 0.9f;
        if (a.has("self")) s.selfCollide = a["self"].asBoolean();
        if (a.has("follow")) sDrape.follow = a["follow"].asBoolean();
        return s;
    }

    void touchStop()
    {
        if (!sTouch.active) return;
        if (sTouch.stroking) touchStrokeEnd();
        // <MG> Edit Mesh: a drag in progress ends; hidden faces come back on the worn garment
        if (sEdit.down) { sEdit.down = false; if (sEdit.dragging) { sEdit.dragging = false; editCommit(); } sEdit.box = false; }
        for (auto& h : sTouch.hidden) std::fill(h.begin(), h.end(), (U8)0);
        editApplyHidden();
        sTouch.sel.clear();
        sTouch.hidden.clear();
        sTouch.editOut = 0.f;
        sTouch.active = false;
        weightViewOff();
        if (sPreview.touch) previewDrop();
        sTouch.live = false;
        if (sTouchTool && LLToolMgr::getInstance()->getCurrentTool() == sTouchTool) LLToolMgr::getInstance()->clearTransientTool();
        sTouch.dae = MGCreatorDae::File();
        sTouch.meshes.clear();
        sTouch.pos.clear();
        sTouch.world.clear();
        sTouch.wnorm.clear();
        sTouch.finals.clear();
        sTouch.undo.clear();
        sTouch.redo.clear();
        sTouch.skin.clear();
        sTouch.skinEdited = false;
    }

    std::string touchStart(const std::string& wornPath)
    {
        lodRestore();
        touchStop();
        drapeStop();
        placeEnd(true);
        // the edits go to the _fit copy, next to the original
        std::string orig = wornPath;
        const size_t dot = orig.find_last_of('.');
        if (dot != std::string::npos && dot >= 4 && orig.compare(dot - 4, 4, "_fit") == 0) orig = orig.substr(0, dot - 4) + orig.substr(dot);
        sTouch.outPath = MGCreatorDae::fitPathFor(orig);
        sTouch.maskPath = maskPathFor(sTouch.outPath);
        // edit what is worn (the _fit copy if it is the one, else the original)
        if (!MGCreatorDae::read(wornPath, sTouch.dae)) return sTouch.dae.error.empty() ? std::string("read") : sTouch.dae.error;
        if (!sTouch.dae.upAxis.empty() && sTouch.dae.upAxis != "Z_UP") return "axis";
        if (std::fabs(sTouch.dae.meter - 1.0) > 1e-6) return "unit";
        const size_t nm = sTouch.dae.meshes.size();
        sTouch.meshes.assign(nm, FitMesh());
        sTouch.pos.resize(nm);
        sTouch.nb.assign(nm, {});
        sTouch.mask.assign(nm, {});
        sTouch.mirror.assign(nm, {});
        sTouch.world.assign(nm, {});
        sTouch.wnorm.assign(nm, {});
        sTouch.finals.assign(nm, {});
        for (size_t mi = 0; mi < nm; ++mi)
        {
            const MGCreatorDae::SkinnedMesh& m = sTouch.dae.meshes[mi];
            if (!makeSkin(m, sTouch.meshes[mi])) return "bones";
            sTouch.pos[mi] = m.positions;
            const size_t n = m.vertexCount();
            sTouch.mask[mi].assign(n, 0);
            auto& nb = sTouch.nb[mi];
            nb.assign(n, {});
            for (const auto& t : m.triangles)
            {
                for (S32 k = 0; k < 3; ++k)
                {
                    const S32 a = t[k], b = t[(k + 1) % 3];
                    if (a < 0 || b < 0 || a >= (S32)n || b >= (S32)n) continue;
                    nb[a].push_back(b);
                    nb[b].push_back(a);
                }
            }
            for (auto& l : nb) { std::sort(l.begin(), l.end()); l.erase(std::unique(l.begin(), l.end()), l.end()); }
            // left/right partners: the point nearest the mirrored position (2 mm)
            Grid g;
            g.cell = 0.004f;
            for (size_t v = 0; v < n; ++v)
                g.map[Grid::key(g.c((F32)m.positions[v * 3]), g.c((F32)m.positions[v * 3 + 1]), g.c((F32)m.positions[v * 3 + 2]))].push_back((S32)v);
            sTouch.mirror[mi].assign(n, -1);
            for (size_t v = 0; v < n; ++v)
            {
                const LLVector3 q((F32)m.positions[v * 3], -(F32)m.positions[v * 3 + 1], (F32)m.positions[v * 3 + 2]);
                F32 best = 0.002f * 0.002f;
                const S32 cx = g.c(q.mV[0]), cy = g.c(q.mV[1]), cz = g.c(q.mV[2]);
                for (S32 x = cx - 1; x <= cx + 1; ++x)
                    for (S32 y = cy - 1; y <= cy + 1; ++y)
                        for (S32 z = cz - 1; z <= cz + 1; ++z)
                        {
                            auto it = g.map.find(Grid::key(x, y, z));
                            if (it == g.map.end()) continue;
                            for (S32 u : it->second)
                            {
                                const LLVector3 pu((F32)m.positions[u * 3], (F32)m.positions[u * 3 + 1], (F32)m.positions[u * 3 + 2]);
                                const F32 d2 = (pu - q).lengthSquared();
                                if (d2 < best) { best = d2; sTouch.mirror[mi][v] = u; }
                            }
                        }
            }
        }
        loadMask(sTouch.maskPath, sTouch.mask);
        // the weights as the file has them, to paint
        sTouch.skin.assign(nm, MGCreatorDae::NewSkin());
        for (size_t mi = 0; mi < nm; ++mi)
        {
            sTouch.skin[mi].joints = sTouch.dae.meshes[mi].joints;
            sTouch.skin[mi].invBind = sTouch.dae.meshes[mi].invBind;
            sTouch.skin[mi].weights = sTouch.dae.meshes[mi].weights;
        }
        sTouch.skinEdited = false;
        sTouch.active = true;
        sTouch.edits = 0;
        sTouch.undo.clear();
        sTouch.redo.clear();
        touchSkin();
        touchPreviewStart();
        if (!sTouchTool) sTouchTool = new MGToolTouch();
        LLToolMgr::getInstance()->setTransientTool(sTouchTool);
        return std::string();
    }

    // ------------------------------------------------------------------
    // <MG> Look: the garment's colours, textures and materials (Second Life's and PBR), on this
    // screen only: the worn prims' faces are set locally, nothing is sent. Textures are local files,
    // reloaded whenever they are saved (Firestorm's local textures). A PBR material is written as a
    // .gltf in "<garment>_look" next to the garment, with its images copied there (the very file you
    // upload to SL as a material), and shown through Firestorm's local materials. Each garment keeps
    // its look in "<garment>.look.xml" next to it.
    // ------------------------------------------------------------------
    struct LookFace
    {
        size_t prim = 0;               // index in sApplied
        S32 te = 0;
        std::string name;
        LLColor4 color = LLColor4::white;
        F32 glow = 0.f, repeat = 1.f;
        bool fullbright = false;
        S32 shiny = 0;                 // 0 none .. 3 high
        std::string diffuse, normal, specular;
        bool pbr = false;
        std::string baseTex, normTex, ormTex, emisTex;
        std::string opacity;           // a grey opacity map: packed into the colour map's alpha (both modes)
        std::string aoTex, metalTex, roughTex;   // separate maps (as Character Creator has them): packed into one ORM image
        LLColor4 baseColor = LLColor4::white;
        F32 metallic = 0.f, roughness = 0.7f;
        LLColor4 emissive = LLColor4::black;
        S32 alphaMode = 0;             // 0 opaque, 1 blend, 2 mask
        F32 cutoff = 0.5f;
        bool doubleSided = false;
        // runtime
        LLUUID track[3];               // local textures: diffuse, normal, specular
        std::string trackPath[3];
        std::string gltf;
        LLUUID gltfWorld;
        std::map<std::string, std::time_t> stamps;   // the PBR images' times (a save refreshes the material)
    };
    struct Look
    {
        std::string base;              // the garment's path, without "_fit" and the extension
        S32 view = 1;                  // 0 the look, 1 clay (the default: shading shows the details), 2 plain white
        std::vector<LookFace> faces;
        std::vector<LLVolume*> vols;   // per worn prim: its volume when last applied (a reload: again)
        LLUUID worn;
        LLFrameTimer timer;
        S32 flash = -1;
        LLFrameTimer flashTimer;
    } sLook;

    std::time_t fileTime(const std::string& path)
    {
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(std::filesystem::path(ll_convert<std::wstring>(path)), ec);
        if (ec) return 0;
        return (std::time_t)std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
    }
    std::string lookSafe(const std::string& s)
    {
        std::string o;
        for (char c : s) o += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
        return o.empty() ? std::string("face") : o;
    }

    // the material names of each geometry, in Local Mesh's face order (its <triangles>, then its <polylist>)
    std::vector<std::vector<std::string>> lookDaeNames(const std::string& path)
    {
        std::vector<std::vector<std::string>> out;
        std::ifstream in(ll_convert<std::wstring>(path), std::ios::binary);
        if (!in) return out;
        std::string t((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto attr = [&](size_t at, size_t end, const char* name)
        {
            const std::string key = std::string(name) + "=\"";
            const size_t a = t.find(key, at);
            if (a == std::string::npos || a > end) return std::string();
            const size_t b = t.find('"', a + key.size());
            return b == std::string::npos ? std::string() : t.substr(a + key.size(), b - a - key.size());
        };
        auto pretty = [](std::string s)
        {
            for (const char* suf : { "-material", "_material", "-mat" })
            {
                const size_t n = strlen(suf);
                if (s.size() > n && s.compare(s.size() - n, n, suf) == 0) s.resize(s.size() - n);
            }
            for (char& c : s) if (c == '_') c = ' ';
            return s;
        };
        size_t g = 0;
        while ((g = t.find("<geometry", g)) != std::string::npos)
        {
            const size_t ge = t.find("</geometry>", g);
            if (ge == std::string::npos) break;
            std::vector<std::string> names;
            for (const char* kind : { "<triangles", "<polylist", "<polygons" })
            {
                size_t p = g;
                while ((p = t.find(kind, p)) != std::string::npos && p < ge)
                {
                    const size_t tagEnd = t.find('>', p);
                    names.push_back(pretty(attr(p, tagEnd, "material")));
                    p = tagEnd;
                }
            }
            out.push_back(names);
            g = ge;
        }
        return out;
    }

    LLSD lookColorSD(const LLColor4& c) { return LLSD().with(0, c.mV[0]).with(1, c.mV[1]).with(2, c.mV[2]).with(3, c.mV[3]); }
    LLColor4 lookColorOf(const LLSD& s, const LLColor4& d) { return s.isArray() && s.size() >= 3 ? LLColor4((F32)s[0].asReal(), (F32)s[1].asReal(), (F32)s[2].asReal(), s.size() > 3 ? (F32)s[3].asReal() : 1.f) : d; }

    LLSD lookFaceSD(const LookFace& f)
    {
        return LLSD().with("prim", (S32)f.prim).with("te", f.te).with("name", f.name).with("color", lookColorSD(f.color)).with("glow", f.glow)
            .with("repeat", f.repeat).with("fullbright", f.fullbright).with("shiny", f.shiny).with("diffuse", f.diffuse).with("normal", f.normal)
            .with("specular", f.specular).with("pbr", f.pbr).with("base_tex", f.baseTex).with("norm_tex", f.normTex).with("orm_tex", f.ormTex)
            .with("emis_tex", f.emisTex).with("opacity", f.opacity).with("ao_tex", f.aoTex).with("metal_tex", f.metalTex).with("rough_tex", f.roughTex).with("base_color", lookColorSD(f.baseColor)).with("metallic", f.metallic).with("roughness", f.roughness)
            .with("emissive", lookColorSD(f.emissive)).with("alpha_mode", f.alphaMode).with("cutoff", f.cutoff).with("double_sided", f.doubleSided);
    }
    void lookFaceFrom(LookFace& f, const LLSD& s)
    {
        if (s.has("color")) f.color = lookColorOf(s["color"], f.color);
        if (s.has("glow")) f.glow = llclamp((F32)s["glow"].asReal(), 0.f, 1.f);
        if (s.has("repeat")) f.repeat = llclamp((F32)s["repeat"].asReal(), 0.01f, 100.f);
        if (s.has("fullbright")) f.fullbright = s["fullbright"].asBoolean();
        if (s.has("shiny")) f.shiny = llclamp(s["shiny"].asInteger(), 0, 3);
        if (s.has("diffuse")) f.diffuse = s["diffuse"].asString();
        if (s.has("normal")) f.normal = s["normal"].asString();
        if (s.has("specular")) f.specular = s["specular"].asString();
        if (s.has("pbr")) f.pbr = s["pbr"].asBoolean();
        if (s.has("base_tex")) f.baseTex = s["base_tex"].asString();
        if (s.has("norm_tex")) f.normTex = s["norm_tex"].asString();
        if (s.has("orm_tex")) f.ormTex = s["orm_tex"].asString();
        if (s.has("emis_tex")) f.emisTex = s["emis_tex"].asString();
        if (s.has("opacity")) f.opacity = s["opacity"].asString();
        if (s.has("ao_tex")) f.aoTex = s["ao_tex"].asString();
        if (s.has("metal_tex")) f.metalTex = s["metal_tex"].asString();
        if (s.has("rough_tex")) f.roughTex = s["rough_tex"].asString();
        if (s.has("base_color")) f.baseColor = lookColorOf(s["base_color"], f.baseColor);
        if (s.has("metallic")) f.metallic = llclamp((F32)s["metallic"].asReal(), 0.f, 1.f);
        if (s.has("roughness")) f.roughness = llclamp((F32)s["roughness"].asReal(), 0.f, 1.f);
        if (s.has("emissive")) f.emissive = lookColorOf(s["emissive"], f.emissive);
        if (s.has("alpha_mode")) f.alphaMode = llclamp(s["alpha_mode"].asInteger(), 0, 2);
        if (s.has("cutoff")) f.cutoff = llclamp((F32)s["cutoff"].asReal(), 0.f, 1.f);
        if (s.has("double_sided")) f.doubleSided = s["double_sided"].asBoolean();
    }

    void lookSave()
    {
        if (sLook.base.empty()) return;
        LLSD faces = LLSD::emptyArray();
        for (const auto& f : sLook.faces) faces.append(lookFaceSD(f));
        LLSD root = LLSD().with("view", sLook.view).with("faces", faces);
        llofstream out(sLook.base + ".look.xml", std::ios::trunc);
        if (out) LLSDSerialize::toPrettyXML(root, out);
    }

    // the faces of what is worn now (names from the file), with the look saved for this garment
    void lookEnsure()
    {
        if (sWornFile.isNull() || sApplied.empty()) { sLook.worn.setNull(); sLook.faces.clear(); return; }
        if (sLook.worn == sWornFile && !sLook.faces.empty()) return;
        const FileWatch* w = watchOf(sWornFile);
        if (!w) return;
        std::string base = w->path;
        const size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) base.resize(dot);
        if (base.size() > 4 && base.compare(base.size() - 4, 4, "_fit") == 0) base.resize(base.size() - 4);
        sLook.base = base;
        sLook.worn = sWornFile;
        sLook.faces.clear();
        sLook.vols.assign(sApplied.size(), nullptr);
        const auto names = lookDaeNames(w->path);
        for (size_t pi = 0; pi < sApplied.size(); ++pi)
        {
            LLVOVolume* v = previewVolume(sApplied[pi].prim);
            if (!v || !v->getVolume()) continue;
            const S32 nf = llmin(v->getVolume()->getNumVolumeFaces(), (S32)v->getNumTEs());
            for (S32 te = 0; te < nf; ++te)
            {
                LookFace f;
                f.prim = pi; f.te = te;
                f.name = pi < names.size() && te < (S32)names[pi].size() && !names[pi][te].empty() ? names[pi][te] : llformat("Material %d", (S32)sLook.faces.size() + 1);
                sLook.faces.push_back(f);
            }
        }
        // the look saved for this garment (by place, else by name); none yet: clay, so its shape shows
        sLook.view = 1;
        llifstream in(base + ".look.xml");
        LLSD root;
        if (in && LLSDSerialize::fromXML(root, in) != LLSDParser::PARSE_FAILURE)
        {
            sLook.view = llclamp(root["view"].asInteger(), 0, 2);
            for (LLSD::array_const_iterator it = root["faces"].beginArray(); it != root["faces"].endArray(); ++it)
            {
                const LLSD& s = *it;
                for (auto& f : sLook.faces)
                    if (((S32)f.prim == s["prim"].asInteger() && f.te == s["te"].asInteger()) || f.name == s["name"].asString()) { lookFaceFrom(f, s); break; }
            }
        }
    }

    // a local texture's id now (it changes each time the file is saved: asked again every time)
    LLUUID lookBitmap(LookFace& f, int slot, const std::string& path)
    {
        if (path.empty()) return LLUUID::null;
        if (f.track[slot].isNull() || f.trackPath[slot] != path)
        {
            f.track[slot] = LLLocalBitmapMgr::getInstance()->addUnit(path);
            f.trackPath[slot] = path;
        }
        return f.track[slot].notNull() ? LLLocalBitmapMgr::getInstance()->getWorldID(f.track[slot]) : LLUUID::null;
    }

    // ---- images: read any (png, jpg, tga, bmp), pack, thumbnail
    LLPointer<LLImageRaw> lookRaw(const std::string& path)
    {
        if (path.empty()) return nullptr;
        LLPointer<LLImageFormatted> img = LLImageFormatted::createFromExtension(path);
        if (img.isNull() || !img->load(path)) return nullptr;
        LLPointer<LLImageRaw> raw = new LLImageRaw;
        if (!img->decode(raw, 0.f) || raw->getWidth() <= 0 || raw->getHeight() <= 0) return nullptr;
        return raw;
    }
    U8 rawGrey(const LLImageRaw* r, S32 x, S32 y)
    {
        const S32 c = r->getComponents();
        const U8* p = r->getData() + ((size_t)y * r->getWidth() + x) * c;
        return c >= 3 ? (U8)((p[0] * 30 + p[1] * 59 + p[2] * 11) / 100) : p[0];
    }
    bool lookSavePng(LLImageRaw* raw, const std::string& out)
    {
        LLPointer<LLImagePNG> png = new LLImagePNG;
        return png->encode(raw, 0.f) && png->save(out);
    }
    // a colour map with an opacity map as its alpha (SL keeps opacity in the colour map's alpha)
    bool lookPackColour(const std::string& colour, const std::string& opacity, const std::string& out)
    {
        LLPointer<LLImageRaw> c = lookRaw(colour), a = lookRaw(opacity);
        if (a.isNull()) return false;
        const S32 w = c.notNull() ? c->getWidth() : a->getWidth(), h = c.notNull() ? c->getHeight() : a->getHeight();
        if (a->getWidth() != w || a->getHeight() != h) a->scale(w, h);
        LLPointer<LLImageRaw> o = new LLImageRaw(w, h, 4);
        U8* d = o->getData();
        const S32 cc = c.notNull() ? c->getComponents() : 0;
        for (S32 y = 0; y < h; ++y)
            for (S32 x = 0; x < w; ++x)
            {
                U8* q = d + ((size_t)y * w + x) * 4;
                if (cc >= 3) { const U8* p = c->getData() + ((size_t)y * w + x) * cc; q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; }
                else if (cc > 0) q[0] = q[1] = q[2] = rawGrey(c, x, y);
                else q[0] = q[1] = q[2] = 255;
                q[3] = rawGrey(a, x, y);
            }
        return lookSavePng(o, out);
    }
    // ambient occlusion (red), roughness (green), metallic (blue): the one image SL's PBR reads; a map
    // left out is full (255): the Metallic / Roughness sliders then set it
    bool lookPackOrm(const std::string& ao, const std::string& metal, const std::string& rough, const std::string& out)
    {
        LLPointer<LLImageRaw> m[3] = { lookRaw(ao), lookRaw(rough), lookRaw(metal) };
        S32 w = 0, h = 0;
        for (auto& r : m) if (r.notNull()) { w = llmax(w, r->getWidth()); h = llmax(h, r->getHeight()); }
        if (!w || !h) return false;
        for (auto& r : m) if (r.notNull() && (r->getWidth() != w || r->getHeight() != h)) r->scale(w, h);
        LLPointer<LLImageRaw> o = new LLImageRaw(w, h, 3);
        U8* d = o->getData();
        for (S32 y = 0; y < h; ++y)
            for (S32 x = 0; x < w; ++x)
                for (int k = 0; k < 3; ++k) d[((size_t)y * w + x) * 3 + k] = m[k].notNull() ? rawGrey(m[k], x, y) : 255;
        return lookSavePng(o, out);
    }
    // a small picture of a texture for the page (it cannot read your disk): a PNG data URI
    std::string lookThumb(const std::string& path)
    {
        static std::map<std::string, std::pair<std::time_t, std::string>> cache;
        const std::time_t ts = fileTime(path);
        auto it = cache.find(path);
        if (it != cache.end() && it->second.first == ts) return it->second.second;
        LLPointer<LLImageRaw> r = lookRaw(path);
        if (r.isNull()) return std::string();
        r->scale(96, 96);
        if (r->getComponents() != 3 && r->getComponents() != 4)
        {
            LLPointer<LLImageRaw> o = new LLImageRaw(96, 96, 4);
            for (S32 y = 0; y < 96; ++y) for (S32 x = 0; x < 96; ++x)
            {
                U8* q = o->getData() + ((size_t)y * 96 + x) * 4;
                q[0] = q[1] = q[2] = rawGrey(r, x, y);
                q[3] = r->getComponents() == 2 ? r->getData()[((size_t)y * 96 + x) * 2 + 1] : 255;
            }
            r = o;
        }
        LLPointer<LLImagePNG> png = new LLImagePNG;
        if (!png->encode(r, 0.f)) return std::string();
        const std::string uri = "data:image/png;base64," + LLBase64::encode(png->getData(), png->getDataSize());
        cache[path] = { ts, uri };
        return uri;
    }

    // a derived image (a packing of sources) made again when a source is newer than it
    bool lookDerive(LookFace& f, const std::string& key, const std::vector<const std::string*>& srcs, const std::string& out, const std::function<bool()>& make)
    {
        std::time_t newest = 0;
        for (const std::string* s : srcs) if (!s->empty()) newest = std::max(newest, fileTime(*s));
        if (fileTime(out) != 0 && f.stamps[key] == newest) return false;
        f.stamps[key] = newest;
        make();
        return true;
    }

    // the PBR material as a .gltf (images copied or packed next to it), loaded as a local material
    void lookWriteGltf(LookFace& f, bool force)
    {
        const std::string dir = sLook.base + "_look";
        LLFile::mkdir(dir);
        const std::string stem = lookSafe(f.name) + llformat("_%d_%d", (S32)f.prim, f.te);
        const std::string sep = gDirUtilp->getDirDelimiter();
        f.gltf = dir + sep + stem + ".gltf";
        std::string uris[4];
        bool changed = force || fileTime(f.gltf) == 0;
        // a source copied next to the material (its own name), when new or saved again
        auto copyIn = [&](int k, const std::string& src, const char* slot)
        {
            std::string ext = gDirUtilp->getExtension(src);
            if (ext.empty()) ext = "png";
            uris[k] = stem + "_" + slot + "." + ext;
            const std::string dst = dir + sep + uris[k];
            const std::time_t ts = fileTime(src);
            if (f.stamps[src] != ts || fileTime(dst) == 0)
            {
                std::error_code ec;
                std::filesystem::copy_file(std::filesystem::path(ll_convert<std::wstring>(src)), std::filesystem::path(ll_convert<std::wstring>(dst)),
                                           std::filesystem::copy_options::overwrite_existing, ec);
                f.stamps[src] = ts;
                changed = true;
            }
        };
        // base colour: with an opacity map, packed (opacity in its alpha)
        if (!f.opacity.empty())
        {
            uris[0] = stem + "_basecolor.png";
            const std::string out = dir + sep + uris[0];
            if (lookDerive(f, "#base", { &f.baseTex, &f.opacity }, out, [&]() { return lookPackColour(f.baseTex, f.opacity, out); })) changed = true;
        }
        else if (!f.baseTex.empty()) copyIn(0, f.baseTex, "basecolor");
        if (!f.normTex.empty()) copyIn(1, f.normTex, "normal");
        // ORM: a packed one as given, else packed here from AO, metallic and roughness
        if (!f.ormTex.empty()) copyIn(2, f.ormTex, "orm");
        else if (!f.aoTex.empty() || !f.metalTex.empty() || !f.roughTex.empty())
        {
            uris[2] = stem + "_orm.png";
            const std::string out = dir + sep + uris[2];
            if (lookDerive(f, "#orm", { &f.aoTex, &f.metalTex, &f.roughTex }, out, [&]() { return lookPackOrm(f.aoTex, f.metalTex, f.roughTex, out); })) changed = true;
        }
        if (!f.emisTex.empty()) copyIn(3, f.emisTex, "emissive");
        if (changed)
        {
            std::ostringstream j;
            auto num = [&](F32 v) { j << llformat("%.4g", v); };
            j << "{\n  \"asset\": { \"version\": \"2.0\", \"generator\": \"MasterGate Creator Lab\" },\n  \"materials\": [ {\n    \"name\": \"" << lookSafe(f.name) << "\",\n";
            int tex = 0;
            std::vector<std::string> images;
            auto texRef = [&](int k) { images.push_back(uris[k]); return tex++; };
            j << "    \"pbrMetallicRoughness\": { \"baseColorFactor\": [";
            num(f.baseColor.mV[0]); j << ", "; num(f.baseColor.mV[1]); j << ", "; num(f.baseColor.mV[2]); j << ", "; num(f.baseColor.mV[3]);
            j << "], \"metallicFactor\": "; num(f.metallic); j << ", \"roughnessFactor\": "; num(f.roughness);
            if (!uris[0].empty()) j << ", \"baseColorTexture\": { \"index\": " << texRef(0) << " }";
            int ormIndex = -1;
            if (!uris[2].empty()) { ormIndex = texRef(2); j << ", \"metallicRoughnessTexture\": { \"index\": " << ormIndex << " }"; }
            j << " },\n";
            // the same image's red channel: ambient occlusion
            if (ormIndex >= 0 && (!f.ormTex.empty() || !f.aoTex.empty())) j << "    \"occlusionTexture\": { \"index\": " << ormIndex << " },\n";
            if (!uris[1].empty()) j << "    \"normalTexture\": { \"index\": " << texRef(1) << " },\n";
            if (!uris[3].empty()) j << "    \"emissiveTexture\": { \"index\": " << texRef(3) << " },\n";
            j << "    \"emissiveFactor\": ["; num(f.emissive.mV[0]); j << ", "; num(f.emissive.mV[1]); j << ", "; num(f.emissive.mV[2]); j << "],\n";
            j << "    \"alphaMode\": \"" << (f.alphaMode == 1 ? "BLEND" : f.alphaMode == 2 ? "MASK" : "OPAQUE") << "\",";
            if (f.alphaMode == 2) { j << " \"alphaCutoff\": "; num(f.cutoff); j << ","; }
            j << " \"doubleSided\": " << (f.doubleSided ? "true" : "false") << "\n  } ]";
            if (!images.empty())
            {
                j << ",\n  \"textures\": [";
                for (size_t k = 0; k < images.size(); ++k) j << (k ? ", " : " ") << "{ \"source\": " << k << " }";
                j << " ],\n  \"images\": [";
                for (size_t k = 0; k < images.size(); ++k) j << (k ? ", " : " ") << "{ \"uri\": \"" << images[k] << "\" }";
                j << " ]";
            }
            j << "\n}\n";
            llofstream out(f.gltf, std::ios::trunc);
            if (out) out << j.str();
        }
        if (f.gltfWorld.isNull())
        {
            LLLocalGLTFMaterialMgr::getInstance()->addUnit(f.gltf);
            f.gltfWorld = LLLocalGLTFMaterialMgr::getInstance()->getWorldIDForFile(f.gltf, 0);
        }
    }

    void lookApplyFace(size_t index)
    {
        if (index >= sLook.faces.size()) return;
        LookFace& f = sLook.faces[index];
        if (f.prim >= sApplied.size()) return;
        LLVOVolume* v = previewVolume(sApplied[f.prim].prim);
        if (!v || f.te >= (S32)v->getNumTEs()) return;
        const U8 te = (U8)f.te;
        const bool clay = sLook.view == 1, white = sLook.view == 2;
        if (sLook.flash == (S32)index)
        {
            // which face is this one: bright cyan for a moment
            v->setRenderMaterialID(f.te, LLUUID::null, false, true);
            v->setTEMaterialParams(te, nullptr);
            v->setTETexture(te, IMG_WHITE);
            v->setTEColor(te, LLColor4(0.f, 0.85f, 1.f, 1.f));
            v->setTEFullbright(te, 1);
        }
        else if (f.pbr && !clay && !white)
        {
            lookWriteGltf(f, false);
            v->setTEMaterialParams(te, nullptr);
            v->setTETexture(te, IMG_WHITE);
            v->setTEColor(te, LLColor4::white);
            v->setTEFullbright(te, 0);
            v->setTEGlow(te, f.glow);
            if (f.gltfWorld.notNull()) v->setRenderMaterialID(f.te, f.gltfWorld, false, true);
        }
        else
        {
            v->setRenderMaterialID(f.te, LLUUID::null, false, true);
            LLUUID tex = IMG_WHITE;
            LLColor4 col = LLColor4::white;
            F32 glow = 0.f, rep = 1.f;
            U8 fb = 0, shiny = 0;
            LLMaterialPtr mat;
            if (clay) { col = LLColor4(0.70f, 0.71f, 0.73f, 1.f); shiny = 1; }
            else if (!white)
            {
                // a diffuse map with an opacity map: packed (opacity in its alpha), next to the garment
                std::string dpath = f.diffuse;
                if (!f.opacity.empty())
                {
                    const std::string dir = sLook.base + "_look";
                    LLFile::mkdir(dir);
                    dpath = dir + gDirUtilp->getDirDelimiter() + lookSafe(f.name) + llformat("_%d_%d_diffuse.png", (S32)f.prim, f.te);
                    lookDerive(f, "#diffuse", { &f.diffuse, &f.opacity }, dpath, [&]() { return lookPackColour(f.diffuse, f.opacity, dpath); });
                }
                const LLUUID d = lookBitmap(f, 0, dpath);
                if (d.notNull()) tex = d;
                col = f.color; glow = f.glow; rep = f.repeat; fb = f.fullbright ? 1 : 0; shiny = (U8)f.shiny;
                const LLUUID n = lookBitmap(f, 1, f.normal), s = lookBitmap(f, 2, f.specular);
                if (n.notNull() || s.notNull())
                {
                    mat = new LLMaterial();
                    mat->setNormalID(n);
                    mat->setSpecularID(s);
                    mat->setNormalRepeat(rep, rep);
                    mat->setSpecularRepeat(rep, rep);
                    mat->setSpecularLightColor(LLColor4U::white);
                    mat->setSpecularLightExponent((U8)(51 + 51 * f.shiny));
                    mat->setEnvironmentIntensity(0);
                    mat->setDiffuseAlphaMode(col.mV[3] < 0.999f || !f.opacity.empty() ? LLMaterial::DIFFUSE_ALPHA_MODE_BLEND : LLMaterial::DIFFUSE_ALPHA_MODE_NONE);
                }
            }
            v->setTETexture(te, tex);
            v->setTEColor(te, col);
            v->setTEGlow(te, glow);
            v->setTEFullbright(te, fb);
            v->setTEShiny(te, shiny);
            v->setTEScale(te, rep, rep);
            v->setTEMaterialParams(te, mat);
        }
        v->markForUpdate();
        if (v->mDrawable.notNull()) gPipeline.markRebuild(v->mDrawable, LLDrawable::REBUILD_ALL);
    }

    void lookApplyAll()
    {
        for (size_t i = 0; i < sLook.faces.size(); ++i) lookApplyFace(i);
        sLook.vols.assign(sApplied.size(), nullptr);
        for (size_t pi = 0; pi < sApplied.size(); ++pi) { LLVOVolume* v = previewVolume(sApplied[pi].prim); sLook.vols[pi] = v ? v->getVolume() : nullptr; }
    }

    LLSD lookState()
    {
        lookEnsure();
        LLSD faces = LLSD::emptyArray();
        for (const auto& f : sLook.faces) faces.append(lookFaceSD(f));
        return LLSD().with("faces", faces).with("view", sLook.view).with("folder", sLook.base.empty() ? std::string() : sLook.base + "_look");
    }

    // twice a second: the garment worn changed or was reloaded (its faces are new): the look again;
    // a PBR image saved: its material written again (the local material reloads it); a flash over
    void lookTick()
    {
        if (sLook.timer.getElapsedTimeF32() < 0.5f) return;
        sLook.timer.reset();
        if (sWornFile.isNull() || sApplied.empty() || sTouch.active || sWV.on) return;   // weight colours and edits own the faces meanwhile
        const bool newWorn = sLook.worn != sWornFile;
        lookEnsure();
        if (sLook.faces.empty()) return;
        bool again = newWorn || sLook.vols.size() != sApplied.size();
        for (size_t pi = 0; pi < sApplied.size() && !again; ++pi)
        {
            LLVOVolume* v = previewVolume(sApplied[pi].prim);
            if ((v ? v->getVolume() : nullptr) != sLook.vols[pi]) again = true;
        }
        if (sLook.flash >= 0 && sLook.flashTimer.getElapsedTimeF32() > 0.7f) { const S32 fi = sLook.flash; sLook.flash = -1; lookApplyFace((size_t)fi); }
        for (auto& f : sLook.faces)
        {
            if (sLook.view != 0) continue;
            if (f.pbr)
            {
                for (const std::string* src : { &f.baseTex, &f.normTex, &f.ormTex, &f.emisTex, &f.opacity, &f.aoTex, &f.metalTex, &f.roughTex })
                    if (!src->empty() && fileTime(*src) > 0 && f.stamps[*src] != fileTime(*src)) { lookWriteGltf(f, false); for (const std::string* s2 : { &f.opacity, &f.aoTex, &f.metalTex, &f.roughTex }) if (!s2->empty()) f.stamps[*s2] = fileTime(*s2); break; }
            }
            else if (!f.opacity.empty())
            {
                // the packed diffuse made again when its sources are saved (the local texture then reloads)
                for (const std::string* src : { &f.diffuse, &f.opacity })
                    if (!src->empty() && f.stamps["#src:" + *src] != fileTime(*src)) { f.stamps["#src:" + *src] = fileTime(*src); lookApplyFace((size_t)(&f - &sLook.faces[0])); break; }
            }
        }
        if (again) lookApplyAll();
    }

    // ------------------------------------------------------------------
    // <MG> Alpha (2026-11-14): the skin the garment covers in every test pose, as Second Life alpha
    // masks (upper / lower body, Bakes on Mesh layout), saved next to the garment as PNGs; on request
    // uploaded and made into an Alpha wearable in the inventory (worn if asked). Only the mask leaves
    // the viewer: which areas of SL's own layout are covered, never the body's shape.
    // ------------------------------------------------------------------
    const S32 ALPHA_SIZE = 1024;

    // a triangle of the layout made transparent (a little wider than exact: no pixel gaps between triangles)
    void alphaFill(LLImageRaw* img, const LLVector2& ua, const LLVector2& ub, const LLVector2& uc)
    {
        const S32 W = img->getWidth(), H = img->getHeight();
        const F32 ax = ua.mV[0] * W, ay = ua.mV[1] * H, bx = ub.mV[0] * W, by = ub.mV[1] * H, cx = uc.mV[0] * W, cy = uc.mV[1] * H;
        const F32 area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (std::fabs(area) < 1e-6f) return;
        const F32 sgn = area > 0.f ? 1.f : -1.f;
        const S32 x0 = llmax(0, (S32)std::floor(llmin(ax, llmin(bx, cx)) - 1.f)), x1 = llmin(W - 1, (S32)std::ceil(llmax(ax, llmax(bx, cx)) + 1.f));
        const S32 y0 = llmax(0, (S32)std::floor(llmin(ay, llmin(by, cy)) - 1.f)), y1 = llmin(H - 1, (S32)std::ceil(llmax(ay, llmax(by, cy)) + 1.f));
        if (x0 > x1 || y0 > y1) return;
        auto edge = [sgn](F32 px, F32 py, F32 qx, F32 qy, F32 x, F32 y)
        {
            const F32 ex = qx - px, ey = qy - py, l = std::sqrt(ex * ex + ey * ey);
            return l > 1e-6f ? sgn * (ex * (y - py) - ey * (x - px)) / l : 0.f;   // signed distance, inside > 0
        };
        U8* d = img->getData();
        for (S32 y = y0; y <= y1; ++y)
            for (S32 x = x0; x <= x1; ++x)
            {
                const F32 px = x + 0.5f, py = y + 0.5f;
                if (edge(ax, ay, bx, by, px, py) < -0.75f || edge(bx, by, cx, cy, px, py) < -0.75f || edge(cx, cy, ax, ay, px, py) < -0.75f) continue;
                U8* p = d + ((size_t)y * W + x) * 4;
                p[0] = p[1] = p[2] = p[3] = 0;
            }
    }

    // the masks from the last full stress test; rings = how many rings of triangles stay shown at the
    // garment's edges (hems, sleeves, neckline: no skin gaps when the cloth moves)
    LLSD alphaMake(S32 rings)
    {
        LLSD r = LLSD().with("ok", false);
        if (!sAlpha.complete || sAlpha.samples <= 0) return r.with("error", "alpha_test");
        if (sAlpha.faces.empty()) return r.with("error", "alpha_bom");
        for (S32 c = 0; c < 2; ++c)
        {
            sAlpha.raw[c] = new LLImageRaw(ALPHA_SIZE, ALPHA_SIZE, 4);
            sAlpha.raw[c]->clear(255, 255, 255, 255);
        }
        S32 used = 0, hiddenTris = 0;
        for (const auto& kv : sAlpha.faces)
        {
            const AlphaFace& a = kv.second;
            if (a.samples < sAlpha.samples) continue;   // not looked at in every pose (reloaded meanwhile)
            ++used;
            const size_t n = a.uv.size();
            std::vector<U8> solid(n, 0);
            for (size_t j = 0; j < n; ++j) solid[j] = a.covered[j] >= a.samples ? 1 : 0;
            std::vector<std::vector<U16>> nb(n);
            for (size_t i = 0; i + 2 < a.idx.size(); i += 3)
                for (S32 k = 0; k < 3; ++k)
                {
                    const U16 p = a.idx[i + k], q = a.idx[i + (k + 1) % 3];
                    if (p < n && q < n) { nb[p].push_back(q); nb[q].push_back(p); }
                }
            for (S32 ring = 0; ring < rings; ++ring)
            {
                std::vector<U8> next = solid;
                for (size_t j = 0; j < n; ++j)
                    if (solid[j]) for (U16 u : nb[j]) if (!solid[u]) { next[j] = 0; break; }
                solid.swap(next);
            }
            for (size_t i = 0; i + 2 < a.idx.size(); i += 3)
            {
                const U16 p = a.idx[i], q = a.idx[i + 1], s = a.idx[i + 2];
                if (p >= n || q >= n || s >= n || !solid[p] || !solid[q] || !solid[s]) continue;
                alphaFill(sAlpha.raw[a.channel], a.uv[p], a.uv[q], a.uv[s]);
                ++hiddenTris;
            }
        }
        if (!used) return r.with("error", "alpha_test");
        // how much of each layout is hidden, and the PNGs next to the garment
        const FileWatch* w = watchOf(sWornFile);
        std::string stem = w ? w->path : std::string();
        const size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, "_fit") == 0) stem = stem.substr(0, stem.size() - 4);
        LLSD files = LLSD::emptyArray();
        S32 uploads = 0;
        for (S32 c = 0; c < 2; ++c)
        {
            const U8* d = sAlpha.raw[c]->getData();
            size_t off = 0;
            const size_t px = (size_t)ALPHA_SIZE * ALPHA_SIZE;
            for (size_t i = 0; i < px; ++i) if (d[i * 4 + 3] == 0) ++off;
            sAlpha.hidden[c] = (F32)off / (F32)px;
            sAlpha.files[c].clear();
            if (off == 0 || stem.empty()) continue;
            ++uploads;
            const std::string out = stem + (c == 0 ? "_alpha_upper.png" : "_alpha_lower.png");
            if (lookSavePng(sAlpha.raw[c], out)) { sAlpha.files[c] = out; files.append(baseName(out)); }
        }
        if (!uploads) return r.with("error", "alpha_none");
        LL_INFOS("MGCreator") << "Alpha: " << hiddenTris << " skin triangles hidden on " << used << " Bakes on Mesh faces; upper "
                              << sAlpha.hidden[0] * 100.f << " %, lower " << sAlpha.hidden[1] * 100.f << " %" << LL_ENDL;
        const S32 each = LLAgentBenefitsMgr::current().getTextureUploadCost(ALPHA_SIZE, ALPHA_SIZE);
        return r.with("ok", true).with("upper", (F32)ll_round(sAlpha.hidden[0] * 1000.f) / 10.f).with("lower", (F32)ll_round(sAlpha.hidden[1] * 1000.f) / 10.f)
                .with("files", files).with("uploads", uploads).with("cost", each * uploads).with("folder", stem.empty() ? std::string() : gDirUtilp->getDirName(stem))
                .with("name", baseName(stem) + " alpha");
    }

    // the two masks uploaded: an Alpha wearable made with them, put in the Clothing folder (worn if asked)
    void alphaCreateWearable()
    {
        if (!isAgentAvatarValid()) return;
        LLViewerWearable* base = LLWearableList::instance().createNewWearable(LLWearableType::WT_ALPHA, gAgentAvatarp);
        if (!base) { push(LLSD().with("type", "lab_alpha_saved").with("ok", false).with("error", "alpha_wearable")); return; }
        for (S32 c = 0; c < 2; ++c)
        {
            if (sAlpha.asset[c].isNull()) continue;
            LLViewerFetchedTexture* img = LLViewerTextureManager::getFetchedTexture(sAlpha.asset[c]);
            LLLocalTextureObject lto(img, sAlpha.asset[c]);
            base->setLocalTextureObject(c == 0 ? LLAvatarAppearanceDefines::TEX_UPPER_ALPHA : LLAvatarAppearanceDefines::TEX_LOWER_ALPHA, lto);
        }
        // a copy carries the textures (the first save had the defaults); it is the one put in the inventory
        LLViewerWearable* wearable = LLWearableList::instance().createCopy(base, sAlpha.name);
        if (!wearable) { push(LLSD().with("type", "lab_alpha_saved").with("ok", false).with("error", "alpha_wearable")); return; }
        const bool wear = sAlpha.wear;
        const std::string name = sAlpha.name;
        LLPointer<LLInventoryCallback> cb = new LLBoostFuncInventoryCallback([wear, name](const LLUUID& item)
        {
            push(LLSD().with("type", "lab_alpha_saved").with("ok", item.notNull()).with("name", name).with("worn", wear && item.notNull()));
            if (wear && item.notNull()) LLAppearanceMgr::instance().wearItemOnAvatar(item, true, false);
        });
        const LLUUID folder = gInventory.findCategoryUUIDForType(LLFolderType::assetTypeToFolderType(wearable->getAssetType()));
        create_inventory_wearable(gAgent.getID(), gAgent.getSessionID(), folder, wearable->getTransactionID(), wearable->getName(),
                                  wearable->getDescription(), wearable->getAssetType(), wearable->getType(),
                                  wearable->getPermissions().getMaskNextOwner(), cb);
    }

    LLSD alphaSave(const std::string& name, bool wear)
    {
        LLSD r = LLSD().with("ok", false);
        if (sAlpha.pending > 0) return r.with("error", "busy");
        if (sAlpha.raw[0].isNull() || (sAlpha.hidden[0] <= 0.f && sAlpha.hidden[1] <= 0.f)) return r.with("error", "alpha_none");
        sAlpha.name = name.empty() ? std::string("Creator Lab alpha") : name.substr(0, 60);
        sAlpha.wear = wear;
        sAlpha.failed = false;
        sAlpha.asset[0].setNull();
        sAlpha.asset[1].setNull();
        S32 cost = 0;
        std::vector<std::pair<S32, std::string>> uploads;
        for (S32 c = 0; c < 2; ++c)
        {
            if (sAlpha.hidden[c] <= 0.f) continue;
            LLPointer<LLImageJ2C> j2c = LLViewerTextureList::convertToUploadFile(sAlpha.raw[c]);
            if (j2c.isNull() || !j2c->getData() || j2c->getDataSize() <= 0) return r.with("error", "alpha_encode");
            uploads.push_back({ c, std::string((const char*)j2c->getData(), j2c->getDataSize()) });
            cost += LLAgentBenefitsMgr::current().getTextureUploadCost(j2c);
        }
        sAlpha.pending = (S32)uploads.size();
        for (const auto& up : uploads)
        {
            const S32 c = up.first;
            LLNewBufferedResourceUploadInfo::uploadFinish_f finish = [c](LLUUID asset, LLSD)
            {
                sAlpha.asset[c] = asset;
                if (--sAlpha.pending == 0 && !sAlpha.failed) alphaCreateWearable();
            };
            LLNewBufferedResourceUploadInfo::uploadFailure_f failure = [](LLUUID, LLSD, std::string reason)
            {
                sAlpha.failed = true;
                --sAlpha.pending;
                push(LLSD().with("type", "lab_alpha_saved").with("ok", false).with("error", reason.empty() ? std::string("upload") : reason));
                return true;   // handled (the page says it)
            };
            const std::string tname = sAlpha.name + (c == 0 ? " (upper)" : " (lower)");
            LLResourceUploadInfo::ptr_t info(std::make_shared<LLNewBufferedResourceUploadInfo>(
                up.second, LLUUID::generateNewID(), tname, tname, 0,
                LLFolderType::FT_TEXTURE, LLInventoryType::IT_TEXTURE, LLAssetType::AT_TEXTURE,
                LLFloaterPerms::getNextOwnerPerms("Uploads"), LLFloaterPerms::getGroupPerms("Uploads"), LLFloaterPerms::getEveryonePerms("Uploads"),
                LLAgentBenefitsMgr::current().getTextureUploadCost(ALPHA_SIZE, ALPHA_SIZE), LLUUID::null, false, finish, failure));
            upload_new_resource(info);
        }
        LL_INFOS("MGCreator") << "Alpha: uploading " << uploads.size() << " masks for \"" << sAlpha.name << "\" (L$ " << cost << ")" << LL_ENDL;
        return r.with("ok", true).with("uploads", (S32)uploads.size()).with("cost", cost);
    }

    void testIdle()
    {
        if (!sRun.active || sRun.step < 0) return;
        if (!isAgentAvatarValid()) { endRun(true); return; }
        const Step& s = STEPS[sRun.step];
        const F32 due = s.settle + s.every * sRun.sampled;
        if (sRun.timer.getElapsedTimeF32() < due) return;
        if (sRun.fit)
        {
            sRun.pushed = llmax(sRun.pushed, fitSample());
            if (++sRun.sampled < s.samples) return;
            fitSmooth(4 + sFit.smooth);   // <MG> the move kept smooth; the next poses fix what it reopens
            push(LLSD().with("type", "lab_fit").with("i", sRun.step).with("key", s.key).with("name", s.name)
                .with("state", "done").with("pushed", sRun.pushed).with("left", sRun.left).with("deep", sRun.deep).with("squashed", sRun.squashed));
            LL_INFOS("MGCreator") << "Make it fit, " << s.key << ": " << sRun.pushed << " found, " << sRun.left << " left by the stress test's rule (" << sRun.deep << " deeper than Max push; " << sRun.squashed << " not pushed where the skinning squashes moves)" << LL_ENDL;
            S32 nx = sRun.step + 1;
            while (nx < NSTEPS && !sFit.shapes && STEPS[nx].shape != 0)
            {
                push(LLSD().with("type", "lab_fit").with("i", nx).with("key", STEPS[nx].key).with("name", STEPS[nx].name).with("state", "skip"));
                ++nx;
            }
            if (nx < NSTEPS) { startStep(nx); return; }
            endRun(false);   // the avatar back to itself first
            finishFit();
            return;
        }
        LLSD c = clipCheck();
        if (!c["ok"].asBoolean())
        {
            push(LLSD().with("type", "lab_test").with("i", sRun.step).with("key", s.key).with("name", s.name)
                .with("state", "error").with("error", c["error"]));
            endRun(true);
            return;
        }
        sRun.poke = llmax(sRun.poke, c["poke"].asInteger());
        sRun.covered = llmax(sRun.covered, c["covered"].asInteger());
        for (auto it = c["regions"].beginMap(); it != c["regions"].endMap(); ++it)
        {
            S32& r = sRun.regions[it->first];
            r = llmax(r, it->second.asInteger());
        }
        if (++sRun.sampled < s.samples) return;

        LLSD regions = LLSD::emptyMap();
        for (const auto& kv : sRun.regions) regions[kv.first] = kv.second;
        push(LLSD().with("type", "lab_test").with("i", sRun.step).with("key", s.key).with("name", s.name)
            .with("state", "done").with("poke", sRun.poke).with("covered", sRun.covered).with("regions", regions));
        if (sRun.step + 1 < NSTEPS) startStep(sRun.step + 1);
        else
        {
            // <MG> every pose looked at: the alpha can be made from it
            sAlpha.complete = sAlpha.collect && sAlpha.samples > 0;
            endRun(false);
            push(LLSD().with("type", "lab_alpha").with("ready", sAlpha.complete).with("bom", !sAlpha.faces.empty()));
        }
    }
}

namespace MGPhoneCreator
{
    bool isCommand(const std::string& cmd)
    {
        return cmd.rfind("lab_", 0) == 0;
    }

    void idle()
    {
        for (auto& w : sFiles)
        {
            LLLocalMeshFile* f = lms()->getFile(w.id);
            if (!f) continue;
            auto info = f->getFileInfo();
            auto& objs = f->getObjectVector();
            const void* gen = objs.empty() ? nullptr : (const void*)objs.front().get();
            if ((S32)info.mStatus == w.status && gen == w.gen) continue;
            const bool reloaded = w.gen != nullptr && gen != nullptr && gen != w.gen;
            w.status = (S32)info.mStatus;
            w.gen = gen;
            if (info.mStatus == LLLocalMeshFile::STATUS_ACTIVE && gen) ++w.loads;
            LLSD ev = fileRow(w);
            ev["type"] = "lab_file";
            ev["reloaded"] = reloaded;
            if (info.mStatus == LLLocalMeshFile::STATUS_ACTIVE) ev["checks"] = runChecks(f, w.path);
            push(ev);
        }
        // the stand-in was taken off or the file closed: nothing left to restore
        if (sStandIn.notNull())
        {
            LLViewerObject* root = gObjectList.findObject(sStandIn);
            if (!root || root->isDead() || !lms()->getFile(sWornFile))
            {
                if (sRun.active) endRun(true);
                clearMarkers();
                sApplied.clear();
                sStandIn.setNull();
                sWornFile.setNull();
            }
        }
        if (sFit.pendingFile.notNull())
        {
            LLLocalMeshFile* f = lms()->getFile(sFit.pendingFile);
            if (!f || sFit.pendingTimer.getElapsedTimeF32() > 60.f || f->getFileInfo().mStatus == LLLocalMeshFile::STATUS_ERROR)
            {
                sFit.pendingFile.setNull();
            }
            else if (f->getFileInfo().mStatus == LLLocalMeshFile::STATUS_ACTIVE)
            {
                const LLUUID id = sFit.pendingFile;
                sFit.pendingFile.setNull();
                LLSD r = wearFile(id, sFit.standin, true);
                push(LLSD().with("type", "lab_fit_worn").with("ok", r["ok"]).with("file", id.asString()).with("error", r["error"]));
            }
        }
        if (sTouch.pendingFile.notNull())
        {
            LLLocalMeshFile* f = lms()->getFile(sTouch.pendingFile);
            if (!f || sTouch.pendingTimer.getElapsedTimeF32() > 60.f || f->getFileInfo().mStatus == LLLocalMeshFile::STATUS_ERROR)
            {
                sTouch.pendingFile.setNull();
            }
            else if (f->getFileInfo().mStatus == LLLocalMeshFile::STATUS_ACTIVE)
            {
                const LLUUID id = sTouch.pendingFile;
                sTouch.pendingFile.setNull();
                const bool wasActive = sTouch.active;
                LLSD r = wearFile(id, sTouch.standin, true);   // ends Touch up (wear) ...
                if (wasActive && r["ok"].asBoolean())
                {
                    const FileWatch* w = watchOf(id);
                    if (w)
                    {
                        // ... and goes on, on the _fit copy, with the same undo history
                        auto undo = sTouch.undo, redo = sTouch.redo;
                        const S32 brush = sTouch.brush;
                        const F32 radius = sTouch.radius, strength = sTouch.strength;
                        const bool sym = sTouch.symmetry;
                        if (touchStart(w->path).empty())
                        {
                            sTouch.undo = undo; sTouch.redo = redo;
                            sTouch.brush = brush; sTouch.radius = radius; sTouch.strength = strength; sTouch.symmetry = sym;
                        }
                    }
                }
                push(LLSD().with("type", "lab_touch_worn").with("ok", r["ok"]).with("file", id.asString()).with("active", sTouch.active));
            }
        }
        if (sTouch.active && sApplied.empty()) touchStop();
        // <MG> versions made on the worker thread since the last frame
        if (sVersionJob)
        {
            std::vector<VersionDone> fresh;
            bool finished = false;
            {
                std::lock_guard<std::mutex> g(sVersionJob->lock);
                for (size_t i = sVersionReported; i < sVersionJob->done.size(); ++i) fresh.push_back(sVersionJob->done[i]);
                sVersionReported = sVersionJob->done.size();
                finished = sVersionJob->finished;
            }
            for (const auto& d : fresh)
            {
                LLSD ev = LLSD().with("type", "lab_version").with("body", d.body).with("ok", d.error.empty());
                if (d.error.empty())
                {
                    sVersionOut.push_back(d.out);
                    ev.with("index", (S32)sVersionOut.size() - 1).with("name", baseName(d.out))
                      .with("surface", d.surface).with("followed", d.followed).with("bones_only", d.bonesOnly)
                      .with("added", d.added).with("mean_mm", d.meanMm).with("max_mm", d.maxMm);
                }
                else ev.with("error", d.error);
                ev.with("done", (S32)sVersionReported).with("total", (S32)sVersionJob->total);
                push(ev);
            }
            if (finished)
            {
                push(LLSD().with("type", "lab_versions_done").with("made", (S32)sVersionOut.size()).with("total", (S32)sVersionJob->total));
                sVersionJob.reset();
                sVersionReported = 0;
            }
        }
        // <MG> a conversion finished on the worker thread
        if (sConvertJob)
        {
            bool finished = false;
            {
                std::lock_guard<std::mutex> g(sConvertJob->lock);
                finished = sConvertJob->finished;
            }
            if (finished)
            {
                const auto job = sConvertJob;
                sConvertJob.reset();
                const MGRig::Report& r = job->report;
                LLSD ev = LLSD().with("type", "lab_convert").with("name", baseName(job->source)).with("ok", job->error.empty());
                if (!job->error.empty()) ev.with("error", job->error);
                if (job->detection.map >= 0)
                    ev.with("map", job->detection.name).with("score", (S32)std::lround(job->detection.score * 100.0))
                      .with("matched", job->detection.matched).with("weighted", job->detection.weighted);
                else ev.with("weighted", job->detection.weighted);
                auto list = [](const std::vector<std::string>& v, size_t most)
                {
                    LLSD a = LLSD::emptyArray();
                    for (size_t i = 0; i < v.size() && i < most; ++i) a.append(v[i]);
                    return a;
                };
                if (job->error.empty())
                {
                    ev.with("out", baseName(job->out))
                      .with("bones_in", r.bonesIn).with("bones_out", r.bonesOut).with("mapped", r.mapped).with("merged", r.merged)
                      .with("fallback_count", (S32)r.fallback.size()).with("fallback", list(r.fallback, 8))
                      .with("left_out", list(r.leftOut, 20)).with("tposed", r.tposed).with("max_turn", r.maxTurnDeg)
                      .with("meshes", r.meshesOut).with("verts", (S32)r.vertices).with("tris", (S32)r.triangles)
                      .with("max_bones", r.maxBonesPerMesh).with("height", r.height).with("warnings", list(r.warnings, 20));
                    LLSD o = openPath(job->out);
                    if (o["ok"].asBoolean()) ev.with("file", o["id"]);
                }
                ev.with("notes", list(job->notes, 10));
                push(ev);
            }
        }
        skinGuard();
        placeIdle();
        drapeFollow(false);
        previewTick();
        if (sTouch.active && sTouch.liveDirty && sTouch.liveTimer.getElapsedTimeF32() > 0.05f)
        {
            sTouch.liveDirty = false;
            sTouch.liveTimer.reset();
            if (sTouch.live) touchPreviewApply();
        }
        if (sTouch.active && sTouch.hideDirty) editApplyHidden();   // <MG> Edit Mesh: hidden faces (again, after a reload)
        lookTick();                                                 // <MG> Look: colours, textures, materials on the garment
        weightViewTick();
        if (sDrape.active && sDrape.job && sDrape.stateTimer.getElapsedTimeF32() > 0.5f)
        {
            sDrape.stateTimer.reset();
            LLSD ev = LLSD().with("type", "lab_drape_state").with("running", true);
            {
                std::lock_guard<std::mutex> g(sDrape.job->lock);
                S32 pinned = 0;
                for (uint8_t p : sDrape.job->pins) pinned += p;
                ev.with("motion", sDrape.job->motion).with("contacts", (S32)sDrape.job->contacts).with("steps", (S32)sDrape.job->steps)
                  .with("points", (S32)(sDrape.job->snap.size() / 3)).with("pinned", pinned).with("paused", sDrape.job->paused)
                  .with("settled", sDrape.job->motion < 0.01f && sDrape.job->steps > 60)
                  .with("preview", sPreview.on).with("follow", sDrape.follow);
            }
            if (sDrape.job->stop) { drapeStop(); ev["running"] = false; ev["error"] = "stopped"; }
            push(ev);
        }
        // <MG> the body finished loading
        if (sBodyJob)
        {
            const auto job = sBodyJob;
            bool done = false;
            {
                std::lock_guard<std::mutex> g(job->lock);
                done = job->done;
            }
            if (done)
            {
                sBodyJob.reset();
                LLSD ev = LLSD().with("type", "lab_devkit").with("name", baseName(job->path)).with("ok", (bool)job->body);
                if (job->body)
                {
                    sDevkitPath = job->path;
                    sBody = job->body;
                    sBodyPieces = job->pieces;
                    size_t verts = 0;
                    for (const auto& m : job->body->meshes) verts += m.vertexCount();
                    LLSD pieces = LLSD::emptyArray();
                    for (const auto& p : job->pieces) pieces.append(p);
                    ev.with("verts", (S32)verts).with("meshes", (S32)job->body->meshes.size()).with("pieces", pieces);
                }
                else ev.with("error", job->error);
                push(ev);
            }
        }
        // <MG> rig job: the analysis (to review), then each rigged file
        if (sRigJob)
        {
            const auto job = sRigJob;
            int phase = 0;
            {
                std::lock_guard<std::mutex> g(job->lock);
                phase = job->phase;
            }
            if (phase != job->reportedPhase)
            {
                job->reportedPhase = phase;
                auto list = [](const std::vector<std::string>& v)
                {
                    LLSD a = LLSD::emptyArray();
                    for (const auto& s : v) a.append(s);
                    return a;
                };
                if (phase == 5)
                {
                    push(LLSD().with("type", "lab_rig_analysis").with("ok", false).with("name", baseName(job->source)).with("error", job->error));
                    sRigJob.reset();
                }
                else if (phase == 2)
                {
                    LLSD items = LLSD::emptyArray();
                    for (size_t i = 0; i < job->items.size(); ++i)
                    {
                        const MGRigCloth::Item& it = job->items[i];
                        items.append(LLSD().with("index", (S32)i).with("name", it.name).with("verts", (S32)it.verts).with("materials", it.materials)
                            .with("rigged", it.rigged).with("sl", it.slBones).with("near", it.nearBody < 0 ? -1 : (S32)std::lround(it.nearBody * 100.0))
                            .with("size_cm", (S32)std::lround(it.size * 100.0)).with("mode", it.mode).with("bone", it.bone));
                    }
                    // bones an accessory can ride on (the body's main bones and the ears)
                    LLSD bones = LLSD::emptyArray();
                    for (const auto& b : job->sl)
                    {
                        if (b.volume || b.name.size() < 2 || b.name[0] != 'm') continue;
                        if (b.name.compare(0, 5, "mFace") == 0 && b.name.compare(0, 9, "mFaceEar1") != 0 && b.name.compare(0, 9, "mFaceEar2") != 0) continue;
                        if (b.name.compare(0, 5, "mHand") == 0 || b.name.compare(0, 6, "mSpine") == 0) continue;
                        bones.append(b.name);
                    }
                    push(LLSD().with("type", "lab_rig_analysis").with("ok", true).with("name", baseName(job->source))
                        .with("devkit", job->body ? baseName(sDevkitPath) : std::string()).with("items", items).with("bones", bones)
                        .with("notes", list(job->model.notes)));
                }
                else if (phase == 4)
                {
                    const MGRigCloth::Result& r = job->result;
                    LLSD ev = LLSD().with("type", "lab_rig_done").with("ok", job->error.empty()).with("name", baseName(job->source));
                    if (!job->error.empty()) ev.with("error", job->error);
                    else
                    {
                        ev.with("out", baseName(job->out)).with("skinned", r.skinned).with("attached", r.attached).with("kept", r.kept)
                          .with("skipped", r.skipped).with("far", (S32)r.farPoints).with("max_bones", r.maxBones).with("warnings", list(r.warnings));
                        // on the stand-in when one is worn
                        LLSD o = openPath(job->out);
                        if (o["ok"].asBoolean())
                        {
                            const LLUUID id(o["id"].asString());
                            ev.with("file", id.asString());
                            // the stand-in in use, else the one the page chose (worn, nothing shown on it yet)
                            const LLUUID standin = sStandIn.notNull() ? sStandIn : job->standin;
                            if (standin.notNull() && id != sWornFile)
                            {
                                sFit.pendingFile = id;
                                sFit.standin = standin;
                                sFit.pendingTimer.reset();
                            }
                        }
                    }
                    if (job->error.empty() && job->preview)
                    {
                        // live placement on the preview: the pieces' centre, the placement asked for
                        glm::vec3 lo(1e9f), hi(-1e9f);
                        for (const auto& m : job->model.meshes)
                            for (size_t v = 0; v < m.vertexCount(); ++v)
                                for (int k = 0; k < 3; ++k) { lo[k] = llmin(lo[k], (F32)m.pos[v * 3 + k]); hi[k] = llmax(hi[k], (F32)m.pos[v * 3 + k]); }
                        sPlace.base.clear();
                        sPlace.ours.clear();
                        sPlace.centre = (lo + hi) * 0.5f;
                        for (int k = 0; k < 3; ++k) sPlace.move[k] = job->pmove[k];
                        sPlace.turn = job->pturn;
                        sPlace.scale = job->pscale;
                        sPlace.on = true;   // applied by placeIdle() once the stand-in shows it
                        placeToolOn();
                        ev.with("placing", true);
                    }
                    push(ev);
                    // ready to change the choices and rig again
                    std::lock_guard<std::mutex> g(job->lock);
                    job->phase = 2;
                    job->reportedPhase = 2;
                }
            }
        }
        if (sDiagPending && sDiagTimer.getElapsedTimeF32() > 2.f)
        {
            sDiagPending = false;
            logRenderState("after 2 s");
        }
        testIdle();
    }

    void onClose()
    {
        lodRestore();
        if (sPoseBones)
        {
            poseToolOff();
            MGPhonePoser::runCommand("pose_gizmo_mode", LLSD().with("mode", "off"));
            MGPhonePoser::runCommand("pose_markers", LLSD().with("on", false));
            sPoseBones = false;
        }
        placeToolOff();   // dragging in the world must not move the garment with the phone closed
        drapeStop();
        touchStop();
        endRun(true);
    }

    LLSD runCommand(const std::string& cmd, const LLSD& args)
    {
        LLSD result = LLSD().with("ok", true);

        if (cmd == "lab_state")
        {
            LLSD files = LLSD::emptyArray();
            for (auto& w : sFiles)
            {
                if (lms()->getFile(w.id)) files.append(fileRow(w));
            }
            result["files"] = files;
            result["autoreload"] = (bool)gSavedSettings.getBOOL("FSLocalMeshAutoReload");
            result["build"] = std::string(__DATE__) + " " + __TIME__;   // which viewer build answers (dev vs installed)
            result["standin"] = sStandIn.isNull() ? LLSD() : LLSD(sStandIn.asString());
            result["worn"] = sWornFile.isNull() ? LLSD() : LLSD(sWornFile.asString());
            result["testing"] = sRun.active;
            result["markers"] = sMarkersOn;
            result["posing"] = MGPhonePoser::isPosing();   // posed to a garment (the poser holds the joints)
            result["pose_bones"] = sPoseBones;
            {
                // one name per TouchBrush (reading past this list crashed the viewer with a weight brush on)
                static const char* BN[] = { "push", "smooth", "inflate", "grab", "protect", "weight", "wsmooth", "stiff", "legs", "erase" };
                const S32 bi = llclamp(sTouch.brush, 0, (S32)(sizeof(BN) / sizeof(BN[0])) - 1);
                result["touch"] = LLSD().with("active", sTouch.active).with("brush", BN[bi])
                    .with("size_mm", sTouch.radius * 1000.f).with("strength", sTouch.strength).with("symmetry", sTouch.symmetry)
                    .with("undo", (S32)sTouch.undo.size()).with("redo", (S32)sTouch.redo.size())
                    .with("out", sTouch.active ? LLSD(baseName(sTouch.outPath)) : LLSD());
            }
            result["hold"] = sRun.hold && sRun.step >= 0 ? LLSD(STEPS[sRun.step].key) : LLSD();
            LLSD steps = LLSD::emptyArray();
            for (const auto& s : STEPS) steps.append(LLSD().with("key", s.key).with("name", s.name));
            result["steps"] = steps;
        }
        else if (cmd == "lab_open")
        {
            if (sPicking) return result.with("ok", false).with("error", "busy");
            sPicking = true;
            LLFilePickerReplyThread::startPicker(
                [](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
                {
                    sPicking = false;
                    if (files.empty()) return;
                    LLSD r = openPath(files[0]);
                    if (!r["ok"].asBoolean())
                    {
                        push(LLSD().with("type", "lab_file").with("status", "error").with("error", r["error"])
                            .with("name", baseName(files[0])));
                    }
                },
                LLFilePicker::FFLOAD_MODEL, false,
                [](const std::vector<std::string>&, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter) { sPicking = false; });
        }
        else if (cmd == "lab_reload")
        {
            LLUUID id(args["file"].asString());
            if (!lms()->getFile(id)) return result.with("ok", false).with("error", "file");
            lms()->reloadFile(id);
        }
        else if (cmd == "lab_close")
        {
            LLUUID id(args["file"].asString());
            if (id == sWornFile) { touchStop(); endRun(true); unwear(); clearMarkers(); }
            if (lms()->getFile(id)) lms()->deleteFile(id);
            sFiles.erase(std::remove_if(sFiles.begin(), sFiles.end(), [&id](const FileWatch& w) { return w.id == id; }), sFiles.end());
        }
        else if (cmd == "lab_autoreload")
        {
            gSavedSettings.setBOOL("FSLocalMeshAutoReload", args["on"].asBoolean());
            lms()->refreshAutoReloadTimer();
            result["on"] = args["on"].asBoolean();
        }
        else if (cmd == "lab_standins")
        {
            LLSD worn = LLSD::emptyArray();
            if (isAgentAvatarValid())
            {
                for (const auto& kv : gAgentAvatarp->mAttachmentPoints)
                {
                    LLViewerJointAttachment* att = kv.second;
                    if (!att || att->getIsHUDAttachment()) continue;
                    for (const auto& ao : att->mAttachedObjects)
                    {
                        LLViewerObject* root = ao.get();
                        if (!root || root->isDead() || root->getPCode() != LL_PCODE_VOLUME) continue;
                        LLVOVolume* v = static_cast<LLVOVolume*>(root);
                        const std::string name = itemName(root);
                        const bool mesh = v->isMesh() && !v->mIsLocalMesh;
                        worn.append(LLSD().with("id", root->getID().asString()).with("name", name)
                            .with("point", att->getName()).with("prims", (S32)primsOf(root).size())
                            .with("mesh", mesh).with("rigged", v->isRiggedMesh() && !v->mIsLocalMesh)
                            .with("suggest", looksLikeStandIn(name) || (!mesh && !v->mIsLocalMesh))
                            .with("current", root->getID() == sStandIn));
                    }
                }
            }
            LLSD inv = LLSD::emptyArray();
            LLInventoryModel::cat_array_t cats;
            LLInventoryModel::item_array_t items;
            StandInFinder finder;
            gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, items, LLInventoryModel::EXCLUDE_TRASH, finder);
            for (const auto& it : items)
            {
                if (it.isNull() || get_is_item_worn(it->getUUID())) continue;
                inv.append(LLSD().with("item", it->getUUID().asString()).with("name", it->getName()));
                if (inv.size() >= 10) break;
            }
            result["worn"] = worn;
            result["inventory"] = inv;
        }
        else if (cmd == "lab_standin_wear")
        {
            LLUUID item(args["item"].asString());
            LLViewerInventoryItem* it = gInventory.getItem(item);
            if (!it || it->getType() != LLAssetType::AT_OBJECT) return result.with("ok", false).with("error", "item");
            LLAppearanceMgr::instance().wearItemOnAvatar(item, true, false);   // added, nothing replaced
        }
        else if (cmd == "lab_wear")
        {
            touchStop();
            result = wearFile(LLUUID(args["file"].asString()), LLUUID(args["standin"].asString()), args["force"].asBoolean());
        }
        else if (cmd == "lab_unwear")
        {
            const LLUUID root = sStandIn;
            touchStop();
            endRun(true);
            unwear();
            clearMarkers();
            // {detach}: the stand-in object comes off the avatar as well
            if (args["detach"].asBoolean())
            {
                LLViewerObject* o = gObjectList.findObject(args.has("standin") ? LLUUID(args["standin"].asString()) : root);
                if (isMyAttachment(o) && o->getAttachmentItemID().notNull())
                {
                    LLAppearanceMgr::instance().removeItemFromAvatar(o->getAttachmentItemID());
                }
            }
        }
        else if (cmd == "lab_checks")
        {
            const LLUUID file(args["file"].asString());
            LLLocalMeshFile* f = lms()->getFile(file);
            const FileWatch* w = watchOf(file);
            if (!f || !w || f->getFileInfo().mStatus != LLLocalMeshFile::STATUS_ACTIVE) return result.with("ok", false).with("error", "file");
            result["checks"] = runChecks(f, w->path);
        }
        else if (cmd == "lab_bodies")
        {
            LLSD rows = LLSD::emptyArray();
            if (isAgentAvatarValid())
            {
                for (const auto& kv : gAgentAvatarp->mAttachmentPoints)
                {
                    LLViewerJointAttachment* att = kv.second;
                    if (!att || att->getIsHUDAttachment()) continue;
                    for (const auto& ao : att->mAttachedObjects)
                    {
                        LLViewerObject* root = ao.get();
                        if (!root || root->isDead() || root->getID() == sStandIn) continue;
                        bool rigged = false;
                        for (LLViewerObject* o : primsOf(root))
                        {
                            if (o->getPCode() == LL_PCODE_VOLUME && static_cast<LLVOVolume*>(o)->isRiggedMesh() &&
                                !static_cast<LLVOVolume*>(o)->mIsLocalMesh) { rigged = true; break; }
                        }
                        if (!rigged) continue;
                        const LLUUID item = root->getAttachmentItemID();
                        const std::string name = itemName(root);
                        static std::set<LLUUID> seenDefault;
                        if (!seenDefault.count(item))
                        {
                            seenDefault.insert(item);
                            if (bodyDefaultOff(name)) sBodyOff.insert(item);
                        }
                        rows.append(LLSD().with("item", item.asString()).with("name", name).with("point", att->getName())
                            .with("on", !sBodyOff.count(item)));
                    }
                }
            }
            result["bodies"] = rows;
        }
        else if (cmd == "lab_body")
        {
            const LLUUID item(args["item"].asString());
            if (args["on"].asBoolean()) sBodyOff.erase(item); else sBodyOff.insert(item);
        }
        else if (cmd == "lab_markers")
        {
            sMarkersOn = args["on"].asBoolean();
            showMarkers();
            result["on"] = sMarkersOn;
        }
        else if (cmd == "lab_clip")
        {
            logRenderState("clip");
            result = clipCheck();
        }
        else if (cmd == "lab_devkit_open")
        {
            if (sPicking) return result.with("ok", false).with("error", "busy");
            sPicking = true;
            LLFilePickerReplyThread::startPicker(
                [](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
                {
                    sPicking = false;
                    if (files.empty()) return;
                    // <MG> a devkit .dae, or the body out of a project file (.glb, .fbx, .dae):
                    // loaded on a thread, reported by idle() ("lab_devkit")
                    auto job = std::make_shared<BodyJob>();
                    job->path = files[0];
                    sBodyJob = job;
                    push(LLSD().with("type", "lab_devkit_loading").with("name", baseName(files[0])));
                    const std::string skelPath = gDirUtilp->getExpandedFilename(LL_PATH_CHARACTER, "avatar_skeleton.xml");
                    std::thread([job, skelPath]()
                    {
                        std::vector<MGRig::SlBone> sl;
                        auto body = std::make_shared<MGCreatorDae::File>();
                        std::vector<std::string> pieces;
                        std::string err;
                        const bool ok = MGRig::loadSlSkeleton(skelPath, sl) && MGRigCloth::loadBody(job->path, sl, *body, pieces, err);
                        std::lock_guard<std::mutex> g(job->lock);
                        if (ok) { job->body = body; job->pieces = pieces; }
                        else job->error = err.empty() ? std::string("read") : err;
                        job->done = true;
                    }).detach();
                },
                LLFilePicker::FFLOAD_CHARACTER, false,
                [](const std::vector<std::string>&, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter) { sPicking = false; });
        }
        else if (cmd == "lab_weights")
        {
            // weights on the worn file, saved in the _fit copy (positions unchanged)
            if (sApplied.empty() || sWornFile.isNull()) return result.with("ok", false).with("error", "not_worn");
            const FileWatch* w = watchOf(sWornFile);
            if (!w) return result.with("ok", false).with("error", "file");
            touchStop();
            endRun(true);
            MGCreatorDae::File f;
            if (!MGCreatorDae::read(w->path, f)) return result.with("ok", false).with("error", f.error.empty() ? std::string("read") : f.error);
            if (!f.upAxis.empty() && f.upAxis != "Z_UP") return result.with("ok", false).with("error", "axis");
            if (std::fabs(f.meter - 1.0) > 1e-6) return result.with("ok", false).with("error", "unit");
            const bool copy = args["copy"].asBoolean();
            const bool fix = args["fix"].asBoolean();
            const S32 passes = llclamp(args["smooth"].asInteger(), 0, 8);
            const bool fitted = args["fitted"].asBoolean();
            const F64 share = llclamp(args.has("share") ? args["share"].asReal() : 1.0, 0.0, 1.0);
            const F64 reach = llclamp(args.has("reach_mm") ? args["reach_mm"].asReal() : 100.0, 10.0, 300.0) / 1000.0;
            std::shared_ptr<const MGCreatorDae::File> body = sBody;
            std::vector<MGCreatorWeights::CollisionVolume> cvs;
            MGCreatorWeights::loadCollisionVolumes(gDirUtilp->getExpandedFilename(LL_PATH_CHARACTER, "avatar_skeleton.xml"), cvs);
            // from the body worn: it and the garment are both carried to SL's skeleton to be compared
            const bool fromWorn = copy && args["my_body"].asBoolean();
            MGCreatorDae::File carried;
            if (fromWorn)
            {
                auto mine = std::make_shared<MGCreatorDae::File>();
                std::map<std::string, std::array<double, 3>> bones;
                const std::string e = myBodyOnSkeleton(*mine, bones, cvs);
                if (!e.empty()) return result.with("ok", false).with("error", e);
                body = mine;
                carried = f;
                carried.text.clear();
                MGCreatorWeights::toSkeletonSpace(carried, bones, cvs);
            }
            if (copy && !body) return result.with("ok", false).with("error", "devkit");
            if (fitted && cvs.empty()) return result.with("ok", false).with("error", "skeleton");
            // the original's path: the copy goes next to it; protected points keep their weights
            std::string orig = w->path;
            const size_t dot = orig.find_last_of('.');
            if (dot != std::string::npos && dot >= 4 && orig.compare(dot - 4, 4, "_fit") == 0) orig = orig.substr(0, dot - 4) + orig.substr(dot);
            const std::string out = MGCreatorDae::fitPathFor(orig);
            std::vector<std::vector<U8>> mask(f.meshes.size());
            for (size_t mi = 0; mi < f.meshes.size(); ++mi) mask[mi].assign(f.meshes[mi].vertexCount(), 0);
            {
                std::ifstream pin(out + ".protect");
                size_t pmi, pv;
                while (pin && (pin >> pmi >> pv)) if (pmi < mask.size() && pv < mask[pmi].size()) mask[pmi][pv] = 1;
            }
            std::vector<MGCreatorDae::NewSkin> skins;
            std::vector<std::vector<double>> pos;
            S32 changed = 0, tooFar = 0, added = 0, over4 = 0, unweighted = 0;
            for (size_t mi = 0; mi < f.meshes.size(); ++mi)
            {
                const MGCreatorDae::SkinnedMesh& m = f.meshes[mi];
                MGCreatorWeights::Skin sk = MGCreatorWeights::fromMesh(m);
                const MGCreatorWeights::Skin before = sk;
                const size_t joints0 = sk.joints.size();
                if (copy)
                {
                    static const MGCreatorWeights::Mat IDENT_W = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
                    const MGCreatorWeights::Stats st = fromWorn
                        ? MGCreatorWeights::copyFromDevkit(sk, carried.meshes[mi].positions, IDENT_W, *body, reach, &cvs)
                        : MGCreatorWeights::copyFromDevkit(sk, m.positions, m.bindShape, *body, reach, &cvs);
                    tooFar += (S32)st.tooFar;
                }
                if (fix) MGCreatorWeights::fixUnweighted(sk, m.triangles);
                // smoothing after the conversion softens the borders between slider bones
                if (fitted) MGCreatorWeights::toFitted(sk, m.positions, m.bindShape, cvs, share);
                if (passes > 0) MGCreatorWeights::smooth(sk, m.triangles, passes, 0.5);
                const MGCreatorWeights::Stats lim = MGCreatorWeights::limit(sk);
                over4 += (S32)lim.over4;
                unweighted += (S32)lim.unweighted;
                // protected points: their weights as they were (the old bones keep their indices)
                for (size_t v = 0; v < sk.weights.size() && v < mask[mi].size(); ++v)
                {
                    if (mask[mi][v]) sk.weights[v] = before.weights[v];
                }
                for (size_t v = 0; v < sk.weights.size(); ++v) if (sk.weights[v] != before.weights[v]) ++changed;
                added += (S32)(sk.joints.size() - joints0);
                MGCreatorDae::NewSkin ns;
                ns.joints = sk.joints;
                ns.invBind = sk.invBind;
                ns.weights = sk.weights;
                skins.push_back(ns);
                pos.push_back(m.positions);
            }
            std::string err;
            if (!MGCreatorDae::writeSkinned(f, pos, skins, out, err)) return result.with("ok", false).with("error", err);
            LLSD r = openPath(out);
            if (r["ok"].asBoolean())
            {
                const LLUUID id(r["id"].asString());
                if (id != sWornFile)
                {
                    sFit.pendingFile = id;
                    sFit.standin = sStandIn;
                    sFit.pendingTimer.reset();
                }
            }
            result["name"] = baseName(out);
            result["changed"] = changed;
            result["far"] = tooFar;
            result["added"] = added;
            result["unweighted"] = unweighted;
        }
        // <MG> Versions for all bodies
        else if (cmd == "lab_versions_list")
        {
            LLSD t = LLSD::emptyArray();
            for (const auto& p : sVersionTargets) t.append(baseName(p));
            LLSD made = LLSD::emptyArray();
            for (const auto& p : sVersionOut) made.append(baseName(p));
            result["source"] = sDevkitPath.empty() ? std::string() : baseName(sDevkitPath);
            result["targets"] = t;
            result["made"] = made;
            result["running"] = (bool)sVersionJob;
            result["convert"] = true;                 // this viewer converts characters (lab_convert_open)
            result["rig"] = true;                     // ... and rigs clothes and accessories (lab_rig_*)
            result["placing"] = sPlace.on;             // live placement of a rig preview (lab_place_*)
            result["my_body"] = true;                  // rig mode "mybody": weights from the worn body
            result["converting"] = (bool)sConvertJob;
        }
        else if (cmd == "lab_versions_add")
        {
            // one or more other bodies' devkits
            if (sPicking) return result.with("ok", false).with("error", "busy");
            sPicking = true;
            LLFilePickerReplyThread::startPicker(
                [](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
                {
                    sPicking = false;
                    LLSD bad = LLSD::emptyArray();
                    for (const auto& f : files)
                    {
                        const std::string k = samePathKey(f);
                        if (!sDevkitPath.empty() && k == samePathKey(sDevkitPath)) continue;   // that is body A
                        bool dup = false;
                        for (const auto& t : sVersionTargets) dup = dup || samePathKey(t) == k;
                        if (dup) continue;
                        // a devkit .dae is checked now; a project file (.glb, .fbx) when the versions are made
                        MGCreatorDae::File d;
                        const bool dae = k.size() > 4 && k.compare(k.size() - 4, 4, ".dae") == 0;
                        if (dae && (!MGCreatorDae::read(f, d) || d.meshes.empty() || (!d.upAxis.empty() && d.upAxis != "Z_UP")))
                        {
                            bad.append(baseName(f));
                            continue;
                        }
                        sVersionTargets.push_back(f);
                    }
                    LLSD t = LLSD::emptyArray();
                    for (const auto& p : sVersionTargets) t.append(baseName(p));
                    push(LLSD().with("type", "lab_versions_targets").with("targets", t).with("bad", bad));
                },
                LLFilePicker::FFLOAD_CHARACTER, true,
                [](const std::vector<std::string>&, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter) { sPicking = false; });
        }
        else if (cmd == "lab_versions_remove")
        {
            const S32 i = args["index"].asInteger();
            if (i >= 0 && i < (S32)sVersionTargets.size()) sVersionTargets.erase(sVersionTargets.begin() + i);
            LLSD t = LLSD::emptyArray();
            for (const auto& p : sVersionTargets) t.append(baseName(p));
            result["targets"] = t;
        }
        else if (cmd == "lab_versions")
        {
            // the worn garment (the _fit copy when it is the one), made on body A (the devkit
            // picked in Weights), refitted to each target body: <garment>_for_<body>.dae
            if (sVersionJob) return result.with("ok", false).with("error", "running");
            if (sApplied.empty() || sWornFile.isNull()) return result.with("ok", false).with("error", "not_worn");
            const FileWatch* w = watchOf(sWornFile);
            if (!w) return result.with("ok", false).with("error", "file");
            if (sDevkitPath.empty()) return result.with("ok", false).with("error", "devkit");
            if (sVersionTargets.empty()) return result.with("ok", false).with("error", "targets");
            const bool copy = args["weights"].asBoolean();
            const bool fitted = args["fitted"].asBoolean();
            const F64 share = llclamp(args.has("share") ? args["share"].asReal() : 1.0, 0.0, 1.0);
            std::vector<MGCreatorWeights::CollisionVolume> cvs;
            MGCreatorWeights::loadCollisionVolumes(gDirUtilp->getExpandedFilename(LL_PATH_CHARACTER, "avatar_skeleton.xml"), cvs);
            if (fitted && cvs.empty()) return result.with("ok", false).with("error", "skeleton");
            touchStop();
            endRun(true);
            auto job = std::make_shared<VersionJob>();
            job->total = sVersionTargets.size();
            sVersionJob = job;
            sVersionReported = 0;
            sVersionOut.clear();
            const std::string garmentPath = w->path, bodyA = sDevkitPath;
            const std::shared_ptr<const MGCreatorDae::File> bodyAFile = sBody;
            if (!bodyAFile) return result.with("ok", false).with("error", "devkit");
            const std::string skelPath = gDirUtilp->getExpandedFilename(LL_PATH_CHARACTER, "avatar_skeleton.xml");
            const std::vector<std::string> targets = sVersionTargets;
            std::thread([job, garmentPath, bodyA, bodyAFile, skelPath, targets, copy, fitted, share, cvs]()
            {
                auto finish = [&](VersionDone d)
                {
                    std::lock_guard<std::mutex> g(job->lock);
                    job->done.push_back(d);
                };
                MGCreatorDae::File g;
                const MGCreatorDae::File& a = *bodyAFile;
                std::vector<MGRig::SlBone> sl;
                MGRig::loadSlSkeleton(skelPath, sl);
                const bool okG = MGCreatorDae::read(garmentPath, g), okA = okG && !a.meshes.empty();
                for (const auto& t : targets)
                {
                    VersionDone d;
                    d.body = t.substr(t.find_last_of("/\\") + 1);
                    if (job->cancel) { d.error = "cancelled"; finish(d); continue; }
                    if (!okG) { d.error = "garment"; finish(d); continue; }
                    if (!okA) { d.error = "body_a"; finish(d); continue; }
                    d.out = MGCreatorRefit::versionPathFor(garmentPath, t);
                    // never over one of the files we read
                    const std::string k = samePathKey(d.out);
                    bool clash = k == samePathKey(garmentPath) || k == samePathKey(bodyA);
                    for (const auto& o : targets) clash = clash || k == samePathKey(o);
                    if (clash) { d.error = "name"; finish(d); continue; }
                    MGCreatorDae::File b;
                    std::vector<std::string> bpieces;
                    std::string berr;
                    if (!MGRigCloth::loadBody(t, sl, b, bpieces, berr)) { d.error = berr.empty() ? std::string("read") : berr; finish(d); continue; }
                    std::vector<std::vector<double>> np;
                    std::vector<std::vector<std::array<double, 16>>> ib;
                    MGCreatorRefit::Result r;
                    if (!MGCreatorRefit::refit(g, a, b, np, ib, r)) { d.error = r.error.empty() ? std::string("refit") : r.error; finish(d); continue; }
                    // the version's skin: the garment's weights on B's binds, or B's own weights
                    std::vector<MGCreatorDae::NewSkin> skins;
                    for (size_t mi = 0; mi < g.meshes.size(); ++mi)
                    {
                        const MGCreatorDae::SkinnedMesh& m = g.meshes[mi];
                        MGCreatorWeights::Skin sk = MGCreatorWeights::fromMesh(m);
                        for (size_t j = 0; j < sk.invBind.size() && j < ib[mi].size(); ++j) sk.invBind[j] = ib[mi][j];
                        const size_t joints0 = sk.joints.size();
                        if (copy) MGCreatorWeights::copyFromDevkit(sk, np[mi], m.bindShape, b, 0.10, &cvs);
                        if (fitted && !cvs.empty()) MGCreatorWeights::toFitted(sk, np[mi], m.bindShape, cvs, share);
                        if (copy || fitted) MGCreatorWeights::smooth(sk, m.triangles, 1, 0.5);
                        MGCreatorWeights::limit(sk);
                        d.added += (S32)(sk.joints.size() - joints0);
                        MGCreatorDae::NewSkin ns;
                        ns.joints = sk.joints;
                        ns.invBind = sk.invBind;
                        ns.weights = sk.weights;
                        skins.push_back(ns);
                    }
                    std::string err;
                    if (!MGCreatorDae::writeSkinned(g, np, skins, d.out, err)) { d.error = err.empty() ? std::string("write") : err; finish(d); continue; }
                    d.surface = (S32)r.surface;
                    d.followed = (S32)r.followed;
                    d.bonesOnly = (S32)r.bonesOnly;
                    d.meanMm = r.meanMove * 1000.0;
                    d.maxMm = r.maxMove * 1000.0;
                    finish(d);
                }
                std::lock_guard<std::mutex> g2(job->lock);
                job->finished = true;
            }).detach();
            result["total"] = (S32)job->total;
            result["source"] = baseName(bodyA);
        }
        else if (cmd == "lab_versions_cancel")
        {
            if (sVersionJob) sVersionJob->cancel = true;
        }
        else if (cmd == "lab_version_try")
        {
            // a version on the stand-in (it opens in Local Mesh, then is worn)
            const S32 i = args["index"].asInteger();
            if (i < 0 || i >= (S32)sVersionOut.size()) return result.with("ok", false).with("error", "index");
            if (sStandIn.isNull()) return result.with("ok", false).with("error", "no_standin");
            touchStop();
            endRun(true);
            LLSD r = openPath(sVersionOut[i]);
            if (!r["ok"].asBoolean()) return r;
            const LLUUID id(r["id"].asString());
            if (id != sWornFile)
            {
                sFit.pendingFile = id;
                sFit.standin = sStandIn;
                sFit.pendingTimer.reset();
            }
            result["name"] = baseName(sVersionOut[i]);
        }
        // <MG> Rig clothes & accessories: open (analyse) -> "lab_rig_analysis"; run -> "lab_rig_done"
        else if (cmd == "lab_rig_open")
        {
            if (sPicking) return result.with("ok", false).with("error", "busy");
            if (sRigJob)
            {
                std::lock_guard<std::mutex> g(sRigJob->lock);
                if (sRigJob->phase == 1 || sRigJob->phase == 3) return result.with("ok", false).with("error", "running");
            }
            const std::string devkitPath = sDevkitPath;
            const std::string skelPath = gDirUtilp->getExpandedFilename(LL_PATH_CHARACTER, "avatar_skeleton.xml");
            sPicking = true;
            LLFilePickerReplyThread::startPicker(
                [devkitPath, skelPath](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
                {
                    sPicking = false;
                    if (files.empty()) return;
                    auto job = std::make_shared<RigJob>();
                    job->source = files[0];
                    job->body = sBody;
                    sRigJob = job;
                    push(LLSD().with("type", "lab_rig_start").with("name", baseName(files[0])));
                    std::thread([job, devkitPath, skelPath]()
                    {
                        auto fail = [&](const std::string& e) { std::lock_guard<std::mutex> g(job->lock); job->error = e; job->phase = 5; };
                        if (!MGRig::readModel(job->source, job->model, true)) { fail(job->model.error.empty() ? std::string("read") : job->model.error); return; }
                        if (!MGRig::loadSlSkeleton(skelPath, job->sl)) { fail("skeleton"); return; }
                        MGRigCloth::analyse(job->model, job->body ? job->body.get() : nullptr, job->sl, job->items);
                        std::lock_guard<std::mutex> g(job->lock);
                        job->phase = 2;
                    }).detach();
                },
                LLFilePicker::FFLOAD_CHARACTER, false,
                [](const std::vector<std::string>&, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter) { sPicking = false; });
        }
        else if (cmd == "lab_rig_run")
        {
            if (!sRigJob) return result.with("ok", false).with("error", "no_file");
            auto job = sRigJob;
            {
                std::lock_guard<std::mutex> g(job->lock);
                if (job->phase != 2) return result.with("ok", false).with("error", "running");
                job->phase = 3;
                job->reportedPhase = 3;
                job->error.clear();
                job->body = sBody;   // the body chosen now (it may have been chosen after opening the file)
                job->standin = args.has("standin") ? LLUUID(args["standin"].asString()) : LLUUID::null;
            }
            std::vector<MGRigCloth::Choice> choices;
            const LLSD& items = args["items"];
            for (size_t i = 0; i < job->items.size(); ++i)
            {
                MGRigCloth::Choice c;
                c.mode = job->items[i].mode;
                c.bone = job->items[i].bone;
                if (items.isArray() && i < (size_t)items.size())
                {
                    const LLSD& it = items[(S32)i];
                    if (it.has("mode")) c.mode = it["mode"].asString();
                    if (it.has("bone")) c.bone = it["bone"].asString();
                    if (it.has("distance_mm")) c.distance = llclamp(it["distance_mm"].asReal(), 5.0, 500.0) / 1000.0;
                    if (it.has("mix")) c.mix = llclamp(it["mix"].asReal(), 0.0, 1.0);
                    if (it.has("smooth")) c.smooth = llclamp(it["smooth"].asInteger(), 0, 8);
                }
                choices.push_back(c);
            }
            MGRigCloth::Options opt;
            opt.fitted = args["fitted"].asBoolean();
            opt.share = llclamp(args.has("share") ? args["share"].asReal() : 1.0, 0.0, 1.0);
            // Place on the body: move (cm: forward, left, up), turn (degrees), size (1 = as made)
            if (args.has("move") && args["move"].isArray() && args["move"].size() >= 3)
                for (S32 k = 0; k < 3; ++k) opt.move[k] = llclamp(args["move"][k].asReal(), -300.0, 300.0) / 100.0;
            opt.turn = llclamp(args.has("turn") ? args["turn"].asReal() : 0.0, -180.0, 180.0);
            opt.scale = llclamp(args.has("scale") ? args["scale"].asReal() : 1.0, 0.05, 20.0);
            // Skin to my body: the worn body read now, on this thread (it touches the objects)
            std::shared_ptr<MGCreatorDae::File> mine;
            if (std::any_of(choices.begin(), choices.end(), [](const MGRigCloth::Choice& c) { return c.mode == "mybody"; }))
            {
                std::set<LLUUID> garment;
                for (const auto& a : sApplied) garment.insert(a.prim);
                mine = std::make_shared<MGCreatorDae::File>();
                std::string e = myBodyFile(*mine, garment);
                if (e.empty())
                {
                    // the body as it stands on SL's skeleton (its own bind space can be any size)
                    std::map<std::string, std::array<double, 3>> bones;
                    for (const auto& b : job->sl) if (!b.volume) bones[b.name] = { b.world[0], b.world[1], b.world[2] };
                    std::vector<MGCreatorWeights::CollisionVolume> bcv;
                    MGCreatorWeights::loadCollisionVolumes(gDirUtilp->getExpandedFilename(LL_PATH_CHARACTER, "avatar_skeleton.xml"), bcv);
                    MGCreatorWeights::toSkeletonSpace(*mine, bones, bcv);
                    double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
                    for (const auto& m : mine->meshes)
                        for (size_t v = 0; v < m.vertexCount(); ++v)
                            if (v < m.weights.size() && !m.weights[v].empty())
                                for (int q = 0; q < 3; ++q) { lo[q] = std::min(lo[q], m.positions[v * 3 + q]); hi[q] = std::max(hi[q], m.positions[v * 3 + q]); }
                    LL_INFOS("MGCreator") << "Skin to my body: on SL's skeleton " << lo[0] << "," << lo[1] << "," << lo[2] << " .. " << hi[0] << "," << hi[1] << "," << hi[2] << LL_ENDL;
                    if (hi[2] - lo[2] < 0.5 || hi[2] - lo[2] > 4.0) e = "my_body";
                }
                if (!e.empty())
                {
                    std::lock_guard<std::mutex> g(job->lock);
                    job->phase = 2;
                    return result.with("ok", false).with("error", e);
                }
            }
            // the preview is not rigged yet: it hangs from the pelvis only, as modelled (it does not bend
            // with the limbs or the animations while it is placed or the avatar posed); the final rig takes
            // the pose, if any, and brings the garment back from it
            if (args["preview"].asBoolean())
                for (auto& c : choices) { c.mode = "bone"; c.bone = "mPelvis"; }
            if (!args["preview"].asBoolean() && args["pose"].asBoolean() && MGPhonePoser::isPosing())
            {
                opt.pose = poseMoves();
                LL_INFOS("MGCreator") << "Rig: the avatar's pose taken (" << opt.pose.size() << " bones); the garment goes back to the rest pose" << LL_ENDL;
            }
            std::shared_ptr<ShownBake> bake;
            // a preview is rigged where it was made; the placement is then shown live (no file)
            job->preview = args["preview"].asBoolean();
            for (int k = 0; k < 3; ++k) job->pmove[k] = (F32)opt.move[k];
            job->pturn = (F32)opt.turn;
            job->pscale = (F32)opt.scale;
            if (job->preview) { opt.move[0] = opt.move[1] = opt.move[2] = 0.0; opt.turn = 0.0; opt.scale = 1.0; }
            else
            {
                // the placement shown live, exactly (any axis, stretch): one matrix, baked into the file
                if (sPlace.on)
                {
                    // what is shown, as shown: baked after the rig (the pose, if any, only guides the weights)
                    bake = shownBake(args["pose"].asBoolean() && MGPhonePoser::isPosing());
                    if (bake) { opt.unpose = false; opt.pose.clear(); opt.shown = shownInGarment(*bake); }
                    const glm::mat4 P = placeMatrix();
                    opt.hasMatrix = true;
                    const F32* pp = glm::value_ptr(P);
                    for (int k = 0; k < 16; ++k) opt.matrix[k] = pp[k];
                }
                placeEnd(false);   // the final file carries the placement: the reload must not get it twice
            }
            std::vector<MGCreatorWeights::CollisionVolume> cvs;
            MGCreatorWeights::loadCollisionVolumes(gDirUtilp->getExpandedFilename(LL_PATH_CHARACTER, "avatar_skeleton.xml"), cvs);
            std::thread([job, choices, opt, cvs, mine, bake]()
            {
                MGRig::Model out;
                MGRigCloth::Result r;
                std::string err;
                std::string path;
                const auto rigT0 = std::chrono::steady_clock::now();
                struct RigTimeLog
                {
                    std::chrono::steady_clock::time_point t0;
                    const MGRigCloth::Result* res;
                    ~RigTimeLog() { LL_INFOS("MGCreator") << "Rig took " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() << " ms; " << res->farPoints << " points out of the body's reach" << LL_ENDL; }
                } rigTimeLog{ rigT0, &r };
                if (MGRigCloth::rig(job->model, choices, job->body ? job->body.get() : nullptr, job->sl, cvs, opt, out, r, err, mine.get()))
                {
                    if (bake) bakeShown(out, *bake);   // where it showed, through its new bones
                    // never over the file it read; only over a file this tool wrote
                    path = convertOutPath(job->source, MGRigCloth::outPathFor(job->source));
                    if (path.empty()) err = "name";
                    else if (!MGRig::writeDae(out, path, err) && err.empty()) err = "write";
                }
                std::lock_guard<std::mutex> g(job->lock);
                job->result = r;
                job->error = err;
                job->out = path;
                job->phase = 4;
            }).detach();
            result["items"] = (S32)choices.size();
        }
        // <MG> live placement of the rig preview: {move[cm forward, left, up], turn, size %}
        else if (cmd == "lab_place_set")
        {
            if (!sPlace.on) return result.with("ok", false).with("error", "not_placing");
            if (args.has("move") && args["move"].isArray() && args["move"].size() >= 3)
                for (S32 k = 0; k < 3; ++k) sPlace.move[k] = llclamp((F32)args["move"][k].asReal(), -300.f, 300.f) / 100.f;
            if (args.has("turn")) sPlace.turn = llclamp((F32)args["turn"].asReal(), -180.f, 180.f);
            if (args.has("size")) sPlace.scale = llclamp((F32)args["size"].asReal(), 5.f, 2000.f) / 100.f;
            if (args.has("tilt") && args["tilt"].isArray() && args["tilt"].size() >= 2)
                for (S32 k = 0; k < 2; ++k) sPlace.tilt[k] = llclamp((F32)args["tilt"][k].asReal(), -180.f, 180.f);
            if (args.has("axis") && args["axis"].isArray() && args["axis"].size() >= 3)
                for (S32 k = 0; k < 3; ++k) sPlace.axis[k] = llclamp((F32)args["axis"][k].asReal(), 5.f, 2000.f) / 100.f;
            placeApply();
        }
        else if (cmd == "lab_place_tool")
        {
            if (!sPlace.on) return result.with("ok", false).with("error", "not_placing");
            if (args.has("mode")) sPlace.mode = llclamp(args["mode"].asInteger(), 0, 2);
            if (!args.has("on") || args["on"].asBoolean()) placeToolOn(); else placeToolOff();
        }
        else if (cmd == "lab_pose")
        {
            // pose the avatar to fit the garment (the Camera's poser): presets, bones with the poser's gizmo
            const std::string act = args["act"].asString();
            auto poser = [&](const std::string& c, const LLSD& a) { return MGPhonePoser::runCommand(c, a); };
            if (act == "preset")
            {
                const LLSD r = poser("pose_preset", LLSD().with("name", args["name"].asString()));
                if (!r["ok"].asBoolean()) return r;
            }
            else if (act == "bones")
            {
                const LLSD r = poser("pose_start", LLSD());
                if (!r["ok"].asBoolean()) return r;
                placeToolOff();
                sPoseBones = true;
                poser("pose_gizmo_mode", LLSD().with("mode", "rotate"));   // turning bones is what fits a pose
                poseSelect(args.has("joint") ? args["joint"].asString() : std::string("mShoulderLeft"));
                showMarkers();
                poseToolOn();   // after the poser's tool change: Creator Lab keeps the mouse (joints, rings, the garment)
            }
            else if (act == "joint")
            {
                // the rings onto it; the poser's tool is not switched again (that dropped ours)
                if (!sPoseBones) poser("pose_gizmo_mode", LLSD().with("mode", "rotate"));
                poseSelect(args["joint"].asString());
                if (sPoseBones) poseToolOn();
            }
            else if (act == "gizmo")
            {
                poser("pose_gizmo_mode", LLSD().with("mode", args["gizmo"].asString()));
                if (sPoseBones) poseToolOn();
            }
            else if (act == "place" || act == "stop")
            {
                // back to placing the garment (the pose stays), or the avatar let go (its animations again)
                poseToolOff();
                poser("pose_gizmo_mode", LLSD().with("mode", "off"));
                poser("pose_markers", LLSD().with("on", false));
                sPoseBones = false;
                showMarkers();
                if (act == "stop") poser("pose_stop", LLSD());
                if (sPlace.on) placeToolOn();
            }
            result["posing"] = MGPhonePoser::isPosing();
            result["bones"] = sPoseBones;
            if (act == "bones")
            {
                // the body's joints for the page's list
                const LLSD st = poser("pose_state", LLSD());
                LLSD list = LLSD::emptyArray();
                for (LLSD::array_const_iterator it = st["joints"].beginArray(); it != st["joints"].endArray(); ++it)
                    if ((*it)["g"].asString() == "body") list.append(LLSD().with("n", (*it)["n"]).with("label", (*it)["label"]));
                result["joints"] = list;
            }
        }
        else if (cmd == "lab_place_end")
        {
            placeEnd(args["restore"].asBoolean());
        }
        else if (cmd == "lab_rig_cancel")
        {
            placeEnd(true);   // what the stand-in shows is the file again
            sRigJob.reset();   // a running thread keeps its own copy of the job
        }
        // <MG> Convert to SL: pick a character (fbx, glb, gltf, vrm); "lab_convert" when done
        else if (cmd == "lab_convert_open")
        {
            if (sPicking) return result.with("ok", false).with("error", "busy");
            if (sConvertJob) return result.with("ok", false).with("error", "running");
            MGRig::Options opt;
            opt.tpose = !args.has("tpose") || args["tpose"].asBoolean();
            opt.jointPositions = !args.has("joint_positions") || args["joint_positions"].asBoolean();
            const std::string mapsPath = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "mgrigmaps.txt");
            const std::string skelPath = gDirUtilp->getExpandedFilename(LL_PATH_CHARACTER, "avatar_skeleton.xml");
            sPicking = true;
            LLFilePickerReplyThread::startPicker(
                [opt, mapsPath, skelPath](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
                {
                    sPicking = false;
                    if (files.empty() || sConvertJob) return;
                    auto job = std::make_shared<ConvertJob>();
                    job->source = files[0];
                    sConvertJob = job;
                    push(LLSD().with("type", "lab_convert_start").with("name", baseName(files[0])));
                    std::thread([job, opt, mapsPath, skelPath]()
                    {
                        auto done = [&](const std::string& err)
                        {
                            std::lock_guard<std::mutex> g(job->lock);
                            job->error = err;
                            job->finished = true;
                        };
                        MGRig::Model in;
                        if (!MGRig::readModel(job->source, in)) { done(in.error.empty() ? std::string("read") : in.error); return; }
                        job->notes = in.notes;
                        std::vector<MGRig::BoneMap> maps;
                        if (!MGRig::loadMaps(mapsPath, maps)) { done("maps"); return; }
                        std::vector<MGRig::SlBone> sl;
                        if (!MGRig::loadSlSkeleton(skelPath, sl)) { done("skeleton"); return; }
                        job->detection = MGRig::detect(in, maps);
                        if (job->detection.map < 0) { done("no_map"); return; }
                        MGRig::Model out;
                        if (!MGRig::convert(in, maps[job->detection.map], sl, opt, out, job->report)) { done(out.error.empty() ? std::string("convert") : out.error); return; }
                        if (out.meshes.empty()) { done("nothing"); return; }
                        job->out = convertOutPath(job->source);
                        if (job->out.empty()) { done("name"); return; }
                        std::string err;
                        if (!MGRig::writeDae(out, job->out, err)) { done(err.empty() ? std::string("write") : err); return; }
                        done(std::string());
                    }).detach();
                },
                LLFilePicker::FFLOAD_CHARACTER, false,
                [](const std::vector<std::string>&, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter) { sPicking = false; });
        }
        else if (cmd == "lab_upload")
        {
            // the worn file (the _fit copy when it is the one) in the mesh uploader
            const FileWatch* w = watchOf(sWornFile.notNull() ? sWornFile : LLUUID(args["file"].asString()));
            if (!w) return result.with("ok", false).with("error", "file");
            LLFloaterModelPreview* fmp = (LLFloaterModelPreview*)LLFloaterReg::getInstance("upload_model");
            if (!fmp) return result.with("ok", false).with("error", "uploader");
            if (fmp->isModelLoading()) return result.with("ok", false).with("error", "busy");
            LLFloaterReg::showInstance("upload_model");
            fmp->loadModel(LLModel::LOD_HIGH, w->path);
            result["name"] = baseName(w->path);
        }
        else if (cmd == "lab_touch_start")
        {
            if (sApplied.empty() || sWornFile.isNull()) return result.with("ok", false).with("error", "not_worn");
            const FileWatch* w = watchOf(sWornFile);
            if (!w) return result.with("ok", false).with("error", "file");
            endRun(true);
            const std::string err = touchStart(w->path);
            if (!err.empty()) { touchStop(); return result.with("ok", false).with("error", err); }
            result["out"] = baseName(sTouch.outPath);
            result["live"] = sTouch.live;   // the garment follows the brushes as they are used
            {
                // the bones: this garment's (most used first), then all of SL's
                std::map<std::string, double> use;
                for (const auto& sk : sTouch.skin)
                    for (const auto& w : sk.weights)
                        for (const auto& jw : w)
                            if (jw.first >= 0 && jw.first < (int)sk.joints.size()) use[sk.joints[jw.first]] += jw.second;
                std::vector<std::pair<double, std::string>> order;
                for (const auto& kv : use) order.push_back({ kv.second, kv.first });
                std::sort(order.begin(), order.end(), [](const std::pair<double, std::string>& a, const std::pair<double, std::string>& b) { return a.first > b.first; });
                LLSD mine = LLSD::emptyArray(), all = LLSD::emptyArray();
                for (const auto& o : order) mine.append(o.second);
                for (const auto& b : slSkeleton()) if (b.name.size() > 1) all.append(b.name);
                result["bones"] = mine;
                result["all_bones"] = all;
                if (sTouch.bone.empty() && !order.empty()) sTouch.bone = order[0].second;
                result["bone"] = sTouch.bone;
            }
            S32 prot = 0;
            for (const auto& m : sTouch.mask) for (U8 b : m) prot += b;
            result["protected"] = prot;
        }
        else if (cmd == "lab_touch_set")
        {
            if (args.has("brush"))
            {
                const std::string b = args["brush"].asString();
                sTouch.brush = b == "smooth" ? TB_SMOOTH : b == "inflate" ? TB_INFLATE : b == "grab" ? TB_GRAB : b == "protect" ? TB_PROTECT
                             : b == "weight" ? TB_WEIGHT : b == "wsmooth" ? TB_WSMOOTH : b == "stiff" ? TB_STIFF : b == "legs" ? TB_LEGS : b == "erase" ? TB_ERASE
                             : b == "select" ? TB_SELECT : TB_PUSH;
                if (sTouch.brush == TB_SELECT && sTouch.active) { editEnsure(); editPushState(); }
            }
            if (args.has("bone")) sTouch.bone = args["bone"].asString();
            if (args.has("size_mm")) sTouch.radius = llclamp((F32)args["size_mm"].asReal(), 5.f, 200.f) / 1000.f;
            if (args.has("strength")) sTouch.strength = llclamp((F32)args["strength"].asReal(), 0.f, 1.f);
            if (args.has("symmetry")) sTouch.symmetry = args["symmetry"].asBoolean();
            if (args.has("show_bones")) sTouch.showBones = args["show_bones"].asBoolean();
            if (args.has("stiff_auto") && args["stiff_auto"].asBoolean()) { sTouch.stiffManual = false; sTouch.stiffBone.clear(); }
        }
        else if (cmd == "lab_touch_undo" || cmd == "lab_touch_redo")
        {
            if (!sTouch.active) return result.with("ok", false).with("error", "inactive");
            auto& from = cmd == "lab_touch_undo" ? sTouch.undo : sTouch.redo;
            auto& to = cmd == "lab_touch_undo" ? sTouch.redo : sTouch.undo;
            if (from.empty()) return result.with("ok", false).with("error", "empty");
            TouchSnap now;
            now.pos = sTouch.pos;
            now.skin = sTouch.skin;
            to.push_back(now);
            sTouch.pos = from.back().pos;
            if (!from.back().skin.empty() && from.back().skin.size() == sTouch.skin.size())
            {
                sTouch.skin = from.back().skin;
                for (size_t mi = 0; mi < sTouch.skin.size(); ++mi) repack(mi);
            }
            from.pop_back();
            touchSkin();
            sTouch.liveDirty = true;
            sWV.dirty = true;
            touchSave();
        }
        else if (cmd == "lab_touch_skirt")
        {
            if (!sTouch.active) return result.with("ok", false).with("error", "inactive");
            TouchSnap before;
            before.pos = sTouch.pos;
            before.skin = sTouch.skin;
            const S32 n = touchSkirt(llclamp(args["legs"].asReal(), 0.0, 100.0) / 100.0);
            if (n <= 0) return result.with("ok", false).with("error", "skirt");
            sTouch.undo.push_back(before);
            if (sTouch.undo.size() > 20) sTouch.undo.erase(sTouch.undo.begin());
            sTouch.redo.clear();
            touchSkin();
            sTouch.liveDirty = true;
            sWV.dirty = true;
            touchSave();
            result["points"] = n;
        }
        else if (cmd == "lab_touch_clearmask")
        {
            for (auto& m : sTouch.mask) std::fill(m.begin(), m.end(), (U8)0);
            if (sTouch.active) saveMask(sTouch.maskPath, sTouch.mask);
        }
        // <MG> Look: {what: get|set|view|pick|clear|flash|copyall|folder|reset, index, ...}
        else if (cmd == "lab_look")
        {
            if (sApplied.empty() || sWornFile.isNull()) return result.with("ok", false).with("error", "not_worn");
            lookEnsure();
            const std::string what = args["what"].asString();
            const S32 index = args["index"].asInteger();
            const bool valid = index >= 0 && index < (S32)sLook.faces.size();
            if (what == "set" && valid)
            {
                lookFaceFrom(sLook.faces[index], args["face"]);
                if (args["face"].has("pbr") || args["face"].has("base_color") || args["face"].has("metallic") || args["face"].has("roughness") || args["face"].has("emissive")
                    || args["face"].has("alpha_mode") || args["face"].has("cutoff") || args["face"].has("double_sided"))
                    if (sLook.faces[index].pbr) lookWriteGltf(sLook.faces[index], true);
                lookSave();
                lookApplyFace((size_t)index);
            }
            else if (what == "view")
            {
                sLook.view = llclamp(args["view"].asInteger(), 0, 2);
                lookSave();
                lookApplyAll();
            }
            else if (what == "clear" && valid)
            {
                // a texture taken away: { slot }
                LookFace& f = sLook.faces[index];
                const std::string slot = args["slot"].asString();
                std::string* p = slot == "diffuse" ? &f.diffuse : slot == "normal" ? &f.normal : slot == "specular" ? &f.specular : slot == "base_tex" ? &f.baseTex
                               : slot == "norm_tex" ? &f.normTex : slot == "orm_tex" ? &f.ormTex : slot == "emis_tex" ? &f.emisTex : slot == "opacity" ? &f.opacity
                               : slot == "ao_tex" ? &f.aoTex : slot == "metal_tex" ? &f.metalTex : slot == "rough_tex" ? &f.roughTex : nullptr;
                if (p) p->clear();
                if (f.pbr) lookWriteGltf(f, true);
                lookSave();
                lookApplyFace((size_t)index);
            }
            else if (what == "pick" && valid)
            {
                // a texture from a file: { slot }; the page hears "lab_look" when it is set
                if (sPicking) return result.with("ok", false).with("error", "busy");
                sPicking = true;
                const std::string slot = args["slot"].asString();
                const LLUUID worn = sWornFile;
                LLFilePickerReplyThread::startPicker(
                    [index, slot, worn](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
                    {
                        sPicking = false;
                        if (files.empty() || worn != sWornFile || index >= (S32)sLook.faces.size()) return;
                        LookFace& f = sLook.faces[index];
                        LLSD s; s[slot] = files[0];
                        if (slot == "base_tex" || slot == "norm_tex" || slot == "orm_tex" || slot == "emis_tex" || slot == "ao_tex" || slot == "metal_tex" || slot == "rough_tex") s["pbr"] = true;
                        lookFaceFrom(f, s);
                        if (f.pbr) lookWriteGltf(f, true);
                        lookSave();
                        lookApplyFace((size_t)index);
                        push(LLSD().with("type", "lab_look").with("index", index).with("face", lookFaceSD(f)));
                    },
                    LLFilePicker::FFLOAD_IMAGE, false,
                    [](const std::vector<std::string>&, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter) { sPicking = false; });
            }
            else if (what == "flash" && valid)
            {
                sLook.flash = index;
                sLook.flashTimer.reset();
                lookApplyFace((size_t)index);
            }
            else if (what == "copyall" && valid)
            {
                // this face's look on every face (their own names and places kept)
                const LookFace src = sLook.faces[index];
                for (size_t i = 0; i < sLook.faces.size(); ++i)
                {
                    if ((S32)i == index) continue;
                    LookFace& f = sLook.faces[i];
                    lookFaceFrom(f, lookFaceSD(src));
                    f.gltfWorld.setNull();
                }
                lookSave();
                lookApplyAll();
            }
            else if (what == "reset" && valid)
            {
                LookFace& f = sLook.faces[index];
                LookFace fresh;
                fresh.prim = f.prim; fresh.te = f.te; fresh.name = f.name;
                f = fresh;
                lookSave();
                lookApplyFace((size_t)index);
            }
            else if (what == "thumb")
            {
                // a small picture of a texture file for the page: { path } -> { uri }
                result["uri"] = lookThumb(args["path"].asString());
                return result;
            }
            else if (what == "folder")
            {
                if (sLook.base.empty()) return result.with("ok", false).with("error", "no_file");
                const std::string dir = sLook.base + "_look";
                LLFile::mkdir(dir);
                if (gViewerWindow && gViewerWindow->getWindow()) gViewerWindow->getWindow()->openFile(dir);
            }
            result = result.with("look", lookState());
        }
        // <MG> the workspace's toolbar in the 3D view: {items: [{id, icon, tip, on, sep}], hint}; [] hides it
        else if (cmd == "lab_bar")
        {
            sBar.clear();
            for (LLSD::array_const_iterator it = args["items"].beginArray(); it != args["items"].endArray(); ++it)
            {
                BarItem b;
                b.id = (*it)["id"].asString(); b.icon = (*it)["icon"].asString(); b.tip = (*it)["tip"].asString();
                b.on = (*it)["on"].asBoolean(); b.sep = (*it)["sep"].asBoolean();
                sBar.push_back(b);
            }
            sBarHint = args["hint"].asString();
            sBarTimer.reset();
            showBar();
        }
        // <MG> Edit Mesh and the garment's pieces (Touch up's session): {what, ...}
        else if (cmd == "lab_edit")
        {
            if (!sTouch.active) return result.with("ok", false).with("error", "inactive");
            editEnsure();
            const std::string what = args["what"].asString();
            const MASK op = args["op"].asString() == "add" ? MASK_SHIFT : args["op"].asString() == "remove" ? MASK_CONTROL : MASK_NONE;
            if (what == "parts")
            {
                // the file's pieces (meshes): name, points, how much is selected / hidden
                LLSD list = LLSD::emptyArray();
                for (size_t mi = 0; mi < sTouch.dae.meshes.size(); ++mi)
                {
                    S32 s = 0, h = 0;
                    for (U8 b : sTouch.sel[mi]) s += b;
                    for (U8 b : sTouch.hidden[mi]) h += b;
                    const auto& m = sTouch.dae.meshes[mi];
                    list.append(LLSD().with("name", m.name.empty() ? llformat("Piece %d", (S32)mi + 1) : m.name).with("points", (S32)m.vertexCount())
                        .with("tris", (S32)m.triangles.size()).with("sel", s).with("hidden", h));
                }
                result["parts"] = list;
            }
            else if (what == "set")
            {
                if (args.has("mode")) sTouch.selMode = llclamp(args["mode"].asInteger(), 0, 3);
                if (args.has("gizmo")) sTouch.gizmo = llclamp(args["gizmo"].asInteger(), 0, 2);
                if (args.has("soft")) sTouch.softOn = args["soft"].asBoolean();
                if (args.has("soft_mm")) sTouch.softR = llclamp((F32)args["soft_mm"].asReal(), 2.f, 300.f) / 1000.f;
                if (args.has("mirror")) sTouch.symmetry = args["mirror"].asBoolean();
                if (args.has("xray")) sTouch.xray = args["xray"].asBoolean();
                if (args.has("edges")) sTouch.showEdges = args["edges"].asBoolean();
                if (args.has("points")) sTouch.showPoints = args["points"].asBoolean();
            }
            else if (what == "all" || what == "none" || what == "invert")
            {
                for (size_t mi = 0; mi < sTouch.sel.size(); ++mi)
                    for (size_t v = 0; v < sTouch.sel[mi].size(); ++v)
                        sTouch.sel[mi][v] = sTouch.hidden[mi][v] ? 0 : what == "all" ? 1 : what == "none" ? 0 : (U8)!sTouch.sel[mi][v];
            }
            else if (what == "grow" || what == "shrink") editGrow(what == "grow");
            else if (what == "loop")
            {
                if (sTouch.lastA < 0 || sTouch.lastMesh >= sTouch.world.size()) return result.with("ok", false).with("error", "no_edge");
                std::vector<std::pair<size_t, S32>> pts;
                for (S32 v : editLoop(sTouch.lastMesh, sTouch.lastA, sTouch.lastB)) pts.push_back({ sTouch.lastMesh, v });
                editApplyOp(pts, MASK_SHIFT);
                result["points"] = (S32)pts.size();
            }
            else if (what == "under")
            {
                const S32 n = editUnderSkin(op);
                if (n < 0) return result.with("ok", false).with("error", "no_body");
                result["points"] = n;
            }
            else if (what == "piece")
            {
                // a piece (mesh) of the file: select it all, hide / show it, or show only it
                const size_t mi = (size_t)llmax(0, args["index"].asInteger());
                if (mi >= sTouch.sel.size()) return result.with("ok", false).with("error", "index");
                const std::string act = args["act"].asString();
                if (act == "select")
                {
                    std::vector<std::pair<size_t, S32>> pts;
                    for (size_t v = 0; v < sTouch.sel[mi].size(); ++v) pts.push_back({ mi, (S32)v });
                    editApplyOp(pts, op);
                }
                else if (act == "hide" || act == "show")
                {
                    std::fill(sTouch.hidden[mi].begin(), sTouch.hidden[mi].end(), (U8)(act == "hide"));
                    if (act == "hide") std::fill(sTouch.sel[mi].begin(), sTouch.sel[mi].end(), (U8)0);
                    editApplyHidden();
                }
                else if (act == "solo")
                {
                    for (size_t k = 0; k < sTouch.hidden.size(); ++k)
                    {
                        std::fill(sTouch.hidden[k].begin(), sTouch.hidden[k].end(), (U8)(k != mi));
                        if (k != mi) std::fill(sTouch.sel[k].begin(), sTouch.sel[k].end(), (U8)0);
                    }
                    editApplyHidden();
                }
            }
            else if (what == "protect")
            {
                // keep the selection out of Drape (and Make it fit): the protected points, saved next to the
                // _fit copy; Drape pins them when it starts (they follow the body, they do not drape)
                if (!editCount()) return result.with("ok", false).with("error", "nothing_selected");
                const U8 on = args["on"].asBoolean() ? 1 : 0;
                S32 n = 0;
                for (size_t mi = 0; mi < sTouch.sel.size() && mi < sTouch.mask.size(); ++mi)
                    for (size_t v = 0; v < sTouch.sel[mi].size() && v < sTouch.mask[mi].size(); ++v)
                        if (sTouch.sel[mi][v]) { sTouch.mask[mi][v] = on; ++n; }
                saveMask(sTouch.maskPath, sTouch.mask);
                result["points"] = n;
            }
            else if (what == "hide") editHide(true);
            else if (what == "unhide") editHide(false);
            else if (what == "isolate")
            {
                // only the selection stays shown
                for (size_t mi = 0; mi < sTouch.sel.size(); ++mi)
                    for (size_t v = 0; v < sTouch.sel[mi].size(); ++v) sTouch.hidden[mi][v] = sTouch.sel[mi][v] ? 0 : 1;
                editApplyHidden();
            }
            else if (what == "push")
            {
                if (!editCount()) return result.with("ok", false).with("error", "nothing_selected");
                editPush(llclamp((F32)args["mm"].asReal(), -50.f, 50.f));
            }
            else if (what == "smooth")
            {
                if (!editCount()) return result.with("ok", false).with("error", "nothing_selected");
                editSmooth(llclamp((F32)args["amount"].asReal(), 0.05f, 1.f));
            }
            else if (what == "weights")
            {
                if (!editCount()) return result.with("ok", false).with("error", "nothing_selected");
                const S32 n = editSetWeights(args["bone"].asString());
                if (!n) return result.with("ok", false).with("error", "bone");
                result["points"] = n;
            }
            editPushState();
            editUiPush();
            result["count"] = editCount();
        }
        // <MG> Drape
        else if (cmd == "lab_drape_start")
        {
            if (sApplied.empty() || sWornFile.isNull()) return result.with("ok", false).with("error", "not_worn");
            const FileWatch* w = watchOf(sWornFile);
            if (!w) return result.with("ok", false).with("error", "file");
            if (sPlace.on) return result.with("ok", false).with("error", "placing");
            endRun(true);
            const std::string err = drapeStart(w->path, drapeSettings(args, MGDrape::Settings()));
            if (!err.empty()) { drapeStop(); return result.with("ok", false).with("error", err); }
            if (args.has("tool")) sDrape.tool = llclamp(args["tool"].asInteger(), 0, 4);
            if (args.has("size_mm")) sDrape.radius = llclamp((F32)args["size_mm"].asReal(), 5.f, 300.f) / 1000.f;
            result["points"] = (S32)(sDrape.job->start.size() / 3);
            result["out"] = baseName(sDrape.outPath);
            LLSD names = LLSD::emptyArray();
            for (const auto& nm : sDrape.bodyNames) names.append(nm);
            result["body"] = names;   // the worn items it drapes on (Stress test switches them)
            result["preview"] = sPreview.on;   // the garment itself follows the cloth (else dots)
        }
        else if (cmd == "lab_drape_set")
        {
            if (!sDrape.active) return result.with("ok", false).with("error", "inactive");
            if (args.has("tool")) sDrape.tool = llclamp(args["tool"].asInteger(), 0, 4);
            if (args.has("size_mm")) sDrape.radius = llclamp((F32)args["size_mm"].asReal(), 5.f, 300.f) / 1000.f;
            DrapeCmd c;
            c.kind = DrapeCmd::SETTINGS;
            c.settings = drapeSettings(args, sDrape.job->settings);
            sDrape.job->settings = c.settings;
            drapeSend(c);
        }
        else if (cmd == "lab_drape_do")
        {
            if (!sDrape.active) return result.with("ok", false).with("error", "inactive");
            const std::string what = args["what"].asString();
            DrapeCmd c;
            c.kind = what == "pause" ? DrapeCmd::PAUSE : what == "resume" ? DrapeCmd::RESUME : what == "reset" ? DrapeCmd::RESET : DrapeCmd::CLEARPINS;
            drapeSend(c);
        }
        else if (cmd == "lab_lod")
        {
            if (sDrape.active || sTouch.active) return result.with("ok", false).with("error", "lod_busy");
            result = lodShow(args["level"].asInteger());
        }
        else if (cmd == "lab_drape_apply")
        {
            result = drapeApply();
        }
        else if (cmd == "lab_drape_stop")
        {
            drapeStop();
        }
        else if (cmd == "lab_touch_stop")
        {
            touchStop();
        }
        else if (cmd == "lab_fit")
        {
            // Make it fit: the worn file, every test pose, then a _fit copy next to it
            if (sApplied.empty() || sWornFile.isNull()) return result.with("ok", false).with("error", "not_worn");
            poseRelease();   // the test poses must animate the avatar
            touchStop();
            drapeStop();
            placeEnd(true);
            const FileWatch* w = watchOf(sWornFile);
            if (!w) return result.with("ok", false).with("error", "file");
            const F32 mm = args.has("margin_mm") ? (F32)args["margin_mm"].asReal() : 3.f;
            sFit.margin = llclamp(mm, 0.5f, 15.f) / 1000.f;
            sFit.smooth = args.has("smooth") ? llclamp(args["smooth"].asInteger(), 0, 12) : 4;
            sFit.maxPush = llclamp(args.has("max_mm") ? (F32)args["max_mm"].asReal() : 20.f, 5.f, 40.f) / 1000.f;
            sFit.shapes = args["shapes"].asBoolean();
            sFit.only.clear();
            if (args["regions"].isArray())
            for (LLSD::array_const_iterator it = args["regions"].beginArray(); it != args["regions"].endArray(); ++it) sFit.only.insert(it->asString());
            endRun(true);
            // from the original (new settings must not stack on an earlier fit), unless asked to keep
            // the current _fit (a Drape or Touch up result)
            std::string src = w->path;
            const size_t dot = src.find_last_of('.');
            if (!args["from_current"].asBoolean() && dot != std::string::npos && dot >= 4 && src.compare(dot - 4, 4, "_fit") == 0)
            {
                const std::string orig = src.substr(0, dot - 4) + src.substr(dot);
                if (std::ifstream(orig).good()) src = orig;
            }
            const std::string err = fitPrepare(src);
            if (!err.empty()) return result.with("ok", false).with("error", err);
            pauseAO();
            sRun.active = true;
            sRun.fit = true;
            startStep(0);
            result["steps"] = NSTEPS;
            result["file"] = baseName(w->path);
            result["out"] = baseName(MGCreatorDae::fitPathFor(w->path));
        }
        else if (cmd == "lab_fit_stop")
        {
            endRun(true);
            sFit.ready = false;
        }
        else if (cmd == "lab_test")
        {
            if (sApplied.empty()) return result.with("ok", false).with("error", "not_worn");
            poseRelease();   // the test poses must animate the avatar
            touchStop();
            endRun(true);
            pauseAO();
            sRun.active = true;
            // <MG> Alpha: count what the garment covers in every pose of this test
            sAlpha.faces.clear();
            sAlpha.samples = 0;
            sAlpha.complete = false;
            sAlpha.collect = true;
            startStep(0);
            result["steps"] = NSTEPS;
        }
        else if (cmd == "lab_alpha")
        {
            // <MG> Alpha: state / make the masks from the last stress test / save them as an Alpha wearable
            const std::string what = args["what"].asString();
            if (what == "make") return alphaMake(llclamp(args.has("rings") ? args["rings"].asInteger() : 1, 0, 4));
            if (what == "save") return alphaSave(args["name"].asString(), !args.has("wear") || args["wear"].asBoolean());
            result["ready"] = sAlpha.complete;
            result["bom"] = !sAlpha.faces.empty();
            result["saving"] = sAlpha.pending > 0;
        }
        else if (cmd == "lab_test_stop")
        {
            endRun(true);
        }
        else if (cmd == "lab_hold")
        {
            const std::string key = args["key"].asString();
            if (!key.empty()) poseRelease();   // holding a test pose animates the avatar
            endRun(true);
            if (!key.empty())
            {
                S32 found = -1;
                for (S32 i = 0; i < NSTEPS; ++i) if (key == STEPS[i].key) found = i;
                if (found < 0) return result.with("ok", false).with("error", "key");
                pauseAO();
                sRun.hold = true;
                startStep(found);
                result["hold"] = key;
            }
        }
        else
        {
            result = LLSD().with("ok", false).with("error", "unknown");
        }
        return result;
    }
}
