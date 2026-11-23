/**
 * @file mgmotionread.h
 * @brief <MG> Motion files (BVH, binary FBX) read into a neutral skeleton + motion, for the
 *        Animation Studio's converter (the page retargets it onto the SL skeleton).
 *
 * Std only (plus zlib for FBX arrays): builds standalone with -DMG_MOTIONREAD_STANDALONE.
 */
#ifndef MG_MOTIONREAD_H
#define MG_MOTIONREAD_H

#include <string>
#include <vector>

struct MGMotionBone
{
    std::string name;
    int parent = -1;                  // index in MGMotion::bones (parents come first)
    float t[3] = { 0.f, 0.f, 0.f };   // rest local translation (file units)
    float r[4] = { 0.f, 0.f, 0.f, 1.f };   // rest local rotation (x, y, z, w), pre/post rotations folded in
    float s = 1.f;                    // rest local scale (uniform)
    float rz[4] = { 0.f, 0.f, 0.f, 1.f };  // local rotation with the rotation channels at zero (FBX: PreRotation * PostRotation^-1)
    bool hasBind = false;
    float bind[16] = { 0 };           // FBX bind pose: world matrix, column-major (translation at 12..14)
    std::vector<float> rot;           // per frame local rotation x, y, z, w (empty = not animated)
    std::vector<float> pos;           // per frame local translation (empty = not animated)
};

struct MGMotion
{
    float fps = 30.f;
    int frames = 0;
    float unit = 1.f;                 // metres per file unit (as the file says)
    std::string kind;                 // "bvh" or "fbx"
    std::string take;                 // FBX animation stack used
    int takes = 0;                    // FBX animation stacks in the file
    std::vector<MGMotionBone> bones;
};

// Reads a .bvh or a binary .fbx (resampled to `fps`, at most `maxSecs`). false + err on failure:
// "read", "format", "ascii_fbx", "no_skeleton", "no_animation", "too_big".
bool mgReadMotion(const std::string& path, MGMotion& out, std::string& err, float fps = 30.f, float maxSecs = 300.f);
bool mgReadMotionData(const std::string& data, const std::string& ext, MGMotion& out, std::string& err, float fps = 30.f, float maxSecs = 300.f);

#endif
