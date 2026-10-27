-- SPDX-License-Identifier: MIT
-- Sonic Beam, native mode: the O menu, the Lab and the Unstuck key. The menu (Sonic mode, speed
-- limit, top speed, boost acceleration, speedometer, Unstuck, Lab) and the Lab are the overlay's
-- (beamng/mod/lua/ge/extensions, sonicfrontiers/sonic.lua drawMenu and drawLab, v0.12.3), with the
-- Lab tabs the native mode has: Sound, Boost, Aura, Motion blur (native only), Water, Wall, Impact,
-- Jump, Animation. Values
-- apply live through sonicnative_body.setSettings, sonicnative_sound.setSettings,
-- sonicnative_fx.setSettings (the boost effects), sonicnative_sharp.setSettings (the motion blur
-- that keeps Sonic sharp) and sonicnative_main.setTuning.
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
local effects = require('sonicnative/effects')
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
for k, v in pairs(effects.settings) do add(k, v, 'fx') end    -- boost effects: trail, lightning, blur, aura, light
for k, v in pairs(effects.sharp.settings) do add(k, v, 'sharp') end   -- the motion blur that keeps Sonic sharp
add('showSpeed', true, 'menu')      -- the speedometer (overlay default, beamng/mod sonic.lua settings.showSpeed)

-- the settings' setters: module, function
local SETTERS = {
  body = {'sonicnative_body', 'setSettings'},
  sound = {'sonicnative_sound', 'setSettings'},
  anim = {'sonicnative_main', 'setTuning'},
  fx = {'sonicnative_fx', 'setSettings'},
  sharp = {'sonicnative_sharp', 'setSettings'},
}
local CHOICES = {
  sonicMode = {auto = true, always = true, off = true},
  impactMode = {mass = true, plow = true, bounce = true, off = true},
}
-- a dragged slider sends the body's settings at most this often (each send is a log line in the
-- walking body's Lua); the sounds, the animation and the effects take them every frame
local BODY_SEND_SECONDS = 0.2

local settings = {}
for k, v in pairs(defaults) do settings[k] = v end
local loadedFrom = 'defaults'      -- 'defaults', 'file' or 'unreadable' (status)
local dirty = {}                   -- target -> changed since last sent
local sentTo = {}                  -- target -> the extension table its settings were last sent to
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
  if e then sentTo[target] = e end
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
  -- a module loaded after the menu, or reloaded, has not had the settings yet: it gets them now
  for target, s in pairs(SETTERS) do
    local e = loaded(s[1])
    if e and e ~= sentTo[target] then dirty[target] = true end
  end
  if dirty.sound then send('sound') end
  if dirty.anim then send('anim') end
  if dirty.fx then send('fx') end
  if dirty.sharp then send('sharp') end
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
  'impactForce', 'carDamage', 'ballSpinTurns', 'jumpHeight', 'animSpeed', 'animLimit',
  'trailLength', 'trailIntensity', 'lightning', 'trailFade', 'fxSpeedAmount', 'blurStrength', 'boostLight',
  'auraIntensity', 'auraLightning', 'motionBlurScale'}
local LAB_BOOLS = {'boostBoom', 'runOnWater', 'runOnWalls', 'fxBySpeed', 'sharpSonic'}
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
  metres = {value = function(x) return string.format('%.2f m', x) end, reset = function(x) return string.format('%g m', x) end},
  -- Speed influence: the share of the effects that follows the speed, and what they start from at standstill
  influence = {value = function(x) return string.format('%.2f (from %d %%)', x, math.floor((1 - x) * 100 + 0.5)) end,
    reset = function(x) return string.format('%g', x) end},
  turns = {value = function(x) return x < 0 and "Frontiers' speed" or string.format('%.1f turns/s', x) end,
    reset = function(x) return x < 0 and 'Frontiers' or string.format('%g', x) end},
}

