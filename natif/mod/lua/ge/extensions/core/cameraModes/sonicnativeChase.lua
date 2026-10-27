-- SPDX-License-Identifier: MIT
-- Sonic Beam, native mode: third-person camera behind Sonic (sonicnative/config.lua, camera). A
-- global camera mode the sonicnative_body extension turns on while Sonic is on foot. The mouse /
-- right stick look around; while he runs it swings back behind him. It also tells Sonic's body
-- which way "forward" is, exactly like BeamNG's own walking camera does. Same as the overlay's
-- chase camera (beamng/mod/lua/ge/extensions/core/cameraModes, v0.12.3), without the pose it
-- handed to the overlay's renderer.
-- Native only: it can also show one of BeamNG's own cameras of the walking body (setView: orbit,
-- drone...), which C cycles through on foot (sonicnative_body). On the walking body BeamNG only ever
-- draws its first-person walking camera unless a global camera is active (core/cameraUpdate.lua,
-- isUnicycle), so this camera draws them itself, and "forward" then follows whichever is shown.
-- In those, a direction held keeps its course (the orbit and the drone swing behind Sonic's run by
-- themselves: "forward" read from them while a key is held would turn him without end), turned only
-- by the player's own mouse look; releasing or turning it reads "forward" from the view again.

local config = require('sonicnative/config')
local K = config.camera
local P = config.physics

local UP = vec3(0, 0, 1)

-- the yaw (radians, counter-clockwise seen from above, measured in game) the player gave a BeamNG
-- view with the mouse or the right stick, on top of the run it follows; nil while orbit resets
local LOOK = {
  orbit = function(cam) return cam.cameraResetted == 0 and cam.camRot and math.rad(cam.camRot.x) or nil end,
  droneChase = function(cam) return cam.orbitYaw end,
}

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
  self.view = 'sonic'         -- 'sonic' or the name of one of BeamNG's cameras of the walking body
  self.sentHeading = nil      -- the level "forward" last sent to the body
  self.heldStick = nil        -- the direction held when its course was set (stick steps), nil when released
  self.heldLook = nil         -- the shown view's look yaw last frame (LOOK)
end

function C:setCustomData(data)
  if data and data.yaw then self.yaw = data.yaw end
end

-- BeamNG's camera of that name on the player's walking body (the vehicle cameras of a vehicle
-- id, core_camera.getCameraDataById, as gameplay/walk.lua reads its 'unicycle' camera)
local function bodyCamera(name)
  local veh = getPlayerVehicle(0)
  local cams = veh and core_camera.getCameraDataById(veh:getId())
  return cams and cams[name] or nil
end

