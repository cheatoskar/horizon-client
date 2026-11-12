# Installs or removes the standalone Controller Hotplug in the game folder (the parent of the folder
# this script lies in). Called by Controller-Hotplug-Install.cmd / Controller-Hotplug-Uninstall.cmd.
#   install:   controller-hotplug\dinput8.dll (a small loader) is put next to the exe as dinput8.dll.
#              An existing dinput8.dll of another mod becomes dinput8.chain.dll (our loader runs it,
#              that mod keeps working). A foreign dinput8.dll is never overwritten or deleted.
#              If the Horizon Legacy Loader is there, our loader goes in as dinput8.chain.dll (Horizon
#              loads it first; its built-in controller module then pauses itself).
#   uninstall: our loader is removed wherever it is (dinput8.dll or dinput8.chain.dll), a foreign
#              dinput8.chain.dll gets its old name back, controller_hotplug.dll is removed. The log stays.
param([ValidateSet('install', 'uninstall')][string]$Mode = 'install')
$ErrorActionPreference = 'Stop'
$game = Split-Path -Parent $PSScriptRoot
$ours = Join-Path $PSScriptRoot 'dinput8.dll'
$mod = Join-Path $game 'controller_hotplug.dll'
$target = Join-Path $game 'dinput8.dll'
$chain = Join-Path $game 'dinput8.chain.dll'

function Test-Has([string]$path, [string]$marker) {
    if (-not (Test-Path $path)) { return $false }
    $bytes = [IO.File]::ReadAllBytes($path)
    return [Text.Encoding]::Unicode.GetString($bytes).Contains($marker)
}
function Test-Ours([string]$path) { Test-Has $path 'Controller Hotplug Loader' }
function Test-Horizon([string]$path) { Test-Has $path 'Horizon Legacy Loader' }

function Stop-With([string]$text) {
    Write-Host $text -ForegroundColor Red
    Write-Host 'Nothing was changed.'
    exit 1
}

$exes = @('TmNationsESWC.exe', 'TmSunrise.exe') | Where-Object { Test-Path (Join-Path $game $_) }
if (-not $exes) {
    Write-Host "No TmNationsESWC.exe or TmSunrise.exe in $game - unzip the zip's content next to the game's exe." -ForegroundColor Red
    exit 1
}

try {
    if ($Mode -eq 'install') {
        if (-not (Test-Path $ours)) { Stop-With "controller-hotplug\dinput8.dll is missing in the zip's folder." }
        if (-not (Test-Path $mod)) { Stop-With 'controller_hotplug.dll is missing: unzip the whole zip into the game folder.' }

        if (-not (Test-Path $target)) {
            Copy-Item $ours $target -Force
            Write-Host 'Controller Hotplug installed as dinput8.dll.' -ForegroundColor Green
        } elseif (Test-Ours $target) {
            Copy-Item $ours $target -Force
            Write-Host 'Controller Hotplug updated (dinput8.dll).' -ForegroundColor Green
        } elseif (Test-Horizon $target) {
            if (-not (Test-Path $chain)) {
                Copy-Item $ours $chain -Force
                Write-Host 'Horizon is installed here and has Controller Hotplug built in ("Controller fix").'
                Write-Host 'Our loader is now dinput8.chain.dll: Horizon loads it first, its module then pauses itself.' -ForegroundColor Green
            } elseif (Test-Ours $chain) {
                Copy-Item $ours $chain -Force
                Write-Host 'Controller Hotplug updated (dinput8.chain.dll, loaded by Horizon).' -ForegroundColor Green
            } else {
                Stop-With 'Horizon is installed and dinput8.chain.dll is another mod. Horizon has Controller Hotplug built in (menu: "Controller fix"), so use that instead.'
            }
        } else {
            if (Test-Path $chain) {
                Stop-With 'dinput8.dll and dinput8.chain.dll are both other mods. Remove one of them first, then run this again.'
            }
            Move-Item $target $chain
            Copy-Item $ours $target -Force
            Write-Host 'The existing dinput8.dll is now dinput8.chain.dll: our loader runs it, that mod keeps working.'
            Write-Host 'Controller Hotplug installed as dinput8.dll.' -ForegroundColor Green
        }
        Write-Host "Start the game ($($exes -join ', ')), then plug in the pad. The log is controller_hotplug.log in the game folder."
    } else {
        $wasTarget = Test-Ours $target
        if ($wasTarget) { Remove-Item $target -Force }
        if (Test-Ours $chain) { Remove-Item $chain -Force }
        if ($wasTarget -and (Test-Path $chain) -and -not (Test-Path $target)) {
            Move-Item $chain $target
            Write-Host 'dinput8.chain.dll is dinput8.dll again.'
        }
        if (Test-Path $mod) { Remove-Item $mod -Force }
        Write-Host 'Controller Hotplug removed. controller_hotplug.log stays; delete it by hand if you want.' -ForegroundColor Green
    }
} catch {
    Write-Host "Failed: $($_.Exception.Message) (is the game still running?)" -ForegroundColor Red
    exit 1
}
