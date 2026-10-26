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
  float trailLength;
  float trailIntensity;
  float lightning;
  float fxSpeedAmount;
  float fxRefKmh;
  float auraThickness;
  float auraIntensity;
  float auraLightning;
  int32_t auraBody;
  float auraLength;
  int32_t water;
  float waterZ;
  float impact;
  int32_t wall;
  float wallNormal[3];
  float trailFade;
  int32_t jumpStyle;
  float ballSpin;
  float ballGlow;
  float blurStrength;
  int32_t airInput;
  int32_t ringCount;
  float rings[768];
} SonicBeamPacket;
]]
local HEADER_BYTES = ffi.offsetof('SonicBeamPacket', 'rings')
local frame = ffi.new('SonicBeamPacket')
frame.magic = 0x31464253
local udp
local linkFailed = false

local onFootDone = false          -- Sonic starts each map on foot once; after that F is the player's choice
local runnerLoadedFor = nil
local cameraActive = false
local hiddenUnicycle = nil
local hiddenAt = -100             -- when the walker's meshes were last hidden (clock)
local runnerBoost, runnerGrounded, pendingJumps = 0, 0, 0
local runnerWantsMove, runnerSurface = 0, 0
local runnerAirInput = 0          -- direction held in the air (1 left, 2 right, 3 back): tricks
local runnerWater = 0             -- 1 running on water, 2 swimming
local runnerWall = 0              -- 1 running up a wall, 2/3 along it (wall on his left/right)
local runnerWallN = {0, 0, 1}     -- the wall's normal
local carMass = {}                -- vehicle id -> kg (reported by the vehicle's sonicfrontiers_impact)
local impactLoaded = {}           -- vehicle id -> the impact extension was loaded into it
local impactCooldown = {}         -- vehicle id -> clock before it can be hit again
local lastImpactPos = nil         -- Sonic's centre last frame (hits are swept along his path)
local lastHit = nil               -- the last hit, for developer checks (M.getLastHit)
local pendingImpact = 0           -- closing speed (m/s) of a hit this frame, for the renderer's flash
local waterObjects = nil          -- the map's WaterBlocks and WaterPlanes (found once per map)
local sentWaterZ = false          -- last water height sent to the body

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
end     -- stick pushed, ground under Sonic (footsteps)
local rings = {}                  -- {pos = vec3, born = seconds}
local lastSpawn = -100
local clock = 0
local pickedThisFrame = 0
local bodyFeet, bodyVel = nil, vec3()
local settingsSent = nil
local camPose = {fresh = false}   -- set by the sonicfrontiersChase camera for this frame
local notAllowedFor = 0            -- seconds Sonic has not been allowed to start the map on foot
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
  -- where Sonic takes over the walker: 'auto' = plain free roam (and BeamNG.Earth) only, starting on
  -- foot; 'always' = every game mode, without ever pulling the player out of a car outside free roam;
  -- 'off' = never (BeamNG's own walker)
  sonicMode = 'auto',
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
  trailLength = 1, trailIntensity = 1, lightning = 1,   -- Power Boost trail and lightning
  trailFade = 1,                                       -- metres over which the trail fades in behind Sonic
  auraThickness = 1, auraIntensity = 1, auraLightning = 1,   -- boost aura on Sonic's body, and its lightning
  auraBodyShape = false,                                      -- false: lying along the run, round in front; true: hugs his body
  auraLength = 1,                                             -- how far the aura streams back behind him
  runOnWater = true, waterRunKmh = 80, swimKmh = 8,           -- water: run on it from this speed, else swim
  runOnWalls = true, wallRunKmh = 120, wallGrip = 3,          -- walls: boosting into one from this speed, for this long
  fxBySpeed = true, fxSpeedAmount = 0.75,              -- trail, lightning and aura grow with Sonic's speed
  blurStrength = 1,                                    -- speed blur multiplier
  -- jump: 0 ball the whole jump, 1 ball going up then tricks, 2 ball with tricks on a direction
  jumpStyle = 0, ballSpin = 3, ballGlow = 1, jumpHeight = 1,
  -- vehicles: 'mass' Sonic loses speed by the car's mass, 'plow' he keeps it, 'bounce' he bounces off,
  -- 'off' only BeamNG's own collision; impactForce: how heavy he hits; carDamage: how much they crush
  impactMode = 'mass', impactForce = 1, carDamage = 1,
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

local function openLab(tab)
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
    trailLength = im.FloatPtr(settings.trailLength),
    trailIntensity = im.FloatPtr(settings.trailIntensity),
    lightning = im.FloatPtr(settings.lightning),
    trailFade = im.FloatPtr(settings.trailFade),
    auraThickness = im.FloatPtr(settings.auraThickness),
    auraIntensity = im.FloatPtr(settings.auraIntensity),
    auraLightning = im.FloatPtr(settings.auraLightning),
    auraBodyShape = im.BoolPtr(settings.auraBodyShape),
    auraLength = im.FloatPtr(settings.auraLength),
    runOnWater = im.BoolPtr(settings.runOnWater),
    waterRunKmh = im.FloatPtr(settings.waterRunKmh),
    swimKmh = im.FloatPtr(settings.swimKmh),
    runOnWalls = im.BoolPtr(settings.runOnWalls),
    wallRunKmh = im.FloatPtr(settings.wallRunKmh),
    wallGrip = im.FloatPtr(settings.wallGrip),
    fxBySpeed = im.BoolPtr(settings.fxBySpeed),
    fxSpeedAmount = im.FloatPtr(settings.fxSpeedAmount),
    blurStrength = im.FloatPtr(settings.blurStrength),
    ballSpin = im.FloatPtr(settings.ballSpin),
    impactForce = im.FloatPtr(settings.impactForce),
    carDamage = im.FloatPtr(settings.carDamage),
    ballGlow = im.FloatPtr(settings.ballGlow),
    jumpHeight = im.FloatPtr(settings.jumpHeight),
    open = im.BoolPtr(true),
    place = true,
    tab = tab,          -- open on this tab once (developer tests)
  }
end

-- a lab tab; selected once when the lab was opened on it
local function labTab(name)
  local w = labWidgets
  local flags = 0
  if w.tab == name then flags = im.TabItemFlags_SetSelected; w.tab = nil end
  return im.BeginTabItem(name, nil, flags)
end

local function dbText(db)
  if db <= LAB_MIN_DB then return 'muted' end
  return string.format('%+.1f dB', db)
end

-- a setting's value written beside its slider: a click turns it into a box to type any value,
-- beyond the slider's range too; Enter (or a click elsewhere) keeps it
local editKey, editBuf, editFrames = nil, nil, 0
local function editableValue(key, ptr, text)
  if editKey == key then
    im.PushItemWidth(110 * im.uiscale[0])
    if editFrames == 0 then im.SetKeyboardFocusHere(0) end
    local entered = im.InputFloat('##edit' .. key, editBuf, 0, 0, '%.3f', im.InputTextFlags_EnterReturnsTrue + im.InputTextFlags_AutoSelectAll)
    im.PopItemWidth()
    editFrames = editFrames + 1
    if entered or (editFrames > 3 and not im.IsItemActive()) then
      editKey = nil
      local v = editBuf[0]
      if v == v and v > -1e9 and v < 1e9 then settings[key] = v; ptr[0] = v; return true end
    end
    return false
  end
  local shown = text(settings[key])
  -- a selectable shows the text as is (% included) and lights up under the mouse
  if im.Selectable1(shown .. '##value' .. key, false, 0, im.ImVec2(im.CalcTextSize(shown).x, 0)) then
    editKey, editBuf, editFrames = key, im.FloatPtr(settings[key]), 0
  end
  if im.IsItemHovered() then im.SetTooltip('Click to type a value') end
  return false
end

-- one lab setting: slider, its value (click to type it), and a button back to the default
local function labSlider(label, ptr, key, low, high, text, default, defaultLabel)
  im.PushItemWidth(260 * im.uiscale[0])
  local changed = false
  if im.SliderFloat(label, ptr, low, high, '', nil) then
    settings[key] = clamp(ptr[0], low, high)
    changed = true
  end
  im.PopItemWidth()
  im.SameLine()
  if editableValue(key, ptr, text) then changed = true end
  im.SameLine()
  if im.SmallButton(defaultLabel .. '##' .. key) then ptr[0] = default; settings[key] = default; changed = true end
  return changed
end

local function timesText(x) return string.format('x%.2f', x) end

-- Lab, Jump tab: one box per jump style, only one ticked at a time
-- Lab, Impact tab: what a hit does to Sonic (one box ticked at a time)
local IMPACT_MODES = {
  {id = 'mass', label = 'Sonic loses speed by the mass of the car'},
  {id = 'plow', label = 'Sonic keeps his speed and plows through'},
  {id = 'bounce', label = 'Sonic bounces off the car'},
  {id = 'off', label = "Off: BeamNG's own collision only"},
}

local JUMP_STYLES = {
  'Ball for the whole jump (no tricks)',
  'Ball going up, then tricks or a normal fall after the top of the jump',
  'Ball, and a trick when you hold a direction in the air (A, D or S)',
}

-- speed at which the boost effects are at full strength: the O menu's top speed, or 500 km/h
-- when the speed limit is off
local function fxReferenceKmh()
  return settings.speedCapOn and math.max(50, settings.speedCapKmh) or 500
end
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
  im.Begin('Sonic Beam Lab', w.open, im.WindowFlags_NoCollapse + im.WindowFlags_AlwaysAutoResize)   -- the window fits the open tab
  -- one tab per theme; Close and save stays below whichever tab is open
  if im.BeginTabBar('##sonicLabTabs') then
    if labTab('Sound') then
      labSlider('Footsteps', w.stepsDb, 'stepsDb', LAB_MIN_DB, LAB_MAX_DB, dbText, 0, '0 dB')
      labSlider('All Sonic sounds', w.soundDb, 'soundDb', LAB_MIN_DB, LAB_MAX_DB, dbText, 0, '0 dB')
      labSlider('Wind', w.windDb, 'windDb', LAB_MIN_DB, LAB_MAX_DB, dbText, 0, '0 dB')
      labBoom('Sonic boom on a fast boost', w.boostBoom, 'boostBoom', w.boostBoomKmh, 'boostBoomKmh', 50, 1000, 200)
      im.EndTabItem()
    end
    if labTab('Shadow') then
      labSlider('Shadow opacity', w.shadowOpacity, 'shadowOpacity', 0, 1, function(x) return string.format('%d %%', math.floor(x * 100 + 0.5)) end, 0.55, '55 %')
      im.EndTabItem()
    end
    if labTab('Boost') then
      labSlider('Trail length', w.trailLength, 'trailLength', 0.25, 3, timesText, 1, 'x1')
      labSlider('Trail intensity', w.trailIntensity, 'trailIntensity', 0, 2, timesText, 1, 'x1')
      labSlider('Lightning', w.lightning, 'lightning', 0, 2, timesText, 1, 'x1')
      labSlider('Trail start fade', w.trailFade, 'trailFade', 0, 4, function(x) return string.format('%.2f m', x) end, 1, '1 m')
      im.Spacing()
      if im.Checkbox('Effects follow Sonic\'s speed', w.fxBySpeed) then settings.fxBySpeed = w.fxBySpeed[0] end
      im.TextUnformatted(string.format('  full effects at %.0f km/h (%s)', fxReferenceKmh(), settings.speedCapOn and 'top speed, O menu' or 'speed limit off'))
      if not w.fxBySpeed[0] then im.BeginDisabled(true) end
      labSlider('Speed influence', w.fxSpeedAmount, 'fxSpeedAmount', 0, 1, function(x) return string.format('%.2f (from %d %%)', x, math.floor((1 - x) * 100 + 0.5)) end, 0.75, '0.75')
      if not w.fxBySpeed[0] then im.EndDisabled() end
      im.Spacing()
      labSlider('Speed blur strength', w.blurStrength, 'blurStrength', 0, 2, timesText, 1, 'x1')
      im.EndTabItem()
    end
    if labTab('Aura') then
      im.TextUnformatted('The blue aura on Sonic while he boosts or runs fast:')
      if im.Checkbox('Aura follows body shape (otherwise it streams along the run)', w.auraBodyShape) then settings.auraBodyShape = w.auraBodyShape[0] end
      labSlider('Aura thickness', w.auraThickness, 'auraThickness', 0, 3, timesText, 1, 'x1')
      labSlider('Aura length', w.auraLength, 'auraLength', 0, 3, timesText, 1, 'x1')
      labSlider('Aura intensity', w.auraIntensity, 'auraIntensity', 0, 2, timesText, 1, 'x1')
      labSlider('Aura lightning', w.auraLightning, 'auraLightning', 0, 3, timesText, 1, 'x1')
      im.EndTabItem()
    end
    if labTab('Water') then
      im.TextUnformatted('Out of his depth, Sonic runs on the water when fast enough, else he swims:')
      if im.Checkbox('Run on water', w.runOnWater) then settings.runOnWater = w.runOnWater[0]; settingsSent = nil end
      if not w.runOnWater[0] then im.BeginDisabled(true) end
      if labSlider('Water run speed', w.waterRunKmh, 'waterRunKmh', 20, 300, kmhText, 80, '80') then settingsSent = nil end
      if not w.runOnWater[0] then im.EndDisabled() end
      if labSlider('Swim speed', w.swimKmh, 'swimKmh', 2, 30, kmhText, 8, '8') then settingsSent = nil end
      im.EndTabItem()
    end
    if labTab('Wall') then
      im.TextUnformatted('Boosting into a steep wall, Sonic runs on it: up when he meets it head on, along it otherwise.')
      im.TextUnformatted('Release the boost to drop off; jump to leap away from it.')
      if im.Checkbox('Run on walls', w.runOnWalls) then settings.runOnWalls = w.runOnWalls[0]; settingsSent = nil end
      if not w.runOnWalls[0] then im.BeginDisabled(true) end
      if labSlider('Wall run speed', w.wallRunKmh, 'wallRunKmh', 40, 400, kmhText, 120, '120') then settingsSent = nil end
      if labSlider('Wall grip', w.wallGrip, 'wallGrip', 0.5, 10, function(x) return string.format('%.1f s', x) end, 3, '3 s') then settingsSent = nil end
      if not w.runOnWalls[0] then im.EndDisabled() end
      im.EndTabItem()
    end
    if labTab('Impact') then
      im.TextUnformatted('When Sonic runs into a vehicle:')
      for _, mode in ipairs(IMPACT_MODES) do
        if im.Checkbox(mode.label .. '##impact' .. mode.id, im.BoolPtr(settings.impactMode == mode.id)) then settings.impactMode = mode.id end
      end
      im.Spacing()
      labSlider('Impact force', w.impactForce, 'impactForce', 0, 5, timesText, 1, 'x1')
      labSlider('Car damage', w.carDamage, 'carDamage', 0, 3, timesText, 1, 'x1')
      im.EndTabItem()
    end
    if labTab('Jump') then
      im.TextUnformatted('Sonic curls up into a ball when he jumps:')
      for i, style in ipairs(JUMP_STYLES) do
        if im.Checkbox(style .. '##jumpStyle' .. i, im.BoolPtr(settings.jumpStyle == i - 1)) then settings.jumpStyle = i - 1 end
      end
      im.Spacing()
      labSlider('Ball spin speed', w.ballSpin, 'ballSpin', 0, 8, function(x) return string.format('%.1f turns/s', x) end, 3, '3')
      labSlider('Ball glow', w.ballGlow, 'ballGlow', 0, 2, timesText, 1, 'x1')
      if labSlider('Jump height', w.jumpHeight, 'jumpHeight', 0.5, 3, timesText, 1, 'x1') then settingsSent = nil end
      im.EndTabItem()
    end
    if labTab('Animation') then
      labSlider('Animation speed', w.animSpeed, 'animSpeed', 0.5, 3, timesText, 1, 'x1')
      labSlider('Animation speed limit', w.animLimit, 'animLimit', 0.5, 3, timesText, 1, 'x1')
      im.EndTabItem()
    end
    im.EndTabBar()
  end
  im.Separator()
  if im.Button('Close and save') then w.open[0] = false end
  im.End()
  if not w.open[0] then labOpen = false; saveSettings() end
end

local SONIC_MODES = {
  {id = 'auto', label = 'Auto', about = 'Free roam only (BeamNG.Earth too), starting on foot.'},
  {id = 'always', label = 'Always', about = 'Every game mode: Sonic whenever you are on foot (press F to get out of a car).'},
  {id = 'off', label = 'Off', about = "BeamNG's own walker everywhere."},
}

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
  if im.SliderFloat('Boost acceleration (km/h per s)', w.ramp, 10, 500, '', nil) then
    settings.boostRampKmhPerSecond = clamp(w.ramp[0], 10, 500); settingsSent = nil
  end
  im.SameLine()
  if editableValue('boostRampKmhPerSecond', w.ramp, function(x) return string.format('%.0f', x) end) then settingsSent = nil end
  im.PopItemWidth()
  if im.Checkbox('Show speed', w.showSpeed) then settings.showSpeed = w.showSpeed[0] end
  im.Separator()
  im.TextUnformatted('Sonic:')
  for _, mode in ipairs(SONIC_MODES) do
    im.SameLine()
    if im.RadioButton1(mode.label, settings.sonicMode == mode.id) then settings.sonicMode = mode.id end
  end
  for _, mode in ipairs(SONIC_MODES) do
    if settings.sonicMode == mode.id then im.TextUnformatted('  ' .. mode.about) end
  end
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

-- Whether Sonic drives the walker now (the player's choice in the O menu, 'Sonic: Auto/Always/Off')
local function sonicAllowed()
  if settings.sonicMode == 'off' then return false end
  if settings.sonicMode == 'always' then return true end
  return plainFreeRoam()
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

-- Sonic against vehicles: each frame his path is swept through every vehicle's box; on a hit he
-- hands it momentum (Impact force: he hits like 1000 kg x the setting), the vehicle's
-- sonicfrontiers_impact pushes it and crushes it around the contact, and Sonic slows, keeps going
-- or bounces (Impact tab)
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
        veh:queueLuaCommand("extensions.load('sonicfrontiers_impact')")
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
              veh:queueLuaCommand(string.format('if sonicfrontiers_impact then sonicfrontiers_impact.hit(%f, %f, %f, %f, %f, %f, %f) end',
                cp.x, cp.y, cp.z, dvCar.x, dvCar.y, dvCar.z, settings.carDamage))
              if dvSonic:length() > 0 then unicycle:applyClusterVelocityScaleAdd(unicycle:getRefNodeId(), 1, dvSonic.x, dvSonic.y, dvSonic.z) end
              pendingImpact = math.max(pendingImpact, closing)
              lastHit = {kmh = closing * 3.6, mode = mode, kg = mc, sonicLoss = dvSonic:length() * 3.6, car = dvCar:length() * 3.6, vs = vs:length() * 3.6}
              log('D', logTag, string.format('hit %s (%.0f kg) at %.0f km/h, mode %s', veh:getJBeamFilename(), mc, closing * 3.6, mode))
            end
            break
          end
        end
      end
    end
  end
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
  local row = sheets.spawns.run_line
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
  local row = sheets.spawns.run_line
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
local function onRunner(boost, jumps, grounded, wantsMove, surface, airInput, water, wall, nx, ny, nz)
  runnerBoost, runnerGrounded, runnerWantsMove, runnerSurface = boost, grounded, wantsMove or 0, surface or 0
  runnerAirInput = airInput or 0
  runnerWater = water or 0
  runnerWall = wall or 0
  runnerWallN = {nx or 0, ny or 0, nz or 1}
  pendingJumps = pendingJumps + jumps
end

local function getBody()
  return bodyFeet, bodyVel
end

local function onPreRender(dtReal, dtSim)
  if not openRenderer() then return end
  if frameReady then sendFrame() end   -- onDrawDebug did not run last frame
  clock = clock + (dtReal or 0)
  local allowed = sonicAllowed()
  -- BeamNG settles its game state a moment after the map appears: only give up putting the player
  -- on foot once a non-free-roam state has held for 3 s (then never pull them out of a car later)
  -- only plain free roam puts the player on foot: elsewhere ('Always') Sonic waits until they get out
  if allowed and plainFreeRoam() then
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
      sonicBodyId, hiddenUnicycle, runnerLoadedFor, settingsSent = nil, nil, nil, nil
    end
    unicycle = nil
  end
  local onFoot = unicycle ~= nil
  local groundedByRay = false
  if onFoot then
    local id = unicycle:getId()
    if runnerLoadedFor ~= id then
      runnerLoadedFor = id
      sentWaterZ = false
      sonicBodyId = id
      unicycle:queueLuaCommand("extensions.load('sonicfrontiers_runner') if sonicfrontiers_runner then sonicfrontiers_runner.setEnabled(true) end")
    end
    if settingsSent ~= id then
      settingsSent = id
      unicycle:queueLuaCommand(string.format('if sonicfrontiers_runner then sonicfrontiers_runner.setSpeedSettings(%s, %f, %f, %f, %s, %f, %f, %s, %f, %f) end',
        tostring(settings.speedCapOn), settings.speedCapKmh, settings.boostRampKmhPerSecond, settings.jumpHeight,
        tostring(settings.runOnWater), settings.waterRunKmh, settings.swimKmh,
        tostring(settings.runOnWalls), settings.wallRunKmh, settings.wallGrip))
    end
    -- BeamNG 0.40.1's walker is a whole character, and the game shows its meshes again on its own
    -- (camera changes, physics resets): hide them again on those events and twice a second anyway
    if hiddenUnicycle ~= id or clock - hiddenAt > 0.5 then
      hiddenUnicycle, hiddenAt = id, clock
      unicycle:setMeshAlpha(0, '')
    end
    -- tell the body where the water's surface is under him (the map's water objects)
    local bodyPos = unicycle:getPosition()
    local wz = waterSurfaceAt(bodyPos.x, bodyPos.y)
    if wz ~= sentWaterZ then
      sentWaterZ = wz
      unicycle:queueLuaCommand('if sonicfrontiers_runner then sonicfrontiers_runner.setWater(' .. (wz and string.format('%.3f', wz) or 'nil') .. ') end')
    end
    groundedByRay = updateBody(unicycle)
    checkImpacts(unicycle)
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
  frame.trailLength = settings.trailLength
  frame.trailIntensity = settings.trailIntensity
  frame.lightning = settings.lightning
  frame.fxSpeedAmount = settings.fxBySpeed and settings.fxSpeedAmount or 0
  frame.fxRefKmh = fxReferenceKmh()
  frame.auraThickness = settings.auraThickness
  frame.auraIntensity = settings.auraIntensity
  frame.auraLightning = settings.auraLightning
  frame.auraBody = settings.auraBodyShape and 1 or 0
  frame.auraLength = settings.auraLength
  frame.water = runnerWater
  frame.waterZ = sentWaterZ or -100000
  frame.impact = pendingImpact
  frame.wall = runnerWall
  frame.wallNormal[0], frame.wallNormal[1], frame.wallNormal[2] = runnerWallN[1], runnerWallN[2], runnerWallN[3]
  pendingImpact = 0
  frame.trailFade = settings.trailFade
  frame.jumpStyle = settings.jumpStyle
  frame.ballSpin = settings.ballSpin
  frame.ballGlow = settings.ballGlow
  frame.blurStrength = settings.blurStrength
  frame.airInput = runnerAirInput
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
  carMass, impactLoaded, impactCooldown, lastImpactPos = {}, {}, {}, nil
  waterObjects, sentWaterZ = nil, false
  notAllowedFor = 0
  safeSpots = {}
  rings = {}
  cameraActive = false
end

-- the game may have shown the walker's meshes again: hide them on the next frame
local function onCameraChanged()
  hiddenUnicycle = nil
end

local function onVehicleResetted(vid)
  if vid == hiddenUnicycle then hiddenUnicycle = nil end
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
M.onCameraChanged = onCameraChanged
M.onVehicleResetted = onVehicleResetted
M.onUpdate = onUpdate
M.onDrawDebug = onDrawDebug
M.toggleMenu = toggleMenu
M.unstuck = unstuck
M.openLab = openLab
M.editValue = function(key) if settings[key] ~= nil then editKey, editBuf, editFrames = key, im.FloatPtr(settings[key]), 0 end end   -- developer tests
M.isMenuOpen = function() return menuOpen end
M.onClientStartMission = onClientStartMission
M.onClientEndMission = onClientEndMission
M.onRunner = onRunner
M.setCarMass = function(id, kg) carMass[id] = kg end
M.getLastHit = function() return lastHit end
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
