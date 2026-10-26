-- SPDX-License-Identifier: MIT
-- Sonic Beam, native mode: the numbers Sonic's physics and camera read. A hand-maintained copy of
-- the overlay's values (overlay v0.12.3); nothing here is read from the overlay at run time, so a
-- change made in the overlay's sheets does not reach the native mode: change it here too.
local M = {}

-- How Sonic's body moves in BeamNG: copied from beamng/sheets/physics.json (every row but
-- unstuckMeters, the overlay's Unstuck button, and the three menu defaults moved to M.settings)
M.physics = {
  runSpeed = 24,              -- m/s, top running speed without boost
  boostSpeed = 52,            -- m/s, speed at which the camera is fully pulled back (boost look)
  runAccel = 16,              -- m/s2
  boostAccel = 34,            -- m/s2, boost pull until he reaches runSpeed
  brakeAccel = 20,            -- m/s2, slowing down with the keys released
  turnGain = 6,               -- 1/s
  airControl = 0.35,          -- share of ground control kept in the air
  jumpSpeed = 10,             -- m/s, upward speed of a jump (about 3 m high)
  extraGravity = 6,           -- m/s2
  waterRunDepth = -0.05,      -- m, the ball skims this far under the surface (negative: above it)
  swimDepth = 0.15,           -- m
  waterSpring = 80,           -- 1/s2
  waterDamping = 9,           -- 1/s
  waterLandDamping = 40,      -- 1/s
  wallGain = 8,               -- 1/s
  wallStick = 45,             -- m/s2
  wallJump = 9,               -- m/s
  wallTopPush = 6,            -- m/s
  wallMaxSlope = 0.45,        -- normal z: steepest that is still a wall
  wallHeadOn = 0.8,           -- cos: met this square-on he runs straight up
  wallTopHop = 5,             -- m/s
  wallMinHeight = 1.5,        -- m
  wallAlongLift = 0.45,       -- x
  wallSteer = 4,              -- 1/s
  wallLookAhead = 0.15,       -- s
  wallSpeedMemory = 150,      -- m/s2
  wallHoldReach = 2.0,        -- m
  waterFooting = 0.9,         -- m, shallower than this he has his footing and just runs
  groundProbe = 0.35,         -- m, gap under the body that still counts as on the ground
  coastAccel = 8,             -- m/s2
  maxTurnAccel = 70,          -- m/s2
  downforce = 0.003,          -- 1/m
  glideKmh = 110,             -- km/h, above this the ball slides and Sonic's own forces carry him
  glideFriction = 0.04,       -- share of the ball's friction kept while gliding
  speedGain = 4,              -- 1/s
  speedLearn = 8,             -- 1/s2
  maxPush = 600,              -- m/s2
  targetLead = 15,            -- m/s
  maxDownforce = 12,          -- m/s2
  coastRate = 0.4,            -- 1/s
  snapMeters = 0.8,           -- m
  snapKmh = 60,               -- km/h
  snapAccel = 60,             -- m/s2
  aeroScale = 0.02,           -- share of the walking body's air drag Sonic keeps
  walkerSprintMs = 100000,    -- m/s, sprint speed given to BeamNG's walking controller
}

-- The third-person camera behind Sonic: copied from beamng/sheets/camera.json
M.camera = {
  distance = 3.3,             -- m
  boostDistance = 4.6,        -- m
  targetHeight = 0.8,         -- m
  pitchDegrees = 10,
  fovDegrees = 70,
  boostFovDegrees = 82,
  followRate = 2.5,           -- 1/s
  mouseSpeed = 7,
  wallMargin = 0.3,           -- m
}

-- The settings the body starts with: the overlay's defaults, from the settings table of its GE
-- extension (beamng/mod/lua/ge/extensions, sonic.lua). The first three come from physics.json
-- (speedCapOn = 1, speedCapKmh, boostRampKmhPerSecond).
M.settings = {
  -- where Sonic takes over the walker: 'auto' = plain free roam only, starting on foot;
  -- 'always' = every game mode, never pulling the player out of a car; 'off' = never
  sonicMode = 'auto',
  speedCapOn = true, speedCapKmh = 600, boostRampKmhPerSecond = 60,
  jumpHeight = 1,                                       -- x, the height grows with the square of the jump speed
  runOnWater = true, waterRunKmh = 80, swimKmh = 8,     -- water: run on it from this speed, else swim
  runOnWalls = true, wallRunKmh = 120, wallGrip = 3,    -- walls: boosting into one from this speed, for this long (s)
  -- vehicles: 'mass' Sonic loses speed by the car's mass, 'plow' he keeps it, 'bounce' he bounces off,
  -- 'off' only BeamNG's own collision; impactForce: how heavy he hits; carDamage: how much they crush
  impactMode = 'mass', impactForce = 1, carDamage = 1,
}

-- Native mode only: the runner tells the game engine its state at once when it changes, and
-- refreshes it this many times a second otherwise (the overlay sent it every frame)
M.relay = {
  hz = 20,
}

return M
