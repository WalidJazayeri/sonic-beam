-- SPDX-License-Identifier: MIT
-- Sonic Beam, native mode: Sonic is a real object of BeamNG's scene (a StaticShape), lit, sorted
-- and drawn by the game itself, with no DLL. The converter (natif/converter) reads Sonic from the
-- player's own Sonic Frontiers and writes his shape, materials and textures into the user folder
-- (mods/sonicnative_data). This extension loads them, keeps Sonic at the walker's feet every
-- frame, hides the walker and picks Sonic's animation and its speed. His body is sonicnative_body.

local M = {}
local logTag = 'sonicnative'

local DATA = '/mods/sonicnative_data/'    -- written on the player's PC by the converter, never shipped
local OBJECT_NAME = 'sonicnative_sonic'
local DEFAULT_DATABLOCK = 'sonicnative_db'

-- Copied from the overlay mode (beamng/), never read from it at run time:
local FACING_RATE = 12          -- 1/s, how fast he turns to face his run (beamng/sheets/tuning.json facingRate)
local SPEED_SMOOTHING = 0.12    -- s, speed smoothing for the animation (beamng/sheets/tuning.json speedSmoothing)
local GROUND_PROBE = 0.35       -- m, gap under the body that still counts as ground (beamng/sheets/physics.json groundProbe)
local HEADING_MIN_SPEED = 1.5   -- m/s, slower than this he keeps his heading (beamng/src/game.cpp)
local AIR_DELAY = 0.12          -- s off the ground before he counts as airborne (beamng/src/game.cpp)
local REHIDE_SECONDS = 0.5      -- the walker's meshes are hidden again this often (beamng/mod sonic.lua)

