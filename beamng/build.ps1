# Builds Sonic Beam: sheet preflight -> generated code -> sonic_beamng.dll -> release zip.
param([string]$Version = '', [switch]$NoPackage, [switch]$Dev)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$repo = Split-Path -Parent $root
$zig = Join-Path $repo '.tools\zig-x86_64-windows-0.15.2\zig.exe'
$acl = Join-Path $repo '.tools\PXDAnimationTools-26fd823adcfba9d053b3ad0acac38aa0ae2086a3\FrontiersAnimDecompress\includes'
# Version: beamng/VERSION (semantic versioning: major.minor.patch), the same as the git tag vX.Y.Z
if (-not $Version) { $Version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim() }
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version '$Version' is not major.minor.patch" }
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null

$genArgs = @((Join-Path $root 'tools\gen_sheets.py'))
if ($Dev) { $genArgs += '--dev' }
& python -I @genArgs
if ($LASTEXITCODE -ne 0) { throw 'Sheet preflight failed: fix the sheets before building.' }

# Lua check: syntax, and no local read as a global by mistake (BeamNG would only fail at run time)
$luac = Join-Path $repo '.tools\luac51.exe'
$luaGlobals = @('be','castRayStatic','clamp','controller','core_camera','core_gamestate','gameplay_missions_missionManager','deepcopy','enablePhysicsStepHook','extensions','gameplay_walk',
  'getPlayerVehicle','ipairs','jsonReadFile','jsonWriteFile','log','math','MoveManager','obj','pairs','particles','quatFromDir','require','serialize',
  'setExtensionUnloadMode','setmetatable','socket','spawn','string','table','tostring','type','ui_imgui','v','vec3')
foreach ($lua in Get-ChildItem -Recurse (Join-Path $root 'mod') -Filter *.lua) {
    $listing = & $luac -l -p $lua.FullName 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Lua syntax error: $listing" }
    $unknown = $listing | Select-String 'GETGLOBAL.*; (\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique | Where-Object { $_ -notin $luaGlobals }
    if ($unknown) { throw "Lua $($lua.Name) reads unknown globals: $($unknown -join ', ')" }
}

& $zig cc -target x86_64-windows-gnu -O2 -c (Join-Path $repo 'src\third_party\clhca\clhca.c') -o (Join-Path $out 'clhca.o')
if ($LASTEXITCODE -ne 0) { throw 'clHCA compilation failed' }

$sources = 'src\main.cpp','src\assets.cpp','src\audio.cpp','src\game.cpp','src\render11.cpp','src\render12.cpp','src\testmode.cpp','src\proxy.cpp','src\link.cpp' | ForEach-Object { Join-Path $root $_ }
$sources += Join-Path $repo 'src\frontiers\anim_acl.cpp'
$dll = Join-Path $out 'version.dll'
$log = Join-Path $out 'compile.log'
& $zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -shared -fno-sanitize=undefined `
    -Wno-missing-template-arg-list-after-template-kw -Wno-unused-command-line-argument `
    "-DSB_VERSION_NUMBER=$Version" -I $acl @sources (Join-Path $out 'clhca.o') (Join-Path $root 'src\version.def') `
    -o $dll -ld3d11 -ld3d12 -ldxgi -lws2_32 -ld3dcompiler_47 -ldxguid -luser32 -lole32 2>&1 | Out-File -Encoding utf8 $log
if ($LASTEXITCODE -ne 0) { Get-Content $log | Select-String 'error' | Select-Object -First 40 | ForEach-Object Line; throw 'Renderer compilation failed' }
# HLSL check: the renderers compile their shaders inside BeamNG; catch mistakes here instead
& $zig c++ -target x86_64-windows-gnu -std=c++17 -O1 -I (Join-Path $root 'src') (Join-Path $root 'tools\hlslcheck.cpp') -o (Join-Path $out 'hlslcheck.exe') -ld3dcompiler_47 2>&1 | Out-File -Encoding utf8 (Join-Path $out 'hlslcheck.log')
if ($LASTEXITCODE -ne 0) { throw 'HLSL check could not be built' }
& (Join-Path $out 'hlslcheck.exe')
if ($LASTEXITCODE -ne 0) { throw 'HLSL shaders do not compile' }
Write-Output ("built {0} ({1} bytes)" -f $dll, (Get-Item $dll).Length)

if ($NoPackage) { return }
# Package layout: mod/ goes to BeamNG's user mods folder as an unpacked mod (sonicfrontiers_sonic).
$stage = Join-Path $out 'package'
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
$modOut = Join-Path $stage 'sonicfrontiers_sonic'
Copy-Item -Recurse (Join-Path $root 'mod') $modOut
$binOut = Join-Path $stage 'bin64'
New-Item -ItemType Directory -Force (Join-Path $binOut 'SonicBeam\licenses') | Out-Null
Copy-Item $dll (Join-Path $binOut 'version.dll')
Get-ChildItem (Join-Path $repo 'licenses') | Where-Object Name -notmatch 'Streamline' | Copy-Item -Destination (Join-Path $binOut 'SonicBeam\licenses')
Copy-Item (Join-Path $repo 'LICENSE') (Join-Path $binOut 'SonicBeam\LICENSE.txt')
$zip = Join-Path $out "SonicBeam-$Version.zip"
Remove-Item -Force $zip -ErrorAction SilentlyContinue
& python -I -c "import sys,os,zipfile;r=sys.argv[1];z=zipfile.ZipFile(sys.argv[2],'w',zipfile.ZIP_DEFLATED)
for d,_,fs in os.walk(r):
  for f in sorted(fs):
    p=os.path.join(d,f);z.write(p,os.path.relpath(p,r).replace(os.sep,'/'))
z.close()" $stage $zip
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output ("package {0} ({1} bytes) sha256 {2}" -f $zip, (Get-Item $zip).Length, $hash)
