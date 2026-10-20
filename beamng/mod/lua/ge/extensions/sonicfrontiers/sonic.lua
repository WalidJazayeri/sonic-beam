-- SPDX-License-Identifier: MIT
-- Sonic Beam: you are Sonic in BeamNG.drive. Starts each map on foot, drives BeamNG's walking
-- body with Sonic's speed (vehicle extension sonicfrontiers_runner), puts the camera behind him
-- (camera mode sonicfrontiersChase), lays Frontiers rings on the ground and hands everything to
-- the native renderer (Bin64\version.dll), which draws the real Sonic from the player's own
-- Sonic Frontiers. Numbers come from beamng/sheets (sonicfrontiers/sheets.lua).

local M = {}
local logTag = 'sonicfrontiers'

local ffi = require('ffi')
local sheets = require('sonicfrontiers/sheets')

-- One frame for the renderer (Bin64\version.dll), sent over loopback UDP: BeamNG's Lua sandbox
-- has no native calls, but it has LuaSocket. Layout matches Packet in beamng/src/link.cpp.
local MAX_RINGS = 256
local PORT = 47613
ffi.cdef[[
typedef struct {
  uint32_t magic;
  float camPos[3];
  float camFwd[3];
  float camUp[3];
  float fovY;
  float dt;
  float feet[3];
  float vel[3];
  int32_t onFoot;
  int32_t grounded;
  int32_t boost;
  int32_t jumped;
  int32_t picked;
  int32_t hudHidden;
  int32_t showSpeed;
  int32_t surface;
  int32_t wantsMove;
  float stepsDb;
  float soundDb;
  float animSpeed;
  float animLimit;
  float windDb;
  float boostBoomKmh;
  float toSun[3];
  float groundPos[3];
  float groundNormal[3];
  float shadowOpacity;
  int32_t hasGround;
  int32_t ringCount;
  float rings[768];
} SonicBeamPacket;
typedef struct { uint32_t magic; int32_t testActive; int32_t rings; } SonicBeamStatus;
]]
local HEADER_BYTES = ffi.offsetof('SonicBeamPacket', 'rings')
local frame = ffi.new('SonicBeamPacket')
frame.magic = 0x31464253
local udp
local linkFailed = false
local lastStatus = -100

local onFootDone = false          -- Sonic starts each map on foot once; after that F is the player's choice
local runnerLoadedFor = nil
local cameraActive = false
local hiddenUnicycle = nil
local runnerBoost, runnerGrounded, pendingJumps = 0, 0, 0
local runnerWantsMove, runnerSurface = 0, 0     -- stick pushed, ground under Sonic (footsteps)
local rings = {}                  -- {pos = vec3, born = seconds}
local lastSpawn = -100
local clock = 0
local pickedThisFrame = 0
local bodyFeet, bodyVel = nil, vec3()
local testMode = false
local autopilotSent = nil
local settingsSent = nil
local camPose = {fresh = false}   -- set by the sonicfrontiersChase camera for this frame
local notAllowedFor = 0            -- seconds the game state has not been plain free roam
local sonicBodyId = nil           -- walking body Sonic currently drives
local safeSpots = {}              -- recent spots where Sonic stood upright, for the Unstuck button
local unstuck                     -- defined below, called from the menu
local frameReady, frameOnFoot, frameDt, frameGroundedByRay = false, false, 0, false
local sendFrame
local prevFeet = nil

-- Sonic Beam menu (key O): speed limit, top speed, boost acceleration, speedometer.
-- Saved in the user folder (settings/sonicfrontiers_sonic.json).
local SETTINGS_FILE = 'settings/sonicfrontiers_sonic.json'
local im = ui_imgui
local settings = {
  speedCapOn = sheets.physics.speedCapOn == 1,
  speedCapKmh = sheets.physics.speedCapKmh,
  boostRampKmhPerSecond = sheets.physics.boostRampKmhPerSecond,
  showSpeed = true,
  -- Sonic Beam Lab (opened from the O menu)
  stepsDb = 0,          -- footsteps, decibels relative to Frontiers' mix
  soundDb = 0,          -- every sound the mod plays (Sonic and his surroundings)
  animSpeed = 1,        -- run-cycle cadence multiplier
  animLimit = 1,        -- run-cycle ceiling multiplier
  windDb = 0,           -- Frontiers' speed wind
  boostBoom = true, boostBoomKmh = 200,        -- sonic boom on a boost started at least this fast
  shadowOpacity = 0.55, -- Sonic's shadow
}
local menuOpen = false
local menuWidgets = nil

