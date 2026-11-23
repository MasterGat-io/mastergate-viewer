/**
 * @file mgmotionread.cpp
 * @brief <MG> BVH and binary FBX motion reader (see mgmotionread.h).
 *
 * FBX: the node tree is read (geometry and embedded media skipped), then the Model hierarchy, the
 * bind pose, the first animation stack's curves (Lcl Translation / Lcl Rotation per model) and each
 * model's transform properties. A local rotation is PreRotation * R(rotation order) * PostRotation^-1
 * (when RotationActive), as the FBX SDK composes it. Everything is sampled at a fixed frame rate.
 */
#ifndef MG_MOTIONREAD_STANDALONE
#include "llviewerprecompiledheaders.h"
#include "zlib-ng/zlib.h"
#else
#include "zlib-ng/zlib.h"
#endif

#include "mgmotionread.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace
{
    // ---------------------------------------------------------------- math
    struct Q { double x = 0, y = 0, z = 0, w = 1; };
    Q qmul(const Q& a, const Q& b)
    {
        return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                 a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
    }
    Q qnorm(Q q)
    {
        const double l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (l < 1e-12) return Q();
        q.x /= l; q.y /= l; q.z /= l; q.w /= l;
        return q;
    }
    Q qconj(const Q& q) { return { -q.x, -q.y, -q.z, q.w }; }
    Q qaxis(int ax, double deg)
    {
        const double h = deg * 3.14159265358979323846 / 360.0, s = std::sin(h);
        Q q; q.w = std::cos(h);
        if (ax == 0) q.x = s; else if (ax == 1) q.y = s; else q.z = s;
        return q;
    }
    void qrot(const Q& q, const double v[3], double o[3])
    {
        const Q p = { v[0], v[1], v[2], 0 };
        const Q r = qmul(qmul(q, p), qconj(q));
        o[0] = r.x; o[1] = r.y; o[2] = r.z;
    }
    Q qslerp(Q a, Q b, double u)
    {
        double d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        if (d < 0) { b = { -b.x, -b.y, -b.z, -b.w }; d = -d; }
        if (d > 0.9995) return qnorm({ a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u, a.w + (b.w - a.w) * u });
        const double th = std::acos(d), s = std::sin(th), wa = std::sin((1 - u) * th) / s, wb = std::sin(u * th) / s;
        return { a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb };
    }
    // FBX rotation orders (EFbxRotationOrder): the first letter turns first
    Q eulerQ(const double e[3], int order)
    {
        const Q X = qaxis(0, e[0]), Y = qaxis(1, e[1]), Z = qaxis(2, e[2]);
        switch (order)
        {
        case 1: return qmul(Y, qmul(Z, X));   // XZY
        case 2: return qmul(X, qmul(Z, Y));   // YZX
        case 3: return qmul(Z, qmul(X, Y));   // YXZ
        case 4: return qmul(Y, qmul(X, Z));   // ZXY
        case 5: return qmul(X, qmul(Y, Z));   // ZYX
        default: return qmul(Z, qmul(Y, X));  // XYZ (and spheric)
        }
    }
    double llclampd(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
    void putQ(std::vector<float>& v, const Q& q) { v.push_back((float)q.x); v.push_back((float)q.y); v.push_back((float)q.z); v.push_back((float)q.w); }

    // ---------------------------------------------------------------- BVH
    bool readBVH(const std::string& data, MGMotion& out, std::string& err, float fps, float maxSecs)
    {
        std::istringstream in(data);
        std::vector<std::string> tok;
        { std::string w; while (in >> w) tok.push_back(w); }
        size_t i = 0;
        auto at = [&](const char* s) { return i < tok.size() && tok[i] == s; };
        if (!at("HIERARCHY")) { err = "format"; return false; }
        ++i;
        struct J { int bone; std::vector<int> ch; };   // channel codes: 0..2 pos x y z, 3..5 rot x y z
        std::vector<J> joints;
        std::vector<int> stack;
        int nch = 0;
        while (i < tok.size() && !at("MOTION"))
        {
            if (at("ROOT") || at("JOINT") || (at("End") && i + 1 < tok.size() && tok[i + 1] == "Site"))
            {
                const bool end = at("End");
                MGMotionBone b;
                b.parent = stack.empty() ? -1 : joints[stack.back()].bone;
                if (end) { b.name = (b.parent >= 0 ? out.bones[b.parent].name : std::string("root")) + "_End"; i += 2; }
                else { b.name = i + 1 < tok.size() ? tok[i + 1] : "joint"; i += 2; }
                if (!at("{")) { err = "format"; return false; }
                ++i;
                out.bones.push_back(b);
                joints.push_back({ (int)out.bones.size() - 1, {} });
                stack.push_back((int)joints.size() - 1);
                continue;
            }
            if (at("OFFSET") && !stack.empty() && i + 3 < tok.size())
            {
                MGMotionBone& b = out.bones[joints[stack.back()].bone];
                for (int c = 0; c < 3; ++c) b.t[c] = (float)atof(tok[i + 1 + c].c_str());
                i += 4;
                continue;
            }
            if (at("CHANNELS") && !stack.empty() && i + 1 < tok.size())
            {
                const int n = atoi(tok[i + 1].c_str());
                i += 2;
                for (int c = 0; c < n && i < tok.size(); ++c, ++i)
                {
                    std::string s = tok[i];
                    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                    int code = -1;
                    if (s == "xposition") code = 0; else if (s == "yposition") code = 1; else if (s == "zposition") code = 2;
                    else if (s == "xrotation") code = 3; else if (s == "yrotation") code = 4; else if (s == "zrotation") code = 5;
                    joints[stack.back()].ch.push_back(code);
                    ++nch;
                }
                continue;
            }
            if (at("}")) { if (!stack.empty()) stack.pop_back(); ++i; continue; }
            ++i;
        }
        if (out.bones.empty()) { err = "no_skeleton"; return false; }
        if (!at("MOTION")) { err = "no_animation"; return false; }
        ++i;
        int nf = 0;
        double dt = 1.0 / 30.0;
        while (i < tok.size())
        {
            if (tok[i] == "Frames:" && i + 1 < tok.size()) { nf = atoi(tok[i + 1].c_str()); i += 2; continue; }
            if (tok[i] == "Frame" && i + 2 < tok.size() && tok[i + 1] == "Time:") { dt = atof(tok[i + 2].c_str()); i += 3; break; }
            ++i;
        }
        if (nf <= 0 || !(dt > 0)) { err = "no_animation"; return false; }
        if ((size_t)nf * (size_t)nch > tok.size() - i) nf = (int)((tok.size() - i) / std::max(1, nch));
        if (nf <= 0) { err = "no_animation"; return false; }
        // per source frame: local rotation and translation of each joint
        const size_t NB = out.bones.size();
        std::vector<Q> R((size_t)nf * NB);
        std::vector<double> T((size_t)nf * NB * 3);
        std::vector<bool> hasPos(NB, false);
        for (int f = 0; f < nf; ++f)
        {
            for (const J& j : joints)
            {
                const MGMotionBone& b = out.bones[j.bone];
                double t[3] = { b.t[0], b.t[1], b.t[2] };
                Q q;
                for (int code : j.ch)
                {
                    const double v = atof(tok[i++].c_str());
                    if (code >= 0 && code < 3) { t[code] = v; hasPos[j.bone] = true; }
                    else if (code >= 3) q = qmul(q, qaxis(code - 3, v));
                }
                R[(size_t)f * NB + j.bone] = qnorm(q);
                for (int c = 0; c < 3; ++c) T[((size_t)f * NB + j.bone) * 3 + c] = t[c];
            }
        }
        const double dur = std::min((double)maxSecs, (nf - 1) * dt);
        const int FO = std::max(1, (int)std::floor(dur * fps + 1e-6) + 1);
        out.fps = fps; out.frames = FO; out.kind = "bvh";
        for (size_t b = 0; b < NB; ++b)
        {
            bool anim = false;
            for (const J& j : joints) if (j.bone == (int)b && !j.ch.empty()) anim = true;
            if (!anim) continue;
            MGMotionBone& mb = out.bones[b];
            for (int f = 0; f < FO; ++f)
            {
                const double s = f / (double)fps / dt;
                const int f0 = std::min(nf - 1, (int)std::floor(s)), f1 = std::min(nf - 1, f0 + 1);
                const double u = s - f0;
                putQ(mb.rot, qnorm(qslerp(R[(size_t)f0 * NB + b], R[(size_t)f1 * NB + b], u)));
                if (hasPos[b])
                    for (int c = 0; c < 3; ++c)
                        mb.pos.push_back((float)(T[((size_t)f0 * NB + b) * 3 + c] * (1 - u) + T[((size_t)f1 * NB + b) * 3 + c] * u));
            }
        }
        return true;
    }

    // ---------------------------------------------------------------- FBX node tree
    struct FProp { char t = 0; int64_t i = 0; double d = 0; std::string s; std::vector<double> ad; std::vector<int64_t> al; };
    struct FNode
    {
        std::string name;
        std::vector<FProp> p;
        std::vector<FNode> c;
        const FNode* child(const char* n) const { for (const FNode& k : c) if (k.name == n) return &k; return nullptr; }
    };

    struct FbxParser
    {
        const uint8_t* b = nullptr;
        size_t n = 0;
        uint32_t ver = 0;
        bool bad = false;

        template <typename T> T rd(size_t& o) { T v{}; if (o + sizeof(T) > n) { bad = true; return v; } memcpy(&v, b + o, sizeof(T)); o += sizeof(T); return v; }

        bool array(size_t& o, char t, FProp& p)
        {
            const uint32_t len = rd<uint32_t>(o), enc = rd<uint32_t>(o), clen = rd<uint32_t>(o);
            if (bad || o + clen > n) { bad = true; return false; }
            const size_t es = (t == 'd' || t == 'l') ? 8 : (t == 'b') ? 1 : 4;
            std::vector<uint8_t> raw;
            const uint8_t* src = b + o;
            if (enc == 1)
            {
                raw.resize((size_t)len * es);
                uLongf dl = (uLongf)raw.size();
                if (uncompress(raw.data(), &dl, src, clen) != Z_OK || dl != raw.size()) { o += clen; return false; }
                src = raw.data();
            }
            else if (clen < (size_t)len * es) { bad = true; return false; }
            if (t == 'd') { p.ad.resize(len); for (uint32_t k = 0; k < len; ++k) { double v; memcpy(&v, src + k * 8, 8); p.ad[k] = v; } }
            else if (t == 'f') { p.ad.resize(len); for (uint32_t k = 0; k < len; ++k) { float v; memcpy(&v, src + k * 4, 4); p.ad[k] = v; } }
            else if (t == 'l') { p.al.resize(len); for (uint32_t k = 0; k < len; ++k) { int64_t v; memcpy(&v, src + k * 8, 8); p.al[k] = v; } }
            else if (t == 'i') { p.al.resize(len); for (uint32_t k = 0; k < len; ++k) { int32_t v; memcpy(&v, src + k * 4, 4); p.al[k] = v; } }
            o += clen;
            return true;
        }

        // one node record; false at the list's end (null record) or on error
        bool node(size_t& o, FNode& out)
        {
            const bool big = ver >= 7500;
            const uint64_t end = big ? rd<uint64_t>(o) : rd<uint32_t>(o);
            const uint64_t np = big ? rd<uint64_t>(o) : rd<uint32_t>(o);
            const uint64_t plen = big ? rd<uint64_t>(o) : rd<uint32_t>(o);
            const uint8_t nl = rd<uint8_t>(o);
            if (bad) return false;
            if (end == 0) return false;   // null record: end of a list
            if (end > n || o + nl > n) { bad = true; return false; }
            out.name.assign((const char*)b + o, nl);
            o += nl;
            // the meshes and embedded media are not needed (and are most of a big file)
            if (out.name == "Geometry" || out.name == "Video" || out.name == "Content") { o = (size_t)end; return true; }
            const size_t pend = o + (size_t)plen;
            for (uint64_t k = 0; k < np && o < pend && !bad; ++k)
            {
                FProp p;
                p.t = (char)rd<uint8_t>(o);
                switch (p.t)
                {
                case 'Y': p.i = rd<int16_t>(o); p.d = (double)p.i; break;
                case 'C': p.i = rd<uint8_t>(o); p.d = (double)p.i; break;
                case 'I': p.i = rd<int32_t>(o); p.d = (double)p.i; break;
                case 'F': p.d = rd<float>(o); break;
                case 'D': p.d = rd<double>(o); break;
                case 'L': p.i = rd<int64_t>(o); p.d = (double)p.i; break;
                case 'S': case 'R': { const uint32_t l = rd<uint32_t>(o); if (o + l > n) { bad = true; break; } if (p.t == 'S') p.s.assign((const char*)b + o, l); o += l; break; }
                case 'f': case 'd': case 'l': case 'i': case 'b': array(o, p.t, p); break;
                default: bad = true; break;
                }
                out.p.push_back(std::move(p));
            }
            o = pend;
            while (o < end && !bad)
            {
                FNode k;
                if (!node(o, k)) break;
                out.c.push_back(std::move(k));
            }
            o = (size_t)end;
            return !bad;
        }
    };

    std::string objName(const std::string& s)
    {
        const size_t k = s.find(std::string("\0\x01", 2));
        std::string r = k == std::string::npos ? s : s.substr(0, k);
        if (r.compare(0, 7, "Model::") == 0) r = r.substr(7);
        return r;
    }

    struct Props
    {
        std::map<std::string, std::vector<double>> v;
        void read(const FNode* p70)
        {
            if (!p70) return;
            for (const FNode& p : p70->c)
            {
                if (p.name != "P" || p.p.empty()) continue;
                std::vector<double> vals;
                for (size_t k = 4; k < p.p.size(); ++k) if (p.p[k].t != 'S') vals.push_back(p.p[k].d);
                v[p.p[0].s] = vals;
            }
        }
        void vec(const char* k, double o[3], double def) const
        {
            auto it = v.find(k);
            for (int c = 0; c < 3; ++c) o[c] = (it != v.end() && it->second.size() > (size_t)c) ? it->second[c] : def;
        }
        double num(const char* k, double def) const { auto it = v.find(k); return it != v.end() && !it->second.empty() ? it->second[0] : def; }
    };

    struct Curve { std::vector<double> t, v; };
    double evalCurve(const Curve& c, double t)
    {
        if (c.t.empty()) return 0;
        if (t <= c.t.front()) return c.v.front();
        if (t >= c.t.back()) return c.v.back();
        const size_t k = std::upper_bound(c.t.begin(), c.t.end(), t) - c.t.begin();
        const double a = c.t[k - 1], b = c.t[k], u = b > a ? (t - a) / (b - a) : 0;
        return c.v[k - 1] + (c.v[k] - c.v[k - 1]) * u;
    }

    struct CurveNode { double def[3] = { 0, 0, 0 }; const Curve* ch[3] = { nullptr, nullptr, nullptr }; };

    struct Model
    {
        int64_t id = 0;
        std::string name, type;
        Props props;
        int64_t parent = 0;
        const CurveNode* T = nullptr;
        const CurveNode* R = nullptr;
        bool keep = false;
        bool hasBind = false;
        double bind[16] = { 0 };
    };

    // the model's local transform at time t (anim = use the curves)
    void localAt(const Model& m, double t, bool anim, Q& q, double tr[3], double& sc)
    {
        double lt[3], lr[3], ls[3], pre[3], post[3], roff[3], rpiv[3];
        m.props.vec("Lcl Translation", lt, 0); m.props.vec("Lcl Rotation", lr, 0); m.props.vec("Lcl Scaling", ls, 1);
        m.props.vec("PreRotation", pre, 0); m.props.vec("PostRotation", post, 0);
        m.props.vec("RotationOffset", roff, 0); m.props.vec("RotationPivot", rpiv, 0);
        if (anim && m.T) for (int c = 0; c < 3; ++c) lt[c] = m.T->ch[c] ? evalCurve(*m.T->ch[c], t) : m.T->def[c];
        if (anim && m.R) for (int c = 0; c < 3; ++c) lr[c] = m.R->ch[c] ? evalCurve(*m.R->ch[c], t) : m.R->def[c];
        const int order = (int)m.props.num("RotationOrder", 0);
        const bool active = m.props.num("RotationActive", 0) != 0;
        Q r = eulerQ(lr, order);
        if (active) r = qmul(qmul(eulerQ(pre, 0), r), qconj(eulerQ(post, 0)));
        q = qnorm(r);
        double pr[3];
        qrot(q, rpiv, pr);
        for (int c = 0; c < 3; ++c) tr[c] = lt[c] + (active ? roff[c] + rpiv[c] - pr[c] : 0);
        sc = (std::fabs(ls[0]) + std::fabs(ls[1]) + std::fabs(ls[2])) / 3.0;
    }

    // ASCII FBX (7.x): Name: value, value, ... { children }  ·  arrays *N { a: v,v,... }  ·  ; comments
    struct FbxAscii
    {
        const std::string& s;
        size_t i = 0;
        explicit FbxAscii(const std::string& d) : s(d) {}
        void ws()
        {
            while (i < s.size())
            {
                const char c = s[i];
                if (c == ';') { while (i < s.size() && s[i] != '\n') ++i; }
                else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++i;
                else break;
            }
        }
        void sp() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) ++i; }
        bool number(FProp& p)
        {
            const char* b = s.c_str() + i;
            char* e = nullptr;
            const double d = strtod(b, &e);
            if (e == b) return false;
            bool integer = true;
            for (const char* q = b; q < e; ++q) if (*q == '.' || *q == 'e' || *q == 'E') integer = false;
            p.t = integer ? 'L' : 'D';
            p.d = d;
            p.i = integer ? strtoll(b, nullptr, 10) : (int64_t)d;
            i += (size_t)(e - b);
            return true;
        }
        bool value(FProp& p)
        {
            sp();
            if (i >= s.size()) return false;
            const char c = s[i];
            if (c == '"')
            {
                const size_t e = s.find('"', i + 1);
                if (e == std::string::npos) return false;
                p.t = 'S'; p.s = s.substr(i + 1, e - i - 1); i = e + 1;
                return true;
            }
            if (c == '*')
            {
                // *N { a: v, v, ... }
                ++i;
                while (i < s.size() && isdigit((unsigned char)s[i])) ++i;
                ws();
                if (i >= s.size() || s[i] != '{') return false;
                ++i; ws();
                if (s.compare(i, 2, "a:") == 0) i += 2;
                p.t = 'd';
                while (i < s.size())
                {
                    ws();
                    if (s[i] == '}') { ++i; break; }
                    if (s[i] == ',') { ++i; continue; }
                    FProp n;
                    if (!number(n)) return false;
                    p.ad.push_back(n.d);
                    p.al.push_back(n.i);
                }
                return true;
            }
            if (c == '-' || c == '+' || c == '.' || isdigit((unsigned char)c)) return number(p);
            // a bare word (Y, N, T, W...)
            const size_t b = i;
            while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '_')) ++i;
            if (i == b) return false;
            p.t = 'S'; p.s = s.substr(b, i - b);
            return true;
        }
        bool node(FNode& n)
        {
            const size_t b = i;
            while (i < s.size() && s[i] != ':' && s[i] != '\n' && s[i] != '{' && s[i] != '}') ++i;
            if (i >= s.size() || s[i] != ':') return false;
            n.name = s.substr(b, i - b);
            while (!n.name.empty() && (n.name.back() == ' ' || n.name.back() == '\t')) n.name.pop_back();
            ++i;
            sp();
            // values on the same line (a comma lets them go on)
            while (i < s.size() && s[i] != '\n' && s[i] != '{' && s[i] != '}')
            {
                FProp p;
                if (!value(p)) break;
                n.p.push_back(std::move(p));
                sp();
                if (i < s.size() && s[i] == ',') { ++i; ws(); continue; }
                break;
            }
            sp();
            if (i < s.size() && s[i] == '{') { ++i; if (!list(n.c, false)) return false; }
            return true;
        }
        bool list(std::vector<FNode>& out, bool top)
        {
            while (true)
            {
                ws();
                if (i >= s.size()) return top;
                if (s[i] == '}') { ++i; return !top; }
                FNode n;
                if (!node(n)) { while (i < s.size() && s[i] != '\n') ++i; continue; }
                if (n.name == "Geometry" || n.name == "Video" || n.name == "Content") { n.c.clear(); n.p.clear(); }
                out.push_back(std::move(n));
            }
        }
    };

    bool readFBX(const std::string& data, MGMotion& out, std::string& err, float fps, float maxSecs)
    {
        std::vector<FNode> top;
        if (data.size() >= 27 && data.compare(0, 18, "Kaydara FBX Binary") == 0)
        {
            FbxParser P;
            P.b = (const uint8_t*)data.data();
            P.n = data.size();
            memcpy(&P.ver, P.b + 23, 4);
            if (P.ver < 7000) { err = "old_fbx"; return false; }
            size_t o = 27;
            while (o < P.n && !P.bad)
            {
                FNode k;
                if (!P.node(o, k)) break;
                top.push_back(std::move(k));
            }
        }
        else if (data.find("FBXHeaderExtension") != std::string::npos)
        {
            // FBX 6 and older keep names, not ids, in their connections
            const size_t v = data.find("FBXVersion:");
            if (v != std::string::npos && atoi(data.c_str() + v + 11) < 7000) { err = "old_fbx"; return false; }
            FbxAscii A(data);
            A.list(top, true);
        }
        else { err = "format"; return false; }
        const FNode *objects = nullptr, *conns = nullptr, *gs = nullptr, *defs = nullptr;
        for (const FNode& k : top)
        {
            if (k.name == "Objects") objects = &k;
            else if (k.name == "Connections") conns = &k;
            else if (k.name == "GlobalSettings") gs = &k;
            else if (k.name == "Definitions") defs = &k;
        }
        if (!objects || !conns) { err = "format"; return false; }
        if (gs) { Props g; g.read(gs->child("Properties70")); out.unit = (float)(g.num("UnitScaleFactor", 1.0) / 100.0); }
        // property defaults of Model (the file's template)
        Props modelDefaults;
        if (defs)
            for (const FNode& ot : defs->c)
                if (ot.name == "ObjectType" && !ot.p.empty() && ot.p[0].s == "Model")
                    for (const FNode& pt : ot.c)
                        if (pt.name == "PropertyTemplate") modelDefaults.read(pt.child("Properties70"));

        std::map<int64_t, Model> models;
        std::vector<int64_t> modelOrder;
        std::map<int64_t, Curve> curves;
        std::map<int64_t, CurveNode> cnodes;
        std::map<int64_t, std::string> cnodeKind;
        std::vector<std::pair<int64_t, std::string>> stacks;
        std::set<int64_t> layers;
        for (const FNode& ob : objects->c)
        {
            if (ob.p.empty()) continue;
            const int64_t id = ob.p[0].i;
            if (ob.name == "Model" && ob.p.size() >= 3)
            {
                Model m;
                m.id = id; m.name = objName(ob.p[1].s); m.type = ob.p[2].s;
                m.props = modelDefaults;
                m.props.read(ob.child("Properties70"));
                models[id] = m;
                modelOrder.push_back(id);
            }
            else if (ob.name == "AnimationCurve")
            {
                Curve c;
                const FNode* kt = ob.child("KeyTime");
                const FNode* kv = ob.child("KeyValueFloat");
                if (!kv) kv = ob.child("KeyValueDouble");
                if (kt && kv && !kt->p.empty() && !kv->p.empty())
                {
                    for (int64_t v : kt->p[0].al) c.t.push_back((double)v / 46186158000.0);
                    c.v = kv->p[0].ad;
                    if (c.v.size() > c.t.size()) c.v.resize(c.t.size());
                    if (c.t.size() > c.v.size()) c.t.resize(c.v.size());
                }
                curves[id] = c;
            }
            else if (ob.name == "AnimationCurveNode")
            {
                CurveNode cn;
                Props p; p.read(ob.child("Properties70"));
                cn.def[0] = p.num("d|X", 0); cn.def[1] = p.num("d|Y", 0); cn.def[2] = p.num("d|Z", 0);
                cnodes[id] = cn;
                cnodeKind[id] = ob.p.size() > 1 ? objName(ob.p[1].s) : "";
            }
            else if (ob.name == "AnimationStack") stacks.push_back({ id, ob.p.size() > 1 ? objName(ob.p[1].s) : "" });
            else if (ob.name == "AnimationLayer") layers.insert(id);
        }
        // the bind pose
        for (const FNode& ob : objects->c)
        {
            if (ob.name != "Pose" || ob.p.size() < 3 || ob.p[2].s != "BindPose") continue;
            for (const FNode& pn : ob.c)
            {
                if (pn.name != "PoseNode") continue;
                const FNode* nd = pn.child("Node");
                const FNode* mx = pn.child("Matrix");
                if (!nd || !mx || nd->p.empty() || mx->p.empty() || mx->p[0].ad.size() != 16) continue;
                auto it = models.find(nd->p[0].i);
                if (it == models.end() || it->second.hasBind) continue;
                it->second.hasBind = true;
                for (int k = 0; k < 16; ++k) it->second.bind[k] = mx->p[0].ad[k];
            }
        }
        // connections: model parents, curves -> curve nodes -> models, curve nodes -> layers -> stacks
        std::map<int64_t, int64_t> layerStack;
        std::map<int64_t, int64_t> cnodeLayer;
        struct OP { int64_t cn; int64_t model; std::string prop; };
        std::vector<OP> cnModel;
        for (const FNode& cc : conns->c)
        {
            if (cc.name != "C" || cc.p.size() < 3) continue;
            const std::string& ty = cc.p[0].s;
            const int64_t a = cc.p[1].i, b = cc.p[2].i;
            const std::string prop = cc.p.size() > 3 ? cc.p[3].s : "";
            if (ty == "OO")
            {
                if (models.count(a) && (b == 0 || models.count(b))) models[a].parent = b;
                else if (layers.count(a)) layerStack[a] = b;
                else if (cnodes.count(a) && layers.count(b)) cnodeLayer[a] = b;
            }
            else if (ty == "OP")
            {
                if (curves.count(a) && cnodes.count(b))
                {
                    const int ch = prop == "d|X" ? 0 : prop == "d|Y" ? 1 : prop == "d|Z" ? 2 : -1;
                    if (ch >= 0) cnodes[b].ch[ch] = &curves[a];
                }
                else if (cnodes.count(a) && models.count(b)) cnModel.push_back({ a, b, prop });
            }
        }
        // the skeleton: limb nodes and every model above them
        bool anyLimb = false;
        for (auto& kv : models)
        {
            const Model& m = kv.second;
            if (m.type != "LimbNode" && m.type != "Root") continue;
            anyLimb = true;
            int64_t id = m.id;
            int guard = 0;
            while (id && models.count(id) && guard++ < 512) { models[id].keep = true; id = models[id].parent; }
        }
        if (!anyLimb) { err = "no_skeleton"; return false; }
        // the take: the first stack whose curves move the skeleton
        int64_t useStack = 0;
        out.takes = (int)stacks.size();
        for (const auto& st : stacks)
        {
            for (const OP& op : cnModel)
            {
                auto l = cnodeLayer.find(op.cn);
                if (l == cnodeLayer.end()) continue;
                auto s = layerStack.find(l->second);
                if (s != layerStack.end() && s->second == st.first && models[op.model].keep) { useStack = st.first; out.take = st.second; break; }
            }
            if (useStack) break;
        }
        double t0 = 1e30, t1 = -1e30;
        for (const OP& op : cnModel)
        {
            Model& m = models[op.model];
            if (!m.keep) continue;
            if (useStack)
            {
                auto l = cnodeLayer.find(op.cn);
                if (l == cnodeLayer.end()) continue;
                auto s = layerStack.find(l->second);
                if (s == layerStack.end() || s->second != useStack) continue;
            }
            const CurveNode* cn = &cnodes[op.cn];
            if (op.prop == "Lcl Translation") m.T = cn;
            else if (op.prop == "Lcl Rotation") m.R = cn;
            else continue;
            for (int c = 0; c < 3; ++c)
                if (cn->ch[c] && !cn->ch[c]->t.empty()) { t0 = std::min(t0, cn->ch[c]->t.front()); t1 = std::max(t1, cn->ch[c]->t.back()); }
        }
        if (t1 < t0) { t0 = 0; t1 = 0; }
        const double dur = std::min((double)maxSecs, t1 - t0);
        const int FO = std::max(1, (int)std::floor(dur * fps + 1e-6) + 1);
        // bones, parents first, in the file's order
        std::map<int64_t, int> index;
        std::function<void(int64_t, int)> add = [&](int64_t id, int parent)
        {
            const Model& m = models[id];
            MGMotionBone b;
            b.name = m.name;
            b.parent = parent;
            Q q; double tr[3], sc;
            localAt(m, t0, false, q, tr, sc);
            for (int c = 0; c < 3; ++c) b.t[c] = (float)tr[c];
            b.r[0] = (float)q.x; b.r[1] = (float)q.y; b.r[2] = (float)q.z; b.r[3] = (float)q.w;
            b.s = (float)sc;
            {
                // the "zero" pose: rotation channels at 0 (Maya-style rigs rest there, the joint orient in PreRotation)
                double pre[3], post[3];
                m.props.vec("PreRotation", pre, 0); m.props.vec("PostRotation", post, 0);
                Q z;
                if (m.props.num("RotationActive", 0) != 0) z = qnorm(qmul(eulerQ(pre, 0), qconj(eulerQ(post, 0))));
                b.rz[0] = (float)z.x; b.rz[1] = (float)z.y; b.rz[2] = (float)z.z; b.rz[3] = (float)z.w;
            }
            b.hasBind = m.hasBind;
            for (int k = 0; k < 16; ++k) b.bind[k] = (float)m.bind[k];
            if (m.R || m.T)
                for (int f = 0; f < FO; ++f)
                {
                    localAt(m, t0 + f / (double)fps, true, q, tr, sc);
                    if (m.R || m.T) putQ(b.rot, q);
                    if (m.T) for (int c = 0; c < 3; ++c) b.pos.push_back((float)tr[c]);
                }
            out.bones.push_back(b);
            const int me = (int)out.bones.size() - 1;
            index[id] = me;
            for (int64_t cid : modelOrder) if (models[cid].keep && models[cid].parent == id) add(cid, me);
        };
        for (int64_t id : modelOrder) if (models[id].keep && (models[id].parent == 0 || !models.count(models[id].parent))) add(id, -1);
        out.fps = fps;
        out.frames = FO;
        out.kind = "fbx";
        if (t1 <= t0) { err = "no_animation"; return false; }
        return true;
    }
}

