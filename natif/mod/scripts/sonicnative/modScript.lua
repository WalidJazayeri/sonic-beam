-- SPDX-License-Identifier: MIT
-- Sonic Beam, native mode: runs when the sonicnative mod is active. Loads Sonic's scene object
-- (sonicnative_main), then his body (sonicnative_body), his sounds (sonicnative_sound), his boost
-- effects (sonicnative_fx), the motion blur that keeps him sharp (sonicnative_sharp) and the O menu
-- (sonicnative_menu, which loads the player's settings) when those files are there.
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

-- the motion blur that keeps Sonic sharp, fed his pose by the boost effects
local sharpFile = '/lua/ge/extensions/sonicnative/sharp.lua'
local sharpLoaded = false
if FS:fileExists(sharpFile) then
  setExtensionUnloadMode('sonicnative_sharp', 'manual')
  local ok, err = pcall(extensions.load, 'sonicnative_sharp')
  if not ok then log('E', 'sonicnative', 'cannot load sonicnative_sharp: ' .. tostring(err)) end
  sharpLoaded = ok and extensions.isExtensionLoaded('sonicnative_sharp')
else
  log('W', 'sonicnative', sharpFile .. ' not found: BeamNG\'s motion blur smears Sonic')
end
if not sharpLoaded then
  -- what it may have left from before a Lua reload: its passes, and BeamNG's motion blur in the bin
  -- that never draws (sharp.lua NO_BIN), put back in its own as enabled as it is
  pcall(function()
    for _, name in ipairs({'sonicnative_sharpBlurGather', 'sonicnative_sharpBlurNeighbor', 'sonicnative_sharpBlurTile',
        'sonicnative_sharpBlurTileX', 'sonicnative_sharpBlur'}) do
      local o = scenetree.findObject(name)
      if o then o:delete() end
    end
    local mb = scenetree.findObject('PostFxMotionBlur')
    if mb and mb:getField('renderBin', 0) == 'sonicnativeNoBin' then
      local enabled = mb:isEnabled()
      if enabled then mb:disable() end
      mb:setField('renderBin', 0, 'OverlayRender')
      if enabled then mb:enable() end
    end
  end)
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
