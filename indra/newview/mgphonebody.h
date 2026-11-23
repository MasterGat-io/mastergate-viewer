/**
 * @file mgphonebody.h
 * @brief <MG> MG Phone Body Studio: shape editor, bone morphs, big size and inflation.
 *
 * Everything it makes is a standard Second Life asset, so EVERY viewer shows it:
 *  - the Shape and Physics wearables (the 127 + 33 sliders; values stay in each slider's range,
 *    as every viewer clamps them);
 *  - bone morphs and inflation as ANIMATIONS: position keys on collision volumes (LEFT_PEC,
 *    BELLY, BUTT...) and Bento face bones. An animation's position REPLACES the bone's position
 *    (llkeyframemotion.cpp), so each one carries the shape's own positions plus the morph; a
 *    stopped animation leaves a bone where it was (llpose.cpp), so level 0 brings them back.
 *    Uploaded and played (sendAnimationRequest), or played by an LSL HUD on any viewer.
 *
 *   body_state                         the worn shape / physics sliders, height, the body found,
 *                                      the published animations, the snapshots
 *   body_set {type, values{id:pct}}    sliders changed on the worn wearable (preview; not saved)
 *   body_save {type, as_new, name}     body_revert {type}
 *   body_camera {view, turn}           face / upper / full / legs, turned (degrees); "reset"
 *   body_pose {on}                     the appearance pose (arms out), on this screen only
 *   body_morph {bones, inflate, level, speed, bounce}   the morph + inflation at a level, previewed
 *                                      on this screen (a local animation)
 *   body_publish {name, levels, max, bones, inflate, speed, bounce}  levels 0..N made and uploaded
 *                                      ("body_published"); body_play {level} / {level:-1}
 *   body_snaps {what: list|save|delete, name, values}   shape versions kept on this computer
 */

#ifndef MG_PHONEBODY_H
#define MG_PHONEBODY_H

#include "llsd.h"

namespace MGPhoneBody
{
    bool isCommand(const std::string& cmd);
    LLSD runCommand(const std::string& cmd, const LLSD& args);
    void idle();
    void onClose();
}

#endif