local down = vec3(0, 0, -1)

local function loadSettings()
  local saved = jsonReadFile(SETTINGS_FILE)
  if type(saved) ~= 'table' then return end
  for k, v in pairs(saved) do
    if settings[k] ~= nil and type(v) == type(settings[k]) then settings[k] = v end
  end
end

local function saveSettings()
  jsonWriteFile(SETTINGS_FILE, settings, true)
  settingsSent = nil
end

local function toggleMenu()
  menuOpen = not menuOpen
  if menuOpen then
    menuWidgets = {
      capOn = im.BoolPtr(settings.speedCapOn),
      capKmh = im.FloatPtr(settings.speedCapKmh),
      ramp = im.FloatPtr(settings.boostRampKmhPerSecond),
      showSpeed = im.BoolPtr(settings.showSpeed),
      open = im.BoolPtr(true),
      place = true,
    }
  else
    saveSettings()
  end
end

-- Sonic Beam Lab: settings we tune together, opened from the O menu. Sliders in decibels;
-- the lowest value mutes. Saved when the lab closes.
local LAB_MIN_DB, LAB_MAX_DB = -40, 12
local labOpen, labWidgets = false, nil

local function openLab()
  labOpen = true
  labWidgets = {
    stepsDb = im.FloatPtr(settings.stepsDb),
    soundDb = im.FloatPtr(settings.soundDb),
    animSpeed = im.FloatPtr(settings.animSpeed),
    animLimit = im.FloatPtr(settings.animLimit),
    windDb = im.FloatPtr(settings.windDb),
    boostBoom = im.BoolPtr(settings.boostBoom),
    boostBoomKmh = im.FloatPtr(settings.boostBoomKmh),
    shadowOpacity = im.FloatPtr(settings.shadowOpacity),
    open = im.BoolPtr(true),
    place = true,
  }
end

local function dbText(db)
  if db <= LAB_MIN_DB then return 'muted' end
  return string.format('%+.1f dB', db)
end

-- one lab setting: slider, its value written beside it, and a button back to the default
local function labSlider(label, ptr, key, low, high, text, default, defaultLabel)
  im.PushItemWidth(260 * im.uiscale[0])
  if im.SliderFloat(label, ptr, low, high, '', nil) then
    settings[key] = clamp(ptr[0], low, high)
  end
  im.PopItemWidth()
  im.SameLine()
  im.TextUnformatted(text(settings[key]))   -- not im.Text: it reads % as a format code
  im.SameLine()
  if im.SmallButton(defaultLabel .. '##' .. key) then ptr[0] = default; settings[key] = default end
end

local function timesText(x) return string.format('x%.2f', x) end
local function kmhText(x) return string.format('%.0f km/h', x) end

-- a switch with the speed it works at (slider disabled while off)
local function labBoom(label, onPtr, onKey, kmhPtr, kmhKey, low, high, default)
  if im.Checkbox(label .. '##' .. onKey, onPtr) then settings[onKey] = onPtr[0] end
  if not onPtr[0] then im.BeginDisabled(true) end
  labSlider('  at speed##' .. kmhKey, kmhPtr, kmhKey, low, high, kmhText, default, string.format('%d', default))
  if not onPtr[0] then im.EndDisabled() end
end