namespace
{
    struct XEl { std::string name, text; std::map<std::string, std::string> at; std::vector<XEl> kids;
        const XEl* kid(const char* n) const { for (const XEl& k : kids) if (k.name == n) return &k; return nullptr; } };
    // a small XML reader (elements, attributes, text; enough for COLLADA)
    struct XReader
    {
        const std::string& s; size_t i = 0;
        explicit XReader(const std::string& d) : s(d) {}
        static std::string local(const std::string& n) { const size_t c = n.find(':'); return c == std::string::npos ? n : n.substr(c + 1); }
        bool parse(XEl& root)
        {
            std::vector<XEl*> st;
            XEl doc;
            st.push_back(&doc);
            while (i < s.size())
            {
                const size_t lt = s.find('<', i);
                if (lt == std::string::npos) break;
                if (lt > i && st.size() > 1) st.back()->text.append(s, i, lt - i);
                i = lt;
                if (s.compare(i, 4, "<!--") == 0) { const size_t e = s.find("-->", i); i = e == std::string::npos ? s.size() : e + 3; continue; }
                if (s.compare(i, 2, "<?") == 0 || s.compare(i, 2, "<!") == 0) { const size_t e = s.find('>', i); i = e == std::string::npos ? s.size() : e + 1; continue; }
                if (s.compare(i, 2, "</") == 0) { const size_t e = s.find('>', i); i = e == std::string::npos ? s.size() : e + 1; if (st.size() > 1) st.pop_back(); continue; }
                const size_t e = s.find('>', i);
                if (e == std::string::npos) break;
                const bool selfClose = s[e - 1] == '/';
                std::string tag = s.substr(i + 1, e - i - 1 - (selfClose ? 1 : 0));
                i = e + 1;
                XEl el;
                size_t k = 0;
                while (k < tag.size() && !isspace((unsigned char)tag[k])) ++k;
                el.name = local(tag.substr(0, k));
                while (k < tag.size())
                {
                    while (k < tag.size() && isspace((unsigned char)tag[k])) ++k;
                    const size_t eq = tag.find('=', k);
                    if (eq == std::string::npos) break;
                    std::string an = tag.substr(k, eq - k);
                    while (!an.empty() && isspace((unsigned char)an.back())) an.pop_back();
                    const size_t q1 = tag.find_first_of("\"'", eq);
                    if (q1 == std::string::npos) break;
                    const size_t q2 = tag.find(tag[q1], q1 + 1);
                    if (q2 == std::string::npos) break;
                    el.at[an] = tag.substr(q1 + 1, q2 - q1 - 1);
                    k = q2 + 1;
                }
                st.back()->kids.push_back(std::move(el));
                if (!selfClose) st.push_back(&st.back()->kids.back());
            }
            if (doc.kids.empty()) return false;
            root = std::move(doc.kids.front());
            return true;
        }
    };
    void floats(const std::string& t, std::vector<double>& out) { const char* p = t.c_str(); char* e; while (*p) { const double d = strtod(p, &e); if (e == p) { ++p; continue; } out.push_back(d); p = e; } }
    // a 4x4 row-major COLLADA matrix -> translation, rotation (Hamilton, column vectors), uniform scale
    void decompose(const double* m, double t[3], Q& q, double& sc)
    {
        t[0] = m[3]; t[1] = m[7]; t[2] = m[11];
        double c[3][3];
        double l[3];
        for (int col = 0; col < 3; ++col)
        {
            l[col] = std::sqrt(m[col] * m[col] + m[4 + col] * m[4 + col] + m[8 + col] * m[8 + col]);
            if (l[col] < 1e-12) l[col] = 1;
            for (int r = 0; r < 3; ++r) c[r][col] = m[r * 4 + col] / l[col];
        }
        sc = (l[0] + l[1] + l[2]) / 3.0;
        const double tr = c[0][0] + c[1][1] + c[2][2];
        if (tr > 0) { const double S = std::sqrt(tr + 1) * 2; q = { (c[2][1] - c[1][2]) / S, (c[0][2] - c[2][0]) / S, (c[1][0] - c[0][1]) / S, 0.25 * S }; }
        else if (c[0][0] > c[1][1] && c[0][0] > c[2][2]) { const double S = std::sqrt(1 + c[0][0] - c[1][1] - c[2][2]) * 2; q = { 0.25 * S, (c[0][1] + c[1][0]) / S, (c[0][2] + c[2][0]) / S, (c[2][1] - c[1][2]) / S }; }
        else if (c[1][1] > c[2][2]) { const double S = std::sqrt(1 + c[1][1] - c[0][0] - c[2][2]) * 2; q = { (c[0][1] + c[1][0]) / S, 0.25 * S, (c[1][2] + c[2][1]) / S, (c[0][2] - c[2][0]) / S }; }
        else { const double S = std::sqrt(1 + c[2][2] - c[0][0] - c[1][1]) * 2; q = { (c[0][2] + c[2][0]) / S, (c[1][2] + c[2][1]) / S, 0.25 * S, (c[1][0] - c[0][1]) / S }; }
        q = qnorm(q);
    }
    // the node's own transform (matrix, or translate / rotate / scale in order)
    void nodeMatrix(const XEl& n, double m[16])
    {
        for (int k = 0; k < 16; ++k) m[k] = (k % 5 == 0) ? 1 : 0;
        auto mul = [&](const double b[16]) { double r[16]; for (int a = 0; a < 4; ++a) for (int c = 0; c < 4; ++c) { r[a * 4 + c] = 0; for (int k = 0; k < 4; ++k) r[a * 4 + c] += m[a * 4 + k] * b[k * 4 + c]; } memcpy(m, r, sizeof(r)); };
        for (const XEl& k : n.kids)
        {
            std::vector<double> v;
            if (k.name == "matrix") { floats(k.text, v); if (v.size() >= 16) mul(v.data()); }
            else if (k.name == "translate") { floats(k.text, v); if (v.size() >= 3) { double b[16] = { 1,0,0,v[0], 0,1,0,v[1], 0,0,1,v[2], 0,0,0,1 }; mul(b); } }
            else if (k.name == "scale") { floats(k.text, v); if (v.size() >= 3) { double b[16] = { v[0],0,0,0, 0,v[1],0,0, 0,0,v[2],0, 0,0,0,1 }; mul(b); } }
            else if (k.name == "rotate")
            {
                floats(k.text, v);
                if (v.size() >= 4)
                {
                    double x = v[0], y = v[1], z = v[2]; const double L = std::sqrt(x * x + y * y + z * z); if (L < 1e-12) continue; x /= L; y /= L; z /= L;
                    const double a = v[3] * 3.14159265358979323846 / 180.0, c = std::cos(a), s2 = std::sin(a), C = 1 - c;
                    double b[16] = { x * x * C + c, x * y * C - z * s2, x * z * C + y * s2, 0,  y * x * C + z * s2, y * y * C + c, y * z * C - x * s2, 0,  z * x * C - y * s2, z * y * C + x * s2, z * z * C + c, 0,  0, 0, 0, 1 };
                    mul(b);
                }
            }
        }
    }
    bool readDAE(const std::string& data, MGMotion& out, std::string& err, float fps, float maxSecs)
    {
        XEl root;
        XReader X(data);
        if (!X.parse(root) || root.name != "COLLADA") { err = "format"; return false; }
        // the scene's nodes
        struct DNode { const XEl* el; int parent; std::string id, name; bool joint; double m[16]; };
        std::vector<DNode> nodes;
        std::function<void(const XEl&, int)> walk = [&](const XEl& n, int parent)
        {
            DNode d; d.el = &n; d.parent = parent;
            auto get = [&](const char* k) { auto it = n.at.find(k); return it == n.at.end() ? std::string() : it->second; };
            d.id = get("id"); d.name = get("sid").empty() ? (get("name").empty() ? d.id : get("name")) : get("sid");
            d.joint = get("type") == "JOINT";
            nodeMatrix(n, d.m);
            nodes.push_back(d);
            const int me = (int)nodes.size() - 1;
            for (const XEl& k : n.kids) if (k.name == "node") walk(k, me);
        };
        const XEl* lvs = root.kid("library_visual_scenes");
        if (!lvs) { err = "no_skeleton"; return false; }
        for (const XEl& vs : lvs->kids) if (vs.name == "visual_scene") for (const XEl& n : vs.kids) if (n.name == "node") walk(n, -1);
        // keep the joints and the nodes above them
        std::vector<bool> keep(nodes.size(), false);
        bool any = false;
        for (size_t k = 0; k < nodes.size(); ++k) if (nodes[k].joint) { any = true; for (int q = (int)k; q >= 0 && !keep[q]; q = nodes[q].parent) keep[q] = true; }
        if (!any) { err = "no_skeleton"; return false; }
        // the animations: channel target "nodeId/transform" (or the matrix's sid) -> sampler -> input times, output 16 floats a key
        std::map<std::string, const XEl*> sources;
        std::function<void(const XEl&)> srcs = [&](const XEl& e) { for (const XEl& k : e.kids) { if (k.name == "source") { auto it = k.at.find("id"); if (it != k.at.end()) sources[it->second] = &k; } srcs(k); } };
        const XEl* la = root.kid("library_animations");
        struct Track { std::vector<double> t, m; };
        std::map<std::string, Track> tracks;   // node id -> matrix track
        double t0 = 1e30, t1 = -1e30;
        if (la)
        {
            srcs(*la);
            std::function<void(const XEl&)> anims = [&](const XEl& a)
            {
                std::map<std::string, const XEl*> samplers;
                for (const XEl& k : a.kids) if (k.name == "sampler") { auto it = k.at.find("id"); if (it != k.at.end()) samplers[it->second] = &k; }
                for (const XEl& ch : a.kids)
                {
                    if (ch.name == "animation") { anims(ch); continue; }
                    if (ch.name != "channel") continue;
                    std::string src = ch.at.count("source") ? ch.at.at("source") : "", tgt = ch.at.count("target") ? ch.at.at("target") : "";
                    if (!src.empty() && src[0] == '#') src = src.substr(1);
                    const size_t sl = tgt.find('/');
                    if (sl == std::string::npos || !samplers.count(src)) continue;
                    const std::string nodeId = tgt.substr(0, sl), what = tgt.substr(sl + 1);
                    if (what.find('.') != std::string::npos) continue;   // single channels (rotateX.ANGLE): not handled
                    Track tr;
                    for (const XEl& in : samplers[src]->kids)
                    {
                        if (in.name != "input") continue;
                        std::string sid = in.at.count("source") ? in.at.at("source") : "";
                        if (!sid.empty() && sid[0] == '#') sid = sid.substr(1);
                        const XEl* so = sources.count(sid) ? sources[sid] : nullptr;
                        const XEl* fa = so ? so->kid("float_array") : nullptr;
                        if (!fa) continue;
                        const std::string sem = in.at.count("semantic") ? in.at.at("semantic") : "";
                        if (sem == "INPUT") floats(fa->text, tr.t); else if (sem == "OUTPUT") floats(fa->text, tr.m);
                    }
                    if (tr.t.empty() || tr.m.size() < tr.t.size() * 16) continue;
                    t0 = std::min(t0, tr.t.front()); t1 = std::max(t1, tr.t.back());
                    tracks[nodeId] = tr;
                }
            };
            anims(*la);
        }
        if (tracks.empty() || t1 <= t0) { err = "no_animation"; return false; }
        const double dur = std::min((double)maxSecs, t1 - t0);
        const int FO = std::max(1, (int)std::floor(dur * fps + 1e-6) + 1);
        out.fps = fps; out.frames = FO; out.kind = "dae";
        std::vector<int> index(nodes.size(), -1);
        for (size_t k = 0; k < nodes.size(); ++k)
        {
            if (!keep[k]) continue;
            const DNode& d = nodes[k];
            MGMotionBone b;
            b.name = d.name;
            b.parent = d.parent >= 0 ? index[d.parent] : -1;
            double tt[3], sc; Q q;
            decompose(d.m, tt, q, sc);
            for (int c = 0; c < 3; ++c) b.t[c] = (float)tt[c];
            b.r[0] = (float)q.x; b.r[1] = (float)q.y; b.r[2] = (float)q.z; b.r[3] = (float)q.w;
            b.s = (float)sc;
            auto it = tracks.find(d.id);
            if (it != tracks.end())
            {
                const Track& tr = it->second;
                for (int f = 0; f < FO; ++f)
                {
                    const double tm = t0 + f / (double)fps;
                    size_t a = 0;
                    while (a + 1 < tr.t.size() && tr.t[a + 1] <= tm) ++a;
                    const size_t c2 = std::min(a + 1, tr.t.size() - 1);
                    const double u = c2 > a && tr.t[c2] > tr.t[a] ? llclampd((tm - tr.t[a]) / (tr.t[c2] - tr.t[a])) : 0.0;
                    double ta[3], tb[3], sa, sb; Q qa, qb;
                    decompose(&tr.m[a * 16], ta, qa, sa);
                    decompose(&tr.m[c2 * 16], tb, qb, sb);
                    putQ(b.rot, qnorm(qslerp(qa, qb, u)));
                    for (int c = 0; c < 3; ++c) b.pos.push_back((float)(ta[c] * (1 - u) + tb[c] * u));
                }
            }
            out.bones.push_back(b);
            index[k] = (int)out.bones.size() - 1;
        }
        // the unit (metres per unit)
        if (const XEl* as = root.kid("asset")) if (const XEl* un = as->kid("unit")) if (un->at.count("meter")) out.unit = (float)atof(un->at.at("meter").c_str());
        return true;
    }
}

