-- SPDX-License-Identifier: MIT
-- Sonic Beam, native mode: the numbers Sonic's animation state machine runs on (GE extension
-- sonicnative_main). A hand-maintained copy of the overlay's values (overlay v0.12.3:
-- beamng/sheets/states.json, events.json, tuning.json, animations.json and the logic of
-- beamng/src/game.cpp); nothing here is read from the overlay at run time, so a change made in the
-- overlay does not reach the native mode: change it here too.
-- The clips themselves come from the player's own Frontiers: the converter writes them as named
-- sequences of Sonic's shape and lists them in manifest.json ("clips", one per animation id below).
local M = {}

-- What Sonic is doing: beamng/sheets/states.json, every row in the sheet's order. The highest
-- priority row whose conditions all hold wins (a tie keeps the earlier row). air and boost: 1 yes,
-- 0 no, -1 any; turn: 1 left, 2 right, -1 any (beamng/src/gen/sheets.hpp). Speeds in km/h, from
-- minKmh up to (not including) maxKmh. enterSound: the sound sonicnative_sound plays when the state
-- starts.
M.states = {
  {id = 'trick',       animation = 'trick_up',    minKmh = 0,   maxKmh = 9999, air = 1,  boost = -1, turn = -1, priority = 90, minAirSeconds = 0.8, enterSound = 'trick'},
  {id = 'airborne',    animation = 'fall',        minKmh = 0,   maxKmh = 9999, air = 1,  boost = -1, turn = -1, priority = 80, minAirSeconds = 0,   enterSound = nil},
  {id = 'drift_left',  animation = 'drift_left',  minKmh = 60,  maxKmh = 9999, air = 0,  boost = 1,  turn = 1,  priority = 70, minAirSeconds = 0,   enterSound = 'drift'},
  {id = 'drift_right', animation = 'drift_right', minKmh = 60,  maxKmh = 9999, air = 0,  boost = 1,  turn = 2,  priority = 70, minAirSeconds = 0,   enterSound = 'drift'},
  {id = 'boost',       animation = 'boost',       minKmh = 30,  maxKmh = 9999, air = 0,  boost = 1,  turn = -1, priority = 60, minAirSeconds = 0,   enterSound = 'boost'},
  {id = 'fast',        animation = 'boost',       minKmh = 150, maxKmh = 9999, air = 0,  boost = 0,  turn = -1, priority = 50, minAirSeconds = 0,   enterSound = nil},
  {id = 'run',         animation = 'run',         minKmh = 16,  maxKmh = 9999, air = 0,  boost = -1, turn = -1, priority = 40, minAirSeconds = 0,   enterSound = nil},
  {id = 'walk',        animation = 'walk',        minKmh = 2,   maxKmh = 16,   air = 0,  boost = -1, turn = -1, priority = 30, minAirSeconds = 0,   enterSound = nil},
  {id = 'idle',        animation = 'idle',        minKmh = 0,   maxKmh = 2,    air = -1, boost = -1, turn = -1, priority = 10, minAirSeconds = 0,   enterSound = nil},
}

-- The landing: beamng/sheets/events.json, row landing. After at least minAirSeconds in the air,
-- touching down plays the land clip for holdSeconds (game.cpp:277-279; only new air cancels it,
-- game.cpp:350). Its sound is sonicnative_sound's (sounds.lua landMinAir).
M.landing = {minAirSeconds = 0.35, holdSeconds = 0.45, animation = 'land'}

-- The overlay's animation ids (beamng/sheets/animations.json, in its order) and, for a shape
-- converted before a clip existed, the clips tried in its place, in order (native mode only: the
-- overlay always had them all). A clip with nothing to fall back on is skipped: the land hold then
-- keeps the state's own clip. The last resort is the idle clip.
M.ids = {'idle', 'walk', 'run', 'boost', 'boost_left', 'boost_right', 'drift_left', 'drift_right', 'fall',
  'trick_up', 'trick_left', 'trick_right', 'land', 'jump_start', 'ball', 'swim', 'wall_up', 'wall_left', 'wall_right'}
M.fallbacks = {
  idle = {},
  walk = {'run'},
  run = {'walk'},
  boost = {'run'},
  boost_left = {'boost', 'run'},
  boost_right = {'boost', 'run'},
  drift_left = {'boost', 'run'},
  drift_right = {'boost', 'run'},
  fall = {'air'},                   -- 'air': an early prototype's name for the fall clip
  trick_up = {'fall', 'air'},
  trick_left = {'trick_up', 'fall', 'air'},
  trick_right = {'trick_up', 'fall', 'air'},
  land = false,                     -- skipped when missing
  jump_start = {'ball', 'fall', 'air'},
  ball = {'fall', 'air'},
  swim = {'walk', 'run'},           -- the overlay's swim clip is Frontiers' walk loop
  wall_up = {'run', 'walk'},
  wall_left = {'run', 'walk'},
  wall_right = {'run', 'walk'},
}