-- what each Lab setting does, shown when the mouse is over it (checked against the code that uses it)
local LAB_TIPS = {
  stepsDb = "Volume of Sonic's footsteps, on top of All Sonic sounds. Higher is louder, lower is quieter; the lowest setting mutes them. Landing sounds are not footsteps and do not change.",
  soundDb = 'Volume of every Sonic sound: footsteps, jumps, boost, wind, sonic booms and the rest. The Footsteps and Wind sliders stack on top of it. Higher is louder; the lowest setting mutes Sonic completely.',
  windDb = 'Volume of the rushing wind heard at high speed and while boosting. Higher is louder, lower is quieter; the lowest setting mutes it. It stacks with All Sonic sounds.',
  boostBoom = 'On: a boost started at or above the speed below sets off a sonic boom, left behind where it began; landing while boosting counts as a new start. Off: boosts never boom. Fast car hits boom either way.',
  boostBoomKmh = 'Sonic must already be this fast when a boost starts, or when he lands while boosting, for it to boom; speeding past it mid-boost does not count. Lower booms more often. Only used while the switch above is on.',
  trailLength = 'How long the blue trail lingers behind Sonic while he boosts, and so how far back it reaches. Higher makes it longer; lower makes it shorter.',
  trailIntensity = 'How bright the blue trail behind Sonic is while he boosts. Higher makes it brighter; 0 hides the trail and the lightning arcs along it.',
  lightning = "How many lightning bolts crackle around Sonic and his trail while he boosts or as a boost starts, and how bright the trail's arcs are. 0 removes them. Bolts on his body are set in the Aura tab.",
  trailFade = "How many metres the trail takes to fade in behind Sonic's back. Higher makes it start softer and further back; 0 starts it at full strength right behind him.",
  fxBySpeed = "On: the trail, the lightning and the aura grow with Sonic's speed and reach full strength at the speed shown below. Off: they stay at full strength at any speed. The speed blur and boost light are not affected.",
  fxSpeedAmount = "How much the effects depend on Sonic's speed. At 0, speed changes nothing; at 1, they start from nothing when he stands still. The percentage is their strength standing still. Only used while the switch above is on.",
  blurStrength = 'Strength of the radial blur streaking the screen toward Sonic from 150 km/h (strongest at 600, half without the boost key) and briefly when a boost starts or he hits a car. Higher gives longer streaks; 0 turns it off.',
  boostLight = 'How bright the blue light is that flashes on the surroundings when a boost starts or Sonic hits a car. Higher is brighter; 0 turns it off.',
  auraIntensity = "How bright the blue aura on Sonic's body is while he boosts or runs fast. Higher is brighter; 0 hides it. It fades while he is a ball, and grows with his speed when the Boost tab's speed switch is on.",
  auraLightning = "How many lightning bolts crackle on Sonic's body while his aura is lit. Higher gives more; 0 removes them. They work even with the aura intensity at 0, follow the Boost tab's speed switch, and hide while he is a ball.",
  sharpSonic = "On: while Sonic is on foot, the scenery still blurs but Sonic stays sharp. Off: BeamNG's own motion blur smears Sonic too. Needs motion blur turned on in BeamNG's graphics options.",
  motionBlurScale = "How strong the motion blur is on foot, relative to BeamNG's own strength setting. Higher blurs the scenery more, up to a limit; 0 removes the motion blur on foot. Only used while the switch above is on.",
  runOnWater = 'On: Sonic runs on top of the water once he is fast enough, even where it is shallow. Off: he never runs on water and swims wherever it is too deep to stand.',
  waterRunKmh = 'How fast Sonic must go to run on water; once on it, he stays up until a bit slower than this. Lower lets him run on it slower, higher needs more speed. Only used while Run on water is on.',
  swimKmh = "Sonic's top speed while swimming in water too deep to stand in. Higher swims faster, lower slower. Boost does not speed him up while he swims.",
  runOnWalls = 'On: boosting into a steep wall at the Wall run speed or faster, Sonic runs on it: straight up when he meets it head on, along it otherwise. Off: he never runs on walls.',
  wallRunKmh = 'The speed Sonic needs, boosting into a wall, to start running on it; it does not limit his speed on the wall. Lower catches walls at slower speeds, higher needs more. Only used while Run on walls is on.',
  wallGrip = 'How long Sonic can stay on a wall before he falls away from it. Higher lets him run further up or along it; lower drops him sooner. Only used while Run on walls is on.',
  impactForce = "How hard Sonic hits, as if he weighed this many tonnes. Higher knocks cars further and, when he loses speed by the car's mass, slows him less; at 0 his hits do not move cars. Unused when impacts are off.",
  carDamage = 'How much a car crumples where Sonic hits it. Higher puts more of the blow on the spot he hits, denting it deeper; 0 pushes the whole car evenly. No effect when Impact force is 0 or impacts are off.',
  ballSpinTurns = "How fast Sonic's jump ball rolls, in turns per second. Higher spins faster; 0 stops it spinning. Type a negative value to use Frontiers' own speed.",
  jumpHeight = 'How high Sonic jumps. The height follows the value: x2 jumps about twice as high, x0.5 about half as high. Jumps off a wall keep their own strength.',
  animSpeed = "How fast Sonic's legs move for his speed when walking, running, boosting or running on walls. Higher makes his strides quicker, lower slower, and his footsteps follow. The limit below still caps it.",
  animLimit = "The fastest Sonic's legs may move when walking, running, boosting or running on walls. Higher lets his strides keep speeding up at very high speed; lower caps them sooner for a calmer look.",
}

