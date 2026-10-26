-- SPDX-License-Identifier: MIT
-- Sonic Beam, native mode: the numbers Sonic's sounds are played with (GE extension
-- sonicnative_sound). A hand-maintained copy of the overlay's values (overlay v0.12.3:
-- beamng/sheets and beamng/src/game.cpp, audio.cpp); nothing here is read from the overlay at run
-- time, so a change made in the overlay does not reach the native mode: change it here too.
-- The sounds themselves come from the player's own Frontiers: the converter writes them into
-- mods/sonicnative_data/sounds and lists them in manifest.json ("sounds").
local M = {}

-- BeamNG's ground under Sonic, in the order of the overlay's kWalk/kRun/kLand tables
-- (beamng/src/game.cpp:136-139): concrete, dirt, grass, sand, snow, water, metal
M.grounds = {'conc', 'dirt', 'gras', 'sand', 'snow', 'watr', 'mtlp'}

-- Volume of each sound id, 0..1: beamng/sheets/sounds.json, volume column. They win over the
-- manifest's own "volume" so they can be tuned here without converting again.
M.volumes = {
  jump = 0.6, boost = 0.6, drift = 0.5, trick = 0.6, brake = 0.55, wind = 0.7, sonic_boom = 0.9,
  land = 0.6,                     -- sn_land_conc: landing on concrete (and the overlay's slow car hit)
  -- native mode only: the converter puts a cue's looping layers in <id>_loop, at the cue's volume
  boost_loop = 0.6, drift_loop = 0.5,
}
for _, g in ipairs(M.grounds) do
  M.volumes['step_walk_' .. g] = 0.35
  M.volumes['step_run_' .. g] = 0.45
  if g ~= 'conc' then M.volumes['land_' .. g] = 0.6 end
end

-- Master volume of every Frontiers sound: beamng/sheets/tuning.json soundVolume
M.master = 0.8

-- When each sound plays
M.rules = {
  speedSmoothing = 0.12,          -- s, speed every threshold reads (tuning.json speedSmoothing; game.cpp:209-212 adds the climb on a wall)
  airDelay = 0.12,                -- s off the ground before he counts as in the air (game.cpp:231; swimming or on a wall is ground)
  landMinAir = 0.35,              -- s in the air for a landing sound (beamng/sheets/events.json landing minAirSeconds)
  landHold = 0.45,                -- s, no footsteps after such a landing (events.json landing holdSeconds: the overlay plays its land clip, game.cpp:278, 350)
  boostMinKmh = 30,               -- km/h, boost held on the ground from this speed is boosting: the boost loop and the sonic boom (beamng/sheets/states.json boost)
  stateHold = 0.1,                -- s, a ground state must be wanted this long before it starts (game.cpp:261-262)
  brakeAboveKmh = 50,             -- km/h, stick released faster than this: brake sound (game.cpp:505)
  hitBoomKmh = 120,               -- km/h, a car hit faster than this booms, slower plays 'land' (game.cpp:293)
  stepPitchMin = 0.94,            -- footstep pitch: random in min .. min + spread (game.cpp:498)
  stepPitchSpread = 0.12,
  windStartKmh = 80,              -- km/h, the speed wind starts (tuning.json windStartKmh)
  windFullKmh = 600,              -- km/h, full wind (tuning.json windFullKmh)
  windBoost = 0.6,                -- least wind while boost is held (tuning.json windBoost)
  windBoostMinKmh = 30,           -- km/h, ... and he goes faster than this (game.cpp:515)
  windRate = 3,                   -- 1/s, how fast the wind follows (game.cpp:517)
  windPitchBase = 0.8,            -- pitch = base + range x wind (game.cpp:518)
  windPitchRange = 0.6,
  loopPitchMin = 0.25,            -- loop pitch limits (beamng/src/audio.cpp:103)
  loopPitchMax = 4,
  silentLevel = 0.001,            -- a loop this quiet is stopped (audio.cpp:73)
  muteDb = -40,                   -- a decibel setting this low mutes (audio.cpp:23)
  -- native mode only:
  walkStepsBelowKmh = 16,         -- km/h, the run clip plays walk footsteps below this (the overlay's walk animation ends at 16 km/h, states.json walk maxKmh); 0: by clip only
  boostLoopFadeOut = 0.25,        -- s, the boost's looping layers fade out over this when the boost ends
  driftLoopFadeOut = 0.25,        -- s, the drift's looping layers (drift_loop) fade out over this when the drift ends
  loopSendHz = 20,                -- loop volume and pitch are sent to the body at most this often
  loopRefreshSeconds = 1,         -- a playing loop is sent again this often (the body re-creates it after a re-init)
  sliderReadSeconds = 0.5,        -- BeamNG's volume sliders are read again this often
}

-- The settings sounds start with: the overlay's Lab defaults (beamng/mod/lua/ge/extensions/
-- sonicfrontiers/sonic.lua settings, Sound tab). Decibels: 0 = Frontiers' mix, -40 = muted.
M.settings = {
  soundDb = 0,                    -- every sound
  stepsDb = 0,                    -- footsteps (ids starting with step_; landings are not footsteps)
  windDb = 0,                     -- the speed wind
  boostBoom = true, boostBoomKmh = 200,   -- sonic boom on a boost started at least this fast
  -- native mode only: scale our sounds by BeamNG's volume sliders (sliderChannels multiplied
  -- together). Raw .wav files may not follow the sliders by themselves (FMOD mixes them through
  -- VCAs meant for its events): switch this off in game to compare (sonicnative_sound.setSettings)
  followSliders = true,
}
-- the sliders read when followSliders is on (Engine.Audio.getChannelVolume, live value); others:
-- AudioChannelOther, AudioChannelSurface, AudioChannelAero, AudioChannelEnvironment
M.sliderChannels = {'AudioChannelMaster'}

return M
