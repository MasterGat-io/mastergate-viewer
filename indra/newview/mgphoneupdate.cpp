/**
 * @file mgphoneupdate.cpp
 * @brief MasterGate Viewer: install a new release from the MG Phone, over this one.
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

#include "mgphoneupdate.h"

#if LL_WINDOWS

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <openssl/evp.h>

#include "llwin32headers.h"
#include <shellapi.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")

#include "llappviewer.h"
#include "llcallbacklist.h"
#include "lldir.h"
#include "llfile.h"
#include "llfloaterreg.h"
#include "llversioninfo.h"
#include "mgfloaterphone.h"

namespace
{
    // a download running on its own thread; the viewer side only reads it
    struct Download
    {
        std::string url, path, sha256, version;
        std::atomic<S64> done { 0 };
        std::atomic<S64> total { 0 };
        std::atomic<bool> cancel { false };
        std::atomic<int> state { 0 };     // 0 running, 1 ready, 2 failed
        std::mutex lock;
        std::string error;                // under lock
    };

    std::shared_ptr<Download> sDownload;
    std::string sInstalling;              // version being installed
    bool sIdle = false;
    F64 sLastPush = 0.0;

    const char* stateName(const std::shared_ptr<Download>& d)
    {
        if (!sInstalling.empty()) return "installing";
        if (!d) return "idle";
        switch (d->state.load())
        {
        case 0: return "downloading";
        case 1: return "ready";
        default: return "failed";
        }
    }

    LLSD stateEvent()
    {
        LLSD ev;
        ev["type"] = "update";
        ev["state"] = stateName(sDownload);
        if (sDownload)
        {
            ev["done"] = (LLSD::Real)sDownload->done.load();
            ev["total"] = (LLSD::Real)sDownload->total.load();
            ev["version"] = sDownload->version;
            std::lock_guard<std::mutex> g(sDownload->lock);
            if (!sDownload->error.empty()) ev["error"] = sDownload->error;
        }
        if (!sInstalling.empty()) ev["version"] = sInstalling;
        ev["current"] = LLVersionInfo::getInstance()->getVersion();
        return ev;
    }

    void push()
    {
        MGFloaterPhone* p = LLFloaterReg::findTypedInstance<MGFloaterPhone>("mg_phone");
        if (p && p->getVisible() && p->isShellReady()) p->pushToPage(stateEvent());
    }

    void onIdle(void*)
    {
        if (!sDownload)
        {
            gIdleCallbacks.deleteFunction(&onIdle, nullptr);
            sIdle = false;
            return;
        }
        const bool finished = sDownload->state.load() != 0;
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (finished || now - sLastPush > 0.5)
        {
            sLastPush = now;
            push();
        }
        if (finished)
        {
            gIdleCallbacks.deleteFunction(&onIdle, nullptr);
            sIdle = false;
        }
    }

    std::string toHex(const unsigned char* b, unsigned int n)
    {
        static const char* digits = "0123456789abcdef";
        std::string s;
        for (unsigned int i = 0; i < n; ++i) { s += digits[b[i] >> 4]; s += digits[b[i] & 15]; }
        return s;
    }

    void fail(const std::shared_ptr<Download>& d, const std::string& why)
    {
        {
            std::lock_guard<std::mutex> g(d->lock);
            d->error = why;
        }
        LLFile::remove(d->path);
        d->state = 2;
    }

    // the worker: WinINet (follows download.php's redirect to viewer/files/), streamed to
    // disk while the SHA-256 is computed, then compared with the one releases.php gave
    void run(std::shared_ptr<Download> d)
    {
        const std::wstring agent = ll_convert_string_to_wide("MasterGateViewer/" + LLVersionInfo::getInstance()->getVersion(), CP_UTF8);
        HINTERNET net = InternetOpenW(agent.c_str(), INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
        if (!net) { fail(d, "no internet"); return; }
        const std::wstring url = ll_convert_string_to_wide(d->url, CP_UTF8);
        HINTERNET req = InternetOpenUrlW(net, url.c_str(), nullptr, 0,
                                         INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
        if (!req) { InternetCloseHandle(net); fail(d, "could not reach mastergate.io"); return; }

        DWORD status = 0, len = sizeof(status);
        HttpQueryInfoW(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, nullptr);
        DWORD size = 0;
        len = sizeof(size);
        if (HttpQueryInfoW(req, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &size, &len, nullptr) && size > 0) d->total = (S64)size;
        if (status != 200)
        {
            InternetCloseHandle(req);
            InternetCloseHandle(net);
            fail(d, "the server answered " + std::to_string(status));
            return;
        }

        LLFILE* out = LLFile::fopen(d->path, "wb");
        EVP_MD_CTX* md = EVP_MD_CTX_new();
        bool ok = out && md && EVP_DigestInit_ex(md, EVP_sha256(), nullptr) == 1;
        std::string why = ok ? "" : "could not write the file";
        std::vector<char> buf(256 * 1024);
        while (ok)
        {
            if (d->cancel) { ok = false; why = "cancelled"; break; }
            DWORD got = 0;
            if (!InternetReadFile(req, buf.data(), (DWORD)buf.size(), &got)) { ok = false; why = "the download was interrupted"; break; }
            if (got == 0) break;
            if (fwrite(buf.data(), 1, got, out) != got) { ok = false; why = "the disk is full"; break; }
            EVP_DigestUpdate(md, buf.data(), got);
            d->done += got;
        }
        if (out) fclose(out);
        InternetCloseHandle(req);
        InternetCloseHandle(net);

        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int n = 0;
        if (ok && EVP_DigestFinal_ex(md, digest, &n) != 1) { ok = false; why = "could not check the file"; }
        if (md) EVP_MD_CTX_free(md);
        if (ok && toHex(digest, n) != d->sha256) { ok = false; why = "the file did not match its checksum"; }
        if (!ok) { fail(d, why); return; }
        d->state = 1;
    }

    bool allowedUrl(const std::string& url)
    {
        // our own server only (download.php counts it, then redirects to viewer/files/)
        return url.rfind("https://mastergate.io/viewer/", 0) == 0 && url.find_first_of(" \"\r\n") == std::string::npos;
    }
}

bool MGPhoneUpdate::isCommand(const std::string& cmd)
{
    return cmd.rfind("update_", 0) == 0;
}

LLSD MGPhoneUpdate::runCommand(const std::string& cmd, const LLSD& args)
{
    LLSD result = LLSD().with("ok", true);

    if (cmd == "update_state")
    {
        result["update"] = stateEvent();
        return result;
    }

    if (cmd == "update_start")
    {
        if (sDownload && sDownload->state.load() == 0) { result["update"] = stateEvent(); return result; }
        const std::string url = args["url"].asString();
        std::string sha = args["sha256"].asString();
        LLStringUtil::toLower(sha);
        if (!allowedUrl(url)) return LLSD().with("ok", false).with("error", "bad url");
        if (sha.size() != 64 || sha.find_first_not_of("0123456789abcdef") != std::string::npos) return LLSD().with("ok", false).with("error", "no checksum");

        const std::string dir = gDirUtilp->getTempDir() + gDirUtilp->getDirDelimiter() + "MasterGateViewerUpdate";
        LLFile::mkdir(dir);
        std::string version = args["version"].asString();
        if (version.find_first_not_of("0123456789.") != std::string::npos) version.clear();
        auto d = std::make_shared<Download>();
        d->url = url;
        d->sha256 = sha;
        d->version = version;
        d->path = dir + gDirUtilp->getDirDelimiter() + "MasterGateViewer-" + (version.empty() ? std::string("update") : version) + "_Setup.exe";
        d->total = (S64)args["size"].asReal();
        sDownload = d;
        sInstalling.clear();
        std::thread(run, d).detach();
        if (!sIdle) { gIdleCallbacks.addFunction(&onIdle, nullptr); sIdle = true; }
        result["update"] = stateEvent();
        return result;
    }

    if (cmd == "update_cancel")
    {
        if (sDownload)
        {
            sDownload->cancel = true;
            if (sDownload->state.load() == 1) LLFile::remove(sDownload->path);
        }
        sDownload.reset();
        result["update"] = stateEvent();
        return result;
    }

    if (cmd == "update_install")
    {
        if (!sDownload || sDownload->state.load() != 1 || !LLFile::isfile(sDownload->path))
            return LLSD().with("ok", false).with("error", "not downloaded");
        // the Setup asks Windows for administrator rights (UAC): declining it is not an error
        const std::wstring exe = ll_convert_string_to_wide(sDownload->path, CP_UTF8);
        // /MGPID: the installer waits for this process only (its usual check closes any
        // window of class "Second Life", which other viewers use too)
        const std::wstring params = L"/MGUPDATE /MGPID=" + std::to_wstring(GetCurrentProcessId());
        HINSTANCE h = ShellExecuteW(nullptr, L"open", exe.c_str(), params.c_str(), nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)h <= 32) return LLSD().with("ok", false).with("error", "the installer did not start");
        sInstalling = sDownload->version;
        push();
        // the installer waits for this window to close, installs into the same folder and
        // opens the new version; settings, chat logs and caches are kept
        LLAppViewer::instance()->requestQuit();
        result["update"] = stateEvent();
        return result;
    }

    return LLSD().with("ok", false).with("error", "unknown command");
}

#else // not Windows: the shell shows the download link

bool MGPhoneUpdate::isCommand(const std::string& cmd)
{
    return cmd.rfind("update_", 0) == 0;
}

LLSD MGPhoneUpdate::runCommand(const std::string& cmd, const LLSD& args)
{
    return LLSD().with("ok", false).with("error", "unsupported");
}

#endif