local function drawLab()
  if not labOpen or not im then return end
  local w = labWidgets
  if w.place then
    w.place = false
    local view = im.GetMainViewport()
    -- left side, below the speedometer: Sonic (centre) is drawn over BeamNG's UI
    im.SetNextWindowPos(im.ImVec2(view.Pos.x + view.Size.x * 0.02, view.Pos.y + view.Size.y * 0.24), im.Cond_Always, im.ImVec2(0, 0))
    im.SetNextWindowSize(im.ImVec2(600 * im.uiscale[0], 0), im.Cond_Always)
  end
  im.Begin('Sonic Beam Lab', w.open, im.WindowFlags_NoCollapse)
  im.Text('Sound')
  im.Separator()
  labSlider('Footsteps', w.stepsDb, 'stepsDb', LAB_MIN_DB, LAB_MAX_DB, dbText, 0, '0 dB')
  labSlider('All Sonic sounds', w.soundDb, 'soundDb', LAB_MIN_DB, LAB_MAX_DB, dbText, 0, '0 dB')
  labSlider('Wind', w.windDb, 'windDb', LAB_MIN_DB, LAB_MAX_DB, dbText, 0, '0 dB')
  labBoom('Sonic boom on a fast boost', w.boostBoom, 'boostBoom', w.boostBoomKmh, 'boostBoomKmh', 50, 1000, 200)
  im.Spacing()
  im.Text('Shadow')
  im.Separator()
  labSlider('Shadow opacity', w.shadowOpacity, 'shadowOpacity', 0, 1, function(x) return string.format('%d %%', math.floor(x * 100 + 0.5)) end, 0.55, '55 %')
  im.Spacing()
  im.Text('Animation')
  im.Separator()
  labSlider('Animation speed', w.animSpeed, 'animSpeed', 0.5, 3, timesText, 1, 'x1')
  labSlider('Animation speed limit', w.animLimit, 'animLimit', 0.5, 3, timesText, 1, 'x1')
  im.Separator()
  if im.Button('Close and save') then w.open[0] = false end
  im.End()
  if not w.open[0] then labOpen = false; saveSettings() end
end

local function drawMenu()
  if not menuOpen or not im then return end
  local w = menuWidgets
  -- upper centre: clear of the speedometer (top left), the start-up hint (top) and Sonic (centre), who are drawn over BeamNG's UI
  local view = im.GetMainViewport()
  if w.place then
    -- forced once per opening: BeamNG remembers window positions between sessions
    w.place = false
    im.SetNextWindowPos(im.ImVec2(view.Pos.x + view.Size.x / 2, view.Pos.y + view.Size.y * 0.24), im.Cond_Always, im.ImVec2(0.5, 0))
    im.SetNextWindowSize(im.ImVec2(560 * im.uiscale[0], 0), im.Cond_Always)
  end
  im.Begin('Sonic Beam', w.open, im.WindowFlags_NoCollapse)
  im.Text('Boost (Shift) keeps adding speed while you hold it.')
  im.Separator()
  if im.Checkbox('Speed limit', w.capOn) then settings.speedCapOn = w.capOn[0]; settingsSent = nil end
  if not w.capOn[0] then im.BeginDisabled(true) end
  im.PushItemWidth(200 * im.uiscale[0])
  if im.InputFloat('Top speed (km/h)', w.capKmh, 50, 500, '%.0f') then
    w.capKmh[0] = clamp(w.capKmh[0], 20, 1000000)
    settings.speedCapKmh = w.capKmh[0]; settingsSent = nil
  end
  if not w.capOn[0] then im.EndDisabled() end
  if im.SliderFloat('Boost acceleration (km/h per s)', w.ramp, 10, 500, '%.0f', nil) then
    settings.boostRampKmhPerSecond = clamp(w.ramp[0], 10, 500); settingsSent = nil
  end
  im.PopItemWidth()
  if im.Checkbox('Show speed', w.showSpeed) then settings.showSpeed = w.showSpeed[0] end
  im.Separator()
  if im.Button('Unstuck: back to the last safe spot') then w.open[0] = false; unstuck() end
  if im.Button('Lab: settings we tune together') then w.open[0] = false; openLab() end
  if im.Button('Close (O)') then w.open[0] = false end
  im.End()
  if not w.open[0] then toggleMenu() end
end

local function openRenderer()
  if udp or linkFailed then return udp ~= nil end
  if not socket or not socket.udp then
    linkFailed = true
    log('E', logTag, 'LuaSocket is not available: Sonic cannot be drawn')
    return false
  end
  udp = socket.udp()
  udp:settimeout(0)
  local ok, err = udp:setpeername('127.0.0.1', PORT)
  if not ok then
    log('E', logTag, 'cannot reach the Sonic renderer: ' .. tostring(err))
    udp, linkFailed = nil, true
    return false
  end
  log('I', logTag, 'talking to the Sonic renderer on 127.0.0.1:' .. PORT)
  return true
end

