# Copies the game-settings module (build.cmd output: dist\legacy\horizon\plugins\game-settings\) into my three
# legacy Horizon test copies. Nothing else is touched: host.dll, the other modules, settings, data, logs, the
# Legacy Loader, %LOCALAPPDATA%\TMLoader and any real game folder stay as they are.
# An existing game-settings folder is kept once as plugins.bak-vor-gs\game-settings. A copy whose game is running
# is skipped. Needs a host that loads modules from horizon\plugins\ (0.3.1 and later). An older version of the
# module in a copy is replaced (1.0.0 -> 1.0.1: no stutter while dragging a slider); its settings stay.
#   powershell -ExecutionPolicy Bypass -File tools/deploy-game-settings.ps1 [-DryRun] [-Source <module folder>] [-Copies <dir>,<dir>]
# Without -Copies the test copies come from HORIZON_TEST_COPIES (folders separated by ';').
param(
    [switch]$DryRun,
    [string]$Source = "",
    [string[]]$Copies = @("$env:HORIZON_TEST_COPIES" -split ';' | Where-Object { $_.Trim() })
)
$ErrorActionPreference = 'Stop'
$W = Split-Path -Parent $PSScriptRoot
$tag = 'bak-vor-gs'
$src = if ($Source) { $Source } else { Join-Path $W 'dist\legacy\horizon\plugins\game-settings' }
if (-not (Test-Path "$src\game-settings.dll")) { throw "missing $src\game-settings.dll: run build.cmd first" }
$version = (Get-Content "$src\plugin.json" -Raw | ConvertFrom-Json).version
$out = @("game-settings $version from $src")

function Do-Copy($from, $to, [switch]$Recurse) {
    if ($DryRun) { $script:out += "  would copy $from -> $to"; return }
    if ($Recurse) { Copy-Item $from $to -Recurse -Force } else { Copy-Item $from $to -Force }
}
function Do-Remove($path) {
    if ($DryRun) { $script:out += "  would remove $path"; return }
    Remove-Item $path -Recurse -Force
}
function Running-In($dir) {
    Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($dir, [StringComparison]::OrdinalIgnoreCase) }
}

if (-not $Copies.Count) { throw 'no test copies: pass -Copies or set HORIZON_TEST_COPIES (folders separated by ;)' }
foreach ($dir in $Copies) {
    $dir = $dir.Trim().TrimEnd('\')
    $hz = Join-Path $dir 'horizon'
    if (-not (Test-Path "$hz\host.dll")) { $out += "${dir}: no horizon\host.dll, skipped"; continue }
    $running = Running-In $dir
    if ($running) { $out += "${dir}: SKIPPED, game running ($($running.Name -join ', '))"; continue }
    $dst = "$hz\plugins\game-settings"
    $bak = "$hz\plugins.$tag"
    if ((Test-Path $dst) -and -not (Test-Path "$bak\game-settings")) {
        if (-not (Test-Path $bak) -and -not $DryRun) { New-Item -ItemType Directory $bak | Out-Null }
        Do-Copy $dst "$bak\game-settings" -Recurse
    }
    if (Test-Path $dst) { Do-Remove $dst }
    if (-not (Test-Path "$hz\plugins") -and -not $DryRun) { New-Item -ItemType Directory "$hz\plugins" | Out-Null }
    Do-Copy $src $dst -Recurse
    $sha = if ($DryRun) { '(dry run)' } else { (Get-FileHash "$dst\game-settings.dll" -Algorithm SHA256).Hash.Substring(0, 12) }
    $old = if (Test-Path "$bak\game-settings\plugin.json") { (Get-Content "$bak\game-settings\plugin.json" -Raw | ConvertFrom-Json).version } else { "" }
    $out += "${dir}: game-settings $version, dll $sha" + $(if ($old) { " (backup of the first one: $old)" } else { "" })
    # a profile.json next to host.dll limits which modules load: game-settings must be listed there
    $profile = "$hz\profile.json"
    if ((Test-Path $profile) -and -not ((Get-Content $profile -Raw) -match '"game-settings"')) {
        $out += "  note: $profile does not list game-settings: add it there (or in the client) to load the module"
    }
}
$out