bool mgReadMotionData(const std::string& data, const std::string& extIn, MGMotion& out, std::string& err, float fps, float maxSecs)
{
    out = MGMotion();
    std::string ext = extIn;
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (data.empty()) { err = "read"; return false; }
    if (ext == "bvh") return readBVH(data, out, err, fps, maxSecs);
    if (ext == "fbx") return readFBX(data, out, err, fps, maxSecs);
    if (ext == "dae") return readDAE(data, out, err, fps, maxSecs);
    err = "format";
    return false;
}

bool mgReadMotion(const std::string& path, MGMotion& out, std::string& err, float fps, float maxSecs)
{
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in.good()) { err = "read"; return false; }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0) { err = "read"; return false; }
    if (size > (std::streamoff)768 * 1024 * 1024) { err = "too_big"; return false; }
    in.seekg(0);
    std::string data((size_t)size, '\0');
    in.read(&data[0], size);
    const size_t dot = path.find_last_of('.');
    return mgReadMotionData(data, dot == std::string::npos ? "" : path.substr(dot + 1), out, err, fps, maxSecs);
}

#ifdef MG_MOTIONREAD_STANDALONE
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
// test tool: mgmotionread <file> <out.json>
#include <cstdio>
int main(int argc, char** argv)
{
    if (argc < 3) { printf("usage: mgmotionread <file> <out.json>\n"); return 2; }
    MGMotion m;
    std::string err;
    const bool ok = mgReadMotion(argv[1], m, err);
    FILE* f = fopen(argv[2], "wb");
    if (!f) return 3;
    fprintf(f, "{\"ok\":%s,\"error\":\"%s\",\"fps\":%g,\"frames\":%d,\"unit\":%g,\"kind\":\"%s\",\"takes\":%d,\"bones\":[", ok ? "true" : "false", err.c_str(), m.fps, m.frames, m.unit, m.kind.c_str(), m.takes);
    for (size_t i = 0; i < m.bones.size(); ++i)
    {
        const MGMotionBone& b = m.bones[i];
        std::string nm;
        for (char c : b.name) { if (c == '"' || c == '\\') nm += '\\'; if ((unsigned char)c >= 32) nm += c; }
        fprintf(f, "%s{\"n\":\"%s\",\"p\":%d,\"t\":[%.6g,%.6g,%.6g],\"r\":[%.7g,%.7g,%.7g,%.7g],\"rz\":[%.7g,%.7g,%.7g,%.7g],\"s\":%.6g", i ? "," : "", nm.c_str(), b.parent, b.t[0], b.t[1], b.t[2], b.r[0], b.r[1], b.r[2], b.r[3], b.rz[0], b.rz[1], b.rz[2], b.rz[3], b.s);
        if (b.hasBind) { fprintf(f, ",\"bind\":["); for (int k = 0; k < 16; ++k) fprintf(f, "%s%.7g", k ? "," : "", b.bind[k]); fprintf(f, "]"); }
        fprintf(f, "}");
    }
    fprintf(f, "],\"rot\":[");
    for (size_t i = 0; i < m.bones.size(); ++i)
    {
        const auto& v = m.bones[i].rot;
        if (v.empty()) { fprintf(f, "%snull", i ? "," : ""); continue; }
        fprintf(f, "%s[", i ? "," : "");
        for (size_t k = 0; k < v.size(); ++k) fprintf(f, "%s%.6g", k ? "," : "", v[k]);
        fprintf(f, "]");
    }
    fprintf(f, "],\"pos\":[");
    for (size_t i = 0; i < m.bones.size(); ++i)
    {
        const auto& v = m.bones[i].pos;
        if (v.empty()) { fprintf(f, "%snull", i ? "," : ""); continue; }
        fprintf(f, "%s[", i ? "," : "");
        for (size_t k = 0; k < v.size(); ++k) fprintf(f, "%s%.6g", k ? "," : "", v[k]);
        fprintf(f, "]");
    }
    fprintf(f, "]}");
    fclose(f);
    printf("%s %s: %d bones, %d frames at %g fps, takes %d (%s)\n", ok ? "OK" : "FAIL", err.c_str(), (int)m.bones.size(), m.frames, m.fps, m.takes, m.kind.c_str());
    return ok ? 0 : 1;
}
#endif