local function readStatus()
  for _ = 1, 8 do
    local data = udp:receive()
    if not data then break end
    if #data >= ffi.sizeof('SonicBeamStatus') then
      local st = ffi.cast('const SonicBeamStatus *', data)
      if st.magic == 0x31534253 then
        testMode = st.testActive == 1
        lastStatus = clock
      end
    end
  end
  if clock - lastStatus > 5 then testMode = false end
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

-- Sonic only takes over BeamNG's plain free roam. Tutorials ("Discover", first drive), career,
-- scenarios and missions script the player themselves, so there the mod stays out of the way.
local function sonicAllowed()
  if testMode then return true end
  local gs = core_gamestate and core_gamestate.state or {}
  if gs.state ~= 'freeroam' or gs.appLayout ~= 'freeroam' then return false end
  local missions = gameplay_missions_missionManager
  if missions and missions.getForegroundMissionId and missions.getForegroundMissionId() then return false end
  return true
end

local function ensureOnFoot()
  if onFootDone or not gameplay_walk then return end
  local veh = getPlayerVehicle(0)
  if not veh then return end
  onFootDone = true
  if not gameplay_walk.isWalking() then
    gameplay_walk.setWalkingMode(true, nil, nil, true)
    log('I', logTag, 'Sonic starts on foot')
  end
end

