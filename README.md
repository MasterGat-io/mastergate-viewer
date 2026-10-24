<img align="left" width="100" height="100" src="indra/newview/icons/release/firestorm_256.png" alt="MasterGate Viewer logo"/>

**[MasterGate Viewer](https://mastergate.io/viewer/) is a free third-party viewer for Second Life with the MasterGate phone (MG Phone) built in.**

This repository contains its full source code. MasterGate Viewer is made by [MasterGate](https://mastergate.io) and based on [Firestorm](https://github.com/FirestormViewer/phoenix-firestorm) and the [Second Life viewer](https://github.com/secondlife/viewer). It is **not provided or supported by Linden Lab** or by The Phoenix Firestorm Project.

## Download

Windows, macOS (beta) and Linux (beta) builds: **https://mastergate.io/viewer/** (also attached to the [releases](../../releases) here, with SHA-256 checksums).
Installing over an older version keeps your settings, and the viewer offers updates itself when a new release is out.

## What it adds

The MG Phone lives inside the viewer as a panel, so there is no HUD to wear. Besides the MasterGate apps (messages, feeds, photos, estates and more), it has viewer-only apps:

- **Calls**: Second Life voice calls ring and run in the phone; contacts, nearby residents, recents and favorites.
- **Camera Pro**: framing guides, exposure, time of day, full-size shots with color looks and an editor.
- **Textures & Materials**: your inventory textures and PBR materials as a photo library; preview, apply, edit and organize.
- **Animations**: animations and gestures on a 3D mannequin before anyone sees them; AO integration.
- **Wardrobe**: clothes, body parts and outfits as picture cards, each item viewable in 3D.

Phone pages never receive asset UUIDs: thumbnails and 3D views are rendered by the viewer. See the [privacy policy](https://mastergate.io/viewer/privacy/) for what the phone sends to mastergate.io.

## Source layout

- Branch **`mastergate/main`**, started from Firestorm's official release tag `Firestorm_Release_7.2.4.80712`.
- MasterGate code is in new files (`indra/newview/mg*.cpp`, `mg*.h`) or in edits marked `<MG>` … `</MG>`, so Firestorm releases can be merged in.
- The MG Phone panel and bridge: `mgfloaterphone.cpp`. Apps: `mgphonecalls.cpp`, `mgphonecamera.cpp`, `mgphoneinventory.cpp`, `mgphoneanims.cpp`, `mgphonewardrobe.cpp`, `mgphoneupdate.cpp`; sign-in: `mgphoneauth.cpp`.
- The interface skin: `indra/newview/skins/mastergate`.

## Building

Follow Firestorm's build instructions for [Windows](doc/building_windows.md), [macOS](doc/building_macos.md) and [Linux](doc/building_linux.md), with the open-source configuration and OpenAL:

```
autobuild configure -A 64 -c ReleaseFS_open -- --chan Release --no-opensim --openal
autobuild build -A 64 -c ReleaseFS_open --no-configure
```

Add `--package` to the configure options to produce the installer.
- The version's last number comes from `indra/newview/MGV_REVISION.txt`.
- [`.github/workflows/mastergate_build.yml`](.github/workflows/mastergate_build.yml) builds all three platforms on GitHub. A `mgv-<version>` tag also drafts a release.

Self-compiled builds are not supported. Please use the official downloads.

## Support

Open a ticket at **https://mastergate.io/m/ticket/add**, or use Help in the viewer. Please do not ask Linden Lab or the Firestorm team for help with MasterGate Viewer.

## License and credits

- The viewer code is available under the **GNU LGPL 2.1** ([LICENSE](doc/LGPL-license.txt)), like the Second Life and Firestorm viewers it is based on.
- Thanks to Linden Lab and to The Phoenix Firestorm Project and its contributors, whose work this viewer builds on.
- Second Life is a trademark of Linden Research, Inc. Firestorm is a project of The Phoenix Firestorm Project, Inc.
- MasterGate and the MasterGate Viewer name and logo belong to MasterGate.
- This viewer follows Linden Lab's [Policy on Third-Party Viewers](https://secondlife.com/corporate/third-party-viewers).