-- Prototype settings (natif plan, section 3); M.setTuning changes them live for tests
local tuning = {
  idleBelowKmh = 1.5,       -- slower than this: idle
  boostAboveKmh = 25,       -- faster: boost, when no body says it (BeamNG's own sprint tops out near 40 km/h)
  clipHoldSeconds = 0.15,   -- a new clip stays at least this long (no flicker around a threshold)
  facingOffsetDeg = 0,      -- turns the model about Z, in case the converted front is off
  feetOffset = 0,           -- metres added to the feet height
}

-- what Sonic is doing -> clip names tried in that order in the manifest
local ROLES = {idle = {'idle'}, run = {'run', 'walk'}, boost = {'boost'}, air = {'fall', 'air'}}

local manifest, manifestOk = nil, false
local shapePath, datablockName = nil, nil
local clipsByName, roleClip, timelineSeconds = {}, {}, 1
local materialsOk, missingMaterials = nil, {}
local constructorsRun = {}
local lastError = nil
local warned = {}               -- problems already logged: logged once, never every frame

local spawned, rendered = false, false
local driveMode = 'engine'      -- 'engine': the engine advances the thread; 'manual': Lua sets its position each frame
local clip, playing = nil, nil  -- clip asked for, sequence really playing ('ambient' when the clip is missing)
local clipRate, clipPhase, clipSince = 1, 0, -100
local appliedScale = nil        -- thread time scale last given to the engine
local debugClip, debugRate = nil, 1
local missingClips = {}

local clock = 0
local walking = false
local heading, headingSet = 0, false
local lastSpeed, lastVx, lastVy = 0, 0, 0
local speedKmh, airTime, airborne, usingBody = 0, 0, false, false
local hiddenWalker, hiddenAt = nil, -100
local lastWalkerId = nil        -- the walker Sonic followed last frame (nil: none)

local center, feet, face = vec3(), vec3(), vec3(0, 1, 0)
local down, up = vec3(0, 0, -1), vec3(0, 0, 1)
local rot = quat(0, 0, 0, 1)

local function report(level, key, msg)
  lastError = msg
  if warned[key] then return end
  warned[key] = true
  log(level, logTag, msg)
end

-- manifest paths are relative to the data folder (or already absolute)
local function dataPath(p)
  if type(p) ~= 'string' or p == '' then return nil end
  if p:sub(1, 1) == '/' then return p end
  return DATA .. p
end

local function samePath(a, b)
  local function norm(p) return (string.lower(tostring(p or '')):gsub('\\', '/'):gsub('^/+', '')) end
  return norm(a) == norm(b)
end

local function levelLoaded()
  return scenetree.findObject('MissionGroup') ~= nil
end

local function readManifest()
  local m = jsonReadFile(DATA .. 'manifest.json')
  if type(m) ~= 'table' or type(m.shape) ~= 'string' then
    manifest, manifestOk, shapePath = nil, false, nil
    report('W', 'manifest', 'no converted Sonic in ' .. DATA .. ': run the converter (natif\\build.ps1 -Converter, then natif\\build\\sonicnative_convert.exe) with BeamNG closed')
    return false
  end
  manifest, manifestOk, shapePath = m, true, dataPath(m.shape)
  clipsByName, roleClip = {}, {}
  local timelineFps, keys = math.max(1, tonumber(m.fps) or 30), 0
  for _, c in ipairs(type(m.clips) == 'table' and m.clips or {}) do
    if type(c) == 'table' and type(c.name) == 'string' then
      local fps = math.max(1, tonumber(c.fps) or timelineFps)
      local frames = tonumber(c.frames) or fps + 1
      -- Torque plays a clip of n keys over (n-1)/fps, which the converter writes as seqSeconds
      local seconds = tonumber(c.seqSeconds) or tonumber(c.duration) or (frames - 1) / fps
      c.seconds = math.max(1 / 30, seconds)
      clipsByName[c.name] = c
      keys = keys + frames
    end
  end
  -- the clips lie end to end on one timeline: the default sequence plays all its keys
  timelineSeconds = math.max(1 / 30, (keys - 1) / timelineFps)
  for role, names in pairs(ROLES) do
    for _, n in ipairs(names) do
      if clipsByName[n] then roleClip[role] = n break end
    end
  end
  -- no idle clip: the first clip, else the shape's default sequence
  if not roleClip.idle then
    local first = type(m.clips) == 'table' and m.clips[1]
    roleClip.idle = type(first) == 'table' and clipsByName[first.name] and first.name or 'ambient'
  end
  return true
end

-- Materials before the datablock: its preload imports the shape, which binds them by name
-- (a slot with no material is drawn orange). Loaded per level, as a level's end may drop them.
local function ensureMaterials()
  missingMaterials = {}
  local file = dataPath(manifest.materials)
  if not file then materialsOk = nil return end
  local defs = jsonReadFile(file)
  if type(defs) ~= 'table' then
    materialsOk = false
    report('E', 'materials', 'cannot read the materials ' .. file)
    return
  end
  local missing = false
  for name in pairs(defs) do
    if not scenetree.findObject(name) then missing = true break end
  end
  if missing then
    local ok, res = pcall(loadJsonMaterialsFile, file)
    if not ok or res == false then report('E', 'materialsLoad', 'loadJsonMaterialsFile failed on ' .. file .. ': ' .. tostring(res)) end
  end
  for name in pairs(defs) do
    if not scenetree.findObject(name) then missingMaterials[#missingMaterials + 1] = name end
  end
  materialsOk = #missingMaterials == 0
  if not materialsOk then
    report('W', 'materialsMissing', 'materials still missing after loading ' .. file .. ': ' .. table.concat(missingMaterials, ', '))
  end
end

-- A '--split cs' conversion names its clips in a TSShapeConstructor (.cs) next to the shape,
-- which must run before the shape loads. The engine may run it by itself too: it is a singleton.
local function runConstructor()
  local cs = dataPath(manifest.constructor)
  if not cs and manifest.split == 'cs' then cs = (shapePath:gsub('%.dae$', '.cs')) end
  if not cs or constructorsRun[cs] then return end
  constructorsRun[cs] = true
  if not FS:fileExists(cs) then report('W', 'constructor', 'shape constructor not found: ' .. cs) return end
  local ok, err = pcall(TorqueScript.exec, cs)
  if not ok then report('E', 'constructorExec', 'cannot run ' .. cs .. ': ' .. tostring(err)) end
end

-- The datablock is declared through TorqueScript, the route verified in BeamNG 0.40: one made
-- from Lua with createObject('StaticShapeData') is never preloaded, so its shape never loads.
local function ensureDatablock()
  if shapePath:find('["\\;]') then report('E', 'shapePath', 'unusable shape path: ' .. shapePath) return false end
  local name = type(manifest.datablock) == 'string' and manifest.datablock or DEFAULT_DATABLOCK
  name = name:gsub('[^%w_]', '_')
  local existing = scenetree.findObject(name)
  if existing then
    local ok, file = pcall(existing.getField, existing, 'shapeFile', 0)
    if ok and samePath(file, shapePath) then datablockName = name return true end
    -- the converter wrote a new shape while the game ran: a datablock of its own for it
    name = name .. '_' .. tostring(manifest.hash or 'new'):gsub('[^%w_]', '_')
    if scenetree.findObject(name) then datablockName = name return true end
  end
  local code = string.format('datablock StaticShapeData(%s) { shapeFile = "%s"; };', name, shapePath)
  local ok, err = pcall(TorqueScript.eval, code)
  if not ok then report('E', 'datablock', 'datablock ' .. name .. ' failed: ' .. tostring(err)) return false end
  if not scenetree.findObject(name) then report('W', 'datablockMissing', 'datablock ' .. name .. ' was declared but cannot be found') end
  datablockName = name
  return true
end

-- a clip's length; the default sequence ('ambient') is the whole timeline
local function clipSeconds(name)
  local c = clipsByName[name]
  if c then return c.seconds end
  return timelineSeconds
end

-- playback speed: the clip's own speed (refKmh) gives 1x, held between its minRate and maxRate
local function rateFor(name, kmh)
  local c = clipsByName[name]
  if not c then return 1 end
  local ref, low, high = tonumber(c.refKmh) or 0, tonumber(c.minRate) or 1, tonumber(c.maxRate) or 1
  high = math.max(low, high)
  if ref <= 0 then return low end
  return math.max(low, math.min(high, kmh / ref))
end

-- engine mode: the thread runs at the rate (sent again only when it moved by more than 0.02);
-- manual mode: the thread stands still and Lua sets its position every frame
local function setRate(o, rate)
  clipRate = rate
  local scale = driveMode == 'manual' and 0 or rate
  if appliedScale and math.abs(scale - appliedScale) <= 0.02 then return end
  local ok, err = pcall(o.setThreadTimeScale, o, 0, scale)
  if ok then appliedScale = scale else report('E', 'timeScale', 'setThreadTimeScale failed: ' .. tostring(err)) end
end

local function playClip(o, name, rate)
  local ok, played = pcall(o.playThread, o, 0, name)
  playing = (ok and played) and name or nil
  if not playing then
    missingClips[name] = true
    report('W', 'clip:' .. name, 'the shape has no sequence "' .. name .. '": playing its whole timeline (ambient) instead')
    if name ~= 'ambient' then
      local okA, playedA = pcall(o.playThread, o, 0, 'ambient')
      playing = (okA and playedA) and 'ambient' or nil
    end
  end
  clip, clipSince, clipPhase, appliedScale = name, clock, 0, nil
  setRate(o, rate)
end

-- which clip now: the test's choice, else in the air, idle, boost or run by speed
local function chooseClip(boosting)
  if debugClip then return debugClip, debugRate end
  local name = airborne and roleClip.air or nil
  if not name then
    if speedKmh < tuning.idleBelowKmh then name = roleClip.idle
    elseif boosting then name = roleClip.boost or roleClip.run or roleClip.idle
    else name = roleClip.run or roleClip.idle end
  end
  return name, rateFor(name, speedKmh)
end

-- the body's state when sonicnative_body is loaded and drives the walker:
-- {active, boosting, airborne, speedKmh}
local function bodyState()
  if not extensions.isExtensionLoaded('sonicnative_body') then return nil end
  local body = extensions.sonicnative_body
  if type(body) ~= 'table' or type(body.getState) ~= 'function' then return nil end
  local ok, s = pcall(body.getState)
  if ok and type(s) == 'table' and s.active then return s end
end

local function currentWalker()
  if not gameplay_walk or not gameplay_walk.isWalking() then return nil end
  return gameplay_walk.getCurrentUnicycle()
end

-- BeamNG 0.40.1's walker is a whole character, and the game shows its meshes again on its own
-- (camera changes, physics resets): hide them again on those events and twice a second anyway
local function hideWalker(walker)
  local id = walker:getId()
  if hiddenWalker == id and clock - hiddenAt <= REHIDE_SECONDS then return end
  if hiddenWalker and hiddenWalker ~= id then
    local old = be:getObjectByID(hiddenWalker)
    if old then pcall(old.setMeshAlpha, old, 1, '') end
  end
  hiddenWalker, hiddenAt = id, clock
  walker:setMeshAlpha(0, '')
end

local function restoreWalker()
  if not hiddenWalker then return end
  local w = be:getObjectByID(hiddenWalker)
  if w then pcall(w.setMeshAlpha, w, 1, '') end
  hiddenWalker = nil
end

-- Feet: the walker's box centre brought down to the ground right under it (one ray), or to the
-- bottom of the box when the ground is further than a step (jumping, falling)
local function readFeet(walker)
  local id = walker:getId()
  center:set(be:getObjectOOBBCenterXYZ(id))
  local half = 0.3
  for axis = 0, 2 do
    local _, _, z = be:getObjectOOBBHalfAxisXYZ(id, axis)
    half = math.max(half, math.abs(z))
  end
  local range = half + 3
  local d = castRayStatic(center, down, range)
  local grounded = d ~= nil and d < range and d <= half + GROUND_PROBE
  feet:set(center.x, center.y, center.z - (grounded and d or half) + tuning.feetOffset)
  return grounded
end

local function despawn()
  local o = scenetree.findObject(OBJECT_NAME)
  if o then pcall(o.delete, o) end
  spawned, rendered, clip, playing, appliedScale = false, false, nil, nil, nil
end

local function spawn()
  despawn()
  lastError, missingClips = nil, {}
  if not levelLoaded() or not readManifest() then return false end
  ensureMaterials()
  runConstructor()
  if not ensureDatablock() then return false end
  local o
  local ok, err = pcall(function()
    o = createObject('StaticShape')
    o:setField('dataBlock', 0, datablockName)   -- before registerObject, or ShapeBase::onAdd has no datablock
    local s = tonumber(manifest.scale) or 1
    o:setField('scale', 0, string.format('%.6f %.6f %.6f', s, s, s))
    o.canSave = false                           -- never written into a level by the editor
    -- what registerObject returns to Lua is not verified: the scene tree tells whether it worked
    o:registerObject(OBJECT_NAME)
    if not scenetree.findObject(OBJECT_NAME) then error('registerObject refused the object') end
  end)
  if not ok then
    -- only a registered object is deleted: Torque's deleteObject expects one
    local stray = scenetree.findObject(OBJECT_NAME)
    if stray then pcall(stray.delete, stray) end
    report('E', 'spawn', 'cannot spawn Sonic (' .. tostring(datablockName) .. '): ' .. tostring(err))
    return false
  end
  pcall(o.setRenderEnabled, o, false)          -- shown on the next frame if the player is on foot
  spawned, rendered = true, false
  playClip(o, roleClip.idle, rateFor(roleClip.idle, 0))
  log('I', logTag, 'Sonic spawned from ' .. shapePath)
  return true
end

local function update(dt)
  clock = clock + dt
  local walker = currentWalker()
  walking = walker ~= nil
  local o = spawned and scenetree.findObject(OBJECT_NAME) or nil
  if spawned and not o then spawned, rendered = false, false end   -- deleted under us (level end, editor)
  if not o or not walker then
    -- no Sonic, in a car or no player: the walker is BeamNG's again
    if o and rendered then o:setRenderEnabled(false) rendered = false end
    restoreWalker()
    lastWalkerId = nil
    return
  end

  -- on foot again or a new walker (respawn): his own facing and speed, not the last run's
  local walkerId = walker:getId()
  if walkerId ~= lastWalkerId then
    headingSet, lastSpeed, lastVx, lastVy, speedKmh, airTime = false, 0, 0, 0, 0, 0
    lastWalkerId = walkerId
  end

  local grounded = readFeet(walker)
  local vx, vy = walker:getVelocityXYZ()
  local speed = math.sqrt(vx * vx + vy * vy)
  -- a respawn or teleport shows up as an absurd one-frame velocity: keep the last real one
  if speed == speed and speed - lastSpeed < 40 then lastSpeed, lastVx, lastVy = speed, vx, vy end
  if lastSpeed > HEADING_MIN_SPEED then
    local target = math.atan2(lastVy, lastVx)
    if not headingSet then heading, headingSet = target, true end
    local delta = (target - heading + math.pi) % (2 * math.pi) - math.pi
    heading = heading + delta * math.min(1, dt * FACING_RATE)
  elseif not headingSet then
    local dir = walker:getDirectionVector()
    heading, headingSet = math.atan2(dir.y, dir.x), true
  end

  local body = bodyState()
  usingBody = body ~= nil
  local kmh = lastSpeed * 3.6
  local boosting = speedKmh >= tuning.boostAboveKmh
  local offGround = not grounded
  if body then
    kmh = tonumber(body.speedKmh) or kmh
    boosting = body.boosting == true
    -- swimming or on a wall is not flying (beamng/src/game.cpp)
    offGround = body.airborne == true and not body.onWall and body.water ~= 2
  end
  airTime = offGround and airTime + dt or 0
  airborne = airTime > AIR_DELAY
  speedKmh = speedKmh + (kmh - speedKmh) * (1 - math.exp(-dt / SPEED_SMOOTHING))

  -- the converted model faces +Y: turn +Y toward his heading, +Z up
  local yaw = heading + math.rad(tuning.facingOffsetDeg)
  face:set(math.cos(yaw), math.sin(yaw), 0)
  rot:setFromDir(face, up)
  o:setPosRot(feet.x, feet.y, feet.z, rot.x, rot.y, rot.z, rot.w)
  if not rendered then o:setRenderEnabled(true) rendered = true end
  hideWalker(walker)

  local name, rate = chooseClip(boosting)
  if name ~= clip and (debugClip or clock - clipSince >= tuning.clipHoldSeconds) then
    playClip(o, name, rate)
  else
    if name ~= clip then rate = rateFor(clip, speedKmh) end   -- held: the current clip at its own rate
    setRate(o, rate)
  end
  if driveMode == 'manual' and playing then
    clipPhase = (clipPhase + dt * clipRate / clipSeconds(playing)) % 1
    o:setThreadPosition(0, clipPhase)
  end
end

local function onPreRender(dtReal)
  local ok, err = pcall(update, dtReal or 0)
  if not ok then report('E', 'frame', 'Sonic update failed: ' .. tostring(err)) end
end

local function onClientPostStartMission()
  headingSet, lastSpeed, speedKmh, airTime, hiddenWalker = false, 0, 0, 0, nil
  spawn()
end

local function onClientEndMission()
  despawn()
  hiddenWalker = nil
end

local function onExtensionLoaded()
  log('I', logTag, 'native Sonic loaded')
  if levelLoaded() then spawn() end
end

local function onExtensionUnloaded()
  restoreWalker()
  despawn()
end

local function onModDeactivated(mod)
  if type(mod) ~= 'table' or mod.modname ~= 'sonicnative' then return end
  restoreWalker()
  despawn()
  -- off the manual list, or every later mod activation tries to load it from the unmounted mod
  -- (modScript.lua sets it back to manual when the mod is turned on again)
  setExtensionUnloadMode('sonicnative_main', 'auto')
  extensions.unload('sonicnative_main')
end

-- the game may have shown the walker's meshes again: hide them on the next frame
local function onCameraChanged()
  hiddenAt = -100
end

local function onVehicleResetted(id)
  if id == hiddenWalker then hiddenAt = -100 end
end

-- Developer API, for the MCP run_lua tool: return jsonEncode(sonicnative_main.status())
M.status = function()
  local o = scenetree.findObject(OBJECT_NAME)
  local shown, model = nil, nil
  if o then
    local ok, r = pcall(o.isRenderEnabled, o)
    if ok then shown = r end
    ok, r = pcall(o.getModelFile, o)   -- the shape the engine really loaded
    if ok then model = r end
  end
  local missing = {}
  for n in pairs(missingClips) do missing[#missing + 1] = n end
  local cache = nil
  if shapePath then
    -- where BeamNG compiled the shape (.cdae): next to the DAE or under temp/ (not verified yet)
    local cdae = (shapePath:gsub('%.dae$', '.cdae'))
    cache = {besideShape = FS:fileExists(cdae), inTemp = FS:fileExists('/temp' .. cdae)}
  end
  return {
    exists = o ~= nil, clip = clip, sequence = playing, rate = clipRate, timeScale = appliedScale,
    mode = driveMode, phase = clipPhase, debugClip = debugClip,
    pos = {x = feet.x, y = feet.y, z = feet.z}, heading = heading,
    face = {x = math.cos(heading), y = math.sin(heading), z = 0},
    walking = walking, rendered = shown, speedKmh = speedKmh, airborne = airborne, body = usingBody,
    shape = shapePath, modelFile = model, datablock = datablockName, manifestOk = manifestOk,
    materialsOk = materialsOk, missingMaterials = missingMaterials, missingClips = missing,
    cache = cache, lastError = lastError,
  }
end

-- forces a clip (name) at a rate, or gives the choice back to Sonic's speed (nil)
M.setDebugClip = function(name, rate)
  debugClip = type(name) == 'string' and name or nil
  debugRate = tonumber(rate) or 1
  clipSince = -100
  return debugClip or 'automatic'
end

M.setDriveMode = function(mode)
  if mode ~= 'engine' and mode ~= 'manual' then return 'unknown mode ' .. tostring(mode) .. ' (engine or manual)' end
  driveMode, appliedScale, clipPhase = mode, nil, 0
  local o = scenetree.findObject(OBJECT_NAME)
  if o and clip then setRate(o, clipRate) end
  return driveMode
end

M.respawn = function()
  warned = {}
  spawn()
  return M.status()
end

-- which clips the shape really has as sequences: tells whether <library_animation_clips> became
-- named sequences or '--split cs' is needed. Tried on thread slot 0, then the current clip starts
-- again: a stopped thread on another slot would keep posing the bones (StaticShape cannot destroy it).
M.checkClips = function()
  local o = scenetree.findObject(OBJECT_NAME)
  if not o then return {error = 'Sonic is not spawned'} end
  local names = {'ambient'}
  for n in pairs(clipsByName) do names[#names + 1] = n end
  local found = {}
  for _, n in ipairs(names) do
    local ok, played = pcall(o.playThread, o, 0, n)
    found[n] = ok and played == true
  end
  local c, r = clip, clipRate
  clip = nil
  if c then playClip(o, c, r) end
  return found
end

-- changes prototype settings live (numbers only), returns them all
M.setTuning = function(values)
  for k, v in pairs(type(values) == 'table' and values or {}) do
    if type(tuning[k]) == 'number' and type(v) == 'number' then tuning[k] = v end
  end
  return tuning
end

M.onExtensionLoaded = onExtensionLoaded
M.onExtensionUnloaded = onExtensionUnloaded
M.onClientPostStartMission = onClientPostStartMission
M.onClientEndMission = onClientEndMission
M.onModDeactivated = onModDeactivated
M.onPreRender = onPreRender
M.onCameraChanged = onCameraChanged
M.onVehicleResetted = onVehicleResetted

return M
