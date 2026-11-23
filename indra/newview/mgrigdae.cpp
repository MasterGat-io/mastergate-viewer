/**
 * @file mgrigdae.cpp
 * @brief MasterGate Viewer: Creator Lab, a general COLLADA reader (static and skinned meshes,
 *        normals, UVs, materials, node transforms) for rigging clothes and accessories.
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

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <unordered_map>

namespace
{
    using std::string;
    using MGRig::Mat;

    const Mat IDENT = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    Mat mul(const Mat& a, const Mat& b)
    {
        Mat r{};
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) for (int k = 0; k < 4; ++k) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
        return r;
    }
    bool invert(const Mat& m, Mat& out)
    {
        double a[4][8];
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
        for (int c = 0; c < 4; ++c)
        {
            int p = c;
            for (int i = c + 1; i < 4; ++i) if (std::fabs(a[i][c]) > std::fabs(a[p][c])) p = i;
            if (std::fabs(a[p][c]) < 1e-12) return false;
            if (p != c) for (int j = 0; j < 8; ++j) std::swap(a[p][j], a[c][j]);
            const double d = a[c][c];
            for (int j = 0; j < 8; ++j) a[c][j] /= d;
            for (int i = 0; i < 4; ++i) { if (i == c) continue; const double f = a[i][c]; for (int j = 0; j < 8; ++j) a[i][j] -= f * a[c][j]; }
        }
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out[i * 4 + j] = a[i][j + 4];
        return true;
    }

    // ---- a small XML scanner (as in mgcreatordae.cpp) ----
    size_t findTag(const string& t, const string& tag, size_t from, size_t to = string::npos)
    {
        const string open = "<" + tag;
        size_t p = from;
        while ((p = t.find(open, p)) != string::npos && p < to)
        {
            const char c = p + open.size() < t.size() ? t[p + open.size()] : '\0';
            if (c == ' ' || c == '>' || c == '/' || c == '\n' || c == '\r' || c == '\t') return p;
            p += open.size();
        }
        return string::npos;
    }
    size_t tagEnd(const string& t, size_t s) { return t.find('>', s); }
    bool selfClosed(const string& t, size_t s) { const size_t e = tagEnd(t, s); return e != string::npos && e > 0 && t[e - 1] == '/'; }
    size_t elemEnd(const string& t, const string& tag, size_t s)
    {
        if (selfClosed(t, s)) return tagEnd(t, s) + 1;
        const size_t c = t.find("</" + tag + ">", s);
        if (c == string::npos) return string::npos;
        return t.find('>', c) + 1;
    }
    bool content(const string& t, const string& tag, size_t s, size_t& cs, size_t& ce)
    {
        const size_t e = tagEnd(t, s);
        if (e == string::npos || selfClosed(t, s)) return false;
        cs = e + 1;
        ce = t.find("</" + tag + ">", cs);
        return ce != string::npos;
    }
    string attr(const string& t, size_t s, const string& name)
    {
        const size_t e = tagEnd(t, s);
        if (e == string::npos) return string();
        const string key = name + "=";
        size_t p = s;
        while ((p = t.find(key, p)) != string::npos && p < e)
        {
            const char before = t[p - 1];
            if (before == ' ' || before == '\n' || before == '\r' || before == '\t')
            {
                const size_t vq = p + key.size();
                const char q = t[vq];
                if (q == '"' || q == '\'')
                {
                    const size_t ve = t.find(q, vq + 1);
                    if (ve != string::npos && ve < e) return t.substr(vq + 1, ve - vq - 1);
                }
            }
            p += key.size();
        }
        return string();
    }
    string idRef(const string& s) { return (!s.empty() && s[0] == '#') ? s.substr(1) : s; }
    void doubles(const string& t, size_t s, size_t e, std::vector<double>& out)
    {
        out.clear();
        const char* p = t.c_str() + s;
        const char* end = t.c_str() + e;
        while (p < end)
        {
            while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
            if (p >= end) break;
            char* q = nullptr;
            const double v = std::strtod(p, &q);
            if (q == p) { ++p; continue; }
            out.push_back(v);
            p = q;
        }
    }
    void ints(const string& t, size_t s, size_t e, std::vector<int>& out)
    {
        out.clear();
        const char* p = t.c_str() + s;
        const char* end = t.c_str() + e;
        while (p < end)
        {
            while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
            if (p >= end) break;
            char* q = nullptr;
            const long v = std::strtol(p, &q, 10);
            if (q == p) { ++p; continue; }
            out.push_back((int)v);
            p = q;
        }
    }

    // every element with an id: tag start, by id (one pass)
    struct Index
    {
        std::unordered_map<string, size_t> byId;
        void build(const string& t)
        {
            size_t p = 0;
            while ((p = t.find(" id=", p)) != string::npos)
            {
                const size_t s = t.rfind('<', p);
                const char q = t[p + 4];
                if (s != string::npos && (q == '"' || q == '\''))
                {
                    const size_t e = t.find(q, p + 5);
                    if (e != string::npos) byId.emplace(t.substr(p + 5, e - p - 5), s);
                }
                p += 4;
            }
        }
        size_t find(const string& id) const { auto it = byId.find(id); return it == byId.end() ? string::npos : it->second; }
    };

    // a <source>: its numbers and stride
    bool source(const string& t, const Index& ix, const string& id, std::vector<double>& v, int& stride)
    {
        const size_t s = ix.find(id);
        if (s == string::npos) return false;
        const size_t e = elemEnd(t, "source", s);
        const size_t fa = findTag(t, "float_array", s, e);
        size_t cs, ce;
        if (fa == string::npos || !content(t, "float_array", fa, cs, ce)) return false;
        doubles(t, cs, ce, v);
        const size_t ac = findTag(t, "accessor", s, e);
        const string st = ac != string::npos ? attr(t, ac, "stride") : string();
        stride = st.empty() ? 1 : std::max(1, std::atoi(st.c_str()));
        return true;
    }

    struct Instance
    {
        bool skinned = false;
        string url;                          // geometry or controller id
        Mat world = IDENT;                   // the node's place
        std::map<string, string> materials;  // symbol -> material name
        string nodeName;
    };
}

namespace MGRig
{
    bool readDae(const std::string& path, Model& out, bool keepStatic)
    {
        out = Model();
        out.format = "dae";
        std::ifstream f(path, std::ios::binary);
        if (!f) { out.error = "read"; return false; }
        const string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        Index ix;
        ix.build(t);

        // up axis and unit: everything to Z up, meters (SL's frame; Blender exports Z up)
        double meter = 1.0;
        {
            const size_t u = findTag(t, "unit", 0);
            if (u != string::npos) { const string m = attr(t, u, "meter"); if (!m.empty()) meter = std::atof(m.c_str()); }
            if (meter <= 0.0) meter = 1.0;
        }
        string up = "Y_UP";
        {
            const size_t ua = findTag(t, "up_axis", 0);
            size_t cs, ce;
            if (ua != string::npos && content(t, "up_axis", ua, cs, ce)) up = t.substr(cs, ce - cs);
            up.erase(std::remove_if(up.begin(), up.end(), [](char c) { return std::isspace((unsigned char)c); }), up.end());
        }
        Mat A = IDENT;
        if (up == "Y_UP") A = Mat{ 1, 0, 0, 0,  0, 0, -1, 0,  0, 1, 0, 0,  0, 0, 0, 1 };      // (x, y, z) -> (x, -z, y)
        else if (up == "X_UP") A = Mat{ 0, -1, 0, 0,  1, 0, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
        for (int i = 0; i < 12; ++i) if (i % 4 != 3) A[i] *= meter;   // turned, then scaled to meters
        if (up != "Z_UP") out.notes.push_back("turned from " + up + " to Z up");

        // the scene: nodes and their transforms, instances
        std::vector<Instance> inst;
        {
            const size_t vs = findTag(t, "library_visual_scenes", 0);
            const size_t ve = vs == string::npos ? string::npos : elemEnd(t, "library_visual_scenes", vs);
            std::vector<Mat> stack(1, IDENT);
            std::vector<string> names(1, string());
            size_t p = vs;
            while (vs != string::npos && (p = t.find('<', p + 1)) != string::npos && p < ve)
            {
                const size_t e = tagEnd(t, p);
                if (e == string::npos) break;
                const bool closing = t[p + 1] == '/';
                auto is = [&](const char* name) { return findTag(t, name, p, p + 1) == p; };
                if (closing)
                {
                    if (t.compare(p, 7, "</node>") == 0 && stack.size() > 1) { stack.pop_back(); names.pop_back(); }
                    continue;
                }
                if (is("node"))
                {
                    if (!selfClosed(t, p)) { stack.push_back(stack.back()); names.push_back(attr(t, p, "name")); }
                    continue;
                }
                size_t cs, ce;
                std::vector<double> v;
                if (is("matrix") && content(t, "matrix", p, cs, ce))
                {
                    doubles(t, cs, ce, v);
                    if (v.size() >= 16) { Mat m{}; for (int i = 0; i < 16; ++i) m[i] = v[i]; stack.back() = mul(stack.back(), m); }
                }
                else if (is("translate") && content(t, "translate", p, cs, ce))
                {
                    doubles(t, cs, ce, v);
                    if (v.size() >= 3) stack.back() = mul(stack.back(), Mat{ 1, 0, 0, v[0], 0, 1, 0, v[1], 0, 0, 1, v[2], 0, 0, 0, 1 });
                }
                else if (is("scale") && content(t, "scale", p, cs, ce))
                {
                    doubles(t, cs, ce, v);
                    if (v.size() >= 3) stack.back() = mul(stack.back(), Mat{ v[0], 0, 0, 0, 0, v[1], 0, 0, 0, 0, v[2], 0, 0, 0, 0, 1 });
                }
                else if (is("rotate") && content(t, "rotate", p, cs, ce))
                {
                    doubles(t, cs, ce, v);
                    if (v.size() >= 4)
                    {
                        double x = v[0], y = v[1], z = v[2];
                        const double l = std::sqrt(x * x + y * y + z * z);
                        if (l > 1e-12)
                        {
                            x /= l; y /= l; z /= l;
                            const double a = v[3] * 3.14159265358979323846 / 180.0, c = std::cos(a), s = std::sin(a), k = 1 - c;
                            stack.back() = mul(stack.back(), Mat{ c + x * x * k, x * y * k - z * s, x * z * k + y * s, 0,
                                                                  y * x * k + z * s, c + y * y * k, y * z * k - x * s, 0,
                                                                  z * x * k - y * s, z * y * k + x * s, c + z * z * k, 0, 0, 0, 0, 1 });
                        }
                    }
                }
                else if (is("instance_geometry") || is("instance_controller"))
                {
                    Instance in;
                    in.skinned = is("instance_controller");
                    in.url = idRef(attr(t, p, "url"));
                    in.world = stack.back();
                    in.nodeName = names.back();
                    const string tag = in.skinned ? "instance_controller" : "instance_geometry";
                    const size_t ie = elemEnd(t, tag, p);
                    size_t q = p;
                    while ((q = findTag(t, "instance_material", q + 1, ie)) != string::npos)
                    {
                        const string sym = attr(t, q, "symbol"), target = idRef(attr(t, q, "target"));
                        const size_t m = ix.find(target);
                        string name = m != string::npos ? attr(t, m, "name") : string();
                        in.materials[sym] = name.empty() ? target : name;
                    }
                    inst.push_back(in);
                }
            }
        }

        size_t staticLeft = 0;
        for (const Instance& in : inst)
        {
            string geomId = in.url;
            Mat place = mul(A, in.world);   // static: the node places it
            std::vector<string> joints;
            std::vector<Mat> invBind;
            std::vector<std::vector<std::pair<int, double>>> skinW;   // per position
            if (in.skinned)
            {
                const size_t c = ix.find(in.url);
                if (c == string::npos) continue;
                const size_t ce = elemEnd(t, "controller", c);
                const size_t sk = findTag(t, "skin", c, ce);
                if (sk == string::npos) continue;
                geomId = idRef(attr(t, sk, "source"));
                Mat bs = IDENT;
                size_t cs, cee;
                const size_t bsm = findTag(t, "bind_shape_matrix", sk, ce);
                std::vector<double> v;
                if (bsm != string::npos && content(t, "bind_shape_matrix", bsm, cs, cee)) { doubles(t, cs, cee, v); if (v.size() >= 16) for (int i = 0; i < 16; ++i) bs[i] = v[i]; }
                place = mul(A, bs);   // skinned: the bind shape places it (in the skeleton's space)
                // joints and inverse binds
                const size_t js = findTag(t, "joints", sk, ce);
                const size_t jse = js == string::npos ? string::npos : elemEnd(t, "joints", js);
                string jointSrc, ibSrc;
                for (size_t q = js; js != string::npos && (q = findTag(t, "input", q + 1, jse)) != string::npos;)
                {
                    const string sem = attr(t, q, "semantic");
                    if (sem == "JOINT") jointSrc = idRef(attr(t, q, "source"));
                    else if (sem == "INV_BIND_MATRIX") ibSrc = idRef(attr(t, q, "source"));
                }
                const size_t jsrc = ix.find(jointSrc);
                if (jsrc != string::npos)
                {
                    const size_t na = findTag(t, "Name_array", jsrc, elemEnd(t, "source", jsrc));
                    if (na != string::npos && content(t, "Name_array", na, cs, cee)) { std::istringstream ss(t.substr(cs, cee - cs)); string w; while (ss >> w) joints.push_back(w); }
                }
                int stride = 16;
                std::vector<double> ib;
                Mat Ainv;
                invert(A, Ainv);
                if (source(t, ix, ibSrc, ib, stride))
                    for (size_t k = 0; k < joints.size() && (k + 1) * 16 <= ib.size(); ++k)
                    {
                        Mat m{};
                        for (int i = 0; i < 16; ++i) m[i] = ib[k * 16 + i];
                        invBind.push_back(mul(m, Ainv));   // the joint's space kept; vertices now in SL's frame
                    }
                // weights per position
                const size_t vw = findTag(t, "vertex_weights", sk, ce);
                if (vw != string::npos)
                {
                    const size_t vwe = elemEnd(t, "vertex_weights", vw);
                    int jo = 0, wo = 1, maxo = 0;
                    string wSrc;
                    for (size_t q = vw; (q = findTag(t, "input", q + 1, vwe)) != string::npos;)
                    {
                        const string sem = attr(t, q, "semantic"), off = attr(t, q, "offset");
                        const int o = off.empty() ? 0 : std::atoi(off.c_str());
                        maxo = std::max(maxo, o);
                        if (sem == "JOINT") jo = o;
                        else if (sem == "WEIGHT") { wo = o; wSrc = idRef(attr(t, q, "source")); }
                    }
                    std::vector<double> wv;
                    int ws = 1;
                    source(t, ix, wSrc, wv, ws);
                    std::vector<int> vcount, vidx;
                    const size_t vc = findTag(t, "vcount", vw, vwe), vv = findTag(t, "v", vw, vwe);
                    if (vc != string::npos && content(t, "vcount", vc, cs, cee)) ints(t, cs, cee, vcount);
                    if (vv != string::npos && content(t, "v", vv, cs, cee)) ints(t, cs, cee, vidx);
                    const int st = maxo + 1;
                    size_t at = 0;
                    for (int n : vcount)
                    {
                        std::vector<std::pair<int, double>> w;
                        for (int k = 0; k < n; ++k, ++at)
                        {
                            if ((at + 1) * st > vidx.size()) break;
                            const int j = vidx[at * st + jo], wi = vidx[at * st + wo];
                            const double wt = wi >= 0 && wi < (int)wv.size() ? wv[wi] : 0.0;
                            if (j >= 0 && j < (int)joints.size() && wt > 0.0) w.push_back({ j, wt });
                        }
                        skinW.push_back(w);
                    }
                }
            }
            else if (!keepStatic) { ++staticLeft; continue; }

            // the geometry
            const size_t g = ix.find(geomId);
            if (g == string::npos) continue;
            const size_t ge = elemEnd(t, "geometry", g);
            const size_t ms = findTag(t, "mesh", g, ge);
            if (ms == string::npos) continue;
            const size_t me = elemEnd(t, "mesh", ms);
            // <vertices>: POSITION (and maybe NORMAL per position)
            const size_t vt = findTag(t, "vertices", ms, me);
            if (vt == string::npos) continue;
            const string verticesId = attr(t, vt, "id");
            string posSrc, vNrmSrc;
            for (size_t q = vt; (q = findTag(t, "input", q + 1, elemEnd(t, "vertices", vt))) != string::npos;)
            {
                const string sem = attr(t, q, "semantic");
                if (sem == "POSITION") posSrc = idRef(attr(t, q, "source"));
                else if (sem == "NORMAL") vNrmSrc = idRef(attr(t, q, "source"));
            }
            std::vector<double> P, N, U, VN;
            int ps = 3, ns = 3, us = 2, vns = 3;
            if (!source(t, ix, posSrc, P, ps) || ps < 3) continue;
            if (!vNrmSrc.empty()) source(t, ix, vNrmSrc, VN, vns);
            Mat nrm = place;
            nrm[3] = nrm[7] = nrm[11] = 0.0;
            {
                Mat inv;
                if (invert(nrm, inv)) for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) nrm[r * 4 + c] = inv[c * 4 + r];   // inverse transpose
            }

            Mesh m;
            m.name = in.nodeName.empty() ? attr(t, g, "name") : in.nodeName;
            if (m.name.empty()) m.name = geomId;
            m.rigged = in.skinned;
            if (in.skinned) { m.skinJoints = joints; m.skinInvBind = invBind; }
            std::map<string, int> matIndex;
            std::unordered_map<MGRig::CornerKey, int, MGRig::CornerKeyHash> made;
            for (const char* kind : { "triangles", "polylist", "polygons" })
            {
                size_t p = ms;
                while ((p = findTag(t, kind, p, me)) != string::npos)
                {
                    const size_t e = elemEnd(t, kind, p);
                    if (e == string::npos || e > me) break;
                    int vOff = -1, nOff = -1, uOff = -1, maxo = 0, uSet = 1 << 30;
                    string nSrc, uSrc;
                    for (size_t q = p; (q = findTag(t, "input", q + 1, e)) != string::npos;)
                    {
                        const string sem = attr(t, q, "semantic"), off = attr(t, q, "offset"), set = attr(t, q, "set");
                        const int o = off.empty() ? 0 : std::atoi(off.c_str());
                        maxo = std::max(maxo, o);
                        if (sem == "VERTEX") vOff = o;
                        else if (sem == "NORMAL") { nOff = o; nSrc = idRef(attr(t, q, "source")); }
                        else if (sem == "TEXCOORD") { const int s = set.empty() ? 0 : std::atoi(set.c_str()); if (s < uSet) { uSet = s; uOff = o; uSrc = idRef(attr(t, q, "source")); } }
                    }
                    if (vOff < 0) { p = e; continue; }
                    if (nOff >= 0 && N.empty()) source(t, ix, nSrc, N, ns);
                    if (uOff >= 0 && U.empty()) source(t, ix, uSrc, U, us);
                    const string sym = attr(t, p, "material");
                    int mat = 0;
                    {
                        auto it = matIndex.find(sym);
                        if (it == matIndex.end())
                        {
                            mat = (int)m.materials.size();
                            matIndex[sym] = mat;
                            auto bm = in.materials.find(sym);
                            m.materials.push_back(bm != in.materials.end() ? bm->second : (sym.empty() ? string("material") : sym));
                        }
                        else mat = it->second;
                    }
                    const int stride = maxo + 1;
                    auto corner = [&](const std::vector<int>& idx, size_t c) -> int
                    {
                        const int vp = idx[c * stride + vOff];
                        const int vn = nOff >= 0 ? idx[c * stride + nOff] : -1;
                        const int vu = uOff >= 0 ? idx[c * stride + uOff] : -1;
                        if (vp < 0 || (size_t)(vp + 1) * ps > P.size()) return -1;
                        double nx = 0, ny = 0, nz = 1;
                        if (vn >= 0 && (size_t)(vn + 1) * ns <= N.size()) { nx = N[vn * ns]; ny = N[vn * ns + 1]; nz = N[vn * ns + 2]; }
                        else if (!VN.empty() && (size_t)(vp + 1) * vns <= VN.size()) { nx = VN[vp * vns]; ny = VN[vp * vns + 1]; nz = VN[vp * vns + 2]; }
                        double tu = 0.0, tv = 0.0;
                        if (uOff >= 0 && vu >= 0 && (size_t)(vu * us + 1) < U.size()) { tu = U[vu * us]; tv = U[vu * us + 1]; }
                        // one vertex per position + normal + uv, by value
                        const MGRig::CornerKey key = MGRig::cornerKey(vp, nx, ny, nz, tu, tv);
                        auto it = made.find(key);
                        if (it != made.end()) return it->second;
                        const int id = (int)m.vertexCount();
                        const double x = P[vp * ps], y = P[vp * ps + 1], z = P[vp * ps + 2];
                        for (int r = 0; r < 3; ++r) m.pos.push_back(place[r * 4] * x + place[r * 4 + 1] * y + place[r * 4 + 2] * z + place[r * 4 + 3]);
                        double n3[3];
                        for (int r = 0; r < 3; ++r) n3[r] = nrm[r * 4] * nx + nrm[r * 4 + 1] * ny + nrm[r * 4 + 2] * nz;
                        const double l = std::sqrt(n3[0] * n3[0] + n3[1] * n3[1] + n3[2] * n3[2]);
                        for (int r = 0; r < 3; ++r) m.nrm.push_back(l > 1e-12 ? n3[r] / l : 0.0);
                        if (uOff >= 0) { m.uv.push_back(tu); m.uv.push_back(tv); }
                        m.weights.push_back(vp < (int)skinW.size() ? skinW[vp] : std::vector<std::pair<int, double>>());
                        made[key] = id;
                        return id;
                    };
                    std::vector<int> counts;
                    if (string(kind) == "polylist")
                    {
                        const size_t vc = findTag(t, "vcount", p, e);
                        size_t cs, ce;
                        if (vc != string::npos && content(t, "vcount", vc, cs, ce)) ints(t, cs, ce, counts);
                    }
                    size_t pp = p;
                    while ((pp = findTag(t, "p", pp, e)) != string::npos)
                    {
                        size_t cs, ce;
                        if (!content(t, "p", pp, cs, ce)) break;
                        std::vector<int> idx;
                        ints(t, cs, ce, idx);
                        const size_t nc = idx.size() / stride;
                        auto tri = [&](size_t a, size_t b, size_t c)
                        {
                            const int va = corner(idx, a), vb = corner(idx, b), vc2 = corner(idx, c);
                            if (va < 0 || vb < 0 || vc2 < 0 || va == vb || vb == vc2 || va == vc2) return;
                            m.tris.push_back({ va, vb, vc2 });
                            m.triMat.push_back(mat);
                        };
                        if (string(kind) == "triangles") for (size_t i = 0; i + 2 < nc; i += 3) tri(i, i + 1, i + 2);
                        else if (string(kind) == "polylist")
                        {
                            size_t at = 0;
                            for (int cnt : counts)
                            {
                                if (at + cnt > nc) break;
                                for (int k = 1; k + 1 < cnt; ++k) tri(at, at + k, at + k + 1);
                                at += cnt;
                            }
                        }
                        else for (size_t k = 1; k + 1 < nc; ++k) tri(0, k, k + 1);
                        pp = ce;
                    }
                    p = e;
                }
            }
            if (m.uv.size() != m.vertexCount() * 2) m.uv.clear();
            if (m.materials.empty()) m.materials.push_back("material");
            if (!m.tris.empty()) out.meshes.push_back(m);
        }
        if (staticLeft) out.notes.push_back(std::to_string(staticLeft) + " static meshes left out");
        if (out.meshes.empty()) { out.error = keepStatic ? "no_mesh" : "no_rigged_mesh"; return false; }
        return true;
    }
}
