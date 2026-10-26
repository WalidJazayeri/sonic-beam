# Sonic Beam — native mode (`natif/`)

Sonic as a real object of BeamNG's scene, with no DLL: BeamNG itself lights him, sorts him with the rest of the world and hides him behind whatever stands in front of him. He casts no shadow yet (a StaticShape does not).

This mode is separate from the overlay mode in `beamng/` (paused). It has its own mod, `sonicnative`, its own version (`natif/VERSION`, git tags `natif-vX.Y.Z`, see `CHANGELOG.md`), and every file, extension, object, material and datablock name starts with `sonicnative`. It never loads anything from `beamng/`.

## How it works
- **Converter** (`natif/converter`, built as `natif/build/sonicnative_convert.exe`): reads Sonic from the player's own Sonic Frontiers (`sonic.pac`: model, skeleton, clips, textures) and writes a skinned, animated COLLADA shape with its materials and textures into the BeamNG user folder, under `mods/sonicnative_data/`, with a `manifest.json`. Nothing from Frontiers is ever in the repo or in the mod.
- **Mod** (`natif/mod`, installed as `mods/unpacked/sonicnative`):
  - `sonicnative_main` loads the materials and the shape's datablock, spawns Sonic as the StaticShape `sonicnative_sonic`, keeps him at the walker's feet facing his run, hides BeamNG's walker and picks his clip and its speed.
  - `sonicnative_body` is Sonic's physics on BeamNG's walking body, ported from the overlay mode, with the vehicle extensions `sonicnative_runner` and `sonicnative_impact` and the camera `sonicnativeChase`.

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
- `-Install` deletes the old `mods\unpacked\sonicnative` before copying. `-Normal` refuses while BeamNG runs.
- Keep only one mode active in a user folder: the overlay mod (`sonicfrontiers_sonic`) and this one both take over the walker.

## Testing in game
With the dev MCP `run_lua` tool:
- `return jsonEncode(sonicnative_main.status())`: spawned or not, clip and rate, position and heading, materials, missing clips, where the `.cdae` cache was written, last error.
- `sonicnative_main.setDebugClip('run', 1.5)` forces a clip and its rate; `setDebugClip(nil)` gives it back to Sonic's speed.
- `sonicnative_main.setDriveMode('manual')` moves the animation from Lua every frame instead of letting the engine run it (`'engine'`).
- `sonicnative_main.checkClips()` lists which clips the shape really has as sequences (the current clip then starts again). Also `respawn()` and `setTuning({...})`.
