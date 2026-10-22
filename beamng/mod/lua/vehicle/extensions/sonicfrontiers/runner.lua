-- SPDX-License-Identifier: MIT
-- Sonic Beam: runs inside BeamNG's walking-mode body (the "unicycle") and gives it Sonic's
-- running speed, boost and jump (beamng/sheets/physics.json). Reads BeamNG's own walking
-- inputs: W A S D move, Shift (sprint) is the boost, Space jumps. Holding boost keeps adding
-- speed with no ceiling unless the player turns on the limit in the Sonic Beam menu (O).

local M = {}

local sheets = require('sonicfrontiers/sheets')
local P = sheets.physics

local walkX, walkY = 0, 0
local boostHeld = 0
local camFwd = vec3(0, 1, 0)
local jumpQueued = false
local jumpTimer, jumpAccel = 0, 0
local jumpWait = 0
local jumpsToReport = 0
local grounded = false
local wrapped = false
local driveNodes = {}          -- {cid, share} where Sonic's forces are applied
local totalMass = 0
local massNodes = {}           -- {cid, weight}: every node, so Sonic's push moves the body like gravity does
local ballNodes = {}
local ballFriction = {}        -- cid -> {static, sliding} from the jbeam, restored after a glide
local gliding = false
local ballNodeSet = {}         -- cid -> true
local groundMaterial = nil     -- BeamNG ground material id under the ball (nodeCollision)
-- BeamNG ground material (lua/common/particles.json) -> Frontiers footstep surface:
-- 0 concrete, 1 dirt, 2 grass, 3 sand, 4 snow, 5 water, 6 metal plate
local SURFACE = {ASPHALT = 0, ASPHALT_WET = 0, ROCK = 0, COBBLESTONE = 0, RUMBLE_STRIP = 0, SLIPPERY = 0, ICE = 0,
  WOOD = 0, GLASS = 0, PLASTIC = 0, RUBBER = 0, DIRT = 1, DIRT_DUSTY = 1, GRAVEL = 1, MUD = 1, GRASS = 2, FOLIAGE = 2,
  FOLIAGE_THIN = 2, SAND = 3, SANDY_ROAD = 3, SNOW = 4, WATER = 5, METAL = 6}
