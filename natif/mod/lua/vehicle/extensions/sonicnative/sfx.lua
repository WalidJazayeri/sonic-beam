-- SPDX-License-Identifier: MIT
-- Sonic Beam, native mode: plays Sonic's sounds through BeamNG's own audio, from inside the walking
-- body (loaded with sonicnative_runner by lua/ge/extensions/sonicnative/body.lua). The game engine side
-- picks what to play and when; the files are the converter's WAVs in the user folder
-- (/mods/sonicnative_data/sounds/...). One-shots follow the body, or stay where they started (the
-- sonic boom); loops are made on first use, and again after the body's sounds were re-initialised.
-- Pause is the game engine side's (sound.lua reads simTimeAuthority.getPause and sends stopAll).
local M = {}

local count = 0                   -- loops made, for unique profile names
local soundNode = nil             -- node the sounds sit on (soundNodeId)
local loops = {}                  -- name -> {src, file, want, wantPitch, vol, pitch, on}
local exists = {}                 -- file -> true/false, looked up once (a missing file is logged once)
local errors = 0

-- the ball's centre (the runner's ballCenter), else the body's reference node
local function soundNodeId()
  if soundNode then return soundNode end
  for _, n in pairs(v.data.nodes or {}) do
    if n.name == 'ball_42' then soundNode = n.cid break end
  end
  if not soundNode then
    local ref = v.data.refNodes and v.data.refNodes[0]
    soundNode = ref and ref.ref or 0
  end
  return soundNode
end

local function fileExists(file)
  local e = exists[file]
  if e == nil then
    e = FS:fileExists(file) and true or false
    exists[file] = e
    if not e then log('W', 'sonicnative', 'sound file missing: ' .. file) end
  end
  return e
end

-- the overlay's pitch range (beamng/src/audio.cpp, SetFrequencyRatio 0.25..4)
local function pitchOf(p)
  return clamp(tonumber(p) or 1, 0.25, 4)
end

-- set a loop's source to what it should play now; the thresholds are the game's own
-- (vehicle/sounds.lua soundObj:setVolumePitch)
local function apply(l)
  if not l.src then return end
  local vol = l.want
  if vol <= 0.001 then
    if l.on then
      obj:setVolumePitch(l.src, 0, l.pitch)
      obj:stopSFX(l.src)
      l.on, l.vol = false, 0
    end
    return
  end
  if math.abs(vol - l.vol) >= 0.01 or math.abs(l.wantPitch - l.pitch) >= 0.0025 then
    obj:setVolumePitch(l.src, vol, l.wantPitch)
    l.vol, l.pitch = vol, l.wantPitch
  end
  if not l.on then
    obj:cutSFX(l.src)
    obj:playSFX(l.src)
    l.on = true
  end
end

local function newLoop(name, file)
  local l = {src = false, file = file, want = 0, wantPitch = 1, vol = 0, pitch = 1, on = false}
  if fileExists(file) then
    count = count + 1
    -- one profile per source (as vehicle/sounds.lua getNextProfile); a profile keeps its sound data, so
    -- the file's name is in it: after a Lua reload a reused name still means the same file
    local base = file:match('([^/\\]+)$') or name
    local profile = 'sonicnative_' .. obj:getId() .. '_' .. count .. '_' .. base:gsub('[^%w_]', '_')
    l.src = obj:createSFXSource(file, 'AudioDefaultLoop3D', profile, soundNodeId()) or false
    if l.src then
      -- a new source may start on its own: quiet until apply() says otherwise
      obj:setVolumePitch(l.src, 0, 1)
      obj:stopSFX(l.src)
    else
      log('W', 'sonicnative', 'sound loop not created: ' .. file)
    end
  end
  return l
end

-- obj:playSFXOnce* only take FMOD events ("profile was not found" for a file path), while
-- obj:createSFXSource loads a raw file: one-shots are sources too, made on first use, two per file
-- so a quick repeat does not cut the one still ringing
local ONESHOT_SOURCES = 2
local oneshots = {}               -- file -> {next, [1..ONESHOT_SOURCES] = source or false}

local function oneshotSource(file)
  local set = oneshots[file]
  if not set then
    set = {next = 1}
    local base = file:match('([^/\\]+)$') or 'sound'
    for i = 1, ONESHOT_SOURCES do
      count = count + 1
      local profile = 'sonicnative_' .. obj:getId() .. '_' .. count .. '_' .. base:gsub('[^%w_]', '_')
      set[i] = obj:createSFXSource(file, 'AudioDefault3D', profile, soundNodeId()) or false
      if set[i] then obj:stopSFX(set[i]) else log('W', 'sonicnative', 'sound not created: ' .. file) end
    end
    oneshots[file] = set
  end
  local src = set[set.next]
  set.next = set.next % ONESHOT_SOURCES + 1
  return src
end

-- one-shot at the ball's centre. Sources sit on a node, so 'static' (the sonic boom, meant to stay
-- where it started) follows Sonic too for now
local function once(static, file, volume, pitch)
  if type(file) ~= 'string' or not fileExists(file) then return end
  volume = tonumber(volume) or 1
  if volume < 0.01 then return end   -- the game's own floor (vehicle/sounds.lua playSoundOnce*)
  local src = oneshotSource(file)
  if not src then return end
  obj:setVolumePitch(src, volume, pitchOf(pitch))
  obj:cutSFX(src)
  obj:playSFX(src)
end

local function loop(name, file, volume, pitch)
  if type(name) ~= 'string' or type(file) ~= 'string' then return end
  local want = math.max(0, tonumber(volume) or 0)
  local l = loops[name]
  if l and l.file ~= file then
    -- another file under the same name: the old source goes quiet, a new one is made
    l.want = 0
    apply(l)
    l = nil
    loops[name] = nil
  end
  if not l then
    if want <= 0.001 then return end   -- made only once it is heard
    l = newLoop(name, file)
    loops[name] = l
  end
  l.want, l.wantPitch = want, pitchOf(pitch)
  apply(l)
end

local function stopAll()
  for _, l in pairs(loops) do
    l.want = 0
    apply(l)
  end
end

-- the body's sounds were re-initialised (vehicle/sounds.lua init deletes every source first):
-- forget the old sources, they are made again on their next use
local function onInit()
  loops, oneshots, exists, soundNode = {}, {}, {}, nil
end

-- called from the game engine side: an error here must never repeat every frame
local function safe(f)
  return function(...)
    local ok, err = pcall(f, ...)
    if not ok and errors < 5 then
      errors = errors + 1
      log('E', 'sonicnative', 'sound: ' .. tostring(err))
    end
  end
end

-- one-shot that follows Sonic (volume 0..1, pitch = playback rate, 1 as recorded)
M.play = safe(function(file, volume, pitch) once(false, file, volume, pitch) end)
-- one-shot left where it started (the sonic boom)
M.playStatic = safe(function(file, volume, pitch) once(true, file, volume, pitch) end)
-- looping sound `name` on `file` at this volume and pitch; volume <= 0.001 silences it
M.loop = safe(loop)
M.stopAll = safe(stopAll)
M.onInit = onInit
M.onExtensionUnloaded = safe(stopAll)
-- developer checks
M.getDebug = function()
  local out = {node = soundNode, loops = {}}
  for name, l in pairs(loops) do out.loops[name] = {file = l.file, made = l.src ~= false, on = l.on, vol = l.vol, pitch = l.pitch} end
  return out
end

return M
