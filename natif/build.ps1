# Builds Sonic Beam's native mode (natif/): Lua check -> converter (-Converter) -> setup (-Setup) -> install (-Install).
# No DLL. The converter's output (Sonic from the player's own Frontiers) goes to the BeamNG user folder only,
# never into the repo, the mod or the setup.
#   -Converter        also builds the converter (natif\build-converter.ps1)
#   -Setup            also builds the converter, then the player's setup natif\build\SonicBeam-Setup-<version>.exe
#                     (natif\build-setup.ps1: natif\mod and the converter in one exe)
#   -Install          mirrors natif\mod into beamng\devuser\current\mods\unpacked\sonicnative
#   -Install -Normal  same into %LOCALAPPDATA%\BeamNG\BeamNG.drive\current\mods\unpacked\sonicnative
param([switch]$Converter, [switch]$Setup, [switch]$Install, [switch]$Normal)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$repo = Split-Path -Parent $root
# Version: natif/VERSION (major.minor.patch), the same as the git tag natif-vX.Y.Z
$Version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "natif\VERSION '$Version' is not major.minor.patch" }
Write-Output "Sonic Beam natif $Version"

# Lua check: syntax, no local read as a global by mistake and no global written by mistake
# (BeamNG would only fail at run time). Every global the mod may read is listed here.
$luac = Join-Path $repo '.tools\luac51.exe'
$luaGlobals = @(
  # Lua 5.1 / LuaJIT
  'assert','error','getmetatable','ipairs','math','next','pairs','pcall','rawget','rawset','require','select',
  'setmetatable','string','table','tonumber','tostring','type','unpack','xpcall',
  # BeamNG, both Lua VMs
  'clamp','deepcopy','dumps','extensions','jsonDecode','jsonEncode','jsonReadFile','jsonWriteFile','log','M',
  'quat','quatFromDir','serialize','setExtensionUnloadMode','vec3',
  # BeamNG game engine (GE) Lua
  'be','castRayStatic','ColorF','commands','core_camera','core_gamestate','core_input_actionFilter','createObject','debugDrawer','Engine','FS',
  'gameplay_missions_missionManager','gameplay_walk','getAllVehicles','getPlayerVehicle','guihooks','hptimer',
  'loadJsonMaterialsFile','readFile','scenetree','simTimeAuthority','spawn','TorqueScript','ui_imgui','ui_message',
  # BeamNG vehicle Lua
  'controller','electrics','enablePhysicsStepHook','input','MoveManager','obj','sensors','v'
)
$luaFiles = @(Get-ChildItem -Recurse (Join-Path $root 'mod') -Filter *.lua)
foreach ($lua in $luaFiles) {
    $name = $lua.FullName.Substring($root.Length + 1)
    # luac writes its errors to stderr: read them as text, not as a PowerShell error
    $listing = & { $ErrorActionPreference = 'Continue'; & $luac -l -p $lua.FullName 2>&1 } | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "Lua syntax error in ${name}: $($listing -join ' ')" }
    $read = $listing | Select-String 'GETGLOBAL.*; (\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique
    $unknown = $read | Where-Object { $_ -cnotin $luaGlobals }
    if ($unknown) { throw "Lua $name reads unknown globals: $($unknown -join ', ')" }
    $written = $listing | Select-String 'SETGLOBAL.*; (\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique
    if ($written) { throw "Lua $name writes globals (missing 'local'?): $($written -join ', ')" }
}
Write-Output ("Lua check passed ({0} files)" -f $luaFiles.Count)

if ($Converter -or $Setup) {
    $converterBuild = Join-Path $root 'build-converter.ps1'
    if (Test-Path $converterBuild) {
        $global:LASTEXITCODE = 0
        & $converterBuild
        if ($LASTEXITCODE -ne 0) { throw "build-converter.ps1 failed (exit code $LASTEXITCODE)" }
    } else {
        Write-Warning 'natif\build-converter.ps1 does not exist yet: converter not built'
    }
}

if ($Setup) {
    # the standalone converter above still builds with its wmain; the setup links the same main.cpp without it
    $global:LASTEXITCODE = 0
    & (Join-Path $root 'build-setup.ps1')
    if ($LASTEXITCODE -ne 0) { throw "build-setup.ps1 failed (exit code $LASTEXITCODE)" }
}

if ($Normal -and -not $Install) { Write-Warning '-Normal only picks the folder for -Install: nothing installed' }
if ($Install) {
    # dev: the Steam launch's -userpath; normal: the folder other launchers (BeamNG.Earth) use
    $userFolder = if ($Normal) { Join-Path $env:LOCALAPPDATA 'BeamNG\BeamNG.drive\current' } else { Join-Path $repo 'beamng\devuser\current' }
    if (-not (Test-Path $userFolder)) { throw "BeamNG user folder not found: $userFolder" }
    if (Get-Process -Name 'BeamNG.drive.x64' -ErrorAction SilentlyContinue) {
        if ($Normal) { throw 'BeamNG.drive is running: close it before installing into the normal user folder' }
        Write-Warning 'BeamNG.drive is running: it reads the new copy at its next Lua reload'
    }
    if (Test-Path (Join-Path $userFolder 'mods\unpacked\sonicfrontiers_sonic')) {
        Write-Warning "the overlay mod (sonicfrontiers_sonic) is also installed in $userFolder : keep only one mode active"
    }
    $unpacked = Join-Path $userFolder 'mods\unpacked'
    $target = Join-Path $unpacked 'sonicnative'
    # a mirror updated in place: files still in natif\mod are overwritten, never deleted first. A running
    # game watches the folder; a camera mode deleted even for a moment makes core/camera.lua rebuild its
    # camera list without it, and Sonic's camera stays active but frozen until the next file change
    $source = Join-Path $root 'mod'
    New-Item -ItemType Directory -Force $target | Out-Null
    foreach ($file in Get-ChildItem -Recurse -File $source) {
        $dest = Join-Path $target $file.FullName.Substring($source.Length + 1)
        New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
        Copy-Item -Force $file.FullName $dest
    }
    foreach ($file in Get-ChildItem -Recurse -File $target) {
        if (-not (Test-Path (Join-Path $source $file.FullName.Substring($target.Length + 1)))) { Remove-Item -Force $file.FullName }
    }
    $count = @(Get-ChildItem -Recurse -File $target).Count
    Write-Output ("installed {0} ({1} files)" -f $target, $count)
}
