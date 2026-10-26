-- SPDX-License-Identifier: MIT
-- Sonic Beam, native mode: Sonic is a real object of BeamNG's scene (a StaticShape), lit, sorted
-- and drawn by the game itself, with no DLL. The converter (natif/converter) reads Sonic from the
-- player's own Sonic Frontiers and writes his shape, materials and textures into the user folder
-- (mods/sonicnative_data). This extension loads them, keeps Sonic at the walker's feet every
-- frame, hides the walker and picks Sonic's animation with the overlay's state machine
-- (beamng/src/game.cpp, its numbers copied into lua/common/sonicnative/anim.lua): states by
-- speed, ground, boost key and turn rate, the landing hold, the jump ball, tricks, wall runs and
-- swimming. When the converter also wrote Frontiers' spin ball shell, it is a second StaticShape
-- that rolls around him during the jump ball. His body is sonicnative_body.

local M = {}
local logTag = 'sonicnative'

local A = require('sonicnative/anim')
local T = A.tuning
local STATES = A.states

local DATA = '/mods/sonicnative_data/'    -- written on the player's PC by the converter, never shipped
local OBJECT_NAME = 'sonicnative_sonic'
local DEFAULT_DATABLOCK = 'sonicnative_db'
local BALL_NAME = 'sonicnative_ball'
local DEFAULT_BALL_DATABLOCK = 'sonicnative_ball_db'

-- Copied from the overlay mode (beamng/), never read from it at run time:
local GROUND_PROBE = 0.35       -- m, gap under the body that still counts as ground (beamng/sheets/physics.json groundProbe)
local REHIDE_SECONDS = 0.5      -- the walker's meshes are hidden again this often (beamng/mod sonic.lua)

local IDLE_STATE, TRICK_STATE = #STATES, nil
local DRIFT_STATES = {}
for i, row in ipairs(STATES) do
  if row.id == 'idle' then IDLE_STATE = i end
  if row.id == 'trick' then TRICK_STATE = i end
  if row.id == 'drift_left' or row.id == 'drift_right' then DRIFT_STATES[i] = true end
end

-- Settings M.setTuning changes live (numbers only): the Lab (sonicnative_menu: jumpStyle,
-- ballSpinTurns, animSpeed, animLimit) and developer tests
local tuning = {
  facingOffsetDeg = 0,          -- turns the model about Z, in case the converted front is off
  feetOffset = 0,               -- metres added to the feet height
  jumpStyle = T.jumpStyle,      -- 0 ball the whole jump, 1 ball going up then tricks, 2 ball with tricks on a direction
  ballSpinTurns = -1,           -- turns/s of the jump ball; -1: the manifest's ball.spinSeconds, else anim.lua's
  ballSpinDirection = 1,        -- 1: the overlay's way (about cross(face, up): the top of the ball goes backward), -1: the other way
  leanDegrees = T.leanDegrees,
  turnRateDegrees = T.turnRateDegrees,
  animSpeed = T.animSpeed,
  animLimit = T.animLimit,
}

local manifest, manifestOk = nil, false
local shapePath, datablockName = nil, nil
local clipsByName, timelineSeconds = {}, 1
local resolved, idleClip = {}, 'ambient'   -- animation id -> sequence played for it; the last resort
local fallbackUsed = {}         -- animation id -> sequence played in its place (old conversion)
local materialsOk, missingMaterials = nil, {}
local constructorsRun = {}
local lastError = nil
local warned = {}               -- problems already logged: logged once, never every frame

