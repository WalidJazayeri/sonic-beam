-- SPDX-License-Identifier: MIT
-- Sonic Beam, native mode: Sonic's boost effects, drawn by BeamNG itself. The overlay's five
-- effects (beamng/src/game.cpp:520-686, shared_draw.hpp, v0.12.3; numbers copied into
-- lua/common/sonicnative/effects.lua):
--   speed blur      a compute PostEffect (shaders/sonicnative/speedBlur.cs.hlsl) streaking the frame
--                   toward his chest; fallbacks: the after-tonemap bin, BeamNG's own motion blur
--   aura            two skinned meshes the converter adds to Sonic's shape (manifest fx.aura), shown
--                   or hidden per mesh; their brightness by a material reload, a skin ladder or steps
--   lightning       camera-facing sprites in the ProceduralMesh sonicnative_fx_sprites: bolts around
--                   him and along the trail, bolts on his body (on the converter's bone tracks, else
--                   on a capsule around him), the orbs, glow and shock ring of a boost start or a hit
--   trail           the overlay's three ribbons (beam, locus, arcs) rebuilt each frame in the
--                   ProceduralMesh sonicnative_fx_trail
--   flash light     a blue PointLight at a boost start, like Frontiers' plight01
-- It also hands Sonic's pose (his bones, or the ball shell) to sonicnative_sharp every frame, the
-- motion blur that keeps him sharp.
-- The meshes have no vertex colour: each quad takes the material of its brightness level
-- (<material>_<k>, the manifest's "alphas" ladder). Depth tested, unlike the overlay: sprites are
-- pulled toward the camera so Sonic and the ground do not cut them. Every texture comes from the
-- player's own Frontiers, written by the converter into mods/sonicnative_data (manifest "fx"); a
-- missing key only turns its own effect off. sonicnative_main calls M.afterSonic at the end of its
-- frame (and M.onSonicMeshesShown when it shows his meshes again); sonicnative_menu sends the Lab's
-- settings (M.setSettings). Everything lives in the SimGroup sonicnative_fx_group (the blur's
-- PostEffect and ShaderData by their names) and is deleted at a map's end, on unload and when the
-- mod is turned off.

local M = {}
local logTag = 'sonicnative'

local E = require('sonicnative/effects')

local DATA = '/mods/sonicnative_data/'    -- written on the player's PC by the converter, never shipped
local GROUP_NAME = 'sonicnative_fx_group'
local TRAIL_NAME = 'sonicnative_fx_trail'
local SPRITES_NAME = 'sonicnative_fx_sprites'
local LIGHT_NAME = 'sonicnative_fx_light'
local BLUR_NAME = 'sonicnative_speedBlur'
local BLUR_SHADER_NAME = 'sonicnative_speedBlurShader'
local BLUR_SHADER_FILE = 'shaders/sonicnative/speedBlur.cs.hlsl'   -- in this mod
local SPRITE_KINDS = {'bolt', 'orb', 'glow', 'ring'}
local RIBBON_KINDS = {'beam', 'locus', 'arc'}
local AURA_METHODS = {reload = true, skin = true, steps = true}
local BLUR_BACKENDS = {compute = 'UpscaleDispatch', overlay = 'OverlayRender', motion = false, off = false}
local MAX_ERRORS = 8

local sqrt, min, max, abs, floor = math.sqrt, math.min, math.max, math.abs, math.floor
local cos, sin, random, mlog = math.cos, math.sin, math.random, math.log

-- this session's settings (M.setSettings: the player's saved ones and the Lab), from effects.lua
local settings = {}
for k, v in pairs(E.settings) do settings[k] = v end

-- developer switches (M.debug)
local dbg = {forceKmh = nil, forceBoost = false, freeze = false, bones = false, off = false, burst = false,
  auraMethod = nil, blurBackend = 'compute', meshMode = 'replace'}

