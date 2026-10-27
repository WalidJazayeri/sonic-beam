# Sonic Beam — native mode (`natif/`)

Sonic as a real object of BeamNG's scene, with no DLL: BeamNG itself lights him, sorts him with the rest of the world and hides him behind whatever stands in front of him. He casts no shadow yet (a StaticShape does not).

This mode is separate from the overlay mode in `beamng/` (paused). It has its own mod, `sonicnative`, its own version (`natif/VERSION`, git tags `natif-vX.Y.Z`, see `CHANGELOG.md`), and every file, extension, object, material and datablock name starts with `sonicnative`. It never loads anything from `beamng/`.

## How it works
- **Converter** (`natif/converter`, built as `natif/build/sonicnative_convert.exe`): reads Sonic from the player's own Sonic Frontiers (`sonic.pac`: model, skeleton, clips, textures; the sound banks under `image/x64/raw/sound`) and writes a skinned, animated COLLADA shape (the overlay's 19 clips as named sequences) with its materials and textures, the spin ball shell as a second shape, plus Sonic's sounds as WAV files, into the BeamNG user folder, under `mods/sonicnative_data/`, with a `manifest.json`. Nothing from Frontiers is ever in the repo or in the mod. `--no-ball` skips the shell; `--out <folder>` writes the package elsewhere for inspection.
- **Mod** (`natif/mod`, installed as `mods/unpacked/sonicnative`):
  - `sonicnative_main` loads the materials and the shape's datablock, spawns Sonic as the StaticShape `sonicnative_sonic`, keeps him at the walker's feet facing his run, hides BeamNG's walker and picks his clip and its speed.
  - `sonicnative_body` is Sonic's physics on BeamNG's walking body, ported from the overlay mode, with the vehicle extensions `sonicnative_runner` and `sonicnative_impact` and the camera `sonicnativeChase`.
  - BeamNG's cameras on foot: on the walking body BeamNG only draws its first-person camera unless a global camera is active, so `sonicnativeChase` (global) also draws the walking body's own orbit, external, drone and top-down cameras (`config.camera.footCameras`, in the Options > Cameras order and on/off state) and sends "forward" from the view on screen. In those views a held direction keeps its course (orbit and drone swing behind the run by themselves), turned only by their own look yaw; releasing or turning the direction (the runner sends it in `stickSteps`) reads the view again. On foot, `sonicnative_body` filters the walker's `unicycle__crouchToggle` so C reaches `switch_camera_next`; its `trackCamMode` hook puts Sonic's camera back at once on the next view (or the number key's) and shows its name. `commands.toggleCamera` is wrapped while on foot so Shift+C (and only it) goes on to the free camera.
  - Sonic's clip comes from the overlay's animation state machine, ported to Lua with its numbers in `lua/common/sonicnative/anim.lua` (from `beamng/sheets/states.json`, `events.json`, `tuning.json` and `beamng/src/game.cpp`): idle, walk, run, boost, fast, drift left/right, the landing hold, the jump (jump start, then the ball; Lab jump style 0 by default), the trick state, wall runs and swimming, with each clip's rate following his speed. When the manifest has a `"ball"`, Frontiers' spin ball shell is a second StaticShape, `sonicnative_ball`, rolling around him, and his meshes are hidden while it shows. A clip missing from an older conversion falls back to a close one (drift to boost, wall run to run, trick to fall).
  - `sonicnative_menu` is the overlay's O menu (Sonic mode, speed limit, top speed, boost acceleration, speedometer, Unstuck, Lab) and its Lab with the tabs the native mode has (Sound, Boost, Aura, Motion blur, Water, Wall, Impact, Jump, Animation), applied live and saved in the user folder (`settings/sonicnative.json`) when a window closes; every Lab setting explains itself in a hover tooltip (`LAB_TIPS`, and `tip` on each choice); the Unstuck key (U by default; both keys can be rebound in BeamNG's Controls) puts Sonic back upright on the last spot he stood on, only on request.
  - `sonicnative_sharp` keeps Sonic sharp in the motion blur. BeamNG's own (`PostFxMotionBlur`) gives per-object motion only to vehicles, so Sonic, followed by the camera, smeared like passing scenery; a PostEffect cannot write BeamNG's velocity buffer. While Sonic is on foot and BeamNG's motion blur is on, five compute PostEffects (`shaders/sonicnative/motionBlur*.cs.hlsl`: prepare, tile max x and y, neighbor max, gather, after McGuire et al. 2012) blur the frame from BeamNG's velocity buffer with BeamNG's strength, giving no motion to the pixels inside rounded cones around his bones (the ball shell, or a capsule without bone tracks), above the ground under his feet. BeamNG's own is never turned off: it is moved to a render bin that never draws, so it stays enabled with its strength and the options, photo mode (which saves and writes back that state) and the World Editor see the player's real state; this one runs only while it is enabled. `sonicnative_fx` hands it his pose every frame. In a car or with the Lab's switch off, BeamNG's goes back to its bin.
  - `sonicnative_sound` decides Sonic's sounds (steps and landings by ground type, jump, boost, brake, wind, sonic boom) with the overlay's rules; the vehicle extension `sonicnative_sfx` plays the converter's WAVs (`mods/sonicnative_data/sounds`) through BeamNG's audio, attached to Sonic's body.