local materialNames = nil
local loggedMaterial = nil
local groundGap = math.huge    -- metres from the ball's bottom down to the ground (updateGFX)
local snapLock = 0             -- seconds left without ground snap (after a jump)
local ballCenter, ballRadius = nil, 0
local up = vec3(0, 0, 1)
local autopilot, autoTime = false, 0
local walker = nil             -- BeamNG's playerController and its original functions
local original = nil           -- BeamNG's own playerController input functions
local enabled = true           -- false outside plain free roam: the body is BeamNG's own walker again
local walkerSpeedCoef = nil
-- set from the Sonic Beam menu (lua/ge/extensions/sonicfrontiers/sonic.lua)
local speedCapOn = P.speedCapOn == 1
local speedCapMs = P.speedCapKmh / 3.6
local boostRamp = P.boostRampKmhPerSecond / 3.6
local jumpSpeed = P.jumpSpeed     -- Sonic Beam Lab: Jump height (the height grows with the square of this speed)
-- water (Sonic Beam Lab, Water): fast enough he runs on it, slower and out of his depth he swims
local waterRunOn, waterRunMs, swimMs = true, 80 / 3.6, 8 / 3.6
local waterZ = nil                -- height of the water surface under Sonic (from the map, set by the mod's GE side)
local ballZ = 0                   -- height of the ball's centre
local waterState = 0              -- 0 none, 1 running on the water, 2 swimming
local lastWaterState = 0
local lastGrounded = true
-- wall run (Sonic Beam Lab, Wall): boosting fast into a steep wall, he runs on it - up when he
-- meets it head on, along it otherwise - until the boost stops, the wall ends or his grip runs out
local wallRunOn, wallRunMs, wallGrip = true, 120 / 3.6, 3
local wallState = 0               -- 0 none, 1 up, 2 along (wall on his left), 3 along (wall on his right)
local wallN = vec3(0, 0, 1)       -- the wall's normal (away from it)
local wallDir = vec3(0, 0, 1)     -- where he runs on it
local wallSpeed, wallTime, wallCooldown = 0, 0, 0
local wallKick = nil              -- velocity added once when he leaves the wall
local leaveWall, wallStep
local debugInfo = {}           -- last physics step, for the developer probe M.getDebug

local function wrapPlayerController()
  if wrapped then return true end
  local pc = controller.getController and controller.getController('playerController')
  if not pc then return false end
  -- keep BeamNG's own functions on the controller, so reloading this extension never wraps a wrapper
  local o = pc.sonicfrontiersOriginal
  if not o then
    o = {}
    for _, k in ipairs({'walkUpDownRaw', 'walkLeftRightRaw', 'walkUpDown', 'walkLeftRight', 'setSpeedCoef', 'toggleSpeedCoef', 'jump', 'setCameraControlData', 'nodeCollision'}) do
      o[k] = pc[k]
    end
    pc.sonicfrontiersOriginal = o
  end
  -- while gliding the walking controller gets no stick input, so it locks the ball (see setGlide)
  pc.walkUpDownRaw = function(v) walkY = v; if not gliding then return o.walkUpDownRaw(v) end end
  pc.walkLeftRightRaw = function(v) walkX = v; if not gliding then return o.walkLeftRightRaw(v) end end
  pc.walkUpDown = function(v) walkY = walkY + v; if not gliding then return o.walkUpDown(v) end end
  pc.walkLeftRight = function(v) walkX = walkX + v; if not gliding then return o.walkLeftRight(v) end end
  -- Shift is Sonic's boost; the walking controller's own speed is set by onPhysicsStep instead.
  pc.setSpeedCoef = function(v) boostHeld = v; if not enabled then return o.setSpeedCoef(v) end end
  pc.toggleSpeedCoef = function() boostHeld = 1 - boostHeld; if not enabled then return o.toggleSpeedCoef() end end
  -- Space is Sonic's own jump (BeamNG's walking jump would stack on top at rest and vanish at speed)
  pc.jump = function(v)
    if not enabled then return o.jump(v) end
    if v == nil or v > 0.5 then jumpQueued = true end
  end
  pc.setCameraControlData = function(data)
    if data and data.cameraRotation then camFwd = data.cameraRotation * vec3(0, 1, 0) end
    return o.setCameraControlData(data)
  end
  -- BeamNG's walking controller brakes anything faster than its sprint speed (11 m/s). Give it
  -- an unreachable sprint speed; onPhysicsStep then keeps its target at Sonic's own speed, so
  -- it balances and steers the body without ever holding him back.
  for _, c in pairs(v.data.controller or {}) do
    if c.fileName == 'playerController' then
      local data = deepcopy(c)
      data.movementSpeedSprint = P.walkerSprintMs
      pc.init(data)
      walker = {pc = pc, normal = data.movementSpeedNormal or 5, sonicData = data, beamngData = deepcopy(c)}
      break
    end
  end
  walkerSpeedCoef = o.setSpeedCoef
  original = o
  -- the ground the ball touches, for Frontiers' footstep sounds
  pc.nodeCollision = function(p)
    if ballNodeSet[p.id1] then groundMaterial = p.materialID1 end   -- materialID2 is the ball's own (cloth)
    if o.nodeCollision then return o.nodeCollision(p) end
  end
  controller.cacheAllControllerFunctions()
  wrapped = true
  return true
end

-- the walking controller aims for this speed (m/s), as a share between its normal and sprint speeds
local function setWalkerSpeed(ms)
  if not walker or not walkerSpeedCoef then return end
  walkerSpeedCoef(clamp((ms - walker.normal) / (P.walkerSprintMs - walker.normal), 0, 1))
end

