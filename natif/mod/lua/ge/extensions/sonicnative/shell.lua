-- SPDX-License-Identifier: MIT
-- Sonic Beam, native mode: the boost aura's shell, the overlay's default aura shape (Lab: Aura follows
-- body shape off). A bullet lying along Sonic's run, a round shield just ahead of his face streaming
-- back past his feet, in three nested layers brighter toward their silhouette (beamng/src game.cpp
-- 650-668, assets.cpp dome, shared_draw.hpp psAura, v0.12.3). BeamNG's materials have no view
-- dependent term, so one compute PostEffect (shaders/sonicnative/shell.cs.hlsl) meets each pixel's eye
-- ray with the three bullets and adds the overlay's colour after the tonemapper. sonicnative_fx hands
-- Sonic's state over every frame (M.update); without it the pass is off. The numbers are in
-- lua/common/sonicnative/effects.lua (shell); the Lab's Aura tab sends the settings through
-- sonicnative_fx (auraThickness, auraLength, auraBodyShape).

local M = {}
local logTag = 'sonicnative'

local E = require('sonicnative/effects')
local Sh = E.shell

local PASS_NAME = 'sonicnative_shell'
local SHADER_NAME = 'sonicnative_shellShader'
local STATE_NAME = 'sonicnative_shellState'
local SHADER_DIR = 'shaders/sonicnative/'   -- in this mod
local MAX_ERRORS = 8

local sqrt, max, min, cos, sin, format = math.sqrt, math.max, math.min, math.cos, math.sin, string.format

-- developer switches (M.debug): the bin and render time it runs in, how it adds (0: onto the image,
-- 1: the image is linear), a forced brightness, the shader's folder (an edited shader needs a new path)
local dbg = {bin = Sh.bin, time = Sh.renderTime, mode = Sh.addMode, force = nil, shaderDir = SHADER_DIR}

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
local texture = nil               -- the aura pattern the pass was made with
local clock = 0                   -- the pattern's clock (game.cpp aura_clock)
local updated = false             -- M.update ran since the last onPreRender
local last = {amount = 0, R = 0, N = 0, tail = 0}

local function deleteNamed(name)
  local o = scenetree.findObject(name)
  if o then pcall(o.delete, o) end
end

local function deletePass()
  deleteNamed(PASS_NAME)
  deleteNamed(SHADER_NAME)
  on, sent, texture = false, {}, nil
end

