# Builds the native-mode converter: natif/converter -> natif/build/sonicnative_convert.exe (no D3D, no DLL).
# Same toolchain and flags as beamng/build.ps1; the Frontiers readers in src/frontiers are shared.
param([string]$Version = '')
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$repo = Split-Path -Parent $root
$zig = Join-Path $repo '.tools\zig-x86_64-windows-0.15.2\zig.exe'
$acl = Join-Path $repo '.tools\PXDAnimationTools-26fd823adcfba9d053b3ad0acac38aa0ae2086a3\FrontiersAnimDecompress\includes'
# Version: natif/VERSION (major.minor.patch), written into the manifest and the DAE
if (-not $Version) { $Version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim() }
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version '$Version' is not major.minor.patch" }
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null

$sources = @((Join-Path $root 'converter\main.cpp'), (Join-Path $repo 'src\frontiers\anim_acl.cpp'))
$exe = Join-Path $out 'sonicnative_convert.exe'
$log = Join-Path $out 'convert-compile.log'
& $zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -municode -fno-sanitize=undefined `
    -Wno-missing-template-arg-list-after-template-kw -Wno-unused-command-line-argument `
    "-DSONICNATIVE_VERSION=$Version" -I $acl @sources -o $exe 2>&1 | Out-File -Encoding utf8 $log
if ($LASTEXITCODE -ne 0) { Get-Content $log | Select-String 'error' | Select-Object -First 40 | ForEach-Object Line; throw 'Converter compilation failed' }
Get-Content $log | Select-String 'warning' | Select-Object -First 20 | ForEach-Object Line
Write-Output ("built {0} ({1} bytes)" -f $exe, (Get-Item $exe).Length)