local spawned, rendered = false, false
-- 'manual': Lua sets the thread's position each frame; 'engine': the engine advances the thread and
-- Lua only estimates where it is. Manual, so the pose drawn is the one Lua knows: the sharp motion
-- blur's mask follows his bones from that phase (in engine mode it drifted off his legs) and the
-- footsteps fall on the frames the converter found
local driveMode = 'manual'
local clip, playing = nil, nil  -- sequence asked for, sequence really playing ('ambient' when the clip is missing)
local clipRate, clipPhase = 1, 0
local playedSerial = -1         -- sm.animSerial the playing sequence was started for
local appliedScale = nil        -- thread time scale last given to the engine
local debugClip, debugRate = nil, 1
local missingClips = {}
local sonicHidden = false       -- Sonic's meshes hidden (only the ball shell shows)
local sonicScale = 1            -- the scale field Sonic was spawned with (the manifest's scale)
-- what place() last gave setPosRot, and the scale (for the boost effects' bone tracks, M.getAnim)
local placed = {x = 0, y = 0, z = 0, qx = 0, qy = 0, qz = 0, qw = 1, scale = 1}

-- Frontiers' spin ball shell (manifest "ball"), a second StaticShape
-- pivot: metres above his feet of the ball's middle, which he and the shell turn about (the
-- manifest's ball.pivotHeight, else anim.lua's)
local ball = {wanted = false, spawned = false, shown = false, shape = nil, datablock = nil, sequence = nil,
  turns = T.ballSpinTurns, pivot = T.sonicHeightMeters * T.ballPivotShare, centre = vec3(), centreFrom = 'none', error = nil}

local clock = 0
local walking = false
local usingBody = false
local hiddenWalker, hiddenAt = nil, -100
local lastWalkerId = nil        -- the walker Sonic followed last frame (nil: none)

-- the state machine (beamng/src/game.cpp, struct State): one table, read by status() and getAnim()
local sm = {}
local function resetMotion()
  sm.heading, sm.headingSet, sm.turnRate, sm.lean = 0, false, 0, 0
  sm.lastSpeed, sm.lastVx, sm.lastVy, sm.lastVz, sm.speedKmh = 0, 0, 0, 0, 0
  sm.air, sm.groundLost, sm.airTime = false, 0, 0
  sm.boostKey, sm.turn, sm.water, sm.wall, sm.airInput = false, 0, 0, 0, 0
  sm.state, sm.candidate, sm.candidateSince = IDLE_STATE, IDLE_STATE, 0
  sm.trickStep, sm.eventUntil = 0, 0
  sm.jumpAir, sm.pastApex, sm.jumpAt, sm.lastJumps = false, false, -100, nil
  sm.airTrick, sm.airTrickUntil = -1, 0    -- -1 none, -2 wait for the release, else an index of A.airTricks
  sm.ballAmount, sm.ballAngle, sm.rolling = 0, 0, false
  sm.anim, sm.animTime, sm.animRate = nil, 0, 1
  sm.why = 'state'
end
resetMotion()
-- counters other extensions compare (sonicnative_sound): they only ever grow
sm.stateCount, sm.airTricks, sm.animSerial = 0, 0, 0

local center, feet, face, tilt = vec3(), vec3(), vec3(0, 1, 0), vec3(0, 0, 1)
local down = vec3(0, 0, -1)
local rot = quat(0, 0, 0, 1)
local ballDir, ballUp, ballRot = vec3(0, 1, 0), vec3(0, 0, 1), quat(0, 0, 0, 1)

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

-- the sequence played for an animation id: its own clip, else its fallbacks (anim.lua), else idle;
-- nil for a clip that is skipped when missing (the land hold)
local function resolveClip(id)
  if clipsByName[id] then return id end
  local chain = A.fallbacks[id]
  if chain == false then return nil end
  for _, n in ipairs(type(chain) == 'table' and chain or {}) do
    if clipsByName[n] then return n end
  end
  return idleClip
end

local function readManifest()
  local m = jsonReadFile(DATA .. 'manifest.json')
  if type(m) ~= 'table' or type(m.shape) ~= 'string' then
    manifest, manifestOk, shapePath = nil, false, nil
    report('W', 'manifest', 'no converted Sonic in ' .. DATA .. ': run the converter (natif\\build.ps1 -Converter, then natif\\build\\sonicnative_convert.exe) with BeamNG closed')
    return false
  end
  manifest, manifestOk, shapePath = m, true, dataPath(m.shape)
  clipsByName, resolved, fallbackUsed = {}, {}, {}
  local timelineFps, keys = math.max(1, tonumber(m.fps) or 30), 0
  for _, c in ipairs(type(m.clips) == 'table' and m.clips or {}) do
    if type(c) == 'table' and type(c.name) == 'string' then
      local fps = math.max(1, tonumber(c.fps) or timelineFps)
      local frames = tonumber(c.frames) or fps + 1
      -- Torque plays a clip of n keys over (n-1)/fps, which the converter writes as seqSeconds
      local seconds = tonumber(c.seqSeconds) or tonumber(c.duration) or (frames - 1) / fps
      c.seconds = math.max(1 / 30, seconds)
      c.looping = c.loop ~= false   -- a one-shot (trick, land, jump start) holds its last frame
      -- toe-down phases (0..1 of the cycle) the converter found for the footsteps; {} for none
      local steps = {}
      for _, p in ipairs(type(c.steps) == 'table' and c.steps or {}) do
        p = tonumber(p)
        if p and p == p then steps[#steps + 1] = p % 1 end
      end
      c.steps = steps
      clipsByName[c.name] = c
      keys = keys + frames
    end
  end
  -- the clips lie end to end on one timeline: the default sequence plays all its keys
  timelineSeconds = math.max(1 / 30, (keys - 1) / timelineFps)
  -- no idle clip: the first clip, else the shape's default sequence
  local first = type(m.clips) == 'table' and m.clips[1]
  idleClip = clipsByName.idle and 'idle' or (type(first) == 'table' and clipsByName[first.name] and first.name) or 'ambient'
  for _, id in ipairs(A.ids) do
    resolved[id] = resolveClip(id)
    if resolved[id] ~= id then fallbackUsed[id] = resolved[id] or 'skipped' end
  end
  return true
end

-- Materials before the datablock: its preload imports the shape, which binds them by name
-- (a slot with no material is drawn orange). Loaded per level, as a level's end may drop them.
-- Returns true, false (some still missing) or nil (no file), and the names still missing.
local function loadMaterials(file, key)
  local missing = {}
  if not file then return nil, missing end
  local defs = jsonReadFile(file)
  if type(defs) ~= 'table' then
    report('E', key, 'cannot read the materials ' .. file)
    return false, missing
  end
  local absent = false
  for name in pairs(defs) do
    if not scenetree.findObject(name) then absent = true break end
  end
  if absent then
    local ok, res = pcall(loadJsonMaterialsFile, file)
    if not ok or res == false then report('E', key .. 'Load', 'loadJsonMaterialsFile failed on ' .. file .. ': ' .. tostring(res)) end
  end
  for name in pairs(defs) do
    if not scenetree.findObject(name) then missing[#missing + 1] = name end
  end
  if #missing > 0 then
    report('W', key .. 'Missing', 'materials still missing after loading ' .. file .. ': ' .. table.concat(missing, ', '))
  end
  return #missing == 0, missing
end

local function ensureMaterials()
  materialsOk, missingMaterials = loadMaterials(dataPath(manifest.materials), 'materials')
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
-- Returns the datablock's name, or nil.
local function declareDatablock(wanted, default, path, suffix, key)
  if path:find('["\\;]') then report('E', key .. 'Path', 'unusable shape path: ' .. path) return nil end
  local name = type(wanted) == 'string' and wanted or default
  name = name:gsub('[^%w_]', '_')
  local existing = scenetree.findObject(name)
  if existing then
    local ok, file = pcall(existing.getField, existing, 'shapeFile', 0)
    if ok and samePath(file, path) then return name end
    -- the converter wrote a new shape while the game ran: a datablock of its own for it
    name = name .. '_' .. tostring(suffix or 'new'):gsub('[^%w_]', '_')
    if scenetree.findObject(name) then return name end
  end
  local code = string.format('datablock StaticShapeData(%s) { shapeFile = "%s"; };', name, path)
  local ok, err = pcall(TorqueScript.eval, code)
  if not ok then report('E', key, 'datablock ' .. name .. ' failed: ' .. tostring(err)) return nil end
  if not scenetree.findObject(name) then report('W', key .. 'Missing', 'datablock ' .. name .. ' was declared but cannot be found') end
  return name
end

local function ensureDatablock()
  datablockName = declareDatablock(manifest.datablock, DEFAULT_DATABLOCK, shapePath, manifest.hash, 'datablock')
  return datablockName ~= nil
end

-- a registered StaticShape of this datablock and scale, hidden until the next frame; nil and the error
local function spawnObject(objectName, dbName, s)
  local o
  local ok, err = pcall(function()
    o = createObject('StaticShape')
    o:setField('dataBlock', 0, dbName)        -- before registerObject, or ShapeBase::onAdd has no datablock
    o:setField('scale', 0, string.format('%.6f %.6f %.6f', s, s, s))
    o.canSave = false                         -- never written into a level by the editor
    -- what registerObject returns to Lua is not verified: the scene tree tells whether it worked
    o:registerObject(objectName)
    if not scenetree.findObject(objectName) then error('registerObject refused the object') end
  end)
  if not ok then
    -- only a registered object is deleted: Torque's deleteObject expects one
    local stray = scenetree.findObject(objectName)
    if stray then pcall(stray.delete, stray) end
    return nil, err
  end
  pcall(o.setRenderEnabled, o, false)
  return o
end

-- a sequence's length; the default sequence ('ambient') is the whole timeline
local function clipSeconds(name)
  local c = clipsByName[name]
  if c then return c.seconds end
  return timelineSeconds
end

-- an animation id's length, as the clip that plays for it
local function animSeconds(id)
  local c = clipsByName[resolved[id] or '']
  return c and c.seconds or 1
end

local function clipLoops(name)
  local c = clipsByName[name]
  return c == nil or c.looping
end

-- Playback speed (game.cpp:360-371): a clip with a reference speed (refKmh) plays at 1x there and
-- follows Sonic's speed, eased into its ceiling (maxRate x the limit) so his limbs never flail; a
-- clip without one plays at 1x
local function rateFor(name, kmh)
  local c = clipsByName[name]
  local ref = c and tonumber(c.refKmh) or 0
  if ref <= 0 then return 1 end
  local low = tonumber(c.minRate) or 1
  local top = math.max(low, (tonumber(c.maxRate) or 1) * tuning.animLimit)
  local raw = kmh / math.max(1, ref) * tuning.animSpeed
  local knee = low + (top - low) * T.rateKnee
  local r = raw
  if raw > knee then r = knee + (top - knee) * (1 - math.exp(-(raw - knee) / math.max(0.01, top - knee))) end
  return math.max(low, math.min(top, r))
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

-- playThread starts the sequence from its first frame at time scale 1 (the rate is sent again)
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
  clip, clipPhase, appliedScale = name, 0, nil
  setRate(o, rate)
end

-- A one-shot that ends within the next two frames (by the phase estimate of update). Torque's
-- ShapeBase stops a thread at the end of a one-shot (Thread::Stop) and holds its last frame, but a
-- new time scale on a stopped thread puts it back on its first frame (updateThread; not verified in
-- BeamNG): from here on it gets none, so it holds its last pose as the overlay's did.
local function oneShotEnding(dt)
  if not playing or clipLoops(playing) then return false end
  local r = driveMode == 'engine' and appliedScale or clipRate
  return clipPhase + 2 * dt * math.abs(r or 1) / clipSeconds(playing) >= 1
end

-- the body's state when sonicnative_body is loaded and drives the walker:
-- {active, boosting, airborne, grounded, speedKmh, water, wall, wallNormal, jumps, airInput, ...}
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

-- the boost effects (sonicnative_fx), when loaded; never through extensions.<name> alone, which
-- would load it
local function fxModule()
  if not extensions.isExtensionLoaded('sonicnative_fx') then return nil end
  local fx = extensions.sonicnative_fx
  if type(fx) == 'table' then return fx end
end

-- the boost effects' frame, after Sonic's (o == nil: they hide)
local function runFx(dt, o)
  local fx = fxModule()
  if fx and fx.afterSonic then
    local ok, err = pcall(fx.afterSonic, dt, o)
    if not ok then report('E', 'fx', 'boost effects failed: ' .. tostring(err)) end
  end
end

-- Sonic's meshes hidden or shown again (only when it changes); showing them all shows the aura's
-- too, which the boost effects then set again
local function setSonicHidden(o, hidden)
  if hidden == sonicHidden or not o then return end
  local ok, err = pcall(o.setAllMeshesHidden, o, hidden)
  if ok then sonicHidden = hidden else report('E', 'meshHidden', 'setAllMeshesHidden failed: ' .. tostring(err)) end
  if ok and not hidden then
    local fx = fxModule()
    if fx and fx.onSonicMeshesShown then pcall(fx.onSonicMeshesShown, o) end
  end
end

local function despawnBall()
  local b = scenetree.findObject(BALL_NAME)
  if b then pcall(b.delete, b) end
  ball.spawned, ball.shown, ball.sequence = false, false, nil
end

-- Frontiers' spin ball shell (manifest "ball": shape, datablock, materials, scale, spinSeconds),
-- spawned like Sonic. Without it the jump ball is Sonic's own ball clip.
local function spawnBall()
  despawnBall()
  local b = manifest and manifest.ball
  ball.wanted, ball.error, ball.shape, ball.datablock = type(b) == 'table', nil, nil, nil
  ball.turns, ball.centreFrom = T.ballSpinTurns, 'none'
  ball.pivot = T.sonicHeightMeters * T.ballPivotShare
  ball.centre:set(0, 0, 0)
  if not ball.wanted then return false end
  local spin = tonumber(b.spinSeconds)
  if spin and spin > 0 then ball.turns = 1 / spin end
  local path = dataPath(b.shape)
  if not path then ball.error = 'the manifest\'s ball has no shape' return false end
  ball.shape = path
  if not FS:fileExists(path) then
    ball.error = 'ball shape not found: ' .. path
    report('W', 'ballShape', ball.error .. ' (run the converter again)')
    return false
  end
  local mats = dataPath(b.materials)
  if mats and not samePath(mats, dataPath(manifest.materials)) then loadMaterials(mats, 'ballMaterials') end
  ball.datablock = declareDatablock(b.datablock, DEFAULT_BALL_DATABLOCK, path, path:match('_(%x+)%.dae$') or manifest.hash, 'ballDatablock')
  if not ball.datablock then ball.error = 'no datablock for the ball' return false end
  local s = tonumber(b.scale) or tonumber(manifest.scale) or 1
  local o, err = spawnObject(BALL_NAME, ball.datablock, s)
  if not o then
    ball.error = 'cannot spawn the ball: ' .. tostring(err)
    report('E', 'ballSpawn', ball.error)
    return false
  end
  ball.spawned = true
  -- its own loop (Frontiers' spin_normal_loop, game.cpp:416) when the manifest names one; the
  -- converter bakes that loop's still pose into the shell instead
  if type(b.clip) == 'string' then
    local ok, played = pcall(o.playThread, o, 0, b.clip)
    if ok and played then ball.sequence = b.clip end
  end
  -- the shell's middle goes to the middle of the ball (game.cpp:411-412): the converter puts the
  -- shape's origin there; a "centre" in the manifest (model units) moves it
  ball.centreFrom = 'origin'
  local c = b.centre or b.center
  if type(c) == 'table' then
    local x, y, z = tonumber(c.x or c[1]), tonumber(c.y or c[2]), tonumber(c.z or c[3])
    if x and y and z then ball.centre:set(x * s, y * s, z * s) ball.centreFrom = 'manifest' end
  end
  local h = tonumber(b.pivotHeight)
  if h and h > 0 and h < 5 then ball.pivot = h end
  log('I', logTag, 'spin ball spawned from ' .. path)
  return true
end

local function despawn()
  despawnBall()
  local o = scenetree.findObject(OBJECT_NAME)
  if o then pcall(o.delete, o) end
  spawned, rendered, clip, playing, appliedScale, sonicHidden = false, false, nil, nil, nil, false
end

local function spawn()
  despawn()
  lastError, missingClips = nil, {}
  if not levelLoaded() or not readManifest() then return false end
  ensureMaterials()
  runConstructor()
  if not ensureDatablock() then return false end
  sonicScale = tonumber(manifest.scale) or 1
  local o, err = spawnObject(OBJECT_NAME, datablockName, sonicScale)
  if not o then
    report('E', 'spawn', 'cannot spawn Sonic (' .. tostring(datablockName) .. '): ' .. tostring(err))
    return false
  end
  spawned, rendered = true, false
  sm.anim, sm.animTime = nil, 0
  playClip(o, idleClip, 1)
  playedSerial = sm.animSerial
  log('I', logTag, 'Sonic spawned from ' .. shapePath)
  local okBall, errBall = pcall(spawnBall)
  if not okBall then report('E', 'ballSpawn', 'spin ball failed: ' .. tostring(errBall)) end
  return true
end

-- rotates (x, y, z) about the unit axis (nx, ny, nz) by the angle of cosine c and sine s (Rodrigues)
local function rotateAbout(x, y, z, nx, ny, nz, c, s)
  local d = (nx * x + ny * y + nz * z) * (1 - c)
  return x * c + (ny * z - nz * y) * s + nx * d,
         y * c + (nz * x - nx * z) * s + ny * d,
         z * c + (nx * y - ny * x) * s + nz * d
end

-- One frame of the overlay's motion reading and state selection (game.cpp:209-266, 277-279)
local function think(dt, walker, body, groundedByRay)
  local vx, vy, vz = walker:getVelocityXYZ()
  vx, vy, vz = tonumber(vx) or 0, tonumber(vy) or 0, tonumber(vz) or 0
  local water = body and tonumber(body.water) or 0
  local wall = body and tonumber(body.wall) or 0
  sm.water, sm.wall = water, wall

  -- speed: horizontal, plus the climb on a wall, smoothed. A respawn or teleport shows up as an
  -- absurd one-frame velocity: the last real one is kept
  local speed = math.sqrt(vx * vx + vy * vy)
  if speed == speed and vz == vz and speed - sm.lastSpeed < 40 then sm.lastSpeed, sm.lastVx, sm.lastVy, sm.lastVz = speed, vx, vy, vz end
  local climb = wall ~= 0 and sm.lastVz or 0
  local kmh = math.sqrt(sm.lastSpeed * sm.lastSpeed + climb * climb) * 3.6
  sm.speedKmh = sm.speedKmh + (kmh - sm.speedKmh) * (1 - math.exp(-dt / T.speedSmoothing))

  -- heading toward his run, and how fast it turns (deg/s smoothed: drift and lean)
  if sm.lastSpeed > T.headingMinSpeed then
    local target = math.atan2(sm.lastVy, sm.lastVx)
    if not sm.headingSet then sm.heading, sm.headingSet = target, true end
    local delta = (target - sm.heading + math.pi) % (2 * math.pi) - math.pi
    local step = delta * math.min(1, dt * T.facingRate * (water == 2 and T.swimTurnShare or 1))
    sm.heading = (sm.heading + step + math.pi) % (2 * math.pi) - math.pi
    sm.turnRate = sm.turnRate + (step / math.max(dt, 1e-3) - sm.turnRate) * math.min(1, dt * T.turnRateFollow)
  else
    sm.turnRate = sm.turnRate * T.turnRateDecay
    if not sm.headingSet then
      local dir = walker:getDirectionVector()
      sm.heading, sm.headingSet = math.atan2(dir.y, dir.x), true
    end
  end

  -- in the air after airDelay off the ground; swimming or on a wall is not flying
  local onGround = groundedByRay
  if body then
    if type(body.grounded) == 'boolean' then onGround = body.grounded else onGround = body.airborne ~= true end
  end
  local wasAir = sm.air
  if onGround or water == 2 or wall ~= 0 then
    sm.groundLost, sm.air = 0, false
  else
    sm.groundLost = sm.groundLost + dt
    if sm.groundLost > T.airDelay then sm.air = true end
  end
  if sm.air then sm.airTime = wasAir and sm.airTime + dt or sm.groundLost end
  local landed = wasAir and not sm.air
  local landedAir = landed and sm.airTime or 0
  if not sm.air and not landed then sm.airTime = 0 end

  -- the jump ball: this airtime started with a jump (the runner's jump counter grew); past the
  -- top once he falls (game.cpp:239-244)
  local jumps = body and tonumber(body.jumps) or nil
  local jumped = jumps ~= nil and sm.lastJumps ~= nil and jumps > sm.lastJumps
  sm.lastJumps = jumps
  local style = math.max(0, math.min(2, math.floor(tonumber(tuning.jumpStyle) or 0)))
  if jumped then sm.jumpAir, sm.pastApex, sm.jumpAt, sm.airTrick = true, false, clock, -1 end
  if landed or (sm.jumpAir and not sm.air and clock - sm.jumpAt > T.jumpLostAfter) then sm.jumpAir = false end
  -- native mode only: a held trick ends with its jump (the overlay kept it until the next jump,
  -- which blocked the landing hold and the trick state after landing mid-trick in jump style 2)
  if not sm.jumpAir then sm.airTrick = -1 end
  if sm.jumpAir and sm.lastVz < 0 and clock - sm.jumpAt > T.apexAfter then sm.pastApex = true end
  local ballJump = sm.jumpAir and not (style == 1 and sm.pastApex)

  local turnDeg = math.deg(sm.turnRate)
  local turn = (turnDeg > tuning.turnRateDegrees and 1) or (turnDeg < -tuning.turnRateDegrees and 2) or 0
  local boostKey = body ~= nil and body.boosting == true
  sm.turn, sm.boostKey = turn, boostKey

  -- the highest-priority state whose conditions hold (states.json); a new one starts once wanted
  -- for candidateDelay, at once to or from the air
  local best = nil
  for i = 1, #STATES do
    local row = STATES[i]
    if sm.speedKmh >= row.minKmh and sm.speedKmh < row.maxKmh
        and (row.air < 0 or (row.air == 1) == sm.air)
        and not (row.air == 1 and sm.airTime < row.minAirSeconds)
        and not (ballJump and i == TRICK_STATE)
        and (row.boost < 0 or (row.boost == 1) == boostKey)
        and (row.turn < 0 or row.turn == turn)
        and (not best or row.priority > STATES[best].priority) then
      best = i
    end
  end
  best = best or IDLE_STATE
  if best ~= sm.candidate then sm.candidate, sm.candidateSince = best, clock end
  local urgent = STATES[best].air == 1 or STATES[sm.state].air == 1
  if best ~= sm.state and (urgent or clock - sm.candidateSince >= T.candidateDelay) then
    sm.state = best
    sm.stateCount = sm.stateCount + 1
    if best == TRICK_STATE then sm.trickStep = math.random(0, #A.tricks - 1) end
  end

  -- the landing hold (events.json)
  if landed and landedAir >= A.landing.minAirSeconds then sm.eventUntil = clock + A.landing.holdSeconds end
  return ballJump, style, vx, vy, vz
end

-- The clip for this frame, in the overlay's order (game.cpp:320-353): a wall run, swimming, the
-- jump ball (jump start, then the ball), a trick held in the air, the trick state's tricks one
-- after the other, the landing hold, else the state's own clip
local function chooseAnim(ballJump, style, airInput)
  local stateRow = STATES[sm.state]
  local want, why = stateRow.animation, 'state'
  local ballNow = ballJump
  -- style 2: holding a direction in the air plays that trick, then Sonic curls up again
  if ballJump and style == 2 then
    if sm.airTrick < 0 and airInput > 0 and sm.air then
      sm.airTrick = A.airTricks[airInput] and airInput or 3
      sm.airTrickUntil = clock + animSeconds(A.airTricks[sm.airTrick]) + T.airTrickPause
      sm.airTricks = sm.airTricks + 1
    end
    -- one trick per press: still held when it ends, he curls up until the direction is released
    -- (native mode: game.cpp:330 sets -1 and -2 the other way round from its own comment, see anim.lua)
    if sm.airTrick > 0 and clock >= sm.airTrickUntil then
      if airInput > 0 then sm.airTrick = -2 else sm.airTrick = -1 end
    end
    if sm.airTrick == -2 and airInput == 0 then sm.airTrick = -1 end
    if sm.airTrick > 0 then ballNow, want, why = false, A.airTricks[sm.airTrick], 'trick held in the air' end
  end
  if sm.wall ~= 0 then
    -- Frontiers' wall run: up the wall, or along it with the wall on his left or right
    want, why, ballNow = A.wallClips[sm.wall] or A.wallClips[3], 'wall run', false
  elseif sm.water == 2 then
    want, why, ballNow = 'swim', 'swimming', false   -- out of his depth and too slow to run on the water
  elseif ballNow then
    local start = math.min(T.jumpStartMax, animSeconds('jump_start'))
    if clock - sm.jumpAt < start then want, why = 'jump_start', 'jump start' else want, why = 'ball', 'jump ball' end
  elseif sm.airTrick > 0 then
    -- the held trick chosen above
  elseif sm.state == TRICK_STATE then
    local n = #A.tricks
    local current = A.tricks[sm.trickStep % n + 1]
    if sm.anim == current and sm.animTime > animSeconds(current) + T.trickPause then sm.trickStep = sm.trickStep + 1 end
    want, why = A.tricks[sm.trickStep % n + 1], 'trick state'
  elseif clock < sm.eventUntil and not sm.air and resolved.land then
    want, why = A.landing.animation, 'landing hold'
  end
  if not resolved[want] then want, why = stateRow.animation, why .. ' (clip missing)' end
  sm.why = why
  return want, ballNow
end

-- the clip's playback: a new animation id starts at time 0 with the rate carried over; the rate
-- follows its target (game.cpp:78-84, 372-373)
local function advanceAnim(dt, want)
  if want ~= sm.anim then
    sm.anim, sm.animTime = want, 0
    sm.animSerial = sm.animSerial + 1
  end
  local target = rateFor(resolved[want], sm.speedKmh)
  sm.animRate = sm.animRate + (target - sm.animRate) * math.min(1, dt * T.rateFollow)
  sm.animTime = sm.animTime + dt * sm.animRate
end

-- Sonic in the world: facing his run and leaning into turns, on a wall with his feet on it, or
-- rolling in the jump ball about his side axis through the ball's middle (game.cpp:380-408)
local function place(o, dt, body, vx, vy, vz)
  local drifting = DRIFT_STATES[sm.state] == true
  local targetLean = 0
  if not drifting and sm.water ~= 2 then
    targetLean = math.max(-1, math.min(1, math.deg(sm.turnRate) / T.leanFullTurn)) * math.rad(tuning.leanDegrees)
  end
  sm.lean = sm.lean + (targetLean - sm.lean) * math.min(1, dt * T.leanFollow)

  local yaw = sm.heading + math.rad(tuning.facingOffsetDeg)
  local fx, fy = math.cos(yaw), math.sin(yaw)
  local px, py, pz = feet.x, feet.y, feet.z
  local dx, dy, dz, ux, uy, uz
  local wn = sm.wall ~= 0 and body and body.wallNormal or nil
  local nx, ny, nz = 0, 0, 1
  if wn then
    nx, ny, nz = tonumber(wn.x) or 0, tonumber(wn.y) or 0, tonumber(wn.z) or 0
    local l = math.sqrt(nx * nx + ny * ny + nz * nz)
    if l > 0.1 then nx, ny, nz = nx / l, ny / l, nz / l else wn = nil end
  end
  if wn then
    -- up is the wall's normal; he faces his run along it, or up the wall when barely moving
    local along = vx * nx + vy * ny + vz * nz
    local rx, ry, rz = vx - nx * along, vy - ny * along, vz - nz * along
    local rl = math.sqrt(rx * rx + ry * ry + rz * rz)
    if rl <= T.wallRunMinSpeed then
      rx, ry, rz = -nx * nz, -ny * nz, 1 - nz * nz
      rl = math.sqrt(rx * rx + ry * ry + rz * rz)
    end
    if rl < 1e-4 then rx, ry, rz, rl = fx, fy, 0, 1 end
    dx, dy, dz, ux, uy, uz = rx / rl, ry / rl, rz / rl, nx, ny, nz
  else
    local sl, cl = math.sin(sm.lean), math.cos(sm.lean)
    dx, dy, dz = fx, fy, 0
    ux, uy, uz = -fy * sl, fx * sl, cl            -- up tilted toward his left when turning left
    if sm.ballAngle ~= 0 then
      local c, s = math.cos(sm.ballAngle), math.sin(sm.ballAngle)
      local rx, ry = fy, -fx                      -- his side axis: cross(face, up)
      dx, dy, dz = rotateAbout(dx, dy, dz, rx, ry, 0, c, s)
      ux, uy, uz = rotateAbout(ux, uy, uz, rx, ry, 0, c, s)
      local h = ball.pivot
      local ox, oy, oz = rotateAbout(0, 0, -h, rx, ry, 0, c, s)   -- his feet about the pivot
      px, py, pz = feet.x + ox, feet.y + oy, feet.z + h + oz
    end
  end
  face:set(dx, dy, dz)
  tilt:set(ux, uy, uz)
  rot:setFromDir(face, tilt)
  o:setPosRot(px, py, pz, rot.x, rot.y, rot.z, rot.w)
  placed.x, placed.y, placed.z, placed.qx, placed.qy, placed.qz, placed.qw = px, py, pz, rot.x, rot.y, rot.z, rot.w
  placed.scale = sonicScale
end

-- the shell around the ball's middle, turned with him (game.cpp:410-414)
local function placeBall(b)
  local yaw = sm.heading + math.rad(tuning.facingOffsetDeg)
  local fx, fy = math.cos(yaw), math.sin(yaw)
  local c, s = math.cos(sm.ballAngle), math.sin(sm.ballAngle)
  local rx, ry = fy, -fx
  local dx, dy, dz = rotateAbout(fx, fy, 0, rx, ry, 0, c, s)
  local ux, uy, uz = rotateAbout(0, 0, 1, rx, ry, 0, c, s)
  -- the model's X, Y, Z in the world: dir x up, dir, up
  local xx, xy, xz = dy * uz - dz * uy, dz * ux - dx * uz, dx * uy - dy * ux
  local cx, cy, cz = ball.centre.x, ball.centre.y, ball.centre.z
  local h = ball.pivot
  ballDir:set(dx, dy, dz)
  ballUp:set(ux, uy, uz)
  ballRot:setFromDir(ballDir, ballUp)
  b:setPosRot(feet.x - (xx * cx + dx * cy + ux * cz), feet.y - (xy * cx + dy * cy + uy * cz),
    feet.z + h - (xz * cx + dz * cy + uz * cz), ballRot.x, ballRot.y, ballRot.z, ballRot.w)
end

-- the ball forms while he rolls and spins at its turns per second; like Frontiers, only the shell
-- shows once it has formed (with no fade in BeamNG, the shell shows from that moment too)
local function updateBall(dt, o, rolling)
  sm.rolling = rolling
  sm.ballAmount = sm.ballAmount + ((rolling and 1 or 0) - sm.ballAmount) * math.min(1, dt * (rolling and T.ballIn or T.ballOut))
  if rolling then
    local turns = tuning.ballSpinTurns >= 0 and tuning.ballSpinTurns or ball.turns
    local way = tuning.ballSpinDirection < 0 and -1 or 1
    sm.ballAngle = (sm.ballAngle + way * dt * math.max(0, turns) * 2 * math.pi) % (2 * math.pi)
  else
    sm.ballAngle = 0
  end
  local b = ball.spawned and scenetree.findObject(BALL_NAME) or nil
  if ball.spawned and not b then ball.spawned, ball.shown = false, false end   -- deleted under us
  local formed = b ~= nil and rendered and sm.ballAmount > T.ballFormed
  if b then
    if formed then placeBall(b) end
    if formed ~= ball.shown then
      pcall(b.setRenderEnabled, b, formed)
      ball.shown = formed
    end
  end
  setSonicHidden(o, formed)
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
    if ball.shown then
      local b = scenetree.findObject(BALL_NAME)
      if b then pcall(b.setRenderEnabled, b, false) end
      ball.shown = false
    end
    restoreWalker()
    lastWalkerId = nil
    runFx(math.min(T.maxDt, dt), nil)
    return
  end

  -- on foot again or a new walker (respawn): his own facing, speed and state, not the last run's
  local walkerId = walker:getId()
  if walkerId ~= lastWalkerId then
    resetMotion()
    lastWalkerId = walkerId
  end

  local sdt = math.min(T.maxDt, dt)
  local groundedByRay = readFeet(walker)
  local body = bodyState()
  usingBody = body ~= nil
  local ballJump, style, vx, vy, vz = think(sdt, walker, body, groundedByRay)
  local airInput = body and tonumber(body.airInput) or 0
  local want, ballNow = chooseAnim(ballJump, style, airInput)
  advanceAnim(sdt, want)
  -- he rolls in his ball clip (or inside the shell); a shape converted without a ball clip shows
  -- its fallback (fall) unspun
  local rolling = ballNow and sm.anim == 'ball' and not debugClip and (resolved.ball == 'ball' or ball.spawned)

  if not rendered then o:setRenderEnabled(true) rendered = true end
  updateBall(sdt, o, rolling)
  place(o, sdt, body, sm.lastVx, sm.lastVy, sm.lastVz)
  hideWalker(walker)

  -- the sequence: the test's choice, else the one for this animation id. A new id with the same
  -- looping sequence keeps it running (no restart, no pop); a one-shot starts again
  local name, rate
  if debugClip then name, rate = debugClip, debugRate
  else name, rate = resolved[sm.anim] or idleClip, sm.animRate end
  if name ~= clip or (not debugClip and playedSerial ~= sm.animSerial and not clipLoops(name)) then
    playClip(o, name, rate)
  elseif driveMode == 'engine' and oneShotEnding(dt) then
    clipRate = rate   -- the engine holds the one-shot's last frame: a new time scale would rewind it
  else
    setRate(o, rate)
  end
  playedSerial = sm.animSerial
  -- the sequence's phase (0..1): Lua sets it in manual mode; in engine mode it is an estimate for
  -- the footsteps (M.getAnim), advanced at the scale the engine was given and reset by playClip.
  -- A one-shot holds its last frame.
  if playing then
    local r = driveMode == 'engine' and appliedScale or clipRate
    local p = clipPhase + dt * (r or 1) / clipSeconds(playing)
    if clipLoops(playing) then p = p % 1 else p = math.min(1, p) end
    clipPhase = p
    if driveMode == 'manual' then o:setThreadPosition(0, clipPhase) end
  end
  -- vx, vy, vz: this frame's raw velocity (status only)
  sm.vx, sm.vy, sm.vz = vx, vy, vz
  -- the boost effects, around Sonic as he is now
  runFx(sdt, o)
end

local function onPreRender(dtReal)
  local ok, err = pcall(update, dtReal or 0)
  if not ok then report('E', 'frame', 'Sonic update failed: ' .. tostring(err)) end
end

local function onClientPostStartMission()
  resetMotion()
  hiddenWalker = nil
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

local AIR_TEXT = {[-1] = 'any', [0] = 'ground', [1] = 'air'}
local YES_TEXT = {[-1] = 'any', [0] = 'no', [1] = 'yes'}
local TURN_TEXT = {[-1] = 'any', [0] = 'none', [1] = 'left', [2] = 'right'}

-- why the current state holds: its row's conditions next to the values read this frame
local function stateWhy()
  local row = STATES[sm.state]
  return string.format('%s (priority %d): %.0f km/h in [%g, %g); %s, wanted %s (%s, %.2f s, min %.2f s); boost key %s, wanted %s; turn %s (%.0f deg/s), wanted %s',
    row.id, row.priority, sm.speedKmh, row.minKmh, row.maxKmh, sm.air and 'air' or 'ground', AIR_TEXT[row.air] or '?',
    sm.air and 'airborne' or 'grounded', sm.airTime, row.minAirSeconds, tostring(sm.boostKey), YES_TEXT[row.boost] or '?',
    TURN_TEXT[sm.turn] or '?', math.deg(sm.turnRate), TURN_TEXT[row.turn] or '?')
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
  local airTrick = sm.airTrick > 0 and A.airTricks[sm.airTrick] or (sm.airTrick == -2 and 'waiting for the release' or nil)
  return {
    exists = o ~= nil, clip = clip, sequence = playing, rate = clipRate, timeScale = appliedScale,
    mode = driveMode, phase = clipPhase, debugClip = debugClip,
    state = STATES[sm.state].id, stateWhy = stateWhy(), candidate = STATES[sm.candidate].id,
    anim = sm.anim, animWhy = sm.why, animTime = sm.animTime, animRate = sm.animRate,
    turnRateDeg = math.deg(sm.turnRate), leanDeg = math.deg(sm.lean), boostKey = sm.boostKey,
    air = sm.air, airTime = sm.airTime, water = sm.water, wall = sm.wall,
    landingHold = math.max(0, sm.eventUntil - clock), jumpStyle = tuning.jumpStyle, jumpAir = sm.jumpAir,
    pastApex = sm.pastApex, airTrick = airTrick, stateCount = sm.stateCount,
    ball = {wanted = ball.wanted, spawned = ball.spawned, shown = ball.shown, amount = sm.ballAmount, rolling = sm.rolling,
      angleDeg = math.deg(sm.ballAngle), turns = tuning.ballSpinTurns >= 0 and tuning.ballSpinTurns or ball.turns,
      sequence = ball.sequence, shape = ball.shape, datablock = ball.datablock, pivot = ball.pivot, centreFrom = ball.centreFrom,
      centre = {x = ball.centre.x, y = ball.centre.y, z = ball.centre.z}, error = ball.error},
    sonicHidden = sonicHidden, fallbacks = fallbackUsed,
    pos = {x = feet.x, y = feet.y, z = feet.z}, heading = sm.heading,
    face = {x = face.x, y = face.y, z = face.z}, up = {x = tilt.x, y = tilt.y, z = tilt.z},
    walking = walking, rendered = shown, speedKmh = sm.speedKmh, airborne = sm.air, body = usingBody,
    shape = shapePath, modelFile = model, datablock = datablockName, manifestOk = manifestOk,
    materialsOk = materialsOk, missingMaterials = missingMaterials, missingClips = missing,
    cache = cache, lastError = lastError,
  }
end

-- For the sounds (sonicnative_sound): the animation id playing (clip; the forced sequence in a
-- test), the sequence really playing, its phase (0..1, estimated in engine mode), its rate and its
-- footstep phases from the manifest; the state (id), how many states have started so far
-- (stateCount) and the current state's enterSound; how many tricks were held in the air
-- (airTricks); whether only the ball shell shows. For the boost effects (sonicnative_fx): his
-- smoothed speed (speedKmh, horizontal plus the climb on a wall), how far the jump ball has formed
-- (ballAmount 0..1), the boost key, his heading (rad), his feet (a vec3), his velocity (vel =
-- {vx, vy, vz}, the last real one), what Sonic's StaticShape was given (place: setPosRot's x, y,
-- z, qx, qy, qz, qw and the scale field), his up before the ball's roll (up, a vec3) and how high
-- above his feet the ball's middle is (ballPivot, m). The same tables every call: read them, do not
-- keep them.
local NO_STEPS = {}
local animVel = {0, 0, 0}
local anim = {clip = nil, sequence = nil, phase = 0, rate = 1, steps = NO_STEPS, shown = false,
  state = nil, stateCount = 0, enterSound = nil, airTricks = 0, ball = false,
  speedKmh = 0, ballAmount = 0, boostKey = false, heading = 0, feet = feet, vel = animVel, place = placed,
  up = tilt, ballPivot = 0}
M.getAnim = function()
  local c = playing and clipsByName[playing]
  local row = STATES[sm.state]
  anim.clip = playing and (debugClip or sm.anim) or nil
  anim.sequence, anim.phase, anim.shown = playing, clipPhase, rendered
  anim.rate = driveMode == 'engine' and appliedScale or clipRate
  anim.steps = c and c.steps or NO_STEPS
  anim.state, anim.stateCount, anim.enterSound = row.id, sm.stateCount, row.enterSound
  anim.airTricks, anim.ball = sm.airTricks, ball.shown
  anim.speedKmh, anim.ballAmount, anim.boostKey, anim.heading = sm.speedKmh, sm.ballAmount, sm.boostKey, sm.heading
  animVel[1], animVel[2], animVel[3] = sm.lastVx, sm.lastVy, sm.lastVz
  anim.feet, anim.vel, anim.place, anim.up, anim.ballPivot = feet, animVel, placed, tilt, ball.pivot
  return anim
end

-- forces a sequence (name) at a rate, or gives the choice back to the state machine (nil)
M.setDebugClip = function(name, rate)
  debugClip = type(name) == 'string' and name or nil
  debugRate = tonumber(rate) or 1
  clip = nil   -- starts again on the next frame
  return debugClip or 'automatic'
end

M.setDriveMode = function(mode)
  if mode ~= 'engine' and mode ~= 'manual' then return 'unknown mode ' .. tostring(mode) .. ' (engine or manual)' end
  -- the phase carries on from where the thread is; a one-shot that has ended keeps its last frame
  -- (no new time scale in engine mode, see oneShotEnding)
  driveMode, appliedScale = mode, nil
  local o = scenetree.findObject(OBJECT_NAME)
  if o and clip and not (mode == 'engine' and oneShotEnding(0)) then setRate(o, clipRate) end
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

-- changes settings live (numbers only), returns them all
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
