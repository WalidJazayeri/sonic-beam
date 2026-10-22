-- SPDX-License-Identifier: MIT
-- Loaded by the Sonic Beam mod into a vehicle Sonic comes close to (traffic, parked cars, the
-- player's own): it tells the mod the vehicle's mass, and when Sonic hits it, it pushes the whole
-- body with the momentum he hands over, harder around the contact point so the panels crush.
local M = {}

local nodes, total = {}, 0      -- {cid, kg}, vehicle mass
local hits = {}                 -- {forces = {{cid, force}}, left = seconds}
local DURATION = 0.015          -- seconds over which a hit's momentum is handed over (short: a blow)
local RADIUS = 1.2              -- metres around the contact point that crush

local function onExtensionLoaded()
  for _, n in pairs(v.data.nodes) do
    local w = n.nodeWeight or 0
    if w > 0 then
      nodes[#nodes + 1] = {n.cid, w}
      total = total + w
    end
  end
  enablePhysicsStepHook()
  obj:queueGameEngineLua(string.format('if sonicfrontiers_sonic then sonicfrontiers_sonic.setCarMass(%d, %.1f) end', obj:getId(), total))
end

-- Sonic hit this vehicle at (px, py, pz): its body gains (dvx, dvy, dvz) on average; the nodes
-- near the contact get more of it (damage: 0 = an even push, 1 = default crush)
local function hit(px, py, pz, dvx, dvy, dvz, damage)
  if total <= 0 then return end
  local origin = obj:getPosition()
  local contact = vec3(px, py, pz)
  local dv = vec3(dvx, dvy, dvz)
  local crush = 1 + 4 * math.max(0, damage or 1)
  local near, nearMass = {}, 0
  for i, n in ipairs(nodes) do
    if (origin + obj:getNodePosition(n[1]) - contact):length() < RADIUS then
      near[i] = true
      nearMass = nearMass + n[2]
    end
  end
  -- the rest of the body gets what keeps the total momentum at mass * dv
  local rest = total - nearMass
  local others = rest > 0 and math.max(0.2, (total - nearMass * crush) / rest) or 1
  local forces = {}
  for i, n in ipairs(nodes) do
    forces[#forces + 1] = {n[1], dv * (n[2] * (near[i] and crush or others) / DURATION)}
  end
  hits[#hits + 1] = {forces = forces, left = DURATION}
end

local function onPhysicsStep(dt)
  for i = #hits, 1, -1 do
    local h = hits[i]
    for _, f in ipairs(h.forces) do obj:applyForceVector(f[1], f[2]) end
    h.left = h.left - dt
    if h.left <= 0 then table.remove(hits, i) end
  end
end

M.onExtensionLoaded = onExtensionLoaded
M.onPhysicsStep = onPhysicsStep
M.hit = hit
M.getMass = function() return total end

return M
