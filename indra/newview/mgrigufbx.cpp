/**
 * @file mgrigufbx.cpp
 * @brief MasterGate Viewer: builds ufbx (FBX reader, MIT / public domain, v0.23.1) for
 *        Creator Lab's rig converter. Compiled as C++, without the viewer's precompiled
 *        header (see CMakeLists.txt: no Windows macros in ufbx).
 */

#include "ufbx/ufbx.c"
