-- SPDX-License-Identifier: MIT
-- Sonic Beam, native mode: the numbers of Sonic's boost effects (GE extension sonicnative_fx: the
-- speed blur, the aura, the lightning, the Power Boost trail and the flash of a boost start) and
-- the Lab's defaults for them (sonicnative_menu, Boost and Aura tabs). A hand-maintained copy of
-- the overlay's values (overlay v0.12.3: beamng/src/game.cpp:266-313 and 520-686,
-- beamng/sheets/tuning.json, beamng/src/shared_draw.hpp:100-186, beamng/mod sonic.lua settings);
-- nothing here is read from the overlay at run time, so a change made in the overlay does not
-- reach the native mode: change it here too. Numbers marked "native only" have no overlay
-- counterpart (BeamNG draws the effects in its own scene, depth tested, with its own light).
-- The textures and the aura come from the player's own Frontiers: the converter writes them into
-- mods/sonicnative_data/sonic and lists them in manifest.json ("fx").
local M = {}

-- The settings the effects start with: the overlay's Lab defaults (beamng/mod/lua/ge/extensions/
-- sonicfrontiers/sonic.lua settings, Boost and Aura tabs)
M.settings = {
  trailLength = 1,                -- x the trail's lifetime (Lab 0.25..3)
  trailIntensity = 1,             -- x the trail's brightness (Lab 0..2)
  lightning = 1,                  -- x the bolts around Sonic and the trail's arcs (Lab 0..2)
  trailFade = 1,                  -- m over which the trail fades in behind Sonic; 0: no fade (Lab 0..4)
  fxBySpeed = true,               -- the trail, lightning and aura grow with Sonic's speed
  fxSpeedAmount = 0.75,           -- share that follows the speed: at a standstill they are at 1 - this
  blurStrength = 1,               -- x the speed blur (Lab 0..2); effects.blur.strength is the shader's own scale
  auraIntensity = 1,              -- x the aura's brightness (Lab 0..2)
  auraLightning = 1,              -- x the bolts on his body (Lab 0..3)
  boostLight = 1,                 -- native only: x the blue light of a boost start (Lab 0..2)
}

-- Shared by every effect (game.cpp:268-274, 288-289, 531; sonic.lua fxReferenceKmh)
-- fx_ratio = clamp(kmh / ref, 0, 1); fx_speed = 1 - (fxBySpeed and clamp(fxSpeedAmount, 0, 1) or 0) * (1 - fx_ratio)
M.common = {
  refKmhNoCap = 500,              -- km/h, full effects when the speed limit is off (sonic.lua fxReferenceKmh)
  refKmhMin = 50,                 -- km/h, the least reference with the limit on: max(this, top speed)
  chest = 0.6,                    -- m above his feet: the flash's origin and the blur's centre (game.cpp:274, 670)
  teleport = 60,                  -- m, a jump this long between two frames clears the trail (game.cpp:531)
  boostStates = {boost = true, drift_left = true, drift_right = true},   -- boosting (game.cpp:288-289)
  fastState = 'fast',             -- running fast without the key also lights the aura (game.cpp:640)
}

-- Speed blur: the frame streaks toward Sonic's chest (game.cpp:635-639, 669-676; shared_draw.hpp psPost)
-- want = min(1, fast * (key ? 1 : withoutBoost) + burst * burstShare), followed at `follow`/s;
-- the shader gets want * strength * blurStrength as its longest streak
M.blur = {
  startKmh = 150,                 -- km/h, the blur starts (tuning.json blurStartKmh)
  fullKmh = 600,                  -- km/h, at its strongest (tuning.json blurFullKmh)
  withoutBoost = 0.5,             -- share kept at speed without the boost key (tuning.json blurWithoutBoost)
  burstShare = 0.6,               -- x the flash of a boost start added (game.cpp:637)
  follow = 6,                     -- 1/s, how fast the blur follows (game.cpp:638)
  strength = 0.22,                -- longest streak, share of the distance to the centre (tuning.json blurStrength)
  sharp = 0.08,                   -- UV radius around the centre left sharp (psPost smoothstep)
  outer = 0.55,                   -- UV radius where the streaks reach full length (psPost smoothstep)
  taps = 16,                      -- samples per pixel (psPost; speedBlur.cs.hlsl unrolls this count)
  centreMin = 0.25,               -- the centre stays inside this part of the screen (game.cpp:673-674)
  centreMax = 0.75,
  minShown = 0.01,                -- below this blur x blurStrength the effect is off (render11.cpp:443)
  -- native only
  renderPriority = 0.05,          -- PostEffect order: higher runs first, so after the depth of field (0.1, dof.lua)
  motionStrength = 1,             -- 'motion' fallback: PostFxMotionBlur.strength added at full blur [not tested]
}

