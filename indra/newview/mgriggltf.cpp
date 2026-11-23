/**
 * @file mgriggltf.cpp
 * @brief MasterGate Viewer: Creator Lab's rig converter, glTF / GLB / VRM reader (tinygltf).
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

#ifndef MG_CREATORDAE_STANDALONE
#include "llviewerprecompiledheaders.h"
#endif

#include "mgrig.h"

// the implementation is compiled once, in gltf/llgltfloader.cpp (same options)
#ifndef TINYGLTF_USE_CPP14
#define TINYGLTF_USE_CPP14
#endif
#include "tinygltf/tiny_gltf.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>

namespace
{
    using MGRig::Mat;

    Mat mul(const Mat& a, const Mat& b)
    {
        Mat r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
        return r;
    }
    const Mat IDENT = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

    bool invert(const Mat& m, Mat& out)
    {
        // Gauss-Jordan on a 4x8
        double a[4][8];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
        for (int c = 0; c < 4; ++c)
        {
            int p = c;
            for (int i = c + 1; i < 4; ++i) if (std::fabs(a[i][c]) > std::fabs(a[p][c])) p = i;
            if (std::fabs(a[p][c]) < 1e-12) return false;
            if (p != c) for (int j = 0; j < 8; ++j) std::swap(a[p][j], a[c][j]);
            const double d = a[c][c];
            for (int j = 0; j < 8; ++j) a[c][j] /= d;
            for (int i = 0; i < 4; ++i)
            {
                if (i == c) continue;
                const double f = a[i][c];
                for (int j = 0; j < 8; ++j) a[i][j] -= f * a[c][j];
            }
        }
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out[i * 4 + j] = a[i][j + 4];
        return true;
    }

    // a glTF node's own transform (column-major matrix, or T * R * S)
    Mat localOf(const tinygltf::Node& n)
    {
        if (n.matrix.size() == 16)
        {
            Mat m{};
            for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) m[r * 4 + c] = n.matrix[c * 4 + r];
            return m;
        }
        double t[3] = { 0, 0, 0 }, s[3] = { 1, 1, 1 }, q[4] = { 0, 0, 0, 1 };
        if (n.translation.size() == 3) for (int i = 0; i < 3; ++i) t[i] = n.translation[i];
        if (n.scale.size() == 3) for (int i = 0; i < 3; ++i) s[i] = n.scale[i];
        if (n.rotation.size() == 4) for (int i = 0; i < 4; ++i) q[i] = n.rotation[i];
        const double x = q[0], y = q[1], z = q[2], w = q[3];
        const double R[9] = { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                              2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                              2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) };
        return Mat{ R[0] * s[0], R[1] * s[1], R[2] * s[2], t[0],
                    R[3] * s[0], R[4] * s[1], R[5] * s[2], t[1],
                    R[6] * s[0], R[7] * s[1], R[8] * s[2], t[2],
                    0, 0, 0, 1 };
    }

    // every element of an accessor as doubles (normalized integers become 0..1)
    std::vector<double> readAccessor(const tinygltf::Model& model, int index, int& comps)
    {
        std::vector<double> out;
        comps = 0;
        if (index < 0 || index >= (int)model.accessors.size()) return out;
        const tinygltf::Accessor& a = model.accessors[index];
        comps = tinygltf::GetNumComponentsInType(a.type);
        if (a.bufferView < 0 || comps <= 0) return out;
        const tinygltf::BufferView& bv = model.bufferViews[a.bufferView];
        const tinygltf::Buffer& buf = model.buffers[bv.buffer];
        const int csize = tinygltf::GetComponentSizeInBytes(a.componentType);
        if (csize <= 0) return out;
        const size_t stride = bv.byteStride ? bv.byteStride : (size_t)csize * comps;
        const size_t base = bv.byteOffset + a.byteOffset;
        if (base + stride * (a.count ? a.count - 1 : 0) + (size_t)csize * comps > buf.data.size()) return out;
        out.resize(a.count * comps);
        for (size_t i = 0; i < a.count; ++i)
        {
            const unsigned char* p = buf.data.data() + base + stride * i;
            for (int c = 0; c < comps; ++c)
            {
                const unsigned char* e = p + c * csize;
                double v = 0.0;
                switch (a.componentType)
                {
                case TINYGLTF_COMPONENT_TYPE_FLOAT: { float f; std::memcpy(&f, e, 4); v = f; break; }
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: v = *e; if (a.normalized) v /= 255.0; break;
                case TINYGLTF_COMPONENT_TYPE_BYTE: v = (signed char)*e; if (a.normalized) v = std::max(v / 127.0, -1.0); break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: { unsigned short s; std::memcpy(&s, e, 2); v = s; if (a.normalized) v /= 65535.0; break; }
                case TINYGLTF_COMPONENT_TYPE_SHORT: { short s; std::memcpy(&s, e, 2); v = s; if (a.normalized) v = std::max(v / 32767.0, -1.0); break; }
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: { unsigned int u; std::memcpy(&u, e, 4); v = u; break; }
                default: break;
                }
                out[i * comps + c] = v;
            }
        }
        return out;
    }

    bool skipImages(tinygltf::Image*, const int, std::string*, std::string*, int, int, const unsigned char*, int, void*) { return true; }
}

namespace MGRig
{
    bool readGltf(const std::string& path, Model& out, bool keepStatic)
    {
        out = Model();
        out.format = "gltf";
        tinygltf::TinyGLTF loader;
        loader.SetImageLoader(skipImages, nullptr);   // textures are not needed for the rig
        tinygltf::Model model;
        std::string err, warn;
        std::string ext;
        const size_t dot = path.find_last_of('.');
        if (dot != std::string::npos) ext = path.substr(dot + 1);
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        const bool ok = ext == "gltf" ? loader.LoadASCIIFromFile(&model, &err, &warn, path) : loader.LoadBinaryFromFile(&model, &err, &warn, path);
        if (!ok) { out.error = "read: " + err; return false; }

        // glTF: Y up, the front looks at +Z, the left side is +X. SL: X front, Y left, Z up.
        // VRM 0.x (VRoid) looks at -Z: turned half a turn.
        bool vrm0 = false;
        for (const auto& e : model.extensionsUsed) if (e == "VRM") vrm0 = true;
        const double f = vrm0 ? -1.0 : 1.0;
        const Mat P = { 0, 0, f, 0,  f, 0, 0, 0,  0, 1, 0, 0,  0, 0, 0, 1 };   // SL = P * glTF
        const Mat Pt = { 0, f, 0, 0,  0, 0, 1, 0,  f, 0, 0, 0,  0, 0, 0, 1 };
        auto toSl = [&](const Mat& m) { return mul(mul(P, m), Pt); };

        // world transforms of every node (default scene, else all roots)
        std::vector<Mat> world(model.nodes.size(), IDENT);
        std::vector<int> parent(model.nodes.size(), -1);
        for (size_t i = 0; i < model.nodes.size(); ++i)
            for (int c : model.nodes[i].children) if (c >= 0 && c < (int)parent.size()) parent[c] = (int)i;
        std::function<void(int, const Mat&)> walk = [&](int i, const Mat& up)
        {
            world[i] = mul(up, localOf(model.nodes[i]));
            for (int c : model.nodes[i].children) if (c >= 0 && c < (int)world.size()) walk(c, world[i]);
        };
        for (size_t i = 0; i < model.nodes.size(); ++i) if (parent[i] < 0) walk((int)i, IDENT);

        // bind places. A skin's vertices are in its own space; the skeleton's root nodes
        // (often a turn: Z-up rigs exported as glTF) place it in the world: that placement is a
        // joint's world transform times its inverse bind, applied to the vertices and binds alike
        std::map<int, Mat> bind;
        std::vector<Mat> skinToWorld(model.skins.size(), IDENT);
        for (size_t si = 0; si < model.skins.size(); ++si)
        {
            const auto& sk = model.skins[si];
            int comps = 0;
            const std::vector<double> ibm = readAccessor(model, sk.inverseBindMatrices, comps);
            std::vector<Mat> inv(sk.joints.size(), IDENT);
            bool haveIbm = comps == 16 && ibm.size() >= sk.joints.size() * 16;
            if (haveIbm)
                for (size_t k = 0; k < sk.joints.size(); ++k)
                {
                    Mat m{};
                    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) m[r * 4 + c] = ibm[k * 16 + c * 4 + r];
                    inv[k] = m;
                }
            if (haveIbm && !sk.joints.empty() && sk.joints[0] >= 0 && sk.joints[0] < (int)world.size())
                skinToWorld[si] = mul(world[sk.joints[0]], inv[0]);
            for (size_t k = 0; k < sk.joints.size(); ++k)
            {
                const int node = sk.joints[k];
                if (node < 0 || node >= (int)world.size() || bind.count(node)) continue;
                Mat b;
                if (haveIbm && invert(inv[k], b)) bind[node] = mul(skinToWorld[si], b);
                else bind[node] = world[node];
            }
        }
        std::map<int, int> index;
        std::function<int(int)> addJoint = [&](int node) -> int
        {
            if (node < 0) return -1;
            auto it = index.find(node);
            if (it != index.end()) return it->second;
            // the chain above a skeleton: only nodes that are joints or hold joints
            const int p = parent[node] >= 0 && (bind.count(parent[node]) || model.nodes[parent[node]].mesh < 0) ? addJoint(parent[node]) : -1;
            Joint j;
            j.name = model.nodes[node].name.empty() ? "node" + std::to_string(node) : model.nodes[node].name;
            j.parent = p;
            j.bind = toSl(bind.count(node) ? bind[node] : world[node]);
            const int id = (int)out.joints.size();
            out.joints.push_back(j);
            index[node] = id;
            return id;
        };
        for (const auto& kv : bind) addJoint(kv.first);

        size_t staticLeft = 0;
        for (size_t ni = 0; ni < model.nodes.size(); ++ni)
        {
            const tinygltf::Node& node = model.nodes[ni];
            if (node.mesh < 0) continue;
            const tinygltf::Mesh& gm = model.meshes[node.mesh];
            const tinygltf::Skin* skin = node.skin >= 0 && node.skin < (int)model.skins.size() ? &model.skins[node.skin] : nullptr;
            // without a skin: rides on the nearest joint above it, else left out
            int rigid = -1;
            if (!skin)
            {
                for (int p = parent[ni]; p >= 0; p = parent[p]) if (bind.count(p)) { rigid = addJoint(p); break; }
                if (rigid < 0 && !keepStatic) { ++staticLeft; continue; }
            }
            // skinned vertices: through their skin's placement; others through their node
            // (raw glTF coordinates in, SL's out: P * M, not P * M * Pt)
            const Mat G = mul(P, skin ? skinToWorld[node.skin] : world[ni]);
            Mesh m;
            m.rigged = skin != nullptr;
            m.name = node.name.empty() ? (gm.name.empty() ? "mesh" + std::to_string(ni) : gm.name) : node.name;
            std::map<int, int> matIndex;
            for (const auto& prim : gm.primitives)
            {
                if (prim.mode != -1 && prim.mode != TINYGLTF_MODE_TRIANGLES) continue;
                auto attrib = [&](const char* n) { auto it = prim.attributes.find(n); return it == prim.attributes.end() ? -1 : it->second; };
                int cp = 0, cn = 0, cu = 0, cj = 0, cw = 0, ci = 0;
                const std::vector<double> P3 = readAccessor(model, attrib("POSITION"), cp);
                if (cp != 3 || P3.empty()) continue;
                const std::vector<double> N3 = readAccessor(model, attrib("NORMAL"), cn);
                const std::vector<double> U2 = readAccessor(model, attrib("TEXCOORD_0"), cu);
                const std::vector<double> J4 = readAccessor(model, attrib("JOINTS_0"), cj);
                const std::vector<double> W4 = readAccessor(model, attrib("WEIGHTS_0"), cw);
                std::vector<double> idx = readAccessor(model, prim.indices, ci);
                const size_t nv = P3.size() / 3;
                if (idx.empty()) for (size_t i = 0; i < nv; ++i) idx.push_back((double)i);
                const int base = (int)m.vertexCount();
                for (size_t v = 0; v < nv; ++v)
                {
                    // glTF -> SL axes (and the node's transform for meshes without a skin)
                    const double gx = P3[v * 3], gy = P3[v * 3 + 1], gz = P3[v * 3 + 2];
                    for (int r = 0; r < 3; ++r) m.pos.push_back(G[r * 4] * gx + G[r * 4 + 1] * gy + G[r * 4 + 2] * gz + G[r * 4 + 3]);
                    double nx = 0, ny = 0, nz = 1;
                    if (cn == 3 && N3.size() >= (v + 1) * 3) { nx = N3[v * 3]; ny = N3[v * 3 + 1]; nz = N3[v * 3 + 2]; }
                    double sn[3];
                    for (int r = 0; r < 3; ++r) sn[r] = G[r * 4] * nx + G[r * 4 + 1] * ny + G[r * 4 + 2] * nz;
                    const double l = std::sqrt(sn[0] * sn[0] + sn[1] * sn[1] + sn[2] * sn[2]);
                    for (int r = 0; r < 3; ++r) m.nrm.push_back(l > 1e-12 ? sn[r] / l : 0.0);
                    // glTF's UV origin is the top left; COLLADA's the bottom left
                    if (cu == 2 && U2.size() >= (v + 1) * 2) { m.uv.push_back(U2[v * 2]); m.uv.push_back(1.0 - U2[v * 2 + 1]); }
                    std::vector<std::pair<int, double>> w;
                    if (skin && cj == 4 && cw == 4 && J4.size() >= (v + 1) * 4 && W4.size() >= (v + 1) * 4)
                        for (int k = 0; k < 4; ++k)
                        {
                            const int js = (int)J4[v * 4 + k];
                            const double ww = W4[v * 4 + k];
                            if (ww <= 0.0 || js < 0 || js >= (int)skin->joints.size()) continue;
                            const int j = addJoint(skin->joints[js]);
                            out.joints[j].weighted = true;
                            w.push_back({ j, ww });
                        }
                    else if (rigid >= 0) { out.joints[rigid].weighted = true; w.push_back({ rigid, 1.0 }); }
                    m.weights.push_back(w);
                }
                // keep the uv list whole: all vertices or none
                if (!m.uv.empty() && m.uv.size() != m.vertexCount() * 2) m.uv.clear();
                int mat = 0;
                {
                    auto it = matIndex.find(prim.material);
                    if (it == matIndex.end())
                    {
                        mat = (int)m.materials.size();
                        matIndex[prim.material] = mat;
                        std::string name = prim.material >= 0 && prim.material < (int)model.materials.size() ? model.materials[prim.material].name : std::string();
                        if (name.empty()) name = "material" + std::to_string(prim.material < 0 ? 0 : prim.material);
                        m.materials.push_back(name);
                    }
                    else mat = it->second;
                }
                for (size_t t = 0; t + 2 < idx.size(); t += 3)
                {
                    const int a = base + (int)idx[t], b = base + (int)idx[t + 1], c = base + (int)idx[t + 2];
                    if (a == b || b == c || a == c) continue;
                    m.tris.push_back({ a, b, c });
                    m.triMat.push_back(mat);
                }
            }
            if (!m.tris.empty()) out.meshes.push_back(m);
        }
        if (staticLeft) out.notes.push_back(std::to_string(staticLeft) + " static meshes (not on a bone) left out");
        if (vrm0) out.notes.push_back("VRM 0.x: turned to face forward");
        if (out.meshes.empty()) { out.error = keepStatic ? "no_mesh" : "no_rigged_mesh"; return false; }
        return true;
    }
}