-- a tooltip for the item just drawn while the mouse is over it, greyed-out items included, wrapped
-- like BeamNG's own help markers (imgui ShowHelpMarker); it goes away when the mouse leaves
local function tip(text)
  if text and im.IsItemHovered(im.HoveredFlags_AllowWhenDisabled) then
    im.BeginTooltip()
    im.PushTextWrapPos(im.GetFontSize() * 30)
    im.TextUnformatted(text)
    im.PopTextWrapPos()
    im.EndTooltip()
  end
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
  -- only while it can be clicked (not on a greyed-out slider)
  if im.IsItemHovered() then im.SetTooltip("Click to type a value, even beyond the slider's range") end
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
  tip(LAB_TIPS[key])
  im.PopItemWidth()
  im.SameLine()
  if editableValue(key, ptr, unit.value) then now = true end
  im.SameLine()
  if im.SmallButton(unit.reset(defaults[key]) .. '##reset' .. key) then ptr[0] = defaults[key]; settings[key] = defaults[key]; now = true end
  tip('Back to the default value (' .. unit.reset(defaults[key]) .. ')')
  if now then changed(key) end
  return now
end

-- a switch; true while it is on (the sliders it governs are disabled while it is off)
local function labSwitch(label, key)
  local ptr = labWidgets[key]
  if im.Checkbox(label .. '##' .. key, ptr) then settings[key] = ptr[0]; changed(key) end
  tip(LAB_TIPS[key])
  return ptr[0]
end

-- one box per choice, only one ticked at a time
local function labChoices(key, choices)
  for _, c in ipairs(choices) do
    if im.Checkbox(c.label .. '##' .. key .. tostring(c.id), im.BoolPtr(settings[key] == c.id)) then
      settings[key] = c.id
      changed(key)
    end
    tip(c.tip)
  end
end

-- Lab, Impact tab: what a hit does to Sonic
local IMPACT_MODES = {
  {id = 'mass', label = 'Sonic loses speed by the mass of the car', tip = 'Sonic knocks the car away but loses speed by its weight: the heavier the car, the more he slows, and a very heavy one stops him. A higher Impact force makes him lose less.'},
  {id = 'plow', label = 'Sonic keeps his speed and plows through', tip = "The hit itself takes none of Sonic's speed: he knocks the car away and plows on. How far the car flies depends on Impact force and the car's weight."},
  {id = 'bounce', label = 'Sonic bounces off the car', tip = "Sonic rebounds off the car, thrown back at more than half his hitting speed, and the car gets a harder shove than in the other modes. Impact force changes only the car's shove."},
  {id = 'off', label = "Off: BeamNG's own collision only", tip = "No special hit: Sonic and the car just collide through BeamNG's normal physics. Impact force and Car damage do nothing in this mode."},
}

