# Sonic Beam

Run free as the real Sonic from Sonic Frontiers across every BeamNG.drive map, with an unlimited boost, Frontiers' boost effects and his own sounds.

You are Sonic. Load any map in Freeroam and you start on foot as the actual Sonic from Sonic Frontiers: his model, his animations, his sounds and his boost effects, read straight from your own copy of Frontiers.

## What you get
- **Real Frontiers Sonic**: model, textures and animations that speed up with him (run, boost, drifts, jumps, air tricks).
- **Unlimited boost**: hold Shift and keep accelerating. Set a top speed (or none) in the O menu.
- **Frontiers boost effects**: radial speed blur, an electric-blue boost aura streaming around him like in the Sonic artwork, with lightning crackling on his body, the Power Boost trail with lightning arcs and crackling bolts, and a flash with glowing orbs when the boost kicks in. Effects grow with your speed.
- **His sounds**: footsteps that match the ground (asphalt, dirt, grass, sand, snow, water, metal), jump, boost, landing, brake, speed wind, a sonic boom on fast boosts.
- **A real shadow** that follows his silhouette and BeamNG's sun.
- **Runs on water**: fast enough, Sonic runs across lakes and the sea; slower and out of his depth, he swims back to the shore.
- **Smashes cars**: hitting traffic at speed sends the cars flying and crushes them. In the Lab, choose whether Sonic loses speed by the car's mass, plows through or bounces off, and tune the impact force and the damage.
- **Runs on walls**: boost into a building or a cliff and Sonic runs up it, or along it when you come in at an angle, with Frontiers' wall-run animations.
- **Frontiers' jump ball**: Sonic curls up into a spinning blue ball when he jumps, in Frontiers' own spin shell. Choose in the Lab whether he stays a ball, does tricks after the top of the jump, or does a trick when you hold a direction in the air; tune the spin speed, the glow and the jump height.
- **Sonic Beam Lab**: an in-game panel (O menu, then Lab) to tune volumes, shadow, trail, lightning, speed blur, aura, jump ball and animation speed. Click any value to type it, even past the slider's range. Saved between sessions.
- Stuck in a fence after a 600 km/h crash? **Unstuck** in the O menu puts you back on your feet.

## Controls
- W A S D: run · Shift: boost · Space: jump · Mouse: camera
- F: get in or out of a car · O: Sonic Beam menu (top speed, Lab, Unstuck)

## Supported game versions
- **BeamNG.drive 0.40** (tested on 0.40.0.0, build 287428), Windows, Direct3D 12 or Direct3D 11.
- **Sonic Frontiers** for PC, current version with all free updates (tested on the January 2026 PC build). Frontiers only has to be installed; it does not need to run.

## Good to know
- By default Sonic takes over **Freeroam** only (BeamNG.Earth's map included); career, scenarios, missions and tutorials keep BeamNG's normal walking. In the O menu, **Sonic: Always** makes him your walker in every game mode (he never pulls you out of a car there: get out with F), and **Sonic: Off** turns him off.
- On the first launch, BeamNG warns about a **"Third-party library"** (`version.dll`, which draws Sonic). Click **Cancel** and it won't ask again.

## Install
1. Close BeamNG.drive.
2. Unzip this package anywhere and double-click **`install.bat`**.
   It finds BeamNG.drive and Sonic Frontiers in your Steam libraries (or asks for their folders), backs up any file it would replace, then installs the mod.
3. Start BeamNG.drive and load any map in **Freeroam**: you are Sonic.

If Windows refuses to write into the BeamNG folder, right-click `install.bat` and choose **Run as administrator**.

## Uninstall
Close BeamNG.drive and double-click **`uninstall.bat`** (from the same unzipped folder). It removes every Sonic Beam file and puts back the files the installer had backed up, so BeamNG is exactly as before.

## Manual install (without the script)
1. Copy `sonicfrontiers_sonic` into `%LOCALAPPDATA%\BeamNG\BeamNG.drive\current\mods\unpacked\`.
2. Copy the content of `bin64` (`version.dll` and the `SonicBeam` folder) into BeamNG.drive's `Bin64` folder (back up any `version.dll` already there).
3. Create `%LOCALAPPDATA%\SonicBeam\settings.ini` with one line pointing to your Sonic Frontiers folder, for example:
   `frontiers=D:/Games/Sonic Frontiers` (the folder that holds `SonicFrontiers.exe` and `image`)

To remove it by hand, delete those three things (and restore your backup).

## What is in this package
- `install.bat`, `uninstall.bat`, `setup.ps1`: the installer and uninstaller.
- `sonicfrontiers_sonic/`: the BeamNG mod (Lua), installed as an unpacked mod in BeamNG's user folder.
- `bin64/version.dll`: the renderer that draws Sonic and plays his sounds, installed in BeamNG's `Bin64` folder; `bin64/SonicBeam/` holds the licenses.

## Credits & licenses
Sonic Beam is MIT licensed. Uses ACL and RTM (Nicholas Frechette, MIT), HedgeLib formats knowledge (MIT) and vgmstream's clHCA decoder (ISC). Sonic and Sonic Frontiers are © SEGA; BeamNG.drive is © BeamNG GmbH. This is a fan mod, not affiliated with either. No game files are included: everything is read from your own installs.