local warned, errors = {}, {}
local function report(level, key, msg)
  if warned[key] then return end
  warned[key] = true
  errors[#errors + 1] = msg
  if #errors > MAX_ERRORS then table.remove(errors, 1) end
  log(level, logTag, msg)
end

local function loaded(name)
  if not extensions.isExtensionLoaded(name) then return nil end
  local e = extensions[name]
  if type(e) == 'table' then return e end
end

-- manifest paths are absolute in the virtual file system, or relative to the data folder
local function dataPath(p)
  if type(p) ~= 'string' or p == '' then return nil end
  if p:sub(1, 1) == '/' then return p end
  return DATA .. p
end

-- ---------------------------------------------------------------- the manifest's "fx" block

local defRead, materialsChecked = false, false
local fxDef = nil                 -- {version, materials}: nil when the manifest has no "fx"
local kinds = {}                  -- sprite and ribbon kinds: {base, names[k], have[k], count, usable, gx, gy}
local auraDef = nil               -- {meshes, materials, alpha, colour, skins, skinNames[i][k], can = {reload, skin, steps}}
local bones = nil                 -- {count, names, clips = {name = {frames, loop, pos}}, clipCount, file, segments}
-- Sonic as he is drawn this frame, handed to sonicnative_sharp (read there, not kept): the ball shell
-- (its middle and radius) or his bones in the world (boneX/Y/Z, the cones' bone indices, bonesOk),
-- the StaticShape's place for a capsule when there are no bone tracks, his feet and his up
local sharpPose = {ball = false, ballX = 0, ballY = 0, ballZ = 0, ballRadius = E.sharp.ballRadius,
  bonesOk = false, boneX = nil, boneY = nil, boneZ = nil, segments = nil, place = nil,
  feetX = 0, feetY = 0, feetZ = 0, upX = 0, upY = 0, upZ = 1}

-- the brightness ladder: level k (1-based) is a quad drawn at alpha a[k]; nearest level in log
-- space, none below drop
local ladder = {n = 0, a = {}, logMid = {}, drop = math.huge, from = 'none'}
local function setLadder(alphas)
  local a = {}
  if type(alphas) == 'table' then
    for i, v in ipairs(alphas) do
      v = tonumber(v)
      if not v or v <= 0 or v ~= v or (i > 1 and v <= a[i - 1]) then a = nil break end
      a[i] = v
    end
  end
  local from = 'manifest'
  if not a or #a == 0 then
    -- the converter's formula (sonic_data.hpp kFxAlphas), for a manifest without "alphas"
    local L = E.levels
    a, from = {}, 'effects.lua'
    for k = 1, L.count do a[k] = L.top * L.ratio ^ (k - L.count) end
  end
  local logMid = {}
  for k = 1, #a - 1 do logMid[k] = (mlog(a[k]) + mlog(a[k + 1])) * 0.5 end
  ladder.n, ladder.a, ladder.logMid, ladder.from = #a, a, logMid, from
  ladder.drop = a[1] * E.levels.dropShare
end

-- the ladder level for an alpha (0: too dim, dropped)
local function level(alpha)
  if not (alpha >= ladder.drop) then return 0 end   -- also NaN
  local la = mlog(alpha)
  local lo, hi, mid = 1, ladder.n, ladder.logMid
  while lo < hi do
    local m = floor((lo + hi) * 0.5)
    if la < mid[m] then hi = m else lo = m + 1 end
  end
  return lo
end

local function validName(s)
  return type(s) == 'string' and s:match('^[%w_]+$') ~= nil
end

-- a sprite or ribbon kind of the manifest: {material, grid}
local function addKind(kind, e)
  kinds[kind] = nil
  if type(e) ~= 'table' or not validName(e.material) then return end
  local g = type(e.grid) == 'table' and e.grid or {}
  local gx, gy = floor(tonumber(g[1]) or 1), floor(tonumber(g[2]) or 1)
  local k = {base = e.material, names = {}, have = {}, count = 0, usable = false,
    gx = max(1, gx), gy = max(1, gy)}
  for l = 1, ladder.n do k.names[l] = e.material .. '_' .. l end
  kinds[kind] = k
end

local function parseAura(e)
  if type(e) ~= 'table' or type(e.meshes) ~= 'table' then return nil end
  local a = {meshes = {}, materials = {}, alpha = {}, colour = nil, skins = nil, skinNames = {},
    can = {reload = false, skin = false, steps = false}}
  for _, name in ipairs(e.meshes) do
    if validName(name) then a.meshes[#a.meshes + 1] = name end
  end
  if #a.meshes == 0 then return nil end
  if type(e.materials) == 'table' then
    for i, name in ipairs(e.materials) do a.materials[i] = validName(name) and name or nil end
  end
  if type(e.alpha) == 'table' then
    for i, v in ipairs(e.alpha) do a.alpha[i] = tonumber(v) end
  end
  local c = e.colour or e.color
  if type(c) == 'table' and tonumber(c[1]) and tonumber(c[2]) and tonumber(c[3]) then
    a.colour = {tonumber(c[1]), tonumber(c[2]), tonumber(c[3])}
  end
  -- the skin ladder's name: one integer conversion (l%02d -> l01 .. l24)
  if type(e.skins) == 'string' and e.skins:match('^[%w_]*%%0?%d*d[%w_]*$') then a.skins = e.skins end
  return a
end

-- the converter's bone tracks: {version, bones = [names], clips = {name = {frames, loop, pos}}},
-- pos holding (frame * B + bone) * 3 model-space coordinates
local function readBones(e)
  local file = type(e) == 'table' and dataPath(e.file) or nil
  if not file then return nil end
  if not FS:fileExists(file) then
    report('W', 'bonesFile', 'bone tracks not found: ' .. file .. ' (run the converter again): body bolts ride a capsule around Sonic')
    return nil
  end
  local j = jsonReadFile(file)
  if type(j) ~= 'table' or type(j.bones) ~= 'table' or #j.bones == 0 or type(j.clips) ~= 'table' then
    report('W', 'bonesRead', 'cannot read the bone tracks ' .. file .. ': body bolts ride a capsule around Sonic')
    return nil
  end
  local b = {count = #j.bones, names = j.bones, clips = {}, clipCount = 0, file = file}
  for name, c in pairs(j.clips) do
    local frames = type(c) == 'table' and floor(tonumber(c.frames) or 0) or 0
    if type(name) == 'string' and frames >= 1 and type(c.pos) == 'table' and #c.pos >= frames * b.count * 3 then
      b.clips[name] = {frames = frames, loop = c.loop ~= false, pos = c.pos}
      b.clipCount = b.clipCount + 1
    end
  end
  if b.clipCount == 0 then
    report('W', 'bonesClips', 'the bone tracks ' .. file .. ' have no usable clip: body bolts ride a capsule around Sonic')
    return nil
  end
  -- the sharp motion blur's rounded cones, by bone index: {from, toward, radius at from, radius at
  -- the end, reach (in from-toward lengths)}
  local index = {}
  for i, name in ipairs(b.names) do index[name] = i end
  b.segments = {}
  for _, g in ipairs(E.sharp.segments) do
    local i, j = index[g[1]], index[g[2]]
    if i and j then
      b.segments[#b.segments + 1] = {i, j, g[3], g[4], tonumber(g[5]) or 1}
    else
      report('W', 'sharpBone:' .. tostring(g[1]) .. '-' .. tostring(g[2]), 'the bone tracks have no ' .. tostring(g[1])
        .. ' or ' .. tostring(g[2]) .. ': that part of Sonic is not kept sharp in the motion blur')
    end
  end
  return b
end

local function readManifest()
  defRead, materialsChecked = true, false
  fxDef, auraDef, bones, kinds = nil, nil, nil, {}
  local m = jsonReadFile(DATA .. 'manifest.json')
  -- the ball shell's radius, for the sharp motion blur: the manifest's ball.radius is already in
  -- metres (the converter scales it, natif/converter/main.cpp)
  local ball = type(m) == 'table' and type(m.ball) == 'table' and m.ball or nil
  local r = ball and tonumber(ball.radius)
  sharpPose.ballRadius = r and r > 0 and r or E.sharp.ballRadius
  local fx = type(m) == 'table' and m.fx or nil
  setLadder(type(fx) == 'table' and fx.alphas or nil)
  if type(fx) ~= 'table' then
    report('W', 'noFx', 'no "fx" in ' .. DATA .. 'manifest.json: run the converter again for the trail, lightning and aura (the speed blur and the light work without it)')
    return false
  end
  fxDef = {version = tonumber(fx.version), materials = dataPath(fx.materials)}
  for _, kind in ipairs(SPRITE_KINDS) do addKind(kind, type(fx.sprites) == 'table' and fx.sprites[kind] or nil) end
  for _, kind in ipairs(RIBBON_KINDS) do addKind(kind, type(fx.ribbons) == 'table' and fx.ribbons[kind] or nil) end
  auraDef = parseAura(fx.aura)
  bones = readBones(fx.bones)
  return true
end

-- which effect materials exist (loaded from the manifest's file when some are missing, once a map);
-- which aura methods can work
local function countMissing()
  local missing = 0
  for _, k in pairs(kinds) do
    k.count = 0
    for l = 1, ladder.n do
      k.have[l] = scenetree.findObject(k.names[l]) ~= nil
      if k.have[l] then k.count = k.count + 1 else missing = missing + 1 end
    end
    k.usable = k.count > 0
  end
  return missing
end

local function checkAura()
  local a = auraDef
  if not a then return end
  local reload = a.colour ~= nil and #a.materials >= #a.meshes
  for i = 1, #a.meshes do
    if not (a.materials[i] and a.alpha[i] and scenetree.findObject(a.materials[i])) then reload = false end
  end
  -- setSkinName renames the material targets starting with "base": base_x -> l01_x .. l24_x
  local skin = a.skins ~= nil and #a.materials >= #a.meshes
  for i = 1, #a.meshes do
    local base = a.materials[i]
    a.skinNames[i] = {}
    if not (a.skins and base and base:sub(1, 4) == 'base') then skin = false
    else
      for l = 1, ladder.n do
        local name = string.format(a.skins, l) .. base:sub(5)
        a.skinNames[i][l] = name
        if not scenetree.findObject(name) then skin = false end
      end
    end
  end
  a.can.reload, a.can.skin, a.can.steps = reload, skin, true
end

local auraAuto = nil              -- the method chosen by itself (effects.lua aura.methods order)
local function chooseAuraMethod()
  auraAuto = nil
  if not auraDef then return end
  for _, m in ipairs(E.aura.methods) do
    if auraDef.can[m] then auraAuto = m return end
  end
end

local function checkMaterials()
  materialsChecked = true
  if not fxDef then return end
  local missing = countMissing()
  if missing > 0 and fxDef.materials then
    if FS:fileExists(fxDef.materials) then
      local ok, res = pcall(loadJsonMaterialsFile, fxDef.materials)
      if not ok or res == false then report('E', 'fxMaterialsLoad', 'loadJsonMaterialsFile failed on ' .. fxDef.materials .. ': ' .. tostring(res)) end
    else
      report('W', 'fxMaterialsFile', 'effect materials not found: ' .. fxDef.materials .. ' (run the converter again)')
    end
    missing = countMissing()
  end
  if missing > 0 then report('W', 'fxMaterialsMissing', missing .. ' effect materials are missing: their quads are not drawn') end
  checkAura()
  chooseAuraMethod()
end

local function ensureReady()
  if not defRead then readManifest() end
  if not materialsChecked then checkMaterials() end
end

-- ---------------------------------------------------------------- scene objects

local function ensureGroup()
  local g = scenetree.findObject(GROUP_NAME)
  if g then return g end
  g = createObject('SimGroup')
  if not g then return nil end
  g.canSave = false
  g:registerObject(GROUP_NAME)
  return scenetree.findObject(GROUP_NAME) and g or nil
end

local function addToGroup(o)
  local g = ensureGroup()
  if not g then return end
  -- the game passes the raw object (meshPart.lua) or the object itself (groundMarkerArrows.lua)
  if not pcall(g.addObject, g, o.obj) then pcall(g.addObject, g, o) end
end

local function setObjHidden(o, hidden)
  if not pcall(o.setHidden, o, hidden) then
    pcall(function() o.hidden = hidden end)
  end
end

local function deleteNamed(name)
  local o = scenetree.findObject(name)
  if o then pcall(o.delete, o) end
end

-- the camera this frame: position, right, forward, up and tan(vertical fov / 2)
local cam = {x = 0, y = 0, z = 0, rx = 1, ry = 0, rz = 0, fx = 0, fy = 1, fz = 0, ux = 0, uy = 0, uz = 1, tanHalf = 0.6}

-- (x, y, z) turned by the quaternion, BeamNG's way (lua/common/mathlib.lua LuaQuat.__mul: q * v)
local function rotate(qx, qy, qz, qw, x, y, z)
  local tx, ty, tz = 2 * (qy * z - qz * y), 2 * (qz * x - qx * z), 2 * (qx * y - qy * x)
  return x - qw * tx + (qy * tz - qz * ty), y - qw * ty + (qz * tx - qx * tz), z - qw * tz + (qx * ty - qy * tx)
end

local function readCamera()
  local c = core_camera
  if type(c) ~= 'table' then return false end
  if c.getPositionXYZ and c.getQuatXYZW then
    local x, y, z = c.getPositionXYZ()
    local qx, qy, qz, qw = c.getQuatXYZW()
    if not (tonumber(x) and tonumber(qw)) then return false end
    cam.x, cam.y, cam.z = x, y, z
    cam.rx, cam.ry, cam.rz = rotate(qx, qy, qz, qw, 1, 0, 0)
    cam.fx, cam.fy, cam.fz = rotate(qx, qy, qz, qw, 0, 1, 0)
    cam.ux, cam.uy, cam.uz = rotate(qx, qy, qz, qw, 0, 0, 1)
  elseif c.getPosition and c.getForward and c.getUp and c.getRight then
    local p, f, u, r = c.getPosition(), c.getForward(), c.getUp(), c.getRight()
    if not (p and f and u and r) then return false end
    cam.x, cam.y, cam.z = p.x, p.y, p.z
    cam.rx, cam.ry, cam.rz, cam.fx, cam.fy, cam.fz, cam.ux, cam.uy, cam.uz = r.x, r.y, r.z, f.x, f.y, f.z, u.x, u.y, u.z
  else
    return false
  end
  local fov = c.getFovRad and tonumber(c.getFovRad()) or nil
  if fov and fov > 0.01 and fov < 3.1 then cam.tanHalf = math.tan(fov * 0.5) end
  return true
end

-- ---------------------------------------------------------------- ProceduralMesh builders
-- One builder per mesh: a part per material, refilled every frame from pooled tables (the engine
-- copies the data in createMesh). Quads are wound for ProceduralMesh's front face (meshPart.lua:10);
-- the materials are double-sided anyway.

local function newBuilder(name)
  return {name = name, byMaterial = {}, used = {}, nUsed = 0, list = {}, listN = 0, arg = nil, stamp = 0,
    ox = 0, oy = 0, oz = 0, vec = vec3(0, 0, 0), hidden = nil, quads = 0, corners = 0, parts = 0, created = 0}
end
local trailB, spriteB = newBuilder(TRAIL_NAME), newBuilder(SPRITES_NAME)
trailB.arg, spriteB.arg = {trailB.list}, {spriteB.list}

local function begin(b, ox, oy, oz)
  b.stamp, b.nUsed, b.quads, b.corners = b.stamp + 1, 0, 0, 0
  b.ox, b.oy, b.oz = ox, oy, oz
end

local function partFor(b, material)
  local info = b.byMaterial[material]
  if not info then
    local normal = {x = 0, y = 0, z = 1}
    info = {part = {material = material, verts = {}, normals = {normal}, uvs = {}, faces = {}},
      normal = normal, sv = {}, su = {}, sf = {}, nv = 0, nf = 0, lastNv = 0, lastNf = 0, stamp = -1}
    b.byMaterial[material] = info
  end
  if info.stamp ~= b.stamp then
    info.stamp, info.nv, info.nf = b.stamp, 0, 0
    -- one normal per part, toward the camera (the materials are emissive and unlit)
    info.normal.x, info.normal.y, info.normal.z = -cam.fx, -cam.fy, -cam.fz
    b.nUsed = b.nUsed + 1
    b.used[b.nUsed] = info
  end
  return info
end

local function vert(b, info, x, y, z, u, v)
  local i = info.nv + 1
  info.nv = i
  local t = info.sv[i]
  if not t then t = {x = 0, y = 0, z = 0} info.sv[i] = t end
  t.x, t.y, t.z = x - b.ox, y - b.oy, z - b.oz
  info.part.verts[i] = t
  local w = info.su[i]
  if not w then w = {u = 0, v = 0} info.su[i] = w end
  w.u, w.v = u, v
  info.part.uvs[i] = w
  return i - 1
end

local function corner(info, i)
  local j = info.nf + 1
  info.nf = j
  local f = info.sf[j]
  if not f then f = {v = 0, n = 0, u = 0} info.sf[j] = f end
  f.v, f.n, f.u = i, 0, i
  info.part.faces[j] = f
end

-- corners 0..3 counterclockwise as the camera sees them; ProceduralMesh's front face has its
-- winding normal pointing away from the viewer, hence (0, 2, 1) (0, 3, 2)
local function quad(b, info, x0, y0, z0, u0, v0, x1, y1, z1, u1, v1, x2, y2, z2, u2, v2, x3, y3, z3, u3, v3)
  if b.corners + 6 > E.mesh.maxCorners then return end
  local i0 = vert(b, info, x0, y0, z0, u0, v0)
  local i1 = vert(b, info, x1, y1, z1, u1, v1)
  local i2 = vert(b, info, x2, y2, z2, u2, v2)
  local i3 = vert(b, info, x3, y3, z3, u3, v3)
  corner(info, i0) corner(info, i2) corner(info, i1)
  corner(info, i0) corner(info, i3) corner(info, i2)
  b.quads, b.corners = b.quads + 1, b.corners + 6
end

local function hideMesh(b)
  if b.hidden == true then return end
  local o = scenetree.findObject(b.name)
  if o then setObjHidden(o, true) end
  b.hidden = true
end

local function ensureMesh(b)
  local o = scenetree.findObject(b.name)
  if o and dbg.meshMode == 'recreate' then
    pcall(o.delete, o)
    o = nil
  end
  if o then return o end
  o = createObject('ProceduralMesh')
  if not o then report('E', 'meshCreate:' .. b.name, 'cannot create the ProceduralMesh ' .. b.name) return nil end
  o:registerObject(b.name)
  o.canSave = false
  o:setField('collisionType', 0, 'None')   -- before createMesh: no hull (meshPart.lua:463-474)
  addToGroup(o)
  b.hidden, b.created = false, b.created + 1
  return scenetree.findObject(b.name) and o or nil
end

-- this frame's parts into the mesh; no quad: the mesh is hidden
local function submit(b)
  local list, n = b.list, 0
  for i = 1, b.nUsed do
    local info = b.used[i]
    local p = info.part
    for k = info.nv + 1, info.lastNv do p.verts[k] = nil p.uvs[k] = nil end
    for k = info.nf + 1, info.lastNf do p.faces[k] = nil end
    info.lastNv, info.lastNf = info.nv, info.nf
    if info.nf > 0 then n = n + 1 list[n] = p end
  end
  for i = n + 1, b.listN do list[i] = nil end
  b.listN, b.parts = n, n
  if n == 0 then hideMesh(b) return end
  local o = ensureMesh(b)
  if not o then return end
  local ok, err = pcall(o.createMesh, o, b.arg)
  if not ok then report('E', 'createMesh:' .. b.name, 'createMesh failed on ' .. b.name .. ': ' .. tostring(err)) return end
  b.vec:set(b.ox, b.oy, b.oz)
  o:setPosition(b.vec)
  if b.hidden ~= false then setObjHidden(o, false) b.hidden = false end
end

-- ---------------------------------------------------------------- frame state

local clock = 0
local ranSinceTick = false        -- afterSonic ran since the last onPreRender
local shown = true                -- something may be showing (true at load: objects of an earlier Lua session)
local inputs = {kmh = 0, fxRatio = 0, fxSpeed = 0, boosting = false, boostKey = false, lit = false, state = nil, ball = 0,
  hidden = false}
local feetX, feetY, feetZ, originX, originY, originZ = 0, 0, 0, 0, 0, 0
local place = {x = 0, y = 0, z = 0, qx = 0, qy = 0, qz = 0, qw = 1, scale = 1}
local auraFade, auraAmount, blurValue, trailOn = 0, 0, 0, 0
local wasBoosting, hitSeen, lastHitClock = false, false, nil
local burstAt, burstX, burstY, burstZ, burst = -100, 0, 0, 0, 0
local glowX, glowY, glowZ = 0, 0, 0
local boltDebt, bodyDebt = 0, 0

-- the trail: points oldest first ({x, y, z, born}, pooled), the path drawn this frame (newest first)
local pts, nPts, sparePts = {}, 0, {}
local pathX, pathY, pathZ, pathB, pathN = {}, {}, {}, {}, 0
local trailActive, keep = false, E.trail.seconds
local chestX, chestY, chestZ = 0, 0, 0
local arcReroll, arcColumn = -100, 0
local jitX, jitY, jitZ = {}, {}, {}
for i = 1, E.trail.arc.jitterCount do jitX[i], jitY[i], jitZ[i] = 0, 0, 0 end

-- bolts and orbs: {kind, x, y, z, vx, vy, vz, born, life, size, spin, cell, bone, capsule}, pooled
local bolts, nBolts, spareBolts = {}, 0, {}
local bodyBoltsAlive = 0

-- bones of the playing clip in the world this frame
local boneStamp, boneOk = -1, false
local boneX, boneY, boneZ = {}, {}, {}
local frameNo = 0

local blurOn = nil                -- the PostEffect enabled (nil: not known, after a Lua reload)
local blurX, blurY, blurSent = 0, 0, {x = nil, y = nil, s = nil}
local motionSaved = nil           -- the player's PostFxMotionBlur strength while the 'motion' fallback runs
local lightOn, lightIntensity = nil, 0
local aura = {id = nil, level = nil, method = nil, reloads = 0, reloadMs = nil, reloadedTo = nil, skinSet = false}

local frameTimer = hptimer and hptimer() or nil
local reloadTimer = hptimer and hptimer() or nil
local ms = {last = 0, max = 0, avg = 0}

-- ---------------------------------------------------------------- trail points

local function clearTrail()
  for i = 1, nPts do sparePts[#sparePts + 1] = pts[i] pts[i] = nil end
  nPts, pathN = 0, 0
end

local function pushPoint(x, y, z, born)
  local p = sparePts[#sparePts]
  if p then sparePts[#sparePts] = nil else p = {} end
  p.x, p.y, p.z, p.born = x, y, z, born
  nPts = nPts + 1
  pts[nPts] = p
end

-- points older than keep leave from the front
local function dropExpired()
  local first = 1
  while first <= nPts and clock - pts[first].born > keep do first = first + 1 end
  if first == 1 then return end
  local gone = first - 1
  for i = 1, gone do sparePts[#sparePts + 1] = pts[i] end
  for i = first, nPts do pts[i - gone] = pts[i] end
  for i = nPts - gone + 1, nPts do pts[i] = nil end
  nPts = nPts - gone
end

-- every other point between the oldest and the newest goes (the trail holds at most maxPoints)
local function decimate()
  local w = 1
  for r = 1, nPts do
    local p = pts[r]
    if r == 1 or r == nPts or (nPts - r) % 2 == 0 then pts[w] = p w = w + 1
    else sparePts[#sparePts + 1] = p end
  end
  for i = w, nPts do pts[i] = nil end
  nPts = w - 1
end

local function addPath(x, y, z, born)
  pathN = pathN + 1
  pathX[pathN], pathY[pathN], pathZ[pathN], pathB[pathN] = x, y, z, born
end

-- ---------------------------------------------------------------- bolts

local function newBolt(kind, born, life, size)
  if nBolts >= E.sprites.maxCount then return nil end
  local b = spareBolts[#spareBolts]
  if b then spareBolts[#spareBolts] = nil else b = {} end
  b.kind, b.born, b.life, b.size = kind, born, life, size
  b.x, b.y, b.z, b.vx, b.vy, b.vz = 0, 0, 0, 0, 0, 0
  b.spin, b.cell, b.bone, b.capsule = random() * 6.28, 0, nil, false
  local k = kinds.bolt
  if kind == 'bolt' and k then b.cell = floor(random() * k.gx * k.gy) end
  nBolts = nBolts + 1
  bolts[nBolts] = b
  return b
end

local function clearBolts()
  for i = 1, nBolts do spareBolts[#spareBolts + 1] = bolts[i] bolts[i] = nil end
  nBolts, bodyBoltsAlive = 0, 0
end

-- the flash of a boost start (bolts by lightning x fx_speed and the orbs) or of a car hit
local function startBurst(impact)
  local Bu = E.burst
  burstAt, burstX, burstY, burstZ = clock, originX, originY, originZ
  local count, delay, life, size, radius
  if impact then
    local I = Bu.impact
    count, delay, life, size, radius = I.bolts, I.delay, I.life, I.size, I.radius
  else
    count = floor(Bu.bolts * max(0, settings.lightning) * inputs.fxSpeed)
    delay, life, size, radius = Bu.delay, Bu.life, Bu.size, Bu.radius
  end
  for i = 0, count - 1 do
    local b = newBolt('bolt', clock + delay * i, life, size[1] + size[2] * random())
    if not b then break end
    local a, r = random() * 2 * math.pi, radius[1] + radius[2] * random()
    b.x, b.y, b.z = originX + cos(a) * r, originY + sin(a) * r, originZ + (random() + Bu.z[1]) * Bu.z[2]
  end
  if impact then return end
  for _ = 1, Bu.orbs do
    local b = newBolt('orb', clock, Bu.orbLife, Bu.orbSize[1] + Bu.orbSize[2] * random())
    if not b then break end
    local a = random() * 2 * math.pi
    b.x, b.y, b.z = originX, originY, originZ
    b.vx = cos(a) * (Bu.orbXY[1] + Bu.orbXY[2] * random())
    b.vy = sin(a) * (Bu.orbXY[1] + Bu.orbXY[2] * random())
    b.vz = Bu.orbUp[1] + Bu.orbUp[2] * random()
    b.spin = 0
  end
end

-- the bones of the playing clip, in the world (once a frame, when asked): false without tracks for it
local function sampleBones(a)
  if boneStamp == frameNo then return boneOk end
  boneStamp, boneOk = frameNo, false
  local bd = bones
  local c = bd and type(a.sequence) == 'string' and bd.clips[a.sequence] or nil
  if not c then return false end
  local last = c.frames - 1
  local f = (tonumber(a.phase) or 0) * last
  if not (f >= 0) then f = 0 elseif f > last then f = last end
  local f0 = floor(f)
  local f1 = min(f0 + 1, last)
  local t = f - f0
  local B, pos, s = bd.count, c.pos, place.scale
  local qx, qy, qz, qw = place.qx, place.qy, place.qz, place.qw
  for b = 0, B - 1 do
    local i0, i1 = (f0 * B + b) * 3, (f1 * B + b) * 3
    local x0, y0, z0 = tonumber(pos[i0 + 1]) or 0, tonumber(pos[i0 + 2]) or 0, tonumber(pos[i0 + 3]) or 0
    local x = x0 + ((tonumber(pos[i1 + 1]) or 0) - x0) * t
    local y = y0 + ((tonumber(pos[i1 + 2]) or 0) - y0) * t
    local z = z0 + ((tonumber(pos[i1 + 3]) or 0) - z0) * t
    local wx, wy, wz = rotate(qx, qy, qz, qw, x * s, y * s, z * s)
    boneX[b + 1], boneY[b + 1], boneZ[b + 1] = place.x + wx, place.y + wy, place.z + wz
  end
  boneOk = true
  return true
end

-- a random point of the capsule around Sonic, in his own frame (metres)
local function capsulePoint(b)
  local C = E.capsule
  local half = C.height * 0.5
  local z = (random() - 0.5) * C.height
  local r = C.radius
  local cap = abs(z) - (half - C.radius)
  if cap > 0 then r = sqrt(max(0, C.radius * C.radius - cap * cap)) end
  local a = random() * 2 * math.pi
  b.x, b.y, b.z, b.capsule = cos(a) * r, sin(a) * r, C.up + z, true
end

local function updateBolts(dt, a)
  local L = max(0, settings.lightning) * inputs.fxSpeed
  local Bo = E.bolts
  -- around Sonic and along the trail while the boost key is held (game.cpp:590-597)
  if trailActive and L > 0 then
    boltDebt = boltDebt + dt * Bo.rate * L
    while boltDebt >= 1 do
      boltDebt = boltDebt - 1
      local x, y, z = chestX, chestY, chestZ
      if nPts > 0 and random() < Bo.trailShare then
        local p = pts[floor(random() * nPts) % nPts + 1]
        x, y, z = p.x, p.y, p.z
      end
      local b = newBolt('bolt', clock, Bo.life[1] + Bo.life[2] * random(), Bo.size[1] + Bo.size[2] * random())
      if b then
        b.x = x + (random() - 0.5) * Bo.spread[1]
        b.y = y + (random() - 0.5) * Bo.spread[2]
        b.z = z + (random() - 0.5) * Bo.spread[3]
      end
    end
  else
    boltDebt = 0
  end
  -- on his body while the aura is lit (game.cpp:600-613): on a bone, else on the capsule; none
  -- while only the ball shell shows
  local BB = E.bodyBolts
  local lvl = auraFade * inputs.fxSpeed * max(0, settings.auraLightning) * (1 - inputs.ball)
  if lvl > BB.minLevel and not inputs.hidden then
    bodyDebt = bodyDebt + dt * BB.rate * lvl
    local onBones = sampleBones(a)
    while bodyDebt >= 1 do
      bodyDebt = bodyDebt - 1
      local b = newBolt('bolt', clock, BB.life[1] + BB.life[2] * random(), BB.size[1] + BB.size[2] * random())
      if b then
        if onBones then
          b.bone = floor(random() * bones.count) % bones.count + 1
          b.x = (random() - 0.5) * BB.jitter
          b.y = (random() - 0.5) * BB.jitter
          b.z = (random() - 0.5) * BB.jitter
        else
          capsulePoint(b)
        end
      end
    end
  else
    bodyDebt = 0
  end
  -- the dead leave (in place, pooled)
  local w = 0
  bodyBoltsAlive = 0
  for i = 1, nBolts do
    local b = bolts[i]
    if clock - b.born > b.life then
      spareBolts[#spareBolts + 1] = b
    else
      w = w + 1
      bolts[w] = b
      if b.bone or b.capsule then bodyBoltsAlive = bodyBoltsAlive + 1 end
    end
  end
  for i = w + 1, nBolts do bolts[i] = nil end
  nBolts = w
end

-- ---------------------------------------------------------------- sprites (port of sprite_vertices)

local spriteCount = 0
local function sprite(kind, px, py, pz, size, alpha, spin, u0, v0, u1, v1)
  local k = kinds[kind]
  if not (k and k.usable) then return end
  local lvl = level(alpha)
  if lvl == 0 or not k.have[lvl] then return end
  local dx, dy, dz = cam.x - px, cam.y - py, cam.z - pz
  local d = sqrt(dx * dx + dy * dy + dz * dz)
  if d < 1e-3 then return end
  -- native only: pulled toward the camera, the same size on screen
  local pull = min(E.sprites.cameraPull[kind] or 0, d * 0.5)
  px, py, pz = px + dx / d * pull, py + dy / d * pull, pz + dz / d * pull
  size = size * (d - pull) / d
  local c, s = cos(spin) * size * 0.5, sin(spin) * size * 0.5
  local rx, ry, rz, ux, uy, uz = cam.rx, cam.ry, cam.rz, cam.ux, cam.uy, cam.uz
  -- corners (-1,-1) (1,-1) (1,1) (-1,1): x = kx c - ky s, y = kx s + ky c
  local ax, ay = -c + s, -s - c
  local bx, by = c + s, s - c
  local cx, cy = c - s, s + c
  local ex, ey = -c - s, -s + c
  quad(spriteB, partFor(spriteB, k.names[lvl]),
    px + rx * ax + ux * ay, py + ry * ax + uy * ay, pz + rz * ax + uz * ay, u0, v1,
    px + rx * bx + ux * by, py + ry * bx + uy * by, pz + rz * bx + uz * by, u1, v1,
    px + rx * cx + ux * cy, py + ry * cx + uy * cy, pz + rz * cx + uz * cy, u1, v0,
    px + rx * ex + ux * ey, py + ry * ex + uy * ey, pz + rz * ex + uz * ey, u0, v0)
  spriteCount = spriteCount + 1
end

local function buildSprites(a)
  spriteCount = 0
  begin(spriteB, originX, originY, originZ)
  local kb = kinds.bolt
  local gx, gy = kb and kb.gx or 1, kb and kb.gy or 1
  for i = 1, nBolts do
    local b = bolts[i]
    local age = clock - b.born
    if age >= 0 then
      local k = age / b.life
      local x, y, z, ok = b.x + b.vx * age, b.y + b.vy * age, b.z + b.vz * age, true
      -- none on his body while only the ball shell shows (game.cpp:619)
      if inputs.hidden and (b.bone or b.capsule) then
        ok = false
      elseif b.bone then
        -- on a bone of his current pose; hidden while the playing clip has no track
        ok = sampleBones(a)
        if ok then x, y, z = boneX[b.bone] + b.x, boneY[b.bone] + b.y, boneZ[b.bone] + b.z end
      elseif b.capsule then
        local wx, wy, wz = rotate(place.qx, place.qy, place.qz, place.qw, b.x, b.y, b.z)
        x, y, z = place.x + wx, place.y + wy, place.z + wz
      end
      if ok then
        if b.kind == 'orb' then
          sprite('orb', x, y, z, b.size, (1 - k) * (1 - k), b.spin, 0, 0, 1, 1)
        else
          local cu, cv = b.cell % gx, floor(b.cell / gx) % gy
          sprite('bolt', x, y, z, b.size, 1 - k, b.spin, cu / gx, cv / gy, (cu + 1) / gx, (cv + 1) / gy)
        end
      end
    end
  end
  -- the glow and the shock ring of a boost start, following Sonic a little (game.cpp:677-683)
  if burst > 0 then
    local Bu = E.burst
    local grow = (clock - burstAt) / Bu.seconds
    sprite('glow', glowX, glowY, glowZ, Bu.glow[1] + Bu.glow[2] * grow, burst * burst, clock * Bu.glow.spin, 0, 0, 1, 1)
    sprite('ring', glowX, glowY, glowZ, Bu.ring[1] + Bu.meters * grow, burst, clock * Bu.ring.spin, 0, 0, 1, 1)
  end
end

-- ---------------------------------------------------------------- trail (port of game.cpp:520-587)

local function updateTrail(dt, a)
  local T = E.trail
  local len = settings.trailLength > 0 and settings.trailLength or 1
  keep = T.seconds * max(T.minLength, len * inputs.fxSpeed)
  local active = inputs.boostKey and inputs.kmh > T.minKmh
  trailActive = active
  trailOn = trailOn + ((active and 1 or 0) - trailOn) * min(1, dt * (active and T.onRate or T.offRate))
  -- his travel direction, else his heading; the trail starts just behind his back
  local v = a.vel
  local vx, vy, vz = 0, 0, 0
  if type(v) == 'table' then vx, vy, vz = tonumber(v[1]) or 0, tonumber(v[2]) or 0, tonumber(v[3]) or 0 end
  local vl = sqrt(vx * vx + vy * vy + vz * vz)
  if vl > 1 then vx, vy, vz = vx / vl, vy / vl, vz / vl
  else
    local h = tonumber(a.heading) or 0
    vx, vy, vz = cos(h), sin(h), 0
  end
  chestX, chestY, chestZ = feetX - vx * T.chestBack, feetY - vy * T.chestBack, feetZ + T.chestUp - vz * T.chestBack
  -- a teleport clears it (checked before the new point: the overlay's order hid it while boosting)
  if nPts > 0 then
    local p = pts[nPts]
    local dx, dy, dz = chestX - p.x, chestY - p.y, chestZ - p.z
    if dx * dx + dy * dy + dz * dz > E.common.teleport * E.common.teleport then clearTrail() end
  end
  -- native only: the spacing grows with the speed so the trail keeps at most maxPoints
  local spacing = max(T.spacing, inputs.kmh / 3.6 * keep / max(1, T.maxPoints - 8))
  if active then
    local far = nPts == 0
    if not far then
      local p = pts[nPts]
      local dx, dy, dz = chestX - p.x, chestY - p.y, chestZ - p.z
      far = dx * dx + dy * dy + dz * dz > spacing * spacing
    end
    if far then pushPoint(chestX, chestY, chestZ, clock) end
  end
  dropExpired()
  if nPts > T.maxPoints then decimate() end
  -- the arcs strike again a few times a second: a new column, a new shake per point
  if clock >= arcReroll then
    local A = T.arc
    arcReroll = clock + A.reroll
    local k = kinds.arc
    arcColumn = floor(random() * (k and k.gx or 1))
    for i = 1, A.jitterCount do
      jitX[i], jitY[i], jitZ[i] = (random() - 0.5) * A.jitter, (random() - 0.5) * A.jitter, (random() - 0.5) * A.jitter
    end
  end
  -- the ribbon runs from his chest back along the points
  pathN = 0
  if active then addPath(chestX, chestY, chestZ, clock) end
  for i = nPts, 1, -1 do
    local p = pts[i]
    addPath(p.x, p.y, p.z, p.born)
  end
end

local function trailLevel(alpha)
  local l = level(alpha)
  local step = E.trail.levelStep
  if l > 0 and step > 1 then l = min(ladder.n, floor((l - 1) / step + 0.5) * step + 1) end
  return l
end

-- one ribbon: faces the camera, lying flat where it points at it; v scrolls (game.cpp:545-580)
local function ribbon(kind, width, alpha, u0, u1, jitter, vScale, vSpeed)
  local k = kinds[kind]
  if not (k and k.usable) then return end
  local T = E.trail
  local along, have = 0, false
  local plx, ply, plz, prx, pry, prz, pv, pa = 0, 0, 0, 0, 0, 0, 0, 0
  local vShift = (clock * vSpeed) % 1      -- the texture wraps: only the fraction moves it
  local fadeLen = settings.trailFade
  local inv = 1 / max(0.05, keep)
  local jn = T.arc.jitterCount
  for i = 1, pathN do
    local qx, qy, qz = pathX[i], pathY[i], pathZ[i]
    local px, py, pz = qx, qy, qz
    if jitter and i > 1 then
      local j = (i - 1) % jn + 1
      px, py, pz = px + jitX[j], py + jitY[j], pz + jitZ[j]
    end
    local dx, dy, dz
    if i < pathN then dx, dy, dz = qx - pathX[i + 1], qy - pathY[i + 1], qz - pathZ[i + 1]
    elseif i > 1 then dx, dy, dz = pathX[i - 1] - qx, pathY[i - 1] - qy, pathZ[i - 1] - qz
    else dx, dy, dz = cam.fx, cam.fy, cam.fz end
    if i > 1 then
      local ex, ey, ez = pathX[i - 1] - qx, pathY[i - 1] - qy, pathZ[i - 1] - qz
      along = along + sqrt(ex * ex + ey * ey + ez * ez)
    end
    local age = (clock - pathB[i]) * inv
    local head = fadeLen > 0.001 and min(1, along / fadeLen) or 1
    local fade = max(0, 1 - age) ^ T.fadePow * alpha * trailOn * head * head
    local dl = sqrt(dx * dx + dy * dy + dz * dz)
    if dl > 1e-6 then dx, dy, dz = dx / dl, dy / dl, dz / dl else dx, dy, dz = cam.fx, cam.fy, cam.fz end
    local vx, vy, vz = px - cam.x, py - cam.y, pz - cam.z
    local vl = sqrt(vx * vx + vy * vy + vz * vz)
    if vl > 1e-6 then vx, vy, vz = vx / vl, vy / vl, vz / vl end
    local fx, fy, fz = dy * vz - dz * vy, dz * vx - dx * vz, dx * vy - dy * vx
    local edge = sqrt(fx * fx + fy * fy + fz * fz)
    local sx, sy, sz = dy, -dx, 0                          -- cross(d, up)
    local sl = sqrt(sx * sx + sy * sy)
    if sl < 1e-3 then sx, sy, sz = cam.rx, cam.ry, cam.rz sl = sqrt(sx * sx + sy * sy + sz * sz) end
    if sl > 1e-9 then sx, sy, sz = sx / sl, sy / sl, sz / sl end
    if fx * sx + fy * sy + fz * sz < 0 then sx, sy, sz = -sx, -sy, -sz end
    local kf = min(1, edge / 0.5)
    if edge > 1e-9 then fx, fy, fz = fx / edge, fy / edge, fz / edge end
    local wx, wy, wz = fx * kf + sx * (1 - kf), fy * kf + sy * (1 - kf), fz * kf + sz * (1 - kf)
    local wl = sqrt(wx * wx + wy * wy + wz * wz)
    if wl < 1e-9 then wx, wy, wz, wl = sx, sy, sz, 1 end
    local half = width * (T.ageWidth + (1 - T.ageWidth) * max(0, 1 - age)) * 0.5 / wl
    wx, wy, wz = wx * half, wy * half, wz * half
    local lx, ly, lz, rx, ry, rz = px - wx, py - wy, pz - wz, px + wx, py + wy, pz + wz
    local v = along / vScale - vShift
    if have then
      local lvl = trailLevel((pa + fade) * 0.5)
      if lvl > 0 and k.have[lvl] then
        quad(trailB, partFor(trailB, k.names[lvl]),
          plx, ply, plz, u0, pv, prx, pry, prz, u1, pv, rx, ry, rz, u1, v, lx, ly, lz, u0, v)
      end
    end
    plx, ply, plz, prx, pry, prz, pv, pa, have = lx, ly, lz, rx, ry, rz, v, fade, true
  end
end

local function buildTrail()
  local T = E.trail
  begin(trailB, chestX, chestY, chestZ)
  local I = max(0, settings.trailIntensity) * inputs.fxSpeed
  local L = max(0, settings.lightning) * inputs.fxSpeed
  if trailOn > 0.01 and pathN >= 2 and I > 0 then
    -- the electric-blue beam underneath, Frontiers' locus streaks, lightning arcs along it
    ribbon('beam', T.width * T.beam.width, T.beam.alpha * min(1, I), 0, 1, false, 1, 0)
    ribbon('locus', T.width, I, 0, 1, false, T.locus.vScale, T.locus.vSpeed)
    if L > 0 then
      local k = kinds.arc
      local cols = k and k.gx or 1
      ribbon('arc', T.width * T.arc.width, T.arc.alpha * min(T.arc.maxLightning, L), arcColumn / cols, (arcColumn + 1) / cols,
        true, T.arc.vScale, 0)
    end
  end
end

-- ---------------------------------------------------------------- speed blur

local function ensureShader()
  local sh = scenetree.findObject(BLUR_SHADER_NAME)
  if sh then return sh end
  if not FS:fileExists('/' .. BLUR_SHADER_FILE) then
    report('E', 'blurShader', 'speed blur shader not found: /' .. BLUR_SHADER_FILE)
    return nil
  end
  sh = createObject('ShaderData')
  if not sh then return nil end
  sh.DXComputShaderName = BLUR_SHADER_FILE
  sh.pixVersion = 5.0
  sh.canSave = false
  sh:registerObject(BLUR_SHADER_NAME)
  return scenetree.findObject(BLUR_SHADER_NAME) and sh or nil
end

local function ensureBlur()
  local p = scenetree.findObject(BLUR_NAME)
  if p then return p end
  local bin = BLUR_BACKENDS[dbg.blurBackend]
  if not bin or not ensureShader() then return nil end
  p = createObject('PostEffect')
  if not p then return nil end
  p.isEnabled = false
  p.allowReflectPass = false
  p.canSave = false
  p:setField('renderTime', 0, 'PFXAfterBin')
  p:setField('renderBin', 0, bin)
  p.renderPriority = E.blur.renderPriority
  p:setField('shader', 0, BLUR_SHADER_NAME)
  p:setField('stateBlock', 0, 'PFX_DefaultStateBlock')
  p:setField('texture', 0, '$backBuffer')
  p:setField('target', 0, '$backBuffer')
  p:registerObject(BLUR_NAME)
  blurOn = false
  return scenetree.findObject(BLUR_NAME) and p or nil
end

local function restoreMotion()
  if not motionSaved then return end
  local mb = scenetree.findObject('PostFxMotionBlur')
  if mb then pcall(function() mb.strength = motionSaved end) end
  motionSaved = nil
end

local function blurOff()
  if blurOn ~= false then
    local p = scenetree.findObject(BLUR_NAME)
    if p then pcall(p.disable, p) end
    blurOn = false
    blurSent.s = nil
  end
  restoreMotion()
end

-- the speed blur at this strength (the Lab's x blurStrength applied, before effects.blur.strength)
local function setBlur(v)
  local Bl = E.blur
  local backend = dbg.blurBackend
  if not (v > Bl.minShown) or backend == 'off' then blurOff() return end
  if backend == 'motion' then
    -- BeamNG's own motion blur, made stronger with the speed (not radial); the player's value comes back after
    local mb = scenetree.findObject('PostFxMotionBlur')
    if not mb then report('W', 'motionBlur', 'PostFxMotionBlur not found: no speed blur') return end
    if not motionSaved then motionSaved = tonumber(mb.strength) or 0 end
    local want = motionSaved + v * Bl.motionStrength
    if abs((tonumber(mb.strength) or 0) - want) > 0.005 then
      local ok, err = pcall(function() mb.strength = want end)
      if not ok then report('E', 'motionBlurSet', 'cannot set PostFxMotionBlur.strength: ' .. tostring(err)) end
    end
    return
  end
  local p = ensureBlur()
  if not p then return end
  if not blurOn then
    p:enable()
    blurOn = true
    -- the streaks' shape (outer radius, the centre's limits), held by the PostEffect
    p:setShaderConst('$sonicBlurShape', string.format('%g %g %g 0', Bl.outer, Bl.centreMin, Bl.centreMax))
    blurSent.x = nil
  end
  local s = v * Bl.strength
  if blurSent.x ~= blurX or blurSent.y ~= blurY or blurSent.s == nil or abs(blurSent.s - s) > 1e-4 then
    p:setShaderConst('$sonicBlur', string.format('%g %g %g %g', blurX, blurY, s, Bl.sharp))
    blurSent.x, blurSent.y, blurSent.s = blurX, blurY, s
  end
end

local function updateBlur(dt)
  local Bl = E.blur
  local fast = max(0, min(1, (inputs.kmh - Bl.startKmh) / max(1, Bl.fullKmh - Bl.startKmh)))
  local want = min(1, fast * (inputs.boostKey and 1 or Bl.withoutBoost) + burst * Bl.burstShare)
  blurValue = blurValue + (want - blurValue) * min(1, dt * Bl.follow)
  -- his chest on screen, in units of tan(vertical fov / 2); kept while he is behind the camera
  local dx, dy, dz = originX - cam.x, originY - cam.y, originZ - cam.z
  local depth = dx * cam.fx + dy * cam.fy + dz * cam.fz
  if depth > 0.1 then
    local k = 1 / (depth * cam.tanHalf)
    blurX = floor((dx * cam.rx + dy * cam.ry + dz * cam.rz) * k * 1e4 + 0.5) / 1e4
    blurY = floor((dx * cam.ux + dy * cam.uy + dz * cam.uz) * k * 1e4 + 0.5) / 1e4
  end
  setBlur(blurValue * max(0, settings.blurStrength))
end

-- ---------------------------------------------------------------- Sonic's pose, for the sharp motion blur

-- the motion blur that keeps Sonic sharp (sonicnative_sharp), when loaded: he is given as he is drawn
-- now (pose nil: not on foot, it gives BeamNG's motion blur back)
local function sendPose(a)
  local sharp = loaded('sonicnative_sharp')
  if not (sharp and type(sharp.update) == 'function') then return end
  local P = nil
  if a then
    P = sharpPose
    P.ball = inputs.hidden
    P.ballX, P.ballY, P.ballZ = feetX, feetY, feetZ + (tonumber(a.ballPivot) or E.capsule.up)
    P.segments = bones and bones.segments or nil
    P.bonesOk = not inputs.hidden and P.segments ~= nil and sampleBones(a)
    P.boneX, P.boneY, P.boneZ, P.place = boneX, boneY, boneZ, place
    local u = a.up
    P.feetX, P.feetY, P.feetZ = feetX, feetY, feetZ
    P.upX, P.upY, P.upZ = u and tonumber(u.x) or 0, u and tonumber(u.y) or 0, u and tonumber(u.z) or 1
  end
  local ok, err = pcall(sharp.update, P)
  if not ok then report('E', 'sharp', 'the sharp motion blur failed: ' .. tostring(err)) end
end

-- ---------------------------------------------------------------- aura

local function setAuraMeshes(o, inner, halo)
  local m = auraDef.meshes
  for i = 1, #m do
    local show = (i == 1 and inner) or (i > 1 and halo)
    if #m == 1 then show = inner or halo end
    local ok, err = pcall(o.setMeshHidden, o, m[i], not show)
    if not ok then report('E', 'auraMesh', 'setMeshHidden failed on the aura mesh ' .. m[i] .. ': ' .. tostring(err)) end
  end
end

-- the base materials' glow at this brightness (method 'reload'), written as the converter writes
-- its ladder: pow(colour, gamma) x the layer's share x brightness (the base holds the opacity, 0.9)
local function reloadAura(brightness)
  local a = auraDef
  local c, g = a.colour, E.aura.gamma
  if reloadTimer then reloadTimer:stopAndReset() end
  local done = true
  for i = 1, #a.meshes do
    local mat = a.materials[i] and scenetree.findObject(a.materials[i])
    if not mat then done = false
    else
      local k = (a.alpha[i] or 1) * brightness
      local ok, err = pcall(function()
        mat:setField('emissiveFactor', 0, string.format('%.5f %.5f %.5f', c[1] ^ g * k, c[2] ^ g * k, c[3] ^ g * k))
        mat:flush()
        mat:reload()
      end)
      if not ok then done = false report('E', 'auraReload', 'aura material reload failed: ' .. tostring(err)) end
    end
  end
  if reloadTimer then aura.reloadMs = reloadTimer:stopAndReset() end
  aura.reloads, aura.reloadedTo = aura.reloads + 1, brightness
  return done
end

-- the method in use: the developer's choice, else the one chosen by itself; a method that cannot
-- work at all (no skin names, no colour) falls back to the steps
local function auraMethod()
  local m = dbg.auraMethod or auraAuto
  if (m == 'skin' and not auraDef.skins) or (m == 'reload' and not auraDef.colour) then m = 'steps' end
  return m
end

-- leaving a method: the skin back to Sonic's own, the base materials back to their converted glow
local function leaveMethod(o, method)
  if method == 'skin' and o and aura.skinSet then
    pcall(o.setSkinName, o, 'base')
    aura.skinSet = false
  end
  if method == 'reload' and aura.reloadedTo and aura.reloadedTo ~= E.aura.opacity and auraDef and auraDef.can.reload then
    reloadAura(E.aura.opacity)
  end
end

-- the aura's meshes and brightness for this amount: applied only when its level changes. The
-- converter's ladder lNN holds colour x layer share x alphas[NN] (the base: x opacity), so the
-- level is the amount's own; the steps compare the share of a full aura (amount / opacity)
local function applyAura(o, amount)
  if not (auraDef and o) then return end
  local method = auraMethod()
  local lvl = 0
  if amount > E.aura.minShown and method then
    if method == 'steps' then
      local share, st = amount / E.aura.opacity, E.aura.steps
      lvl = share < st[1] and 1 or (share < st[2] and 2 or 3)
    else
      lvl = level(amount)
    end
  end
  local id = o:getId()
  if id == aura.id and lvl == aura.level and method == aura.method then return end
  if id ~= aura.id then aura.skinSet = false end
  if aura.method and aura.method ~= method and id == aura.id then leaveMethod(o, aura.method) end
  aura.id, aura.level, aura.method = id, lvl, method
  if lvl == 0 then setAuraMeshes(o, false, false) return end
  if method == 'steps' then
    setAuraMeshes(o, lvl >= 2, lvl ~= 2)
  elseif method == 'skin' then
    setAuraMeshes(o, true, true)
    local ok, err = pcall(o.setSkinName, o, string.format(auraDef.skins, lvl))
    if ok then aura.skinSet = true else report('E', 'auraSkin', 'setSkinName failed: ' .. tostring(err)) end
  elseif method == 'reload' then
    setAuraMeshes(o, true, true)
    reloadAura(ladder.a[lvl])
    -- too slow for a change of level now and then: the next method (only when chosen by itself)
    if not dbg.auraMethod and aura.reloadMs and aura.reloadMs > E.aura.reloadMaxMs then
      report('I', 'auraSlow', string.format('aura material reload took %.2f ms: the aura switches to its next method', aura.reloadMs))
      local nextOne, past = nil, false
      for _, m in ipairs(E.aura.methods) do
        if past and auraDef.can[m] and not nextOne then nextOne = m end
        if m == 'reload' then past = true end
      end
      if nextOne then
        reloadAura(E.aura.opacity)
        auraAuto, aura.level = nextOne, nil
      end
    end
  end
end

local function updateAura(dt, o)
  local A = E.aura
  local rate = dt / max(0.01, inputs.lit and A.fadeIn or A.fadeOut)
  auraFade = inputs.lit and min(1, auraFade + rate) or max(0, auraFade - rate)
  -- off while only the ball shell shows: sonicnative_main hid his meshes, the aura's with them
  -- (it calls M.onSonicMeshesShown when it shows them again)
  auraAmount = inputs.hidden and 0 or auraFade * A.opacity * inputs.fxSpeed * max(0, settings.auraIntensity) * (1 - inputs.ball)
  applyAura(o, auraAmount)
end

-- ---------------------------------------------------------------- light

local function lightOff()
  if lightOn == false then return end
  local l = scenetree.findObject(LIGHT_NAME)
  if l then
    pcall(function()
      l.intensity = 0
      l.isEnabled = false
      l:postApply()
    end)
  end
  lightOn, lightIntensity = false, 0
end

local function ensureLight()
  local l = scenetree.findObject(LIGHT_NAME)
  if l then return l end
  local Li = E.light
  l = createObject('PointLight')
  if not l then return nil end
  l.canSave = false
  l.radius = Li.radius
  l.intensity = 0
  l.isEnabled = false
  l:setField('color', 0, string.format('%g %g %g 1', Li.colour[1], Li.colour[2], Li.colour[3]))
  l:setField('castShadows', 0, Li.castShadows and '1' or '0')
  l:registerObject(LIGHT_NAME)
  addToGroup(l)
  lightOn = false
  return scenetree.findObject(LIGHT_NAME) and l or nil
end

local function updateLight()
  local want = E.light.peak * burst * burst * max(0, settings.boostLight)
  if not (want > 1) then lightOff() return end
  local l = ensureLight()
  if not l then return end
  l:setPosRot(glowX, glowY, glowZ, 0, 0, 0, 1)
  if lightOn ~= true or abs(want - lightIntensity) > 1 then
    l.intensity = want
    l.isEnabled = true
    l:postApply()
    lightOn, lightIntensity = true, want
  end
end

-- ---------------------------------------------------------------- frame

-- everything off and forgotten (not on foot, in a car, no Sonic); his aura meshes hidden when he is given
local function hideAll(o)
  if o and auraDef then applyAura(o, 0) end
  if not shown then return end
  shown = false
  clearTrail()
  clearBolts()
  auraFade, auraAmount, blurValue, trailOn, trailActive = 0, 0, 0, 0, false
  boltDebt, bodyDebt, burst, burstAt = 0, 0, 0, -100
  wasBoosting, hitSeen, lastHitClock = false, false, nil
  hideMesh(trailB)
  hideMesh(spriteB)
  blurOff()
  sendPose(nil)
  lightOff()
end

local BOOST_STATES = E.common.boostStates

local function readInputs(st, a, body)
  local kmh = tonumber(a.speedKmh) or tonumber(st.speedKmh) or 0
  if dbg.forceKmh then kmh = dbg.forceKmh end
  local ref = E.common.refKmhNoCap
  local bs = type(body.getSettings) == 'function' and body.getSettings() or nil
  if type(bs) == 'table' and bs.speedCapOn and tonumber(bs.speedCapKmh) then ref = max(E.common.refKmhMin, bs.speedCapKmh) end
  local ratio = max(0, min(1, kmh / max(1, ref)))
  local amount = settings.fxBySpeed and max(0, min(1, settings.fxSpeedAmount)) or 0
  inputs.kmh, inputs.fxRatio, inputs.fxSpeed = kmh, ratio, 1 - amount * (1 - ratio)
  inputs.state = a.state
  inputs.boostKey = st.boosting == true or dbg.forceBoost
  inputs.boosting = BOOST_STATES[a.state] == true or dbg.forceBoost
  inputs.lit = inputs.boosting or (inputs.boostKey and kmh > E.aura.litKmh) or a.state == E.common.fastState
  inputs.ball = max(0, min(1, tonumber(a.ballAmount) or 0))
  inputs.hidden = a.ball == true   -- only the ball shell shows: Sonic's meshes are hidden
  local f = a.feet
  feetX, feetY, feetZ = tonumber(f.x) or 0, tonumber(f.y) or 0, tonumber(f.z) or 0
  originX, originY, originZ = feetX, feetY, feetZ + E.common.chest
  local p = a.place
  place.x, place.y, place.z = tonumber(p.x) or feetX, tonumber(p.y) or feetY, tonumber(p.z) or feetZ
  place.qx, place.qy, place.qz, place.qw = tonumber(p.qx) or 0, tonumber(p.qy) or 0, tonumber(p.qz) or 0, tonumber(p.qw) or 1
  place.scale = tonumber(p.scale) or 1
end

-- a boost start (boost or drift state begins) and a car hit (sonicnative_body's last hit changed)
local function events(body)
  if inputs.boosting and not wasBoosting then startBurst(false) end
  wasBoosting = inputs.boosting
  local hit = type(body.getLastHit) == 'function' and body.getLastHit() or nil
  local at = type(hit) == 'table' and tonumber(hit.clock) or nil
  if hitSeen and at and at ~= lastHitClock then startBurst(true) end
  lastHitClock, hitSeen = at, true
  if dbg.burst then dbg.burst = false startBurst(false) end
  local age = clock - burstAt
  burst = age < E.burst.seconds and 1 - age / E.burst.seconds or 0
  local follow = E.burst.follow
  glowX = burstX + (originX - burstX) * follow
  glowY = burstY + (originY - burstY) * follow
  glowZ = burstZ + (originZ - burstZ) * follow
end

-- developer view: a sphere on each tracked bone (ColorF and debugDrawer exist in GE only)
local function drawBones(a)
  if not (debugDrawer and ColorF and sampleBones(a)) then return end
  local colour = ColorF(1, 0.3, 0.1, 0.9)
  for b = 1, bones.count do
    debugDrawer:drawSphere(vec3(boneX[b], boneY[b], boneZ[b]), 0.025, colour)
  end
end

local function frame(dt, o)
  if not o then hideAll(nil) return end
  ensureReady()                   -- once a map: the manifest (its aura meshes) and the materials
  if dbg.off then hideAll(o) return end
  local body = loaded('sonicnative_body')
  local st = body and type(body.getState) == 'function' and body.getState() or nil
  if not (type(st) == 'table' and st.active) then hideAll(o) return end
  local main = loaded('sonicnative_main')
  local a = main and type(main.getAnim) == 'function' and main.getAnim() or nil
  if type(a) ~= 'table' or a.feet == nil or type(a.place) ~= 'table' then hideAll(o) return end
  if dbg.freeze then return end
  if not readCamera() then hideAll(o) return end
  frameNo = frameNo + 1
  clock = clock + dt
  shown = true
  readInputs(st, a, body)
  events(body)
  updateTrail(dt, a)
  buildTrail()
  updateBolts(dt, a)
  buildSprites(a)
  updateBlur(dt)
  updateAura(dt, o)
  updateLight()
  submit(trailB)
  submit(spriteB)
  if dbg.bones then drawBones(a) end
  -- last: an error above reaches hideAll (sendPose(nil)) before the sharp blur was turned on this frame
  sendPose(a)
end

-- ---------------------------------------------------------------- lifecycle

local function cleanup()
  local o = aura.id and scenetree.findObjectById and scenetree.findObjectById(aura.id) or nil
  if o and auraDef then
    pcall(setAuraMeshes, o, false, false)
    if aura.skinSet then pcall(o.setSkinName, o, 'base') end
  end
  if aura.reloadedTo and aura.reloadedTo ~= E.aura.opacity and auraDef and auraDef.can.reload then pcall(reloadAura, E.aura.opacity) end
  restoreMotion()
  local p = scenetree.findObject(BLUR_NAME)
  if p then pcall(p.disable, p) pcall(p.delete, p) end
  deleteNamed(BLUR_SHADER_NAME)
  deleteNamed(GROUP_NAME)
  -- strays outside the group (an earlier version, the editor)
  deleteNamed(TRAIL_NAME)
  deleteNamed(SPRITES_NAME)
  deleteNamed(LIGHT_NAME)
  shown = true
  hideAll(nil)
  shown = false
  trailB.hidden, spriteB.hidden = nil, nil
  blurOn, lightOn = nil, nil
  aura.id, aura.level, aura.method, aura.skinSet, aura.reloadedTo = nil, nil, nil, false, nil
end

local function onPreRender()
  -- sonicnative_main stopped calling (unloaded, failing): nothing stays on screen, the aura's
  -- meshes included on the Sonic it last lit (he may still be drawn)
  if not ranSinceTick and shown then
    local o = auraDef and aura.id and aura.level ~= 0 and scenetree.findObjectById and scenetree.findObjectById(aura.id) or nil
    local ok, err = pcall(hideAll, o)
    if not ok then report('E', 'hide', 'hiding the boost effects failed: ' .. tostring(err)) end
  end
  ranSinceTick = false
end

local function onExtensionLoaded()
  setExtensionUnloadMode(M, 'manual')
  readManifest()
end

local function onClientPostStartMission()
  defRead, materialsChecked = false, false   -- the converter may have run since the last map
  aura.id, aura.level = nil, nil
end

local function onClientEndMission()
  cleanup()
  materialsChecked = false
end

local function onModDeactivated(mod)
  if type(mod) ~= 'table' or mod.modname ~= 'sonicnative' then return end
  cleanup()
  -- off the manual list, as sonicnative_main (modScript.lua loads it again with the mod)
  setExtensionUnloadMode('sonicnative_fx', 'auto')
  extensions.unload('sonicnative_fx')
end

-- ---------------------------------------------------------------- public API

-- All per-frame work, called by sonicnative_main at the end of its frame with its step (s) and
-- Sonic's StaticShape; o == nil: everything hidden
M.afterSonic = function(dt, o)
  ranSinceTick = true
  if frameTimer then frameTimer:stopAndReset() end
  local ok, err = pcall(frame, tonumber(dt) or 0, o)
  if not ok then
    report('E', 'frame', 'boost effects failed: ' .. tostring(err))
    shown = true
    pcall(hideAll, o)
  end
  if frameTimer then
    local t = tonumber(frameTimer:stopAndReset()) or 0
    ms.last, ms.max, ms.avg = t, max(ms.max, t), ms.avg + (t - ms.avg) * 0.05
  end
end

-- sonicnative_main showed all of Sonic's meshes again (setAllMeshesHidden(false)): the aura's
-- meshes go back to what they were
M.onSonicMeshesShown = function(o)
  if not (o and auraDef) then return end
  aura.level = nil
  local ok, err = pcall(applyAura, o, shown and auraAmount or 0)
  if not ok then report('E', 'auraShown', 'aura after showing Sonic failed: ' .. tostring(err)) end
end

-- changes settings live (same keys and types as effects.lua settings; others are ignored):
-- sonicnative_menu (the Lab's Boost and Aura tabs), and developer tests
M.setSettings = function(values)
  for k, v in pairs(type(values) == 'table' and values or {}) do
    if settings[k] ~= nil and type(v) == type(settings[k]) and v == v then settings[k] = v end
  end
  return settings
end

M.getSettings = function() return settings end


local function kindsStatus()
  local out = {}
  for name, k in pairs(kinds) do out[name] = {material = k.base, levels = k.count, grid = {k.gx, k.gy}} end
  return out
end

-- Developer API, for the MCP run_lua tool: return jsonEncode(sonicnative_fx.status())
M.status = function()
  local draws = nil
  local render = Engine and Engine.Render
  if render and render.getDrawCallCount then
    local ok, n = pcall(render.getDrawCallCount)
    if ok then draws = n end
  end
  local backend = dbg.blurBackend
  if (backend == 'compute' or backend == 'overlay') and not scenetree.findObject(BLUR_NAME) then backend = backend .. ' (not created)' end
  return {
    inputs = {kmh = inputs.kmh, fxSpeed = inputs.fxSpeed, fxRatio = inputs.fxRatio, boosting = inputs.boosting,
      boostKey = inputs.boostKey, lit = inputs.lit, state = inputs.state, ball = inputs.ball, hidden = inputs.hidden},
    blur = {backend = backend, value = blurValue * max(0, settings.blurStrength), on = blurOn, centre = {blurX, blurY},
      motionSaved = motionSaved},
    aura = {backend = auraDef and auraMethod() or 'none (no aura in the manifest)', amount = auraAmount, level = aura.level, fade = auraFade,
      can = auraDef and auraDef.can or nil, meshes = auraDef and auraDef.meshes or nil,
      reloadMs = aura.reloadMs, reloads = aura.reloads},
    trail = {points = nPts, path = pathN, quads = trailB.quads, parts = trailB.parts, on = trailOn, keep = keep,
      created = trailB.created},
    sprites = {count = spriteCount, bolts = nBolts, bodyBolts = bodyBoltsAlive, quads = spriteB.quads, parts = spriteB.parts,
      created = spriteB.created},
    bones = bones and {file = bones.file, count = bones.count, clips = bones.clipCount, onClip = boneOk} or false,
    light = {on = lightOn == true, intensity = lightIntensity},
    burst = burst,
    ms = {last = ms.last, max = ms.max, avg = ms.avg},
    drawCalls = draws,
    manifest = {fx = fxDef ~= nil, version = fxDef and fxDef.version or nil, materials = fxDef and fxDef.materials or nil,
      ladder = {levels = ladder.n, from = ladder.from, first = ladder.a[1], last = ladder.a[ladder.n]}, kinds = kindsStatus()},
    shown = shown, clock = clock, settings = settings,
    debug = {forceKmh = dbg.forceKmh, forceBoost = dbg.forceBoost, freeze = dbg.freeze, bones = dbg.bones, off = dbg.off,
      auraMethod = dbg.auraMethod, blurBackend = dbg.blurBackend, meshMode = dbg.meshMode},
    errors = errors,
  }
end

-- Developer switches; a key left out keeps its value, false clears it:
-- {forceKmh = 600, forceBoost = true, freeze = true, bones = true (debugDrawer spheres), burst = true (one now),
--  auraMethod = 'reload' | 'skin' | 'steps', blurBackend = 'compute' | 'overlay' | 'motion' | 'off',
--  meshMode = 'replace' | 'recreate', off = true, reload = true (manifest read again), resetStats = true}
M.debug = function(opts)
  if type(opts) ~= 'table' then return M.status() end
  if opts.forceKmh ~= nil then dbg.forceKmh = tonumber(opts.forceKmh) end
  if opts.forceBoost ~= nil then dbg.forceBoost = opts.forceBoost == true end
  if opts.freeze ~= nil then dbg.freeze = opts.freeze == true end
  if opts.bones ~= nil then dbg.bones = opts.bones == true end
  if opts.burst then dbg.burst = true end
  if opts.off ~= nil then dbg.off = opts.off == true end
  if opts.auraMethod ~= nil then
    local m = AURA_METHODS[opts.auraMethod] and opts.auraMethod or nil
    if m ~= dbg.auraMethod then dbg.auraMethod, aura.level = m, nil end
  end
  if opts.blurBackend ~= nil and BLUR_BACKENDS[opts.blurBackend] ~= nil and opts.blurBackend ~= dbg.blurBackend then
    -- another bin needs another PostEffect
    blurOff()
    local p = scenetree.findObject(BLUR_NAME)
    if p then pcall(p.delete, p) end
    blurOn = nil
    dbg.blurBackend = opts.blurBackend
  end
  if opts.meshMode == 'replace' or opts.meshMode == 'recreate' then dbg.meshMode = opts.meshMode end
  if opts.reload then
    warned = {}
    readManifest()
    aura.level = nil
  end
  if opts.resetStats then ms.max = 0 end
  return M.status()
end

M.onExtensionLoaded = onExtensionLoaded
M.onExtensionUnloaded = cleanup
M.onClientPostStartMission = onClientPostStartMission
M.onClientEndMission = onClientEndMission
M.onModDeactivated = onModDeactivated
M.onPreRender = onPreRender

return M
