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
  local boosting = boostHeld > 0.5 and amount > 0.1
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
    if not grounded then side = side * P.airControl end
    a = dir * push + side
  else
    speedTarget = speed
    learnedPush = learnedPush * math.max(0, 1 - 2 * dt)
    -- no stick: slow down, quicker the faster he goes (from 1000 km/h he stops in about 4 s)
    local brake = P.brakeAccel + speed * P.coastRate
    a = -vh * P.turnGain
    local len = a:length()
    if len > brake then a = a * (brake / len) end
    if not grounded then a = a * P.airControl end
  end
  if grounded then
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
  -- a sliding ball skips over the ground at speed, so the ray counts as ground contact too
  grounded = groundGap < 0.15
  for _, cid in ipairs(ballNodes) do
    if grounded then break end
    if obj:isNodeColliding(cid) then grounded = true end
  end
  if jumpQueued then jumpWait = 0.25; jumpQueued = false end
  if jumpWait > 0 then
    jumpWait = jumpWait - dt   -- a press waits a moment for the ground rather than being lost
    if grounded and jumpTimer <= 0 then
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
  obj:queueGameEngineLua(string.format('if sonicfrontiers_sonic then sonicfrontiers_sonic.onRunner(%d, %d, %d, %d, %d, %d) end',
    boosting, jumpsToReport, grounded and 1 or 0, amount > 0.1 and 1 or 0, surfaceIndex(), airInput))
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
M.setSpeedSettings = function(capOn, capKmh, rampKmhPerSecond, jumpHeight)
  speedCapOn = capOn and true or false
  jumpSpeed = P.jumpSpeed * math.sqrt(clamp(jumpHeight or 1, 0.25, 4))
  speedCapMs = math.max(10, capKmh or P.speedCapKmh) / 3.6
  boostRamp = math.max(1, rampKmhPerSecond or P.boostRampKmhPerSecond) / 3.6
  log('I', 'sonicfrontiers', string.format('speed: limit %s at %.0f km/h, boost +%.0f km/h per second, jump %.1f m/s',
    tostring(speedCapOn), speedCapMs * 3.6, boostRamp * 3.6, jumpSpeed))
end
M.setAutopilot = function(on)
  autopilot = on and true or false
  autoTime = 0
  if not autopilot then walkX, walkY, boostHeld = 0, 0, 0 end
  log('I', 'sonicfrontiers', 'autopilot ' .. tostring(autopilot))
end

return M
