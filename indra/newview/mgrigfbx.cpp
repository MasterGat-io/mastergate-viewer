/**
 * @file mgrigfbx.cpp
 * @brief MasterGate Viewer: Creator Lab's rig converter, FBX reader (ufbx).
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
#include "ufbx/ufbx.h"

#include <cmath>
#include <functional>
#include <map>
#include <unordered_map>

namespace
{
    // ufbx's vectors are a struct around a union around a struct: set the fields rather than
    // brace-initialise them (clang -Wmissing-braces on macOS)
    ufbx_vec3 vec3(ufbx_real x, ufbx_real y, ufbx_real z) { ufbx_vec3 r; r.x = x; r.y = y; r.z = z; return r; }
    ufbx_vec2 vec2(ufbx_real x, ufbx_real y) { ufbx_vec2 r; r.x = x; r.y = y; return r; }

    MGRig::Mat toMat(const ufbx_matrix& m)
    {
        MGRig::Mat r{};
        for (int c = 0; c < 4; ++c)
        {
            r[c] = m.cols[c].x;
            r[4 + c] = m.cols[c].y;
            r[8 + c] = m.cols[c].z;
        }
        r[15] = 1.0;
        return r;
    }

    std::string str(const ufbx_string& s) { return std::string(s.data, s.length); }
}

namespace MGRig
{
    bool readFbx(const std::string& path, Model& out, bool keepStatic)
    {
        out = Model();
        out.format = "fbx";
        ufbx_load_opts opts = {};
        // SL's frame: X forward (the character's front), Y left, Z up; meters. ufbx's
        // "right" is the viewer's right looking at the front, i.e. the character's left
        opts.target_axes.right = UFBX_COORDINATE_AXIS_POSITIVE_Y;
        opts.target_axes.up = UFBX_COORDINATE_AXIS_POSITIVE_Z;
        opts.target_axes.front = UFBX_COORDINATE_AXIS_POSITIVE_X;
        opts.target_unit_meters = 1.0;
        opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
        opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
        opts.generate_missing_normals = true;
        opts.ignore_animation = true;
        opts.ignore_embedded = true;
        opts.load_external_files = false;
        opts.clean_skin_weights = true;
        ufbx_error err;
        ufbx_scene* scene = ufbx_load_file_len(path.data(), path.size(), &opts, &err);
        if (!scene)
        {
            out.error = "read: " + str(err.description);
            return false;
        }

        // bind places: from the skin clusters, else the bind pose, else the scene as it is
        std::unordered_map<const ufbx_node*, ufbx_matrix> clusterBind;
        for (size_t s = 0; s < scene->skin_deformers.count; ++s)
        {
            const ufbx_skin_deformer* sk = scene->skin_deformers.data[s];
            for (size_t c = 0; c < sk->clusters.count; ++c)
            {
                const ufbx_skin_cluster* cl = sk->clusters.data[c];
                if (cl->bone_node && !clusterBind.count(cl->bone_node)) clusterBind[cl->bone_node] = cl->bind_to_world;
            }
        }
        std::vector<const ufbx_pose*> bindPoses;
        for (size_t i = 0; i < scene->poses.count; ++i) if (scene->poses.data[i]->is_bind_pose) bindPoses.push_back(scene->poses.data[i]);
        auto bindOf = [&](const ufbx_node* n) -> Mat
        {
            auto it = clusterBind.find(n);
            if (it != clusterBind.end()) return toMat(it->second);
            for (const ufbx_pose* p : bindPoses)
            {
                const ufbx_bone_pose* bp = ufbx_get_bone_pose(p, n);
                if (bp) return toMat(bp->bone_to_world);
            }
            return toMat(n->node_to_world);
        };

        std::unordered_map<const ufbx_node*, int> index;
        std::function<int(const ufbx_node*)> addJoint = [&](const ufbx_node* n) -> int
        {
            if (!n || n->is_root) return -1;
            auto it = index.find(n);
            if (it != index.end()) return it->second;
            const int parent = addJoint(n->parent);
            Joint j;
            j.name = str(n->name);
            j.parent = parent;
            j.bind = bindOf(n);
            const int id = (int)out.joints.size();
            out.joints.push_back(j);
            index[n] = id;
            return id;
        };
        // a node belongs to the skeleton when it is a bone or a cluster's bone
        auto isSkeletonNode = [&](const ufbx_node* n) { return n && !n->is_root && (n->bone || clusterBind.count(n)); };

        std::vector<uint32_t> tri;
        size_t staticLeft = 0;
        for (size_t ni = 0; ni < scene->nodes.count; ++ni)
        {
            const ufbx_node* node = scene->nodes.data[ni];
            const ufbx_mesh* mesh = node->mesh;
            if (!mesh || mesh->num_faces == 0) continue;
            const ufbx_skin_deformer* skin = mesh->skin_deformers.count ? mesh->skin_deformers.data[0] : nullptr;
            // a mesh without skin under a bone (eyes, teeth, props) rides on that bone
            int rigidJoint = -1;
            if (!skin)
            {
                for (const ufbx_node* p = node->parent; p && !p->is_root; p = p->parent)
                    if (isSkeletonNode(p)) { rigidJoint = addJoint(p); break; }
                if (rigidJoint < 0 && !keepStatic) { ++staticLeft; continue; }
            }
            // the geometry at bind time: through the strongest cluster (they agree)
            ufbx_matrix G = node->geometry_to_world;
            if (skin)
            {
                size_t best = 0, bestN = 0;
                for (size_t c = 0; c < skin->clusters.count; ++c)
                    if (skin->clusters.data[c]->num_weights > bestN) { bestN = skin->clusters.data[c]->num_weights; best = c; }
                if (bestN)
                {
                    const ufbx_skin_cluster* cl = skin->clusters.data[best];
                    G = ufbx_matrix_mul(&cl->bind_to_world, &cl->geometry_to_bone);
                }
            }
            const ufbx_matrix N = ufbx_matrix_for_normals(&G);

            Mesh m;
            m.name = str(node->name);
            m.rigged = skin != nullptr;
            const size_t nm = node->materials.count ? node->materials.count : mesh->materials.count;
            for (size_t k = 0; k < nm; ++k)
            {
                const ufbx_material* mat = node->materials.count ? node->materials.data[k] : mesh->materials.data[k];
                m.materials.push_back(mat ? str(mat->name) : std::string("material") + std::to_string(k));
            }
            if (m.materials.empty()) m.materials.push_back("material");

            // one vertex per (position, normal, uv) used together
            std::unordered_map<CornerKey, int, CornerKeyHash> made;
            const bool hasUv = mesh->vertex_uv.exists;
            auto vertexFor = [&](uint32_t ix) -> int
            {
                const uint32_t vp = mesh->vertex_indices.data[ix];
                const uint32_t vn = mesh->vertex_normal.exists ? mesh->vertex_normal.indices.data[ix] : 0;
                const uint32_t vu = hasUv ? mesh->vertex_uv.indices.data[ix] : 0;
                // by value: same position, normal and uv
                const ufbx_vec3 rn = mesh->vertex_normal.exists ? mesh->vertex_normal.values.data[vn] : vec3(0, 0, 1);
                const ufbx_vec2 ru = hasUv ? mesh->vertex_uv.values.data[vu] : vec2(0, 0);
                const CornerKey key = cornerKey(vp, rn.x, rn.y, rn.z, ru.x, ru.y);
                auto it = made.find(key);
                if (it != made.end()) return it->second;
                const int id = (int)(m.pos.size() / 3);
                const ufbx_vec3 p = ufbx_transform_position(&G, mesh->vertex_position.values.data[mesh->vertex_position.indices.data[ix]]);
                m.pos.insert(m.pos.end(), { p.x, p.y, p.z });
                ufbx_vec3 n = vec3(0, 0, 1);
                if (mesh->vertex_normal.exists) n = ufbx_transform_direction(&N, mesh->vertex_normal.values.data[vn]);
                const double l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                if (l > 1e-12) { n.x /= l; n.y /= l; n.z /= l; }
                m.nrm.insert(m.nrm.end(), { n.x, n.y, n.z });
                if (hasUv)
                {
                    const ufbx_vec2 u = mesh->vertex_uv.values.data[vu];
                    m.uv.insert(m.uv.end(), { u.x, u.y });
                }
                std::vector<std::pair<int, double>> w;
                if (skin && vp < skin->vertices.count)
                {
                    const ufbx_skin_vertex sv = skin->vertices.data[vp];
                    for (uint32_t k = 0; k < sv.num_weights; ++k)
                    {
                        const ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + k];
                        const ufbx_skin_cluster* cl = skin->clusters.data[sw.cluster_index];
                        if (!cl->bone_node || sw.weight <= 0.0) continue;
                        const int j = addJoint(cl->bone_node);
                        out.joints[j].weighted = true;
                        w.push_back({ j, sw.weight });
                    }
                }
                else if (rigidJoint >= 0)
                {
                    out.joints[rigidJoint].weighted = true;
                    w.push_back({ rigidJoint, 1.0 });
                }
                m.weights.push_back(w);
                made[key] = id;
                return id;
            };
            tri.resize(mesh->max_face_triangles * 3);
            for (size_t f = 0; f < mesh->num_faces; ++f)
            {
                const ufbx_face face = mesh->faces.data[f];
                if (face.num_indices < 3) continue;
                const uint32_t nt = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
                const int mat = mesh->face_material.count > f ? (int)mesh->face_material.data[f] : 0;
                for (uint32_t t = 0; t < nt; ++t)
                {
                    const int a = vertexFor(tri[t * 3]), b = vertexFor(tri[t * 3 + 1]), c = vertexFor(tri[t * 3 + 2]);
                    if (a == b || b == c || a == c) continue;
                    m.tris.push_back({ a, b, c });
                    m.triMat.push_back(mat < (int)m.materials.size() ? mat : 0);
                }
            }
            if (!m.tris.empty()) out.meshes.push_back(m);
        }
        // the whole skeleton (bones nothing is weighted to still carry their children)
        for (size_t ni = 0; ni < scene->nodes.count; ++ni)
            if (isSkeletonNode(scene->nodes.data[ni])) addJoint(scene->nodes.data[ni]);
        if (staticLeft) out.notes.push_back(std::to_string(staticLeft) + " static meshes (not on a bone) left out");
        if (scene->blend_deformers.count) out.notes.push_back("shape keys (morphs) left out");
        ufbx_free_scene(scene);
        if (out.meshes.empty()) { out.error = keepStatic ? "no_mesh" : "no_rigged_mesh"; return false; }
        return true;
    }
}
