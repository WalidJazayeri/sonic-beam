-- SPDX-License-Identifier: MIT
-- Sonic Beam, native mode: the motion blur that keeps Sonic sharp (native only: the overlay drew him
-- over BeamNG's finished frame, after its motion blur). BeamNG's motion blur (PostFxMotionBlur) spares
-- only vehicles, which write their own motion into its velocity buffer; every other pixel gets the
-- camera's motion, so Sonic, whom the camera follows, smeared like passing scenery. A PostEffect
-- cannot write that buffer (its '#VelocityBuffer' target lands in the frame itself), so while Sonic
-- is on foot and BeamNG's motion blur is on, five compute PostEffects blur the frame in its place,
-- with its strength (shaders/sonicnative):
--   motionBlurPrepare      BeamNG's velocity buffer -> half blur in pixels, distance, Sonic or not:
--                          no motion inside rounded cones around his bones (or the ball shell),
--                          except the ground under his feet
--   motionBlurTileMax x2   the longest half blur per tile, along x then along y
--   motionBlurNeighborMax  the longest around each tile
--   motionBlurGather       the blur itself (McGuire et al. 2012), Sonic's pixels left as they are
-- BeamNG's own is not turned off meanwhile: it is moved to a render bin that never draws, so it stays
-- enabled with its strength. Whatever reads or sets it (the options, photo mode, which saves its state
-- when a session starts and writes it back to the options when it ends, the World Editor) sees the
-- player's real state; this one runs only while it is enabled and reads its strength. sonicnative_fx
-- hands Sonic's pose over every frame (M.update); without it, off foot, in a car or with the Lab's
-- switch off, BeamNG's goes back to its bin. The numbers are in lua/common/sonicnative/effects.lua
-- (sharp); the Lab's Motion blur tab sends the settings.

local M = {}
local logTag = 'sonicnative'

local E = require('sonicnative/effects')
local S = E.sharp

-- the passes, in order: the first is enabled and disabled, the others are its children. A target's
-- size is its targetScale times the size of the pass's texture 0, not of the screen (seen in game:
-- 1/tile by 1/tile after the x pass gave a target one texel wide); tileX / tileY: this pass divides
-- its input by the tile on that axis
local PASSES = {
  {name = 'sonicnative_sharpBlur', shader = 'motionBlurPrepare', target = '#sonicnative_sharpVel',
    textures = {'#VelocityBuffer', '#prepass[Depth]'}},
  {name = 'sonicnative_sharpBlurTileX', shader = 'motionBlurTileMax', target = '#sonicnative_sharpTileX',
    textures = {'#sonicnative_sharpVel'}, tileX = true},
  {name = 'sonicnative_sharpBlurTile', shader = 'motionBlurTileMax', target = '#sonicnative_sharpTile',
    textures = {'#sonicnative_sharpTileX'}, tileY = true},
  {name = 'sonicnative_sharpBlurNeighbor', shader = 'motionBlurNeighborMax', target = '#sonicnative_sharpNeighbor',
    textures = {'#sonicnative_sharpTile'}},
  {name = 'sonicnative_sharpBlurGather', shader = 'motionBlurGather', target = '$backBuffer',
    textures = {'$backBuffer', '#sonicnative_sharpVel', '#sonicnative_sharpNeighbor'}},
}
local SHADER_DIR = 'shaders/sonicnative/'   -- in this mod
local BEAMNG_BLUR = 'PostFxMotionBlur'
local BEAMNG_BIN = 'OverlayRender'          -- its bin (renderTime PFXBeforeBin, priority 1)
local NO_BIN = 'sonicnativeNoBin'           -- a bin no render pass has: an effect there never draws
local MAX_ERRORS = 8

local max, format = math.max, string.format

-- this session's settings (M.setSettings: the player's saved ones and the Lab), from effects.lua
local settings = {}
for k, v in pairs(S.settings) do settings[k] = v end

-- developer switches (M.debug)
local dbg = {mask = false, view = 0, shaderDir = SHADER_DIR}
local VIEWS = {frame = 0, blur = 1, tiles = 2, distance = 3}

