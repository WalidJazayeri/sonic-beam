-- SPDX-License-Identifier: MIT
-- Sonic Beam, native mode: Sonic's sounds, through BeamNG's own audio. This extension decides every
-- sound with the overlay's rules (beamng/src/game.cpp, v0.12.3; the numbers are copied into
-- lua/common/sonicnative/sounds.lua) and has the walking body play them: its vehicle extension
-- sonicnative_sfx attaches them to Sonic, or leaves the sonic boom where it started. The sounds are
-- the player's own Frontiers ones, converted into mods/sonicnative_data/sounds and listed in
-- manifest.json ("sounds"). It reads sonicnative_body (state, velocity, car hits) and
-- sonicnative_main (the clip and its phase, for the footsteps).
-- Not played for now: drift and trick (the native mode has no drift or trick state yet) and the
-- rings (not in the native mode). sonicnative_sound.test(id) still plays any of them.

local M = {}
local logTag = 'sonicnative'

local S = require('sonicnative/sounds')
local R = S.rules

local DATA = '/mods/sonicnative_data/'    -- written on the player's PC by the converter, never shipped
local SFX_FILE = '/lua/vehicle/extensions/sonicnative/sfx.lua'
local SFX_LOAD = "if not sonicnative_sfx then extensions.load('sonicnative_sfx') end"
local SFX_STOP = 'if sonicnative_sfx then sonicnative_sfx.stopAll() end'
local SFX_CHECK_SECONDS = 2       -- the body is asked this often to load sonicnative_sfx again (a Lua reload drops it)
local MAX_EVENTS = 16             -- sounds kept for status()

-- this session's settings (M.setSettings), starting from sounds.lua
local settings = {}
for k, v in pairs(S.settings) do settings[k] = v end

local GROUND = {}
for _, g in ipairs(S.grounds) do GROUND[g] = true end

local sounds, soundCount, missingFiles = {}, 0, 0   -- id -> {files, loops, mode, volume, seconds, last}
local sfxOk = false
local windFile = nil              -- the wind variant looped this session
local warned = {}
local lastError = nil

local clock = 0
local bodyId, sfxCheckAt = nil, -100   -- walking body the commands go to
local out = {}                    -- commands for the body, sent once per frame
local pending = {}                -- layers that start later in their cue: {at = clock, cmd}
local loops = {}                  -- loop name -> {vol, pitch, sentAt} as last sent
local muted, mutedReason = true, 'starting'
local sliderGain, sliderReadAt = 1, -100
local events = {}

-- what the rules remember from one frame to the next
local speedKmh, air, groundLost, airTime = 0, false, 0, 0
local lastJumps, lastHit, wanted = nil, nil, false
local boostCand, boostCandSince, inBoost, boostLoop = false, 0, false, 0
local boostAt, boostEndAt = 0, 0  -- clock when the boost state last started and ended
local wind = 0
local stepClip, stepPhase = nil, 0
local stepsHeldUntil = 0          -- clock until which a landing keeps the footsteps quiet
local surface = 'conc'

local function report(level, key, msg)
  lastError = msg
  if warned[key] then return end
  warned[key] = true
  log(level, logTag, msg)
end

local function loaded(name)
  if not extensions.isExtensionLoaded(name) then return nil end
  local e = extensions[name]
  if type(e) == 'table' then return e end
end

local function currentUnicycle()
  if not gameplay_walk or not gameplay_walk.isWalking() then return nil end
  return gameplay_walk.getCurrentUnicycle()
end

-- manifest paths are absolute in the virtual file system, or relative to the data folder
local function dataPath(p)
  if type(p) ~= 'string' or p == '' then return nil end
  if p:sub(1, 1) == '/' then return p end
  return DATA .. p
end

