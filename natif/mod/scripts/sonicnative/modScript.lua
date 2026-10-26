-- SPDX-License-Identifier: MIT
-- Sonic Beam, native mode: runs when the sonicnative mod is active. Loads Sonic's scene object
-- (sonicnative_main), then his body (sonicnative_body), his sounds (sonicnative_sound), his boost
-- effects (sonicnative_fx) and the O menu (sonicnative_menu, which loads the player's settings)
-- when those files are there.
-- The unload mode is set before each load: initDB warns about an extension loaded without it.
setExtensionUnloadMode('sonicnative_main', 'manual')
extensions.load('sonicnative_main')

local bodyFile = '/lua/ge/extensions/sonicnative/body.lua'
if FS:fileExists(bodyFile) then
  setExtensionUnloadMode('sonicnative_body', 'manual')
  local ok, err = pcall(extensions.load, 'sonicnative_body')
  if not ok then log('E', 'sonicnative', 'cannot load sonicnative_body: ' .. tostring(err)) end
else
  log('W', 'sonicnative', bodyFile .. ' not found: Sonic is shown without his body')
end

-- his sounds, after the body they listen to
local soundFile = '/lua/ge/extensions/sonicnative/sound.lua'
if FS:fileExists(soundFile) then
  setExtensionUnloadMode('sonicnative_sound', 'manual')
  local ok, err = pcall(extensions.load, 'sonicnative_sound')
  if not ok then log('E', 'sonicnative', 'cannot load sonicnative_sound: ' .. tostring(err)) end
else
  log('W', 'sonicnative', soundFile .. ' not found: Sonic is silent')
end

-- his boost effects (speed blur, aura, lightning, trail, flash), after Sonic they are drawn around
local fxFile = '/lua/ge/extensions/sonicnative/fx.lua'
if FS:fileExists(fxFile) then
  setExtensionUnloadMode('sonicnative_fx', 'manual')
  local ok, err = pcall(extensions.load, 'sonicnative_fx')
  if not ok then log('E', 'sonicnative', 'cannot load sonicnative_fx: ' .. tostring(err)) end
else
  log('W', 'sonicnative', fxFile .. ' not found: Sonic has no boost effects')
end

-- the O menu, the Lab and Unstuck, last: it hands the player's saved settings to the others
local menuFile = '/lua/ge/extensions/sonicnative/menu.lua'
if FS:fileExists(menuFile) then
  setExtensionUnloadMode('sonicnative_menu', 'manual')
  local ok, err = pcall(extensions.load, 'sonicnative_menu')
  if not ok then log('E', 'sonicnative', 'cannot load sonicnative_menu: ' .. tostring(err)) end
else
  log('W', 'sonicnative', menuFile .. ' not found: no O menu, Sonic keeps the default settings')
end