## Convert
Close BeamNG first, build the converter (`natif\build.ps1 -Converter`), then run it for the dev user folder (its options are described in `natif/converter`):
```
natif\build\sonicnative_convert.exe --userpath C:\Users\jazay\Documents\Sonic_Horizon\beamng\devuser
```

## Build and install for dev
```
powershell -ExecutionPolicy Bypass -File natif\build.ps1                    # Lua check
powershell -ExecutionPolicy Bypass -File natif\build.ps1 -Converter         # + build the converter
powershell -ExecutionPolicy Bypass -File natif\build.ps1 -Install           # + mod into beamng\devuser
powershell -ExecutionPolicy Bypass -File natif\build.ps1 -Install -Normal   # + mod into %LOCALAPPDATA%\BeamNG\BeamNG.drive\current
```
- The Lua check runs `luac -p` on every file of `natif/mod`, and refuses globals missing from the list in `build.ps1` and any global written by mistake.
- `-Install` updates `mods\unpacked\sonicnative` in place (never deletes the folder first: a running game would drop Sonic's camera mode) and removes files that left `natif\mod`. `-Normal` refuses while BeamNG runs.
- Keep only one mode active in a user folder: the overlay mod (`sonicfrontiers_sonic`) and this one both take over the walker.

## Testing in game
With the dev MCP `run_lua` tool:
- `return jsonEncode(sonicnative_main.status())`: spawned or not, clip and rate, position and heading, materials, missing clips, where the `.cdae` cache was written, last error.
- `sonicnative_main.setDebugClip('run', 1.5)` forces a clip and its rate; `setDebugClip(nil)` gives it back to the state machine.
- `sonicnative_main.setDriveMode('engine')` lets the engine run the animation instead of Lua setting its position every frame (`'manual'`, the default since natif-v0.5.1: the sharp motion blur's mask needs the exact pose).
- `sonicnative_main.checkClips()` lists which clips the shape really has as sequences (the current clip then starts again). Also `respawn()` and `setTuning({...})`.
- `return jsonEncode(sonicnative_sharp.status())`: the sharp motion blur on or not, BeamNG's (enabled, bin, strength), the mask (bones, ball, capsule) and its cones. `sonicnative_sharp.debug({mask = true})` tints Sonic's pixels; `debug({view = 'blur' | 'tiles' | 'distance' | 'frame'})` shows each pass in grey; `debug({shaderDir = 'shaders/x/'})` makes the passes again from other files (BeamNG keeps a shader compiled from a path for the whole session).
- `Engine.Debug.getGpuTimings()` gives the GPU time of each render stage (`PostFX`, `PostFxMotionBlur`, ...).
