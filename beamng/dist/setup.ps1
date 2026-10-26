# SPDX-License-Identifier: MIT
# Sonic Beam installer / uninstaller (run through install.bat and uninstall.bat).
# Install: finds BeamNG.drive and Sonic Frontiers (Steam libraries, or asks), backs up every file it
# would replace, copies the mod and writes %LOCALAPPDATA%\SonicBeam\install.json.
# Uninstall: removes what install.json lists and puts the backed-up files back.
param([switch]$Uninstall, [string]$GameDir = '', [string]$FrontiersDir = '', [string]$UserDir = '')
$ErrorActionPreference = 'Stop'

$here = $PSScriptRoot
$data = Join-Path $env:LOCALAPPDATA 'SonicBeam'
$record = Join-Path $data 'install.json'
$backup = Join-Path $data 'backup'
$modName = 'sonicfrontiers_sonic'

function Say([string]$text) { Write-Host $text }
function Fail([string]$text) { Write-Host ''; Write-Host "ERROR: $text" -ForegroundColor Red; exit 1 }

function Steam-Libraries {
    $roots = @()
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam') {
        try {
            $item = Get-ItemProperty $key -ErrorAction Stop
            foreach ($name in 'SteamPath', 'InstallPath') { if ($item.$name) { $roots += ($item.$name -replace '/', '\') } }
        } catch {}
    }
    $libraries = @()
    foreach ($root in ($roots | Select-Object -Unique)) {
        $libraries += $root
        $vdf = Join-Path $root 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) { $libraries += ($m.Groups[1].Value -replace '\\\\', '\') }
        }
    }
    return $libraries | Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique
}

function Find-SteamGame([string]$appId) {
    foreach ($library in Steam-Libraries) {
        $manifest = Join-Path $library "steamapps\appmanifest_$appId.acf"
        if (Test-Path $manifest) {
            $m = [regex]::Match((Get-Content $manifest -Raw), '"installdir"\s+"([^"]+)"')
            if ($m.Success) {
                $dir = Join-Path $library ("steamapps\common\" + $m.Groups[1].Value)
                if (Test-Path $dir) { return $dir }
            }
        }
    }
    return ''
}

function Ask-Folder([string]$what, [string]$mustHave) {
    while ($true) {
        $answer = Read-Host "Folder of $what (the one holding $mustHave), or leave empty to cancel"
        if (-not $answer) { Fail "$what was not found. Run the installer again and give its folder." }
        $answer = $answer.Trim('"', ' ')
        if (Test-Path (Join-Path $answer $mustHave)) { return $answer }
        Say "  $mustHave is not in that folder."
    }
}

function Is-SonicBeamDll([string]$path) {
    if (-not (Test-Path $path)) { return $false }
    $text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($path))
    return $text.Contains('Sonic Beam renderer')
}

function Assert-BeamNGClosed {
    if (Get-Process -Name 'BeamNG.drive*' -ErrorAction SilentlyContinue) { Fail 'BeamNG.drive is running. Close it, then run this again.' }
}

function Install {
    Assert-BeamNGClosed
    if (-not (Test-Path (Join-Path $here "$modName\scripts")) -or -not (Test-Path (Join-Path $here 'bin64\version.dll'))) {
        Fail "Run this from the unzipped Sonic Beam folder (it must hold $modName and bin64)."
    }
    if (Test-Path $record) { Fail 'Sonic Beam is already installed. Run uninstall.bat first, then install again.' }

    if (-not $GameDir) { $GameDir = Find-SteamGame '284160' }
    if (-not $GameDir -or -not (Test-Path (Join-Path $GameDir 'Bin64'))) { $GameDir = Ask-Folder 'BeamNG.drive' 'Bin64' }
    if (-not $FrontiersDir) { $FrontiersDir = Find-SteamGame '1237320' }
    if (-not $FrontiersDir -or -not (Test-Path (Join-Path $FrontiersDir 'image\x64\raw'))) { $FrontiersDir = Ask-Folder 'Sonic Frontiers' 'image\x64\raw' }
    if (-not $UserDir) { $UserDir = Join-Path $env:LOCALAPPDATA 'BeamNG\BeamNG.drive\current' }

    $bin64 = Join-Path $GameDir 'Bin64'
    $mods = Join-Path $UserDir 'mods\unpacked'
    Say "BeamNG.drive:     $GameDir"
    Say "Sonic Frontiers:  $FrontiersDir"
    Say "BeamNG user data: $UserDir"
    Say ''

    New-Item -ItemType Directory -Force $backup | Out-Null
    $installed = @()
    $backedUp = @()
    try {
        # Back up anything this install would overwrite (another mod's version.dll, an older copy)
        $targets = @(
            @{ from = (Join-Path $here 'bin64\version.dll'); to = (Join-Path $bin64 'version.dll'); kind = 'file' },
            @{ from = (Join-Path $here 'bin64\SonicBeam'); to = (Join-Path $bin64 'SonicBeam'); kind = 'folder' },
            @{ from = (Join-Path $here $modName); to = (Join-Path $mods $modName); kind = 'folder' }
        )
        foreach ($t in $targets) {
            if (Test-Path $t.to) {
                $saved = Join-Path $backup ([System.IO.Path]::GetFileName($t.to) + '.' + [guid]::NewGuid().ToString('N').Substring(0, 8))
                Move-Item $t.to $saved
                $backedUp += @{ original = $t.to; saved = $saved }
                Say "Backed up $($t.to)"
            }
        }
        New-Item -ItemType Directory -Force $mods | Out-Null
        foreach ($t in $targets) {
            Copy-Item -Recurse $t.from $t.to
            $installed += $t.to
            Say "Installed $($t.to)"
        }
        # The renderer reads the Sonic Frontiers folder from settings.ini
        $settings = Join-Path $data 'settings.ini'
        Set-Content -Encoding ASCII $settings ('frontiers=' + ($FrontiersDir -replace '\\', '/'))
        Say "Wrote $settings"
    } catch {
        Say ''
        Say 'Something went wrong; putting everything back...'
        foreach ($path in $installed) { Remove-Item -Recurse -Force $path -ErrorAction SilentlyContinue }
        foreach ($b in $backedUp) { Move-Item $b.saved $b.original -ErrorAction SilentlyContinue }
        if ($_.Exception -is [System.UnauthorizedAccessException]) { Fail "Windows refused to write a file ($($_.Exception.Message)). Run install.bat as administrator." }
        Fail $_.Exception.Message
    }
    $state = [ordered]@{
        version = [string](Get-Content -Raw (Join-Path $here 'bin64\SonicBeam\VERSION') -ErrorAction SilentlyContinue).Trim()
        gameDir = [string]$GameDir; frontiersDir = [string]$FrontiersDir; userDir = [string]$UserDir
        installed = @($installed | ForEach-Object { [string]$_ })
        backups = @($backedUp | ForEach-Object { [ordered]@{ original = [string]$_.original; saved = [string]$_.saved } })
        labSettings = [string](Join-Path $UserDir 'settings\sonicfrontiers_sonic.json')
    }
    $state | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 $record
    Say ''
    Say 'Sonic Beam is installed. Start BeamNG.drive, load any map in Freeroam and you are Sonic.'
    Say 'If BeamNG warns about a "Third-party library" (version.dll), click Cancel.'
    Say 'To remove it and put everything back: uninstall.bat'
}

function Uninstall {
    Assert-BeamNGClosed
    if (Test-Path $record) {
        $state = Get-Content $record -Raw | ConvertFrom-Json
        foreach ($path in $state.installed) {
            if (Test-Path $path) { Remove-Item -Recurse -Force $path; Say "Removed $path" }
        }
        foreach ($b in $state.backups) {
            if (Test-Path $b.saved) { Move-Item $b.saved $b.original; Say "Restored $($b.original)" }
        }
        if ($state.labSettings -and (Test-Path $state.labSettings)) { Remove-Item -Force $state.labSettings; Say "Removed $($state.labSettings)" }
    } else {
        # No record (installed by hand): remove Sonic Beam's files where they usually are
        Say 'No install record found; looking for Sonic Beam in the usual places.'
        if (-not $GameDir) { $GameDir = Find-SteamGame '284160' }
        if (-not $UserDir) { $UserDir = Join-Path $env:LOCALAPPDATA 'BeamNG\BeamNG.drive\current' }
        if ($GameDir) {
            $dll = Join-Path $GameDir 'Bin64\version.dll'
            if (Is-SonicBeamDll $dll) { Remove-Item -Force $dll; Say "Removed $dll" }
            elseif (Test-Path $dll) { Say "Left $dll in place: it is not Sonic Beam's." }
            $folder = Join-Path $GameDir 'Bin64\SonicBeam'
            if (Test-Path $folder) { Remove-Item -Recurse -Force $folder; Say "Removed $folder" }
        } else { Say 'BeamNG.drive was not found in the Steam libraries: remove Bin64\version.dll and Bin64\SonicBeam by hand.' }
        foreach ($path in (Join-Path $UserDir "mods\unpacked\$modName"), (Join-Path $UserDir 'settings\sonicfrontiers_sonic.json')) {
            if (Test-Path $path) { Remove-Item -Recurse -Force $path; Say "Removed $path" }
        }
    }
    if (Test-Path $data) { Remove-Item -Recurse -Force $data; Say "Removed $data" }
    Say ''
    Say 'Sonic Beam is uninstalled and the files it replaced are back.'
}

try {
    if ($Uninstall) { Uninstall } else { Install }
} catch {
    Fail $_.Exception.Message
}
