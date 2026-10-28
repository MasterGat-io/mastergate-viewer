/**
 * @file mgphoneplaces.cpp
 * @brief MasterGate Viewer: the MG Phone "Places" app (the avatar's landmarks).
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

#include "mgphoneplaces.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <set>

#include "llagent.h"
#include "llclipboard.h"
#include "llfloaterreg.h"
#include "llfloaterworldmap.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llinventoryobserver.h"
#include "lllandmark.h"
#include "lllandmarkactions.h"
#include "lllandmarklist.h"
#include "llparcel.h"
#include "llremoteparcelrequest.h"
#include "llstartup.h"
#include "llteleporthistorystorage.h"
#include "llviewerinventory.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "message.h"
#include "mgfloaterphone.h"

namespace
{
    void push(const LLSD& ev)
    {
        MGFloaterPhone* phone = LLFloaterReg::findTypedInstance<MGFloaterPhone>("mg_phone");
        if (phone) phone->pushToPage(ev);
    }

    LLSD fail(const std::string& why) { return LLSD().with("ok", false).with("error", why); }

    LLSD vec(const LLVector3d& v)
    {
        return LLSD().with(0, std::round(v.mdV[VX] * 10.0) / 10.0).with(1, std::round(v.mdV[VY] * 10.0) / 10.0).with(2, std::round(v.mdV[VZ] * 10.0) / 10.0);
    }

    // a landmark the avatar owns (not a link, not in the trash, not the Library)
    LLViewerInventoryItem* ownLandmark(const LLUUID& id)
    {
        LLViewerInventoryItem* item = gInventory.getItem(id);
        if (!item || item->getIsLinkType() || item->getType() != LLAssetType::AT_LANDMARK) return nullptr;
        if (!gInventory.isObjectDescendentOf(id, gInventory.getRootFolderID())) return nullptr;
        const LLUUID trash = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
        if (gInventory.isObjectDescendentOf(id, trash)) return nullptr;
        return item;
    }

    bool userFolder(const LLUUID& id)
    {
        if (!gInventory.getCategory(id)) return false;
        if (id != gInventory.getRootFolderID() && !gInventory.isObjectDescendentOf(id, gInventory.getRootFolderID())) return false;
        const LLUUID trash = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
        return id != trash && !gInventory.isObjectDescendentOf(id, trash);
    }

    struct CollectLandmarks : public LLInventoryCollectFunctor
    {
        bool operator()(LLInventoryCategory* cat, LLInventoryItem* item) override
        {
            return !cat && item && !item->getIsLinkType() && item->getType() == LLAssetType::AT_LANDMARK;
        }
    };

    // landmark item ids the Favorites bar links to
    std::set<LLUUID> favoriteIds(std::vector<LLUUID>* order = nullptr)
    {
        std::set<LLUUID> out;
        const LLUUID fav = gInventory.findCategoryUUIDForType(LLFolderType::FT_FAVORITE);
        if (fav.isNull()) return out;
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(fav, cats, items);
        if (!items) return out;
        std::vector<LLViewerInventoryItem*> sorted(items->begin(), items->end());
        std::sort(sorted.begin(), sorted.end(), [](LLViewerInventoryItem* a, LLViewerInventoryItem* b)
            { return a->getSortField() < b->getSortField(); });
        for (LLViewerInventoryItem* it : sorted)
        {
            if (!it || it->getType() != LLAssetType::AT_LANDMARK) continue;
            const LLUUID target = it->getLinkedUUID();
            if (out.insert(target).second && order) order->push_back(target);
        }
        return out;
    }

    // tell the page the landmarks moved (throttled)
    class PlacesWatch : public LLInventoryObserver
    {
    public:
        F64 mLast = 0.0;
        void changed(U32 mask) override
        {
            if (!(mask & (ADD | REMOVE | LABEL | STRUCTURE | INTERNAL))) return;
            const F64 now = LLTimer::getTotalSeconds();
            if (now - mLast < 1.0) return;
            mLast = now;
            push(LLSD().with("type", "place_changed"));
        }
    };
    PlacesWatch* sWatch = nullptr;
    void watch()
    {
        if (!sWatch)
        {
            sWatch = new PlacesWatch();
            gInventory.addObserver(sWatch);
        }
    }

    // ---------------------------------------------------------------------
    // place details: landmark asset -> global position -> parcel id -> parcel
    // data. A few at a time, each with a timeout; results cached for the session.
    // ---------------------------------------------------------------------
    std::map<std::string, LLSD> sInfo;   // key (item id, or a history key) -> details

    class InfoRequest : public LLRemoteParcelInfoObserver
    {
    public:
        enum Stage { LOAD, HANDLE, PARCEL, WAIT, DONE };
        std::string mKey;
        LLUUID mItem, mAsset, mRegion, mParcel;
        LLVector3d mPos;
        bool mHavePos = false;
        Stage mStage = LOAD;
        F64 mStarted = 0.0, mHandleAsked = 0.0;
        LLSD mOut;

        void processParcelInfo(const LLParcelData& d) override
        {
            mOut["parcel"] = d.name;
            mOut["desc"] = d.desc;
            mOut["region"] = d.sim_name;
            if (d.snapshot_id.notNull()) mOut["snap"] = d.snapshot_id.asString();
            mOut["rating"] = (d.flags & 0x2) ? "A" : ((d.flags & 0x1) ? "M" : "G");
            mOut["area"] = d.actual_area;
            mOut["traffic"] = (S32)d.dwell;
            if (d.sale_price > 0) mOut["sale"] = d.sale_price;
            mStage = DONE;
        }
        void setParcelID(const LLUUID& parcel_id) override
        {
            if (parcel_id.isNull()) { mStage = DONE; return; }
            mParcel = parcel_id;
            LLRemoteParcelInfoProcessor::getInstance()->addObserver(parcel_id, this);
            LLRemoteParcelInfoProcessor::getInstance()->sendParcelInfoRequest(parcel_id);
            mStage = WAIT;
        }
        void setErrorStatus(S32 status, const std::string& reason) override
        {
            mOut["perror"] = reason;
            mStage = DONE;
        }
        ~InfoRequest() override
        {
            if (mParcel.notNull()) LLRemoteParcelInfoProcessor::getInstance()->removeObserver(mParcel, this);
        }
    };

    std::deque<std::unique_ptr<InfoRequest>> sQueue;
    std::vector<std::unique_ptr<InfoRequest>> sActive;
    constexpr size_t MAX_ACTIVE = 3;
    constexpr F64 REQUEST_TIMEOUT = 25.0;

    bool queued(const std::string& key)
    {
        for (const auto& r : sQueue) if (r->mKey == key) return true;
        for (const auto& r : sActive) if (r->mKey == key) return true;
        return false;
    }

    void finish(InfoRequest* r)
    {
        LLSD out = r->mOut;
        out["ok"] = r->mHavePos;
        if (r->mHavePos)
        {
            const LLVector3d& p = r->mPos;
            out["pos"] = vec(p);
            out["gx"] = (S32)(p.mdV[VX] / REGION_WIDTH_METERS);
            out["gy"] = (S32)(p.mdV[VY] / REGION_WIDTH_METERS);
            out["x"] = ll_round((F32)fmod(p.mdV[VX], (F64)REGION_WIDTH_METERS));
            out["y"] = ll_round((F32)fmod(p.mdV[VY], (F64)REGION_WIDTH_METERS));
            out["z"] = ll_round((F32)p.mdV[VZ]);
        }
        else if (!out.has("error")) out["error"] = "unknown position";
        out["at"] = (S32)LLDate::now().secondsSinceEpoch();
        sInfo[r->mKey] = out;
        LLSD ev = out;
        ev["type"] = "place_info";
        ev["key"] = r->mKey;
        push(ev);
    }

    // distance from the avatar, recomputed each time it is sent
    LLSD withDistance(LLSD info)
    {
        if (info.has("pos") && gAgent.getRegion())
        {
            LLVector3d p(info["pos"][0].asReal(), info["pos"][1].asReal(), info["pos"][2].asReal());
            info["dist"] = ll_round((F32)(p - gAgent.getPositionGlobal()).magVec());
        }
        return info;
    }

    void step(InfoRequest* r)
    {
        const F64 now = LLTimer::getTotalSeconds();
        if (r->mStage == InfoRequest::LOAD)
        {
            LLLandmark* lm = gLandmarkList.getAsset(r->mAsset, nullptr);
            if (!lm) return;   // still loading
            LLVector3d pos;
            if (lm->getGlobalPos(pos))
            {
                r->mPos = pos;
                r->mHavePos = true;
                lm->getRegionID(r->mRegion);
                r->mStage = InfoRequest::PARCEL;
            }
            else if (lm->getRegionID(r->mRegion))
            {
                LLLandmark::requestRegionHandle(gMessageSystem, gAgent.getRegionHost(), r->mRegion, [](const LLUUID&, const U64&) {});
                r->mHandleAsked = now;
                r->mStage = InfoRequest::HANDLE;
            }
            else
            {
                r->mOut["error"] = "bad landmark";
                r->mStage = InfoRequest::DONE;
            }
            return;
        }
        if (r->mStage == InfoRequest::HANDLE)
        {
            LLLandmark* lm = gLandmarkList.getAsset(r->mAsset, nullptr);
            LLVector3d pos;
            if (lm && lm->getGlobalPos(pos))
            {
                r->mPos = pos;
                r->mHavePos = true;
                r->mStage = InfoRequest::PARCEL;
            }
            else if (now - r->mHandleAsked > 10.0)
            {
                r->mOut["error"] = "region not found";
                r->mStage = InfoRequest::DONE;
            }
            return;
        }
        if (r->mStage == InfoRequest::PARCEL)
        {
            LLViewerRegion* region = gAgent.getRegion();
            const std::string url = region ? region->getCapability("RemoteParcelRequest") : std::string();
            if (url.empty()) { r->mStage = InfoRequest::DONE; return; }
            const LLVector3 pos_region((F32)fmod(r->mPos.mdV[VX], (F64)REGION_WIDTH_METERS),
                                       (F32)fmod(r->mPos.mdV[VY], (F64)REGION_WIDTH_METERS), (F32)r->mPos.mdV[VZ]);
            r->mStage = InfoRequest::WAIT;
            LLRemoteParcelInfoProcessor::getInstance()->requestRegionParcelInfo(url, r->mRegion, pos_region, r->mPos, r->getObserverHandle());
        }
    }

    void enqueueItem(const LLUUID& id)
    {
        LLViewerInventoryItem* item = ownLandmark(id);
        if (!item) return;
        const std::string key = id.asString();
        if (queued(key)) return;
        auto r = std::make_unique<InfoRequest>();
        r->mKey = key;
        r->mItem = id;
        r->mAsset = item->getAssetUUID();
        sQueue.push_back(std::move(r));
    }

    void enqueuePoint(const std::string& key, const LLVector3d& pos)
    {
        if (key.empty() || queued(key)) return;
        auto r = std::make_unique<InfoRequest>();
        r->mKey = key;
        r->mPos = pos;
        r->mHavePos = true;
        r->mStage = InfoRequest::PARCEL;
        sQueue.push_back(std::move(r));
    }

    bool positionOf(const LLSD& args, LLVector3d& pos, LLViewerInventoryItem*& item)
    {
        item = nullptr;
        if (args.has("id"))
        {
            item = ownLandmark(LLUUID(args["id"].asString()));
            if (!item) return false;
            const auto it = sInfo.find(item->getUUID().asString());
            if (it != sInfo.end() && it->second.has("pos"))
            {
                pos.setVec(it->second["pos"][0].asReal(), it->second["pos"][1].asReal(), it->second["pos"][2].asReal());
                return true;
            }
            return LLLandmarkActions::getLandmarkGlobalPos(item->getUUID(), pos);
        }
        if (args.has("gx"))
        {
            pos.setVec(args["gx"].asReal(), args["gy"].asReal(), args["gz"].asReal());
            return !pos.isExactlyZero();
        }
        return false;
    }
}

namespace MGPhonePlaces
{
    bool isCommand(const std::string& cmd)
    {
        return cmd.rfind("place_", 0) == 0;
    }

    void idle()
    {
        while (sActive.size() < MAX_ACTIVE && !sQueue.empty())
        {
            sActive.push_back(std::move(sQueue.front()));
            sQueue.pop_front();
            sActive.back()->mStarted = LLTimer::getTotalSeconds();
        }
        const F64 now = LLTimer::getTotalSeconds();
        for (auto it = sActive.begin(); it != sActive.end();)
        {
            InfoRequest* r = it->get();
            step(r);
            if (r->mStage != InfoRequest::DONE && now - r->mStarted > REQUEST_TIMEOUT)
            {
                r->mOut["error"] = r->mHavePos ? "no parcel details" : "timeout";
                r->mStage = InfoRequest::DONE;
            }
            if (r->mStage == InfoRequest::DONE)
            {
                finish(r);
                it = sActive.erase(it);
            }
            else ++it;
        }
    }

    LLSD runCommand(const std::string& cmd, const LLSD& args)
    {
        LLSD result = LLSD().with("ok", true);
        watch();

        if (cmd == "place_list")
        {
            LLInventoryModelBackgroundFetch& bg = LLInventoryModelBackgroundFetch::instance();
            if (!bg.isEverythingFetched()) bg.start();
            const LLUUID root = gInventory.getRootFolderID();
            LLInventoryModel::cat_array_t cats;
            LLInventoryModel::item_array_t items;
            CollectLandmarks collect;
            gInventory.collectDescendentsIf(root, cats, items, LLInventoryModel::EXCLUDE_TRASH, collect);
            std::vector<LLUUID> fav_order;
            const std::set<LLUUID> favs = favoriteIds(&fav_order);
            std::map<LLUUID, S32> direct;
            LLSD rows = LLSD::emptyArray();
            for (const auto& item : items)
            {
                direct[item->getParentUUID()]++;
                LLSD row = LLSD().with("id", item->getUUID().asString()).with("n", item->getName())
                    .with("f", item->getParentUUID().asString()).with("t", (S32)item->getCreationDate());
                if (!item->getDescription().empty()) row["d"] = item->getDescription();
                if (favs.count(item->getUUID())) row["fav"] = true;
                const auto info = sInfo.find(item->getUUID().asString());
                if (info != sInfo.end()) row["info"] = withDistance(info->second);
                rows.append(row);
            }
            // the folders that hold landmarks, the Landmarks folder and all its subfolders
            const LLUUID lm_root = gInventory.findCategoryUUIDForType(LLFolderType::FT_LANDMARK);
            std::set<LLUUID> want;
            for (const auto& kv : direct)
            {
                LLUUID cur = kv.first;
                for (S32 guard = 0; guard < 40 && cur.notNull() && cur != root; ++guard)
                {
                    if (!want.insert(cur).second) break;
                    LLViewerInventoryCategory* c = gInventory.getCategory(cur);
                    if (!c) break;
                    cur = c->getParentUUID();
                }
            }
            if (lm_root.notNull())
            {
                want.insert(lm_root);
                LLInventoryModel::cat_array_t subs;
                LLInventoryModel::item_array_t none;
                LLIsType is_cat(LLAssetType::AT_CATEGORY);
                gInventory.collectDescendentsIf(lm_root, subs, none, LLInventoryModel::EXCLUDE_TRASH, is_cat);
                for (const auto& c : subs) want.insert(c->getUUID());
            }
            LLSD folders = LLSD::emptyArray();
            for (const LLUUID& id : want)
            {
                LLViewerInventoryCategory* c = gInventory.getCategory(id);
                if (!c || !userFolder(id)) continue;
                const LLFolderType::EType ft = c->getPreferredType();
                folders.append(LLSD().with("id", id.asString()).with("parent", c->getParentUUID().asString())
                    .with("name", c->getName()).with("n", direct[id])
                    .with("sys", ft != LLFolderType::FT_NONE && LLFolderType::lookupIsProtectedType(ft)));
            }
            LLSD order = LLSD::emptyArray();
            for (const LLUUID& id : fav_order) order.append(id.asString());
            result["places"] = rows;
            result["folders"] = folders;
            result["favorites"] = order;
            result["root"] = root.asString();
            result["landmarks"] = lm_root.asString();
            result["fetched"] = bg.isEverythingFetched();
            return result;
        }

        if (cmd == "place_info")
        {
            LLSD cached = LLSD::emptyArray();
            for (const auto& v : llsd::inArray(args["ids"]))
            {
                const std::string key = v.asString();
                const auto it = sInfo.find(key);
                if (it != sInfo.end() && !args["fresh"].asBoolean())
                {
                    LLSD c = withDistance(it->second);
                    c["key"] = key;
                    cached.append(c);
                }
                else enqueueItem(LLUUID(key));
            }
            for (const auto& p : llsd::inArray(args["points"]))
            {
                const std::string key = p["key"].asString();
                const auto it = sInfo.find(key);
                if (it != sInfo.end())
                {
                    LLSD c = withDistance(it->second);
                    c["key"] = key;
                    cached.append(c);
                }
                else enqueuePoint(key, LLVector3d(p["gx"].asReal(), p["gy"].asReal(), p["gz"].asReal()));
            }
            result["cached"] = cached;
            result["queued"] = (S32)(sQueue.size() + sActive.size());
            return result;
        }

        if (cmd == "place_here")
        {
            LLViewerRegion* region = gAgent.getRegion();
            if (!region) return fail("not in world");
            const LLVector3d pos = gAgent.getPositionGlobal();
            const LLVector3 local = gAgent.getPositionAgent();
            result["region"] = region->getName();
            result["rating"] = LLViewerRegion::accessToShortString(region->getSimAccess()) == "PG" ? "G" : LLViewerRegion::accessToShortString(region->getSimAccess());
            result["pos"] = vec(pos);
            result["x"] = ll_round(local.mV[VX]);
            result["y"] = ll_round(local.mV[VY]);
            result["z"] = ll_round(local.mV[VZ]);
            result["gx"] = (S32)(pos.mdV[VX] / REGION_WIDTH_METERS);
            result["gy"] = (S32)(pos.mdV[VY] / REGION_WIDTH_METERS);
            if (LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel())
            {
                result["parcel"] = parcel->getName();
                result["desc"] = parcel->getDesc();
                result["area"] = parcel->getArea();
                if (parcel->getSnapshotID().notNull()) result["snap"] = parcel->getSnapshotID().asString();
            }
            if (LLViewerInventoryItem* lm = LLLandmarkActions::findLandmarkForAgentPos()) result["landmark"] = lm->getUUID().asString();
            const LLUUID lm_root = gInventory.findCategoryUUIDForType(LLFolderType::FT_LANDMARK);
            result["landmarks"] = lm_root.asString();
            return result;
        }

        if (cmd == "place_history")
        {
            const auto& list = LLTeleportHistoryStorage::getInstance()->getItems();
            LLSD rows = LLSD::emptyArray();
            const S32 n = (S32)list.size();
            for (S32 i = n - 1; i >= 0 && rows.size() < 300; --i)
            {
                const auto& h = list[i];
                rows.append(LLSD().with("i", i).with("title", h.mTitle).with("pos", vec(h.mGlobalPos))
                    .with("t", (S32)h.mDate.secondsSinceEpoch()));
            }
            result["history"] = rows;
            return result;
        }

        if (cmd == "place_tp")
        {
            if (args.has("id"))
            {
                LLViewerInventoryItem* item = ownLandmark(LLUUID(args["id"].asString()));
                if (!item) return fail("not found");
                gAgent.teleportViaLandmark(item->getAssetUUID());
                return result;
            }
            LLVector3d pos(args["gx"].asReal(), args["gy"].asReal(), args["gz"].asReal());
            if (pos.isExactlyZero()) return fail("no position");
            gAgent.teleportViaLocation(pos);
            return result;
        }

        if (cmd == "place_map")
        {
            LLFloaterWorldMap* map = LLFloaterWorldMap::getInstance();
            if (!map) return fail("no map");
            if (args.has("id"))
            {
                LLViewerInventoryItem* item = ownLandmark(LLUUID(args["id"].asString()));
                if (!item) return fail("not found");
                map->trackLandmark(item->getUUID());
            }
            else
            {
                LLVector3d pos(args["gx"].asReal(), args["gy"].asReal(), args["gz"].asReal());
                if (pos.isExactlyZero()) return fail("no position");
                map->trackLocation(pos);
            }
            LLFloaterReg::showInstance("world_map", "center");
            return result;
        }

        if (cmd == "place_slurl")
        {
            LLVector3d pos;
            LLViewerInventoryItem* item = nullptr;
            if (!positionOf(args, pos, item)) return fail("position not known yet");
            const std::string key = item ? item->getUUID().asString() : args["key"].asString();
            const bool copy = args["copy"].asBoolean();
            LLLandmarkActions::getSLURLfromPosGlobal(pos, [key, copy](std::string& slurl)
                {
                    if (copy)
                    {
                        const LLWString w = utf8str_to_wstring(slurl);
                        LLClipboard::instance().copyToClipboard(w, 0, (S32)w.size());
                    }
                    push(LLSD().with("type", "place_slurl").with("key", key).with("slurl", slurl).with("copied", copy));
                }, false);
            result["pending"] = true;
            return result;
        }

        if (cmd == "place_create")
        {
            if (!gAgent.getRegion()) return fail("not in world");
            std::string name = args["name"].asString();
            std::string desc = args["desc"].asString();
            LLStringUtil::trim(name);
            if (name.size() > 63) name.resize(63);
            if (desc.size() > 127) desc.resize(127);
            LLUUID folder(args["folder"].asString());
            if (folder.isNull() || !userFolder(folder)) folder = gInventory.findCategoryUUIDForType(LLFolderType::FT_LANDMARK);
            if (name.empty())
            {
                LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
                name = parcel && !parcel->getName().empty() ? parcel->getName() : gAgent.getRegion()->getName();
            }
            LLLandmarkActions::createLandmarkHere(name, desc, folder);
            result["pending"] = true;
            return result;
        }

        if (cmd == "place_edit")
        {
            LLViewerInventoryItem* item = ownLandmark(LLUUID(args["id"].asString()));
            if (!item) return fail("not found");
            if (!item->getPermissions().allowModifyBy(gAgentID)) return fail("no modify");
            LLSD updates;
            if (args.has("name"))
            {
                std::string name = args["name"].asString();
                LLStringUtil::trim(name);
                if (name.empty() || name.size() > 63) return fail("bad name");
                updates["name"] = name;
            }
            if (args.has("desc"))
            {
                std::string desc = args["desc"].asString();
                if (desc.size() > 127) desc.resize(127);
                updates["desc"] = desc;
            }
            if (updates.size()) update_inventory_item(item->getUUID(), updates, nullptr);
            return result;
        }

        if (cmd == "place_move")
        {
            const LLUUID folder(args["folder"].asString());
            if (!userFolder(folder)) return fail("bad folder");
            S32 moved = 0;
            for (const auto& v : llsd::inArray(args["ids"]))
            {
                LLViewerInventoryItem* it = ownLandmark(LLUUID(v.asString()));
                if (it && it->getParentUUID() != folder)
                {
                    gInventory.changeItemParent(it, folder, false);
                    ++moved;
                }
            }
            gInventory.notifyObservers();
            result["moved"] = moved;
            return result;
        }

        if (cmd == "place_trash")
        {
            S32 n = 0;
            for (const auto& v : llsd::inArray(args["ids"]))
            {
                LLViewerInventoryItem* it = ownLandmark(LLUUID(v.asString()));
                if (it) { gInventory.removeItem(it->getUUID()); ++n; }
            }
            gInventory.notifyObservers();
            result["trashed"] = n;
            return result;
        }

        if (cmd == "place_folder_new")
        {
            LLUUID parent(args["parent"].asString());
            if (parent.isNull()) parent = gInventory.findCategoryUUIDForType(LLFolderType::FT_LANDMARK);
            std::string name = args["name"].asString();
            LLStringUtil::trim(name);
            if (!userFolder(parent) || name.empty() || name.size() > 63) return fail("bad folder");
            gInventory.createNewCategory(parent, LLFolderType::FT_NONE, name, [parent, name](const LLUUID& id)
                {
                    push(LLSD().with("type", "place_folder_new").with("id", id.asString()).with("parent", parent.asString()).with("name", name));
                });
            result["pending"] = true;
            return result;
        }

        if (cmd == "place_fav")
        {
            LLViewerInventoryItem* item = ownLandmark(LLUUID(args["id"].asString()));
            if (!item) return fail("not found");
            const LLUUID fav = gInventory.findCategoryUUIDForType(LLFolderType::FT_FAVORITE);
            if (fav.isNull()) return fail("no favorites folder");
            const bool on = args["on"].asBoolean();
            const std::set<LLUUID> favs = favoriteIds();
            if (on)
            {
                if (!favs.count(item->getUUID())) link_inventory_object(fav, item->getUUID(), nullptr);
                return result;
            }
            LLInventoryModel::cat_array_t* cats = nullptr;
            LLInventoryModel::item_array_t* items = nullptr;
            gInventory.getDirectDescendentsOf(fav, cats, items);
            if (items)
            {
                std::vector<LLUUID> drop;
                for (LLViewerInventoryItem* it : *items)
                {
                    if (it && it->getIsLinkType() && it->getLinkedUUID() == item->getUUID()) drop.push_back(it->getUUID());
                }
                for (const LLUUID& id : drop) remove_inventory_object(id, nullptr);
            }
            return result;
        }

        if (cmd == "place_home_set")
        {
            if (!gAgent.getRegion()) return fail("not in world");
            gAgent.setStartPosition(START_LOCATION_ID_HOME);
            return result;
        }

        if (cmd == "place_home_go")
        {
            gAgent.teleportHome();
            return result;
        }

        return fail("unknown command");
    }
}