-- BeamNG's center-camera key (resetCamera reaches the active global camera only): Sonic's view
-- swings back behind him, a BeamNG camera is reset as on a car
function C:reset()
  local cam = self.view ~= 'sonic' and bodyCamera(self.view)
  if cam then
    if cam.reset then pcall(cam.reset, cam) end
  else
    self.yaw = nil
  end
  self.heldLook = nil   -- the view recentred by itself (the drone's look yaw back to 0): not the player's look
end

-- shows Sonic's own view ('sonic') or one of BeamNG's cameras of the walking body (force: set it
-- up again even if it is already shown, for a new walking body); a direction held keeps its course.
-- Returns the view now shown
function C:setView(name, force)
  name = name or 'sonic'
  if name == self.view and not force then return self.view end
  local cam = name ~= 'sonic' and bodyCamera(name)
  if name ~= 'sonic' and not cam then return self.view end
  local old = self.view ~= 'sonic' and bodyCamera(self.view)
  if old then
    old.focused = false
    if old.onCameraChanged then pcall(old.onCameraChanged, old, false) end
  end
  if cam then
    -- placed afresh around Sonic: orbit snaps behind him, the drone takes off from him, the
    -- external camera picks a new spot
    cam.focused = true
    if cam.onCameraChanged then pcall(cam.onCameraChanged, cam, true) end
    if cam.reset then pcall(cam.reset, cam) end
  elseif self.view ~= 'sonic' and self.sentHeading then
    -- carry on the way the last view had him run (read from its up vector too: right after the
    -- view from above as well)
    self.yaw = math.atan2(self.sentHeading.y, self.sentHeading.x)
  end
  self.view = name
  self.heldLook = nil   -- the old view's look yaw means nothing in the new one
  return self.view
end

function C:getView()
  return self.view
end

-- the level direction "forward" means for a camera turned by rot: away from the camera, read from
-- its up vector as well so that it holds when it looks straight down (exact at any pitch)
local function heading(rot)
  local f, u = rot * vec3(0, 1, 0), rot * UP
  local h = vec3(f.x - u.x * f.z, f.y - u.y * f.z, 0)
  if h:squaredLength() < 1e-6 then return nil end
  h:normalize()
  return h
end

-- Sonic's body moves relative to the camera (W = away from it), level. hold (BeamNG's cameras): while
-- the same direction stays held (within one stick step of the one that set the course), the course
-- is kept, turned only by the view's look yaw (look, LOOK) the player changes; it is read from the
-- view again once the direction is released or turned. Sonic's own view is followed every frame
-- (it swings behind his run slowly, as in the overlay), but a course held into a BeamNG view stays
function C:sendControl(veh, rot, hold, look)
  local h = heading(rot) or self.sentHeading
  if not h then return end
  local body = extensions.sonicnative_body
  local state = body and body.getState and body.getState()
  local stick = state and state.wantsMove and state.stick or -1
  local same = false
  if stick >= 0 and self.heldStick then
    local d = (stick - self.heldStick) % K.stickSteps
    same = math.min(d, K.stickSteps - d) <= 1
  end
  if not same then self.heldStick = stick >= 0 and stick or nil end
  if hold and same and self.sentHeading then
    h = self.sentHeading
    if look and self.heldLook then
      local d = (look - self.heldLook + math.pi) % (2 * math.pi) - math.pi
      if math.abs(d) < math.pi / 2 then   -- larger in one frame: the view flipped by itself
        local c, s = math.cos(d), math.sin(d)
        h = vec3(h.x * c - h.y * s, h.x * s + h.y * c, 0)
      end
    end
  end
  self.heldLook = look
  self.sentHeading = h
  local q = quatFromDir(h, UP)
  veh:queueLuaCommand(string.format("controller.getControllerSafe('playerController').setCameraControlData({cameraRotation = quat(%.6f, %.6f, %.6f, %.6f)})",
    q.x, q.y, q.z, q.w))
end

local function invalid(x) return x ~= x or x == math.huge or x == -math.huge end

-- one of BeamNG's cameras of the walking body, in place of Sonic's view; false: it is gone, failed
-- or gave no usable pose (then Sonic's view takes over)
function C:updateBodyView(data)
  local cams = data.vid and core_camera.getCameraDataById(data.vid)
  local cam = cams and cams[self.view]
  if not cam then return false end
  local ok, res = pcall(cam.update, cam, data)
  data.dt = data.dtReal   -- some switch to the simulation's step (external.lua); BeamNG puts it back the same way
  local r = data.res
  if not ok or res == false or invalid(r.fov + r.pos:squaredLength() + r.rot:squaredNorm()) then
    log('W', 'sonicnative', "camera " .. tostring(self.view) .. " failed on foot, back to Sonic's view: " .. tostring(res))
    if ok and cam.init then pcall(cam.init, cam) end
    return false
  end
  local look = LOOK[self.view]
  self:sendControl(data.veh, r.rot, true, look and look(cam) or nil)
  return true
end

function C:update(data)
  local veh = getPlayerVehicle(0)
  local sonic = extensions.sonicnative_body
  if not veh or not sonic then return false end
  if self.view ~= 'sonic' then
    if self:updateBodyView(data) then return true end
    self:setView('sonic')   -- never false here: BeamNG would leave this camera altogether
  end
  local dt = math.max(data.dtReal or 0.016, 1e-4)
  local feet, vel = sonic.getBody()
  feet = feet or vec3(veh:getPosition())   -- his first frame on foot: never false, BeamNG would drop this camera
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

  local rot = quatFromDir(fwd, UP)
  data.res.pos = pos
  data.res.rot = rot
  data.res.fov = self.fov
  data.res.targetPos = target

  self:sendControl(veh, rot, false, nil)
  return true
end

return function(...)
  local o = ... or {}
  setmetatable(o, C)
  o:init()
  return o
end
