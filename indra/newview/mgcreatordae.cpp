/**
 * @file mgcreatordae.cpp
 * @brief MasterGate Viewer: Creator Lab's COLLADA reader / writer for rigged garments.
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

#include "mgcreatordae.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

namespace
{
    using std::string;

    // ---- a small, forgiving XML scanner (COLLADA as written by Blender exporters) ----

    // the next "<tag" (followed by a space, '>' or '/') at or after from
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

    size_t tagEnd(const string& t, size_t tagStart)   // the '>' closing the opening tag
    {
        return t.find('>', tagStart);
    }

    bool selfClosed(const string& t, size_t tagStart)
    {
        const size_t e = tagEnd(t, tagStart);
        return e != string::npos && e > 0 && t[e - 1] == '/';
    }

    // the end of the element (position just after "</tag>"); no nesting of the same tag
    size_t elemEnd(const string& t, const string& tag, size_t tagStart)
    {
        if (selfClosed(t, tagStart)) return tagEnd(t, tagStart) + 1;
        const size_t c = t.find("</" + tag + ">", tagStart);
        if (c == string::npos) return string::npos;
        const size_t gt = t.find('>', c);
        return gt == string::npos ? string::npos : gt + 1;
    }

    // content between the opening tag and "</tag>"
    bool content(const string& t, const string& tag, size_t tagStart, size_t& cs, size_t& ce)
    {
        const size_t e = tagEnd(t, tagStart);
        if (e == string::npos || selfClosed(t, tagStart)) return false;
        cs = e + 1;
        ce = t.find("</" + tag + ">", cs);
        return ce != string::npos;
    }

    string attr(const string& t, size_t tagStart, const string& name)
    {
        const size_t e = tagEnd(t, tagStart);
        if (e == string::npos) return string();
        const string key = name + "=";
        size_t p = tagStart;
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

    // an element <tag ... id="id">, anywhere
    size_t findById(const string& t, const string& tag, const string& id)
    {
        size_t p = 0;
        while ((p = findTag(t, tag, p)) != string::npos)
        {
            if (attr(t, p, "id") == id) return p;
            ++p;
        }
        return string::npos;
    }

    string idRef(const string& s) { return (!s.empty() && s[0] == '#') ? s.substr(1) : s; }

    void parseDoubles(const string& t, size_t s, size_t e, std::vector<double>& out)
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

    void parseInts(const string& t, size_t s, size_t e, std::vector<int>& out)
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

    void parseNames(const string& t, size_t s, size_t e, std::vector<string>& out)
    {
        out.clear();
        std::istringstream in(t.substr(s, e - s));
        string w;
        while (in >> w) out.push_back(w);
    }

    // the <float_array> (or Name_array) inside <source id=...>: its content range
    bool sourceArray(const string& t, const string& sourceId, const string& arrayTag, size_t& cs, size_t& ce, size_t* arrayTagStart = nullptr)
    {
        const size_t src = findById(t, "source", sourceId);
        if (src == string::npos) return false;
        const size_t srcEnd = elemEnd(t, "source", src);
        const size_t a = findTag(t, arrayTag, src, srcEnd);
        if (a == string::npos) return false;
        if (arrayTagStart) *arrayTagStart = a;
        return content(t, arrayTag, a, cs, ce);
    }

    // an <input semantic="X" .../> inside [from, to): its source and offset
    bool input(const string& t, size_t from, size_t to, const string& semantic, string& source, int& offset)
    {
        size_t p = from;
        while ((p = findTag(t, "input", p, to)) != string::npos)
        {
            if (attr(t, p, "semantic") == semantic)
            {
                source = idRef(attr(t, p, "source"));
                const string o = attr(t, p, "offset");
                offset = o.empty() ? 0 : std::atoi(o.c_str());
                return true;
            }
            ++p;
        }
        return false;
    }

    int maxOffset(const string& t, size_t from, size_t to)
    {
        int m = 0;
        size_t p = from;
        while ((p = findTag(t, "input", p, to)) != string::npos)
        {
            const string o = attr(t, p, "offset");
            if (!o.empty()) m = std::max(m, std::atoi(o.c_str()));
            ++p;
        }
        return m;
    }

    bool readMatrix(const string& txt, std::array<double, 16>& m)
    {
        std::vector<double> v;
        parseDoubles(txt, 0, txt.size(), v);
        if (v.size() < 16) return false;
        for (int i = 0; i < 16; ++i) m[i] = v[i];
        return true;
    }

    // the triangles of a <mesh>: <triangles>, <polylist> and <polygons> (fanned)
    void readTriangles(const string& t, size_t meshStart, size_t meshEnd, std::vector<std::array<int, 3>>& tris)
    {
        for (const char* kind : { "triangles", "polylist", "polygons" })
        {
            size_t p = meshStart;
            while ((p = findTag(t, kind, p, meshEnd)) != string::npos)
            {
                const size_t e = elemEnd(t, kind, p);
                if (e == string::npos || e > meshEnd) break;
                string src;
                int voff = 0;
                if (!input(t, p, e, "VERTEX", src, voff)) { p = e; continue; }
                const int stride = maxOffset(t, p, e) + 1;
                std::vector<int> counts;
                if (string(kind) == "polylist")
                {
                    const size_t vc = findTag(t, "vcount", p, e);
                    size_t cs, ce;
                    if (vc != string::npos && content(t, "vcount", vc, cs, ce)) parseInts(t, cs, ce, counts);
                }
                // <polygons> holds one <p> per polygon; the others one <p> for all
                size_t pp = p;
                while ((pp = findTag(t, "p", pp, e)) != string::npos)
                {
                    size_t cs, ce;
                    if (!content(t, "p", pp, cs, ce)) break;
                    std::vector<int> idx;
                    parseInts(t, cs, ce, idx);
                    const size_t nv = idx.size() / stride;
                    std::vector<int> verts(nv);
                    for (size_t i = 0; i < nv; ++i) verts[i] = idx[i * stride + voff];
                    if (string(kind) == "triangles")
                    {
                        for (size_t i = 0; i + 2 < nv; i += 3) tris.push_back({ verts[i], verts[i + 1], verts[i + 2] });
                    }
                    else if (string(kind) == "polylist")
                    {
                        size_t at = 0;
                        for (int c : counts)
                        {
                            if (at + c > nv) break;
                            for (int k = 1; k + 1 < c; ++k) tris.push_back({ verts[at], verts[at + k], verts[at + k + 1] });
                            at += c;
                        }
                    }
                    else
                    {
                        for (size_t k = 1; k + 1 < nv; ++k) tris.push_back({ verts[0], verts[k], verts[k + 1] });
                    }
                    pp = ce;
                }
                p = e;
            }
        }
    }

    // numbers written back: enough digits to keep sub-millimetre detail
    void appendNumber(string& out, double v)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.7g", v);
        out += buf;
    }

    struct Patch { size_t start, end; string text; };
}

namespace MGCreatorDae
{
    bool read(const std::string& path, File& out)
    {
        out = File();
        out.path = path;
        std::ifstream in(path, std::ios::binary);
        if (!in) { out.error = "open"; return false; }
        out.text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const string& t = out.text;

        size_t ua = findTag(t, "up_axis", 0);
        size_t cs, ce;
        if (ua != string::npos && content(t, "up_axis", ua, cs, ce))
        {
            out.upAxis = t.substr(cs, ce - cs);
            out.upAxis.erase(std::remove_if(out.upAxis.begin(), out.upAxis.end(), [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }), out.upAxis.end());
        }
        const size_t un = findTag(t, "unit", 0);
        if (un != string::npos)
        {
            const string m = attr(t, un, "meter");
            if (!m.empty()) out.meter = std::atof(m.c_str());
        }

        size_t c = 0;
        while ((c = findTag(t, "controller", c)) != string::npos)
        {
            const size_t cEnd = elemEnd(t, "controller", c);
            if (cEnd == string::npos) break;
            const size_t sk = findTag(t, "skin", c, cEnd);
            if (sk == string::npos) { c = cEnd; continue; }
            SkinnedMesh m;
            m.geometryId = idRef(attr(t, sk, "source"));
            m.name = attr(t, c, "name");

            // bind shape
            m.bindShape = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            const size_t bs = findTag(t, "bind_shape_matrix", sk, cEnd);
            if (bs != string::npos && content(t, "bind_shape_matrix", bs, cs, ce)) readMatrix(t.substr(cs, ce - cs), m.bindShape);

            // joints + inverse binds
            const size_t jn = findTag(t, "joints", sk, cEnd);
            if (jn == string::npos) { c = cEnd; continue; }
            const size_t jnEnd = elemEnd(t, "joints", jn);
            string jSrc, ibSrc;
            int dummy;
            if (!input(t, jn, jnEnd, "JOINT", jSrc, dummy) || !input(t, jn, jnEnd, "INV_BIND_MATRIX", ibSrc, dummy)) { c = cEnd; continue; }
            if (sourceArray(t, jSrc, "Name_array", cs, ce) || sourceArray(t, jSrc, "IDREF_array", cs, ce))
            {
                parseNames(t, cs, ce, m.joints);
                m.jStart = cs;
                m.jEnd = ce;
            }
            std::vector<double> ib;
            if (sourceArray(t, ibSrc, "float_array", cs, ce))
            {
                parseDoubles(t, cs, ce, ib);
                m.ibStart = cs;
                m.ibEnd = ce;
            }
            for (size_t i = 0; i + 15 < ib.size(); i += 16)
            {
                std::array<double, 16> a;
                for (int k = 0; k < 16; ++k) a[k] = ib[i + k];
                m.invBind.push_back(a);
            }

            // vertex weights
            const size_t vw = findTag(t, "vertex_weights", sk, cEnd);
            if (vw == string::npos) { c = cEnd; continue; }
            const size_t vwEnd = elemEnd(t, "vertex_weights", vw);
            string wSrc, jSrc2;
            if (!input(t, vw, vwEnd, "JOINT", jSrc2, m.jointOffset) || !input(t, vw, vwEnd, "WEIGHT", wSrc, m.weightOffset)) { c = cEnd; continue; }
            m.wStride = maxOffset(t, vw, vwEnd) + 1;
            std::vector<double> wvals;
            if (sourceArray(t, wSrc, "float_array", m.wStart, m.wEnd)) parseDoubles(t, m.wStart, m.wEnd, wvals);
            std::vector<int> vcount, v;
            const size_t vc = findTag(t, "vcount", vw, vwEnd);
            const size_t vv = findTag(t, "v", vw, vwEnd);
            if (vc == string::npos || vv == string::npos ||
                !content(t, "vcount", vc, m.vcountStart, m.vcountEnd) || !content(t, "v", vv, m.vStart, m.vEnd)) { c = cEnd; continue; }
            parseInts(t, m.vcountStart, m.vcountEnd, vcount);
            parseInts(t, m.vStart, m.vEnd, v);
            size_t at = 0;
            for (int n : vcount)
            {
                std::vector<std::pair<int, double>> vtx;
                for (int k = 0; k < n; ++k)
                {
                    const size_t base = (at + k) * m.wStride;
                    if (base + std::max(m.jointOffset, m.weightOffset) >= v.size()) break;
                    const int j = v[base + m.jointOffset];
                    const int wi = v[base + m.weightOffset];
                    const double w = (wi >= 0 && wi < (int)wvals.size()) ? wvals[wi] : 0.0;
                    if (j >= 0) vtx.push_back({ j, w });
                }
                m.weights.push_back(vtx);
                at += n;
            }

            // the geometry: positions + triangles
            const size_t g = findById(t, "geometry", m.geometryId);
            if (g == string::npos) { c = cEnd; continue; }
            const size_t gEnd = elemEnd(t, "geometry", g);
            const size_t me = findTag(t, "mesh", g, gEnd);
            if (me == string::npos) { c = cEnd; continue; }
            const size_t meEnd = elemEnd(t, "mesh", me);
            if (m.name.empty()) m.name = attr(t, g, "name");
            const size_t vert = findTag(t, "vertices", me, meEnd);
            string posSrc;
            if (vert == string::npos || !input(t, vert, elemEnd(t, "vertices", vert), "POSITION", posSrc, dummy)) { c = cEnd; continue; }
            if (!sourceArray(t, posSrc, "float_array", m.posStart, m.posEnd)) { c = cEnd; continue; }
            parseDoubles(t, m.posStart, m.posEnd, m.positions);
            readTriangles(t, me, meEnd, m.triangles);

            if (m.weights.size() == m.vertexCount() && m.joints.size() == m.invBind.size() && !m.joints.empty())
            {
                out.meshes.push_back(std::move(m));
            }
            c = cEnd;
        }
        if (out.meshes.empty()) { out.error = "no_rigged_mesh"; return false; }
        return true;
    }

    bool write(const File& f,
               const std::vector<std::vector<double>>& newPositions,
               const std::vector<std::vector<std::vector<std::pair<int, double>>>>& newWeights,
               const std::string& outPath, std::string& error)
    {
        if (newPositions.size() != f.meshes.size()) { error = "count"; return false; }
        std::vector<Patch> patches;
        for (size_t i = 0; i < f.meshes.size(); ++i)
        {
            const SkinnedMesh& m = f.meshes[i];
            const std::vector<double>& np = newPositions[i];
            if (np.size() != m.positions.size()) { error = "count"; return false; }
            string s;
            s.reserve(np.size() * 12);
            for (size_t k = 0; k < np.size(); ++k)
            {
                s += (k == 0) ? "" : " ";
                appendNumber(s, np[k]);
            }
            patches.push_back({ m.posStart, m.posEnd, s });

            if (i < newWeights.size() && !newWeights[i].empty())
            {
                const auto& nw = newWeights[i];
                if (nw.size() != m.vertexCount()) { error = "weights"; return false; }
                // one weight value per influence, written in order
                string wtxt, vctxt, vtxt;
                size_t n = 0;
                for (size_t vi = 0; vi < nw.size(); ++vi)
                {
                    if (vi) vctxt += " ";
                    vctxt += std::to_string(nw[vi].size());
                    for (const auto& jw : nw[vi])
                    {
                        if (n) wtxt += " ";
                        appendNumber(wtxt, jw.second);
                        std::vector<int> slot(m.wStride, 0);
                        slot[m.jointOffset] = jw.first;
                        slot[m.weightOffset] = (int)n;
                        for (int sidx = 0; sidx < m.wStride; ++sidx)
                        {
                            if (!vtxt.empty()) vtxt += " ";
                            vtxt += std::to_string(slot[sidx]);
                        }
                        ++n;
                    }
                }
                patches.push_back({ m.wStart, m.wEnd, wtxt });
                patches.push_back({ m.vcountStart, m.vcountEnd, vctxt });
                patches.push_back({ m.vStart, m.vEnd, vtxt });
                // the WEIGHT array's count attributes: <float_array count> and <accessor count>
                const size_t arrTag = f.text.rfind("<float_array", m.wStart);
                if (arrTag != string::npos)
                {
                    const size_t q = f.text.find("count=\"", arrTag);
                    if (q != string::npos && q < m.wStart)
                    {
                        const size_t qe = f.text.find('"', q + 7);
                        patches.push_back({ q + 7, qe, std::to_string(n) });
                    }
                    const size_t acc = f.text.find("<accessor", m.wEnd);
                    const size_t srcEnd = f.text.find("</source>", m.wEnd);
                    if (acc != string::npos && acc < srcEnd)
                    {
                        const size_t q2 = f.text.find("count=\"", acc);
                        if (q2 != string::npos && q2 < srcEnd)
                        {
                            const size_t qe2 = f.text.find('"', q2 + 7);
                            patches.push_back({ q2 + 7, qe2, std::to_string(n) });
                        }
                    }
                }
            }
        }
        std::sort(patches.begin(), patches.end(), [](const Patch& a, const Patch& b) { return a.start > b.start; });
        string out = f.text;
        for (const Patch& p : patches)
        {
            if (p.start > p.end || p.end > out.size()) { error = "range"; return false; }
            out.replace(p.start, p.end - p.start, p.text);
        }
        const string tmp = outPath + ".tmp";
        {
            std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
            if (!o) { error = "write"; return false; }
            o.write(out.data(), (std::streamsize)out.size());
            if (!o) { error = "write"; return false; }
        }
        std::remove(outPath.c_str());
        if (std::rename(tmp.c_str(), outPath.c_str()) != 0) { error = "rename"; return false; }
        return true;
    }

    // the count="" of the array whose content starts at contentStart, and of the
    // <accessor> after it in the same <source>
    static void countPatches(const string& t, size_t contentStart, size_t contentEnd, size_t count, size_t accessorCount, std::vector<Patch>& patches)
    {
        const size_t arrTag = t.rfind('<', contentStart - 1);
        if (arrTag != string::npos)
        {
            const size_t q = t.find("count=\"", arrTag);
            if (q != string::npos && q < contentStart)
            {
                const size_t qe = t.find('"', q + 7);
                patches.push_back({ q + 7, qe, std::to_string(count) });
            }
        }
        const size_t acc = t.find("<accessor", contentEnd);
        const size_t srcEnd = t.find("</source>", contentEnd);
        if (acc != string::npos && srcEnd != string::npos && acc < srcEnd)
        {
            const size_t q2 = t.find("count=\"", acc);
            if (q2 != string::npos && q2 < srcEnd)
            {
                const size_t qe2 = t.find('"', q2 + 7);
                patches.push_back({ q2 + 7, qe2, std::to_string(accessorCount) });
            }
        }
    }

    bool writeSkinned(const File& f,
                      const std::vector<std::vector<double>>& newPositions,
                      const std::vector<NewSkin>& skins,
                      const std::string& outPath, std::string& error)
    {
        if (newPositions.size() != f.meshes.size()) { error = "count"; return false; }
        std::vector<Patch> patches;
        for (size_t i = 0; i < f.meshes.size(); ++i)
        {
            const SkinnedMesh& m = f.meshes[i];
            const std::vector<double>& np = newPositions[i];
            if (np.size() != m.positions.size()) { error = "count"; return false; }
            string s;
            s.reserve(np.size() * 12);
            for (size_t k = 0; k < np.size(); ++k)
            {
                if (k) s += " ";
                appendNumber(s, np[k]);
            }
            patches.push_back({ m.posStart, m.posEnd, s });
            if (i >= skins.size()) continue;
            const NewSkin& sk = skins[i];
            if (!sk.joints.empty())
            {
                if (sk.invBind.size() != sk.joints.size() || !m.jEnd || !m.ibEnd) { error = "joints"; return false; }
                string jt, it;
                for (size_t k = 0; k < sk.joints.size(); ++k)
                {
                    if (k) jt += " ";
                    jt += sk.joints[k];
                    for (int e = 0; e < 16; ++e)
                    {
                        if (k || e) it += " ";
                        appendNumber(it, sk.invBind[k][e]);
                    }
                }
                patches.push_back({ m.jStart, m.jEnd, jt });
                countPatches(f.text, m.jStart, m.jEnd, sk.joints.size(), sk.joints.size(), patches);
                patches.push_back({ m.ibStart, m.ibEnd, it });
                countPatches(f.text, m.ibStart, m.ibEnd, sk.joints.size() * 16, sk.joints.size(), patches);
            }
            if (!sk.weights.empty())
            {
                const auto& nw = sk.weights;
                if (nw.size() != m.vertexCount()) { error = "weights"; return false; }
                string wtxt, vctxt, vtxt;
                size_t n = 0;
                for (size_t vi = 0; vi < nw.size(); ++vi)
                {
                    if (vi) vctxt += " ";
                    vctxt += std::to_string(nw[vi].size());
                    for (const auto& jw : nw[vi])
                    {
                        if (n) wtxt += " ";
                        appendNumber(wtxt, jw.second);
                        std::vector<int> slot(m.wStride, 0);
                        slot[m.jointOffset] = jw.first;
                        slot[m.weightOffset] = (int)n;
                        for (int sidx = 0; sidx < m.wStride; ++sidx)
                        {
                            if (!vtxt.empty()) vtxt += " ";
                            vtxt += std::to_string(slot[sidx]);
                        }
                        ++n;
                    }
                }
                patches.push_back({ m.wStart, m.wEnd, wtxt });
                countPatches(f.text, m.wStart, m.wEnd, n, n, patches);
                patches.push_back({ m.vcountStart, m.vcountEnd, vctxt });
                patches.push_back({ m.vStart, m.vEnd, vtxt });
            }
        }
        std::sort(patches.begin(), patches.end(), [](const Patch& a, const Patch& b) { return a.start > b.start; });
        for (size_t k = 1; k < patches.size(); ++k)
        {
            if (patches[k].end > patches[k - 1].start) { error = "overlap"; return false; }
        }
        string out = f.text;
        for (const Patch& p : patches)
        {
            if (p.start > p.end || p.end > out.size()) { error = "range"; return false; }
            out.replace(p.start, p.end - p.start, p.text);
        }
        const string tmp = outPath + ".tmp";
        {
            std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
            if (!o) { error = "write"; return false; }
            o.write(out.data(), (std::streamsize)out.size());
            if (!o) { error = "write"; return false; }
        }
        std::remove(outPath.c_str());
        if (std::rename(tmp.c_str(), outPath.c_str()) != 0) { error = "rename"; return false; }
        return true;
    }

    std::string fitPathFor(const std::string& path)
    {
        const size_t dot = path.find_last_of('.');
        const size_t slash = path.find_last_of("/\\");
        if (dot == string::npos || (slash != string::npos && dot < slash)) return path + "_fit.dae";
        const string base = path.substr(0, dot);
        if (base.size() >= 4 && base.compare(base.size() - 4, 4, "_fit") == 0) return path;
        return base + "_fit" + path.substr(dot);
    }
}
