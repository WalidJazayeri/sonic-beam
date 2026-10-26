# Sonic Beam — native mode (`natif/`)

Sonic as a real object of BeamNG's scene, with no DLL: BeamNG itself lights him, sorts him with the rest of the world and hides him behind whatever stands in front of him. He casts no shadow yet (a StaticShape does not).

This mode is separate from the overlay mode in `beamng/` (paused). It has its own mod, `sonicnative`, its own version (`natif/VERSION`, git tags `natif-vX.Y.Z`, see `CHANGELOG.md`), and every file, extension, object, material and datablock name starts with `sonicnative`. It never loads anything from `beamng/`.

## How it works
- **Converter** (`natif/converter`, built as `natif/build/sonicnative_convert.exe`): reads Sonic from the player's own Sonic Frontiers (`sonic.pac`: model, skeleton, clips, textures; the sound banks under `image/x64/raw/sound`) and writes a skinned, animated COLLADA shape (the overlay's 19 clips as named sequences) with its materials and textures, the spin ball shell as a second shape, plus Sonic's sounds as WAV files, into the BeamNG user folder, under `mods/sonicnative_data/`, with a `manifest.json`. Nothing from Frontiers is ever in the repo or in the mod. `--no-ball` skips the shell; `--out <folder>` writes the package elsewhere for inspection.
- **Mod** (`natif/mod`, installed as `mods/unpacked/sonicnative`):
  - `sonicnative_main` loads the materials and the shape's datablock, spawns Sonic as the StaticShape `sonicnative_sonic`, keeps him at the walker's feet facing his run, hides BeamNG's walker and picks his clip and its speed.
  - `sonicnative_body` is Sonic's physics on BeamNG's walking body, ported from the overlay mode, with the vehicle extensions `sonicnative_runner` and `sonicnative_impact` and the camera `sonicnativeChase`.
  - Sonic's clip comes from the overlay's animation state machine, ported to Lua with its numbers in `lua/common/sonicnative/anim.lua` (from `beamng/sheets/states.json`, `events.json`, `tuning.json` and `beamng/src/game.cpp`): idle, walk, run, boost, fast, drift left/right, the landing hold, the jump (jump start, then the ball; Lab jump style 0 by default), the trick state, wall runs and swimming, with each clip's rate following his speed. When the manifest has a `"ball"`, Frontiers' spin ball shell is a second StaticShape, `sonicnative_ball`, rolling around him, and his meshes are hidden while it shows. A clip missing from an older conversion falls back to a close one (drift to boost, wall run to run, trick to fall).
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
- `sonicnative_main.setDriveMode('manual')` moves the animation from Lua every frame instead of letting the engine run it (`'engine'`).
- `sonicnative_main.checkClips()` lists which clips the shape really has as sequences (the current clip then starts again). Also `respawn()` and `setTuning({...})`.