-- The trick state cycles through these, starting at a random one (game.cpp:264, 346-349)
M.tricks = {'trick_up', 'trick_left', 'trick_right'}
-- A trick held in the air (jump style 2): runner.lua airInput 1 left, 2 right, 3 back (game.cpp:326).
-- Native mode: one trick per press, then the ball until the direction is released, as game.cpp:330's
-- comment says ("-2: wait for the release"); its code does the reverse and starts the trick again
-- while the direction is held, with a frame of ball in between that its cross-fade hid.
M.airTricks = {'trick_left', 'trick_right', 'trick_up'}
-- Wall run clips by the runner's wall kind: 1 up the wall, 2 wall on his left, else on his right (game.cpp:336)
M.wallClips = {'wall_up', 'wall_left', 'wall_right'}

-- Numbers and rules of beamng/src/game.cpp and beamng/sheets/tuning.json
M.tuning = {
  speedSmoothing = 0.12,        -- s, time constant smoothing his speed for the states and rates (tuning.json speedSmoothing)
  facingRate = 12,              -- 1/s, how fast he turns to face his run (tuning.json facingRate)
  swimTurnShare = 0.2,          -- x facingRate while swimming: waves made him spin (game.cpp:223)
  headingMinSpeed = 1.5,        -- m/s, slower than this he keeps his heading (game.cpp:216)
  turnRateDegrees = 70,         -- deg/s, yaw rate that counts as a hard turn: drift while boosting (tuning.json turnRateDegrees)
  turnRateFollow = 6,           -- 1/s, how fast the measured turn rate follows the heading (game.cpp:225)
  turnRateDecay = 0.9,          -- x per frame, turn rate kept each frame he is too slow to have a heading (game.cpp:227)
  maxDt = 0.1,                  -- s, longest frame step the state machine takes (game.cpp:180)
  airDelay = 0.12,              -- s off the ground before he counts as airborne; swimming or on a wall is ground (game.cpp:231-232)
  candidateDelay = 0.1,         -- s a new state must be wanted before it starts; to or from the air is at once (game.cpp:261-262)
  leanDegrees = 16,             -- deg, how far he leans into turns (tuning.json leanDegrees)
  leanFullTurn = 90,            -- deg/s, turn rate that gives the full lean (game.cpp:381)
  leanFollow = 8,               -- 1/s, how fast the lean follows (game.cpp:382); none while drifting or swimming
  rateFollow = 8,               -- 1/s, how fast a clip's playback rate follows its target (game.cpp:372)
  rateKnee = 0.7,               -- share of minRate..maxRate where the speed rate starts easing into its ceiling (game.cpp:368)
  animSpeed = 1,                -- run-cycle cadence multiplier (overlay Lab default, beamng/mod sonic.lua settings.animSpeed)
  animLimit = 1,                -- run-cycle ceiling multiplier (overlay Lab default, settings.animLimit)
  trickPause = 0.12,            -- s after a trick clip ends before the next trick of the trick state (game.cpp:348)
  airTrickPause = 0.05,         -- s after a held trick ends before he curls up again (game.cpp:327)
  -- jump ball (Frontiers: Sonic curls up into a spinning ball when he jumps)
  jumpStyle = 0,                -- 0 ball the whole jump, 1 ball going up then tricks, 2 ball with tricks on a direction (overlay Lab default, settings.jumpStyle)
  jumpStartMax = 0.22,          -- s, longest the jump start clip plays before the ball (game.cpp:342)
  apexAfter = 0.15,             -- s after the jump from which falling means past the top (game.cpp:242)
  jumpLostAfter = 0.4,          -- s after a jump that never left the ground before it is forgotten (game.cpp:241)
  ballSpinTurns = 3,            -- turns/s of the rolling ball (overlay Lab default, settings.ballSpin); the manifest's ball.spinSeconds wins
  ballIn = 14,                  -- 1/s, how fast the ball forms (game.cpp:404)
  ballOut = 10,                 -- 1/s, how fast it goes (game.cpp:404)
  ballFormed = 0.5,             -- ball amount from which only the shell shows (game.cpp:420); native mode: the shell, with no fade, shows from here too
  ballPivotShare = 0.42,        -- the ball turns about a point this share of his height above his feet (game.cpp:407)
  sonicHeightMeters = 1.0,      -- m, Sonic's standing height in BeamNG (tuning.json sonicHeightMeters)
  wallRunMinSpeed = 0.5,        -- m/s along the wall from which he faces his run on it, else up the wall (game.cpp:399)
}

return M
