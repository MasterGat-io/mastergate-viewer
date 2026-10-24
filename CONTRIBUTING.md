# Contributing to MasterGate Viewer

Thank you for helping improve MasterGate Viewer. This guide explains how to report problems and how to send code, so that changes stay easy to review and easy to merge with new Firestorm releases.

## Reporting a problem or an idea

- **Bugs and support:** open a ticket at **https://mastergate.io/m/ticket/add**. Say which version you use (Help › About), your system (Windows, macOS or Linux), what you did, what you expected and what happened. Add a screenshot or the viewer log if you can.
- **Ideas and feature requests:** the same ticket page, or the MasterGate group in Second Life.
- **Security issues** (anything that could expose accounts, inventory or content): use a ticket marked *Security*, and please do not publish details before we have answered.

Problems with MasterGate Viewer are not handled by Linden Lab or by the Firestorm team, so please report them to us.

## Sending code (pull requests)

Pull requests go to the **`mastergate/main`** branch.

1. **Title:** short and clear, e.g. `Wardrobe: keep the 3D view after a teleport`.
2. **Description:** what the change does, why it is needed, and how you tested it (system, steps to check it).
3. **One topic per pull request.** Keep unrelated fixes apart.
4. **It must build without warnings**: the Windows build treats warnings as errors, and the macOS and Linux builds run on GitHub for every release.
5. **License:** by sending a pull request you agree that your contribution is published under the GNU LGPL 2.1, like the rest of the viewer.

### Where MasterGate code goes

- **New features:** new files named `mg*.cpp` / `mg*.h` in `indra/newview` (one per MG Phone app, e.g. `mgphonewardrobe.cpp`), registered in `indra/newview/CMakeLists.txt`. Code in these files needs no special comments.
- **Changes to upstream code** (Linden Lab or Firestorm files): mark them with `<MG>` comment tags and keep the original code in a comment. When a new Firestorm release is merged, whoever merges can then see what the original did, what upstream changed and what we changed on top:

  ```c++
  // <MG> the phone rings instead of the call dialog
  // LLFloaterReg::showInstance("incoming_call", payload, false);
  if (!MGPhoneCalls::onIncomingInvite(payload))
  {
      LLFloaterReg::showInstance("incoming_call", payload, false);
  }
  // </MG>
  ```

  A one-line change can use the short form: `bool keep = true; // <MG/> was false: ...`
- **The interface skin** lives in `indra/newview/skins/mastergate`; the default skin's files stay as upstream.

### Rules for the MG Phone

- **Never send asset UUIDs to a web page.** Phone pages get inventory item ids and pictures rendered by the viewer (thumbnails, 3D frames), never the UUID of a texture, material, animation or mesh.
- **No content export** unless the user created every part of it (Linden Lab's Policy on Third-Party Viewers, 2.b). Geometry and pictures read for previews stay inside the viewer.
- **Nothing happens without the user:** no automatic purchases, uploads or installs. Anything sent to mastergate.io must be described in the [privacy policy](https://mastergate.io/viewer/privacy/).
- Follow Linden Lab's [Policy on Third-Party Viewers](https://secondlife.com/corporate/third-party-viewers) and the [Second Life Terms of Service](https://lindenlab.com/tos).

Thank you!