local function findNodes()
  totalMass = 0
  local byName = {}
  for _, node in pairs(v.data.nodes) do
    totalMass = totalMass + (node.nodeWeight or 0)
    if (node.nodeWeight or 0) > 0 then massNodes[#massNodes + 1] = {node.cid, node.nodeWeight} end
    if node.name then byName[node.name] = node.cid end
    if node.tag == 'ball' then
      ballNodes[#ballNodes + 1] = node.cid
      ballNodeSet[node.cid] = true
      if node.frictionCoef then ballFriction[node.cid] = {node.frictionCoef, node.slidingFrictionCoef or node.frictionCoef} end
    end
  end
  -- Push the ball's centre and the body's top together, so Sonic's force never tips the body.
  local ball, top = byName['ball_42'], byName['r2']
  ballCenter = ball
  if ball then
    local centre = obj:getNodePosition(ball)
    for _, cid in ipairs(ballNodes) do ballRadius = math.max(ballRadius, (obj:getNodePosition(cid) - centre):length()) end
  end
  if ball and top then
    driveNodes = {{ball, 0.5}, {top, 0.5}}
  elseif ball then
    driveNodes = {{ball, 1}}
  end
end

local function inputDirection()
  local fwd = vec3(camFwd.x, camFwd.y, 0)
  if fwd:length() < 1e-4 then fwd = vec3(0, 1, 0) end
  fwd:normalize()
  local right = fwd:cross(up)
  local dir = right * walkX + fwd * walkY
  local len = dir:length()
  if len > 1 then dir = dir / len; len = 1 end
  return dir, len
end

-- At high speed the soft ball would have to spin hundreds of times a second: its rolling losses
-- cap Sonic's speed and, past ~550 km/h, BeamNG's physics cannot keep it together. While gliding
-- the walking controller sees no stick input, so it locks the ball like when standing still; the
-- locked ball slides on almost no friction and Sonic's own forces carry and steer him.
local function setGlide(on)
  if on == gliding then return end
  gliding = on
  if original then
    original.walkUpDownRaw(on and 0 or walkY)
    original.walkLeftRightRaw(on and 0 or walkX)
  end
  local k = on and P.glideFriction or 1
  for cid, f in pairs(ballFriction) do
    obj:setNodeFrictionSlidingCoefs(cid, f[1] * k, f[2] * k)
  end
end

-- Sonic ignores the air: set the body's triangles up again with almost no drag (same nodes,
-- pressure and collision as BeamNG's jbeam loader gave them).
local function setAirDrag(scale)
  local groups = v.data.pressureGroups or {}
  local count = 0
  for _, tr in pairs(v.data.triangles or {}) do
    if tr.cid then
      local drag = (tr.dragCoef or 100) * scale
      local lift = (tr.liftCoef or tr.dragCoef or 100) * scale
      local external = 1
      if tr.externalCollisionBias == 'out' then external = 2 elseif tr.externalCollisionBias == 'in' then external = 3
      elseif tr.externalCollisionBias == 'oneWayOut' or tr.externalCollisionBias == 'oneWayIn' then external = 4 end
      local group = (tr.pressureGroup and groups[tr.pressureGroup]) or -1
      obj:setTriangle(tr.cid, tr.id1, tr.id2, tr.id3, drag * 0.01, lift * 0.01, (tr.skinDragCoef or 0) * scale * 0.01,
        type(tr.stallAngle) == 'number' and tr.stallAngle or 0.58, group >= 0 and (tr.pressure or -1) or -1, group,
        external, tr.triangleType, tr.groundModel or 'asphalt')
      count = count + 1
    end
  end
  return count
end

local function applyAccel(a)
  for _, entry in ipairs(driveNodes) do
    obj:applyForceVector(entry[1], a * (totalMass * entry[2]))
  end
end

-- the same acceleration on every node (no stretching), for the big pushes at extreme speed
local function applyAccelEverywhere(a)
  for i = 1, #massNodes do
    local n = massNodes[i]
    obj:applyForceVector(n[1], a * n[2])
  end
end

-- Sonic's speed is a target that boost raises (with no ceiling unless the menu sets one) and a
-- controller that pushes the body to follow it. The controller measures his real speed, so it
-- cancels whatever slows the walking body (air, ground) without ever pushing past the target.
local speedTarget, learnedPush = 0, 0

local function onPhysicsStep(dt)
  if not enabled or #massNodes == 0 or totalMass <= 0 then return end
  local vel = obj:getVelocity()
  local vh = vec3(vel.x, vel.y, 0)
  local speed = vh:length()
  local dir, amount = inputDirection()
  if wallKick then applyAccelEverywhere(wallKick / dt); wallKick = nil end
  if wallState ~= 0 then
    -- running on the wall: gravity cancelled, held against it, carried along wallDir at wallSpeed
    wallSpeed = wallSpeed + boostRamp * dt
    if speedCapOn then wallSpeed = math.min(wallSpeed, speedCapMs) end
    if wallState ~= 1 and wallTime > 0.35 then
      -- along the wall: level out after the first climb onto it
      local level = wallDir - up * up:dot(wallDir)
      if level:length() > 0.1 then wallDir = (wallDir + (level:normalized() - wallDir) * math.min(1, 3 * dt)):normalized() end
    end
    if dir:length() > 0.1 and wallState ~= 1 then
      -- along the wall the stick steers up and down a little
      local lift = clamp(dir:dot(wallDir:cross(wallN)), -1, 1)
      wallDir = (wallDir + up * (lift * 0.6 * dt)):normalized()
    end
    local a = (wallDir * wallSpeed - vel) * P.wallGain - wallN * P.wallStick + up * 9.81
    applyAccelEverywhere(a)
    setGlide(true)
    setWalkerSpeed(speed)
    return
  end
  local swimming = waterState == 2
  local boosting = boostHeld > 0.5 and amount > 0.1 and not swimming
  local a
  if amount > 0.1 then
    if boosting then
      speedTarget = math.max(speedTarget, P.runSpeed * amount)
      speedTarget = speedTarget + (speedTarget < P.runSpeed and P.boostAccel or boostRamp) * dt
    else
      local cruise = P.runSpeed * amount
      if speedTarget > cruise then speedTarget = math.max(cruise, speedTarget - (P.coastAccel + (speedTarget - cruise) * P.coastRate) * dt)
      else speedTarget = math.min(cruise, speedTarget + P.runAccel * dt) end
    end
    if speedCapOn then speedTarget = math.min(speedTarget, speedCapMs) end
    if swimming then speedTarget = math.min(speedTarget, swimMs) end
    speedTarget = math.min(speedTarget, speed + P.targetLead)
    -- forward: follow the target speed along the input direction
    local along = vh:dot(dir)
    local err = speedTarget - along
    learnedPush = clamp(learnedPush + err * P.speedLearn * dt, 0, P.maxPush)
    local push = clamp(err * P.speedGain + learnedPush, -P.brakeAccel * 3, P.maxPush)
    -- sideways: turn his run toward the input
    local side = (dir * along - vh) * P.turnGain
    local len = side:length()
    if len > P.maxTurnAccel then side = side * (P.maxTurnAccel / len) end
    if not grounded and waterState == 0 then side = side * P.airControl end
    a = dir * push + side
  else
    speedTarget = speed
    learnedPush = learnedPush * math.max(0, 1 - 2 * dt)
    -- no stick: slow down, quicker the faster he goes (from 1000 km/h he stops in about 4 s)
    local brake = P.brakeAccel + speed * P.coastRate
    a = -vh * P.turnGain
    local len = a:length()
    if len > brake then a = a * (brake / len) end
    if not grounded and waterState == 0 then a = a * P.airControl end
  end
  if waterState ~= 0 then
    -- the water carries him: a spring holds the ball just touching it (running) or chest-deep (swimming)
    local target = swimming and (waterZ - P.swimDepth) or (waterZ + ballRadius - P.waterRunDepth)
    local spring = swimming and P.waterSpring * 0.4 or P.waterSpring   -- floating gently, no bobbing
    local lift = clamp(spring * (target - ballZ), -spring, spring) - vel.z * P.waterDamping * (swimming and 1.5 or 1)
    a = a + up * (9.81 + lift)
    -- landing on the water while running: stop the plunge at once (deep under it, BeamNG's water drag
    -- on the soft body killed his speed)
    if not swimming and vel.z < 0 then a = a - up * (vel.z * P.waterLandDamping) end
  elseif grounded then
    a = a - up * math.min(P.maxDownforce, P.downforce * speed * speed)
  else
    a = a - up * P.extraGravity
    -- Like Frontiers, a fast Sonic sticks to the ground instead of flying off every seam
    if snapLock <= 0 and groundGap < P.snapMeters and speed > P.snapKmh / 3.6 then a = a - up * P.snapAccel end
  end
  snapLock = math.max(0, snapLock - dt)
  applyAccelEverywhere(a)
  if jumpTimer > 0 then
    jumpTimer = jumpTimer - dt
    applyAccelEverywhere(up * jumpAccel)
  end
  setGlide(speed > P.glideKmh / 3.6 or (gliding and speed > P.glideKmh / 3.6 - 3))
  setWalkerSpeed(gliding and speed or speed + 1)
  debugInfo.speed, debugInfo.target, debugInfo.learned = speed, speedTarget, learnedPush
  debugInfo.ax, debugInfo.ay, debugInfo.az = a.x, a.y, a.z
  debugInfo.boosting, debugInfo.grounded, debugInfo.walker, debugInfo.gliding = boosting, grounded, walker ~= nil, gliding
end

-- a steep surface `reach` metres or less along dir from origin: its normal (towards origin) and distance,
-- from three rays a little apart
local function wallAt(origin, dir, reach)
  local d = obj:castRayStatic(origin, dir, reach)
  if not d or d >= reach then return nil end
  local side = dir:cross(up)
  if side:length() < 1e-3 then side = vec3(1, 0, 0) end
  side:normalize()
  local lift = side:cross(dir):normalized()
  local o1, o2 = origin + side * 0.3, origin + lift * 0.3
  local d1, d2 = obj:castRayStatic(o1, dir, reach + 1), obj:castRayStatic(o2, dir, reach + 1)
  if not d1 or not d2 or d1 >= reach + 1 or d2 >= reach + 1 then return nil end
  local p0 = origin + dir * d
  local n = (o1 + dir * d1 - p0):cross(o2 + dir * d2 - p0)
  if n:length() < 1e-6 then return nil end
  n:normalize()
  if n:dot(dir) > 0 then n = -n end
  return n, d
end

leaveWall = function(kick)
  wallState, wallCooldown, wallKick = 0, 0.6, kick
end

wallStep = function(dt, vel)
  wallCooldown = math.max(0, wallCooldown - dt)
  if not ballCenter then return end
  local c = obj:getPosition() + obj:getNodePosition(ballCenter)
  local _, amount = inputDirection()
  local boosting = boostHeld > 0.5 and amount > 0.1
  if wallState == 0 then
    if not (wallRunOn and boosting and wallCooldown <= 0 and waterState ~= 2) then return end
    local vh = vec3(vel.x, vel.y, 0)
    local sp = vh:length()
    if sp < wallRunMs then return end
    local dirH = vh / sp
    local reach = ballRadius + 0.6 + sp * 0.06
    local n = wallAt(c, dirH, reach)
    if not n or math.abs(n.z) > P.wallMaxSlope then return end
    -- a wall, not a kerb: it still stands in front of his chest
    local high = obj:castRayStatic(c + up * P.wallMinHeight, dirH, reach + 1)
    if not high or high >= reach + 1 then return end
    wallN, wallTime, wallSpeed = n, 0, vel:length()
    if -n:dot(dirH) > P.wallHeadOn then
      wallState, wallDir = 1, (up - n * n:dot(up)):normalized()   -- head on: straight up
    else
      local along = dirH - n * n:dot(dirH)
      along.z = 0
      -- he runs up onto the wall at a slant first, then levels out along it (physics step)
      wallDir = (along:normalized() + up * P.wallAlongLift):normalized()
      wallState = (-n):dot(wallDir:cross(up)) > 0 and 3 or 2         -- 3: the wall is on his right
    end
    log('D', 'sonicfrontiers', string.format('wall run %d at %.0f km/h', wallState, wallSpeed * 3.6))
    return
  end
  wallTime = wallTime + dt
  local n = wallAt(c, -wallN, ballRadius + 1.2)
  if n and math.abs(n.z) <= P.wallMaxSlope + 0.15 then
    wallN = (wallN * 0.8 + n * 0.2):normalized()
    wallDir = (wallDir - wallN * wallN:dot(wallDir)):normalized()
  end
  if not n and wallState == 1 then
    -- over the top: a little hop onto what is up there, his climbing speed turned forward
    leaveWall(-wallN * (P.wallTopPush + wallSpeed * 0.5) + up * (P.wallTopHop - vel.z))
  elseif not n or not wallRunOn or not boosting or wallTime > wallGrip then
    leaveWall(wallN * 3)                             -- falls away from the wall
  end
end

local function surfaceIndex()
  if not groundMaterial then return 0 end
  if not materialNames then
    materialNames = {}
    for id, m in pairs(particles.getMaterialsParticlesTable() or {}) do materialNames[id] = m.name end
  end
  local name = materialNames[groundMaterial]
  if loggedMaterial ~= groundMaterial then
    loggedMaterial = groundMaterial
    log('D', 'sonicfrontiers', 'ground material ' .. tostring(groundMaterial) .. ' = ' .. tostring(name))
  end
  return SURFACE[name or ''] or 0
end

local function updateGFX(dt)
  if not wrapped then wrapPlayerController() end
  if not enabled then return end
  if autopilot then
    -- Melty test runs with nobody at the keyboard: run, weave, boost and jump on a loop.
    autoTime = autoTime + dt
    walkY, walkX = 1, math.sin(autoTime * 0.5) * 0.35
    boostHeld = (autoTime % 12) > 5 and 1 or 0
    if autoTime % 7 < dt then jumpQueued = true end
  end
  if ballCenter then
    local c = obj:getPosition() + obj:getNodePosition(ballCenter)
    local d = obj:castRayStatic(c, vec3(0, 0, -1), ballRadius + P.snapMeters + 0.5)
    groundGap = (d and d < ballRadius + P.snapMeters + 0.5) and math.max(0, d - ballRadius) or math.huge
  end
  -- water: how much of the ball is in it; with no bottom under him he runs on it or swims
  -- (the surface comes from the map's water objects: BeamNG's own per-node test reads nothing at high speed)
  local vel = obj:getVelocity()
  local speedH = math.sqrt(vel.x * vel.x + vel.y * vel.y)
  waterState = 0
  if ballCenter and waterZ then
    local c = obj:getPosition() + obj:getNodePosition(ballCenter)
    ballZ = c.z
    if c.z - ballRadius < waterZ + 0.3 then
      -- the bottom under him, looked for from above (from the surface, a ray starting inside a
      -- beach saw nothing and he swam on dry land)
      local from = math.max(c.z, waterZ) + 1
      local d = obj:castRayStatic(vec3(c.x, c.y, from), vec3(0, 0, -1), from - (waterZ - 50))
      local depth = d and (waterZ - (from - d)) or 50   -- water depth under him
      -- fast enough he runs on the surface however shallow it is (wading in at speed, BeamNG's water
      -- drag braked him before the deep water); slower he swims once out of his depth
      local fast = waterRunOn and (speedH > waterRunMs or ((lastWaterState == 1 or not lastGrounded) and speedH > waterRunMs * 0.85))
      if fast and depth > 0.05 then waterState = 1
      elseif depth > P.waterFooting then waterState = 2 end
    end
  end
  lastWaterState = waterState
  -- a sliding ball skips over the ground at speed, so the ray counts as ground contact too
  grounded = groundGap < 0.15 or waterState == 1
  debugInfo.waterZ, debugInfo.ballZ, debugInfo.water = waterZ or -1, ballZ, waterState
  for _, cid in ipairs(ballNodes) do
    if grounded then break end
    if obj:isNodeColliding(cid) then grounded = true end
  end
  wallStep(dt, vel)
  if wallState ~= 0 then grounded = true end
  if jumpQueued and wallState ~= 0 then
    -- a jump off the wall: pushed away from it and up
    jumpQueued = false
    leaveWall(wallN * P.wallJump + up * (P.wallJump * 0.6))
    jumpsToReport = jumpsToReport + 1
  end
  if jumpQueued then jumpWait = 0.25; jumpQueued = false end
  if jumpWait > 0 then
    jumpWait = jumpWait - dt   -- a press waits a moment for the ground rather than being lost
    if grounded and jumpTimer <= 0 and waterState ~= 2 then
      jumpWait = 0
      -- Reach Sonic's jump speed over a short push (a single physics step would shake the soft body).
      local vz = obj:getVelocity().z
      jumpTimer = 0.05
      snapLock = 0.6
      jumpAccel = math.max(0, jumpSpeed - vz) / jumpTimer
      jumpsToReport = jumpsToReport + 1
    end
  end
  local _, amount = inputDirection()
  local boosting = (boostHeld > 0.5 and amount > 0.1) and 1 or 0
  -- direction held in the air, for the jump ball's tricks: 1 left, 2 right, 3 back
  local airInput = 0
  if walkX < -0.5 then airInput = 1 elseif walkX > 0.5 then airInput = 2 elseif walkY < -0.5 then airInput = 3 end
  if waterState == 2 then boosting = 0 end
  lastGrounded = grounded
  obj:queueGameEngineLua(string.format('if sonicfrontiers_sonic then sonicfrontiers_sonic.onRunner(%d, %d, %d, %d, %d, %d, %d, %d, %.3f, %.3f, %.3f) end',
    boosting, jumpsToReport, grounded and 1 or 0, amount > 0.1 and 1 or 0, waterState ~= 0 and SURFACE.WATER or surfaceIndex(), airInput, waterState,
    wallState, wallN.x, wallN.y, wallN.z))
  jumpsToReport = 0
end

local function onExtensionLoaded()
  findNodes()
  local triangles = setAirDrag(P.aeroScale)
  wrapPlayerController()
  enablePhysicsStepHook()
  log('I', 'sonicfrontiers', string.format('runner ready: %d drive nodes, %.1f kg, %d ball nodes, %d triangles without air drag', #driveNodes, totalMass, #ballNodes, triangles))
end

local function onReset()
  jumpTimer, jumpQueued, walkX, walkY = 0, false, 0, 0
  speedTarget, learnedPush = 0, 0
  gliding = true
  setGlide(false)
  -- a reset rebuilds the physics from the jbeam: give Sonic his low air drag back
  if enabled then setAirDrag(P.aeroScale) end
end

M.onExtensionLoaded = onExtensionLoaded
M.onReset = onReset
M.onPhysicsStep = onPhysicsStep
M.updateGFX = updateGFX
M.wrapPlayerController = wrapPlayerController
M.getDebug = function() return debugInfo end
-- Outside plain free roam (tutorials, career, scenarios, missions) the body goes back to being
-- BeamNG's own walker: its speeds, jump, air drag, ball grip and stick input are restored.
M.setEnabled = function(on)
  on = on and true or false
  if on == enabled then return end
  if not on then setGlide(false) end
  enabled = on
  jumpTimer, jumpWait, jumpQueued, speedTarget, learnedPush = 0, 0, false, 0, 0
  if walker then walker.pc.init(on and walker.sonicData or walker.beamngData) end
  setAirDrag(on and P.aeroScale or 1)
  if original then
    original.walkUpDownRaw(walkY)
    original.walkLeftRightRaw(walkX)
    original.setSpeedCoef(on and 0 or boostHeld)
  end
  log('I', 'sonicfrontiers', on and 'Sonic takes over the walking body' or 'walking body handed back to BeamNG')
end
M.setSpeedSettings = function(capOn, capKmh, rampKmhPerSecond, jumpHeight, runOnWater, waterRunKmh, swimKmh, runOnWalls, wallRunKmh, wallGripSeconds)
  if runOnWalls ~= nil then wallRunOn = runOnWalls and true or false end
  wallRunMs = math.max(0, wallRunKmh or 120) / 3.6
  wallGrip = math.max(0.2, wallGripSeconds or 3)
  if runOnWater ~= nil then waterRunOn = runOnWater and true or false end
  waterRunMs = math.max(0, waterRunKmh or 80) / 3.6
  swimMs = math.max(0.5, swimKmh or 8) / 3.6
  speedCapOn = capOn and true or false
  jumpSpeed = P.jumpSpeed * math.sqrt(math.max(0.01, jumpHeight or 1))   -- typed values may go past the lab's slider
  speedCapMs = math.max(10, capKmh or P.speedCapKmh) / 3.6
  boostRamp = math.max(1, rampKmhPerSecond or P.boostRampKmhPerSecond) / 3.6
  log('I', 'sonicfrontiers', string.format('speed: limit %s at %.0f km/h, boost +%.0f km/h per second, jump %.1f m/s',
    tostring(speedCapOn), speedCapMs * 3.6, boostRamp * 3.6, jumpSpeed))
end
-- the water surface under Sonic, from the map's water objects (nil: none)
M.setWater = function(z) waterZ = z end
M.setAutopilot = function(on)
  autopilot = on and true or false
  autoTime = 0
  if not autopilot then walkX, walkY, boostHeld = 0, 0, 0 end
  log('I', 'sonicfrontiers', 'autopilot ' .. tostring(autopilot))
end

return M
