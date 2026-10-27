-- SPDX-License-Identifier: MIT
-- Sonic Beam, native mode: Sonic's shadow (Lab: Shadow tab). A StaticShape casts none, so one compute
-- PostEffect (shaders/sonicnative/shadow.cs.hlsl) darkens the lit scene before the translucent objects
-- are drawn, as the overlay drew its shadow before the trail and the aura (beamng/src game.cpp 446-481,
-- beamng/mod sonic.lua readGround and readSun, v0.12.3): in the sun his silhouette, from rounded cones
-- on his bones (the sharp motion blur's), along the sun's direction; with a low sun or at night a soft
-- round blob under him; both fading as he rises. sonicnative_fx hands his pose over every frame
-- (M.update); without it the pass is off. The numbers are in lua/common/sonicnative/effects.lua (shadow).

local M = {}
local logTag = 'sonicnative'

local E = require('sonicnative/effects')
local Sd = E.shadow
local S = E.sharp

local PASS_NAME = 'sonicnative_shadow'
local SHADER_NAME = 'sonicnative_shadowShader'
local SHADER_DIR = 'shaders/sonicnative/'   -- in this mod
local MAX_ERRORS = 8

local sqrt, max, min, cos, sin, rad, format = math.sqrt, math.max, math.min, math.cos, math.sin, math.rad, string.format

local dbg = {force = nil, shaderDir = SHADER_DIR}

local warned, errors = {}, {}
local function report(level, key, msg)
  if warned[key] then return end
  warned[key] = true
  errors[#errors + 1] = msg
  if #errors > MAX_ERRORS then table.remove(errors, 1) end
  log(level, logTag, msg)
end

local on = nil                    -- the pass enabled (nil: not known, after a Lua reload)
local sent = {}                   -- shader constant -> the text last sent
local updated = false             -- M.update ran since the last onPreRender
local clock, sunReadAt = 0, -100
local toSun = {x = -0.35, y = 0.45, z = 0.82}   -- the overlay's default without a sky (normalised below)
do
  local l = sqrt(toSun.x ^ 2 + toSun.y ^ 2 + toSun.z ^ 2)
  toSun.x, toSun.y, toSun.z = toSun.x / l, toSun.y / l, toSun.z / l
end
local last = {silhouette = 0, blob = 0, height = 0, cones = 0}

local SEG_A, SEG_B = {}, {}
for n = 0, S.maxSegments - 1 do SEG_A[n], SEG_B[n] = '$sonicSegA' .. n, '$sonicSegB' .. n end

local down = vec3(0, 0, -1)

local function deleteNamed(name)
  local o = scenetree.findObject(name)
  if o then pcall(o.delete, o) end
end

local function deletePass()
  deleteNamed(PASS_NAME)
  deleteNamed(SHADER_NAME)
  on, sent = false, {}
end

local function ensurePass()
  local p = scenetree.findObject(PASS_NAME)
  if p then return p end
  local path = dbg.shaderDir .. 'shadow.cs.hlsl'
  if not FS:fileExists('/' .. path) then
    report('E', 'shader', 'shadow shader not found: /' .. path)
    return nil
  end
  if not scenetree.findObject(SHADER_NAME) then
    local sh = createObject('ShaderData')
    if not sh then return nil end
    sh.DXComputShaderName = path
    sh.pixVersion = 5.0
    sh.canSave = false
    sh:registerObject(SHADER_NAME)
  end
  p = createObject('PostEffect')
  if not p then report('E', 'create', 'cannot create the shadow PostEffect') return nil end
  p.isEnabled = false
  p.allowReflectPass = false
  p.canSave = false
  -- before the translucent objects, as BeamNG's caustics and fog: the lit scene only
  p:setField('renderTime', 0, 'PFXBeforeBin')
  p:setField('renderBin', 0, Sd.bin)
  p.renderPriority = Sd.renderPriority
  p:setField('shader', 0, SHADER_NAME)
  p:setField('stateBlock', 0, 'PFX_DefaultStateBlock')
  p:setField('texture', 0, '$backBuffer')
  p:setField('texture', 1, '#prepass[Depth]')
  p:setField('target', 0, '$backBuffer')
  p:registerObject(PASS_NAME)
  on, sent = false, {}
  return scenetree.findObject(PASS_NAME)
end

local function turnOff()
  if on ~= false then
    local p = scenetree.findObject(PASS_NAME)
    if p then pcall(p.disable, p) end
  end
  on = false
  last.silhouette, last.blob = 0, 0
end

local function send(p, name, text)
  if sent[name] == text then return end
  p:setShaderConst(name, text)
  sent[name] = text
end

-- the sun from BeamNG's sky (azimuth and elevation in degrees), once a second (overlay readSun)
local function readSun()
  if clock - sunReadAt < Sd.sunEvery then return end
  sunReadAt = clock
  local sky = scenetree and scenetree.sunsky
  if not sky then return end
  local ok, az, el = pcall(function() return tonumber(sky.azimuth), tonumber(sky.elevation) end)
  if not ok or not az or not el then return end
  local a, e = rad(az), rad(el)
  toSun.x, toSun.y, toSun.z = sin(a) * cos(e), cos(a) * cos(e), sin(e)
end

-- the ground under him: the point below and its slope from three rays around (overlay readGround)
local function readGround(cx, cy, cz)
  local c = vec3(cx, cy, cz)
  local d = castRayStatic(c, down, Sd.rayRange)
  if not (d and d < Sd.rayRange) then return nil end
  local gx, gy, gz = cx, cy, cz - d
  local nx, ny, nz = 0, 0, 1
  local pts = {}
  for k = 0, 2 do
    local a = k * 2.094
    local top = vec3(cx + cos(a) * Sd.normalSpread, cy + sin(a) * Sd.normalSpread, cz)
    local h = castRayStatic(top, down, Sd.rayRange)
    if not (h and h < Sd.rayRange) then pts = nil break end
    pts[#pts + 1] = top + down * h
  end
  if pts then
    local n = (pts[2] - pts[1]):cross(pts[3] - pts[1])
    if n:length() > 1e-6 then
      n:normalize()
      if n.z < 0 then n = -n end
      nx, ny, nz = n.x, n.y, n.z
    end
  end
  if nz < Sd.minNormalZ then nx, ny, nz = 0, 0, 1 end
  return gx, gy, gz, nx, ny, nz
end

-- (x, y, z) turned by the quaternion, BeamNG's way (as sharp.lua)
local function rotate(qx, qy, qz, qw, x, y, z)
  local tx, ty, tz = 2 * (qy * z - qz * y), 2 * (qz * x - qx * z), 2 * (qx * y - qy * x)
  return x - qw * tx + (qy * tz - qz * ty), y - qw * ty + (qz * tx - qx * tz), z - qw * tz + (qx * ty - qy * tx)
end

local function cone(p, n, ax, ay, az, ra, bx, by, bz, rb)
  send(p, SEG_A[n], format('%.4f %.4f %.4f %.4f', ax, ay, az, ra))
  send(p, SEG_B[n], format('%.4f %.4f %.4f %.4f', bx, by, bz, rb))
end

-- Sonic as he is drawn: the ball shell, else cones on his bones, else one capsule (as sharp.lua, no margin)
local function sendCones(p, pose)
  local C = E.capsule
  local pl = pose.place
  local s = tonumber(pl.scale) or 1
  local n = 0
  if pose.ball then
    local r = pose.ballRadius
    cone(p, 0, pose.ballX, pose.ballY, pose.ballZ, r, pose.ballX, pose.ballY, pose.ballZ, r)
    n = 1
  elseif pose.bonesOk then
    local X, Y, Z = pose.boneX, pose.boneY, pose.boneZ
    for _, seg in ipairs(pose.segments) do
      if n >= S.maxSegments then break end
      local i, j, k = seg[1], seg[2], seg[5]
      local ax, ay, az = X[i], Y[i], Z[i]
      cone(p, n, ax, ay, az, seg[3] * s, ax + (X[j] - ax) * k, ay + (Y[j] - ay) * k, az + (Z[j] - az) * k, seg[4] * s)
      n = n + 1
    end
  else
    local ux, uy, uz = rotate(pl.qx, pl.qy, pl.qz, pl.qw, 0, 0, 1)
    local mx, my, mz = pl.x + ux * C.up * s, pl.y + uy * C.up * s, pl.z + uz * C.up * s
    local half, r = max(0, C.height * 0.5 - C.radius) * s, C.radius * s
    cone(p, 0, mx - ux * half, my - uy * half, mz - uz * half, r, mx + ux * half, my + uy * half, mz + uz * half, r)
    n = 1
  end
  for k = n, S.maxSegments - 1 do send(p, SEG_A[k], '0 0 0 -1') end
  last.cones = n
end

local function frame(pose)
  local opacity = pose and max(0, min(1, tonumber(pose.shadowOpacity) or 0)) or 0
  if dbg.force then opacity = dbg.force end
  if not (pose and opacity > 0) then turnOff() return end
  clock = clock + max(0, tonumber(pose.dt) or 0)
  readSun()
  local fx, fy, fz = pose.feetX, pose.feetY, pose.feetZ
  local gx, gy, gz, nx, ny, nz = readGround(fx, fy, fz + Sd.rayStart)
  if not gx then turnOff() return end
  local height = max(0, fz - gz)
  local fade = max(0, 1 - height / Sd.fadeMeters)
  local sunUp = max(0, min(1, (toSun.z - Sd.sunMin) / Sd.sunRamp))
  local body = extensions.sonicnative_body
  local st = body and type(body.getState) == 'function' and body.getState() or nil
  local onWall = type(st) == 'table' and st.onWall == true
  local silhouette = onWall and 0 or opacity * fade * sunUp
  local blob = opacity * fade * (1 - sunUp) * Sd.blobShare
  last.silhouette, last.blob, last.height = silhouette, blob, height
  if silhouette <= Sd.minShown and blob <= Sd.minShown then turnOff() return end
  local p = ensurePass()
  if not p then turnOff() return end
  -- around the ground under him: as far as the shadow of his top can fall (farther as the sun sinks
  -- and as he rises, up to reach); with no silhouette, just the blob
  local bound = silhouette > Sd.minShown and min(Sd.reach, (Sd.height + height) / max(toSun.z, 0.05)) + Sd.boundExtra
    or Sd.blobRadius + 0.6
  send(p, '$sonicSun', format('%.4f %.4f %.4f %.4f', toSun.x, toSun.y, toSun.z, silhouette))
  send(p, '$sonicGround', format('%.4f %.4f %.4f %.4f', gx, gy, gz, blob))
  send(p, '$sonicGroundN', format('%.4f %.4f %.4f %g', nx, ny, nz, Sd.blobRadius))
  send(p, '$sonicShadowBound', format('%.4f %.4f %.4f %.3f', gx, gy, gz, bound))
  -- what is his: within maskMargin of a cone and above the plane through his feet (not for the ball)
  send(p, '$sonicShadowFeet', pose.ball and '0 0 0 -1000' or format('%.4f %.4f %.4f %g', fx, fy, fz, S.groundClear))
  send(p, '$sonicShadowUp', format('%.4f %.4f %.4f %g', tonumber(pose.upX) or 0, tonumber(pose.upY) or 0, tonumber(pose.upZ) or 1, Sd.maskMargin))
  send(p, '$sonicShadowSoft', format('%g %g %g %g', Sd.coneScale, Sd.penumbra, Sd.penumbraGrowth, Sd.gamma))
  sendCones(p, pose)
  if not on then
    p:enable()
    on = true
  end
end

local function cleanup()
  turnOff()
  deletePass()
end

local function onPreRender()
  if not updated and on ~= false then
    local ok, err = pcall(turnOff)
    if not ok then report('E', 'off', 'turning the shadow off failed: ' .. tostring(err)) end
  end
  updated = false
end

local function onExtensionLoaded()
  setExtensionUnloadMode(M, 'manual')
end

local function onModDeactivated(mod)
  if type(mod) ~= 'table' or mod.modname ~= 'sonicnative' then return end
  cleanup()
  setExtensionUnloadMode('sonicnative_shadow', 'auto')
  extensions.unload('sonicnative_shadow')
end

-- ---------------------------------------------------------------- public API

-- every frame, from sonicnative_fx: the sharp motion blur's pose (ball, ballX/Y/Z, ballRadius,
-- bonesOk, boneX/Y/Z, segments, place, feetX/Y/Z) with shadowOpacity (the Lab's) and dt, or nil when
-- he is not on foot
M.update = function(pose)
  updated = true
  local ok, err = pcall(frame, pose)
  if not ok then
    report('E', 'frame', 'the shadow failed: ' .. tostring(err))
    pcall(turnOff)
  end
end

-- Developer API, for the MCP run_lua tool: return jsonEncode(sonicnative_shadow.status())
M.status = function()
  return {on = on == true, pass = scenetree.findObject(PASS_NAME) ~= nil, last = last,
    toSun = {toSun.x, toSun.y, toSun.z}, sent = sent, debug = dbg, errors = errors}
end

-- Developer switches: {force = 0..1 (an opacity whatever the Lab's; false: back to it),
-- shaderDir = 'shaders/x/' (made again from there: an edited shader needs a new path)}
M.debug = function(opts)
  if type(opts) ~= 'table' then return M.status() end
  if opts.force ~= nil then dbg.force = tonumber(opts.force) end
  if opts.shaderDir ~= nil then
    turnOff()
    deletePass()
    warned = {}
    dbg.shaderDir = type(opts.shaderDir) == 'string' and opts.shaderDir or SHADER_DIR
  end
  return M.status()
end

M.onExtensionLoaded = onExtensionLoaded
M.onExtensionUnloaded = cleanup
M.onClientEndMission = cleanup
M.onModDeactivated = onModDeactivated
M.onPreRender = onPreRender

return M
