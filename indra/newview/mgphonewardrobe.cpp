/**
 * @file mgphonewardrobe.cpp
 * @brief MasterGate Viewer: the MG Phone "Wardrobe" app: outfits, what is worn,
 *        the 3D view of clothes and wearable objects.
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

#include "mgphonewardrobe.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <vector>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include "glm/mat3x3.hpp"
#include "glm/mat4x4.hpp"
#include "glm/matrix.hpp"
#include "glm/gtc/type_ptr.hpp"

#include "llagent.h"
#include "llanimationstates.h"
#include "llappearancemgr.h"
#include "llavatarappearance.h"
#include "llavatarappearancedefines.h"
#include "llbase64.h"
#include "llcallbacklist.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "lldrawpoolavatar.h"
#include "lldynamictexture.h"
#include "llface.h"
#include "llfile.h"
#include "llfloaterreg.h"
#include "llfloatersimplesnapshot.h"
#include "llgltfmaterial.h"
#include "llimagejpeg.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "lllocaltextureobject.h"
#include "llmaterial.h"
#include "llmeshrepository.h"
#include "llprimitive.h"
#include "llvolumemgr.h"
#include "llrender.h"
#include "llsdserialize.h"
#include "llskinningutil.h"
#include "lltextureentry.h"
#include "llviewercamera.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewerobjectlist.h"
#include "llviewershadermgr.h"
#include "llviewertexturelist.h"
#include "llviewerwearable.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llvovolume.h"
#include "llwearablelist.h"
#include "llwearabletype.h"
#include "llwindow.h"
#include "pipeline.h"
#include "mgfloaterphone.h"
#include "mgphonecamera.h"
#include "mgphoneinventory.h"

using namespace LLAvatarAppearanceDefines;

namespace
{
    void push(const LLSD& ev)
    {
        MGFloaterPhone* phone = LLFloaterReg::findTypedInstance<MGFloaterPhone>("mg_phone");
        if (phone) phone->pushToPage(ev);
    }

    LLSD fail(const std::string& error) { return LLSD().with("ok", false).with("error", error); }

    // an item of the avatar's own inventory (not the Library, not the trash)
    LLViewerInventoryItem* ownItem(const LLUUID& id)
    {
        LLViewerInventoryItem* item = gInventory.getItem(id);
        if (!item || item->getIsLinkType()) return nullptr;
        if (!gInventory.isObjectDescendentOf(id, gInventory.getRootFolderID())) return nullptr;
        if (gInventory.isObjectDescendentOf(id, gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH))) return nullptr;
        return item;
    }

    bool isWear(const LLInventoryItem* item)
    {
        if (!item) return false;
        const LLAssetType::EType t = item->getType();
        return t == LLAssetType::AT_CLOTHING || t == LLAssetType::AT_BODYPART || t == LLAssetType::AT_OBJECT;
    }

    // the page's row for an item: o object, wt layer / body part type (b body part), on worn
    LLSD rowOf(const LLViewerInventoryItem* item)
    {
        LLSD r;
        r["id"] = item->getUUID().asString();
        r["n"] = item->getName();
        if (item->getType() == LLAssetType::AT_OBJECT) r["o"] = 1;
        else
        {
            r["wt"] = LLWearableType::getInstance()->getTypeName(item->getWearableType());
            if (item->getType() == LLAssetType::AT_BODYPART) r["b"] = 1;
        }
        if (get_is_item_worn(item->getUUID())) r["on"] = 1;
        if (item->getThumbnailUUID().notNull()) r["th"] = 1;
        if (MGPhoneWardrobe::hasShape(item->getUUID())) r["3d"] = 1;
        return r;
    }

    // ---------------------------------------------------------------------
    // shapes: what a worn object looks like, kept in cache/mgwardrobe
    // ---------------------------------------------------------------------
    // v2: rigged faces keep their joint weights and skin, other prims sit in their
    // attach point's frame, so the 3D view poses them on a mannequin (like the mesh
    // upload preview). v1 files (rest pose only) are dropped.
    constexpr S32 SHAPE_VERSION = 2;
    constexpr S32 CARD_SIZE = 256;

    struct Part
    {
        std::vector<LLVector3> pos, nrm;
        std::vector<LLVector2> uv;
        std::vector<U16> idx;
        LLUUID tex;                         // diffuse / base color (a baked texture id with Bakes on Mesh)
        LLColor4 color = LLColor4::white;   // sRGB tint and opacity
        S32 alpha = -1;                     // -1 by the texture, 0 opaque, 1 blend, 2 mask
        bool bright = false;                // fullbright / emissive
        S32 prim = 0;                       // which prim of the linkset
        LLVector3 mid;                      // centre, for sorting the see-through parts
        // rigged: positions in bind space (bind shape applied), 4 x "<joint>.<weight>" per vertex,
        // skin = index into Shape::skins. Not rigged (skin -1): positions in the attach point's
        // frame. Rigged without weights (-2): bind space, on the avatar's root.
        std::vector<F32> w;
        S32 skin = -1;
    };

    struct Shape
    {
        std::vector<Part> parts;
        LLVector3 lo, hi;
        S32 prims = 0, faces = 0, tris = 0, verts = 0;
        bool rigged = false, bom = false, pbr = false, alpha = false;
        std::string point;
        std::string region;   // where it sits on the body (see summarizeRegions)
        S32 when = 0;
        std::vector<LLSD> skins;   // LLMeshSkinInfo::asLLSD of each rigged prim
        S32 ap = 0;                // attach point id (0: unknown, the avatar's root)

        S32 textureCount() const
        {
            std::set<LLUUID> ids;
            for (const Part& p : parts) if (p.tex.notNull()) ids.insert(p.tex);
            return (S32)ids.size();
        }
        LLSD facts() const
        {
            LLSD f;
            f["prims"] = prims; f["faces"] = faces; f["tris"] = tris; f["verts"] = verts;
            f["textures"] = textureCount();
            f["rigged"] = rigged; f["bom"] = bom; f["pbr"] = pbr; f["alpha"] = alpha;
            f["point"] = point; f["region"] = region; f["when"] = when;
            const LLVector3 d = hi - lo;
            f["size"] = LLSD().with("x", d.mV[VX]).with("y", d.mV[VY]).with("z", d.mV[VZ]);
            return f;
        }
    };
    typedef std::shared_ptr<Shape> ShapePtr;

    std::string cacheDir()
    {
        const std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "mgwardrobe");
        if (!LLFile::isdir(dir)) LLFile::mkdir(dir);
        return dir;
    }
    std::string shapeFile(const LLUUID& item) { return cacheDir() + gDirUtilp->getDirDelimiter() + item.asString() + ".shape3"; }
    // unsealed shape files from older releases: converted when first read
    std::string plainShapeFile(const LLUUID& item) { return cacheDir() + gDirUtilp->getDirDelimiter() + item.asString() + ".shape2"; }

    // ---- sealed shape files --------------------------------------------------------
    // The geometry read from worn items is the creators' content: it stays inside this
    // viewer (TPV Policy 2.b, no export). On disk it is sealed with AES-256-GCM under a
    // random key made for this installation and kept in the user settings folder (not
    // the cache), so a copied cache folder is unreadable and other tools cannot open the
    // files as meshes. File: "MGW3" + 12-byte nonce + ciphertext + 16-byte tag.
    const std::string SEAL_MAGIC = "MGW3";
    constexpr size_t SEAL_IV = 12;
    constexpr size_t SEAL_TAG = 16;

    const std::string& sealKey()
    {
        static std::string key;
        if (key.size() == 32) return key;
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "mg_wardrobe.key");
        {
            llifstream in(path.c_str(), std::ios::in | std::ios::binary);
            if (in.is_open()) key.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }
        if (key.size() != 32)
        {
            unsigned char k[32];
            key.clear();
            if (RAND_bytes(k, (int)sizeof(k)) != 1) return key;
            key.assign((const char*)k, sizeof(k));
            llofstream out(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
            if (out.is_open()) out.write(key.data(), (std::streamsize)key.size());
        }
        return key;
    }

    bool seal(const std::string& plain, std::string& sealed)
    {
        const std::string& key = sealKey();
        if (key.size() != 32 || plain.empty()) return false;
        unsigned char iv[SEAL_IV];
        unsigned char tag[SEAL_TAG];
        if (RAND_bytes(iv, (int)sizeof(iv)) != 1) return false;
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return false;
        std::string body(plain.size(), '\0');
        int len = 0, fin = 0;
        const bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, (const unsigned char*)key.data(), iv) == 1
            && EVP_EncryptUpdate(ctx, (unsigned char*)&body[0], &len, (const unsigned char*)plain.data(), (int)plain.size()) == 1
            && EVP_EncryptFinal_ex(ctx, (unsigned char*)&body[0] + len, &fin) == 1
            && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, (int)SEAL_TAG, tag) == 1;
        EVP_CIPHER_CTX_free(ctx);
        if (!ok) return false;
        body.resize((size_t)(len + fin));
        sealed = SEAL_MAGIC + std::string((const char*)iv, SEAL_IV) + body + std::string((const char*)tag, SEAL_TAG);
        return true;
    }

    bool unseal(const std::string& sealed, std::string& plain)
    {
        const std::string& key = sealKey();
        const size_t head = SEAL_MAGIC.size() + SEAL_IV;
        plain.clear();
        if (key.size() != 32 || sealed.size() <= head + SEAL_TAG || sealed.compare(0, SEAL_MAGIC.size(), SEAL_MAGIC) != 0) return false;
        const size_t n = sealed.size() - head - SEAL_TAG;
        std::string tag = sealed.substr(sealed.size() - SEAL_TAG);
        std::string body(n, '\0');
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return false;
        int len = 0, fin = 0;
        const bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, (const unsigned char*)key.data(),
                                           (const unsigned char*)sealed.data() + SEAL_MAGIC.size()) == 1
            && EVP_DecryptUpdate(ctx, (unsigned char*)&body[0], &len, (const unsigned char*)sealed.data() + head, (int)n) == 1
            && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, (int)SEAL_TAG, &tag[0]) == 1
            && EVP_DecryptFinal_ex(ctx, (unsigned char*)&body[0] + len, &fin) == 1;
        EVP_CIPHER_CTX_free(ctx);
        if (!ok) return false;
        body.resize((size_t)(len + fin));
        plain.swap(body);
        return true;
    }

    std::string readFile(const std::string& path)
    {
        llifstream in(path.c_str(), std::ios::in | std::ios::binary);
        if (!in.is_open()) return std::string();
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    // seal and write a shape's LLSD (through a .tmp file, so a crash never leaves half a file)
    bool writeShapeFile(const LLUUID& item, const LLSD& sd)
    {
        std::ostringstream str;
        LLSDSerialize::toBinary(sd, str);
        std::string sealed;
        if (!seal(str.str(), sealed)) return false;
        const std::string path = shapeFile(item), tmp = path + ".tmp";
        {
            llofstream out(tmp.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
            if (!out.is_open()) return false;
            out.write(sealed.data(), (std::streamsize)sealed.size());
            if (!out.good()) return false;
        }
        LLFile::remove(path);
        return LLFile::rename(tmp, path) == 0;
    }
    std::string cardFile(const LLUUID& id) { return cacheDir() + gDirUtilp->getDirDelimiter() + id.asString() + "_card1.jpg"; }

    // where each known item sits on the body (cache/mgwardrobe/regions.llsd), for the
    // Wardrobe's categories without reading every shape
    std::map<LLUUID, std::string> sRegions;
    std::string regionsFile() { return cacheDir() + gDirUtilp->getDirDelimiter() + "regions.llsd"; }
    void readRegions()
    {
        llifstream in(regionsFile().c_str(), std::ios::in | std::ios::binary);
        if (!in.is_open()) return;
        LLSD sd;
        if (LLSDSerialize::fromXML(sd, in) <= 0) return;
        for (const auto& kv : llsd::inMap(sd))
        {
            if (LLUUID::validate(kv.first)) sRegions[LLUUID(kv.first)] = kv.second.asString();
        }
    }
    void setRegion(const LLUUID& item, const std::string& region)
    {
        if (region.empty()) sRegions.erase(item);
        else sRegions[item] = region;
        LLSD sd = LLSD::emptyMap();
        for (const auto& kv : sRegions) sd[kv.first.asString()] = kv.second;
        llofstream out(regionsFile().c_str(), std::ios::out | std::ios::binary);
        if (out.is_open()) LLSDSerialize::toXML(sd, out);
    }

    // the items with a known shape (the cache folder, read once)
    std::set<LLUUID> sKnown;
    bool sKnownRead = false;
    void readKnown()
    {
        if (sKnownRead) return;
        sKnownRead = true;
        readRegions();
        std::string name;
        {
            // v1 shapes (rest pose only, no joints): read again as items are worn
            std::vector<std::string> old;
            LLDirIterator it1(cacheDir(), "*.shape");
            while (it1.next(name)) old.push_back(name);
            for (const std::string& f : old) LLFile::remove(cacheDir() + gDirUtilp->getDirDelimiter() + f);
        }
        for (const char* pattern : { "*.shape3", "*.shape2" })
        {
            LLDirIterator it(cacheDir(), pattern);
            while (it.next(name))
            {
                const std::string id = name.substr(0, name.find('.'));
                if (LLUUID::validate(id)) sKnown.insert(LLUUID(id));
            }
        }
    }

    template <class T> LLSD::Binary blob(const std::vector<T>& v)
    {
        const U8* b = (const U8*)v.data();
        return LLSD::Binary(b, b + v.size() * sizeof(T));
    }
    template <class T> void unblob(const LLSD& sd, std::vector<T>& v)
    {
        const LLSD::Binary& b = sd.asBinary();
        v.resize(b.size() / sizeof(T));
        if (!v.empty()) memcpy(v.data(), b.data(), v.size() * sizeof(T));
    }
    LLSD vec(const LLVector3& v) { return LLSD().with(0, v.mV[0]).with(1, v.mV[1]).with(2, v.mV[2]); }
    LLVector3 vec(const LLSD& s) { return LLVector3((F32)s[0].asReal(), (F32)s[1].asReal(), (F32)s[2].asReal()); }

    std::map<LLUUID, ShapePtr> sShapes;   // the last few read or captured
    std::deque<LLUUID> sShapeOrder;
    constexpr size_t SHAPE_MEMORY = 8;

    void remember(const LLUUID& item, const ShapePtr& s)
    {
        if (!sShapes.count(item)) sShapeOrder.push_back(item);
        sShapes[item] = s;
        while (sShapeOrder.size() > SHAPE_MEMORY)
        {
            sShapes.erase(sShapeOrder.front());
            sShapeOrder.pop_front();
        }
    }

    bool saveShape(const LLUUID& item, const ShapePtr& s)
    {
        LLSD sd;
        sd["v"] = SHAPE_VERSION;
        sd["point"] = s->point; sd["region"] = s->region; sd["when"] = s->when;
        sd["prims"] = s->prims; sd["faces"] = s->faces; sd["tris"] = s->tris; sd["verts"] = s->verts;
        sd["rigged"] = s->rigged; sd["bom"] = s->bom; sd["pbr"] = s->pbr; sd["alpha"] = s->alpha;
        sd["lo"] = vec(s->lo); sd["hi"] = vec(s->hi);
        sd["ap"] = s->ap;
        LLSD skins = LLSD::emptyArray();
        for (const LLSD& k : s->skins) skins.append(k);
        sd["skins"] = skins;
        LLSD parts = LLSD::emptyArray();
        for (const Part& p : s->parts)
        {
            LLSD x;
            x["p"] = blob(p.pos); x["n"] = blob(p.nrm); x["t"] = blob(p.uv); x["i"] = blob(p.idx);
            x["tex"] = p.tex;
            x["c"] = LLSD().with(0, p.color.mV[0]).with(1, p.color.mV[1]).with(2, p.color.mV[2]).with(3, p.color.mV[3]);
            x["a"] = p.alpha; x["b"] = p.bright; x["prim"] = p.prim;
            x["sk"] = p.skin;
            if (!p.w.empty()) x["w"] = blob(p.w);
            parts.append(x);
        }
        sd["parts"] = parts;
        if (!writeShapeFile(item, sd)) return false;
        LLFile::remove(plainShapeFile(item));
        readKnown();
        sKnown.insert(item);
        remember(item, s);
        setRegion(item, s->region);
        LLFile::remove(cardFile(item));   // the card follows the new shape
        return true;
    }

    void forgetShape(const LLUUID& item)
    {
        LLFile::remove(shapeFile(item));
        LLFile::remove(plainShapeFile(item));
        LLFile::remove(cardFile(item));
        sKnown.erase(item);
        sShapes.erase(item);
        if (sRegions.count(item)) setRegion(item, "");
        sShapeOrder.erase(std::remove(sShapeOrder.begin(), sShapeOrder.end(), item), sShapeOrder.end());
    }

    ShapePtr loadShape(const LLUUID& item)
    {
        auto it = sShapes.find(item);
        if (it != sShapes.end()) return it->second;
        std::string data;
        bool convert = false;
        if (LLFile::isfile(shapeFile(item)))
        {
            if (!unseal(readFile(shapeFile(item)), data))
            {
                // another installation's key, or a damaged file: read the item again when worn
                LLFile::remove(shapeFile(item));
                sKnown.erase(item);
                return nullptr;
            }
        }
        else if (LLFile::isfile(plainShapeFile(item)))
        {
            data = readFile(plainShapeFile(item));
            convert = true;
        }
        if (data.empty()) return nullptr;
        std::istringstream str(data);
        LLSD sd;
        if (LLSDSerialize::fromBinary(sd, str, (llssize)data.size()) <= 0 || sd["v"].asInteger() != SHAPE_VERSION) return nullptr;
        if (convert && writeShapeFile(item, sd)) LLFile::remove(plainShapeFile(item));   // sealed from now on
        ShapePtr s = std::make_shared<Shape>();
        s->point = sd["point"].asString(); s->region = sd["region"].asString(); s->when = sd["when"].asInteger();
        s->prims = sd["prims"].asInteger(); s->faces = sd["faces"].asInteger();
        s->tris = sd["tris"].asInteger(); s->verts = sd["verts"].asInteger();
        s->rigged = sd["rigged"].asBoolean(); s->bom = sd["bom"].asBoolean();
        s->pbr = sd["pbr"].asBoolean(); s->alpha = sd["alpha"].asBoolean();
        s->lo = vec(sd["lo"]); s->hi = vec(sd["hi"]);
        s->ap = sd["ap"].asInteger();
        for (const auto& k : llsd::inArray(sd["skins"])) s->skins.push_back(k);
        for (const auto& x : llsd::inArray(sd["parts"]))
        {
            Part p;
            unblob(x["p"], p.pos); unblob(x["n"], p.nrm); unblob(x["t"], p.uv); unblob(x["i"], p.idx);
            if (p.pos.empty() || p.nrm.size() != p.pos.size() || p.uv.size() != p.pos.size() || p.idx.empty()) continue;
            bool bad = false;
            for (U16 i : p.idx) if (i >= p.pos.size()) { bad = true; break; }
            if (bad) continue;
            p.tex = x["tex"].asUUID();
            p.color = LLColor4((F32)x["c"][0].asReal(), (F32)x["c"][1].asReal(), (F32)x["c"][2].asReal(), (F32)x["c"][3].asReal());
            p.alpha = x["a"].asInteger(); p.bright = x["b"].asBoolean(); p.prim = x["prim"].asInteger();
            p.skin = x["sk"].asInteger();
            if (x.has("w")) unblob(x["w"], p.w);
            if (p.skin >= 0 && (p.skin >= (S32)s->skins.size() || p.w.size() != p.pos.size() * 4)) p.skin = -2;
            LLVector3 lo = p.pos[0], hi = p.pos[0];
            for (const LLVector3& v : p.pos) { lo.set(llmin(lo.mV[0], v.mV[0]), llmin(lo.mV[1], v.mV[1]), llmin(lo.mV[2], v.mV[2])); hi.set(llmax(hi.mV[0], v.mV[0]), llmax(hi.mV[1], v.mV[1]), llmax(hi.mV[2], v.mV[2])); }
            p.mid = (lo + hi) * 0.5f;
            s->parts.push_back(std::move(p));
        }
        if (s->parts.empty()) return nullptr;
        remember(item, s);
        return s;
    }

    // the texture transform of a Blinn-Phong face (as llface.cpp xform())
    void xformUV(LLVector2& tc, F32 cos_a, F32 sin_a, F32 off_s, F32 off_t, F32 mag_s, F32 mag_t)
    {
        F32 s = tc.mV[0] - 0.5f, t = tc.mV[1] - 0.5f;
        const F32 temp = s;
        s = s * cos_a + t * sin_a;
        t = -temp * sin_a + t * cos_a;
        tc.mV[0] = s * mag_s + off_s + 0.5f;
        tc.mV[1] = t * mag_t + off_t + 0.5f;
    }

    // the glTF texture transform of a PBR face (as textureUtilV.glsl texture_transform())
    void gltfUV(LLVector2& tc, const LLGLTFMaterial::TextureTransform& tt)
    {
        F32 x = tc.mV[0] * tt.mScale.mV[0], y = (1.f - tc.mV[1]) * tt.mScale.mV[1];
        const F32 c = cosf(tt.mRotation), s = sinf(tt.mRotation);
        const F32 x2 = c * x + s * y + tt.mOffset.mV[0];
        const F32 y2 = -s * x + c * y + tt.mOffset.mV[1];
        tc.set(x2, 1.f - y2);
    }

    // ---------------------------------------------------------------------
    // body regions: rigged mesh by the joints its vertices follow (weights),
    // the other prims by their attach point
    // ---------------------------------------------------------------------
    std::string jointRegion(std::string n)
    {
        LLStringUtil::toLower(n);
        auto starts = [&](const char* s) { return n.rfind(s, 0) == 0; };
        auto has = [&](const char* s) { return n.find(s) != std::string::npos; };
        if (starts("mhair")) return "hair";
        if (starts("mface") || starts("meye") || starts("mskull") || starts("mhead") || n == "head") return "head";
        if (starts("mneck") || n == "neck") return "neck";
        if (starts("mhand") || starts("mwrist") || has("_hand")) return "hands";
        if (starts("mfoot") || starts("mtoe") || starts("mankle") || has("_foot")) return "feet";
        if (starts("mknee") || starts("mhip") || starts("mhindlimb") || has("upper_leg") || has("lower_leg")) return "legs";
        if (starts("mpelvis") || n == "pelvis" || n == "butt" || starts("mgroin")) return "pelvis";
        if (starts("mtail")) return "tail";
        if (starts("mwing")) return "wings";
        if (starts("mchest") || starts("mtorso") || starts("mspine") || starts("mcollar") || starts("mshoulder") || starts("melbow")
            || has("chest") || has("pec") || n == "belly" || has("_back") || has("clavicle") || has("upper_arm") || has("lower_arm")) return "chest";
        return "";
    }

    std::string pointRegion(std::string p)
    {
        LLStringUtil::toLower(p);
        auto has = [&](const char* s) { return p.find(s) != std::string::npos; };
        if (has("foot")) return "feet";
        if (has("hand") || has("finger")) return "hands";
        if (has("skull") || has("chin") || has("ear") || has("eye") || has("nose") || has("mouth") || has("jaw") || has("tongue")) return "head";
        if (has("neck")) return "neck";
        if (has("tail")) return "tail";
        if (has("wing")) return "wings";
        if (has("chest") || has("spine") || has("pec") || has("stomach") || has("shoulder") || has("arm")) return "top";
        if (has("pelvis") || has("hip") || has("groin") || has("leg")) return "bottom";
        return "";
    }

    // the weights per joint region -> one region: body (a mesh body: head to feet), hair, head,
    // neck, hands, feet, top, bottom, full (top and bottom: dresses, suits), tail, wings
    std::string summarizeRegions(const std::map<std::string, F32>& acc)
    {
        F32 total = 0.f;
        for (const auto& kv : acc) total += kv.second;
        if (total <= 0.f) return "";
        auto f = [&](const char* k) { auto it = acc.find(k); return it == acc.end() ? 0.f : it->second / total; };
        const F32 head = f("head"), hair = f("hair"), neck = f("neck"), hands = f("hands"), feet = f("feet");
        const F32 legs = f("legs"), pelvis = f("pelvis"), chest = f("chest");
        if (head > 0.08f && chest > 0.15f && legs + pelvis > 0.15f && feet > 0.03f) return "body";
        if (hair > 0.35f) return "hair";
        if (f("tail") > 0.5f) return "tail";
        if (f("wings") > 0.5f) return "wings";
        if (feet > 0.55f) return "feet";
        if (hands > 0.55f) return "hands";
        if (head + hair > 0.55f) return "head";
        if (neck > 0.45f) return "neck";
        const F32 top = chest + neck * 0.5f, bottom = legs + pelvis;
        if (top > 0.2f && bottom > 0.2f) return "full";
        if (top >= bottom && top > 0.2f) return "top";
        if (bottom > 0.2f) return "bottom";
        return "";
    }

    // The highest level of detail of each prim, held while a shape is read: a mesh's
    // high LOD is asked from the mesh repository and lands in the held volume (the
    // attachment itself may be showing a lower one).
    struct Held
    {
        LLVolume* vol = nullptr;
        bool asked = false;
    };
    std::map<LLUUID, std::map<LLVolumeParams, Held>> sHeld;   // by item
    LLUUID sPeekItem;

    void releaseHeld(const LLUUID& item)
    {
        auto it = sHeld.find(item);
        if (it == sHeld.end()) return;
        for (auto& kv : it->second)
        {
            if (kv.second.vol) LLPrimitive::getVolumeManager()->unrefVolume(kv.second.vol);
        }
        sHeld.erase(it);
    }

    // the prim's volume at the highest detail; nullptr while a mesh's is still coming
    LLVolume* highVolume(const LLUUID& item, LLVOVolume* vol, LLVolume* cur)
    {
        if (vol->isSculpted() && !vol->isMesh()) return cur;   // a sculpt's shape comes from its texture
        const LLVolumeParams& params = cur->getParams();
        Held& h = sHeld[item][params];
        if (!h.vol) h.vol = LLPrimitive::getVolumeManager()->refVolume(params, 3);
        if (!h.vol) return cur;
        if (!vol->isMesh()) return h.vol->getNumVolumeFaces() > 0 ? h.vol : cur;
        if (h.vol->isMeshAssetLoaded() && h.vol->getNumVolumeFaces() > 0) return h.vol;
        if (!h.asked)
        {
            h.asked = true;
            gMeshRepo.loadMesh(vol, params, 3);
        }
        return nullptr;
    }

    // A worn object's shape: every prim of the linkset with its textures, rigged mesh
    // in its rest pose (bind shape), the other prims in the avatar's frame, each at its
    // highest level of detail. false while a mesh or that detail is still loading
    // (accept_low: take what the attachment shows).
    bool captureShape(const LLUUID& item, LLViewerObject* root, Shape& out, bool accept_low)
    {
        if (!root || root->isDead() || !isAgentAvatarValid() || !gAgentAvatarp->mRoot) return false;
        std::vector<LLViewerObject*> objs;
        objs.push_back(root);
        for (const auto& c : root->getChildren()) if (c.notNull()) objs.push_back(c.get());

        const glm::mat4 av_inv = glm::inverse(glm::make_mat4((const F32*)gAgentAvatarp->mRoot->getWorldMatrix().mMatrix));
        out = Shape();
        // the prims that are not rigged: in the frame of the point they are attached to
        const S32 ap = ATTACHMENT_ID_FROM_STATE(root->getAttachmentState());
        LLViewerJointAttachment* att = get_if_there(gAgentAvatarp->mAttachmentPoints, ap, (LLViewerJointAttachment*)nullptr);
        const glm::mat4 att_inv = att ? glm::inverse(glm::make_mat4((const F32*)att->getWorldMatrix().mMatrix)) : av_inv;
        out.ap = att ? ap : 0;
        std::map<const LLMeshSkinInfo*, S32> skin_index;
        std::map<std::string, F32> regions;   // rigged weight per body region
        bool first = true;
        S32 prim = -1;
        for (LLViewerObject* obj : objs)
        {
            ++prim;
            LLVOVolume* vol = dynamic_cast<LLVOVolume*>(obj);
            if (!vol || vol->mDrawable.isNull()) continue;
            LLVolume* v = vol->getVolume();
            if (!v) return false;
            LLVolume* hv = highVolume(item, vol, v);
            if (hv) v = hv;
            else if (!accept_low) return false;   // the high detail mesh is on its way
            if (vol->isMesh() && (!v->isMeshAssetLoaded() || v->getNumVolumeFaces() == 0)) return false;
            if (v->getNumVolumeFaces() == 0) continue;

            const LLMeshSkinInfo* skin = vol->isRiggedMesh() ? vol->getSkinInfo() : nullptr;
            glm::mat4 M;
            if (skin)
            {
                M = glm::make_mat4(skin->mBindShapeMatrix.getF32ptr());   // the rest pose
                out.rigged = true;
            }
            else
            {
                const glm::mat4 rel = glm::make_mat4((const F32*)vol->getRelativeXform().mMatrix);
                const glm::mat4 world = glm::make_mat4((const F32*)vol->getRenderMatrix().mMatrix);
                M = att_inv * world * rel;
            }
            S32 skin_at = -1;
            if (skin)
            {
                auto si = skin_index.find(skin);
                if (si == skin_index.end())
                {
                    skin_at = (S32)out.skins.size();
                    skin_index[skin] = skin_at;
                    out.skins.push_back(skin->asLLSD(false, false));
                }
                else skin_at = si->second;
            }
            const glm::mat3 Nm = glm::transpose(glm::inverse(glm::mat3(M)));
            ++out.prims;

            for (S32 f = 0; f < v->getNumVolumeFaces() && f < vol->getNumTEs(); ++f)
            {
                const LLVolumeFace& vf = v->getVolumeFace(f);
                if (vf.mNumVertices <= 0 || vf.mNumIndices <= 0 || !vf.mPositions || !vf.mIndices) continue;
                const LLTextureEntry* te = vol->getTE(f);
                if (!te) continue;

                Part p;
                p.prim = prim;
                p.skin = skin ? (vf.mWeights ? skin_at : -2) : -1;
                p.color = te->getColor();
                p.tex = te->getID();
                p.bright = te->getFullbright() != 0 || te->getGlow() > 0.f;
                bool pbr = false;
                LLGLTFMaterial::TextureTransform tt;
                LLGLTFMaterial* gm = te->getGLTFRenderMaterial();
                if (gm)
                {
                    pbr = true;
                    out.pbr = true;
                    p.tex = gm->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR];
                    p.color = srgbColor4(gm->mBaseColor);
                    p.alpha = gm->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND ? 1 : (gm->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_MASK ? 2 : 0);
                    tt = gm->mTextureTransform[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR];
                }
                else if (te->getMaterialParams().notNull())
                {
                    const U8 mode = te->getMaterialParams()->getDiffuseAlphaMode();
                    p.alpha = mode == LLMaterial::DIFFUSE_ALPHA_MODE_BLEND ? 1 : (mode == LLMaterial::DIFFUSE_ALPHA_MODE_MASK ? 2 : 0);
                    if (mode == LLMaterial::DIFFUSE_ALPHA_MODE_EMISSIVE) p.bright = true;
                }
                // faces made invisible on purpose (hidden layers of a body, alpha cuts)
                if (p.color.mV[VALPHA] < 0.02f || p.tex == IMG_INVISIBLE) continue;
                if (p.color.mV[VALPHA] < 0.99f) p.alpha = 1;
                if (LLAvatarAppearanceDictionary::isBakedImageId(p.tex)) out.bom = true;
                if (p.alpha == 1 || p.alpha == 2) out.alpha = true;

                F32 ss = 1.f, st = 1.f, os = 0.f, ot = 0.f;
                te->getScale(&ss, &st);
                te->getOffset(&os, &ot);
                const F32 rot = te->getRotation(), ca = cosf(rot), sa = sinf(rot);

                p.pos.reserve(vf.mNumVertices); p.nrm.reserve(vf.mNumVertices); p.uv.reserve(vf.mNumVertices);
                LLVector3 lo, hi;
                for (S32 k = 0; k < vf.mNumVertices; ++k)
                {
                    const F32* pp = vf.mPositions[k].getF32ptr();
                    const glm::vec4 w = M * glm::vec4(pp[0], pp[1], pp[2], 1.f);
                    const LLVector3 pos(w.x, w.y, w.z);
                    p.pos.push_back(pos);
                    if (k == 0) { lo = hi = pos; }
                    else
                    {
                        lo.set(llmin(lo.mV[0], pos.mV[0]), llmin(lo.mV[1], pos.mV[1]), llmin(lo.mV[2], pos.mV[2]));
                        hi.set(llmax(hi.mV[0], pos.mV[0]), llmax(hi.mV[1], pos.mV[1]), llmax(hi.mV[2], pos.mV[2]));
                    }
                    glm::vec3 n(0.f, 0.f, 1.f);
                    if (vf.mNormals)
                    {
                        const F32* nn = vf.mNormals[k].getF32ptr();
                        n = Nm * glm::vec3(nn[0], nn[1], nn[2]);
                        const F32 len = glm::length(n);
                        n = len > 1e-6f ? n / len : glm::vec3(0.f, 0.f, 1.f);
                    }
                    p.nrm.emplace_back(n.x, n.y, n.z);
                    LLVector2 tc = vf.mTexCoords ? vf.mTexCoords[k] : LLVector2(0.f, 0.f);
                    if (pbr) gltfUV(tc, tt);
                    else xformUV(tc, ca, sa, os, ot, ss, st);
                    p.uv.push_back(tc);
                }
                p.idx.assign(vf.mIndices, vf.mIndices + vf.mNumIndices);
                if (p.skin >= 0)
                {
                    p.w.resize((size_t)vf.mNumVertices * 4);
                    for (S32 k = 0; k < vf.mNumVertices; ++k) memcpy(&p.w[(size_t)k * 4], vf.mWeights[k].getF32ptr(), 4 * sizeof(F32));
                }
                if (skin && vf.mWeights && !skin->mJointNames.empty())
                {
                    // each vertex: up to 4 joints, as <joint index>.<weight>
                    const S32 joints = (S32)skin->mJointNames.size();
                    std::vector<std::string> names(joints);
                    for (S32 j = 0; j < joints; ++j) names[j] = jointRegion(skin->mJointNames[j]);
                    for (S32 k = 0; k < vf.mNumVertices; ++k)
                    {
                        const F32* w = vf.mWeights[k].getF32ptr();
                        for (S32 i = 0; i < 4; ++i)
                        {
                            const S32 j = llclamp((S32)floorf(w[i]), 0, joints - 1);
                            const F32 wt = w[i] - floorf(w[i]);
                            if (wt > 0.f && !names[j].empty()) regions[names[j]] += wt;
                        }
                    }
                }
                p.mid = (lo + hi) * 0.5f;
                if (first) { out.lo = lo; out.hi = hi; first = false; }
                else
                {
                    out.lo.set(llmin(out.lo.mV[0], lo.mV[0]), llmin(out.lo.mV[1], lo.mV[1]), llmin(out.lo.mV[2], lo.mV[2]));
                    out.hi.set(llmax(out.hi.mV[0], hi.mV[0]), llmax(out.hi.mV[1], hi.mV[1]), llmax(out.hi.mV[2], hi.mV[2]));
                }
                ++out.faces;
                out.tris += vf.mNumIndices / 3;
                out.verts += vf.mNumVertices;
                out.parts.push_back(std::move(p));
            }
        }
        if (out.parts.empty()) return accept_low;   // nothing visible: nothing to wait for
        std::string point;
        if (gAgentAvatarp->getAttachedPointName(root->getAttachmentItemID(), point)) out.point = point;
        out.region = summarizeRegions(regions);
        if (out.region.empty() && !out.rigged) out.region = pointRegion(out.point);
        out.when = (S32)time_corrected();
        return true;
    }

    // ---------------------------------------------------------------------
    // the 3D view (and the cards): the shape drawn alone in a dynamic texture,
    // textured like the mesh upload preview; only JPEG frames reach the page
    // ---------------------------------------------------------------------
    class MGItemStage : public LLViewerDynamicTexture
    {
    public:
        MGItemStage(S32 size, const ShapePtr& shape, const LLUUID& item, bool card)
            : LLViewerDynamicTexture(size, size, 3, ORDER_MIDDLE, false), mShape(shape), mItem(item), mCard(card)
        {
            mStart = LLTimer::getTotalSeconds();
            if (card) { mYaw = 0.6f; mPitch = 0.22f; }
            for (const Part& p : mShape->parts)
            {
                LLPointer<LLViewerTexture> t;
                if (p.tex.notNull())
                {
                    if (LLAvatarAppearanceDictionary::isBakedImageId(p.tex))
                    {
                        if (isAgentAvatarValid()) t = gAgentAvatarp->getBakedTexture((U8)LLAvatarAppearanceDictionary::assetIdToBakedTextureIndex(p.tex));
                    }
                    else
                    {
                        t = LLViewerTextureManager::getFetchedTexture(p.tex, FTT_DEFAULT, true, LLGLTexture::BOOST_PREVIEW);
                    }
                }
                mTex.push_back(t);
            }
            for (const LLSD& k : mShape->skins)
            {
                LLSD copy = k;
                mSkins.push_back(new LLMeshSkinInfo(copy));
            }
        }

        S8 getType() const override { return LLViewerDynamicTexture::LL_IMAGE_PREVIEW_SCULPTED; }

        // keeps the textures wanted; true when they are all there, sharp enough
        bool texturesReady()
        {
            bool ready = true;
            const F32 area = (F32)(mFullWidth * mFullHeight) * 2.f;
            for (const auto& t : mTex)
            {
                if (t.isNull()) continue;
                t->addTextureStats(area);
                const S32 d = t->getDiscardLevel();
                if (d < 0 || d > (mCard ? 2 : 1)) ready = false;
            }
            return ready;
        }

        // the skeleton the item is posed on: the wearer's own (their shape, their mesh body's
        // joint positions), so an item looks as it does on them; none outside the world
        static LLVOAvatar* skeleton()
        {
            return isAgentAvatarValid() && gAgentAvatarp->mRoot ? gAgentAvatarp.get() : nullptr;
        }

        bool needsRender() override
        {
            const F64 now = LLTimer::getTotalSeconds();
            if (mCard) return !mDone && now - mStart > 0.3 && (texturesReady() || now - mStart > 10.0);
            if (mAnimate && mOnMe && skeleton()) { texturesReady(); return now - mLastFrame >= 1.0 / 12.0; }
            if (now - mLastFrame < 1.0 / 15.0) return false;
            if (mDirty || mSpin) { texturesReady(); return true; }
            if (!mReady)
            {
                if (texturesReady() || now - mStart > 30.0) { mReady = true; return true; }
                return now - mLastFrame > 0.7;
            }
            return false;
        }

        // follow the wearer's moves live, or keep the pose of the moment the view opened
        void setAnimate(bool on)
        {
            mAnimate = on;
            mDirty = true;
        }

        // every face where it is. Its own shape (default): rigged faces in the rest pose they were
        // rigged in (each item is made for its own body), as the mesh upload preview shows a
        // model. On me (mOnMe): rigged faces skinned to the wearer's joints, as LLRiggedVolume
        // does it for worn mesh. Prims that are not rigged sit on their attach point. The frame
        // is the avatar's own axes (front +X), whatever the avatar does in the world.
        void pose()
        {
            LLVOAvatar* av = skeleton();
            const size_t n = mShape->parts.size();
            mPos.resize(n);
            mNrm.resize(n);
            LLMatrix4a root, at;
            root.setIdentity();
            at.setIdentity();
            if (av)
            {
                root = av->mRoot->getWorldMatrix4a();
                LLViewerJointAttachment* att = mShape->ap ? get_if_there(av->mAttachmentPoints, mShape->ap, (LLViewerJointAttachment*)nullptr) : nullptr;
                at = att ? att->getWorldMatrix4a() : root;
            }
            // world -> the avatar's frame (inverse of its root)
            LLMatrix4a back;
            {
                glm::mat4 r = glm::inverse(glm::make_mat4(root.getF32ptr()));
                back.loadu(glm::value_ptr(r));
            }
            bool first = true;
            auto grow = [&](const LLVector3& v)
            {
                if (first) { mLo = mHi = v; first = false; return; }
                mLo.set(llmin(mLo.mV[0], v.mV[0]), llmin(mLo.mV[1], v.mV[1]), llmin(mLo.mV[2], v.mV[2]));
                mHi.set(llmax(mHi.mV[0], v.mV[0]), llmax(mHi.mV[1], v.mV[1]), llmax(mHi.mV[2], v.mV[2]));
            };
            auto place = [&](size_t k, const LLMatrix4a& m)
            {
                const Part& p = mShape->parts[k];
                for (size_t i = 0; i < p.pos.size(); ++i)
                {
                    LLVector4a v, o, b;
                    v.load3(p.pos[i].mV);
                    m.affineTransform(v, o);
                    back.affineTransform(o, b);
                    LLVector4a nn, no, nb;
                    nn.load3(p.nrm[i].mV);
                    m.rotate(nn, no);
                    back.rotate(no, nb);
                    nb.normalize3fast();
                    mPos[k][i].set(b.getF32ptr());
                    mNrm[k][i].set(nb.getF32ptr());
                    grow(mPos[k][i]);
                }
            };
            for (size_t k = 0; k < n; ++k)
            {
                const Part& p = mShape->parts[k];
                mPos[k].resize(p.pos.size());
                mNrm[k].resize(p.pos.size());
                const S32 sk = p.skin;
                if (sk == -1) place(k, at);
                else if (!mOnMe || sk < 0 || !av || sk >= (S32)mSkins.size())
                {
                    // the rest pose: the bind space positions as read
                    for (size_t i = 0; i < p.pos.size(); ++i)
                    {
                        mPos[k][i] = p.pos[i];
                        mNrm[k][i] = p.nrm[i];
                        grow(mPos[k][i]);
                    }
                }
            }
            // on me: rigged faces, one skin (matrix palette) at a time
            for (S32 si = 0; si < (S32)mSkins.size() && av && mOnMe; ++si)
            {
                LLMeshSkinInfo* skin = mSkins[si];
                const U32 count = LLSkinningUtil::getMeshJointCount(skin);
                if (count == 0) continue;
                LLMatrix4a mat[LL_MAX_JOINTS_PER_MESH_OBJECT];
                LLSkinningUtil::initSkinningMatrixPalette(mat, count, skin, av);
                const U32 max_joints = llmin(count, LLSkinningUtil::getMaxJointCount());
                for (size_t k = 0; k < n; ++k)
                {
                    const Part& p = mShape->parts[k];
                    if (p.skin != si) continue;
                    for (size_t i = 0; i < p.pos.size(); ++i)
                    {
                        F32 w[4];
                        memcpy(w, &p.w[i * 4], sizeof(w));
                        LLMatrix4a fm;
                        LLSkinningUtil::getPerVertexSkinMatrix(w, mat, false, fm, max_joints);
                        LLVector4a v, o, b;
                        v.load3(p.pos[i].mV);
                        fm.affineTransform(v, o);
                        back.affineTransform(o, b);
                        LLVector4a nn, no, nb;
                        nn.load3(p.nrm[i].mV);
                        fm.rotate(nn, no);
                        back.rotate(no, nb);
                        nb.normalize3fast();
                        mPos[k][i].set(b.getF32ptr());
                        mNrm[k][i].set(nb.getF32ptr());
                        grow(mPos[k][i]);
                    }
                }
            }
            if (first) { mLo = mShape->lo; mHi = mShape->hi; }
            for (size_t k = 0; k < n; ++k)
            {
                LLVector3 lo, hi;
                const std::vector<LLVector3>& pp = mPos[k];
                if (pp.empty()) continue;
                lo = hi = pp[0];
                for (const LLVector3& v : pp)
                {
                    lo.set(llmin(lo.mV[0], v.mV[0]), llmin(lo.mV[1], v.mV[1]), llmin(lo.mV[2], v.mV[2]));
                    hi.set(llmax(hi.mV[0], v.mV[0]), llmax(hi.mV[1], v.mV[1]), llmax(hi.mV[2], v.mV[2]));
                }
                mMid.resize(n);
                mMid[k] = (lo + hi) * 0.5f;
            }
            mPosed = true;
        }

        void buildBuffers()
        {
            mVB.clear();
            for (size_t k = 0; k < mShape->parts.size(); ++k)
            {
                const Part& p = mShape->parts[k];
                LLPointer<LLVertexBuffer> vb = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL | LLVertexBuffer::MAP_TEXCOORD0);
                if (!vb->allocateBuffer((U32)p.pos.size(), (U32)p.idx.size()))
                {
                    mVB.push_back(nullptr);
                    continue;
                }
                LLStrider<LLVector3> v, nr;
                LLStrider<LLVector2> t;
                LLStrider<U16> i;
                vb->getVertexStrider(v);
                vb->getNormalStrider(nr);
                vb->getTexCoord0Strider(t);
                vb->getIndexStrider(i);
                for (size_t x = 0; x < p.pos.size(); ++x)
                {
                    *(v++) = mPos[k][x];
                    *(nr++) = mNrm[k][x];
                    *(t++) = p.uv[x];
                }
                for (U16 x : p.idx) *(i++) = x;
                vb->unmapBuffer();
                mVB.push_back(vb);
            }
        }

        // the face's look: 0 opaque, 1 see-through (blended)
        S32 blendOf(size_t k) const
        {
            const Part& p = mShape->parts[k];
            if (p.alpha == 1 || p.alpha == 2) return 1;
            if (p.alpha == 0) return 0;
            const LLViewerTexture* t = mTex[k].get();
            return t && t->getDiscardLevel() >= 0 && t->getComponents() == 4 ? 1 : 0;
        }

        void drawPart(size_t k, bool textured)
        {
            LLVertexBuffer* vb = k < mVB.size() ? mVB[k].get() : nullptr;
            if (!vb) return;
            const Part& p = mShape->parts[k];
            LLViewerTexture* t = mTex[k].get();
            if (textured && t && t->getDiscardLevel() >= 0) gGL.getTexUnit(0)->bind(t);
            else gGL.getTexUnit(0)->bind(LLViewerFetchedTexture::sWhiteImagep);
            if (textured)
            {
                LLColor4 c = p.color;
                if (p.bright)   // this shader lights every face: fullbright ones just brighter
                {
                    for (S32 i = 0; i < 3; ++i) c.mV[i] = llmin(1.f, c.mV[i] * 1.25f);
                }
                gGL.diffuseColor4fv(c.mV);
            }
            vb->setBuffer();
            vb->drawRange(LLRender::TRIANGLES, 0, (U32)p.pos.size() - 1, (U32)p.idx.size(), 0);
        }

        void studioLights()
        {
            gPipeline.enableLightsPreview();
            const LLColor4 ambient(0.46f, 0.46f, 0.5f, 1.f);
            gGL.setAmbientLightColor(ambient);
            const LLColor4 key(1.25f, 1.2f, 1.12f, 1.f), fill(0.5f, 0.56f, 0.7f, 1.f), rim(0.85f, 0.9f, 1.05f, 1.f);
            const LLColor4* cols[3] = { &key, &fill, &rim };
            for (S32 l = 1; l <= 3; ++l)
            {
                LLLightState* light = gGL.getLight(l);
                light->setDiffuse(*cols[l - 1]);
                light->setAmbient(ambient);
            }
        }

        bool render() override
        {
            mLastFrame = LLTimer::getTotalSeconds();
            mDirty = false;
            if (mSpin) mYaw += 0.03f;
            // following the wearer: posed again every frame; still: once
            if (!mPosed || (mAnimate && mOnMe && skeleton()))
            {
                pose();
                buildBuffers();
                if (!mFramed)
                {
                    mCenter = (mLo + mHi) * 0.5f;
                    mRadius = llmax(0.03f, (mHi - mLo).magVec() * 0.5f);
                    mFramed = true;
                }
            }
            const F32 W = (F32)mFullWidth, H = (F32)mFullHeight;

            // backdrop: 0 dark studio, 1 light studio, 2 grey
            gGL.matrixMode(LLRender::MM_PROJECTION);
            gGL.pushMatrix();
            gGL.loadIdentity();
            gGL.ortho(0.0f, W, 0.0f, H, -1.0f, 1.0f);
            gGL.matrixMode(LLRender::MM_MODELVIEW);
            gGL.pushMatrix();
            gGL.loadIdentity();
            gUIProgram.bind();
            {
                LLGLSUIDefault def;
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                static const F32 BG[3][2][3] = { { { 0.035f, 0.04f, 0.055f }, { 0.12f, 0.135f, 0.18f } },
                                                 { { 0.70f, 0.72f, 0.76f }, { 0.95f, 0.955f, 0.965f } },
                                                 { { 0.22f, 0.23f, 0.25f }, { 0.43f, 0.44f, 0.47f } } };
                const F32* lo = BG[mBg][0];
                const F32* hi = BG[mBg][1];
                gGL.begin(LLRender::TRIANGLES);
                gGL.color4f(lo[0], lo[1], lo[2], 1.f); gGL.vertex2f(0.f, 0.f);
                gGL.color4f(lo[0], lo[1], lo[2], 1.f); gGL.vertex2f(W, 0.f);
                gGL.color4f(hi[0], hi[1], hi[2], 1.f); gGL.vertex2f(W, H);
                gGL.color4f(lo[0], lo[1], lo[2], 1.f); gGL.vertex2f(0.f, 0.f);
                gGL.color4f(hi[0], hi[1], hi[2], 1.f); gGL.vertex2f(W, H);
                gGL.color4f(hi[0], hi[1], hi[2], 1.f); gGL.vertex2f(0.f, H);
                gGL.end();
            }
            gGL.matrixMode(LLRender::MM_PROJECTION);
            gGL.popMatrix();
            gGL.matrixMode(LLRender::MM_MODELVIEW);
            gGL.popMatrix();
            gGL.flush();

            // camera: around the item, in the avatar's frame (front = +X)
            LLVector3 center = mCenter;
            center.mV[VZ] += mLift * mRadius;
            LLViewerCamera* camera = LLViewerCamera::getInstance();
            const F32 fov = camera->getDefaultFOV();
            const F32 dist = mRadius / sinf(fov * 0.5f) * 1.04f / mZoom;
            const LLQuaternion rot = LLQuaternion(-mPitch, LLVector3::y_axis) * LLQuaternion(mYaw, LLVector3::z_axis);
            const LLVector3 eye = center + LLVector3(dist, 0.f, 0.f) * rot;
            camera->setOriginAndLookAt(eye, LLVector3::z_axis, center);
            camera->setViewNoBroadcast(fov);
            camera->setAspect(W / H);
            const F32 z_far = dist + mRadius * 4.f;
            camera->setPerspective(false, mOrigin.mX, mOrigin.mY, mFullWidth, mFullHeight, false, llclamp(dist * 0.01f, 0.002f, 0.1f), z_far);

            // a turntable under the item
            if (mGrid)
            {
                gUIProgram.bind();
                LLGLDepthTest depth(GL_TRUE, GL_FALSE);
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                const LLVector3 c = center;
                const F32 z = mLo.mV[VZ] - mRadius * 0.01f;
                const F32 r0 = llmax(mHi.mV[VX] - mLo.mV[VX], mHi.mV[VY] - mLo.mV[VY]) * 0.62f + mRadius * 0.12f;
                const F32 g = mBg == 1 ? 0.3f : 0.75f;
                gGL.begin(LLRender::LINES);
                for (S32 ring = 0; ring < 3; ++ring)
                {
                    const F32 r = r0 * (1.f - ring * 0.3f);
                    gGL.color4f(g, g + 0.05f, g + 0.12f, ring == 0 ? 0.5f : 0.22f);
                    for (S32 i = 0; i < 72; ++i)
                    {
                        const F32 a0 = F_TWO_PI * i / 72.f, a1 = F_TWO_PI * (i + 1) / 72.f;
                        gGL.vertex3f(c.mV[VX] + r * cosf(a0), c.mV[VY] + r * sinf(a0), z);
                        gGL.vertex3f(c.mV[VX] + r * cosf(a1), c.mV[VY] + r * sinf(a1), z);
                    }
                }
                gGL.end();
                gGL.flush();
            }

            // the item, textured like the mesh upload preview
            gObjectPreviewProgram.bind();
            studioLights();
            gObjectPreviewProgram.uniform4fv(LLShaderMgr::AMBIENT, 1, LLColor4(0.46f, 0.46f, 0.5f, 1.f).mV);
            {
                LLGLDisable no_cull(GL_CULL_FACE);
                const bool textured = mMode != 1;
                if (!textured) gGL.diffuseColor4f(0.8f, 0.81f, 0.84f, 1.f);
                // solid faces, then the see-through ones far to near
                {
                    LLGLDepthTest depth(GL_TRUE, GL_TRUE);
                    LLGLDisable no_blend(GL_BLEND);
                    for (size_t k = 0; k < mShape->parts.size(); ++k)
                    {
                        if (textured && blendOf(k)) continue;
                        drawPart(k, textured);
                    }
                }
                if (textured)
                {
                    std::vector<std::pair<F32, size_t>> order;
                    for (size_t k = 0; k < mShape->parts.size(); ++k)
                    {
                        if (blendOf(k)) order.emplace_back(-((k < mMid.size() ? mMid[k] : center) - eye).magVecSquared(), k);
                    }
                    std::sort(order.begin(), order.end());
                    LLGLEnable blend(GL_BLEND);
                    gGL.setSceneBlendType(LLRender::BT_ALPHA);
                    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
                    for (const auto& o : order) drawPart(o.second, true);
                }
                // the wires: everything (wire mode) or the part picked in the list
                if (mMode == 2 || mPart >= 0)
                {
                    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
                    LLGLEnable blend(GL_BLEND);
                    gGL.setSceneBlendType(LLRender::BT_ALPHA);
                    gGL.getTexUnit(0)->bind(LLViewerFetchedTexture::sWhiteImagep);
                    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                    for (size_t k = 0; k < mShape->parts.size(); ++k)
                    {
                        const bool picked = mShape->parts[k].prim == mPart;
                        if (mMode != 2 && !picked) continue;
                        gGL.diffuseColor4f(picked ? 1.f : 0.f, picked ? 0.85f : 0.83f, picked ? 0.2f : 1.f, picked ? 0.95f : 0.55f);
                        drawPart(k, false);
                    }
                    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                }
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            }
            gObjectPreviewProgram.unbind();
            gGL.color4f(1, 1, 1, 1);

            // read the frame back
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
            LLPointer<LLImageJPEG> jpeg = new LLImageJPEG(mCard ? 86 : 82);
            if (!jpeg->encode(raw, 0.f)) return true;
            const std::string uri = "data:image/jpeg;base64," + LLBase64::encode(jpeg->getData(), jpeg->getDataSize());
            if (mCard)
            {
                mDone = true;
                llofstream out(cardFile(mItem).c_str(), std::ios::out | std::ios::binary);
                if (out.is_open()) out.write((const char*)jpeg->getData(), jpeg->getDataSize());
                if (mUpload)
                {
                    // the 3D view as the item's inventory thumbnail (seen in every viewer)
                    const LLUUID item = mItem;
                    LLFloaterSimpleSnapshot::uploadThumbnail(raw, item, LLUUID::null, [item](const LLUUID&)
                        {
                            push(LLSD().with("type", "ward_thumb").with("id", item.asString()).with("ok", true));
                        });
                    push(LLSD().with("type", "ward_thumb").with("id", mItem.asString()).with("image", uri).with("sending", true));
                }
                push(LLSD().with("type", "inv_thumb").with("id", mItem.asString()).with("image", uri).with("card", true));
                return true;
            }
            push(LLSD().with("type", "ward_stage").with("id", mItem.asString()).with("image", uri).with("loading", !mReady));
            return true;
        }

        ShapePtr mShape;
        LLUUID mItem;
        bool mCard = false, mUpload = false, mDone = false;
        std::vector<LLPointer<LLViewerTexture>> mTex;
        std::vector<LLPointer<LLVertexBuffer>> mVB;
        std::vector<LLPointer<LLMeshSkinInfo>> mSkins;
        std::vector<std::vector<LLVector3>> mPos, mNrm;   // the faces as posed now
        std::vector<LLVector3> mMid;
        LLVector3 mLo, mHi, mCenter;
        F32 mRadius = 0.5f;
        bool mPosed = false, mFramed = false, mAnimate = false, mOnMe = false;
        bool mReady = false, mDirty = true, mSpin = false, mGrid = true;
        S32 mBg = 0, mMode = 0, mPart = -1;   // mode 0 textured, 1 clay, 2 wires
        F32 mYaw = 0.35f, mPitch = 0.12f, mZoom = 1.f, mLift = 0.f;
        F64 mStart = 0.0, mLastFrame = 0.0;

    };

    LLPointer<MGItemStage> sStage;
    LLPointer<MGItemStage> sCard;
    std::deque<std::pair<LLUUID, bool>> sCards;   // item, upload as its thumbnail

    // ---------------------------------------------------------------------
    // reading shapes: every worn attachment once per session (session idle),
    // and "peeks" (an item added for a moment, read, taken off)
    // ---------------------------------------------------------------------
    std::set<LLUUID> sSeen;
    std::map<LLUUID, S32> sTries;
    F64 sNextWatch = 0.0;

    void watchIdle(void*)
    {
        const F64 now = LLTimer::getTotalSeconds();
        if (now < sNextWatch || !isAgentAvatarValid()) return;
        sNextWatch = now + 2.5;
        // volumes held for items no longer worn (detached while loading)
        for (auto it = sHeld.begin(); it != sHeld.end();)
        {
            const LLUUID held = it->first;
            ++it;
            if (held != sPeekItem && !gAgentAvatarp->getWornAttachment(held)) releaseHeld(held);
        }
        for (const auto& kv : gAgentAvatarp->mAttachmentPoints)
        {
            LLViewerJointAttachment* att = kv.second;
            if (!att || att->getIsHUDAttachment()) continue;
            for (const auto& o : att->mAttachedObjects)
            {
                LLViewerObject* obj = o.get();
                if (!obj || obj->isDead()) continue;
                const LLUUID item = obj->getAttachmentItemID();
                if (item.isNull() || sSeen.count(item)) continue;
                S32& tries = sTries[item];
                ShapePtr s = std::make_shared<Shape>();
                if (!captureShape(item, obj, *s, tries >= 8))
                {
                    ++tries;
                    continue;
                }
                sSeen.insert(item);
                releaseHeld(item);
                if (!s->parts.empty() && saveShape(item, s)) push(LLSD().with("type", "ward_changed").with("id", item.asString()));
                return;   // one a tick
            }
        }
    }

    struct Peek
    {
        LLUUID item;
        S32 phase = 0;
        bool ours = false;
        F64 t0 = 0.0, next = 0.0;
        S32 tries = 0;
    };
    std::unique_ptr<Peek> sPeek;

    void endPeek(bool ok, const std::string& error = "")
    {
        if (!sPeek) return;
        if (sPeek->ours) LLAppearanceMgr::instance().removeItemFromAvatar(sPeek->item);
        releaseHeld(sPeek->item);
        sPeekItem.setNull();
        LLSD ev = LLSD().with("type", "ward_peek").with("id", sPeek->item.asString()).with("ok", ok);
        if (!error.empty()) ev["error"] = error;
        sPeek.reset();
        push(ev);
    }

    void peekIdle()
    {
        if (!sPeek || !isAgentAvatarValid()) return;
        const F64 now = LLTimer::getTotalSeconds();
        if (sPeek->phase == 0)
        {
            sPeek->ours = !get_is_item_worn(sPeek->item);
            if (sPeek->ours) LLAppearanceMgr::instance().wearItemOnAvatar(sPeek->item, true, false);
            sPeek->phase = 1;
            sPeek->t0 = now;
            sPeek->next = now + 0.5;
            return;
        }
        if (now < sPeek->next) return;
        sPeek->next = now + 0.5;
        if (now - sPeek->t0 > 35.0)
        {
            endPeek(false, "timeout");
            return;
        }
        LLViewerObject* obj = gAgentAvatarp->getWornAttachment(sPeek->item);
        if (!obj) return;
        ShapePtr s = std::make_shared<Shape>();
        if (!captureShape(sPeek->item, obj, *s, ++sPeek->tries > 30)) return;
        if (s->parts.empty())
        {
            endPeek(false, "nothing to see");
            return;
        }
        sSeen.insert(sPeek->item);
        endPeek(saveShape(sPeek->item, s), "");
    }

    // ---------------------------------------------------------------------
    // outfit photos: the camera in front of the avatar, a square shot without
    // the interface, set as the outfit folder's thumbnail
    // ---------------------------------------------------------------------
    struct Photo
    {
        LLUUID id;
        S32 phase = 0;
        F64 t = 0.0;
    };
    std::unique_ptr<Photo> sPhoto;

    void photoIdle()
    {
        if (!sPhoto) return;
        const F64 now = LLTimer::getTotalSeconds();
        if (sPhoto->phase == 0)
        {
            MGPhoneCamera::runCommand("cam_view", LLSD().with("preset", "person").with("dist", 3.1));
            sPhoto->phase = 1;
            sPhoto->t = now;
            return;
        }
        if (sPhoto->phase != 1 || now - sPhoto->t < 1.4) return;
        sPhoto->phase = 2;
        const LLUUID id = sPhoto->id;
        // never render from inside the phone's draw: next idle
        doOnIdleOneTime([id]()
            {
                LLPointer<LLImageRaw> raw = new LLImageRaw;
                MGPhoneCamera::hideGuidesForShot(true);
                const ECursorType cursor = gViewerWindow->getWindow() ? gViewerWindow->getWindow()->getCursor() : UI_CURSOR_ARROW;
                const bool ok = gViewerWindow->rawSnapshot(raw, 512, 512, false, false, false, false, false, false, false);
                gViewerWindow->setCursor(cursor);
                MGPhoneCamera::hideGuidesForShot(false);
                MGPhoneCamera::runCommand("cam_view", LLSD().with("preset", "reset"));
                sPhoto.reset();
                LLSD ev = LLSD().with("type", "ward_photo").with("id", id.asString());
                if (!ok || !raw->getData())
                {
                    push(ev.with("ok", false).with("error", "snapshot failed"));
                    return;
                }
                LLPointer<LLImageRaw> small = new LLImageRaw(raw->getData(), raw->getWidth(), raw->getHeight(), raw->getComponents());
                small->scale(CARD_SIZE, CARD_SIZE);
                LLPointer<LLImageJPEG> jpeg = new LLImageJPEG(88);
                if (jpeg->encode(small, 0.f))
                {
                    llofstream out(cardFile(id).c_str(), std::ios::out | std::ios::binary);
                    if (out.is_open()) out.write((const char*)jpeg->getData(), jpeg->getDataSize());
                    ev["image"] = "data:image/jpeg;base64," + LLBase64::encode(jpeg->getData(), jpeg->getDataSize());
                }
                LLFloaterSimpleSnapshot::uploadThumbnail(raw, id, LLUUID::null, [id](const LLUUID&)
                    {
                        push(LLSD().with("type", "ward_photo").with("id", id.asString()).with("ok", true).with("uploaded", true));
                    });
                push(ev.with("ok", true));
            });
    }

    // ---------------------------------------------------------------------
    // clothing layers and body parts: their textures, as pictures under keys
    // ---------------------------------------------------------------------
    void onWearable(LLViewerWearable* w, void* data)
    {
        LLUUID* idp = (LLUUID*)data;
        const LLUUID id = idp ? *idp : LLUUID::null;
        delete idp;
        LLSD ev = LLSD().with("type", "ward_system").with("id", id.asString());
        if (!w)
        {
            push(ev.with("ok", false).with("error", "could not load"));
            return;
        }
        const LLAvatarAppearanceDictionary* dict = LLAvatarAppearance::getDictionary();
        LLSD slots = LLSD::emptyArray();
        for (S32 te = 0; te < TEX_NUM_INDICES && dict; ++te)
        {
            const LLAvatarAppearanceDictionary::TextureEntry* entry = dict->getTexture((ETextureIndex)te);
            if (!entry || !entry->mIsLocalTexture || entry->mWearableType != w->getType()) continue;
            const LLLocalTextureObject* lto = w->getLocalTextureObject(te);
            if (!lto || lto->getID().isNull()) continue;
            LLUUID key;
            key.generate(id.asString() + ":" + std::to_string(te));
            LLSD slot = LLSD().with("name", entry->mName).with("key", key.asString());
            const std::string img = MGPhoneInventory::cachedThumb(lto->getID(), 256);
            if (!img.empty()) slot["image"] = img;
            else MGPhoneInventory::queueThumb(key, lto->getID(), 256);
            slots.append(slot);
        }
        push(ev.with("ok", true).with("slots", slots));
    }

    // attachments worn now (LLVOAvatar's own count is protected)
    S32 attachedCount()
    {
        S32 n = 0;
        if (!isAgentAvatarValid()) return 0;
        for (const auto& kv : gAgentAvatarp->mAttachmentPoints)
        {
            if (kv.second) n += kv.second->getNumObjects();
        }
        return n;
    }

    LLSD outfitRow(const LLViewerInventoryCategory* cat, const LLUUID& base)
    {
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendents(cat->getUUID(), cats, items, LLInventoryModel::EXCLUDE_TRASH);
        S32 n = 0, worn = 0;
        for (const auto& it : items)
        {
            const LLViewerInventoryItem* real = it->getLinkedItem() ? it->getLinkedItem() : it.get();
            if (!real || !isWear(real)) continue;
            ++n;
            if (get_is_item_worn(real->getUUID())) ++worn;
        }
        LLSD r;
        r["id"] = cat->getUUID().asString();
        r["n"] = cat->getName();
        r["items"] = n;
        r["worn"] = worn;
        r["base"] = cat->getUUID() == base;
        r["th"] = cat->getThumbnailUUID().notNull();
        r["card"] = LLFile::isfile(cardFile(cat->getUUID()));
        return r;
    }
}

namespace MGPhoneWardrobe
{
    bool isCommand(const std::string& cmd)
    {
        return cmd.rfind("ward_", 0) == 0;
    }

    std::string cardPath(const LLUUID& item)
    {
        const std::string p = cardFile(item);
        return LLFile::isfile(p) ? p : "";
    }

    bool hasShape(const LLUUID& item)
    {
        readKnown();
        return sKnown.count(item) > 0;
    }

    std::string regionOf(const LLUUID& item)
    {
        readKnown();
        auto it = sRegions.find(item);
        return it == sRegions.end() ? "" : it->second;
    }

    void startWatching()
    {
        static bool started = false;
        if (started) return;
        started = true;
        gIdleCallbacks.addFunction(watchIdle, nullptr);
    }

    void idle()
    {
        peekIdle();
        photoIdle();
        if (sCard.notNull() && sCard->mDone) sCard = nullptr;
        while (sCard.isNull() && !sCards.empty())
        {
            const auto job = sCards.front();
            sCards.pop_front();
            ShapePtr s = loadShape(job.first);
            if (!s)
            {
                push(LLSD().with("type", "inv_thumb").with("id", job.first.asString()).with("error", "no shape"));
                continue;
            }
            sCard = new MGItemStage(CARD_SIZE, s, job.first, true);
            sCard->mUpload = job.second;
        }
    }

    void onClose()
    {
        sStage = nullptr;
        sCard = nullptr;
        sCards.clear();
        endPeek(false, "closed");
        if (sPhoto)
        {
            sPhoto.reset();
            MGPhoneCamera::runCommand("cam_view", LLSD().with("preset", "reset"));
        }
    }

    LLSD runCommand(const std::string& cmd, const LLSD& args)
    {
        LLSD result = LLSD().with("ok", true);
        startWatching();
        if (!isAgentAvatarValid()) return fail("no avatar");
        const LLUUID id(args["id"].asString());

        if (cmd == "ward_status")
        {
            const LLUUID base = LLAppearanceMgr::instance().getBaseOutfitUUID();
            const LLViewerInventoryCategory* bc = base.notNull() ? gInventory.getCategory(base) : nullptr;
            result["outfits"] = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS).asString();
            result["base"] = bc ? base.asString() : "";
            result["baseName"] = bc ? bc->getName() : "";
            result["attachments"] = attachedCount();
            result["maxAttachments"] = gAgentAvatarp->getMaxAttachments();
            // the avatar's shape decides the wardrobe's categories and icons (her / his)
            result["male"] = gAgentAvatarp->getVisualParamWeight("male") > 0.5f;
            return result;
        }

        if (cmd == "ward_worn")
        {
            LLInventoryModel::cat_array_t cats;
            LLInventoryModel::item_array_t items;
            gInventory.collectDescendents(LLAppearanceMgr::instance().getCOF(), cats, items, LLInventoryModel::EXCLUDE_TRASH);
            LLSD rows = LLSD::emptyArray();
            std::set<LLUUID> done;
            for (const auto& link : items)
            {
                const LLViewerInventoryItem* real = link->getLinkedItem();
                if (!real || !isWear(real) || done.count(real->getUUID())) continue;
                done.insert(real->getUUID());
                LLSD r = rowOf(real);
                std::string point;
                if (real->getType() == LLAssetType::AT_OBJECT && gAgentAvatarp->getAttachedPointName(real->getUUID(), point)) r["pt"] = point;
                r["f"] = real->getParentUUID().asString();
                rows.append(r);
            }
            result["rows"] = rows;
            return result;
        }

        if (cmd == "ward_wear" || cmd == "ward_off")
        {
            LLViewerInventoryItem* item = ownItem(id);
            if (!isWear(item)) return fail("not found");
            if (cmd == "ward_wear")
            {
                // add: one more layer / attachment; else it replaces its layer or attach point
                LLAppearanceMgr::instance().wearItemOnAvatar(id, true, !args["add"].asBoolean());
                return result;
            }
            if (item->getType() == LLAssetType::AT_BODYPART) return fail("a body part can only be replaced");
            LLAppearanceMgr::instance().removeItemFromAvatar(id);
            return result;
        }

        if (cmd == "ward_outfits")
        {
            const LLUUID mine = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
            const LLUUID base = LLAppearanceMgr::instance().getBaseOutfitUUID();
            LLInventoryModel::cat_array_t* cats = nullptr;
            LLInventoryModel::item_array_t* items = nullptr;
            gInventory.getDirectDescendentsOf(mine, cats, items);
            LLSD rows = LLSD::emptyArray();
            if (cats)
            {
                for (const auto& cat : *cats)
                {
                    if (cat) rows.append(outfitRow(cat.get(), base));
                }
            }
            result["rows"] = rows;
            result["root"] = mine.asString();
            result["base"] = base.asString();
            return result;
        }

        if (cmd == "ward_outfit")
        {
            LLViewerInventoryCategory* cat = gInventory.getCategory(id);
            if (!cat) return fail("not found");
            LLInventoryModel::cat_array_t cats;
            LLInventoryModel::item_array_t items;
            gInventory.collectDescendents(id, cats, items, LLInventoryModel::EXCLUDE_TRASH);
            LLSD rows = LLSD::emptyArray();
            for (const auto& it : items)
            {
                const LLViewerInventoryItem* real = it->getLinkedItem() ? it->getLinkedItem() : it.get();
                if (!real || !isWear(real)) continue;
                rows.append(rowOf(real));
            }
            result["rows"] = rows;
            result["name"] = cat->getName();
            return result;
        }

        if (cmd == "ward_outfit_thumbs")
        {
            // outfit folders' pictures: their inventory thumbnail, else the local photo
            const S32 size = llclamp(args.has("size") ? args["size"].asInteger() : CARD_SIZE, 64, 512);
            LLSD cached = LLSD::emptyArray();
            for (const auto& v : llsd::inArray(args["ids"]))
            {
                const LLUUID cid(v.asString());
                const LLViewerInventoryCategory* cat = gInventory.getCategory(cid);
                if (!cat) continue;
                if (cat->getThumbnailUUID().notNull())
                {
                    const std::string img = MGPhoneInventory::cachedThumb(cat->getThumbnailUUID(), size);
                    if (!img.empty()) cached.append(LLSD().with("id", cid.asString()).with("image", img));
                    else MGPhoneInventory::queueThumb(cid, cat->getThumbnailUUID(), size);
                    continue;
                }
                const std::string card = cardPath(cid);
                if (card.empty()) continue;
                llifstream in(card.c_str(), std::ios::in | std::ios::binary);
                std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (data.size() > 64) cached.append(LLSD().with("id", cid.asString()).with("image", "data:image/jpeg;base64," + LLBase64::encode((const U8*)data.data(), (U32)data.size())));
            }
            result["cached"] = cached;
            return result;
        }

        if (cmd == "ward_outfit_wear" || cmd == "ward_outfit_off")
        {
            LLViewerInventoryCategory* cat = gInventory.getCategory(id);
            if (!cat) return fail("not found");
            if (cmd == "ward_outfit_off") LLAppearanceMgr::instance().takeOffOutfit(id);
            else LLAppearanceMgr::instance().wearInventoryCategory(cat, false, args["add"].asBoolean());
            return result;
        }

        if (cmd == "ward_outfit_save")
        {
            std::string name = args["name"].asString();
            LLStringUtil::trim(name);
            if (name.empty()) return fail("no name");
            LLAppearanceMgr::instance().makeNewOutfitLinks(name.substr(0, 63), false);
            return result;
        }

        if (cmd == "ward_outfit_update")
        {
            if (LLAppearanceMgr::instance().getBaseOutfitUUID().isNull()) return fail("no current outfit");
            if (!LLAppearanceMgr::instance().updateBaseOutfit()) return fail("could not save");
            return result;
        }

        if (cmd == "ward_photo")
        {
            if (!gInventory.getCategory(id)) return fail("not found");
            if (sPhoto) return fail("busy");
            sPhoto = std::make_unique<Photo>();
            sPhoto->id = id;
            return result;
        }

        if (cmd == "ward_thumb_3d")
        {
            LLViewerInventoryItem* item = ownItem(id);
            if (!item || !hasShape(id)) return fail("not found");
            if (!item->getPermissions().allowModifyBy(gAgentID)) return fail("no modify permission");
            sCards.emplace_front(id, true);
            return result;
        }

        if (cmd == "ward_shape")
        {
            LLViewerInventoryItem* item = ownItem(id);
            if (!item) return fail("not found");
            ShapePtr s = hasShape(id) ? loadShape(id) : nullptr;
            result["has"] = !!s;
            result["worn"] = get_is_item_worn(id);
            result["object"] = item->getType() == LLAssetType::AT_OBJECT;
            if (s) result["facts"] = s->facts();
            return result;
        }

        if (cmd == "ward_peek")
        {
            LLViewerInventoryItem* item = ownItem(id);
            if (!item || item->getType() != LLAssetType::AT_OBJECT) return fail("objects only");
            if (sPeek) return fail("busy");
            if (attachedCount() >= gAgentAvatarp->getMaxAttachments() && !get_is_item_worn(id)) return fail("too many attachments");
            if (args["fresh"].asBoolean()) forgetShape(id);   // Refresh 3D: read it again from scratch
            sSeen.erase(id);
            sTries.erase(id);
            releaseHeld(id);
            sPeek = std::make_unique<Peek>();
            sPeek->item = id;
            sPeekItem = id;
            return result;
        }

        if (cmd == "ward_stage")
        {
            ShapePtr s = hasShape(id) ? loadShape(id) : nullptr;
            if (!s) return fail("no shape");
            const S32 size = llclamp(args.has("size") ? args["size"].asInteger() : 512, 256, 768);
            sStage = new MGItemStage(size, s, id, false);
            if (args.has("anim")) sStage->mAnimate = args["anim"].asBoolean();   // follow the wearer's moves
            if (args.has("onme")) sStage->mOnMe = args["onme"].asBoolean();     // on the wearer's body
            result["facts"] = s->facts();
            LLSD prims = LLSD::emptyArray();
            std::map<S32, S32> faces;
            for (const Part& p : s->parts) faces[p.prim]++;
            for (const auto& kv : faces) prims.append(LLSD().with("prim", kv.first).with("faces", kv.second));
            result["prims"] = prims;
            return result;
        }

        if (cmd == "ward_stage_set")
        {
            if (sStage.isNull()) return fail("no stage");
            MGItemStage& st = *sStage;
            if (args.has("dyaw")) st.mYaw += (F32)args["dyaw"].asReal();
            if (args.has("dpitch")) st.mPitch = llclamp(st.mPitch + (F32)args["dpitch"].asReal(), -1.45f, 1.45f);
            if (args.has("dzoom")) st.mZoom = llclamp(st.mZoom * (F32)args["dzoom"].asReal(), 0.5f, 8.f);
            if (args.has("lift")) st.mLift = llclamp((F32)args["lift"].asReal(), -1.f, 1.f);
            if (args.has("spin")) st.mSpin = args["spin"].asBoolean();
            if (args.has("grid")) st.mGrid = args["grid"].asBoolean();
            if (args.has("bg")) st.mBg = llclamp(args["bg"].asInteger(), 0, 2);
            if (args.has("mode")) st.mMode = llclamp(args["mode"].asInteger(), 0, 2);
            if (args.has("part")) st.mPart = args["part"].asInteger();
            if (args.has("anim")) st.setAnimate(args["anim"].asBoolean());
            if (args.has("onme") && args["onme"].asBoolean() != st.mOnMe)
            {
                st.mOnMe = args["onme"].asBoolean();
                st.mPosed = false;    // pose and frame again
                st.mFramed = false;
            }
            if (args.has("view"))
            {
                const std::string v = args["view"].asString();
                st.mZoom = 1.f; st.mLift = 0.f;
                if (v == "front") { st.mYaw = 0.f; st.mPitch = 0.08f; }
                else if (v == "back") { st.mYaw = F_PI; st.mPitch = 0.08f; }
                else if (v == "left") { st.mYaw = F_PI_BY_TWO; st.mPitch = 0.08f; }
                else if (v == "right") { st.mYaw = -F_PI_BY_TWO; st.mPitch = 0.08f; }
                else if (v == "top") { st.mYaw = 0.f; st.mPitch = 1.4f; }
                else { st.mYaw = 0.35f; st.mPitch = 0.12f; }
            }
            st.mDirty = true;
            return result;
        }

        if (cmd == "ward_stage_close")
        {
            sStage = nullptr;
            return result;
        }

        if (cmd == "ward_cards")
        {
            S32 queued = 0;
            for (const auto& v : llsd::inArray(args["ids"]))
            {
                const LLUUID item(v.asString());
                if (!hasShape(item) || !cardPath(item).empty()) continue;
                bool dup = false;
                for (const auto& c : sCards) if (c.first == item) { dup = true; break; }
                if (dup) continue;
                sCards.emplace_back(item, false);
                ++queued;
            }
            while (sCards.size() > 60) sCards.pop_back();
            result["queued"] = queued;
            return result;
        }

        if (cmd == "ward_system")
        {
            LLViewerInventoryItem* item = ownItem(id);
            if (!item || (item->getType() != LLAssetType::AT_CLOTHING && item->getType() != LLAssetType::AT_BODYPART)) return fail("not found");
            LLWearableList::instance().getAsset(item->getAssetUUID(), item->getName(), gAgentAvatarp, item->getType(), onWearable, new LLUUID(id));
            return result;
        }

        return fail("unknown command");
    }
}
