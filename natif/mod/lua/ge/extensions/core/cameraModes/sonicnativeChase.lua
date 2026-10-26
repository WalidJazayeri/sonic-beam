-- SPDX-License-Identifier: MIT
-- Sonic Beam, native mode: third-person camera behind Sonic (sonicnative/config.lua, camera). A
-- global camera mode the sonicnative_body extension turns on while Sonic is on foot. The mouse /
-- right stick look around; while he runs it swings back behind him. It also tells Sonic's body
-- which way "forward" is, exactly like BeamNG's own walking camera does. Same as the overlay's
-- chase camera (beamng/mod/lua/ge/extensions/core/cameraModes, v0.12.3), without the pose it
-- handed to the overlay's renderer.

local config = require('sonicnative/config')
local K = config.camera
local P = config.physics

local C = {}
C.__index = C

function C:init()
  self.isGlobal = true
  self.hidden = true
  self.yaw = nil
  self.pitch = math.rad(K.pitchDegrees)
  self.lookTimer = 0
  self.distance = K.distance
  self.fov = K.fovDegrees
end

function C:reset()
  self.yaw = nil
end

function C:setCustomData(data)
  if data and data.yaw then self.yaw = data.yaw end
end

function C:update(data)
  local veh = getPlayerVehicle(0)
  local sonic = extensions.sonicnative_body
  if not veh or not sonic then return false end
  local dt = math.max(data.dtReal or 0.016, 1e-4)
  local feet, vel = sonic.getBody()
  if not feet then return false end
  local target = feet + vec3(0, 0, K.targetHeight)

  -- look input, same sources and scale as BeamNG's walking camera
  local rdx = MoveManager.yawRelative + 20 * dt * (MoveManager.yawRight - MoveManager.yawLeft)
  local rdy = MoveManager.pitchRelative + 20 * dt * (MoveManager.pitchUp - MoveManager.pitchDown)
  local speedH = math.sqrt(vel.x * vel.x + vel.y * vel.y)
  if not self.yaw then
    local dir = veh:getDirectionVector()
    self.yaw = math.atan2(dir.y, dir.x)
  end
  if math.abs(rdx) + math.abs(rdy) > 1e-4 then
    self.lookTimer = 0.8
    self.yaw = self.yaw - math.rad(rdx * K.mouseSpeed)
    self.pitch = clamp(self.pitch - math.rad(rdy * K.mouseSpeed), math.rad(-35), math.rad(70))
  else
    self.lookTimer = math.max(0, self.lookTimer - dt)
    if self.lookTimer == 0 and speedH > 4 then
      -- swing back behind Sonic's run
      local runYaw = math.atan2(vel.y, vel.x)
      local delta = (runYaw - self.yaw + math.pi) % (2 * math.pi) - math.pi
      self.yaw = self.yaw + delta * math.min(1, dt * K.followRate)
      self.pitch = self.pitch + (math.rad(K.pitchDegrees) - self.pitch) * math.min(1, dt)
    end
  end

  local boostAmount = clamp((speedH - P.runSpeed) / (P.boostSpeed - P.runSpeed), 0, 1)
  local wantDistance = K.distance + (K.boostDistance - K.distance) * boostAmount
  self.distance = self.distance + (wantDistance - self.distance) * math.min(1, dt * 3)
  local wantFov = K.fovDegrees + (K.boostFovDegrees - K.fovDegrees) * boostAmount
  self.fov = self.fov + (wantFov - self.fov) * math.min(1, dt * 3)

  local cp = math.cos(self.pitch)
  local fwd = vec3(math.cos(self.yaw) * cp, math.sin(self.yaw) * cp, -math.sin(self.pitch))
  local back = -fwd
  local dist = self.distance
  local hit = castRayStatic(target, back, dist)
  if hit and hit < dist then dist = math.max(0.4, hit - K.wallMargin) end
  local pos = target + back * dist

  local rot = quatFromDir(fwd, vec3(0, 0, 1))
  data.res.pos = pos
  data.res.rot = rot
  data.res.fov = self.fov
  data.res.targetPos = target

  -- Sonic's body moves relative to this camera (W = away from it).
  veh:queueLuaCommand("controller.getControllerSafe('playerController').setCameraControlData(" .. serialize({cameraRotation = rot}) .. ")")
  return true
end

return function(...)
  local o = ... or {}
  setmetatable(o, C)
  o:init()
  return o
end