-- the manifest's "sounds": {id: {files, mode, loop, volume, seconds, delays}}. The converter puts a
-- cue's looping layers in an id of their own (boost_loop); a file may also be {file, loop}, or the
-- entry may have "loops" (one flag per file). delays: when each layer starts after the cue (s).
local function readManifest()
  sounds, soundCount, missingFiles, windFile = {}, 0, 0, nil
  sfxOk = FS:fileExists(SFX_FILE)
  if not sfxOk then report('W', 'sfx', SFX_FILE .. ' not found: Sonic stays silent') end
  local m = jsonReadFile(DATA .. 'manifest.json')
  local list = type(m) == 'table' and m.sounds or nil
  if type(list) ~= 'table' then
    report('W', 'noSounds', 'no "sounds" in ' .. DATA .. 'manifest.json: run the converter again for Sonic\'s sounds')
    return false
  end
  for key, e in pairs(list) do
    local id = type(key) == 'string' and key or (type(e) == 'table' and e.id)
    local fileList = type(e) == 'table' and (e.files or e.variants) or nil
    if type(id) == 'string' and type(fileList) == 'table' then
      local files, isLoop, delays = {}, {}, {}
      for i, f in ipairs(fileList) do
        local path, l = f, nil
        if type(f) == 'table' then path, l = f.file, f.loop end
        if type(l) ~= 'boolean' and type(e.loops) == 'table' then l = e.loops[i] end
        if type(l) ~= 'boolean' then l = e.loop == true end
        local d = type(e.delays) == 'table' and tonumber(e.delays[i]) or 0
        path = dataPath(path)
        if path and FS:fileExists(path) then
          local n = #files + 1
          files[n], isLoop[n], delays[n] = path, l, (d and d > 0) and d or 0
        else
          missingFiles = missingFiles + 1
        end
      end
      if #files > 0 then
        sounds[id] = {files = files, loops = isLoop, delays = delays, mode = e.mode == 'layers' and 'layers' or 'random',
          volume = tonumber(S.volumes[id]) or tonumber(e.volume) or 1, seconds = tonumber(e.seconds) or 0, last = 0}
        soundCount = soundCount + 1
      end
    end
  end
  if missingFiles > 0 then report('W', 'soundFiles', missingFiles .. ' sound files of the manifest are missing: run the converter again') end
  -- one wind variant loops for the whole session (the body makes one source per loop name)
  local w = sounds.wind
  if w then windFile = w.files[math.random(#w.files)] end
  log('I', logTag, string.format('%d Sonic sounds in the manifest', soundCount))
  return soundCount > 0
end

-- decibels -> gain, -40 dB and below muted (beamng/src/audio.cpp:23)
local function dbGain(db)
  db = tonumber(db) or 0
  if db <= R.muteDb then return 0 end
  return 10 ^ (db / 20)
end

-- BeamNG's volume sliders (live values), when followSliders is on; read twice a second
local function readSliders()
  if clock - sliderReadAt < R.sliderReadSeconds then return end
  sliderReadAt = clock
  local g = 1
  local audio = Engine and Engine.Audio
  if settings.followSliders and audio and audio.getChannelVolume then
    for _, ch in ipairs(S.sliderChannels) do
      local ok, v = pcall(audio.getChannelVolume, ch, true)
      if ok and type(v) == 'number' and v == v then g = g * math.max(0, math.min(1, v)) end
    end
  end
  sliderGain = g
end

-- the one gain every sound goes through: its sheet volume, the overlay's master and Lab decibels
-- (footsteps and wind have their own), BeamNG's sliders
local function volumeOf(id)
  local s = sounds[id]
  if not s then return 0 end
  local v = s.volume * S.master * dbGain(settings.soundDb) * sliderGain
  if id:sub(1, 5) == 'step_' then v = v * dbGain(settings.stepsDb) end
  if id == 'wind' then v = v * dbGain(settings.windDb) end
  return v
end

local function push(cmd) out[#out + 1] = cmd end

-- this frame's commands, in one call to the body's Lua
local function flush(target)
  if target and #out > 0 then
    local ok, err = pcall(target.queueLuaCommand, target, table.concat(out, ' '))
    if not ok then report('E', 'queue', 'cannot reach the walking body: ' .. tostring(err)) end
  end
  out = {}
end

local function addEvent(id, files, vol, pitch)
  events[#events + 1] = {t = math.floor(clock * 100 + 0.5) / 100, id = id, files = files,
    volume = math.floor(vol * 1000 + 0.5) / 1000, pitch = math.floor(pitch * 1000 + 0.5) / 1000}
  if #events > MAX_EVENTS then table.remove(events, 1) end
end

-- One sound id now: 'random' picks one file (never the one picked last time), 'layers' plays them
-- all. Looping files are left to the loops unless withLoops (tests). static: left where it started.
local function playId(id, pitch, static, withLoops)
  local s = sounds[id]
  if not s then return 0 end
  local vol = volumeOf(id)
  if vol < 0.01 then return 0 end   -- the game's own floor (vehicle/sounds.lua)
  local fn = static and 'playStatic' or 'play'
  local n = 0
  local function send(i)
    local cmd = string.format('if sonicnative_sfx then sonicnative_sfx.%s(%q, %.3f, %.3f) end', fn, s.files[i], vol, pitch)
    -- a layer that starts later in the cue waits for its time
    local d = s.delays[i] or 0
    if d > 0.0005 then pending[#pending + 1] = {at = clock + d, cmd = cmd} else push(cmd) end
    n = n + 1
  end
  if s.mode == 'layers' then
    for i = 1, #s.files do
      if withLoops or not s.loops[i] then send(i) end
    end
  else
    local pick = {}
    for i = 1, #s.files do
      if (withLoops or not s.loops[i]) and i ~= s.last then pick[#pick + 1] = i end
    end
    -- a single file (or only the last one left) plays again
    if #pick == 0 and s.files[s.last] and (withLoops or not s.loops[s.last]) then pick[1] = s.last end
    if #pick > 0 then
      s.last = pick[math.random(#pick)]
      send(s.last)
    end
  end
  if n > 0 then addEvent(id, n, vol, pitch) end
  return n
end

-- a looping sound at this volume and pitch: sent when it changed (at most loopSendHz times a
-- second), when it starts or stops, and again every loopRefreshSeconds while it plays
local function setLoop(name, file, vol, pitch)
  local l = loops[name]
  if not l then
    l = {vol = -1, pitch = 1, sentAt = -100}
    loops[name] = l
  end
  pitch = math.max(R.loopPitchMin, math.min(R.loopPitchMax, pitch))
  if not (vol > R.silentLevel) then
    if l.vol > 0 then
      push(string.format('if sonicnative_sfx then sonicnative_sfx.loop(%q, %q, 0, %.3f) end', name, file, l.pitch))
      l.sentAt = clock
    end
    l.vol = 0
    return
  end
  local starting = l.vol <= 0
  local changed = math.abs(vol - l.vol) > 0.01 or math.abs(pitch - l.pitch) > 0.005
  if starting or (changed and clock - l.sentAt >= 1 / R.loopSendHz) or clock - l.sentAt >= R.loopRefreshSeconds then
    push(string.format('if sonicnative_sfx then sonicnative_sfx.loop(%q, %q, %.3f, %.3f) end', name, file, vol, pitch))
    l.vol, l.pitch, l.sentAt = vol, pitch, clock
  end
end

-- delayed layers whose time has come join this frame's commands
local function releaseDue()
  if #pending == 0 then return end
  local keep = {}
  for _, p in ipairs(pending) do
    if clock >= p.at then push(p.cmd) else keep[#keep + 1] = p end
  end
  pending = keep
end

-- which looping files of a 'random' id loop this time (one of them); 'layers' loop them all
local function pickLoop(id)
  local s = sounds[id]
  if not s then return end
  s.chosen = nil
  if s.mode ~= 'random' then return end
  local pick = {}
  for i = 1, #s.files do
    if s.loops[i] then pick[#pick + 1] = i end
  end
  if #pick > 0 then s.chosen = pick[math.random(#pick)] end
end

-- the looping files of an id at this level, each from its delay in the cue (ran: seconds since
-- the cue started)
local function loopLayers(id, level, ran)
  local s = sounds[id]
  if not s then return end
  local vol = volumeOf(id) * level
  for i, f in ipairs(s.files) do
    if s.loops[i] then
      local on = (s.chosen == nil or s.chosen == i) and ran >= (s.delays[i] or 0)
      setLoop(id .. '_' .. i, f, on and vol or 0, 1)
    end
  end
end

-- the body the commands go to: sonicnative_sfx is loaded into it (again every few seconds, in
-- case its Lua was reloaded), and a new body starts with no loop playing (its vehicle Lua may
-- outlive an earlier instance of this extension: its loops are stopped too)
local function useBody(unicycle)
  local id = unicycle:getId()
  local adopted = id ~= bodyId
  if adopted then
    if bodyId then
      local old = be:getObjectByID(bodyId)
      if old then pcall(old.queueLuaCommand, old, SFX_STOP) end
    end
    bodyId, loops, sfxCheckAt = id, {}, -100
  end
  if clock - sfxCheckAt >= SFX_CHECK_SECONDS then
    sfxCheckAt = clock
    push(SFX_LOAD)
  end
  if adopted then push(SFX_STOP) end
end

-- everything quiet: loops stopped in the body (one-shots already playing end by themselves)
local function silence(target)
  out, pending = {}, {}
  if target then
    push(SFX_STOP)
    flush(target)
  end
  loops = {}
end

-- off foot: the next time starts from scratch
local function forget()
  speedKmh, air, groundLost, airTime = 0, false, 0, 0
  lastJumps, lastHit, wanted = nil, nil, false
  boostCand, boostCandSince, inBoost, boostLoop = false, 0, false, 0
  boostAt, boostEndAt = 0, 0
  wind, stepClip, stepPhase, stepsHeldUntil = 0, nil, 0, 0
end

-- sound comes back: counters and edges as they are now, so nothing old plays (the clip's phase
-- moved on during a pause: the footsteps start again from its next frame)
local function resync(st, body)
  lastJumps = tonumber(st.jumps) or 0
  lastHit = body.getLastHit and body.getLastHit() or nil
  wanted = st.wantsMove == true
  stepClip = nil
end

local function gamePaused()
  if simTimeAuthority and simTimeAuthority.getPause then
    local ok, p = pcall(simTimeAuthority.getPause)
    if ok then return p == true end
  end
  return false
end

local function landId(g)
  local id = g == 'conc' and 'land' or 'land_' .. g
  return sounds[id] and id or 'land'
end

local function stepId(gait, g)
  local id = 'step_' .. gait .. '_' .. g
  return sounds[id] and id or 'step_' .. gait .. '_conc'
end

-- the overlay's sound rules (beamng/src/game.cpp:209-305, 485-519), one frame
local function rules(dt, st, body)
  -- speed: horizontal, plus the climb on a wall, smoothed (game.cpp:209-213)
  local vel = nil
  if body.getBody then
    local _, v = body.getBody()
    vel = v
  end
  local kmh = 0
  if vel then
    local vz = st.onWall and vel.z or 0
    kmh = math.sqrt(vel.x * vel.x + vel.y * vel.y + vz * vz) * 3.6
    if not (kmh < 1e5) then kmh = 0 end   -- NaN or a teleport
  end
  speedKmh = speedKmh + (kmh - speedKmh) * (1 - math.exp(-dt / R.speedSmoothing))

  -- in the air after airDelay off the ground; swimming or on a wall is ground (game.cpp:230-236)
  local water = tonumber(st.water) or 0
  local grounded = st.grounded
  if type(grounded) ~= 'boolean' then grounded = not st.airborne end
  local wasAir = air
  if grounded or water == 2 or st.onWall then
    groundLost, air = 0, false
  else
    groundLost = groundLost + dt
    if groundLost > R.airDelay then air = true end
  end
  if air then airTime = wasAir and airTime + dt or groundLost end
  local landed = wasAir and not air
  local landedAir = landed and airTime or 0
  if not air then airTime = 0 end

  -- the ground under him; on the water it is water (overlay runner.lua:520)
  local g = water ~= 0 and 'watr' or st.surface
  surface = GROUND[g] and g or 'conc'

  -- jump: the runner counted a jump, from the ground or a wall (game.cpp:237)
  local jumps = tonumber(st.jumps) or 0
  if lastJumps and jumps > lastJumps then playId('jump', 1) end
  lastJumps = jumps

  -- landing after landMinAir in the air, by ground; no footsteps for landHold after it, as the
  -- overlay's land clip (game.cpp:277-279, 350)
  if landed and landedAir >= R.landMinAir then
    playId(landId(surface), 1)
    stepsHeldUntil = clock + R.landHold
  end

  -- the boost state: boost held on the ground from boostMinKmh (states.json). A change to or from
  -- the air is at once, any other must be wanted for stateHold (game.cpp:259-265)
  local cand = st.boosting == true and not air and speedKmh >= R.boostMinKmh
  if cand ~= boostCand then boostCand, boostCandSince = cand, clock end
  if cand ~= inBoost and (air or wasAir or clock - boostCandSince >= R.stateHold) then
    inBoost = cand
    if inBoost then
      boostAt = clock
      playId('boost', 1)
      pickLoop('boost')
      pickLoop('boost_loop')
      -- a sonic boom on a fast boost (game.cpp:302, Lab setting)
      if settings.boostBoom and settings.boostBoomKmh > 0 and speedKmh >= settings.boostBoomKmh then playId('sonic_boom', 1, true) end
    else
      boostEndAt = clock
    end
  end
  -- Frontiers' boost cue loops its looped layers (boost_loop) while the boost lasts, each from its
  -- place in the cue; they fade out when it ends (the overlay only had a 2.5 s one-shot)
  boostLoop = inBoost and 1 or math.max(0, boostLoop - dt / math.max(0.01, R.boostLoopFadeOut))
  local ran = (inBoost and clock or boostEndAt) - boostAt
  loopLayers('boost', boostLoop, ran)
  loopLayers('boost_loop', boostLoop, ran)

  -- brake: the stick released on the ground faster than brakeAboveKmh (game.cpp:504-506)
  local wants = st.wantsMove == true
  if wanted and not wants and not air and speedKmh > R.brakeAboveKmh then playId('brake', 1) end
  wanted = wants

  -- a car hit: a boom faster than hitBoomKmh, else a landing thud (game.cpp:291-293)
  local hit = body.getLastHit and body.getLastHit() or nil
  if hit ~= lastHit then
    lastHit = hit
    if type(hit) == 'table' then
      if (tonumber(hit.kmh) or 0) > R.hitBoomKmh then playId('sonic_boom', 1, true) else playId('land', 1) end
    end
  end

  -- footsteps: the walk or run clip's phase crosses a toe-down phase, on the ground, not swimming
  -- nor on a wall nor just after a landing (game.cpp:487-503); the run clip plays walk steps when
  -- slow (sounds.lua)
  local main = loaded('sonicnative_main')
  local a = main and main.getAnim and main.getAnim() or nil
  local clipName = a and a.clip or nil
  local steps = a and a.steps or nil
  local stepping = not air and water ~= 2 and not st.onWall and (clipName == 'walk' or clipName == 'run')
    and type(steps) == 'table' and #steps > 0 and clock >= stepsHeldUntil
  if stepping then
    local phase = tonumber(a.phase) or 0
    if stepClip == clipName then
      for _, at in ipairs(steps) do
        local crossed
        if phase >= stepPhase then crossed = at > stepPhase and at <= phase
        else crossed = at > stepPhase or at <= phase end
        if crossed then
          local gait = (clipName == 'walk' or speedKmh < R.walkStepsBelowKmh) and 'walk' or 'run'
          playId(stepId(gait, surface), R.stepPitchMin + R.stepPitchSpread * math.random())
          break   -- one step a frame
        end
      end
    end
    stepPhase = phase
  end
  stepClip = stepping and clipName or nil

  -- the speed wind: louder and higher with speed, at least windBoost while boosting (game.cpp:509-519)
  local want = math.max(0, math.min(1, (speedKmh - R.windStartKmh) / math.max(1, R.windFullKmh - R.windStartKmh)))
  if st.boosting and speedKmh > R.windBoostMinKmh then want = math.max(want, R.windBoost) end
  wind = wind + (want - wind) * math.min(1, dt * R.windRate)
  if windFile then setLoop('wind', windFile, volumeOf('wind') * wind * wind, R.windPitchBase + R.windPitchRange * wind) end
end

local function update(dt)
  clock = clock + dt
  local body = loaded('sonicnative_body')
  local st = body and body.getState and body.getState() or nil
  local unicycle = st and st.active and currentUnicycle() or nil

  -- why Sonic is silent now (nil: he is not)
  local reason = nil
  if not sfxOk then reason = 'vehicle extension sonicnative_sfx missing'
  elseif soundCount == 0 then reason = 'no sounds in the manifest (run the converter)'
  elseif not unicycle then reason = 'not on foot'
  elseif gamePaused() then reason = 'paused' end

  if reason then
    if not muted then silence(unicycle or (bodyId and be:getObjectByID(bodyId)) or nil) end
    muted, mutedReason = true, reason
    if reason ~= 'paused' then forget() end
    out = {}
    -- the later layers of a sound test still play
    if #pending > 0 then
      local u = currentUnicycle()
      if u then releaseDue() flush(u) else pending = {} end
    end
    return
  end

  useBody(unicycle)
  readSliders()
  if muted then
    muted, mutedReason = false, nil
    resync(st, body)
  end
  if dt > 0 then rules(dt, st, body) end
  releaseDue()
  flush(unicycle)
end

local function onPreRender(dtReal)
  local ok, err = pcall(update, dtReal or 0)
  if not ok then
    out = {}
    report('E', 'frame', 'Sonic sound update failed: ' .. tostring(err))
  end
end

local function stopHere()
  local u = currentUnicycle() or (bodyId and be:getObjectByID(bodyId)) or nil
  silence(u)
  forget()
  muted, mutedReason = true, 'stopped'
end

local function onExtensionLoaded()
  setExtensionUnloadMode(M, 'manual')
  readManifest()
end

local function onClientPostStartMission()
  readManifest()   -- the converter may have run since the last map
  bodyId, loops = nil, {}
  forget()
end

local function onClientEndMission()
  stopHere()
  bodyId = nil
end

local function onModDeactivated(mod)
  if type(mod) ~= 'table' or mod.modname ~= 'sonicnative' then return end
  stopHere()
  -- off the manual list, as sonicnative_main (modScript.lua loads it again with the mod)
  setExtensionUnloadMode('sonicnative_sound', 'auto')
  extensions.unload('sonicnative_sound')
end

-- Developer API, for the MCP run_lua tool: return jsonEncode(sonicnative_sound.status())
M.status = function()
  local loopInfo = {}
  for name, l in pairs(loops) do loopInfo[name] = {volume = l.vol, pitch = l.pitch, sentAt = l.sentAt} end
  local ids = {}
  for id, s in pairs(sounds) do ids[id] = {files = #s.files, mode = s.mode, volume = s.volume, seconds = s.seconds} end
  return {
    sounds = soundCount, ids = ids, missingFiles = missingFiles, sfxFile = sfxOk, windFile = windFile,
    muted = muted and mutedReason or false, events = events, loops = loopInfo, pending = #pending, clock = clock,
    speedKmh = speedKmh, air = air, surface = surface, inBoost = inBoost, wind = wind,
    sliderGain = sliderGain, settings = settings, body = bodyId, lastError = lastError,
  }
end

-- plays one sound id now (every layer, loop files once), even when the rules keep him silent
M.test = function(id)
  local s = sounds[id]
  if not s then return {error = 'no sound "' .. tostring(id) .. '" in the manifest (' .. soundCount .. ' sounds)'} end
  local u = currentUnicycle()
  if not u then return {error = 'Sonic is not on foot'} end
  useBody(u)
  readSliders()
  local n = playId(id, 1, id == 'sonic_boom', true)
  flush(u)
  return {id = id, files = n, volume = volumeOf(id), muted = muted and mutedReason or false}
end

-- reads the manifest again (after running the converter with the game open)
M.reload = function()
  local u = currentUnicycle()
  silence(u)
  warned = {}
  readManifest()
  return M.status()
end

-- developer tests: change settings for this session (same keys and types as sounds.lua settings)
M.setSettings = function(changes)
  for k, v in pairs(type(changes) == 'table' and changes or {}) do
    if settings[k] ~= nil and type(v) == type(settings[k]) then settings[k] = v end
  end
  sliderReadAt = -100
  return settings
end

M.onExtensionLoaded = onExtensionLoaded
M.onExtensionUnloaded = stopHere
M.onClientPostStartMission = onClientPostStartMission
M.onClientEndMission = onClientEndMission
M.onModDeactivated = onModDeactivated
M.onPreRender = onPreRender

return M