-- wrap the pattern (slot 2), clamp the frame and the depth (BeamNG's caustics state block, the same way)
local function ensureState()
  if scenetree.findObject(STATE_NAME) then return STATE_NAME end
  local parent = scenetree.findObject('PFX_DefaultStateBlock')
  local sb = createObject('GFXStateBlockData')
  if not (sb and parent) then return nil end
  sb:inheritParentFields(parent)
  sb.samplersDefined = true
  sb:setField('samplerStates', 0, 'SamplerClampLinear')
  sb:setField('samplerStates', 1, 'SamplerClampPoint')
  sb:setField('samplerStates', 2, 'SamplerWrapLinear')
  sb.canSave = false
  sb:registerObject(STATE_NAME)
  return scenetree.findObject(STATE_NAME) and STATE_NAME or nil
end

local function ensurePass(tex)
  local p = scenetree.findObject(PASS_NAME)
  if p and texture == tex then return p end
  if p then deletePass() end
  local path = dbg.shaderDir .. 'shell.cs.hlsl'
  if not FS:fileExists('/' .. path) then
    report('E', 'shader', 'aura shell shader not found: /' .. path)
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
  local state = ensureState()
  if not state then report('E', 'state', 'cannot make the aura shell state block') return nil end
  p = createObject('PostEffect')
  if not p then report('E', 'create', 'cannot create the aura shell PostEffect') return nil end
  p.isEnabled = false
  p.allowReflectPass = false
  p.canSave = false
  p:setField('renderTime', 0, dbg.time)
  p:setField('renderBin', 0, dbg.bin)
  p.renderPriority = Sh.renderPriority
  p:setField('shader', 0, SHADER_NAME)
  p:setField('stateBlock', 0, state)
  p:setField('texture', 0, '$backBuffer')
  p:setField('texture', 1, '#prepass[Depth]')
  p:setField('texture', 2, tex)
  p:setField('target', 0, '$backBuffer')
  p:registerObject(PASS_NAME)
  on, sent, texture = false, {}, tex
  return scenetree.findObject(PASS_NAME)
end

local function turnOff()
  if on ~= false then
    local p = scenetree.findObject(PASS_NAME)
    if p then pcall(p.disable, p) end
  end
  on = false
  last.amount = 0
end

local function send(p, name, text)
  if sent[name] == text then return end
  p:setShaderConst(name, text)
  sent[name] = text
end

-- the bullet for this frame (game.cpp 650-663): along his velocity, else his heading; up made square
-- to it; its base just behind his chest so the nose's tip sits ahead of his face
local function frame(s)
  local amount = s and tonumber(s.amount) or 0
  if dbg.force then amount = dbg.force end
  -- the jump ball's glow (game.cpp 422-424)
  local glow = s and min(1, Sh.ballGlowShare * max(0, tonumber(s.ballGlow) or 0)) or 0
  if glow <= Sh.minShown then glow = 0 end
  -- the shell needs the aura pattern; the ball's glow does not (slot 2 then holds the frame, never read)
  if amount <= Sh.minShown or not (s and s.texture) then amount = 0 end
  if not (s and (amount > 0 or glow > 0)) then turnOff() return end
  local p = ensurePass(s.texture or '$backBuffer')
  if not p then turnOff() return end
  local dt = max(0, tonumber(s.dt) or 0)
  local ratio = max(0, min(1, tonumber(s.fxRatio) or 0))
  clock = (clock + dt * (Sh.clockBase + Sh.clockSpeed * ratio)) % 1000

  local H = Sh.height
  local size = Sh.sizeBase + Sh.sizeThickness * max(0, tonumber(s.thickness) or 1)
  local vx, vy, vz = tonumber(s.vx) or 0, tonumber(s.vy) or 0, tonumber(s.vz) or 0
  local speed = sqrt(vx * vx + vy * vy + vz * vz)
  local dx, dy, dz
  if speed > Sh.minSpeed then
    dx, dy, dz = vx / speed, vy / speed, vz / speed
  else
    local h = tonumber(s.heading) or 0
    dx, dy, dz = cos(h), sin(h), 0
  end
  -- world up made square to the run
  local ux, uy, uz = -dx * dz, -dy * dz, 1 - dz * dz
  local ul = sqrt(ux * ux + uy * uy + uz * uz)
  if ul > 1e-3 then ux, uy, uz = ux / ul, uy / ul, uz / ul else ux, uy, uz = 0, 1, 0 end
  local R = Sh.radius * H * size
  local N = Sh.nose * R
  local cx, cy, cz = s.feetX + ux * Sh.chest * H, s.feetY + uy * Sh.chest * H, s.feetZ + uz * Sh.chest * H
  local ahead = Sh.ahead * H * size - N
  local bx, by, bz = cx + dx * ahead, cy + dy * ahead, cz + dz * ahead
  local tail = max(0.05 * N, (Sh.tailBase + Sh.tailSpeed * ratio) * H * max(0, tonumber(s.length) or 1))

  -- a sphere holding all three layers (the outer one: x 1.27 across and along, its tail x 1.24 more)
  local outer = Sh.outerScale
  local front, back = N * outer, tail * outer * (1 + 0.24)
  local mid = (front - back) * 0.5
  local half = (front + back) * 0.5
  local wide = R * outer * (1 + 0.15)
  local radius = sqrt(half * half + wide * wide) + 0.1
  local mx, my, mz = bx + dx * mid, by + dy * mid, bz + dz * mid
  local gx, gy, gz = tonumber(s.ballX) or s.feetX, tonumber(s.ballY) or s.feetY, tonumber(s.ballZ) or s.feetZ
  if glow > 0 then
    -- the sphere holds the ball's glow too
    local ex, ey, ez = gx - mx, gy - my, gz - mz
    radius = max(radius, sqrt(ex * ex + ey * ey + ez * ez) + Sh.ballGlowSize * 0.5 + 0.1)
  end

  send(p, '$sonicShellBase', format('%.4f %.4f %.4f %.4f', bx, by, bz, amount))
  send(p, '$sonicShellDir', format('%.5f %.5f %.5f %.4f', dx, dy, dz, N))
  send(p, '$sonicShellUp', format('%.5f %.5f %.5f %.4f', ux, uy, uz, R))
  send(p, '$sonicShellTail', format('%.4f %.4f %g %d', tail, clock, Sh.margin, dbg.mode))
  send(p, '$sonicShellBound', format('%.4f %.4f %.4f %.4f', mx, my, mz, radius))
  send(p, '$sonicShellGlow', format('%.4f %.4f %.4f %.4f', gx, gy, gz, glow))
  send(p, '$sonicShellGlowTint', format('%g %g %g %g', Sh.ballGlowColour[1], Sh.ballGlowColour[2], Sh.ballGlowColour[3], Sh.ballGlowSize * 0.5))
  send(p, '$sonicShellTint', format('%g %g %g 0', Sh.colour[1], Sh.colour[2], Sh.colour[3]))
  -- his body: a capsule from a little above his feet along his up (the wall's normal on a wall run;
  -- world up while curled, his up then turns with the ball), his shoes and the ground left out
  local B = Sh.body
  local qx, qy, qz = 0, 0, 1
  if glow <= 0 then
    qx, qy, qz = tonumber(s.upX) or 0, tonumber(s.upY) or 0, tonumber(s.upZ) or 1
    local ql = sqrt(qx * qx + qy * qy + qz * qz)
    if ql > 1e-3 then qx, qy, qz = qx / ql, qy / ql, qz / ql else qx, qy, qz = 0, 0, 1 end
  end
  send(p, '$sonicShellBody', format('%.4f %.4f %.4f %g', s.feetX + qx * B.bottom * H, s.feetY + qy * B.bottom * H,
    s.feetZ + qz * B.bottom * H, B.radius * H))
  send(p, '$sonicShellBodyUp', format('%.4f %.4f %.4f %g', qx, qy, qz, B.length * H))
  if not on then
    p:enable()
    on = true
  end
  last.amount, last.R, last.N, last.tail = amount, R, N, tail
end

local function cleanup()
  turnOff()
  deletePass()
  deleteNamed(STATE_NAME)
end

-- sonicnative_fx stopped calling (unloaded, failing, not on foot): the pass goes off
local function onPreRender()
  if not updated and on ~= false then
    local ok, err = pcall(turnOff)
    if not ok then report('E', 'off', 'turning the aura shell off failed: ' .. tostring(err)) end
  end
  updated = false
end

local function onExtensionLoaded()
  setExtensionUnloadMode(M, 'manual')
end

local function onModDeactivated(mod)
  if type(mod) ~= 'table' or mod.modname ~= 'sonicnative' then return end
  cleanup()
  setExtensionUnloadMode('sonicnative_shell', 'auto')
  extensions.unload('sonicnative_shell')
end

-- ---------------------------------------------------------------- public API

-- every frame, from sonicnative_fx: {amount (the aura's brightness, 0 hides it), texture (the aura
-- pattern's file), feetX/Y/Z, vx/vy/vz (his velocity), heading (rad), fxRatio (0..1), thickness and
-- length (the Lab's), dt, ballGlow (the Lab's x how far he is curled into the ball), ballX/Y/Z (the
-- ball's middle)}, or nil when he is not on foot
M.update = function(s)
  updated = true
  local ok, err = pcall(frame, s)
  if not ok then
    report('E', 'frame', 'the aura shell failed: ' .. tostring(err))
    pcall(turnOff)
  end
end

-- Developer API, for the MCP run_lua tool: return jsonEncode(sonicnative_shell.status())
M.status = function()
  return {on = on == true, texture = texture, pass = scenetree.findObject(PASS_NAME) ~= nil, last = last,
    clock = clock, sent = sent, debug = dbg, errors = errors}
end

-- Developer switches; a key left out keeps its value: {bin = 'OverlayRender', time = 'PFXAfterBin',
-- mode = 0 | 1 (add onto the image / in gamma onto a linear image), force = 0..1 (a brightness drawn
-- whatever the boost; false: back to the boost's), shaderDir = 'shaders/x/' (made again from there)}
M.debug = function(opts)
  if type(opts) ~= 'table' then return M.status() end
  local remake = false
  if opts.bin ~= nil then dbg.bin = opts.bin or Sh.bin; remake = true end
  if opts.time ~= nil then dbg.time = opts.time or Sh.renderTime; remake = true end
  if opts.mode ~= nil then dbg.mode = tonumber(opts.mode) or Sh.addMode end
  if opts.force ~= nil then dbg.force = tonumber(opts.force) end
  if opts.shaderDir ~= nil then dbg.shaderDir = type(opts.shaderDir) == 'string' and opts.shaderDir or SHADER_DIR; remake = true end
  if remake then
    turnOff()
    deletePass()
    warned = {}
  end
  return M.status()
end

M.onExtensionLoaded = onExtensionLoaded
M.onExtensionUnloaded = cleanup
M.onClientEndMission = cleanup
M.onModDeactivated = onModDeactivated
M.onPreRender = onPreRender

return M