-- Aura on his body (game.cpp:640-644, 650): lit = boosting or (key and kmh > litKmh) or the fast state;
-- amount = fade * opacity * fx_speed * auraIntensity * (1 - ballAmount)
M.aura = {
  fadeIn = 0.12,                  -- s to light up (tuning.json auraFadeIn)
  fadeOut = 0.35,                 -- s to go out (tuning.json auraFadeOut)
  opacity = 0.9,                  -- tuning.json auraOpacity; the converter bakes it into the aura's base materials
  gamma = 2.2,                    -- the manifest's aura colour is in gamma space; materials take pow(c, gamma) (converter sonic_data.hpp kGamma)
  litKmh = 30,                    -- km/h, the boost key lights it from this speed (game.cpp:640)
  minShown = 0.01,                -- hidden below this amount (game.cpp: drawn above 0.01)
  -- native only: how the aura's brightness changes (a StaticShape has no colour of its own)
  methods = {'reload', 'skin', 'steps'},   -- tried in this order: material reload, skin ladder, mesh steps
  reloadMaxMs = 0.5,              -- ms, a material reload slower than this moves on to the skin ladder (plan_fx 5)
  steps = {0.4, 0.75},            -- 'steps': below the first only the halo shows, below the second only the inner layer
}

-- Bolts on his body, riding his bones (game.cpp:600-626): level = fade * fx_speed * auraLightning * (1 - ballAmount)
M.bodyBolts = {
  rate = 26,                      -- per second at level 1 (game.cpp:602)
  minLevel = 0.02,                -- none below this level (game.cpp:601)
  jitter = 0.16,                  -- m, the whole range of the random offset from the bone, per axis (game.cpp:606)
  life = {0.06, 0.08},            -- s, base + spread x random (game.cpp:606)
  size = {0.22, 0.28},            -- m, full width, base + spread x random (game.cpp:607)
}

-- Bolts crackling around a boosting Sonic and along his trail (game.cpp:589-597): x lightning x fx_speed
M.bolts = {
  rate = 14,                      -- per second (tuning.json boltsPerSecond)
  minKmh = 40,                    -- km/h, with the boost key held (game.cpp:523)
  trailShare = 0.4,               -- share born on a point of the trail, else at his back (game.cpp:594)
  spread = {0.8, 0.8, 0.9},       -- m, the whole range of the random offset per axis (game.cpp:595)
  life = {0.08, 0.07},            -- s, base + spread x random (game.cpp:595)
  size = {0.45, 0.5},             -- m, base + spread x random (game.cpp:595)
}

