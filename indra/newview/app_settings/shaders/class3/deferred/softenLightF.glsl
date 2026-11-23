/**
 * @file class3/deferred/softenLightF.glsl
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
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
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

#define FLT_MAX 3.402823466e+38

out vec4 frag_color;

const float M_PI = 3.14159265;

#if defined(HAS_SUN_SHADOW) || defined(HAS_SSAO)
uniform sampler2D lightMap;
#endif

uniform sampler2D     lightFunc;

uniform float blur_size;
uniform float blur_fidelity;

#if defined(HAS_SSAO)
uniform float ssao_irradiance_scale;
uniform float ssao_irradiance_max;
#endif

// Inputs
uniform vec4 clipPlane;
uniform mat3 env_mat;
uniform mat3  ssao_effect_mat;
uniform vec3 sun_dir;
uniform vec3 moon_dir;
uniform int  sun_up_factor;
uniform int classic_mode;

in vec2 vary_fragcoord;

uniform mat4 inv_proj;
uniform vec2 screen_res;

vec4 getNorm(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);

void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 atten, out vec3 additive);
vec3  atmosFragLightingLinear(vec3 l, vec3 additive, vec3 atten);
vec3  scaleSoftClipFragLinear(vec3 l);

// reflection probe interface
void sampleReflectionProbes(inout vec3 ambenv, inout vec3 glossenv,
    vec2 tc, vec3 pos, vec3 norm, float glossiness, bool transparent, vec3 amblit_linear);
void sampleReflectionProbesLegacy(inout vec3 ambenv, inout vec3 glossenv, inout vec3 legacyenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, float envIntensity, bool transparent, vec3 amblit_linear);
void applyGlossEnv(inout vec3 color, vec3 glossenv, vec4 spec, vec3 pos, vec3 norm);
void applyLegacyEnv(inout vec3 color, vec3 legacyenv, vec4 spec, vec3 pos, vec3 norm, float envIntensity);
float getDepth(vec2 pos_screen);

vec3 linear_to_srgb(vec3 c);
vec3 srgb_to_linear(vec3 c);

uniform vec4 waterPlane;

uniform int cube_snapshot;

uniform float sky_hdr_scale;

void calcHalfVectors(vec3 lv, vec3 n, vec3 v, out vec3 h, out vec3 l, out float nh, out float nl, out float nv, out float vh, out float lightDist);
void calcDiffuseSpecular(vec3 baseColor, float metallic, inout vec3 diffuseColor, inout vec3 specularColor);

vec3 pbrBaseLight(vec3 diffuseColor,
                  vec3 specularColor,
                  float metallic,
                  vec3 pos,
                  vec3 norm,
                  float perceptualRoughness,
                  vec3 light_dir,
                  vec3 sunlit,
                  float scol,
                  vec3 radiance,
                  vec3 irradiance,
                  vec3 colorEmissive,
                  float ao,
                  vec3 additive,
                  vec3 atten);

GBufferInfo getGBuffer(vec2 screenpos);
vec3 clampHDRRange(vec3 color);

void adjustIrradiance(inout vec3 irradiance, float ambocc)
{
    // use sky settings ambient or irradiance map sample, whichever is brighter
    //irradiance = max(amblit_linear, irradiance);

#if defined(HAS_SSAO)
    irradiance = mix(ssao_effect_mat * min(irradiance.rgb*ssao_irradiance_scale, vec3(ssao_irradiance_max)), irradiance.rgb, ambocc);
#endif
}


// <MG> MasterGate loading hologram (mgloadingfx.cpp): a surface whose texture has not arrived
// is a dark glassy shell wrapped in a glowing honeycomb. Colour bands of MasterGate cyan and
// violet climb it, cells light up as if being assembled, and a scan pulse sweeps out from
// your avatar every few seconds.
uniform vec4 mg_holo;           // x: time (s), y: 1 = on
uniform vec3 mg_holo_center;    // the pulse's centre (agent space)
uniform mat4 mg_holo_view_inv;  // eye space -> agent space

float mgHash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

// x: distance to the nearest hexagon edge (0 on it, 0.5 at the centre); yz: the cell
vec3 mgHex(vec2 uv)
{
    const vec2 r = vec2(1.0, 1.7320508);
    vec2 a = mod(uv, r) - r * 0.5;
    vec2 b = mod(uv - r * 0.5, r) - r * 0.5;
    vec2 g = dot(a, a) < dot(b, b) ? a : b;
    vec2 q = abs(g);
    float d = max(dot(q, vec2(0.5, 0.8660254)), q.x);
    return vec3(0.5 - d, uv - g);
}

vec3 mgHologram(vec3 posEye, vec3 nEye, vec2 tc, float fwEye)
{
    float t = mg_holo.x;
    vec3 p = (mg_holo_view_inv * vec4(posEye, 1.0)).xyz;
    vec3 n = normalize(mat3(mg_holo_view_inv) * nEye);
    vec3 an = abs(n);

    // the honeycomb, projected on the face's main plane: a bold one and a fine one inside it
    vec2 uv;
    float plane;
    if (an.z >= an.x && an.z >= an.y) { uv = p.xy; plane = 0.0; }
    else if (an.x >= an.y)            { uv = p.yz; plane = 1.0; }
    else                              { uv = p.xz; plane = 2.0; }
    vec3 big = mgHex(uv / 0.45);
    vec3 fine = mgHex(uv / 0.11);

    // neon lines about a pixel wide with a soft glow; where cells shrink to a few pixels they fade
    float fb = max(fwEye / 0.45, 1e-4);
    float ff = max(fwEye / 0.11, 1e-4);
    float lineB = (1.0 - smoothstep(fb * 0.4, fb * 1.3, big.x)) * (1.0 - smoothstep(0.1, 0.35, fb));
    float glowB = exp(-big.x / max(0.05, fb * 2.0)) * 0.35 * (1.0 - smoothstep(0.15, 0.5, fb));
    float lineF = (1.0 - smoothstep(ff * 0.4, ff * 1.2, fine.x)) * (1.0 - smoothstep(0.08, 0.3, ff));

    float h = mgHash(big.yz + plane * 17.0);
    // energy running up the lines in bright dashes
    float dash = smoothstep(0.75, 1.0, fract(p.z * 0.9 - t * 1.1 + h * 3.0));
    // cells being assembled: each flares for an instant at its own rhythm
    float flare = pow(max(0.0, sin(t * (0.5 + h * 1.5) + h * 40.0)), 28.0);
    // the scan pulse, sweeping out from you every 3 s
    float radius = mod(t, 3.0) * 9.0;
    float ring = exp(-pow((distance(p, mg_holo_center) - radius) / 0.5, 2.0)) * (1.0 - smoothstep(16.0, 27.0, radius));
    // colour bands of MasterGate cyan and violet climbing the surfaces
    float band = 0.5 + 0.5 * sin(p.z * 1.2 - t * 1.6 + h * 0.4);
    vec3 cyan = vec3(0.0, 0.6, 0.8);
    vec3 violet = vec3(0.34, 0.1, 0.8);
    vec3 tint = mix(cyan, violet, band);
    vec3 hot = mix(tint, vec3(1.0), 0.6);

    vec3 v = normalize(-posEye);
    float ndv = clamp(abs(dot(nEye, v)), 0.0, 1.0);
    float rim = pow(1.0 - ndv, 3.0);

    vec3 col = vec3(0.003, 0.006, 0.016) + tint * (0.015 + 0.03 * (1.0 - ndv));   // dark glass
    col += tint * lineF * 0.22;
    col += tint * (lineB * (0.9 + 1.4 * dash) + glowB);
    col += hot * lineB * dash * 0.6;
    col += tint * flare * (0.12 + 0.6 * big.x);
    col += hot * ring * (0.12 + 2.0 * lineB + 0.8 * lineF);
    col += mix(cyan, vec3(1.0), 0.3) * rim * 0.9;
    // a faint interference line drifting down the screen
    col *= 1.0 + 0.3 * smoothstep(0.985, 1.0, fract(tc.y * 3.0 + t * 0.35));

    // far away: a soft glow instead of lines
    float far = smoothstep(60.0, 160.0, length(posEye));
    return mix(col, tint * 0.05 + cyan * rim * 0.5, far);
}
// </MG>

void main()
{
    vec2  tc           = vary_fragcoord.xy;
    float depth        = getDepth(tc.xy);
    vec4  pos          = getPositionWithDepth(tc, depth);
    float mg_fw        = length(fwidth(pos.xyz)); // <MG/> (taken where every pixel runs it)

    GBufferInfo gb = getGBuffer(tc);

    vec3 colorEmissive = gb.emissive.rgb;
    float envIntensity = gb.envIntensity;
    // <MG> a surface still loading (see MG_HOLO_GBUFFER)
    bool mg_h = GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_SKIP_ATMOS) && envIntensity > 0.75;
    if (mg_h) envIntensity = 0.0;
    // </MG>
    vec3  light_dir   = (sun_up_factor == 1) ? sun_dir : moon_dir;

    vec4 baseColor     = gb.albedo;
    vec4 spec        = gb.specular; // NOTE: PBR linear Emissive

#if defined(HAS_SUN_SHADOW) || defined(HAS_SSAO)
    vec2 scol_ambocc = texture(lightMap, vary_fragcoord.xy).rg;
#endif

#if defined(HAS_SUN_SHADOW)
    float scol       = max(scol_ambocc.r, baseColor.a);
#else
    float scol = 1.0;
#endif
#if defined(HAS_SSAO)
    float ambocc     = scol_ambocc.g;
#else
    float ambocc = 1.0;
#endif

    vec3  color = vec3(0);
    float bloom = 0.0;

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;

    calcAtmosphericVarsLinear(pos.xyz, gb.normal, light_dir, sunlit, amblit, additive, atten);

    if (classic_mode > 0)
        sunlit *= 1.35;

    vec3 sunlit_linear = sunlit;
    vec3 amblit_linear = amblit;

    vec3  radiance  = vec3(0);

    if (mg_h && mg_holo.y > 0.5)
    {
        color = mgHologram(pos.xyz, gb.normal, tc, mg_fw); // <MG/>
    }
    else if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_PBR))
    {
        vec3 orm = spec.rgb;
        float perceptualRoughness = orm.g;
        float metallic = orm.b;
        float ao = orm.r;

        vec3  irradiance = amblit_linear;

        // PBR IBL
        float gloss      = 1.0 - perceptualRoughness;

        sampleReflectionProbes(irradiance, radiance, tc, pos.xyz, gb.normal, gloss, false, amblit_linear);

        adjustIrradiance(irradiance, ambocc);

        vec3 diffuseColor;
        vec3 specularColor;
        calcDiffuseSpecular(baseColor.rgb, metallic, diffuseColor, specularColor);

        vec3 v = -normalize(pos.xyz);
        color = pbrBaseLight(diffuseColor, specularColor, metallic, v, gb.normal, perceptualRoughness, light_dir, sunlit_linear, scol, radiance, irradiance, colorEmissive, ao, additive, atten);
    }
    else if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_HDRI))
    {
        // actual HDRI sky, just copy color value
        color = colorEmissive.rgb;
    }
    else if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_SKIP_ATMOS) && !mg_h) // <MG/> (effect off: lit as usual)
    {
        //should only be true of WL sky, port over base color value and scale for fake HDR
#if defined(HAS_EMISSIVE)
        color = colorEmissive.rgb;
#else
        color = baseColor.rgb;
#endif
        color = srgb_to_linear(color);
        color *= sky_hdr_scale;
    }
    else
    {
        // legacy shaders are still writng sRGB to gbuffer
        baseColor.rgb = srgb_to_linear(baseColor.rgb);

        spec.rgb = srgb_to_linear(spec.rgb);

        float da          = clamp(dot(gb.normal, light_dir.xyz), 0.0, 1.0);

        vec3 irradiance = amblit;
        vec3 glossenv = vec3(0);
        vec3 legacyenv = vec3(0);

        sampleReflectionProbesLegacy(irradiance, glossenv, legacyenv, tc, pos.xyz, gb.normal, spec.a, envIntensity, false, amblit_linear);

        adjustIrradiance(irradiance, ambocc);

        // apply lambertian IBL only (see pbrIbl)
        color.rgb = irradiance;

        if (classic_mode > 0)
        {
            da = pow(da,1.2);
            vec3 sun_contrib = vec3(min(da, scol));

            color.rgb = srgb_to_linear(color.rgb * 0.9 + (linear_to_srgb(sun_contrib) * sunlit_linear * 0.7));
            sunlit_linear = srgb_to_linear(sunlit_linear);
        }
        else
        {
            vec3 sun_contrib = min(da, scol) * sunlit_linear;
            color.rgb += sun_contrib;
        }

        color.rgb *= baseColor.rgb;

        vec3 refnormpersp = reflect(pos.xyz, gb.normal);

        if (spec.a > 0.0)
        {
            vec3  lv = light_dir.xyz;
            vec3  h, l, v = -normalize(pos.xyz);
            float nh, nl, nv, vh, lightDist;
            vec3 n = gb.normal;
            calcHalfVectors(lv, n, v, h, l, nh, nl, nv, vh, lightDist);

            if (nl > 0.0 && nh > 0.0)
            {
                float lit = min(nl*6.0, 1.0);

                float sa = nh;
                float fres = pow(1 - vh, 5) * 0.4+0.5;
                float gtdenom = 2 * nh;
                float gt = max(0,(min(gtdenom * nv / vh, gtdenom * nl / vh)));

                scol *= fres*texture(lightFunc, vec2(nh, spec.a)).r*gt/(nh*nl);
                color.rgb += lit*scol*sunlit_linear.rgb*spec.rgb;
            }

            // add radiance map
            applyGlossEnv(color, glossenv, spec, pos.xyz, gb.normal);

        }

        color.rgb = mix(color.rgb, baseColor.rgb, baseColor.a);

        if (envIntensity > 0.0)
        {  // add environment map
            applyLegacyEnv(color, legacyenv, spec, pos.xyz, gb.normal, envIntensity);
        }
   }

    //color.r = classic_mode > 0 ? 1.0 : 0.0;
    float final_scale = 1;
    if (classic_mode > 0)
        final_scale = 1.1;

    frag_color.rgb = clampHDRRange(color.rgb * final_scale); //output linear since local lights will be added to this shader's results
    frag_color.a = 0.0;
}
