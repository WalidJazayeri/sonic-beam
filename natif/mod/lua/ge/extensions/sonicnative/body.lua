-- SPDX-License-Identifier: MIT
-- Sonic Beam, native mode: Sonic's physics on the game engine side. In plain free roam it starts
-- each map on foot, loads Sonic's runner into BeamNG's walking body (vehicle extension
-- sonicnative_runner), hands it its settings and the water level under it, sweeps Sonic's path
-- through nearby vehicles (vehicle extension sonicnative_impact) and puts the camera behind him
-- (camera mode sonicnativeChase), where C goes through BeamNG's own cameras too. It draws nothing: the visual module reads M.getState() and
-- M.getBody(). Same behaviour as the overlay's GE extension (beamng/mod/lua/ge/extensions,
-- sonic.lua, v0.12.3) without its renderer link, rings, menus, settings file and sounds (the O
-- menu and the settings file are sonicnative_menu's, which calls M.setSettings and M.unstuck). It
-- also loads the body's sound player (vehicle extension sonicnative_sfx) for whoever plays Sonic's sounds.

local M = {}
local logTag = 'sonicnative'

local config = require('sonicnative/config')
local P = config.physics
local U = config.unstuck
local K = config.camera

-- the overlay's defaults; M.setSettings changes them live (sonicnative_menu: the player's saved
-- settings at start, then the O menu and the Lab)
local settings = {}
for k, v in pairs(config.settings) do settings[k] = v end

