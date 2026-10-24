/**
 * @file mgphoneinventory.cpp
 * @brief MasterGate Viewer: the MG Phone "Textures" app's access to the inventory.
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

#include "mgphoneinventory.h"

#include <array>
#include <cmath>
#include <deque>
#include <map>
#include <set>

#include "llagent.h"
#include "llavatarnamecache.h"
#include "llbase64.h"
#include "llclipboard.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldir.h"
#include "llfetchedgltfmaterial.h"
#include "llfile.h"
#include "llfloaterreg.h"
#include "llgiveinventory.h"
#include "llgltfmateriallist.h"
#include "llimagejpeg.h"
#include "llimagepng.h"
#include "llinventorydefines.h" // <MG/>
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llinventoryobserver.h"
#include "llmd5.h"
#include "llsdserialize.h"
#include "roles_constants.h"
#include "llselectmgr.h"
#include "lluri.h"
#include "llviewerassetupload.h"
#include "llviewerinventory.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llwearabletype.h" // <MG/>
#include "llworld.h"
#include "mgfloaterphone.h"
#include "mgphoneanims.h"
#include "mgphonecamera.h"
#include "mgphonewardrobe.h" // <MG/>

namespace
{
    // ---------------------------------------------------------------------
    // helpers
    // ---------------------------------------------------------------------
    void push(const LLSD& ev)
    {
        MGFloaterPhone* phone = LLFloaterReg::findTypedInstance<MGFloaterPhone>("mg_phone");
        if (phone) phone->pushToPage(ev);
    }

    std::string sSalt;
    // duplicates are matched by asset without telling the page the asset UUID
    std::string dupKey(const LLUUID& asset)
    {
        if (sSalt.empty())
        {
            LLUUID r; r.generate();
            sSalt = r.asString();
        }
        LLMD5 md5;
        const std::string s = sSalt + asset.asString();
        md5.update((const unsigned char*)s.data(), s.size());
        md5.finalize();
        char hex[33] = { 0 };
        md5.hex_digest(hex);
        return std::string(hex, 12);
    }

    U32 permBits(const LLPermissions& p, U32 mask)
    {
        (void)p;
        return ((mask & PERM_COPY) ? 1 : 0) | ((mask & PERM_MODIFY) ? 2 : 0) | ((mask & PERM_TRANSFER) ? 4 : 0);
    }

    U32 agentBits(const LLPermissions& p)
    {
        return (p.allowCopyBy(gAgentID) ? 1 : 0) | (p.allowModifyBy(gAgentID) ? 2 : 0)
             | (p.allowOperationBy(PERM_TRANSFER, gAgentID) ? 4 : 0);
    }

    bool isFullPerm(const LLViewerInventoryItem* item)
    {
        return item && agentBits(item->getPermissions()) == 7;
    }

    bool isMaterial(const LLInventoryItem* item)
    {
        return item && item->getType() == LLAssetType::AT_MATERIAL;
    }

    // <MG> the Wardrobe app's kinds: clothing layers, body parts, objects (attachments)
    bool isWear(const LLInventoryItem* item)
    {
        if (!item) return false;
        const LLAssetType::EType t = item->getType();
        return t == LLAssetType::AT_CLOTHING || t == LLAssetType::AT_BODYPART || t == LLAssetType::AT_OBJECT;
    }

    // a texture, snapshot or PBR material the avatar owns (not a link, not in the trash, not the Library)
    LLViewerInventoryItem* ownTexture(const LLUUID& id)
    {
        LLViewerInventoryItem* item = gInventory.getItem(id);
        if (!item || item->getIsLinkType()) return nullptr;
        const LLAssetType::EType t = item->getType();
        if (t != LLAssetType::AT_TEXTURE && t != LLAssetType::AT_MATERIAL && t != LLAssetType::AT_ANIMATION && t != LLAssetType::AT_GESTURE
            && !isWear(item)) return nullptr;
        if (!gInventory.isObjectDescendentOf(id, gInventory.getRootFolderID())) return nullptr;
        return item;
    }

    bool userFolder(const LLUUID& id)
    {
        LLViewerInventoryCategory* cat = gInventory.getCategory(id);
        if (!cat) return false;
        if (id != gInventory.getRootFolderID() && !gInventory.isObjectDescendentOf(id, gInventory.getRootFolderID())) return false;
        const LLUUID trash = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
        return id != trash && !gInventory.isObjectDescendentOf(id, trash);
    }

    std::string folderPath(const LLUUID& id)
    {
        std::string path;
        LLUUID cur = id;
        for (S32 guard = 0; guard < 40 && cur.notNull(); ++guard)
        {
            LLViewerInventoryCategory* cat = gInventory.getCategory(cur);
            if (!cat) break;
            path = cat->getName() + (path.empty() ? "" : " / " + path);
            cur = cat->getParentUUID();
        }
        return path;
    }

    // the Animations app's kinds (the Textures app has textures + materials)
    bool isMotion(const LLInventoryItem* item)
    {
        return item && (item->getType() == LLAssetType::AT_ANIMATION || item->getType() == LLAssetType::AT_GESTURE);
    }

    // kind 0: textures + materials (the Textures app), 1: animations + gestures,
    // 2: all of them (folder counts), 3: clothing, body parts, objects (the Wardrobe app)
    struct CollectTextures : public LLInventoryCollectFunctor
    {
        S32 mKind = 0;
        explicit CollectTextures(S32 kind = 0) : mKind(kind) {}
        bool operator()(LLInventoryCategory* cat, LLInventoryItem* item) override
        {
            if (cat) return true;
            if (!item || item->getIsLinkType()) return false;
            const LLAssetType::EType t = item->getType();
            const bool tex = t == LLAssetType::AT_TEXTURE || t == LLAssetType::AT_MATERIAL;
            const bool mot = t == LLAssetType::AT_ANIMATION || t == LLAssetType::AT_GESTURE;
            if (mKind == 3) return isWear(item);
            return mKind == 0 ? tex : (mKind == 1 ? mot : (tex || mot || isWear(item)));
        }
    };

    // the page asks for the index in pages; built once per offset 0
    std::vector<LLUUID> sIndexes[3];   // per kind (textures / animations / wearables)

    // tell the page the inventory moved (throttled)
    class InvWatch : public LLInventoryObserver
    {
    public:
        F64 mLast = 0.0;
        void changed(U32 mask) override
        {
            if (!(mask & (ADD | REMOVE | LABEL | STRUCTURE))) return;
            const F64 now = LLTimer::getTotalSeconds();
            if (now - mLast < 1.5) return;
            mLast = now;
            push(LLSD().with("type", "inv_changed"));
        }
    };
    InvWatch* sWatch = nullptr;
    void watch()
    {
        if (!sWatch)
        {
            sWatch = new InvWatch();
            gInventory.addObserver(sWatch);
        }
    }

    // ---------------------------------------------------------------------
    // thumbnails: the texture fetched at a small discard level, flattened,
    // scaled, JPEG; cached on disk (cache/mgthumbs) for the next session
    // ---------------------------------------------------------------------
    enum JobKind { JOB_THUMB, JOB_FULL, JOB_SAVE };
    // the material maps, in LLGLTFMaterial::TextureInfo order
    constexpr S32 MAP_COUNT = 4;
    const char* const MAP_SLOT[MAP_COUNT] = { "base", "normal", "orm", "emissive" };

    // ---------------------------------------------------------------------
    // PBR materials: the material asset and its maps are fetched here and
    // drawn on the CPU as a studio "material ball" (sphere, cube or a tiled
    // plane). The page gets pictures and factors; never a material or map UUID.
    // ---------------------------------------------------------------------
    struct MatData
    {
        LLColor4 base = LLColor4(1.f, 1.f, 1.f, 1.f);   // linear, as in glTF
        LLColor3 emis = LLColor3(0.f, 0.f, 0.f);
        F32 metal = 1.f;
        F32 rough = 1.f;
        F32 cutoff = 0.5f;
        S32 alphaMode = LLGLTFMaterial::ALPHA_MODE_OPAQUE;
        bool doubleSided = false;
        std::array<LLGLTFMaterial::TextureTransform, MAP_COUNT> tt;
        std::array<LLUUID, MAP_COUNT> mapId;
        std::array<LLPointer<LLImageRaw>, MAP_COUNT> map;
        std::array<S32, MAP_COUNT> mapW = { { 0, 0, 0, 0 } };
        std::array<S32, MAP_COUNT> mapH = { { 0, 0, 0, 0 } };
        S32 side = 0;       // the longest side the maps were fetched for (0: full size)
        F64 used = 0.0;
    };

    // An edit made in the app, not saved yet: factors, and per map "keep",
    // "none" or an inventory texture (resolved here to its asset) + transform.
    struct Draft
    {
        bool set = false;
        LLColor4 base = LLColor4(1.f, 1.f, 1.f, 1.f);
        LLColor3 emis = LLColor3(0.f, 0.f, 0.f);
        F32 metal = 1.f, rough = 1.f, cutoff = 0.5f;
        S32 alphaMode = LLGLTFMaterial::ALPHA_MODE_OPAQUE;
        bool doubleSided = false;
        enum MapMode { KEEP, NONE, ITEM };
        std::array<S32, MAP_COUNT> mode = { { KEEP, KEEP, KEEP, KEEP } };
        std::array<LLUUID, MAP_COUNT> asset;
        std::array<bool, MAP_COUNT> hasTT = { { false, false, false, false } };
        std::array<LLGLTFMaterial::TextureTransform, MAP_COUNT> tt;
    };

    struct Job
    {
        LLUUID item;
        LLUUID asset;
        JobKind kind = JOB_THUMB;
        S32 maxSide = 192;
        F64 started = 0.0;
        S32 discard = 2;
        LLPointer<LLViewerFetchedTexture> tex;

        // PBR material jobs: the material asset first, then its texture maps
        bool mat = false;
        S32 phase = 0;
        LLPointer<LLFetchedGLTFMaterial> material;
        std::array<LLPointer<LLViewerFetchedTexture>, MAP_COUNT> mapTex;
        std::array<S32, MAP_COUNT> mapDiscard = { { 0, 0, 0, 0 } };
        MatData md;          // what gets drawn: the material (+ the draft), maps filled as they arrive
        Draft draft;         // a preview of unsaved edits: never cached as the material
        S32 shape = 0;    // 0 sphere, 1 cube, 2 tiled plane
        F32 light = 0.f;    // studio rotation, degrees
        S32 bg = 0;         // 0 dark, 1 light, 2 checker
        bool viewOnly = false;
    };
    std::deque<Job*> sQueue;
    std::map<LLUUID, Job*> sActive;   // key: job id
    constexpr size_t MAX_ACTIVE = 8;
    constexpr size_t MAX_QUEUE = 120;
    constexpr F64 JOB_TIMEOUT = 30.0;

    std::string thumbPath(const LLUUID& asset, S32 size, bool material = false)
    {
        const std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "mgthumbs");
        if (!LLFile::isdir(dir)) LLFile::mkdir(dir);
        // material balls carry a render version: bump it when the renderer changes
        return dir + gDirUtilp->getDirDelimiter() + asset.asString() + "_" + std::to_string(size) + (material ? "_m1" : "") + ".jpg";
    }

    std::string readFileB64(const std::string& path)
    {
        llifstream in(path.c_str(), std::ios::in | std::ios::binary);
        if (!in.is_open()) return "";
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (data.size() < 64) return "";
        return LLBase64::encode((const U8*)data.data(), (U32)data.size());
    }

    // small RGB copy of the texture (alpha flattened on dark grey), longest side <= max_side
    LLPointer<LLImageRaw> previewOf(LLImageRaw* src, S32 max_side, bool* had_alpha)
    {
        const S32 w = src->getWidth(), h = src->getHeight(), c = src->getComponents();
        LLPointer<LLImageRaw> rgb = new LLImageRaw((U16)w, (U16)h, 3);
        const U8* s = src->getData();
        U8* d = rgb->getData();
        bool alpha = false;
        for (S32 i = 0; i < w * h; ++i)
        {
            if (c >= 3)
            {
                const U8 a = (c == 4) ? s[i * 4 + 3] : 255;
                if (a < 250) alpha = true;
                for (S32 k = 0; k < 3; ++k)
                {
                    d[i * 3 + k] = (U8)((s[i * c + k] * a + 42 * (255 - a)) / 255);
                }
            }
            else
            {
                const U8 g = s[i * c];
                d[i * 3] = d[i * 3 + 1] = d[i * 3 + 2] = g;
            }
        }
        if (had_alpha) *had_alpha = alpha;
        if (w > max_side || h > max_side)
        {
            const F32 k = (F32)max_side / (F32)llmax(w, h);
            rgb->scale(llmax(1, ll_round(w * k)), llmax(1, ll_round(h * k)));
        }
        return rgb;
    }

    void finishJob(Job* job, const LLSD& ev)
    {
        push(ev);
        for (auto it = sActive.begin(); it != sActive.end(); ++it)
        {
            if (it->second == job)
            {
                sActive.erase(it);
                break;
            }
        }
        // No callback list is registered: a job that timed out is just forgotten
        // here, and its late callback finds no job and returns (removing entries
        // from inside the texture's own callback loop is not safe).
        delete job;
    }

    LLPointer<LLImageJPEG> toJpeg(LLImageRaw* raw, S32 quality)
    {
        LLPointer<LLImageJPEG> jpeg = new LLImageJPEG(quality);
        if (!raw || !jpeg->encode(raw, 0.f)) return nullptr;
        return jpeg;
    }

    std::string jpegUri(LLImageJPEG* jpeg)
    {
        return jpeg ? "data:image/jpeg;base64," + LLBase64::encode(jpeg->getData(), jpeg->getDataSize()) : "";
    }

    // longest side wanted -> the coarsest discard level that still covers it (0: full size)
    S32 discardFor(LLViewerFetchedTexture* tex, S32 side, S32 fallback)
    {
        if (side <= 0) return 0;
        const S32 full = llmax(tex->getFullWidth(), tex->getFullHeight());
        if (full <= 0) return fallback;
        S32 d = 0;
        while (d < 5 && (full >> (d + 1)) >= side) ++d;
        return d;
    }

    std::map<LLUUID, MatData> sMatCache;   // by material asset: the last few, for instant re-renders
    constexpr size_t MAT_CACHE = 6;

    // maps fetched for a draft (textures picked in the app), by texture asset
    struct MapRaw { LLPointer<LLImageRaw> raw; S32 w = 0, h = 0, side = 0; F64 used = 0.0; };
    std::map<LLUUID, MapRaw> sMapRaw;
    constexpr size_t MAP_RAW_CACHE = 16;

    const MapRaw* cachedMap(const LLUUID& asset, S32 side)
    {
        auto it = sMapRaw.find(asset);
        if (it == sMapRaw.end() || !it->second.raw) return nullptr;
        if (it->second.side != 0 && (side == 0 || it->second.side < side)) return nullptr;
        it->second.used = LLTimer::getTotalSeconds();
        return &it->second;
    }

    void cacheMap(const LLUUID& asset, LLImageRaw* raw, S32 w, S32 h, S32 side)
    {
        if (!raw || asset.isNull()) return;
        while (sMapRaw.size() >= MAP_RAW_CACHE && !sMapRaw.count(asset))
        {
            auto oldest = sMapRaw.begin();
            for (auto it = sMapRaw.begin(); it != sMapRaw.end(); ++it) if (it->second.used < oldest->second.used) oldest = it;
            sMapRaw.erase(oldest);
        }
        MapRaw& m = sMapRaw[asset];
        m.raw = raw; m.w = w; m.h = h; m.side = side; m.used = LLTimer::getTotalSeconds();
    }

    S32 mapSideFor(const Job* job)
    {
        return job->kind == JOB_THUMB ? 128 : (job->kind == JOB_SAVE ? 0 : 512);
    }

    MatData* cachedMaterial(const LLUUID& asset, S32 side)
    {
        auto it = sMatCache.find(asset);
        if (it == sMatCache.end()) return nullptr;
        const S32 have = it->second.side;
        if (have != 0 && (side == 0 || have < side)) return nullptr;
        it->second.used = LLTimer::getTotalSeconds();
        return &it->second;
    }

    MatData& cacheMaterial(const LLUUID& asset, const MatData& md)
    {
        while (sMatCache.size() >= MAT_CACHE && !sMatCache.count(asset))
        {
            auto oldest = sMatCache.begin();
            for (auto it = sMatCache.begin(); it != sMatCache.end(); ++it)
            {
                if (it->second.used < oldest->second.used) oldest = it;
            }
            sMatCache.erase(oldest);
        }
        MatData& slot = sMatCache[asset];
        slot = md;
        slot.used = LLTimer::getTotalSeconds();
        return slot;
    }

    // --- a tiny shading kit ---
    struct V3 { F32 x, y, z; };
    inline V3 v3(F32 x, F32 y, F32 z) { return { x, y, z }; }
    inline V3 operator+(const V3& a, const V3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline V3 operator-(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    inline V3 operator*(const V3& a, F32 k) { return { a.x * k, a.y * k, a.z * k }; }
    inline V3 vmul(const V3& a, const V3& b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }
    inline F32 vdot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline V3 vcross(const V3& a, const V3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    inline V3 vnorm(const V3& a) { const F32 l = sqrtf(vdot(a, a)); return l > 1e-6f ? a * (1.f / l) : v3(0.f, 0.f, 1.f); }
    inline V3 vlerp(const V3& a, const V3& b, F32 t) { return a + (b - a) * t; }
    inline V3 rotY(const V3& d, F32 c, F32 s) { return { c * d.x + s * d.z, d.y, -s * d.x + c * d.z }; }
    inline V3 rotX(const V3& d, F32 c, F32 s) { return { d.x, c * d.y - s * d.z, s * d.y + c * d.z }; }
    inline F32 pow5(F32 x) { const F32 x2 = x * x; return x2 * x2 * x; }
    // sRGB <-> linear (a cheap fit is plenty for a preview)
    inline F32 toLin(F32 c) { return c * (c * (c * 0.305306011f + 0.682171111f) + 0.012522878f); }
    inline F32 toSrgb(F32 x)
    {
        x = llclamp(x, 0.f, 1.f);
        return x <= 0.0031308f ? 12.92f * x : 1.055f * powf(x, 1.f / 2.4f) - 0.055f;
    }
    inline F32 tonemap(F32 x)   // ACES fit
    {
        x *= 0.85f;
        return llclamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.f, 1.f);
    }

    // bilinear and wrapped; v = 0 at the top of the image (raw images are stored bottom-up)
    void sampleMap(const LLImageRaw* img, F32 u, F32 v, F32 out[4])
    {
        const S32 w = img->getWidth(), h = img->getHeight(), c = img->getComponents();
        const U8* d = img->getData();
        u -= floorf(u);
        v -= floorf(v);
        const F32 x = u * w - 0.5f, y = (1.f - v) * h - 0.5f;
        const S32 x0 = (S32)floorf(x), y0 = (S32)floorf(y);
        const F32 fx = x - x0, fy = y - y0;
        const S32 xs[2] = { ((x0 % w) + w) % w, (((x0 + 1) % w) + w) % w };
        const S32 ys[2] = { ((y0 % h) + h) % h, (((y0 + 1) % h) + h) % h };
        F32 acc[4] = { 0.f, 0.f, 0.f, 0.f };
        for (S32 j = 0; j < 2; ++j)
        {
            for (S32 i = 0; i < 2; ++i)
            {
                const F32 wt = (i ? fx : 1.f - fx) * (j ? fy : 1.f - fy);
                const U8* p = d + ((size_t)ys[j] * w + xs[i]) * c;
                if (c >= 3)
                {
                    acc[0] += p[0] * wt; acc[1] += p[1] * wt; acc[2] += p[2] * wt;
                    acc[3] += (c == 4 ? p[3] : 255) * wt;
                }
                else
                {
                    acc[0] += p[0] * wt; acc[1] += p[0] * wt; acc[2] += p[0] * wt;
                    acc[3] += (c == 2 ? p[1] : 255) * wt;
                }
            }
        }
        for (S32 k = 0; k < 4; ++k) out[k] = acc[k] / 255.f;
    }

    // KHR_texture_transform: scale, rotate, offset
    inline void uvFor(const MatData& m, S32 slot, F32 u, F32 v, F32& ou, F32& ov)
    {
        const LLGLTFMaterial::TextureTransform& t = m.tt[slot];
        const F32 su = u * t.mScale.mV[0], sv = v * t.mScale.mV[1];
        const F32 c = cosf(t.mRotation), s = sinf(t.mRotation);
        ou = c * su + s * sv + t.mOffset.mV[0];
        ov = -s * su + c * sv + t.mOffset.mV[1];
    }

    struct Studio { V3 key, fill; };

    // the studio around the ball (linear): floor, horizon, sky, and two softboxes
    // that blur out as the surface gets rougher
    V3 studioEnv(const V3& d, F32 rough, const Studio& L)
    {
        const F32 t = llclamp(0.5f + 0.5f * d.y, 0.f, 1.f);
        V3 c = t < 0.5f ? vlerp(v3(0.03f, 0.028f, 0.026f), v3(0.19f, 0.2f, 0.22f), t * 2.f)
                        : vlerp(v3(0.19f, 0.2f, 0.22f), v3(0.5f, 0.56f, 0.66f), (t - 0.5f) * 2.f);
        const F32 g = (1.f - rough) * (1.f - rough);
        const F32 sharp = 3.f + 120.f * g, gain = 0.25f + 6.f * g;
        c = c + v3(1.f, 0.96f, 0.9f) * (powf(llmax(0.f, vdot(d, L.key)), sharp) * gain);
        c = c + v3(0.7f, 0.82f, 1.f) * (powf(llmax(0.f, vdot(d, L.fill)), sharp) * gain * 0.4f);
        return c;
    }

    struct Shaded { V3 col; F32 a; };

    // metallic-roughness PBR at one surface point. ng / tg / bt: the surface frame in view
    // space (z toward the camera); u, v: the surface coordinates before the map transforms
    Shaded shadeAt(const MatData& m, const Studio& L, const V3& ng, const V3& tg, const V3& bt, F32 u, F32 v)
    {
        F32 s[4], mu, mv;
        V3 base = v3(m.base.mV[0], m.base.mV[1], m.base.mV[2]);
        F32 alpha = m.base.mV[3];
        if (m.map[0])
        {
            uvFor(m, 0, u, v, mu, mv); sampleMap(m.map[0], mu, mv, s);
            base = vmul(base, v3(toLin(s[0]), toLin(s[1]), toLin(s[2])));
            alpha *= s[3];
        }
        F32 rough = m.rough, metal = m.metal;
        if (m.map[2])
        {
            uvFor(m, 2, u, v, mu, mv); sampleMap(m.map[2], mu, mv, s);
            rough *= s[1];
            metal *= s[2];
        }
        rough = llclamp(rough, 0.045f, 1.f);
        metal = llclamp(metal, 0.f, 1.f);
        V3 n = ng;
        if (m.map[1])
        {
            uvFor(m, 1, u, v, mu, mv); sampleMap(m.map[1], mu, mv, s);
            n = vnorm(tg * (s[0] * 2.f - 1.f) + bt * (s[1] * 2.f - 1.f) + ng * llmax(0.05f, s[2] * 2.f - 1.f));
        }
        V3 emis = v3(m.emis.mV[0], m.emis.mV[1], m.emis.mV[2]);
        if (m.map[3])
        {
            uvFor(m, 3, u, v, mu, mv); sampleMap(m.map[3], mu, mv, s);
            emis = vmul(emis, v3(toLin(s[0]), toLin(s[1]), toLin(s[2])));
        }

        const V3 V = v3(0.f, 0.f, 1.f);
        const V3 one = v3(1.f, 1.f, 1.f);
        const F32 nv = llmax(vdot(n, V), 1e-3f);
        const V3 f0 = vlerp(v3(0.04f, 0.04f, 0.04f), base, metal);
        const V3 diff = base * (1.f - metal);
        const F32 a = rough * rough, a2 = a * a, k = (rough + 1.f) * (rough + 1.f) / 8.f;
        V3 col = v3(0.f, 0.f, 0.f);
        const V3 dirs[2] = { L.key, L.fill };
        const V3 cols[2] = { v3(3.1f, 2.95f, 2.75f), v3(0.5f, 0.58f, 0.72f) };
        for (S32 i = 0; i < 2; ++i)
        {
            const V3& l = dirs[i];
            const F32 nl = vdot(n, l);
            if (nl <= 0.f) continue;
            const V3 h = vnorm(l + V);
            const F32 nh = llmax(vdot(n, h), 0.f), vh = llmax(vdot(V, h), 0.f);
            const F32 dd = nh * nh * (a2 - 1.f) + 1.f;
            const F32 D = a2 / (F_PI * dd * dd);
            const F32 G = (nl / (nl * (1.f - k) + k)) * (nv / (nv * (1.f - k) + k));
            const V3 F = f0 + (one - f0) * pow5(1.f - vh);
            const V3 spec = F * (D * G / (4.f * nl * nv));
            col = col + vmul(vmul(one - F, diff) * (1.f / F_PI) + spec, cols[i]) * nl;
        }
        // image based part: the studio, diffuse (blurred) and reflected
        const V3 R = n * (2.f * vdot(n, V)) - V;
        const V3 Fa = f0 + (one - f0) * (pow5(1.f - nv) * (1.f - rough));
        col = col + vmul(diff, studioEnv(n, 1.f, L)) * 0.85f;
        col = col + vmul(Fa, studioEnv(R, rough, L));
        col = col + emis;

        Shaded out;
        out.col = col;
        if (m.alphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND) out.a = llclamp(alpha, 0.f, 1.f);
        else if (m.alphaMode == LLGLTFMaterial::ALPHA_MODE_MASK) out.a = alpha >= m.cutoff ? 1.f : 0.f;
        else out.a = 1.f;
        return out;
    }

    void backdrop(S32 bg, S32 px, S32 py, S32 size, F32 out[3])
    {
        const F32 fy = (F32)py / (F32)size;
        if (bg == 2)
        {
            const S32 cell = llmax(4, size / 12);
            const bool on = ((px / cell) + (py / cell)) & 1;
            out[0] = out[1] = out[2] = on ? 0.31f : 0.22f;
            return;
        }
        const F32 fx = (F32)px / (F32)size - 0.5f, fc = fy - 0.45f;
        const F32 glow = llmax(0.f, 1.f - (fx * fx + fc * fc) * 2.6f);
        if (bg == 1)
        {
            const F32 g = 0.80f + 0.12f * (1.f - fy) + 0.06f * glow;
            out[0] = g; out[1] = g + 0.01f; out[2] = g + 0.025f;
            return;
        }
        const F32 g = 0.06f + 0.09f * (1.f - fy) + 0.07f * glow;
        out[0] = g; out[1] = g + 0.012f; out[2] = g + 0.03f;
    }

    // the material on a studio sphere / cube / tiled plane: RGB, rows bottom-up like every LLImageRaw
    LLPointer<LLImageRaw> renderMaterial(const MatData& m, S32 size, S32 shape, F32 light_deg, S32 bg)
    {
        const F32 az = light_deg * DEG_TO_RAD, ca = cosf(az), sa = sinf(az);
        Studio L;
        L.key = vnorm(rotY(shape == 2 ? v3(-0.72f, 0.48f, 0.4f) : v3(-0.55f, 0.62f, 0.56f), ca, sa));
        L.fill = vnorm(rotY(v3(0.72f, -0.05f, 0.45f), ca, sa));
        const F32 yc = cosf(35.f * DEG_TO_RAD), ys = sinf(35.f * DEG_TO_RAD);
        const F32 pc = cosf(28.f * DEG_TO_RAD), ps = sinf(28.f * DEG_TO_RAD);
        const F32 H = 0.5f;   // cube half size
        // cube faces: normal, tangent (u), up (-v)
        static const V3 FN[6] = { {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1} };
        static const V3 FT[6] = { {0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0} };
        static const V3 FB[6] = { {0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0} };
        auto toView = [&](const V3& c) { return rotX(rotY(c, yc, ys), pc, ps); };
        auto toCube = [&](const V3& w) { return rotY(rotX(w, pc, -ps), yc, -ys); };

        LLPointer<LLImageRaw> out = new LLImageRaw((U16)size, (U16)size, 3);
        U8* dst = out->getData();
        const S32 ss = shape == 1 ? 2 : 1;   // the cube's straight edges get 2x2 samples
        const F32 inv = 1.f / (F32)ss;
        for (S32 py = 0; py < size; ++py)
        {
            for (S32 px = 0; px < size; ++px)
            {
                F32 bgc[3];
                backdrop(bg, px, py, size, bgc);
                F32 acc[3] = { 0.f, 0.f, 0.f };
                for (S32 sy = 0; sy < ss; ++sy)
                {
                    for (S32 sx = 0; sx < ss; ++sx)
                    {
                        const F32 fx = ((px + (sx + 0.5f) * inv) / size) * 2.f - 1.f;
                        const F32 fy = 1.f - ((py + (sy + 0.5f) * inv) / size) * 2.f;
                        F32 cover = 0.f;
                        Shaded p = { v3(0.f, 0.f, 0.f), 0.f };
                        if (shape == 0)
                        {
                            const F32 R = 0.86f, x = fx / R, y = fy / R, r2 = x * x + y * y;
                            if (r2 < 1.f)
                            {
                                const F32 r = sqrtf(r2);
                                cover = llclamp((1.f - r) * R * size * 0.5f, 0.f, 1.f);
                                const V3 ng = v3(x, y, sqrtf(1.f - r2));
                                const F32 lon = atan2f(x, ng.z), lat = asinf(llclamp(y, -1.f, 1.f));
                                const V3 tg = v3(cosf(lon), 0.f, -sinf(lon));
                                p = shadeAt(m, L, ng, tg, vcross(ng, tg), 0.5f + lon / F_PI, 0.5f - lat / F_PI);
                            }
                        }
                        else if (shape == 1)
                        {
                            const V3 o = toCube(v3(fx, fy, 4.f)), d = toCube(v3(0.f, 0.f, -1.f));
                            const F32 oa[3] = { o.x, o.y, o.z }, da[3] = { d.x, d.y, d.z };
                            F32 t0 = -1e9f, t1 = 1e9f;
                            S32 axis = 0;
                            bool hit = true;
                            for (S32 k = 0; k < 3 && hit; ++k)
                            {
                                if (fabsf(da[k]) < 1e-6f) { if (fabsf(oa[k]) > H) hit = false; continue; }
                                F32 ta = (-H - oa[k]) / da[k], tb = (H - oa[k]) / da[k];
                                if (ta > tb) std::swap(ta, tb);
                                if (ta > t0) { t0 = ta; axis = k; }
                                t1 = llmin(t1, tb);
                                if (t0 > t1) hit = false;
                            }
                            if (hit)
                            {
                                const S32 f = axis * 2 + (da[axis] > 0.f ? 1 : 0);
                                const V3 hp = o + d * t0;
                                const F32 u = 0.5f + vdot(hp, FT[f]) / (2.f * H), v = 0.5f - vdot(hp, FB[f]) / (2.f * H);
                                p = shadeAt(m, L, toView(FN[f]), toView(FT[f]), toView(FB[f]), u, v);
                                cover = 1.f;
                            }
                        }
                        else
                        {
                            p = shadeAt(m, L, v3(0.f, 0.f, 1.f), v3(1.f, 0.f, 0.f), v3(0.f, 1.f, 0.f), fx + 1.f, 1.f - fy);
                            cover = 1.f;
                        }
                        const F32 w = p.a * cover;
                        acc[0] += bgc[0] * (1.f - w) + toSrgb(tonemap(p.col.x)) * w;
                        acc[1] += bgc[1] * (1.f - w) + toSrgb(tonemap(p.col.y)) * w;
                        acc[2] += bgc[2] * (1.f - w) + toSrgb(tonemap(p.col.z)) * w;
                    }
                }
                U8* o = dst + ((size_t)(size - 1 - py) * size + px) * 3;
                const F32 n = 1.f / (F32)(ss * ss);
                for (S32 k = 0; k < 3; ++k) o[k] = (U8)llclamp(ll_round(acc[k] * n * 255.f), 0, 255);
            }
        }
        return out;
    }

    std::string hexColor(F32 r, F32 g, F32 b)
    {
        char buf[8];
        snprintf(buf, sizeof(buf), "#%02x%02x%02x", ll_round(toSrgb(r) * 255.f), ll_round(toSrgb(g) * 255.f), ll_round(toSrgb(b) * 255.f));
        return buf;
    }

    bool glows(const MatData& m)
    {
        return m.emis.mV[0] + m.emis.mV[1] + m.emis.mV[2] > 0.001f && (m.mapId[3].notNull() || m.emis.mV[0] + m.emis.mV[1] + m.emis.mV[2] > 0.02f);
    }

    struct CollectByAsset : public LLInventoryCollectFunctor
    {
        std::set<LLUUID> mWant;
        bool operator()(LLInventoryCategory* cat, LLInventoryItem* item) override
        {
            return !cat && item && !item->getIsLinkType() && item->getType() == LLAssetType::AT_TEXTURE && mWant.count(item->getAssetUUID());
        }
    };

    // factors, map previews and the inventory textures the maps match
    LLSD materialInfo(const MatData& m)
    {
        LLSD info;
        info["base"] = hexColor(m.base.mV[0], m.base.mV[1], m.base.mV[2]);
        info["opacity"] = m.base.mV[3];
        info["metal"] = m.metal;
        info["rough"] = m.rough;
        info["emis"] = hexColor(m.emis.mV[0], m.emis.mV[1], m.emis.mV[2]);
        info["glow"] = glows(m);
        info["alpha"] = m.alphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND ? "blend" : (m.alphaMode == LLGLTFMaterial::ALPHA_MODE_MASK ? "mask" : "opaque");
        info["cutoff"] = m.cutoff;
        info["ds"] = m.doubleSided;

        CollectByAsset collect;
        for (S32 i = 0; i < MAP_COUNT; ++i) if (m.mapId[i].notNull()) collect.mWant.insert(m.mapId[i]);
        std::map<LLUUID, LLSD> owned;
        if (!collect.mWant.empty())
        {
            LLInventoryModel::cat_array_t cats;
            LLInventoryModel::item_array_t items;
            gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, items, LLInventoryModel::EXCLUDE_TRASH, collect);
            for (const auto& item : items)
            {
                LLSD& list = owned[item->getAssetUUID()];
                if (list.size() < 4) list.append(LLSD().with("id", item->getUUID().asString()).with("n", item->getName()));
            }
        }

        LLSD maps = LLSD::emptyArray();
        for (S32 i = 0; i < MAP_COUNT; ++i)
        {
            if (m.mapId[i].isNull()) continue;
            LLSD e;
            e["slot"] = MAP_SLOT[i];
            e["w"] = m.mapW[i];
            e["h"] = m.mapH[i];
            if (m.map[i])
            {
                LLPointer<LLImageRaw> small = previewOf(m.map[i].get(), 160, nullptr);
                LLPointer<LLImageJPEG> jpeg = toJpeg(small, 85);
                if (jpeg) e["image"] = jpegUri(jpeg);
            }
            else
            {
                e["error"] = "could not load";
            }
            const LLGLTFMaterial::TextureTransform& t = m.tt[i];
            if (t.mScale.mV[0] != 1.f || t.mScale.mV[1] != 1.f || t.mOffset.mV[0] != 0.f || t.mOffset.mV[1] != 0.f || t.mRotation != 0.f)
            {
                e["tt"] = LLSD().with("sx", t.mScale.mV[0]).with("sy", t.mScale.mV[1]).with("ox", t.mOffset.mV[0])
                                .with("oy", t.mOffset.mV[1]).with("rot", t.mRotation * RAD_TO_DEG);
            }
            auto it = owned.find(m.mapId[i]);
            if (it != owned.end()) e["inv"] = it->second;
            maps.append(e);
        }
        info["maps"] = maps;
        return info;
    }

    std::string safeName(std::string name)
    {
        static const std::string bad = "\\/:*?\"<>|";
        for (char& ch : name) if (bad.find(ch) != std::string::npos || (unsigned char)ch < 32) ch = '_';
        LLStringUtil::trim(name);
        return name.empty() ? std::string("material") : name;
    }

    // TPV Policy 2.b: content leaves the viewer only when the user created every part of
    // it (full permissions are not enough). A map counts as the user's when a texture in
    // the user's inventory with that asset was created by the user (as Firestorm's export).
    bool createdByAgent(const LLUUID& texture)
    {
        LLViewerInventoryCategory::cat_array_t cats;
        LLViewerInventoryItem::item_array_t items;
        LLAssetIDMatches matches(texture);
        gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, items, LLInventoryModel::EXCLUDE_TRASH, matches);
        for (const auto& it : items)
        {
            if (it && it->getCreatorUUID() == gAgentID) return true;
        }
        return false;
    }

    // own-creation export: every map as PNG + the factors as JSON, in <camera folder>/Materials/<name>
    LLSD exportMaterial(const MatData& m, const LLViewerInventoryItem* item)
    {
        LLSD ev;
        if (!item || item->getCreatorUUID() != gAgentID)
        {
            ev["error"] = "only materials you created can be saved";
            return ev;
        }
        for (S32 i = 0; i < MAP_COUNT; ++i)
        {
            if (m.mapId[i].notNull() && !createdByAgent(m.mapId[i]))
            {
                ev["error"] = "this material uses textures you did not create";
                return ev;
            }
        }
        const std::string name = safeName(item ? item->getName() : "material");
        const std::string d = gDirUtilp->getDirDelimiter();
        const std::string top = MGPhoneCamera::saveFolder() + d + "Materials";
        LLFile::mkdir(MGPhoneCamera::saveFolder());
        LLFile::mkdir(top);
        std::string dir = top + d + name;
        for (S32 i = 2; LLFile::isdir(dir) && i < 1000; ++i) dir = top + d + name + "_" + std::to_string(i);
        LLFile::mkdir(dir);
        S32 files = 0;
        std::string json = "{\n";
        char buf[256];
        snprintf(buf, sizeof(buf), "  \"baseColorFactor\": [%.4f, %.4f, %.4f, %.4f],\n", m.base.mV[0], m.base.mV[1], m.base.mV[2], m.base.mV[3]);
        json += buf;
        snprintf(buf, sizeof(buf), "  \"metallicFactor\": %.4f,\n  \"roughnessFactor\": %.4f,\n", m.metal, m.rough);
        json += buf;
        snprintf(buf, sizeof(buf), "  \"emissiveFactor\": [%.4f, %.4f, %.4f],\n", m.emis.mV[0], m.emis.mV[1], m.emis.mV[2]);
        json += buf;
        snprintf(buf, sizeof(buf), "  \"alphaMode\": \"%s\",\n  \"alphaCutoff\": %.4f,\n  \"doubleSided\": %s,\n  \"maps\": {",
                 m.alphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND ? "BLEND" : (m.alphaMode == LLGLTFMaterial::ALPHA_MODE_MASK ? "MASK" : "OPAQUE"),
                 m.cutoff, m.doubleSided ? "true" : "false");
        json += buf;
        bool first = true;
        for (S32 i = 0; i < MAP_COUNT; ++i)
        {
            if (!m.map[i]) continue;
            const std::string file = name + "_" + MAP_SLOT[i] + ".png";
            LLPointer<LLImagePNG> png = new LLImagePNG();
            if (!png->encode(m.map[i], 0.f) || !png->save(dir + d + file)) continue;
            ++files;
            const LLGLTFMaterial::TextureTransform& t = m.tt[i];
            snprintf(buf, sizeof(buf), "%s\n    \"%s\": { \"file\": \"%s\", \"scale\": [%.4f, %.4f], \"offset\": [%.4f, %.4f], \"rotation\": %.4f }",
                     first ? "" : ",", MAP_SLOT[i], file.c_str(), t.mScale.mV[0], t.mScale.mV[1], t.mOffset.mV[0], t.mOffset.mV[1], t.mRotation);
            json += buf;
            first = false;
        }
        json += "\n  }\n}\n";
        llofstream out((dir + d + name + ".json").c_str());
        if (out.is_open()) { out << json; out.close(); }
        if (files > 0) ev["path"] = dir;
        else ev["error"] = "no map could be written";
        ev["files"] = files;
        return ev;
    }

    // the material and its maps are in: answer the job
    void finishMaterial(Job* job, MatData& m)
    {
        const char* type = job->kind == JOB_FULL ? "inv_full" : (job->kind == JOB_SAVE ? "inv_saved" : "inv_thumb");
        LLSD ev;
        ev["type"] = type;
        ev["id"] = job->item.asString();
        ev["mat"] = true;
        if (job->kind == JOB_SAVE)
        {
            LLSD r = exportMaterial(m, gInventory.getItem(job->item));
            for (const auto& kv : llsd::inMap(r)) ev[kv.first] = kv.second;
            finishJob(job, ev);
            return;
        }
        const S32 size = job->kind == JOB_THUMB ? job->maxSide : llclamp(job->maxSide, 256, 1024);
        LLPointer<LLImageRaw> img = renderMaterial(m, size, job->kind == JOB_THUMB ? 0 : job->shape, job->light, job->kind == JOB_THUMB ? 0 : job->bg);
        LLPointer<LLImageJPEG> jpeg = toJpeg(img, job->kind == JOB_FULL ? 90 : 84);
        if (!jpeg)
        {
            ev["error"] = "encode failed";
            finishJob(job, ev);
            return;
        }
        ev["image"] = jpegUri(jpeg);
        ev["w"] = m.mapW[0];
        ev["h"] = m.mapH[0];
        ev["alpha"] = m.alphaMode != LLGLTFMaterial::ALPHA_MODE_OPAQUE;
        ev["glow"] = glows(m);
        ev["metal"] = m.metal >= 0.5f;
        if (job->kind == JOB_FULL)
        {
            ev["shape"] = job->shape;
            if (!job->viewOnly) ev["info"] = materialInfo(m);
        }
        else
        {
            const std::string path = thumbPath(job->asset, job->maxSide, true);
            llofstream out(path.c_str(), std::ios::out | std::ios::binary);
            if (out.is_open())
            {
                out.write((const char*)jpeg->getData(), jpeg->getDataSize());
                out.close();
                llofstream meta((path + ".txt").c_str());
                if (meta.is_open())
                {
                    meta << m.mapW[0] << " " << m.mapH[0] << " " << (ev["alpha"].asBoolean() ? 1 : 0) << " "
                         << (ev["glow"].asBoolean() ? 1 : 0) << " " << (ev["metal"].asBoolean() ? 1 : 0);
                }
            }
        }
        finishJob(job, ev);
    }

    // sRGB "#rrggbb" (what the page shows and edits) -> linear (what glTF stores)
    F32 srgbToLinear(F32 c)
    {
        return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    }

    bool parseHex(const std::string& s, F32 out[3])
    {
        if (s.size() != 7 || s[0] != '#') return false;
        for (S32 k = 0; k < 3; ++k)
        {
            char* end = nullptr;
            const std::string part = s.substr(1 + k * 2, 2);
            const long v = strtol(part.c_str(), &end, 16);
            if (!end || *end) return false;
            out[k] = srgbToLinear((F32)v / 255.f);
        }
        return true;
    }

    // a texture the avatar may put in a material: owned, copy + transfer (as the material editor asks)
    LLViewerInventoryItem* usableMapTexture(const LLUUID& item_id)
    {
        LLViewerInventoryItem* item = gInventory.getItem(item_id);
        if (!item || item->getType() != LLAssetType::AT_TEXTURE || item->getAssetUUID().isNull()) return nullptr;
        if (!gInventory.isObjectDescendentOf(item->getUUID(), gInventory.getRootFolderID())) return nullptr;
        const LLPermissions& p = item->getPermissions();
        if (!p.allowCopyBy(gAgentID) || !p.allowOperationBy(PERM_TRANSFER, gAgentID)) return nullptr;
        return item;
    }

    // {base:"#rrggbb", opacity, metal, rough, emis:"#rrggbb", alpha:"opaque|blend|mask", cutoff, ds,
    //  maps:{base|normal|orm|emissive: {use:"keep"|"none"|<texture item id>, sx, sy, ox, oy, rot (degrees)}}}
    bool parseDraft(const LLSD& in, Draft& d, std::string& err)
    {
        d = Draft();
        if (!in.isMap()) return true;
        d.set = true;
        F32 c[3];
        if (!parseHex(in["base"].asString(), c)) { err = "bad base color"; return false; }
        d.base = LLColor4(c[0], c[1], c[2], llclamp((F32)in["opacity"].asReal(), 0.f, 1.f));
        if (!parseHex(in["emis"].asString(), c)) { err = "bad emissive color"; return false; }
        d.emis = LLColor3(c[0], c[1], c[2]);
        d.metal = llclamp((F32)in["metal"].asReal(), 0.f, 1.f);
        d.rough = llclamp((F32)in["rough"].asReal(), 0.f, 1.f);
        d.cutoff = llclamp((F32)in["cutoff"].asReal(), 0.f, 1.f);
        const std::string a = in["alpha"].asString();
        d.alphaMode = a == "blend" ? LLGLTFMaterial::ALPHA_MODE_BLEND : (a == "mask" ? LLGLTFMaterial::ALPHA_MODE_MASK : LLGLTFMaterial::ALPHA_MODE_OPAQUE);
        d.doubleSided = in["ds"].asBoolean();
        const LLSD& maps = in["maps"];
        for (S32 i = 0; i < MAP_COUNT; ++i)
        {
            if (!maps.has(MAP_SLOT[i])) continue;
            const LLSD& m = maps[MAP_SLOT[i]];
            const std::string use = m["use"].asString();
            if (use == "none")
            {
                d.mode[i] = Draft::NONE;
            }
            else if (!use.empty() && use != "keep")
            {
                LLViewerInventoryItem* tex = usableMapTexture(LLUUID(use));
                if (!tex) { err = "texture needs copy and transfer"; return false; }
                d.mode[i] = Draft::ITEM;
                d.asset[i] = tex->getAssetUUID();
            }
            if (m.has("sx"))
            {
                d.hasTT[i] = true;
                d.tt[i].mScale.set(llclamp((F32)m["sx"].asReal(), -100.f, 100.f), llclamp((F32)m["sy"].asReal(), -100.f, 100.f));
                d.tt[i].mOffset.set(llclamp((F32)m["ox"].asReal(), -100.f, 100.f), llclamp((F32)m["oy"].asReal(), -100.f, 100.f));
                d.tt[i].mRotation = (F32)m["rot"].asReal() * DEG_TO_RAD;
            }
        }
        return true;
    }

    // the draft over the drawing data: a changed map loses its pixels (fetched again)
    void applyDraft(MatData& m, const Draft& d)
    {
        if (!d.set) return;
        m.base = d.base; m.emis = d.emis; m.metal = d.metal; m.rough = d.rough;
        m.cutoff = d.cutoff; m.alphaMode = d.alphaMode; m.doubleSided = d.doubleSided;
        for (S32 i = 0; i < MAP_COUNT; ++i)
        {
            if (d.hasTT[i]) m.tt[i] = d.tt[i];
            LLUUID want = m.mapId[i];
            if (d.mode[i] == Draft::NONE) want.setNull();
            else if (d.mode[i] == Draft::ITEM) want = d.asset[i];
            if (want != m.mapId[i])
            {
                m.mapId[i] = want;
                m.map[i] = nullptr;
                m.mapW[i] = m.mapH[i] = 0;
            }
        }
    }

    // ... and over the real material, for saving
    void applyDraft(LLGLTFMaterial& mat, const Draft& d)
    {
        if (!d.set) return;
        mat.mBaseColor = d.base; mat.mEmissiveColor = d.emis; mat.mMetallicFactor = d.metal; mat.mRoughnessFactor = d.rough;
        mat.mAlphaCutoff = d.cutoff; mat.mAlphaMode = (LLGLTFMaterial::AlphaMode)d.alphaMode; mat.mDoubleSided = d.doubleSided;
        for (S32 i = 0; i < MAP_COUNT; ++i)
        {
            if (d.hasTT[i]) mat.mTextureTransform[i] = d.tt[i];
            if (d.mode[i] == Draft::NONE) mat.mTextureId[i].setNull();
            else if (d.mode[i] == Draft::ITEM) mat.mTextureId[i] = d.asset[i];
        }
    }

    void readMaterial(MatData& m, const LLFetchedGLTFMaterial* mat)
    {
        m.base = mat->mBaseColor;
        m.emis = mat->mEmissiveColor;
        m.metal = mat->mMetallicFactor;
        m.rough = mat->mRoughnessFactor;
        m.cutoff = mat->mAlphaCutoff;
        m.alphaMode = (S32)mat->mAlphaMode;
        m.doubleSided = mat->mDoubleSided;
        for (S32 i = 0; i < MAP_COUNT; ++i)
        {
            m.mapId[i] = mat->mTextureId[i];
            m.tt[i] = mat->mTextureTransform[i];
            m.map[i] = nullptr;
            m.mapW[i] = m.mapH[i] = 0;
        }
    }

    // the drawing data is known: fetch the maps it lacks; false when none are missing
    bool startMaps(Job* job)
    {
        const S32 side = mapSideFor(job);
        job->md.side = side;
        bool any = false;
        for (S32 i = 0; i < MAP_COUNT; ++i)
        {
            const LLUUID& id = job->md.mapId[i];
            if (id.isNull() || job->md.map[i]) continue;
            if (const MapRaw* c = cachedMap(id, side))
            {
                job->md.map[i] = c->raw;
                job->md.mapW[i] = c->w;
                job->md.mapH[i] = c->h;
                continue;
            }
            LLViewerFetchedTexture* tex = LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, true, LLGLTexture::BOOST_PREVIEW);
            if (!tex) continue;
            job->mapTex[i] = tex;
            job->mapDiscard[i] = discardFor(tex, side, side == 0 ? 0 : (side >= 512 ? 1 : 2));
            tex->forceToSaveRawImage(job->mapDiscard[i], 30.f);
            any = true;
        }
        job->phase = 1;
        job->started = LLTimer::getTotalSeconds();   // the maps get their own time
        return any;
    }

    void completeMaterial(Job* job)
    {
        const S32 side = mapSideFor(job);
        for (S32 i = 0; i < MAP_COUNT; ++i)
        {
            if (job->md.map[i]) cacheMap(job->md.mapId[i], job->md.map[i], job->md.mapW[i], job->md.mapH[i], side);
        }
        if (job->draft.set)
        {
            MatData m = job->md;   // a preview: drawn, not remembered as the material
            finishMaterial(job, m);
            return;
        }
        finishMaterial(job, cacheMaterial(job->asset, job->md));
    }

    void startMaterialJob(Job* job)
    {
        job->started = LLTimer::getTotalSeconds();
        if (MatData* m = cachedMaterial(job->asset, mapSideFor(job)))
        {
            if (!job->draft.set)
            {
                finishMaterial(job, *m);
                return;
            }
            job->md = *m;
            applyDraft(job->md, job->draft);
            if (!startMaps(job))
            {
                completeMaterial(job);
                return;
            }
        }
        else
        {
            job->material = gGLTFMaterialList.getMaterial(job->asset);
            if (job->material.isNull())
            {
                finishJob(job, LLSD().with("type", job->kind == JOB_FULL ? "inv_full" : (job->kind == JOB_SAVE ? "inv_saved" : "inv_thumb"))
                                     .with("id", job->item.asString()).with("mat", true).with("error", "no material"));
                return;
            }
            job->phase = 0;
        }
        LLUUID key; key.generate();
        sActive[key] = job;
    }

    // every frame for a material job: the asset, then the maps, then the render
    void pollMaterialJob(Job* job, bool late)
    {
        const char* type = job->kind == JOB_FULL ? "inv_full" : (job->kind == JOB_SAVE ? "inv_saved" : "inv_thumb");
        const S32 side = mapSideFor(job);
        if (job->phase == 0)
        {
            if (job->material->isFetching())
            {
                if (late) finishJob(job, LLSD().with("type", type).with("id", job->item.asString()).with("mat", true).with("error", "timeout"));
                return;
            }
            if (!job->material->isLoaded())
            {
                finishJob(job, LLSD().with("type", type).with("id", job->item.asString()).with("mat", true).with("error", "could not load"));
                return;
            }
            readMaterial(job->md, job->material);
            applyDraft(job->md, job->draft);
            startMaps(job);
            late = false;
        }

        bool waiting = false;
        for (S32 i = 0; i < MAP_COUNT; ++i)
        {
            LLViewerFetchedTexture* tex = job->mapTex[i];
            if (!tex) continue;
            if (tex->isMissingAsset())
            {
                job->mapTex[i] = nullptr;
                continue;
            }
            const S32 want = discardFor(tex, side, job->mapDiscard[i]);
            if (want < job->mapDiscard[i])
            {
                // the size is known now and asks for a finer level
                job->mapDiscard[i] = want;
                tex->forceToSaveRawImage(want, 30.f);
            }
            if (tex->hasSavedRawImage() && (late || tex->getSavedRawImageLevel() <= job->mapDiscard[i]))
            {
                LLImageRaw* raw = tex->getSavedRawImage();
                if (raw && raw->getData() && raw->getWidth() > 0 && raw->getHeight() > 0)
                {
                    job->md.map[i] = raw->duplicate();
                }
                job->md.mapW[i] = tex->getFullWidth();
                job->md.mapH[i] = tex->getFullHeight();
                job->mapTex[i] = nullptr;
                continue;
            }
            if (late)
            {
                job->mapTex[i] = nullptr;   // render without this map
                continue;
            }
            tex->addTextureStats((F32)(llmax(side, 256) * llmax(side, 256)));
            waiting = true;
        }
        if (waiting) return;
        completeMaterial(job);
    }

    void onTextureLoaded(bool success, LLViewerFetchedTexture* src_vi, LLImageRaw* src, LLImageRaw* aux_src,
                         S32 discard_level, bool final, void* userdata)
    {
        LLUUID* key = (LLUUID*)userdata;
        auto it = sActive.find(*key);
        if (final || !success) delete key;
        if (it == sActive.end()) return;
        Job* job = it->second;
        if (success && !final) return;   // a lower resolution on the way to the one asked for

        const char* type = job->kind == JOB_FULL ? "inv_full" : (job->kind == JOB_SAVE ? "inv_saved" : "inv_thumb");
        LLSD ev;
        ev["type"] = type;
        ev["id"] = job->item.asString();
        if (!success || !src || !src->getData())
        {
            ev["error"] = "could not load";
            finishJob(job, ev);
            return;
        }
        ev["w"] = src_vi ? src_vi->getFullWidth() : src->getWidth();
        ev["h"] = src_vi ? src_vi->getFullHeight() : src->getHeight();

        if (job->kind == JOB_SAVE)
        {
            LLViewerInventoryItem* item = gInventory.getItem(job->item);
            std::string name = item ? item->getName() : job->item.asString();
            static const std::string bad = "\\/:*?\"<>|";
            for (char& ch : name) if (bad.find(ch) != std::string::npos || (unsigned char)ch < 32) ch = '_';
            const std::string dir = MGPhoneCamera::saveFolder() + gDirUtilp->getDirDelimiter() + "Textures";
            LLFile::mkdir(MGPhoneCamera::saveFolder());
            LLFile::mkdir(dir);
            std::string path = dir + gDirUtilp->getDirDelimiter() + name + ".png";
            for (S32 i = 2; LLFile::isfile(path) && i < 1000; ++i)
                path = dir + gDirUtilp->getDirDelimiter() + name + "_" + std::to_string(i) + ".png";
            LLPointer<LLImagePNG> png = new LLImagePNG();
            if (png->encode(src, 0.f) && png->save(path)) ev["path"] = path;
            else ev["error"] = "could not write the file";
            finishJob(job, ev);
            return;
        }

        bool alpha = false;
        LLPointer<LLImageRaw> small = previewOf(src, job->maxSide, &alpha);
        LLPointer<LLImageJPEG> jpeg = new LLImageJPEG(job->kind == JOB_FULL ? 90 : 82);
        if (!jpeg->encode(small, 0.f))
        {
            ev["error"] = "encode failed";
            finishJob(job, ev);
            return;
        }
        ev["alpha"] = alpha;
        ev["image"] = "data:image/jpeg;base64," + LLBase64::encode(jpeg->getData(), jpeg->getDataSize());
        if (job->kind == JOB_THUMB)
        {
            // cache: the JPEG bytes, plus the full size in a tiny side file
            const std::string path = thumbPath(job->asset, job->maxSide);
            llofstream out(path.c_str(), std::ios::out | std::ios::binary);
            if (out.is_open())
            {
                out.write((const char*)jpeg->getData(), jpeg->getDataSize());
                out.close();
                llofstream meta((path + ".txt").c_str());
                if (meta.is_open()) meta << ev["w"].asInteger() << " " << ev["h"].asInteger() << " " << (alpha ? 1 : 0);
            }
        }
        finishJob(job, ev);
    }

    void startJob(Job* job)
    {
        if (job->mat)
        {
            startMaterialJob(job);
            return;
        }
        job->started = LLTimer::getTotalSeconds();
        LLViewerFetchedTexture* tex = LLViewerTextureManager::getFetchedTexture(job->asset, FTT_DEFAULT, true, LLGLTexture::BOOST_PREVIEW);
        if (!tex)
        {
            finishJob(job, LLSD().with("type", job->kind == JOB_FULL ? "inv_full" : "inv_thumb").with("id", job->item.asString()).with("error", "no texture"));
            return;
        }
        job->tex = tex;
        if (job->kind == JOB_THUMB)
        {
            // the smallest level that still covers the thumbnail
            const S32 full = llmax(tex->getFullWidth(), tex->getFullHeight());
            S32 d = 2;
            if (full > 0)
            {
                d = 0;
                while (d < 5 && (full >> (d + 1)) >= job->maxSide) ++d;
            }
            job->discard = d;
        }
        else
        {
            job->discard = 0;
        }
        LLUUID key; key.generate();
        sActive[key] = job;
        tex->forceToSaveRawImage(job->discard, 30.f);
        tex->setLoadedCallback(onTextureLoaded, job->discard, true, false, new LLUUID(key), nullptr);
    }

    Job* queueJob(const LLUUID& item_id, const LLUUID& asset, JobKind kind, S32 max_side, bool mat = false)
    {
        Job* job = new Job();
        job->item = item_id;
        job->asset = asset;
        job->kind = kind;
        job->maxSide = max_side;
        job->mat = mat;
        if (kind == JOB_THUMB)
        {
            sQueue.push_back(job);
            while (sQueue.size() > MAX_QUEUE)   // the page scrolled on: drop the oldest asks
            {
                Job* old = sQueue.front();
                sQueue.pop_front();
                push(LLSD().with("type", "inv_thumb").with("id", old->item.asString()).with("error", "skipped"));
                if (old == job) job = nullptr;
                delete old;
            }
        }
        else
        {
            sQueue.push_front(job);   // a user action: next in line
        }
        return job;
    }

    // may the avatar set this item's next owner permissions (the item Properties panel's rule)
    bool canSetNext(const LLViewerInventoryItem* item)
    {
        const LLPermissions& p = item->getPermissions();
        return !LLInventoryType::cannotRestrictPermissions(item->getInventoryType())
            && gAgent.allowOperation(PERM_OWNER, p, GP_OBJECT_SET_SALE)
            && gAgent.allowOperation(PERM_TRANSFER, p, GP_OBJECT_MANIPULATE);
    }

    // a material asset onto an inventory item, through the region's material capability
    void uploadMaterial(const LLUUID& item_id, const std::string& buffer, const std::string& url, bool copy)
    {
        LLResourceUploadInfo::ptr_t info = std::make_shared<LLBufferedAssetUploadInfo>(item_id, LLAssetType::AT_MATERIAL, buffer,
            [copy](LLUUID itemId, LLUUID newAssetId, LLUUID newItemId, LLSD)
            {
                push(LLSD().with("type", "inv_mat_saved").with("ok", true).with("copy", copy)
                           .with("id", (newItemId.notNull() ? newItemId : itemId).asString()));
            },
            [copy](LLUUID itemId, LLUUID, LLSD, std::string reason)
            {
                push(LLSD().with("type", "inv_mat_saved").with("ok", false).with("copy", copy)
                           .with("id", itemId.asString()).with("error", reason));
                return true;
            });
        LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
    }

    class MatCopied : public LLInventoryCallback
    {
    public:
        MatCopied(const std::string& buffer, const std::string& url) : mBuffer(buffer), mUrl(url) {}
        void fire(const LLUUID& new_item_id) override
        {
            if (new_item_id.isNull() || !gInventory.getItem(new_item_id))
            {
                push(LLSD().with("type", "inv_mat_saved").with("ok", false).with("copy", true).with("error", "could not copy"));
                return;
            }
            uploadMaterial(new_item_id, mBuffer, mUrl, true);
        }
    private:
        std::string mBuffer;
        std::string mUrl;
    };

    // resident search through the region's avatar picker capability -> "inv_people" event
    void searchPeople(const std::string& url, const std::string& q)
    {
        LLCore::HttpRequest::policy_t policy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("MGPeopleSearch", policy);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t opts = std::make_shared<LLCore::HttpOptions>();
        opts->setTimeout(15);
        LLSD result = adapter->getAndSuspend(request, url, opts);
        const LLSD http = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        const LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http);
        LLSD ev = LLSD().with("type", "inv_people").with("q", q);
        LLSD people = LLSD::emptyArray();
        if (status)
        {
            for (const auto& row : llsd::inArray(result["agents"]))
            {
                const LLUUID id = row["id"].asUUID();
                if (id.isNull() || id == gAgentID) continue;
                people.append(LLSD().with("id", id.asString()).with("name", row["display_name"].asString()).with("user", row["username"].asString()));
            }
        }
        else
        {
            ev["error"] = status.toString();
        }
        ev["people"] = people;
        push(ev);
    }
}

namespace MGPhoneInventory
{
    bool isCommand(const std::string& cmd)
    {
        return cmd.rfind("inv_", 0) == 0;
    }

    // <MG> a texture picture under any key (the Wardrobe's clothing layer textures)
    std::string cachedThumb(const LLUUID& texture, S32 size)
    {
        const std::string path = thumbPath(texture, size);
        const std::string b64 = LLFile::isfile(path) ? readFileB64(path) : "";
        return b64.empty() ? "" : "data:image/jpeg;base64," + b64;
    }

    void queueThumb(const LLUUID& event_id, const LLUUID& texture, S32 size)
    {
        if (texture.notNull()) queueJob(event_id, texture, JOB_THUMB, size);
    }

    void idle()
    {
        while (sActive.size() < MAX_ACTIVE && !sQueue.empty())
        {
            Job* job = sQueue.front();
            sQueue.pop_front();
            startJob(job);
        }
        const F64 now = LLTimer::getTotalSeconds();
        std::vector<Job*> late, mats;
        for (auto& kv : sActive)
        {
            Job* job = kv.second;
            if (job->mat)
            {
                mats.push_back(job);
                continue;
            }
            if (job->tex)
            {
                // keep it wanted although nothing in the world draws it
                job->tex->addTextureStats((F32)(job->maxSide * job->maxSide * 4));
            }
            if (now - job->started > JOB_TIMEOUT) late.push_back(job);
        }
        for (Job* job : mats)
        {
            pollMaterialJob(job, now - job->started > JOB_TIMEOUT);
        }
        for (Job* job : late)
        {
            finishJob(job, LLSD().with("type", job->kind == JOB_FULL ? "inv_full" : (job->kind == JOB_SAVE ? "inv_saved" : "inv_thumb"))
                                 .with("id", job->item.asString()).with("error", "timeout"));
        }
    }

    LLSD runCommand(const std::string& cmd, const LLSD& args)
    {
        LLSD result = LLSD().with("ok", true);
        watch();

        if (cmd == "inv_status")
        {
            LLInventoryModelBackgroundFetch& bg = LLInventoryModelBackgroundFetch::instance();
            if (!bg.isEverythingFetched()) bg.start();
            result["fetched"] = bg.isEverythingFetched();
            result["selection"] = LLSelectMgr::getInstance()->getSelection()->getObjectCount();
            return result;
        }

        if (cmd == "inv_folders")
        {
            const LLUUID root = gInventory.getRootFolderID();
            LLInventoryModel::cat_array_t cats;
            LLInventoryModel::item_array_t items;
            CollectTextures collect(2);
            gInventory.collectDescendentsIf(root, cats, items, LLInventoryModel::EXCLUDE_TRASH, collect);
            // counts per folder, textures and materials apart: direct, and with the subfolders
            std::map<LLUUID, S32> direct, total, mdirect, mtotal, adirect, atotal, gdirect, gtotal, wdirect, wtotal;
            for (const auto& item : items)
            {
                const LLAssetType::EType t = item->getType();
                (isWear(item) ? wdirect : t == LLAssetType::AT_MATERIAL ? mdirect : t == LLAssetType::AT_ANIMATION ? adirect : t == LLAssetType::AT_GESTURE ? gdirect : direct)[item->getParentUUID()]++;
            }
            auto addUp = [&](const std::map<LLUUID, S32>& from, std::map<LLUUID, S32>& to)
            {
                for (const auto& kv : from)
                {
                    LLUUID cur = kv.first;
                    for (S32 guard = 0; guard < 60 && cur.notNull(); ++guard)
                    {
                        to[cur] += kv.second;
                        LLViewerInventoryCategory* c = gInventory.getCategory(cur);
                        if (!c || cur == root) break;
                        cur = c->getParentUUID();
                    }
                }
            };
            addUp(direct, total);
            addUp(mdirect, mtotal);
            addUp(adirect, atotal);
            addUp(gdirect, gtotal);
            addUp(wdirect, wtotal);
            LLSD list = LLSD::emptyArray();
            LLViewerInventoryCategory* rc = gInventory.getCategory(root);
            if (rc)
            {
                list.append(LLSD().with("id", root.asString()).with("parent", "").with("name", rc->getName())
                    .with("n", direct[root]).with("all", total[root]).with("mn", mdirect[root]).with("mall", mtotal[root])
                    .with("an", adirect[root]).with("aall", atotal[root]).with("gn", gdirect[root]).with("gall", gtotal[root])
                    .with("wn", wdirect[root]).with("wall", wtotal[root]).with("sys", true));
            }
            for (const auto& cat : cats)
            {
                const LLFolderType::EType ft = cat->getPreferredType();
                const LLUUID& id = cat->getUUID();
                list.append(LLSD().with("id", id.asString()).with("parent", cat->getParentUUID().asString())
                    .with("name", cat->getName()).with("n", direct[id]).with("all", total[id]).with("mn", mdirect[id]).with("mall", mtotal[id])
                    .with("an", adirect[id]).with("aall", atotal[id]).with("gn", gdirect[id]).with("gall", gtotal[id])
                    .with("wn", wdirect[id]).with("wall", wtotal[id]).with("ft", (S32)ft).with("fav", cat->getIsFavorite())
                    .with("sys", ft != LLFolderType::FT_NONE && LLFolderType::lookupIsProtectedType(ft)));
            }
            result["folders"] = list;
            result["root"] = root.asString();
            result["fetched"] = LLInventoryModelBackgroundFetch::instance().isEverythingFetched();
            return result;
        }

        if (cmd == "inv_index")
        {
            const S32 offset = llmax(0, args["offset"].asInteger());
            const S32 limit = llclamp(args.has("limit") ? args["limit"].asInteger() : 1500, 1, 3000);
            const std::string kname = args["kind"].asString();
            const S32 kind = kname == "anim" ? 1 : (kname == "wear" ? 2 : 0);
            std::vector<LLUUID>& sIndex = sIndexes[kind];
            static std::set<LLUUID> sOutfitLinked;   // <MG/> items an outfit folder links to
            std::map<LLUUID, bool> favFolder;          // <MG/> folder (or one above it) starred in the inventory
            auto inFavFolder = [&](LLUUID cur)
            {
                std::vector<LLUUID> seen;
                bool fav = false;
                for (S32 guard = 0; guard < 40 && cur.notNull(); ++guard)
                {
                    auto it = favFolder.find(cur);
                    if (it != favFolder.end()) { fav = it->second; break; }
                    const LLViewerInventoryCategory* c = gInventory.getCategory(cur);
                    if (!c) break;
                    seen.push_back(cur);
                    if (c->getIsFavorite()) { fav = true; break; }
                    cur = c->getParentUUID();
                }
                for (const LLUUID& f : seen) favFolder[f] = fav;
                return fav;
            };
            if (offset == 0 || sIndex.empty())
            {
                sIndex.clear();
                if (kind == 2)
                {
                    sOutfitLinked.clear();
                    LLInventoryModel::cat_array_t ocats;
                    LLInventoryModel::item_array_t links;
                    gInventory.collectDescendents(gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS), ocats, links, LLInventoryModel::EXCLUDE_TRASH);
                    for (const auto& l : links) if (l->getIsLinkType()) sOutfitLinked.insert(l->getLinkedUUID());
                }
                LLInventoryModel::cat_array_t cats;
                LLInventoryModel::item_array_t items;
                CollectTextures collect(kind == 2 ? 3 : kind);
                gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, items, LLInventoryModel::EXCLUDE_TRASH, collect);
                for (const auto& item : items) sIndex.push_back(item->getUUID());
            }
            LLSD rows = LLSD::emptyArray();
            for (S32 i = offset; i < (S32)sIndex.size() && i < offset + limit; ++i)
            {
                LLViewerInventoryItem* item = gInventory.getItem(sIndex[i]);
                if (!item) continue;
                const LLPermissions& p = item->getPermissions();
                LLSD r;
                r["id"] = item->getUUID().asString();
                r["n"] = item->getName();
                r["d"] = item->getDescription();
                r["f"] = item->getParentUUID().asString();
                r["s"] = item->getInventoryType() == LLInventoryType::IT_SNAPSHOT ? 1 : 0;
                if (isMaterial(item)) r["m"] = 1;
                if (item->getType() == LLAssetType::AT_GESTURE) r["g"] = 1;
                r["c"] = (S32)item->getCreationDate();
                r["p"] = (S32)agentBits(p);
                r["np"] = (S32)permBits(p, p.getMaskNextOwner());
                // duplicates: the same asset; when the viewer is not told the asset (most
                // no-modify items), the same name, creator and type (<MG/> flagged kn)
                if (item->getAssetUUID().notNull()) r["k"] = dupKey(item->getAssetUUID());
                else
                {
                    std::string nm = item->getName();
                    LLStringUtil::toLower(nm);
                    LLStringUtil::trim(nm);
                    LLUUID nk;
                    nk.generate("n:" + nm + ":" + p.getCreator().asString() + ":" + std::to_string((S32)item->getType()) + ":" + std::to_string((S32)item->getWearableType()));
                    r["k"] = dupKey(nk);
                    r["kn"] = 1;
                }
                if (kind == 2)
                {
                    // <MG> wearables: o object, else wt the layer / body part type (b body part);
                    // on worn now; th an inventory thumbnail; 3d the shape is known (3D view)
                    if (item->getType() == LLAssetType::AT_OBJECT) r["o"] = 1;
                    else
                    {
                        r["wt"] = LLWearableType::getInstance()->getTypeName(item->getWearableType());
                        if (item->getType() == LLAssetType::AT_BODYPART) r["b"] = 1;
                    }
                    if (get_is_item_worn(item->getUUID())) r["on"] = 1;
                    if (item->getThumbnailUUID().notNull()) r["th"] = 1;
                    if (MGPhoneWardrobe::hasShape(item->getUUID())) r["3d"] = 1;
                    // what tells clothes from other objects: rg the body region of a known shape,
                    // ol linked from an outfit, mo several objects packed together (never worn)
                    const std::string rg = MGPhoneWardrobe::regionOf(item->getUUID());
                    if (!rg.empty()) r["rg"] = rg;
                    if (sOutfitLinked.count(item->getUUID())) r["ol"] = 1;
                    if (item->getType() == LLAssetType::AT_OBJECT && (item->getFlags() & LLInventoryItemFlags::II_FLAGS_OBJECT_HAS_MULTIPLE_ITEMS)) r["mo"] = 1;
                    // the inventory's favorites (the star of the Favorites tab): fv 1 the item, 2 a folder above it
                    if (item->getIsFavorite()) r["fv"] = 1;
                    else if (inFavFolder(item->getParentUUID())) r["fv"] = 2;
                }
                rows.append(r);
            }
            result["rows"] = rows;
            result["total"] = (S32)sIndex.size();
            result["next"] = offset + limit < (S32)sIndex.size() ? offset + limit : -1;
            result["fetched"] = LLInventoryModelBackgroundFetch::instance().isEverythingFetched();
            return result;
        }

        if (cmd == "inv_item")
        {
            LLViewerInventoryItem* item = ownTexture(LLUUID(args["id"].asString()));
            if (!item) return LLSD().with("ok", false).with("error", "not found");
            const LLPermissions& p = item->getPermissions();
            result["id"] = item->getUUID().asString();
            result["name"] = item->getName();
            result["desc"] = item->getDescription();
            result["folder"] = item->getParentUUID().asString();
            result["path"] = folderPath(item->getParentUUID());
            result["created"] = (S32)item->getCreationDate();
            result["snapshot"] = item->getInventoryType() == LLInventoryType::IT_SNAPSHOT;
            result["material"] = isMaterial(item);
            result["animation"] = item->getType() == LLAssetType::AT_ANIMATION;
            result["gesture"] = item->getType() == LLAssetType::AT_GESTURE;
            if (isWear(item))
            {
                // <MG> the Wardrobe app
                result["object"] = item->getType() == LLAssetType::AT_OBJECT;
                result["bodypart"] = item->getType() == LLAssetType::AT_BODYPART;
                if (item->getType() != LLAssetType::AT_OBJECT) result["wt"] = LLWearableType::getInstance()->getTypeName(item->getWearableType());
                result["worn"] = get_is_item_worn(item->getUUID());
                result["thumb"] = item->getThumbnailUUID().notNull();
                result["3d"] = MGPhoneWardrobe::hasShape(item->getUUID());
            }
            result["you"] = (S32)agentBits(p);
            result["next"] = (S32)permBits(p, p.getMaskNextOwner());
            result["everyone"] = (S32)permBits(p, p.getMaskEveryone());
            result["group"] = (S32)permBits(p, p.getMaskGroup());
            result["full"] = isFullPerm(item);
            result["base"] = (S32)permBits(p, p.getMaskBase());
            result["canNext"] = canSetNext(item);   // next owner permissions editable
            LLAvatarName av;
            const LLUUID creator = p.getCreator();
            if (creator.notNull())
            {
                if (LLAvatarNameCache::get(creator, &av)) result["creator"] = av.getCompleteName();
                else LLAvatarNameCache::get(creator, [](const LLUUID&, const LLAvatarName&) {});   // ask; the page asks again
            }
            result["selection"] = LLSelectMgr::getInstance()->getSelection()->getObjectCount();
            return result;
        }

        if (cmd == "inv_thumbs")
        {
            const S32 size = llclamp(args.has("size") ? args["size"].asInteger() : 192, 64, 512);
            LLSD cached = LLSD::emptyArray();
            LLSD none = LLSD::emptyArray();
            S32 queued = 0;
            for (const auto& v : llsd::inArray(args["ids"]))
            {
                LLViewerInventoryItem* item = ownTexture(LLUUID(v.asString()));
                if (!item || (item->getAssetUUID().isNull() && !isWear(item))) continue;   // <MG/> wearables: thumbnail / card
                if (isWear(item))
                {
                    // <MG> the inventory thumbnail, else the Wardrobe's 3D card, else none
                    const LLUUID thumb = item->getThumbnailUUID();
                    if (thumb.notNull())
                    {
                        const std::string path = thumbPath(thumb, size);
                        const std::string b64 = LLFile::isfile(path) ? readFileB64(path) : "";
                        if (!b64.empty()) cached.append(LLSD().with("id", item->getUUID().asString()).with("image", "data:image/jpeg;base64," + b64));
                        else { queueJob(item->getUUID(), thumb, JOB_THUMB, size); ++queued; }
                        continue;
                    }
                    const std::string card = MGPhoneWardrobe::cardPath(item->getUUID());
                    const std::string b64 = card.empty() ? "" : readFileB64(card);
                    if (!b64.empty()) cached.append(LLSD().with("id", item->getUUID().asString()).with("image", "data:image/jpeg;base64," + b64).with("card", true));
                    else none.append(item->getUUID().asString());
                    continue;
                }
                if (isMotion(item))
                {
                    // animations: only the preview photos taken with anim_shot (gestures: none)
                    const std::string shot = item->getType() == LLAssetType::AT_ANIMATION ? MGPhoneAnims::shotPath(item->getAssetUUID().asString(), 192) : "";
                    const std::string img = !shot.empty() && LLFile::isfile(shot) ? readFileB64(shot) : "";
                    if (!img.empty()) cached.append(LLSD().with("id", item->getUUID().asString()).with("image", "data:image/jpeg;base64," + img).with("anim", true));
                    else none.append(item->getUUID().asString());
                    continue;
                }
                const bool mat = isMaterial(item);
                const std::string path = thumbPath(item->getAssetUUID(), size, mat);
                const std::string b64 = LLFile::isfile(path) ? readFileB64(path) : "";
                if (!b64.empty())
                {
                    S32 w = 0, h = 0, a = 0, g = 0, mt = 0;
                    llifstream meta((path + ".txt").c_str());
                    if (meta.is_open()) meta >> w >> h >> a >> g >> mt;
                    LLSD c = LLSD().with("id", item->getUUID().asString()).with("image", "data:image/jpeg;base64," + b64)
                        .with("w", w).with("h", h).with("alpha", a != 0);
                    if (mat) c.with("mat", true).with("glow", g != 0).with("metal", mt != 0);
                    cached.append(c);
                }
                else
                {
                    queueJob(item->getUUID(), item->getAssetUUID(), JOB_THUMB, size, mat);
                    ++queued;
                }
            }
            result["cached"] = cached;
            result["none"] = none;
            result["queued"] = queued;
            return result;
        }

        if (cmd == "inv_cancel")
        {
            for (Job* job : sQueue) delete job;
            sQueue.clear();
            return result;
        }

        LLViewerInventoryItem* item = args.has("id") ? ownTexture(LLUUID(args["id"].asString())) : nullptr;

        if (cmd == "inv_full")
        {
            if (!item || item->getAssetUUID().isNull() || isMotion(item) || isWear(item)) return LLSD().with("ok", false).with("error", "not found");
            if (isMaterial(item))
            {
                // the material ball: {shape 0 sphere / 1 cube / 2 plane, light degrees, bg 0 dark / 1 light / 2 checker,
                // view: true = only the picture (a re-render), not the factors and maps again,
                // draft: unsaved edits to preview (see parseDraft)}
                Draft draft;
                std::string err;
                if (args.has("draft") && !parseDraft(args["draft"], draft, err)) return LLSD().with("ok", false).with("error", err);
                Job* job = queueJob(item->getUUID(), item->getAssetUUID(), JOB_FULL, llclamp(args.has("max") ? args["max"].asInteger() : 512, 256, 1024), true);
                job->draft = draft;
                job->shape = llclamp(args["shape"].asInteger(), 0, 2);
                job->light = (F32)args["light"].asReal();
                job->bg = llclamp(args["bg"].asInteger(), 0, 2);
                job->viewOnly = args["view"].asBoolean();
                if (cachedMaterial(job->asset, mapSideFor(job)))
                {
                    // everything is here already: render now rather than behind the thumbnails
                    sQueue.pop_front();
                    startJob(job);
                }
                return result;
            }
            queueJob(item->getUUID(), item->getAssetUUID(), JOB_FULL, llclamp(args.has("max") ? args["max"].asInteger() : 1024, 256, 2048));
            return result;
        }
        if (cmd == "inv_open")
        {
            if (!item || isWear(item)) return LLSD().with("ok", false).with("error", "not found");
            if (isMaterial(item)) LLFloaterReg::showInstance("material_editor", LLSD(item->getUUID()), TAKE_FOCUS_YES);
            else if (item->getType() == LLAssetType::AT_ANIMATION) LLFloaterReg::showInstance("preview_anim", LLSD(item->getUUID()), TAKE_FOCUS_YES);
            else if (item->getType() == LLAssetType::AT_GESTURE) LLFloaterReg::showInstance("preview_gesture", LLSD(item->getUUID()), TAKE_FOCUS_YES);
            else LLFloaterReg::showInstance("preview_texture", LLSD(item->getUUID()), TAKE_FOCUS_YES);
            return result;
        }
        if (cmd == "inv_apply")
        {
            if (!item || isMotion(item) || isWear(item)) return LLSD().with("ok", false).with("error", "not found");
            if (LLSelectMgr::getInstance()->getSelection()->getObjectCount() <= 0)
                return LLSD().with("ok", false).with("error", "nothing selected");
            if (isMaterial(item))
            {
                // given the item id, the selection manager checks copy / attachment / object permissions
                if (!LLSelectMgr::getInstance()->selectionSetGLTFMaterial(item->getUUID()))
                    return LLSD().with("ok", false).with("error", "not allowed");
                return result;
            }
            if (!LLSelectMgr::getInstance()->selectionSetImage(item->getUUID()))
                return LLSD().with("ok", false).with("error", "not allowed");
            return result;
        }
        if (cmd == "inv_mat_clear")
        {
            // take the PBR material off the selected faces (back to their Blinn-Phong texture)
            if (LLSelectMgr::getInstance()->getSelection()->getObjectCount() <= 0)
                return LLSD().with("ok", false).with("error", "nothing selected");
            if (!LLSelectMgr::getInstance()->selectionSetGLTFMaterial(LLUUID::null))
                return LLSD().with("ok", false).with("error", "not allowed");
            return result;
        }
        if (cmd == "inv_copy_uuid")
        {
            if (!item || !isFullPerm(item) || isWear(item)) return LLSD().with("ok", false).with("error", "full permissions only");
            const LLWString w = utf8str_to_wstring(item->getAssetUUID().asString());
            LLClipboard::instance().copyToClipboard(w, 0, (S32)w.size());
            return result;
        }
        if (cmd == "inv_save")
        {
            if (!item || !isFullPerm(item) || isMotion(item) || isWear(item)) return LLSD().with("ok", false).with("error", "full permissions only");
            // materials: the user's own creations only (TPV Policy 2.b; the maps are checked in exportMaterial)
            if (isMaterial(item) && item->getCreatorUUID() != gAgentID) return LLSD().with("ok", false).with("error", "only materials you created can be saved");
            queueJob(item->getUUID(), item->getAssetUUID(), JOB_SAVE, 0, isMaterial(item));   // a material: its maps + factors
            return result;
        }
        if (cmd == "inv_give")
        {
            // {ids | id, to}: any resident, friend or not. Transferable items only; a no-copy item
            // leaves the inventory and the viewer asks for a confirmation itself ("confirm").
            const LLUUID to(args["to"].asString());
            if (to.isNull() || to == gAgentID) return LLSD().with("ok", false).with("error", "bad resident");
            std::vector<LLUUID> ids;
            if (args.has("ids")) for (const auto& v : llsd::inArray(args["ids"])) ids.emplace_back(v.asString());
            else if (args.has("id")) ids.emplace_back(args["id"].asString());
            if (ids.empty() || ids.size() > 25) return LLSD().with("ok", false).with("error", "1 to 25 items");
            S32 given = 0, confirm = 0, skipped = 0;
            for (const LLUUID& id : ids)
            {
                LLViewerInventoryItem* it = ownTexture(id);
                if (!it || !it->getPermissions().allowOperationBy(PERM_TRANSFER, gAgentID) || !LLGiveInventory::isInventoryGiveAcceptable(it))
                {
                    ++skipped;
                    continue;
                }
                if (!it->getPermissions().allowCopyBy(gAgentID))
                {
                    LLGiveInventory::doGiveInventoryItem(to, it);
                    ++confirm;
                    continue;
                }
                if (LLGiveInventory::doGiveInventoryItem(to, it)) ++given;
                else ++skipped;
            }
            if (given + confirm == 0) return LLSD().with("ok", false).with("error", "not transferable").with("skipped", skipped);
            result["given"] = given;
            result["confirm"] = confirm;
            result["skipped"] = skipped;
            return result;
        }
        if (cmd == "inv_give_folder")
        {
            // a whole folder ("album"); the viewer checks and confirms what it contains
            const LLUUID to(args["to"].asString());
            const LLUUID id(args["id"].asString());
            LLViewerInventoryCategory* cat = gInventory.getCategory(id);
            if (to.isNull() || to == gAgentID) return LLSD().with("ok", false).with("error", "bad resident");
            if (!cat || !userFolder(id) || id == gInventory.getRootFolderID() || LLFolderType::lookupIsProtectedType(cat->getPreferredType()))
                return LLSD().with("ok", false).with("error", "not allowed");
            if (!LLGiveInventory::doGiveInventoryCategory(to, cat))
                return LLSD().with("ok", false).with("error", "could not give");
            return result;
        }
        if (cmd == "inv_people")
        {
            // {q}: resident search ("inv_people" event) -- {nearby: true}: avatars around, now
            if (args["nearby"].asBoolean())
            {
                uuid_vec_t ids;
                std::vector<LLVector3d> pos;
                const LLVector3d me = gAgent.getPositionGlobal();
                LLWorld::getInstance()->getAvatars(&ids, &pos, me, 96.f);
                LLSD people = LLSD::emptyArray();
                bool pending = false;
                for (size_t i = 0; i < ids.size() && i < pos.size(); ++i)
                {
                    if (ids[i] == gAgentID) continue;
                    LLSD p = LLSD().with("id", ids[i].asString()).with("m", (S32)dist_vec(pos[i], me));
                    LLAvatarName av;
                    if (LLAvatarNameCache::get(ids[i], &av))
                    {
                        p["name"] = av.getDisplayName();
                        p["user"] = av.getUserName();
                    }
                    else
                    {
                        LLAvatarNameCache::get(ids[i], [](const LLUUID&, const LLAvatarName&) {});
                        pending = true;
                    }
                    people.append(p);
                }
                result["people"] = people;
                result["pending"] = pending;
                return result;
            }
            std::string q = args["q"].asString();
            LLStringUtil::trim(q);
            if (q.size() < 3) return LLSD().with("ok", false).with("error", "3 letters at least");
            const LLViewerRegion* region = gAgent.getRegion();
            std::string url = region ? region->getCapability("AvatarPickerSearch") : "";
            if (url.empty()) return LLSD().with("ok", false).with("error", "search is not available here");
            if (url.back() != '/') url += "/";
            std::string text = q;
            std::replace(text.begin(), text.end(), '.', ' ');
            url += "?page_size=50&names=" + LLURI::escape(text);
            LLCoros::instance().launch("MGPhoneInventory::people", [url, q]() { searchPeople(url, q); });
            result["pending"] = true;
            return result;
        }
        if (cmd == "inv_perms")
        {
            // {ids, next: bits C1 M2 T4}: next owner permissions, as the item Properties panel allows them
            const S32 want = args["next"].asInteger();
            S32 changed = 0, skipped = 0;
            LLSD reasons = LLSD::emptyArray();
            LLSD now = LLSD::emptyMap();   // item id -> next owner bits after this call
            for (const auto& v : llsd::inArray(args["ids"]))
            {
                LLViewerInventoryItem* it = ownTexture(LLUUID(v.asString()));
                std::string why;
                if (!it) why = "not found";
                else if (!canSetNext(it)) why = "you need to own it with transfer rights";
                else if (!it->isFinished())
                {
                    it->fetchFromServer();
                    why = "still loading from the server, try again in a moment";
                }
                if (!why.empty())
                {
                    ++skipped;
                    if (reasons.size() < 3) reasons.append(why);
                    continue;
                }
                LLPermissions perm(it->getPermissions());
                const U32 base = perm.getMaskBase();
                const bool c = (want & 1) && (base & PERM_COPY);
                const bool m = (want & 2) && (base & PERM_MODIFY);
                const bool t = (want & 4) || !c;   // a next owner keeps copy or transfer
                perm.setNextOwnerBits(gAgentID, LLUUID::null, m, PERM_MODIFY);
                perm.setNextOwnerBits(gAgentID, LLUUID::null, c, PERM_COPY);
                perm.setNextOwnerBits(gAgentID, LLUUID::null, t, PERM_TRANSFER);
                now[it->getUUID().asString()] = (S32)permBits(perm, perm.getMaskNextOwner());
                if (!(perm != it->getPermissions())) continue;
                LLPointer<LLViewerInventoryItem> n = new LLViewerInventoryItem(it);
                n->setPermissions(perm);
                update_inventory_item(n.get(), nullptr);
                gInventory.updateItem(n);
                ++changed;
            }
            gInventory.notifyObservers();
            result["changed"] = changed;
            result["skipped"] = skipped;
            result["reasons"] = reasons;
            result["items"] = now;
            return result;
        }
        if (cmd == "inv_mat_save")
        {
            // {id, draft, copy, name}: save the edits over the material (modify), or as a copy (copy + modify)
            if (!item || !isMaterial(item)) return LLSD().with("ok", false).with("error", "not found");
            const LLPermissions& p = item->getPermissions();
            const bool copy = args["copy"].asBoolean();
            if (!p.allowModifyBy(gAgentID)) return LLSD().with("ok", false).with("error", "no modify");
            if (copy && !p.allowCopyBy(gAgentID)) return LLSD().with("ok", false).with("error", "no copy");
            Draft d;
            std::string err;
            if (!parseDraft(args["draft"], d, err)) return LLSD().with("ok", false).with("error", err);
            LLFetchedGLTFMaterial* fetched = gGLTFMaterialList.getMaterial(item->getAssetUUID());
            if (!fetched || !fetched->isLoaded()) return LLSD().with("ok", false).with("error", "the material is still loading");
            const LLViewerRegion* region = gAgent.getRegion();
            const std::string url = region ? region->getCapability("UpdateMaterialAgentInventory") : "";
            if (url.empty()) return LLSD().with("ok", false).with("error", "this region cannot save materials");

            LLGLTFMaterial mat(*fetched);
            applyDraft(mat, d);
            LLSD asset;
            asset["version"] = LLGLTFMaterial::ASSET_VERSION;
            asset["type"] = LLGLTFMaterial::ASSET_TYPE;
            asset["data"] = mat.asJSON();
            std::ostringstream str;
            LLSDSerialize::serialize(asset, str, LLSDSerialize::LLSD_BINARY);
            if (!copy)
            {
                uploadMaterial(item->getUUID(), str.str(), url, false);
            }
            else
            {
                std::string name = args["name"].asString();
                LLStringUtil::trim(name);
                LLInventoryObject::correctInventoryName(name);
                if (name.empty()) name = item->getName() + " copy";
                // copied first (the copy keeps the permissions), then the new asset goes on the copy
                copy_inventory_item(gAgentID, p.getOwner(), item->getUUID(), item->getParentUUID(), name,
                                    new MatCopied(str.str(), url));
            }
            result["pending"] = true;
            return result;
        }
        if (cmd == "inv_move")
        {
            const LLUUID folder(args["folder"].asString());
            if (!userFolder(folder)) return LLSD().with("ok", false).with("error", "bad folder");
            S32 moved = 0;
            for (const auto& v : llsd::inArray(args["ids"]))
            {
                LLViewerInventoryItem* it = ownTexture(LLUUID(v.asString()));
                if (it && it->getParentUUID() != folder)
                {
                    gInventory.changeItemParent(it, folder, false);
                    ++moved;
                }
            }
            for (const auto& v : llsd::inArray(args["cats"]))
            {
                LLViewerInventoryCategory* cat = gInventory.getCategory(LLUUID(v.asString()));
                if (cat && userFolder(cat->getUUID()) && cat->getUUID() != folder && !gInventory.isObjectDescendentOf(folder, cat->getUUID())
                    && !LLFolderType::lookupIsProtectedType(cat->getPreferredType()))
                {
                    gInventory.changeCategoryParent(cat, folder, false);
                    ++moved;
                }
            }
            gInventory.notifyObservers();
            result["moved"] = moved;
            return result;
        }
        if (cmd == "inv_favorite")
        {
            // <MG> {ids | id, on}: the inventory's favorite star on items or folders (synced to the account)
            std::vector<LLUUID> ids;
            if (args.has("ids")) for (const auto& v : llsd::inArray(args["ids"])) ids.emplace_back(v.asString());
            else if (args.has("id")) ids.emplace_back(args["id"].asString());
            const bool on = args["on"].asBoolean();
            S32 n = 0;
            for (const LLUUID& id : ids)
            {
                if (ownTexture(id) || (gInventory.getCategory(id) && userFolder(id)))
                {
                    set_favorite(id, on);
                    ++n;
                }
            }
            result["changed"] = n;
            return result;
        }
        if (cmd == "inv_rename")
        {
            std::string name = args["name"].asString();
            LLStringUtil::trim(name);
            if (name.empty() || name.size() > 63) return LLSD().with("ok", false).with("error", "bad name");
            const LLUUID id(args["id"].asString());
            if (item)
            {
                if (!item->getPermissions().allowModifyBy(gAgentID)) return LLSD().with("ok", false).with("error", "no modify");
                update_inventory_item(item->getUUID(), LLSD().with("name", name), nullptr);
                return result;
            }
            LLViewerInventoryCategory* cat = gInventory.getCategory(id);
            if (cat && userFolder(id) && !LLFolderType::lookupIsProtectedType(cat->getPreferredType()))
            {
                rename_category(&gInventory, id, name, nullptr);
                return result;
            }
            return LLSD().with("ok", false).with("error", "not found");
        }
        if (cmd == "inv_folder_new")
        {
            const LLUUID parent(args["parent"].asString());
            std::string name = args["name"].asString();
            LLStringUtil::trim(name);
            if (!userFolder(parent) || name.empty()) return LLSD().with("ok", false).with("error", "bad folder");
            gInventory.createNewCategory(parent, LLFolderType::FT_NONE, name, [parent, name](const LLUUID& id)
                {
                    push(LLSD().with("type", "inv_folder_new").with("id", id.asString()).with("parent", parent.asString()).with("name", name));
                });
            result["pending"] = true;
            return result;
        }
        if (cmd == "inv_trash")
        {
            S32 n = 0;
            for (const auto& v : llsd::inArray(args["ids"]))
            {
                LLViewerInventoryItem* it = ownTexture(LLUUID(v.asString()));
                if (it) { gInventory.removeItem(it->getUUID()); ++n; }
            }
            gInventory.notifyObservers();
            result["trashed"] = n;
            return result;
        }
        if (cmd == "inv_folder_trash")
        {
            const LLUUID id(args["id"].asString());
            LLViewerInventoryCategory* cat = gInventory.getCategory(id);
            if (!cat || !userFolder(id) || id == gInventory.getRootFolderID() || LLFolderType::lookupIsProtectedType(cat->getPreferredType()))
                return LLSD().with("ok", false).with("error", "not allowed");
            gInventory.removeCategory(id);
            gInventory.notifyObservers();
            return result;
        }
        return LLSD().with("ok", false).with("error", "unknown command");
    }
}
