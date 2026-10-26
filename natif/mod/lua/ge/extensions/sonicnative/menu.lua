-- SPDX-License-Identifier: MIT
-- Sonic Beam, native mode: the O menu, the Lab and the Unstuck key. The menu (Sonic mode, speed
-- limit, top speed, boost acceleration, speedometer, Unstuck, Lab) and the Lab are the overlay's
-- (beamng/mod/lua/ge/extensions, sonicfrontiers/sonic.lua drawMenu and drawLab, v0.12.3), with the
-- Lab tabs the native mode has: Sound, Water, Wall, Impact, Jump, Animation. Values apply live
-- through sonicnative_body.setSettings, sonicnative_sound.setSettings and sonicnative_main.setTuning.
-- The player's settings are saved in the user folder (settings/sonicnative.json) when the menu or
-- the Lab closes, and loaded when this extension loads, before Sonic takes a body. Unstuck is
-- sonicnative_body's. Keys: input actions sonicnative_menu (O) and sonicnative_unstuck (U), in
-- lua/ge/extensions/core/input/actions/sonicnative.json and settings/inputmaps/keyboard_sonicnative.json;
-- players rebind them in BeamNG's Controls.

local M = {}
local logTag = 'sonicnative'

local config = require('sonicnative/config')
local soundConfig = require('sonicnative/sounds')
local animConfig = require('sonicnative/anim')
local json = require('json')

local SETTINGS_FILE = 'settings/sonicnative.json'
local im = ui_imgui

