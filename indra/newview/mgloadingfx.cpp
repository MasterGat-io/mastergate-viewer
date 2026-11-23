/**
 * @file mgloadingfx.cpp
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

#include "llviewerprecompiledheaders.h"

#include "mgloadingfx.h"

#include <glm/gtc/type_ptr.hpp>

#include "llagent.h"
#include "llframetimer.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llvoavatarself.h"

namespace
{
    bool sStarted = false;
}

namespace MGLoadingFX
{
    void start()
    {
        sStarted = true;
    }

    void bindSoften(LLGLSLShader& shader)
    {
        static LLCachedControl<bool> enabled(gSavedSettings, "MGLoadingEffects", true);
        static LLStaticHashedString s_holo("mg_holo");
        static LLStaticHashedString s_center("mg_holo_center");
        static LLStaticHashedString s_inv("mg_holo_view_inv");

        // the clock wraps every hour (float precision in the shader)
        const F32 t = (F32)fmod(LLFrameTimer::getElapsedSeconds(), 3600.0);
        shader.uniform4f(s_holo, t, (enabled && sStarted) ? 1.f : 0.f, 0.f, 0.f);

        // the scan pulse starts at your avatar (or the camera before it exists)
        const LLVector3 c = isAgentAvatarValid() ? gAgentAvatarp->getPositionAgent() : LLViewerCamera::getInstance()->getOrigin();
        shader.uniform3f(s_center, c.mV[VX], c.mV[VY], c.mV[VZ]);

        // eye space -> agent space (the deferred lighting runs with the camera's view)
        const glm::mat4 inv = glm::inverse(get_current_modelview());
        shader.uniformMatrix4fv(s_inv, 1, GL_FALSE, glm::value_ptr(inv));
    }
}
