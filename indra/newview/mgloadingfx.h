/**
 * @file mgloadingfx.h
 * @brief MasterGate Viewer: how the world appears while it loads.
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

#ifndef MG_LOADINGFX_H
#define MG_LOADINGFX_H

class LLGLSLShader;

// The MasterGate loading hologram. A surface whose texture has not arrived shows the
// placeholder texture, whose colour is a key (llviewertexture.cpp, MG_HOLO_KEY in
// llshadermgr.cpp). The G-buffer shaders recognise the key and mark the pixel; the deferred
// lighting (class3/deferred/softenLightF.glsl, mgHologram) draws it as a dark glassy shell in
// a glowing honeycomb: colour bands of cyan and violet climbing it, cells flaring as if being
// assembled, a scan pulse sweeping out from your avatar. The texture arriving ends it.
// Other avatars still loading are shown as they arrive (LLVOAvatar::isFullyLoaded): their
// unloaded body and attachments are the hologram, instead of a particle cloud.
// Setting MGLoadingEffects (on by default) switches all of it.
namespace MGLoadingFX
{
    // once logged in (llstartup.cpp)
    void start();
    // the deferred lighting shader is bound (pipeline.cpp): its clock, camera and pulse centre
    void bindSoften(LLGLSLShader& shader);
}

#endif // MG_LOADINGFX_H