-- Every setting the menu and the Lab show: its default and the module it is sent to
local defaults, owner = {}, {}
local function add(key, value, target) defaults[key], owner[key] = value, target end
for k, v in pairs(config.settings) do add(k, v, 'body') end   -- Sonic mode, speed, jump height, water, wall, impact
for _, k in ipairs({'soundDb', 'stepsDb', 'windDb', 'boostBoom', 'boostBoomKmh'}) do add(k, soundConfig.settings[k], 'sound') end
add('jumpStyle', animConfig.tuning.jumpStyle, 'anim')
add('ballSpinTurns', -1, 'anim')    -- main.lua's default: -1 spins the ball at Frontiers' own speed (the manifest's ball.spinSeconds)
add('animSpeed', animConfig.tuning.animSpeed, 'anim')
add('animLimit', animConfig.tuning.animLimit, 'anim')
add('showSpeed', true, 'menu')      -- the speedometer (overlay default, beamng/mod sonic.lua settings.showSpeed)

-- the settings' setters: module, function
local SETTERS = {
  body = {'sonicnative_body', 'setSettings'},
  sound = {'sonicnative_sound', 'setSettings'},
  anim = {'sonicnative_main', 'setTuning'},
}
local CHOICES = {
  sonicMode = {auto = true, always = true, off = true},
  impactMode = {mass = true, plow = true, bounce = true, off = true},
}
-- a dragged slider sends the body's settings at most this often (each send is a log line in the
-- walking body's Lua); the sounds and the animation take them every frame
local BODY_SEND_SECONDS = 0.2

local settings = {}
for k, v in pairs(defaults) do settings[k] = v end
local loadedFrom = 'defaults'      -- 'defaults', 'file' or 'unreadable' (status)
local dirty = {}                   -- target -> changed since last sent
local clock, bodySentAt = 0, -100

local function loaded(name)
  if not extensions.isExtensionLoaded(name) then return nil end
  local e = extensions[name]
  if type(e) == 'table' then return e end
end

-- a value from the file or typed in: the default's type, a known choice, a finite number; nil if not
local function valid(key, v)
  local d = defaults[key]
  if d == nil or type(v) ~= type(d) then return nil end
  if type(v) == 'number' then
    if v ~= v or v <= -1e9 or v >= 1e9 then return nil end
    if key == 'jumpStyle' then v = math.max(0, math.min(2, math.floor(v + 0.5))) end
    if key == 'ballSpinTurns' and v < 0 then v = -1 end   -- any negative value: Frontiers' speed
  elseif CHOICES[key] and not CHOICES[key][v] then
    return nil
  end
  return v
end

-- the saved settings over the defaults; missing or unknown keys and bad values keep the default
local function loadSettings()
  local text = FS:fileExists(SETTINGS_FILE) and readFile(SETTINGS_FILE) or nil
  if not text then loadedFrom = 'defaults' return end
  local ok, saved = pcall(json.decode, text)
  if not ok or type(saved) ~= 'table' then
    loadedFrom = 'unreadable'
    log('W', logTag, SETTINGS_FILE .. ' cannot be read: Sonic starts with the default settings')
    return
  end
  local count = 0
  for k, v in pairs(saved) do
    local good = valid(k, v)
    if good ~= nil then settings[k] = good; count = count + 1 end
  end
  loadedFrom = 'file'
  log('I', logTag, string.format('%d settings loaded from %s', count, SETTINGS_FILE))
end

local function saveSettings()
  if jsonWriteFile(SETTINGS_FILE, settings, true) then
    log('I', logTag, 'settings saved in ' .. SETTINGS_FILE)
  else
    log('W', logTag, 'cannot save the settings in ' .. SETTINGS_FILE)
  end
end

local function send(target)
  dirty[target] = nil
  if target == 'body' then bodySentAt = clock end
  local s = SETTERS[target]
  local e = s and loaded(s[1])
  if not (e and type(e[s[2]]) == 'function') then return end
  local values = {}
  for k, v in pairs(settings) do
    if owner[k] == target then values[k] = v end
  end
  e[s[2]](values)
end

local function sendAll()
  for target in pairs(SETTERS) do send(target) end
end

-- a setting changed in the menu or the Lab: sent on this frame (the body's within BODY_SEND_SECONDS)
local function changed(key)
  if SETTERS[owner[key]] then dirty[owner[key]] = true end
end

local function sendChanges()
  if dirty.sound then send('sound') end
  if dirty.anim then send('anim') end
  if dirty.body and clock - bodySentAt >= BODY_SEND_SECONDS then send('body') end
end

local function unstuck()
  local body = loaded('sonicnative_body')
  if not (body and body.unstuck) then
    log('W', logTag, 'Unstuck: sonicnative_body is not loaded')
    return false
  end
  return body.unstuck()
end

-- Sonic Beam menu (key O): Sonic mode, speed limit, top speed, boost acceleration, speedometer
local menuOpen, menuWidgets = false, nil

local function toggleMenu()
  if not im then return end
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

-- Sonic Beam Lab: settings we tune together, opened from the O menu. Saved when the Lab closes.
local LAB_MIN_DB, LAB_MAX_DB = soundConfig.rules.muteDb, 12   -- the overlay's range; the lowest mutes
local LAB_FLOATS = {'stepsDb', 'soundDb', 'windDb', 'boostBoomKmh', 'waterRunKmh', 'swimKmh', 'wallRunKmh', 'wallGrip',
  'impactForce', 'carDamage', 'ballSpinTurns', 'jumpHeight', 'animSpeed', 'animLimit'}
local LAB_BOOLS = {'boostBoom', 'runOnWater', 'runOnWalls'}
local labOpen, labWidgets = false, nil

local function openLab(tab)
  if not im then return end
  labOpen = true
  labWidgets = {open = im.BoolPtr(true), place = true, tab = tab}   -- tab: open on it once (developer tests)
  for _, k in ipairs(LAB_FLOATS) do labWidgets[k] = im.FloatPtr(settings[k]) end
  for _, k in ipairs(LAB_BOOLS) do labWidgets[k] = im.BoolPtr(settings[k]) end
end

-- a Lab tab; selected once when the Lab was opened on it
local function labTab(name)
  local w = labWidgets
  local flags = 0
  if w.tab == name then flags = im.TabItemFlags_SetSelected; w.tab = nil end
  return im.BeginTabItem(name, nil, flags)
end

-- how a value is written beside its slider, and on the button back to its default
local UNITS = {
  db = {value = function(x) return x <= LAB_MIN_DB and 'muted' or string.format('%+.1f dB', x) end,
    reset = function(x) return string.format('%g dB', x) end},
  times = {value = function(x) return string.format('x%.2f', x) end, reset = function(x) return string.format('x%g', x) end},
  kmh = {value = function(x) return string.format('%.0f km/h', x) end, reset = function(x) return string.format('%g', x) end},
  seconds = {value = function(x) return string.format('%.1f s', x) end, reset = function(x) return string.format('%g s', x) end},
  turns = {value = function(x) return x < 0 and "Frontiers' speed" or string.format('%.1f turns/s', x) end,
    reset = function(x) return x < 0 and 'Frontiers' or string.format('%g', x) end},
}

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
      local v = valid(key, editBuf[0])
      if v ~= nil then settings[key] = v; ptr[0] = v; return true end
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

-- one Lab setting: slider, its value (click to type it), and a button back to the default
local function labSlider(label, key, low, high, unit)
  local ptr = labWidgets[key]
  local now = false
  im.PushItemWidth(260 * im.uiscale[0])
  if im.SliderFloat(label .. '##' .. key, ptr, low, high, '', nil) then
    settings[key] = clamp(ptr[0], low, high)
    now = true
  end
  im.PopItemWidth()
  im.SameLine()
  if editableValue(key, ptr, unit.value) then now = true end
  im.SameLine()
  if im.SmallButton(unit.reset(defaults[key]) .. '##reset' .. key) then ptr[0] = defaults[key]; settings[key] = defaults[key]; now = true end
  if now then changed(key) end
  return now
end

-- a switch; true while it is on (the sliders it governs are disabled while it is off)
local function labSwitch(label, key)
  local ptr = labWidgets[key]
  if im.Checkbox(label .. '##' .. key, ptr) then settings[key] = ptr[0]; changed(key) end
  return ptr[0]
end

-- one box per choice, only one ticked at a time
local function labChoices(key, choices)
  for _, c in ipairs(choices) do
    if im.Checkbox(c.label .. '##' .. key .. tostring(c.id), im.BoolPtr(settings[key] == c.id)) then
      settings[key] = c.id
      changed(key)
    end
  end
end

-- Lab, Impact tab: what a hit does to Sonic
local IMPACT_MODES = {
  {id = 'mass', label = 'Sonic loses speed by the mass of the car'},
  {id = 'plow', label = 'Sonic keeps his speed and plows through'},
  {id = 'bounce', label = 'Sonic bounces off the car'},
  {id = 'off', label = "Off: BeamNG's own collision only"},
}

-- Lab, Jump tab: the jump styles of sonicnative_main (anim.lua tuning.jumpStyle)
local JUMP_STYLES = {
  {id = 0, label = 'Ball for the whole jump (no tricks)'},
  {id = 1, label = 'Ball going up, then tricks or a normal fall after the top of the jump'},
  {id = 2, label = 'Ball, and a trick when you hold a direction in the air (A, D or S)'},
}

local function drawLab()
  if not labOpen or not im then return end
  local w = labWidgets
  if w.place then
    -- forced once per opening (BeamNG remembers window positions between sessions): left side
    w.place = false
    local view = im.GetMainViewport()
    im.SetNextWindowPos(im.ImVec2(view.Pos.x + view.Size.x * 0.02, view.Pos.y + view.Size.y * 0.24), im.Cond_Always, im.ImVec2(0, 0))
    im.SetNextWindowSize(im.ImVec2(600 * im.uiscale[0], 0), im.Cond_Always)
  end
  im.Begin('Sonic Beam Lab##sonicnativeLab', w.open, im.WindowFlags_NoCollapse + im.WindowFlags_AlwaysAutoResize)   -- the window fits the open tab
  -- one tab per theme; Close and save stays below whichever tab is open
  if im.BeginTabBar('##sonicnativeLabTabs') then
    if labTab('Sound') then
      labSlider('Footsteps', 'stepsDb', LAB_MIN_DB, LAB_MAX_DB, UNITS.db)
      labSlider('All Sonic sounds', 'soundDb', LAB_MIN_DB, LAB_MAX_DB, UNITS.db)
      labSlider('Wind', 'windDb', LAB_MIN_DB, LAB_MAX_DB, UNITS.db)
      local on = labSwitch('Sonic boom on a fast boost', 'boostBoom')
      if not on then im.BeginDisabled(true) end
      labSlider('  at speed', 'boostBoomKmh', 50, 1000, UNITS.kmh)
      if not on then im.EndDisabled() end
      im.EndTabItem()
    end
    if labTab('Water') then
      im.TextUnformatted('Out of his depth, Sonic runs on the water when fast enough, else he swims:')
      local on = labSwitch('Run on water', 'runOnWater')
      if not on then im.BeginDisabled(true) end
      labSlider('Water run speed', 'waterRunKmh', 20, 300, UNITS.kmh)
      if not on then im.EndDisabled() end
      labSlider('Swim speed', 'swimKmh', 2, 30, UNITS.kmh)
      im.EndTabItem()
    end
    if labTab('Wall') then
      im.TextUnformatted('Boosting into a steep wall, Sonic runs on it: up when he meets it head on, along it otherwise.')
      im.TextUnformatted('Release the boost to drop off; jump to leap away from it.')
      local on = labSwitch('Run on walls', 'runOnWalls')
      if not on then im.BeginDisabled(true) end
      labSlider('Wall run speed', 'wallRunKmh', 40, 400, UNITS.kmh)
      labSlider('Wall grip', 'wallGrip', 0.5, 10, UNITS.seconds)
      if not on then im.EndDisabled() end
      im.EndTabItem()
    end
    if labTab('Impact') then
      im.TextUnformatted('When Sonic runs into a vehicle:')
      labChoices('impactMode', IMPACT_MODES)
      im.Spacing()
      labSlider('Impact force', 'impactForce', 0, 5, UNITS.times)
      labSlider('Car damage', 'carDamage', 0, 3, UNITS.times)
      im.EndTabItem()
    end
    if labTab('Jump') then
      im.TextUnformatted('Sonic curls up into a ball when he jumps:')
      labChoices('jumpStyle', JUMP_STYLES)
      im.Spacing()
      labSlider('Ball spin speed', 'ballSpinTurns', 0, 8, UNITS.turns)
      labSlider('Jump height', 'jumpHeight', 0.5, 3, UNITS.times)
      im.EndTabItem()
    end
    if labTab('Animation') then
      labSlider('Animation speed', 'animSpeed', 0.5, 3, UNITS.times)
      labSlider('Animation speed limit', 'animLimit', 0.5, 3, UNITS.times)
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
  if w.place then
    -- forced once per opening (BeamNG remembers window positions between sessions): upper centre
    w.place = false
    local view = im.GetMainViewport()
    im.SetNextWindowPos(im.ImVec2(view.Pos.x + view.Size.x / 2, view.Pos.y + view.Size.y * 0.24), im.Cond_Always, im.ImVec2(0.5, 0))
    im.SetNextWindowSize(im.ImVec2(560 * im.uiscale[0], 0), im.Cond_Always)
  end
  im.Begin('Sonic Beam##sonicnativeMenu', w.open, im.WindowFlags_NoCollapse)
  im.Text('Boost (Shift) keeps adding speed while you hold it.')
  im.Separator()
  if im.Checkbox('Speed limit', w.capOn) then settings.speedCapOn = w.capOn[0]; changed('speedCapOn') end
  local capOn = w.capOn[0]
  if not capOn then im.BeginDisabled(true) end
  im.PushItemWidth(200 * im.uiscale[0])
  if im.InputFloat('Top speed (km/h)', w.capKmh, 50, 500, '%.0f') then
    w.capKmh[0] = clamp(w.capKmh[0], 20, 1000000)
    settings.speedCapKmh = w.capKmh[0]; changed('speedCapKmh')
  end
  if not capOn then im.EndDisabled() end
  if im.SliderFloat('Boost acceleration (km/h per s)', w.ramp, 10, 500, '', nil) then
    settings.boostRampKmhPerSecond = clamp(w.ramp[0], 10, 500); changed('boostRampKmhPerSecond')
  end
  im.SameLine()
  if editableValue('boostRampKmhPerSecond', w.ramp, function(x) return string.format('%.0f', x) end) then changed('boostRampKmhPerSecond') end
  im.PopItemWidth()
  if im.Checkbox('Show speed', w.showSpeed) then settings.showSpeed = w.showSpeed[0] end
  im.Separator()
  im.TextUnformatted('Sonic:')
  for _, mode in ipairs(SONIC_MODES) do
    im.SameLine()
    if im.RadioButton1(mode.label, settings.sonicMode == mode.id) then settings.sonicMode = mode.id; changed('sonicMode') end
  end
  for _, mode in ipairs(SONIC_MODES) do
    if settings.sonicMode == mode.id then im.TextUnformatted('  ' .. mode.about) end
  end
  im.Separator()
  if im.Button('Unstuck: back to the last safe spot (U)') then w.open[0] = false; unstuck() end
  if im.Button('Lab: settings we tune together') then w.open[0] = false; openLab() end
  if im.Button('Close (O)') then w.open[0] = false end
  im.End()
  if not w.open[0] then toggleMenu() end
end

-- BeamNG's own screens (pause menu, main menu, garage...): the speedometer hides while one is open,
-- as the overlay's HUD did (beamng/mod sonic.lua beamngMenuOpen). The gameplay route is "play".
local menuCheckAt, beamngMenu = -100, false
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

-- Speedometer (menu: Show speed): Sonic's speed at the top left while he is on foot, as the
-- overlay's renderer drew it (beamng/src/game.cpp:215 and 720-727: speed in 3D smoothed over
-- 0.08 s, light blue, about 3 % of the screen high)
local SPEED_SMOOTHING = 0.08      -- s
local SPEED_FONT_SCALE = 2
local shownKmh = 0
local function drawSpeed(dt)
  local body = loaded('sonicnative_body')
  local st = body and body.getState and body.getState()
  if not (st and st.active) then shownKmh = 0 return end
  local _, vel = body.getBody()
  local kmh = vel and vel:length() * 3.6 or 0
  shownKmh = shownKmh + (kmh - shownKmh) * (1 - math.exp(-dt / SPEED_SMOOTHING))
  if not settings.showSpeed or not im or beamngMenuOpen() then return end
  local view = im.GetMainViewport()
  im.SetNextWindowPos(im.ImVec2(view.Pos.x + view.Size.x * 0.03, view.Pos.y + view.Size.y * 0.07), im.Cond_Always, im.ImVec2(0, 0))
  im.SetNextWindowBgAlpha(0.35)
  -- distinct bits only: the flags are added, and NoInputs already holds NoNav
  im.Begin('##sonicnativeSpeed', nil, im.WindowFlags_NoDecoration + im.WindowFlags_NoInputs + im.WindowFlags_AlwaysAutoResize
    + im.WindowFlags_NoSavedSettings + im.WindowFlags_NoFocusOnAppearing)
  im.SetWindowFontScale(SPEED_FONT_SCALE)
  im.TextColored(im.ImVec4(0.55, 0.85, 1, 1), string.format('%.0f KM/H', math.min(shownKmh, 99999999)))
  im.End()
end

local function onUpdate(dtReal)
  local dt = dtReal or 0
  clock = clock + dt
  drawSpeed(dt)
  drawMenu()
  drawLab()
  sendChanges()
end

local function onExtensionLoaded()
  setExtensionUnloadMode(M, 'manual')
  loadSettings()
  sendAll()   -- before Sonic takes a body: the body sends them on with its first frame
  log('I', logTag, 'Sonic Beam menu loaded')
end

-- a window still open keeps what was changed in it
local function onExtensionUnloaded()
  if menuOpen or labOpen then saveSettings() end
  menuOpen, labOpen, editKey = false, false, nil
end

local function onModDeactivated(mod)
  if type(mod) ~= 'table' or mod.modname ~= 'sonicnative' then return end
  -- off the manual list, as sonicnative_main (modScript.lua loads it again with the mod)
  setExtensionUnloadMode('sonicnative_menu', 'auto')
  extensions.unload('sonicnative_menu')
end

M.onExtensionLoaded = onExtensionLoaded
M.onExtensionUnloaded = onExtensionUnloaded
M.onModDeactivated = onModDeactivated
M.onUpdate = onUpdate
-- input actions (O and U by default)
M.toggleMenu = toggleMenu
M.unstuck = unstuck
-- developer API, for the MCP run_lua tool
M.openLab = openLab
M.isMenuOpen = function() return menuOpen end
-- changes settings live as the menu does (checked like the file's values), not saved until a window closes
M.setSettings = function(changes)
  for k, v in pairs(type(changes) == 'table' and changes or {}) do
    local good = valid(k, v)
    if good ~= nil then settings[k] = good; changed(k) end
  end
  bodySentAt = -100
  sendChanges()
  return settings
end
M.save = saveSettings
M.status = function()
  return {menuOpen = menuOpen, labOpen = labOpen, file = SETTINGS_FILE, loadedFrom = loadedFrom, settings = settings, shownKmh = shownKmh}
end

return M