-- Ground under Sonic for his shadow: the point straight below him and the slope there (three
-- rays around his feet). Read every frame in updateBody.
local groundPos, groundNormal = nil, vec3(0, 0, 1)
local function readGround(center)
  local d = groundBelow(center, 50)
  if not d then groundPos = nil return end
  groundPos = center + down * d
  local pts = {}
  for k = 0, 2 do
    local a = k * 2.094
    local top = vec3(center.x + math.cos(a) * 0.45, center.y + math.sin(a) * 0.45, center.z)
    local h = groundBelow(top, 50)
    if not h then return end
    pts[#pts + 1] = top + down * h
  end
  local n = (pts[2] - pts[1]):cross(pts[3] - pts[1])
  if n:length() < 1e-6 then return end
  n:normalize()
  if n.z < 0 then n = -n end
  groundNormal = n
end

-- Sun direction from BeamNG's sky (azimuth / elevation in degrees), read once a second
local toSun, sunReadAt = vec3(-0.35, 0.45, 0.82):normalized(), -100
local function readSun()
  if clock - sunReadAt < 1 then return end
  sunReadAt = clock
  local sky = scenetree and scenetree.sunsky
  if not sky then return end
  local ok, az, el = pcall(function() return tonumber(sky.azimuth), tonumber(sky.elevation) end)
  if not ok or not az or not el then return end
  local a, e = math.rad(az), math.rad(el)
  toSun = vec3(math.sin(a) * math.cos(e), math.cos(a) * math.cos(e), math.sin(e))
end

local function updateBody(unicycle)
  local id = unicycle:getId()
  local center = vec3(be:getObjectOOBBCenterXYZ(id))
  local half = 0
  for axis = 0, 2 do half = math.max(half, math.abs(vec3(be:getObjectOOBBHalfAxisXYZ(id, axis)).z)) end
  half = math.max(half, 0.3)
  local vel = vec3(unicycle:getVelocityXYZ())
  -- a respawn or teleport shows up as an absurd one-frame velocity: keep the last real one
  if vel:length() - bodyVel:length() < 40 then bodyVel = vel end
  readGround(center)
  local d = groundBelow(center, half + 3)
  if d and d <= half + sheets.physics.groundProbe then
    bodyFeet = center + down * d
  else
    bodyFeet = center + down * half
  end
  return d ~= nil and d <= half + sheets.physics.groundProbe
end

local function spawnRings(feet, vel)
  local row = testMode and sheets.spawns.test or sheets.spawns.play
  local speedKmh = math.sqrt(vel.x * vel.x + vel.y * vel.y) * 3.6
  if clock - lastSpawn < row.everySeconds / math.max(1, speedKmh / 240) or speedKmh < row.minKmh or #rings + row.count > MAX_RINGS then return end
  lastSpawn = clock
  local dir = vec3(vel.x, vel.y, 0)
  if dir:length() < 0.5 then
    local camFwd = core_camera.getQuat() * vec3(0, 1, 0)
    dir = vec3(camFwd.x, camFwd.y, 0)
  end
  if dir:length() < 1e-3 then dir = vec3(0, 1, 0) end
  dir:normalize()
  local stretch = math.max(1, speedKmh / 120)   -- at extreme speed the line starts further ahead and spreads out
  for i = 0, row.count - 1 do
    local p = feet + dir * (row.startMeters * stretch + i * row.spacingMeters * stretch)
    local top = p + vec3(0, 0, 30)
    local d = groundBelow(top, 80)
    if d then
      rings[#rings + 1] = {pos = top + down * d + vec3(0, 0, row.heightMeters), born = clock}
    end
  end
end

-- distance from p to the segment a-b (Sonic's path this frame, so fast runs never skip rings)
local function segmentDistance(p, a, b)
  local ab = b - a
  local len2 = ab:squaredLength()
  local t = len2 > 1e-6 and clamp((p - a):dot(ab) / len2, 0, 1) or 0
  return (a + ab * t - p)
end

local function updateRings(feet)
  local row = testMode and sheets.spawns.test or sheets.spawns.play
  local from = prevFeet or feet
  if (feet - from):length() > 200 then from = feet end   -- teleport, not running
  prevFeet = vec3(feet)
  local keep = {}
  for _, ring in ipairs(rings) do
    local d = segmentDistance(ring.pos - vec3(0, 0, 0.5), from, feet)
    local dx, dy, dz = d.x, d.y, d.z
    if dx * dx + dy * dy < row.collectMeters * row.collectMeters and math.abs(dz) < 2.5 then
      pickedThisFrame = pickedThisFrame + 1
    elseif clock - ring.born < row.lifetimeSeconds then
      keep[#keep + 1] = ring
    end
  end
  rings = keep
end

-- Unstuck (menu button): a crash at speed can leave the walking body on its side or wedged in
-- a fence with no way to get up. Only on request, Sonic goes back upright to the last spot he
-- stood on, a few metres back; BeamNG's safeTeleport finds a free place there and resets the body.
local function rememberSafeSpot(unicycle, grounded)
  if not grounded or unicycle:getDirectionVectorUp().z < 0.9 then return end
  local pos = vec3(unicycle:getPosition())
  local last = safeSpots[#safeSpots]
  if last and (pos - last.pos):length() < 2 then return end
  local dir = bodyVel:length() > 1 and bodyVel or unicycle:getDirectionVector()
  safeSpots[#safeSpots + 1] = {pos = pos, yaw = math.atan2(dir.y, dir.x)}
  if #safeSpots > 30 then table.remove(safeSpots, 1) end
end

unstuck = function()
  local unicycle = getUnicycle()
  if not unicycle then return end
  local here = vec3(unicycle:getPosition())
  local spot
  for i = #safeSpots, 1, -1 do
    if (safeSpots[i].pos - here):length() >= sheets.physics.unstuckMeters then spot = safeSpots[i] break end
  end
  spot = spot or safeSpots[1] or {pos = here, yaw = 0}
  spawn.safeTeleport(unicycle, spot.pos + vec3(0, 0, 0.3), quatFromDir(vec3(-math.cos(spot.yaw), -math.sin(spot.yaw), 0), vec3(0, 0, 1)))
  prevFeet = nil
  log('I', logTag, 'Unstuck: Sonic put back on his feet')
end

local function setSonicCamera(on)
  if on == cameraActive then return end
  cameraActive = on
  core_camera.setGlobalCameraByName(on and 'sonicfrontiersChase' or nil)
end

-- called by the runner inside the walking body, every graphics frame
local function onRunner(boost, jumps, grounded, wantsMove, surface)
  runnerBoost, runnerGrounded, runnerWantsMove, runnerSurface = boost, grounded, wantsMove or 0, surface or 0
  pendingJumps = pendingJumps + jumps
end

local function getBody()
  return bodyFeet, bodyVel
end

local function onPreRender(dtReal, dtSim)
  if not openRenderer() then return end
  if frameReady then sendFrame() end   -- onDrawDebug did not run last frame
  clock = clock + (dtReal or 0)
  readStatus()
  local allowed = sonicAllowed()
  -- BeamNG settles its game state a moment after the map appears: only give up putting the player
  -- on foot once a non-free-roam state has held for 3 s (then never pull them out of a car later)
  if allowed then
    notAllowedFor = 0
    ensureOnFoot()
  elseif getPlayerVehicle(0) then
    notAllowedFor = notAllowedFor + (dtReal or 0)
    if notAllowedFor > 3 then onFootDone = true end
  end
  local unicycle = getUnicycle()
  if unicycle and not allowed then
    if sonicBodyId == unicycle:getId() then
      -- hand the walking body back to BeamNG
      unicycle:queueLuaCommand('if sonicfrontiers_runner then sonicfrontiers_runner.setEnabled(false) end')
      unicycle:setMeshAlpha(1, '')
      sonicBodyId, hiddenUnicycle, runnerLoadedFor, settingsSent, autopilotSent = nil, nil, nil, nil, nil
    end
    unicycle = nil
  end
  local onFoot = unicycle ~= nil
  local groundedByRay = false
  if onFoot then
    local id = unicycle:getId()
    if runnerLoadedFor ~= id then
      runnerLoadedFor = id
      autopilotSent = nil
      sonicBodyId = id
      unicycle:queueLuaCommand("extensions.load('sonicfrontiers_runner') if sonicfrontiers_runner then sonicfrontiers_runner.setEnabled(true) end")
    end
    if settingsSent ~= id then
      settingsSent = id
      unicycle:queueLuaCommand(string.format('if sonicfrontiers_runner then sonicfrontiers_runner.setSpeedSettings(%s, %f, %f) end',
        tostring(settings.speedCapOn), settings.speedCapKmh, settings.boostRampKmhPerSecond))
    end
    if autopilotSent ~= testMode then
      autopilotSent = testMode
      unicycle:queueLuaCommand('if sonicfrontiers_runner then sonicfrontiers_runner.setAutopilot(' .. tostring(testMode) .. ') end')
    end
    if hiddenUnicycle ~= id then
      hiddenUnicycle = id
      unicycle:setMeshAlpha(0, '')
    end
    groundedByRay = updateBody(unicycle)
    rememberSafeSpot(unicycle, groundedByRay or runnerGrounded == 1)
    if sheets.tuning.ringsEnabled == 1 then   -- rings are switched off for now (beamng/sheets/tuning.json)
      spawnRings(bodyFeet, bodyVel)
      updateRings(bodyFeet)
    end
  else
    hiddenUnicycle = nil
    bodyVel = vec3()
    prevFeet = nil
  end
  setSonicCamera(onFoot)
  frameOnFoot, frameDt, frameGroundedByRay = onFoot, dtReal or 0, groundedByRay
  frameReady = true
end

-- Sends this frame to the renderer. Runs in onDrawDebug, which BeamNG calls after every
-- onPreRender (core_camera's included) and right before rendering, so the camera we send is the
-- one this frame is drawn with; reading it in onPreRender could give last frame's camera.
-- BeamNG's own screens (pause menu, main menu, garage...) are drawn by its UI, under the mod's
-- overlay: while one is open, Sonic and the HUD are hidden. The gameplay route is "play".
local menuCheckAt, beamngMenu = -1, false
local function beamngMenuOpen()
  if clock - menuCheckAt < 0.2 then return beamngMenu end
  menuCheckAt = clock
  local router, menus = extensions.ui_router, extensions.ui_menuManager
  beamngMenu = false
  if router and router.getCurrent and menus and menus.getGameplayRoute then
    local current = router.getCurrent()
    local name = type(current) == 'table' and (current.name or (current.request and current.request.name)) or nil
    local play = menus.getGameplayRoute()
    beamngMenu = name ~= nil and play ~= nil and name ~= play
  end
  return beamngMenu
end

sendFrame = function()
  if not frameReady or not udp then return end
  frameReady = false
  local onFoot = frameOnFoot and not beamngMenuOpen()
  local pos, fwd, up, fov, feet
  if cameraActive and camPose.fresh then
    pos, fwd, up, fov, feet = camPose.pos, camPose.fwd, camPose.up, camPose.fov, camPose.feet
  else
    pos = core_camera.getPosition()
    local q = core_camera.getQuat()
    fwd, up, fov = q * vec3(0, 1, 0), q * vec3(0, 0, 1), core_camera.getFovRad()
    feet = bodyFeet or pos
  end
  camPose.fresh = false
  frame.camPos[0], frame.camPos[1], frame.camPos[2] = pos.x, pos.y, pos.z
  frame.camFwd[0], frame.camFwd[1], frame.camFwd[2] = fwd.x, fwd.y, fwd.z
  frame.camUp[0], frame.camUp[1], frame.camUp[2] = up.x, up.y, up.z
  frame.fovY = fov
  frame.dt = frameDt
  frame.feet[0], frame.feet[1], frame.feet[2] = feet.x, feet.y, feet.z
  frame.vel[0], frame.vel[1], frame.vel[2] = bodyVel.x, bodyVel.y, bodyVel.z
  frame.onFoot = onFoot and 1 or 0
  frame.grounded = (runnerGrounded == 1 or frameGroundedByRay) and 1 or 0
  frame.boost = runnerBoost
  frame.jumped = pendingJumps
  frame.picked = pickedThisFrame
  frame.hudHidden = onFoot and 0 or 1
  frame.showSpeed = settings.showSpeed and 1 or 0
  frame.surface = runnerSurface
  frame.stepsDb = settings.stepsDb
  frame.soundDb = settings.soundDb
  frame.animSpeed = settings.animSpeed
  frame.animLimit = settings.animLimit
  frame.windDb = settings.windDb
  readSun()
  frame.toSun[0], frame.toSun[1], frame.toSun[2] = toSun.x, toSun.y, toSun.z
  frame.shadowOpacity = settings.shadowOpacity
  if groundPos then
    frame.hasGround = 1
    frame.groundPos[0], frame.groundPos[1], frame.groundPos[2] = groundPos.x, groundPos.y, groundPos.z
    frame.groundNormal[0], frame.groundNormal[1], frame.groundNormal[2] = groundNormal.x, groundNormal.y, groundNormal.z
  else
    frame.hasGround = 0
  end
  frame.boostBoomKmh = settings.boostBoom and settings.boostBoomKmh or 0
  frame.wantsMove = runnerWantsMove
  local count = 0
  if onFoot then
    for i, ring in ipairs(rings) do
      if i > MAX_RINGS then break end
      frame.rings[count * 3], frame.rings[count * 3 + 1], frame.rings[count * 3 + 2] = ring.pos.x, ring.pos.y, ring.pos.z
      count = count + 1
    end
  end
  frame.ringCount = count
  udp:send(ffi.string(frame, HEADER_BYTES + count * 12))
  pendingJumps, pickedThisFrame = 0, 0
end

local function onClientStartMission()
  onFootDone, runnerLoadedFor, hiddenUnicycle, sonicBodyId = false, nil, nil, nil
  notAllowedFor = 0
  safeSpots = {}
  rings = {}
  cameraActive = false
end

local function onClientEndMission()
  setSonicCamera(false)
  rings = {}
end

local function onDrawDebug()
  sendFrame()
end

local function onUpdate()
  drawMenu()
  drawLab()
end

local function onExtensionLoaded()
  setExtensionUnloadMode(M, 'manual')
  loadSettings()
  openRenderer()
  log('I', logTag, 'Sonic Beam loaded')
end

M.onExtensionLoaded = onExtensionLoaded
M.onPreRender = onPreRender
M.onUpdate = onUpdate
M.onDrawDebug = onDrawDebug
M.toggleMenu = toggleMenu
M.unstuck = unstuck
M.openLab = openLab
M.isMenuOpen = function() return menuOpen end
M.onClientStartMission = onClientStartMission
M.onClientEndMission = onClientEndMission
M.onRunner = onRunner
M.getBody = getBody
-- developer probe: the last frame sent to the renderer
M.lastFrame = function()
  return {cam = {frame.camPos[0], frame.camPos[1], frame.camPos[2]}, fwd = {frame.camFwd[0], frame.camFwd[1], frame.camFwd[2]},
    feet = {frame.feet[0], frame.feet[1], frame.feet[2]}, fov = math.deg(frame.fovY), onFoot = frame.onFoot, fresh = camPose.fresh}
end
M.setCameraPose = function(pos, fwd, up, fov, feet)
  camPose.pos, camPose.fwd, camPose.up, camPose.fov, camPose.feet = vec3(pos), vec3(fwd), vec3(up), fov, vec3(feet)
  camPose.fresh = true
end
M.openRenderer = openRenderer
M.ensureOnFoot = ensureOnFoot
M.groundBelow = groundBelow

return M