local warned, errors = {}, {}
local function report(level, key, msg)
  if warned[key] then return end
  warned[key] = true
  errors[#errors + 1] = msg
  if #errors > MAX_ERRORS then table.remove(errors, 1) end
  log(level, logTag, msg)
end

local on = nil                    -- the passes enabled (nil: not known, after a Lua reload)
local sent = {}                   -- shader constant -> the text last sent
local mask, cones = 'none', 0     -- what stands for Sonic ('bones', 'capsule', 'ball') and its cone count (status)
local updated = false             -- M.update ran since the last onPreRender

local SEG_A, SEG_B = {}, {}
for n = 0, S.maxSegments - 1 do SEG_A[n], SEG_B[n] = '$sonicSegA' .. n, '$sonicSegB' .. n end

local function deleteNamed(name)
  local o = scenetree.findObject(name)
  if o then pcall(o.delete, o) end
end

local function shaderName(file)
  return 'sonicnative_' .. file .. 'Shader'
end

local function ensureShader(file)
  local name = shaderName(file)
  if scenetree.findObject(name) then return name end
  local path = dbg.shaderDir .. file .. '.cs.hlsl'
  if not FS:fileExists('/' .. path) then
    report('E', 'shader:' .. file, 'motion blur shader not found: /' .. path .. ': BeamNG keeps its own motion blur')
    return nil
  end
  local sh = createObject('ShaderData')
  if not sh then return nil end
  sh.DXComputShaderName = path
  sh.pixVersion = 5.0
  sh.canSave = false
  sh:registerObject(name)
  return scenetree.findObject(name) and name or nil
end

local function deletePasses()
  for i = #PASSES, 1, -1 do deleteNamed(PASSES[i].name) end
  for _, pass in ipairs(PASSES) do deleteNamed(shaderName(pass.shader)) end
end

-- the five PostEffects (made once a map): the first where effects.lua puts it (before the translucent
-- objects, which stay sharp as in the overlay; BeamNG's own runs later, in OverlayRender)
local function ensurePasses()
  local first = scenetree.findObject(PASSES[1].name)
  if first then return first end
  local shaders = {}
  for i, pass in ipairs(PASSES) do
    shaders[i] = ensureShader(pass.shader)
    if not shaders[i] then return nil end
  end
  local parent = nil
  for i, pass in ipairs(PASSES) do
    local p = createObject('PostEffect')
    if not p then
      report('E', 'create', 'cannot create the motion blur PostEffect ' .. pass.name)
      deletePasses()
      return nil
    end
    p.allowReflectPass = false
    p.canSave = false
    if i == 1 then
      p.isEnabled = false
      p:setField('renderTime', 0, S.renderTime)
      p:setField('renderBin', 0, S.bin)
      p.renderPriority = S.renderPriority
    end
    p:setField('shader', 0, shaders[i])
    p:setField('stateBlock', 0, 'PFX_DefaultStateBlock')
    for k, texture in ipairs(pass.textures) do p:setField('texture', k - 1, texture) end
    p:setField('target', 0, pass.target)
    if pass.target ~= '$backBuffer' then
      p:setField('targetFormat', 0, 'GFXFormatR16G16B16A16F')
      p:setField('targetScale', 0, format('%g %g', pass.tileX and 1 / S.tile or 1, pass.tileY and 1 / S.tile or 1))
    end
    p:registerObject(pass.name)
    if parent then parent:add(p) else parent = p end
  end
  on, sent = false, {}
  return scenetree.findObject(PASSES[1].name) and parent or nil
end

-- BeamNG's motion blur into a bin (an effect takes its bin when it is enabled), left as enabled as it was
local function moveBeamng(mb, bin)
  if mb:getField('renderBin', 0) == bin then return end
  local enabled = mb:isEnabled()
  if enabled then mb:disable() end
  mb:setField('renderBin', 0, bin)
  if enabled then mb:enable() end
end

-- BeamNG's motion blur back in its own bin (whatever left it in the empty one: this session, an
-- earlier Lua session)
local function giveBack()
  local mb = scenetree.findObject(BEAMNG_BLUR)
  if mb then
    local ok, err = pcall(moveBeamng, mb, BEAMNG_BIN)
    if not ok then report('E', 'giveBack', "cannot give BeamNG's motion blur back: " .. tostring(err)) end
  end
end

local function turnOff()
  if on ~= false then
    local p = scenetree.findObject(PASSES[1].name)
    if p then pcall(p.disable, p) end
  end
  giveBack()
  on, mask, cones = false, 'none', 0
end

local function send(p, name, text)
  if sent[name] == text then return end
  p:setShaderConst(name, text)
  sent[name] = text
end

-- rounded cone n: its ends in the world and the radius at each
local function cone(p, n, ax, ay, az, ra, bx, by, bz, rb)
  send(p, SEG_A[n], format('%.4f %.4f %.4f %.4f', ax, ay, az, ra))
  send(p, SEG_B[n], format('%.4f %.4f %.4f %.4f', bx, by, bz, rb))
end

-- (x, y, z) turned by the quaternion, BeamNG's way (lua/common/mathlib.lua LuaQuat.__mul: q * v)
local function rotate(qx, qy, qz, qw, x, y, z)
  local tx, ty, tz = 2 * (qy * z - qz * y), 2 * (qz * x - qx * z), 2 * (qx * y - qy * x)
  return x - qw * tx + (qy * tz - qz * ty), y - qw * ty + (qz * tx - qx * tz), z - qw * tz + (qx * ty - qy * tx)
end

-- Sonic as he is drawn this frame: the ball shell, else rounded cones on his bones, else one capsule
-- around his StaticShape (effects.lua capsule); and the ground under his feet, kept out of them
local function sendSonic(p, pose)
  local m, C = S.margin, E.capsule
  local pl = pose.place
  local s = tonumber(pl.scale) or 1
  local ux, uy, uz = rotate(pl.qx, pl.qy, pl.qz, pl.qw, 0, 0, 1)
  local mx, my, mz = pl.x + ux * C.up * s, pl.y + uy * C.up * s, pl.z + uz * C.up * s
  local n = 0
  local ground = true
  if pose.ball then
    local r = pose.ballRadius + m
    mx, my, mz = pose.ballX, pose.ballY, pose.ballZ
    cone(p, 0, mx, my, mz, r, mx, my, mz, r)
    n, mask, ground = 1, 'ball', false
  elseif pose.bonesOk then
    local X, Y, Z = pose.boneX, pose.boneY, pose.boneZ
    for _, seg in ipairs(pose.segments) do
      if n >= S.maxSegments then break end
      local i, j, k = seg[1], seg[2], seg[5]
      local ax, ay, az = X[i], Y[i], Z[i]
      cone(p, n, ax, ay, az, seg[3] * s + m, ax + (X[j] - ax) * k, ay + (Y[j] - ay) * k, az + (Z[j] - az) * k, seg[4] * s + m)
      n = n + 1
    end
    mask = 'bones'
  else
    local half, r = max(0, C.height * 0.5 - C.radius) * s, C.radius * s + m
    cone(p, 0, mx - ux * half, my - uy * half, mz - uz * half, r, mx + ux * half, my + uy * half, mz + uz * half, r)
    n, mask = 1, 'capsule'
  end
  for k = n, S.maxSegments - 1 do send(p, SEG_A[k], '0 0 0 -1') end
  cones = n
  send(p, '$sonicBound', format('%.4f %.4f %.4f %g', mx, my, mz, S.bound))
  -- the plane through his feet, square to his up (the ground, or the wall he runs on): what lies
  -- less than groundClear above it is not his (the ball shell never touches it)
  local gx, gy, gz = tonumber(pose.upX) or 0, tonumber(pose.upY) or 0, tonumber(pose.upZ) or 1
  send(p, '$sonicGround', ground and format('%.4f %.4f %.4f %g', pose.feetX, pose.feetY, pose.feetZ, S.groundClear) or '0 0 0 -1000')
  send(p, '$sonicUp', format('%.4f %.4f %.4f 0', gx, gy, gz))
end

local function frame(pose)
  local mb = scenetree.findObject(BEAMNG_BLUR)
  -- BeamNG's turned off (the options, photo mode, the editor): no motion blur at all
  if not (pose and settings.sharpSonic and mb and mb:isEnabled()) then turnOff() return end
  local p = ensurePasses()
  if not p then turnOff() return end
  moveBeamng(mb, NO_BIN)
  if not on then
    p:enable()
    on = true
  end
  local strength = max(0, tonumber(mb.strength) or 0) * max(0, settings.motionBlurScale) * S.strength
  send(p, '$sonicMotion', format('%g %g %g 0', strength, S.maxRadius, S.tile))
  local gather = format('%g %d %d 0', S.softDepth, dbg.mask and 1 or 0, dbg.view)
  if sent.gather ~= gather then
    local g = scenetree.findObject(PASSES[#PASSES].name)
    if g then
      g:setShaderConst('$sonicGather', gather)
      sent.gather = gather
    end
  end
  sendSonic(p, pose)
end

local function cleanup()
  turnOff()
  deletePasses()
end

-- sonicnative_fx stopped calling (unloaded, failing) or never called since a Lua reload: BeamNG's
-- motion blur goes back to its bin
local function onPreRender()
  if not updated and on ~= false then
    local ok, err = pcall(turnOff)
    if not ok then report('E', 'off', 'turning the motion blur off failed: ' .. tostring(err)) end
  end
  updated = false
end

local function onExtensionLoaded()
  setExtensionUnloadMode(M, 'manual')
end

local function onClientEndMission()
  cleanup()
end

local function onModDeactivated(mod)
  if type(mod) ~= 'table' or mod.modname ~= 'sonicnative' then return end
  cleanup()
  -- off the manual list, as sonicnative_main (modScript.lua loads it again with the mod)
  setExtensionUnloadMode('sonicnative_sharp', 'auto')
  extensions.unload('sonicnative_sharp')
end

-- ---------------------------------------------------------------- public API

-- every frame, from sonicnative_fx: Sonic as he is drawn now (pose: ball, ballX/Y/Z, ballRadius,
-- bonesOk, boneX/Y/Z, segments = {{bone, bone, radius, radius, reach}}, place = his StaticShape's
-- {x, y, z, qx, qy, qz, qw, scale}, feetX/Y/Z and upX/Y/Z: his feet and his up), or nil when he is
-- not on foot
M.update = function(pose)
  updated = true
  local ok, err = pcall(frame, pose)
  if not ok then
    report('E', 'frame', 'the motion blur that keeps Sonic sharp failed: ' .. tostring(err))
    pcall(turnOff)
  end
end

-- changes settings live (same keys and types as effects.lua sharp.settings; others are ignored):
-- sonicnative_menu (the Lab's Motion blur tab), and developer tests
M.setSettings = function(values)
  for k, v in pairs(type(values) == 'table' and values or {}) do
    if settings[k] ~= nil and type(v) == type(settings[k]) and v == v then settings[k] = v end
  end
  return settings
end

-- for the Lab's Motion blur tab: BeamNG's motion blur on, and this one running now
M.getState = function()
  local mb = scenetree.findObject(BEAMNG_BLUR)
  return {beamngOn = mb ~= nil and mb:isEnabled(), on = on == true}
end

-- Developer API, for the MCP run_lua tool: return jsonEncode(sonicnative_sharp.status())
M.status = function()
  local mb = scenetree.findObject(BEAMNG_BLUR)
  return {
    on = on == true, mask = mask, cones = cones,
    beamng = mb and {enabled = mb:isEnabled(), bin = mb:getField('renderBin', 0), strength = tonumber(mb.strength)} or false,
    motion = sent['$sonicMotion'], bound = sent['$sonicBound'], ground = sent['$sonicGround'],
    passes = scenetree.findObject(PASSES[1].name) ~= nil, settings = settings,
    debug = {mask = dbg.mask, view = dbg.view, shaderDir = dbg.shaderDir}, errors = errors,
  }
end

-- Developer switches; a key left out keeps its value: {mask = true (Sonic's pixels tinted),
-- view = 'blur' | 'tiles' | 'distance' | 'frame' (grey views of each pixel's half blur, the longest
-- around its tile, its distance; 'frame' the blur itself), shaderDir = 'shaders/x/' (the passes made
-- again from there: BeamNG keeps a shader compiled from a path for the whole session, so an edited
-- shader needs a new path), shaderDir = false (back to the mod's)}
M.debug = function(opts)
  if type(opts) ~= 'table' then return M.status() end
  if opts.mask ~= nil then dbg.mask = opts.mask == true end
  if opts.view ~= nil then dbg.view = VIEWS[opts.view] or 0 end
  if opts.shaderDir ~= nil then
    turnOff()
    deletePasses()
    warned = {}
    dbg.shaderDir = type(opts.shaderDir) == 'string' and opts.shaderDir or SHADER_DIR
  end
  return M.status()
end

M.onExtensionLoaded = onExtensionLoaded
M.onExtensionUnloaded = cleanup
M.onClientEndMission = onClientEndMission
M.onModDeactivated = onModDeactivated
M.onPreRender = onPreRender

return M
