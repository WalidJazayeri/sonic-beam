-- SPDX-License-Identifier: MIT
-- Sonic Beam, native mode: runs when the sonicnative mod is active. Loads Sonic's scene object
-- (sonicnative_main), then his body (sonicnative_body) and his sounds (sonicnative_sound) when
-- those files are there.
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