-- Lab, Jump tab: the jump styles of sonicnative_main (anim.lua tuning.jumpStyle)
local JUMP_STYLES = {
  {id = 0, label = 'Ball for the whole jump (no tricks)', tip = 'Sonic stays curled in his spinning ball from take-off until he lands, with no tricks however long the jump. Falling off a ledge without jumping can still play tricks.'},
  {id = 1, label = 'Ball going up, then tricks or a normal fall after the top of the jump', tip = 'Sonic is a ball on the way up and uncurls once he falls past the top of the jump. After almost a second in the air he does tricks; before that he takes his falling pose.'},
  {id = 2, label = 'Ball, and a trick when you hold a direction in the air (A, D or S)', tip = 'Sonic stays in his ball the whole jump. Holding left, right or back (A, D or S) in the air plays a trick, then he curls back up; release and press again for another trick.'},
}

-- Lab, Boost tab: the speed at which the boost effects are at full strength, the O menu's top
-- speed, or effects.lua's reference when the speed limit is off (overlay sonic.lua fxReferenceKmh)
local function fxReferenceKmh()
  local c = effects.common
  return settings.speedCapOn and math.max(c.refKmhMin, settings.speedCapKmh) or c.refKmhNoCap
end

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
    if labTab('Boost') then
      labSlider('Trail length', 'trailLength', 0.25, 3, UNITS.times)
      labSlider('Trail intensity', 'trailIntensity', 0, 2, UNITS.times)
      labSlider('Lightning', 'lightning', 0, 2, UNITS.times)
      labSlider('Trail start fade', 'trailFade', 0, 4, UNITS.metres)
      im.Spacing()
      local bySpeed = labSwitch("Effects follow Sonic's speed", 'fxBySpeed')
      im.TextUnformatted(string.format('  full effects at %.0f km/h (%s)', fxReferenceKmh(),
        settings.speedCapOn and 'top speed, O menu' or 'speed limit off'))
      if not bySpeed then im.BeginDisabled(true) end
      labSlider('Speed influence', 'fxSpeedAmount', 0, 1, UNITS.influence)
      if not bySpeed then im.EndDisabled() end
      im.Spacing()
      labSlider('Speed blur strength', 'blurStrength', 0, 2, UNITS.times)
      labSlider('Boost light', 'boostLight', 0, 2, UNITS.times)   -- native only: the blue light of a boost start
      im.EndTabItem()
    end
    if labTab('Aura') then
      -- the overlay's body-shape switch, thickness and length are not in the native mode
      im.TextUnformatted('The blue aura on Sonic while he boosts or runs fast:')
      labSlider('Aura intensity', 'auraIntensity', 0, 2, UNITS.times)
      labSlider('Aura lightning', 'auraLightning', 0, 3, UNITS.times)
      im.EndTabItem()
    end
    if labTab('Motion blur') then
      -- native only: BeamNG's motion blur spares only vehicles and smeared Sonic (sonicnative_sharp)
      local ext = loaded('sonicnative_sharp')
      local sharp = ext and type(ext.getState) == 'function' and ext.getState() or nil
      im.TextUnformatted("BeamNG's motion blur (Options > Graphics) while Sonic is on foot:")
      local on = labSwitch('Keep Sonic sharp (the scenery stays blurred)', 'sharpSonic')
      if not on then im.BeginDisabled(true) end
      labSlider('Motion blur strength', 'motionBlurScale', 0, 2, UNITS.times)
      if not on then im.EndDisabled() end
      if sharp and not sharp.beamngOn then
        im.TextUnformatted('Motion blur is off in the options: nothing to do.')
      end
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
  tip('Close the Lab and save these settings: they stay for your next games.')
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
  sendAll()   -- before Sonic takes a body: the body sends them on with its first frame (a module loaded later: sendChanges)
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