-- Power Boost trail (game.cpp:520-587): keep = seconds * max(minLength, trailLength * fx_speed);
-- fade = (1 - age)^fadePow * alpha * trail_on * head^2, head = min(1, along / trailFade);
-- half width = width * (ageWidth + (1 - ageWidth) * (1 - age)) * 0.5
M.trail = {
  seconds = 0.45,                 -- s a point lingers (tuning.json trailSeconds)
  width = 0.55,                   -- m next to Sonic (tuning.json trailWidth)
  minKmh = 40,                    -- km/h, with the boost key held (game.cpp:523)
  onRate = 10, offRate = 3,       -- 1/s, how fast it comes and goes (game.cpp:524)
  spacing = 0.25,                 -- m he moves before a new point (game.cpp:529)
  minLength = 0.1,                -- the least trailLength x fx_speed (game.cpp:521)
  chestUp = 0.55, chestBack = 0.35,   -- m, the trail starts this high and this far behind him (game.cpp:526)
  ageWidth = 0.15,                -- share of the width left at the end of its life (game.cpp:571)
  fadePow = 1.5,                  -- game.cpp:563
  -- native only: points kept at most (the spacing grows with the speed past this, then old points are halved)
  maxPoints = 160,
  levelStep = 1,                  -- native only: the trail's quads use every n-th brightness level (2: half the parts, so fewer draw calls)
  beam = {width = 1.3, alpha = 0.75},                 -- x width, x min(1, intensity) (game.cpp:585)
  locus = {vScale = 3, vSpeed = 2.5},                 -- v = along / vScale - now * vSpeed (game.cpp:586)
  arc = {width = 0.6, alpha = 0.9, maxLightning = 1.5, vScale = 2,   -- game.cpp:587
    reroll = 0.07,                -- s between two strikes: a new column and jitter (game.cpp:539-543)
    jitter = 0.3,                 -- m, the whole range of each point's shake per axis (game.cpp:542)
    jitterCount = 64},            -- shakes drawn per strike (game.cpp:541)
}

-- The flash of a boost start or a car hit (game.cpp:291-313, 677-683)
M.burst = {
  seconds = 0.45,                 -- s (tuning.json burstSeconds)
  meters = 2.6,                   -- m the shock ring grows by (tuning.json burstMeters)
  follow = 0.7,                   -- share of the way the glow follows Sonic from where it started (game.cpp:679)
  glow = {1.6, 1.2, spin = 2},    -- m size = base + grow x share; rad/s (game.cpp:681)
  ring = {0.6, spin = -1.5},      -- m size = base + meters x share; rad/s (game.cpp:682)
  bolts = 10,                     -- x lightning x fx_speed, on a boost start (game.cpp:304)
  delay = 0.03,                   -- s between two of them (game.cpp:306)
  life = 0.14,                    -- s (game.cpp:306)
  size = {0.7, 0.6},              -- m, base + spread x random (game.cpp:306)
  radius = {0.2, 0.6},            -- m from the origin, base + spread x random (game.cpp:305)
  z = {-0.3, 0.9},                -- m height = (random + first) x second (game.cpp:306)
  orbs = 9,                       -- glowing orbs drifting away, whatever the speed (game.cpp:308)
  orbXY = {1.2, 1.0},             -- m/s across, base + spread x random (game.cpp:310)
  orbUp = {0.4, 1.4},             -- m/s up, base + spread x random (game.cpp:310)
  orbLife = 0.9,                  -- s (game.cpp:310)
  orbSize = {0.35, 0.25},         -- m, base + spread x random (game.cpp:310)
  impact = {bolts = 14, delay = 0.02, life = 0.16, size = {0.8, 0.6}, radius = {0.2, 0.7}},   -- a car hit (game.cpp:293-296)
}

-- native only: sprites are depth tested in BeamNG; each is pulled this far toward the camera (and
-- scaled to keep its size on screen) so Sonic and the ground do not cut it. To tune in game.
M.sprites = {cameraPull = {bolt = 0.25, orb = 0.3, glow = 1.5, ring = 1.5},
  maxCount = 128}                 -- bolts and orbs alive at most (a few dozen at the most in the overlay's numbers)

-- native only: the blue light of a boost start, like Frontiers' plight01 (pow_boost_impact)
M.light = {colour = {0.25, 0.55, 1.0}, radius = 8, peak = 6000, castShadows = false}   -- peak in lumens [not tested]

-- native only: body bolts when the converter wrote no bone tracks: random points on a capsule that
-- follows Sonic (centre this high above his feet, radius, height), in metres
M.capsule = {up = 0.55, radius = 0.3, height = 1.0}

-- native only: the converter's brightness ladder (sonic_data.hpp kFxAlphas: top x ratio^(k - count)),
-- used when the manifest has no "alphas"; a quad dimmer than dropShare x the first level is dropped
M.levels = {count = 24, top = 2, ratio = 1.16, dropShare = 0.7}

-- native only: corners one ProceduralMesh takes at most (its 16-bit index counter hangs past 65535,
-- lua/ge/extensions/editor/toolUtilities/meshPart.lua:371)
M.mesh = {maxCorners = 60000}

return M
