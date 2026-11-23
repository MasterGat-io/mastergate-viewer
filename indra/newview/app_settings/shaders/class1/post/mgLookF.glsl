/**
 * @file mgLookF.glsl
 * @brief MasterGate Viewer: the MG Phone Camera's live looks (mgphonecamera.cpp, cam_look).
 *
 * Up to four stacked looks graded on the finished picture (after tone mapping), then the
 * effects (chromatic aberration, soft focus, halation, vignette, grain, cinema bars) and a
 * before / after line. Snapshots go through the same pass: the photo is what the
 * viewfinder shows. The page's thumbnails run the same math (apps/camera/client.js
 * lookGrade): keep the two in step.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * MasterGate Viewer, based on the Firestorm Viewer and the Second Life Viewer.
 * This library is free software; you can redistribute it and/or modify it under the terms
 * of the GNU Lesser General Public License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D diffuseRect;
uniform vec2 screen_res;
in vec2 vary_fragcoord;

uniform int  mg_layers;
uniform vec4 mg_a[4];   // saturation, contrast, exposure (stops), strength
uniform vec4 mg_b[4];   // gain r g b, hue rotation (radians)
uniform vec4 mg_c[4];   // shadow tint r g b, lift (fade)
uniform vec4 mg_d[4];   // highlight tint r g b, monochrome
uniform vec4 mg_e[4];   // sepia, posterize levels (0 off), invert, duotone (c = dark, d = light)
uniform vec4 mg_fx;     // vignette, grain, halation, chromatic aberration
uniform vec4 mg_fx2;    // soft focus, cinema bars aspect (0 off), before/after line (< 0 off), time
uniform vec4 mg_frame;  // the photo's rectangle in this target's uv (x0, y0, w, h)

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 hueRotate(vec3 c, float a)
{
    const vec3 k = vec3(0.57735);
    float ca = cos(a);
    return c * ca + cross(k, c) * sin(a) + k * dot(k, c) * (1.0 - ca);
}

float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }

vec3 grade(vec3 c, vec4 a, vec4 b, vec4 tc, vec4 d, vec4 e)
{
    vec3 x = c * exp2(a.z);
    if (e.z > 0.5) x = 1.0 - x;
    float L = luma(x);
    x = mix(vec3(L), x, a.x);
    x = (x - 0.5) * a.y + 0.5;
    x *= b.rgb;
    if (abs(b.w) > 0.001) x = hueRotate(x, b.w);
    float L2 = clamp(luma(x), 0.0, 1.0);
    if (e.w > 0.5)
    {
        x = mix(tc.rgb, d.rgb, L2);
    }
    else
    {
        x += tc.rgb * (1.0 - L2) * (1.0 - L2) + d.rgb * L2 * L2;
    }
    x = mix(x, vec3(L2), d.w);
    if (e.x > 0.0)
    {
        vec3 s = vec3(dot(x, vec3(0.393, 0.769, 0.189)), dot(x, vec3(0.349, 0.686, 0.168)), dot(x, vec3(0.272, 0.534, 0.131)));
        x = mix(x, s, e.x);
    }
    x = x * (1.0 - tc.w) + tc.w;
    if (e.y > 1.5) x = floor(x * e.y + 0.5) / e.y;
    return mix(c, clamp(x, 0.0, 1.0), a.w);
}

void main()
{
    vec2 uv = vary_fragcoord.xy;
    vec2 p = (uv - mg_frame.xy) / mg_frame.zw;   // 0..1 across the photo
    vec2 px = 1.0 / screen_res;
    vec4 src = texture(diffuseRect, uv);
    vec3 c = src.rgb;

    if (mg_fx.w > 0.0)
    {
        vec2 dir = (p - 0.5) * mg_fx.w * 0.012 * mg_frame.zw;
        c = vec3(texture(diffuseRect, uv + dir).r, c.g, texture(diffuseRect, uv - dir).b);
    }
    vec3 orig = c;

    if (mg_fx2.x > 0.0)
    {
        vec3 bl = vec3(0.0);
        for (int k = 0; k < 8; k++)
        {
            float an = float(k) * 0.785398;
            bl += texture(diffuseRect, uv + vec2(cos(an), sin(an)) * px * 5.0).rgb;
        }
        bl /= 8.0;
        c = mix(c, max(c, bl), mg_fx2.x * 0.75);
    }

    if (mg_fx.z > 0.0)
    {
        vec3 h = vec3(0.0);
        for (int k = 0; k < 12; k++)
        {
            float an = float(k) * 0.5236;
            for (int r = 1; r <= 2; r++)
            {
                vec3 s = texture(diffuseRect, uv + vec2(cos(an), sin(an)) * px * float(r) * 7.0).rgb;
                h += max(vec3(0.0), s - 0.7);
            }
        }
        h /= 24.0;
        float lh = luma(h);
        c += mg_fx.z * vec3(lh * 1.9, lh * 0.55, lh * 0.25) * 2.2;
    }

    for (int i = 0; i < 4; i++)
    {
        if (i >= mg_layers) break;
        c = grade(c, mg_a[i], mg_b[i], mg_c[i], mg_d[i], mg_e[i]);
    }

    if (mg_fx.x > 0.0)
    {
        float v = smoothstep(0.35, 0.85, length(p - 0.5) * 1.41421);
        c *= 1.0 - mg_fx.x * 0.75 * v;
    }
    if (mg_fx.y > 0.0)
    {
        c += (hash(floor(gl_FragCoord.xy) + fract(mg_fx2.w) * 97.0) - 0.5) * mg_fx.y * 0.14;
    }
    if (mg_fx2.y > 0.0)
    {
        float ar = (mg_frame.z * screen_res.x) / max(1.0, mg_frame.w * screen_res.y);
        float f = ar < mg_fx2.y ? 0.5 * (1.0 - ar / mg_fx2.y) : 0.0;
        if (p.y < f || p.y > 1.0 - f) c = vec3(0.0);
    }
    if (mg_fx2.z >= 0.0)
    {
        if (p.x < mg_fx2.z) c = orig;
        else if ((p.x - mg_fx2.z) * mg_frame.z * screen_res.x < 2.0) c = vec3(1.0);
    }
    frag_color = vec4(clamp(c, 0.0, 1.0), src.a);
}