local onFootDone = false          -- Sonic starts each map on foot once; after that F is the player's choice
local runnerLoadedFor = nil
local settingsSent = nil
local cameraActive = false
local notAllowedFor = 0           -- seconds Sonic has not been allowed to start the map on foot
local sonicBodyId = nil           -- walking body Sonic currently drives
local stopped = false             -- the mod was deactivated: hands off everything
local clock = 0
local bodyFeet, bodyVel = nil, vec3()
local groundedByRay = false
-- what the runner inside the walking body last told us (onRunner)
local runner = {boost = 0, grounded = 0, wantsMove = 0, airInput = 0, water = 0, wall = 0, wallN = vec3(0, 0, 1), jumps = 0, surface = 'conc', stick = 0}
local groundKeys = {}             -- the ground keys the runner may send (config.ground)
for _, k in ipairs(config.ground.keys) do groundKeys[k] = true end
local carMass = {}                -- vehicle id -> kg (reported by the vehicle's sonicnative_impact)
local impactLoaded = {}           -- vehicle id -> the impact extension was loaded into it
local impactCooldown = {}         -- vehicle id -> clock before it can be hit again
local lastImpactPos = nil         -- Sonic's centre last frame (hits are swept along his path)
local lastHit = nil               -- the last hit, for developer checks (M.getLastHit)
local waterObjects = nil          -- the map's WaterBlocks and WaterPlanes (found once per map)
local sentWaterZ = false          -- last water height sent to the body
local safeSpots = {}              -- recent spots where Sonic stood upright, for Unstuck: {pos, yaw}
-- for the visual module: the same table on every M.getState() call, refreshed each frame
local state = {active = false, boosting = false, airborne = false, grounded = false, speedKmh = 0, onWater = false, onWall = false,
  water = 0, wall = 0, wallNormal = vec3(0, 0, 1), wantsMove = false, airInput = 0, jumps = 0, surface = 'conc', stick = -1}

local down = vec3(0, 0, -1)

local function resetRunner()
  runner.boost, runner.grounded, runner.wantsMove, runner.airInput, runner.water, runner.wall = 0, 0, 0, 0, 0, 0
  runner.wallN = vec3(0, 0, 1)
  runner.surface = 'conc'
  runner.stick = 0
end

-- the water surface at x, y: WaterBlocks cover their box (their surface is at their position's
-- height), a WaterPlane is a sea everywhere
local function waterSurfaceAt(x, y)
  if not waterObjects then
    waterObjects = {}
    for _, cls in ipairs({'WaterBlock', 'WaterPlane'}) do
      for _, name in ipairs(scenetree.findClassObjects(cls) or {}) do
        local o = scenetree.findObject(name)
        if o then
          local p = o:getPosition()
          local entry = {z = p.z, plane = cls == 'WaterPlane'}
          if not entry.plane and o.getWorldBox then
            local b = o:getWorldBox()
            entry.x0, entry.y0, entry.x1, entry.y1 = b.minExtents.x, b.minExtents.y, b.maxExtents.x, b.maxExtents.y
          end
          if entry.plane or entry.x0 then waterObjects[#waterObjects + 1] = entry end
        end
      end
    end
  end
  local best = nil
  for _, w in ipairs(waterObjects) do
    if w.plane or (x >= w.x0 and x <= w.x1 and y >= w.y0 and y <= w.y1) then
      if not best or w.z > best then best = w.z end
    end
  end
  return best
end

-- distance down to the ground from a point (nil when nothing within range)
local function groundBelow(pos, range)
  local d = castRayStatic(pos, down, range)
  if d and d < range then return d end
end

local function getUnicycle()
  if not gameplay_walk or not gameplay_walk.isWalking() then return nil end
  return gameplay_walk.getCurrentUnicycle()
end

-- Plain free roam: no tutorial ("Discover", first drive), career, scenario or mission scripting the
-- player. BeamNG.Earth's map is free roam with its own app layout ('beamngEarth').
local FREE_ROAM_LAYOUTS = {freeroam = true, beamngEarth = true}
local function plainFreeRoam()
  local gs = core_gamestate and core_gamestate.state or {}
  if gs.state ~= 'freeroam' or not FREE_ROAM_LAYOUTS[gs.appLayout] then return false end
  local missions = gameplay_missions_missionManager
  if missions and missions.getForegroundMissionId and missions.getForegroundMissionId() then return false end
  return true
end

-- Whether Sonic drives the walker now ('auto': plain free roam only)
local function sonicAllowed()
  if settings.sonicMode == 'off' then return false end
  if settings.sonicMode == 'always' then return true end
  return plainFreeRoam()
end

local function ensureOnFoot()
  if onFootDone then return end
  -- free roam normally loads BeamNG's walking mode; a map started another way may not have it yet
  if not gameplay_walk then extensions.load('gameplay_walk') end
  if not gameplay_walk then return end
  local veh = getPlayerVehicle(0)
  if not veh then return end
  onFootDone = true
  if not gameplay_walk.isWalking() then
    gameplay_walk.setWalkingMode(true, nil, nil, true)
    log('I', logTag, 'Sonic starts on foot')
  end
end

-- Sonic against vehicles: each frame his path is swept through every vehicle's box; on a hit he
-- hands it momentum (impactForce: he hits like 1000 kg x the setting), the vehicle's
-- sonicnative_impact pushes it and crushes it around the contact, and Sonic slows, keeps going
-- or bounces (impactMode)
local IMPACT_MIN_MS = 4           -- slower than this it is just a bump
local IMPACT_LOOKAHEAD = 0.08     -- seconds of his path tested ahead of him
local function checkImpacts(unicycle)
  if settings.impactMode == 'off' or not bodyFeet then lastImpactPos = nil return end
  local centre = bodyFeet + vec3(0, 0, 0.5)
  local from = lastImpactPos or centre
  if (centre - from):length() > 60 then from = centre end   -- a teleport
  lastImpactPos = centre
  local myId = unicycle:getId()
  -- the sweep runs a little ahead of him: BeamNG's own collision stops his light body as soon as he
  -- touches a car, so the hit must land the moment before, with all his speed
  local vsNow = vec3(unicycle:getVelocityXYZ())
  local ahead = centre + vsNow * IMPACT_LOOKAHEAD
  local path = (ahead - from):length()
  -- a body that just spawned or blew up reports absurd speeds: never sweep thousands of points (the
  -- loop below froze the game)
  if not (path < 120) then from, ahead, path = centre, centre, 0 end
  for _, veh in ipairs(getAllVehicles()) do
    local id = veh:getId()
    if id ~= myId and veh:getJBeamFilename() ~= 'unicycle' then
      local bb = veh:getSpawnWorldOOBB()
      local c = bb:getCenter()
      local far = (c - centre):length()
      if far < 40 and not impactLoaded[id] then
        impactLoaded[id] = true
        veh:queueLuaCommand("extensions.load('sonicnative_impact')")
      end
      if far < 12 + path and (impactCooldown[id] or -1) < clock then
        local half = bb:getHalfExtents()
        local ax = {bb:getAxis(0), bb:getAxis(1), bb:getAxis(2)}
        local hx, hy, hz = half.x, half.y, half.z
        local steps = math.min(300, math.max(1, math.ceil(path / 0.4)))
        for s = 1, steps do
          local q = from + (ahead - from) * (s / steps)
          local d = q - c
          local lx, ly, lz = d:dot(ax[1]), d:dot(ax[2]), d:dot(ax[3])
          if math.abs(lx) < hx + 0.45 and math.abs(ly) < hy + 0.45 and math.abs(lz) < hz + 0.3 then
            local vs, vc = vec3(unicycle:getVelocityXYZ()), vec3(veh:getVelocityXYZ())
            local rel = vs - vc
            local closing = rel:length()
            if closing >= IMPACT_MIN_MS then
              impactCooldown[id] = clock + 0.5
              -- contact: Sonic's position brought onto the vehicle's box
              local cp = c + ax[1] * clamp(lx, -hx, hx) + ax[2] * clamp(ly, -hy, hy) + ax[3] * clamp(lz, -hz, hz)
              local ms = 1000 * math.max(0, settings.impactForce)
              local mc = carMass[id] or 1500
              local mode = settings.impactMode
              local dvCar = rel * ((ms / (ms + mc)) * (mode == 'bounce' and 1.6 or 1.15))
              local dvSonic = vec3(0, 0, 0)
              if mode == 'mass' then dvSonic = -rel * math.min(1, (mc / (ms + mc)) * 1.15)
              elseif mode == 'bounce' then dvSonic = -rel * 1.6 end
              veh:queueLuaCommand(string.format('if sonicnative_impact then sonicnative_impact.hit(%f, %f, %f, %f, %f, %f, %f) end',
                cp.x, cp.y, cp.z, dvCar.x, dvCar.y, dvCar.z, settings.carDamage))
              if dvSonic:length() > 0 then unicycle:applyClusterVelocityScaleAdd(unicycle:getRefNodeId(), 1, dvSonic.x, dvSonic.y, dvSonic.z) end
              lastHit = {kmh = closing * 3.6, mode = mode, kg = mc, sonicLoss = dvSonic:length() * 3.6, car = dvCar:length() * 3.6, vs = vs:length() * 3.6, clock = clock}
              log('D', logTag, string.format('hit %s (%.0f kg) at %.0f km/h, mode %s', veh:getJBeamFilename(), mc, closing * 3.6, mode))
            end
            break
          end
        end
      end
    end
  end
end

-- Sonic's feet (the ground under the walking body, or the bottom of its box in the air) and
-- velocity; true when the ground is within reach
local function updateBody(unicycle)
  local id = unicycle:getId()
  local center = vec3(be:getObjectOOBBCenterXYZ(id))
  local half = 0
  for axis = 0, 2 do half = math.max(half, math.abs(vec3(be:getObjectOOBBHalfAxisXYZ(id, axis)).z)) end
  half = math.max(half, 0.3)
  local vel = vec3(unicycle:getVelocityXYZ())
  -- a respawn or teleport shows up as an absurd one-frame velocity: keep the last real one
  if vel:length() - bodyVel:length() < 40 then bodyVel = vel end
  local d = groundBelow(center, half + 3)
  if d and d <= half + P.groundProbe then
    bodyFeet = center + down * d
  else
    bodyFeet = center + down * half
  end
  return d ~= nil and d <= half + P.groundProbe
end

-- Unstuck (O menu button and its key): a crash at speed can leave the walking body on its side or
-- wedged in a fence with no way to get up. Only on request, Sonic goes back upright to the last
-- spot he stood on, a few metres back; BeamNG's safeTeleport finds a free place there and resets
-- the body. Same as the overlay (beamng/mod sonic.lua rememberSafeSpot and unstuck, v0.12.3),
-- except that a spot far from the last one (U.forget) drops the older ones.
local function rememberSafeSpot(unicycle, grounded)
  if not grounded or unicycle:getDirectionVectorUp().z < U.uprightZ then return end
  local pos = vec3(unicycle:getPosition())
  local last = safeSpots[#safeSpots]
  local far = last and (pos - last.pos):length()
  if far and far < U.spacing then return end
  -- a car trip or a teleport since the last spot: Unstuck must not send him back there
  if far and far > U.forget then safeSpots = {} end
  local dir = bodyVel:length() > 1 and bodyVel or unicycle:getDirectionVector()
  safeSpots[#safeSpots + 1] = {pos = pos, yaw = math.atan2(dir.y, dir.x)}
  if #safeSpots > U.keep then table.remove(safeSpots, 1) end
end

-- true when Sonic was put back; only while he is on foot as Sonic
local function unstuck()
  local unicycle = getUnicycle()
  if stopped or not unicycle or unicycle:getId() ~= sonicBodyId then
    log('I', logTag, 'Unstuck: Sonic is not on foot, nothing done')
    return false
  end
  local here = vec3(unicycle:getPosition())
  local spot
  for i = #safeSpots, 1, -1 do
    if (safeSpots[i].pos - here):length() >= U.meters then spot = safeSpots[i] break end
  end
  spot = spot or safeSpots[1] or {pos = here, yaw = 0}
  spawn.safeTeleport(unicycle, spot.pos + vec3(0, 0, U.lift), quatFromDir(vec3(-math.cos(spot.yaw), -math.sin(spot.yaw), 0), vec3(0, 0, 1)))
  lastImpactPos = nil   -- no car hit swept along the way back
  log('I', logTag, string.format('Unstuck: Sonic put back on his feet %.1f m from where he was', (spot.pos - here):length()))
  return true
end

local footView = 'sonic'          -- the view C last picked on foot ('sonic' or one of BeamNG's cameras), kept across car trips
local slotCamera = nil            -- the walking body's camera a number key just picked (onCameraModeChanged)
local lastGlobal = nil            -- the global camera active at the end of the last frame
local announceAt = nil            -- clock at which the view's name shows after Sonic's camera came back

-- (the global camera is read and set on the main view's context: with a player, split-screen gives
-- player 0's own view, which Sonic's camera does not use)
local function setSonicCamera(on)
  if not core_camera then return end
  if on then
    -- BeamNG's walking mode clears the global camera while a new walking body settles (map start),
    -- and C, the free camera or the big map leave it too: on foot, with no other global camera
    -- active, Sonic's camera comes back, on the view C last picked
    local active = core_camera.getActiveGlobalCameraName()
    if cameraActive and active then return end
    cameraActive = true
    if active ~= 'sonicnativeChase' then core_camera.setGlobalCameraByName('sonicnativeChase') end
    footView = core_camera.globalCameraFunction('sonicnativeChase', 'setView', footView, true) or 'sonic'
    -- its name, a moment later: getting out of a car, BeamNG shows the name of the walking body's
    -- own camera, which is not the one on screen
    announceAt = clock + 0.25
  elseif cameraActive then
    cameraActive = false
    core_camera.setGlobalCameraByName(nil)
  end
end

-- The camera keys while Sonic is on foot.
-- C: BeamNG's walking body binds C to crouching in its own key map, which comes before the game's
-- camera keys. Sonic never crouches, so that action is filtered out and C reaches BeamNG's "next
-- camera" (the way BeamNG's ui/missionInfo.lua frees its interact key).
-- Shift+C: BeamNG's camera group toggle (commands.toggleCamera) is wrapped, the way BeamNG's
-- gameplay/crawl/utils.lua wraps commands.dropPlayerAtCameraNoReset, so that onCameraToggled knows
-- the key did it (F7, the big map and others send the same hook)
local CAMERA_KEY_FILTER = 'sonicnativeCameraKey'
local cameraKeysOn = false
local toggleKey = false           -- inside BeamNG's camera group toggle
local originalToggleCamera = nil
local function toggleCameraKey(...)
  toggleKey = true
  local ok, err = pcall(originalToggleCamera, ...)
  toggleKey = false
  if not ok then log('E', logTag, 'camera toggle failed: ' .. tostring(err)) end
end
local function setCameraKeys(on)
  if on == cameraKeysOn then return end
  cameraKeysOn = on
  if core_input_actionFilter then
    core_input_actionFilter.setGroup(CAMERA_KEY_FILTER, on and {'unicycle__crouchToggle'} or {})
    core_input_actionFilter.addAction(0, CAMERA_KEY_FILTER, on)
  end
  if on and not originalToggleCamera and commands and commands.toggleCamera then
    originalToggleCamera = commands.toggleCamera
    commands.toggleCamera = toggleCameraKey
  elseif not on and originalToggleCamera and commands and commands.toggleCamera == toggleCameraKey then
    -- put back only if still ours; otherwise the wrapper stays a plain pass-through for whoever wrapped it
    commands.toggleCamera, originalToggleCamera = originalToggleCamera, nil
  end
end

-- the views C goes through on foot: Sonic's own, then BeamNG's cameras of the walking body named in
-- config.camera.footCameras, in the order of the player's Options > Cameras list and only those left
-- on there
local function footViews()
  local views = {'sonic'}
  local cams = sonicBodyId and core_camera.getCameraDataById(sonicBodyId)
  if not cams then return views end
  local wanted = {}
  for _, name in ipairs(K.footCameras) do wanted[name] = true end
  for _, c in ipairs(core_camera.getConfigurationReadOnly()) do
    local cam = cams[c.name]
    if wanted[c.name] and c.enabled and cam and not cam.hidden then views[#views + 1] = c.name end
  end
  return views
end

-- a camera a number key may show on foot: one of config.camera.footCameras on the walking body, even
-- if it is off in the Options list (as on a car)
local function footCameraUsable(name)
  local cams = sonicBodyId and core_camera.getCameraDataById(sonicBodyId)
  local cam = cams and cams[name]
  if not cam or cam.hidden then return false end
  for _, n in ipairs(K.footCameras) do if n == name then return true end end
  return false
end

-- the view on screen (the camera falls back to Sonic's own by itself if a BeamNG camera fails)
local function shownView()
  return core_camera.globalCameraFunction('sonicnativeChase', 'getView') or footView
end

-- its name, the way BeamNG shows a car's camera (Sonic's own: "Sonic")
local function showViewName(views)
  local shown = shownView()
  local index = 1
  for i, name in ipairs(views) do if name == shown then index = i end end
  guihooks.trigger('Message', {txt = 'ui.camera.switched', ttl = 5, category = 'cameramode', icon = 'survellianceCamera',
    context = {name = shown == 'sonic' and 'Sonic' or 'ui.camera.mode.' .. shown},
    availableOptionsCount = #views, currentOptionIndex = index - 1})
end

-- C (BeamNG's next camera) or a camera number key, on foot (hook trackCamMode, fired by those
-- keys): BeamNG only knows its first-person walking camera there, so it just left Sonic's camera.
-- It comes back at once on the next view of the list, or the one the number key picked, and its
-- name shows. Previous camera also goes to the next view
local function onCameraKey()
  local picked = slotCamera
  slotCamera = nil
  local veh = getPlayerVehicle(0)
  if stopped or not cameraActive or not veh or veh:getId() ~= sonicBodyId then return end
  -- C and a number key with a camera have left Sonic's camera; still on it (a number key with no
  -- camera) or on another global camera (the free camera...): nothing to do
  if core_camera.getActiveGlobalCameraName() then return end
  local views = footViews()
  local want = 'sonic'
  if picked then
    if footCameraUsable(picked) then want = picked end
  else
    local shown = shownView()
    for i, name in ipairs(views) do if name == shown then want = views[i % #views + 1] end end
  end
  core_camera.setGlobalCameraByName('sonicnativeChase')
  footView = core_camera.globalCameraFunction('sonicnativeChase', 'setView', want) or 'sonic'
  core_camera.startCameraTransition()
  announceAt = nil
  showViewName(views)
end

-- Shift+C (BeamNG's camera group toggle, hook onCameraToggled) on foot: from Sonic's camera BeamNG
-- goes back to its game camera, which Sonic's camera replaces at once on foot, so the free camera
-- could never be reached. Go on to it instead, as a second press did before; from the free camera
-- the next press comes back to Sonic's camera
local function onCameraToggled()
  if not toggleKey then return end
  local from = lastGlobal
  lastGlobal = nil   -- entered once only, even if BeamNG has no free camera to go to
  local veh = getPlayerVehicle(0)
  if stopped or not cameraActive or from ~= 'sonicnativeChase' or not veh or veh:getId() ~= sonicBodyId then return end
  if core_camera.getActiveGlobalCameraName() == nil then core_camera.cycleCameraGroup() end
end

-- hand the walking body back to BeamNG (its own walker, speeds and camera)
local function releaseBody()
  if sonicBodyId then
    local body = be:getObjectByID(sonicBodyId)
    if body then body:queueLuaCommand('if sonicnative_runner then sonicnative_runner.setEnabled(false) end if sonicnative_sfx then sonicnative_sfx.stopAll() end') end
    log('I', logTag, 'walking body handed back to BeamNG')
  end
  sonicBodyId, runnerLoadedFor, settingsSent = nil, nil, nil
  resetRunner()
end

local function updateState(onFoot)
  local s = state
  s.active = onFoot
  s.speedKmh = math.sqrt(bodyVel.x * bodyVel.x + bodyVel.y * bodyVel.y) * 3.6
  s.boosting = onFoot and runner.boost == 1
  -- on the ground, a wall or running on the water (the runner), or the ground within reach (ray)
  s.grounded = onFoot and (runner.grounded == 1 or groundedByRay)
  s.airborne = onFoot and not s.grounded
  s.water = onFoot and runner.water or 0          -- 1 running on the water, 2 swimming
  s.wall = onFoot and runner.wall or 0            -- 1 up the wall, 2/3 along it (wall on his left/right)
  s.onWater = s.water ~= 0
  s.onWall = s.wall ~= 0
  s.wallNormal = runner.wallN
  s.wantsMove = onFoot and runner.wantsMove == 1  -- stick pushed
  s.airInput = onFoot and runner.airInput or 0    -- direction held in the air: 1 left, 2 right, 3 back
  s.stick = s.wantsMove and runner.stick or -1    -- direction held, 0 = forward then clockwise in config.camera.stickSteps steps; -1 none
  s.jumps = runner.jumps                          -- jumps since the extension loaded: a change is a new jump
  -- ground under him for the footstep and landing sounds ('conc', 'dirt', 'gras', 'sand', 'snow',
  -- 'watr', 'mtlp'): the last one the ball touched, kept in the air; the water while on it
  s.surface = onFoot and runner.surface or 'conc'
end

-- called by the runner inside the walking body when its state changes, and a few times a second
local function onRunner(boost, jumps, grounded, wantsMove, airInput, water, wall, nx, ny, nz, surface, stick)
  runner.boost, runner.grounded, runner.wantsMove = boost or 0, grounded or 0, wantsMove or 0
  runner.airInput, runner.water, runner.wall = airInput or 0, water or 0, wall or 0
  runner.wallN = vec3(nx or 0, ny or 0, nz or 1)
  runner.jumps = runner.jumps + (jumps or 0)
  runner.surface = groundKeys[surface] and surface or 'conc'
  runner.stick = stick or 0
end

local function onPreRender(dtReal)
  if stopped then return end
  clock = clock + (dtReal or 0)
  local allowed = sonicAllowed()
  -- BeamNG settles its game state a moment after the map appears: only give up putting the player
  -- on foot once a non-free-roam state has held for 3 s (then never pull them out of a car later)
  -- only plain free roam puts the player on foot: elsewhere ('always') Sonic waits until they get out
  if allowed and plainFreeRoam() then
    notAllowedFor = 0
    ensureOnFoot()
  elseif getPlayerVehicle(0) then
    notAllowedFor = notAllowedFor + (dtReal or 0)
    if notAllowedFor > 3 then onFootDone = true end
  end
  local unicycle = getUnicycle()
  if unicycle and not allowed then
    if sonicBodyId == unicycle:getId() then releaseBody() end
    unicycle = nil
  end
  local onFoot = unicycle ~= nil
  groundedByRay = false
  if onFoot then
    local id = unicycle:getId()
    if runnerLoadedFor ~= id then
      runnerLoadedFor = id
      sentWaterZ = false
      sonicBodyId = id
      cameraActive = false   -- Sonic's camera and its view set up again on this body
      resetRunner()
      -- Sonic's physics, and the player for his sounds (they follow the body)
      unicycle:queueLuaCommand("extensions.load('sonicnative_runner') extensions.load('sonicnative_sfx') if sonicnative_runner then sonicnative_runner.setEnabled(true) end")
    end
    if settingsSent ~= id then
      settingsSent = id
      unicycle:queueLuaCommand(string.format('if sonicnative_runner then sonicnative_runner.setSpeedSettings(%s, %f, %f, %f, %s, %f, %f, %s, %f, %f) end',
        tostring(settings.speedCapOn), settings.speedCapKmh, settings.boostRampKmhPerSecond, settings.jumpHeight,
        tostring(settings.runOnWater), settings.waterRunKmh, settings.swimKmh,
        tostring(settings.runOnWalls), settings.wallRunKmh, settings.wallGrip))
    end
    -- tell the body where the water's surface is under him (the map's water objects)
    local bodyPos = unicycle:getPosition()
    local wz = waterSurfaceAt(bodyPos.x, bodyPos.y)
    if wz ~= sentWaterZ then
      sentWaterZ = wz
      unicycle:queueLuaCommand('if sonicnative_runner then sonicnative_runner.setWater(' .. (wz and string.format('%.3f', wz) or 'nil') .. ') end')
    end
    groundedByRay = updateBody(unicycle)
    checkImpacts(unicycle)
    rememberSafeSpot(unicycle, groundedByRay or runner.grounded == 1)
  else
    bodyVel = vec3()
  end
  slotCamera = nil   -- only read by a camera key's hook, within the same key press
  setCameraKeys(onFoot)
  setSonicCamera(onFoot)
  lastGlobal = core_camera and core_camera.getActiveGlobalCameraName()
  if announceAt and clock >= announceAt then
    announceAt = nil
    if onFoot and lastGlobal == 'sonicnativeChase' then showViewName(footViews()) end
  end
  updateState(onFoot)
end

local function onClientStartMission()
  onFootDone, runnerLoadedFor, sonicBodyId, settingsSent = false, nil, nil, nil
  carMass, impactLoaded, impactCooldown, lastImpactPos = {}, {}, {}, nil
  waterObjects, sentWaterZ = nil, false
  notAllowedFor = 0
  safeSpots = {}
  cameraActive = false
  footView = 'sonic'
  resetRunner()
end

local function onClientEndMission()
  setSonicCamera(false)
  setCameraKeys(false)
end

local function onExtensionLoaded()
  setExtensionUnloadMode(M, 'manual')
  log('I', logTag, 'Sonic physics loaded')
end

-- unloaded or the mod switched off: the walking body and the camera go back to BeamNG
local function onExtensionUnloaded()
  releaseBody()
  setSonicCamera(false)
  setCameraKeys(false)
  bodyVel = vec3()
  updateState(false)   -- readers of getState() see him off foot, not the last frame's state
end

local function onModDeactivated(mod)
  if mod and mod.modname == 'sonicnative' then
    stopped = true
    releaseBody()
    setSonicCamera(false)
    setCameraKeys(false)
    bodyVel = vec3()
    updateState(false)   -- onPreRender stops refreshing the state from here on
  end
end

local function onModActivated(mod)
  if mod and mod.modname == 'sonicnative' then stopped = false end
end

M.onExtensionLoaded = onExtensionLoaded
M.onExtensionUnloaded = onExtensionUnloaded
M.onModDeactivated = onModDeactivated
M.onModActivated = onModActivated
M.onPreRender = onPreRender
M.onClientStartMission = onClientStartMission
M.onClientEndMission = onClientEndMission
M.onRunner = onRunner
M.trackCamMode = onCameraKey
M.onCameraToggled = onCameraToggled
M.onCameraModeChanged = function(camName) slotCamera = camName end
M.setCarMass = function(id, kg) carMass[id] = kg end
M.getLastHit = function() return lastHit end
-- Sonic's feet and velocity (vec3s; feet is nil until he has been on foot), for the camera and the visual module
M.getBody = function() return bodyFeet, bodyVel end
-- {active, boosting, airborne, grounded, speedKmh, onWater, onWall, surface, ...}: the same table every call, read it, do not keep it
M.getState = function() return state end
M.ensureOnFoot = ensureOnFoot
M.groundBelow = groundBelow
M.unstuck = unstuck
-- changes settings live (same keys and types as config.settings; others are ignored) and sends
-- them to the current body on the next frame: sonicnative_menu, and developer tests
M.setSettings = function(changes)
  for k, v in pairs(changes or {}) do
    if settings[k] ~= nil and type(v) == type(settings[k]) then settings[k] = v end
  end
  settingsSent = nil
end
M.getSettings = function() return settings end

return M
