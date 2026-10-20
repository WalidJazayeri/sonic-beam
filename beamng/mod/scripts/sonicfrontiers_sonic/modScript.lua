-- SPDX-License-Identifier: MIT
-- Sonic Beam: runs when the mod is active and loads the Sonic extension.
local function loadSonicExtension()
  extensions.load('sonicfrontiers_sonic')
  setExtensionUnloadMode('sonicfrontiers_sonic', 'manual')
end
loadSonicExtension()
