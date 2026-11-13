# Installs or removes the Horizon Legacy Loader in the game folder (the folder this script's
# horizon\loader\ lies in). Called by Horizon-Install.cmd / Horizon-Uninstall.cmd.
#   install:   an existing dinput8.dll that is not ours becomes dinput8.chain.dll (the loader runs it
#              first, so that mod keeps working), then horizon\loader\dinput8.dll is copied next to the exe.
#   uninstall: our dinput8.dll is removed, dinput8.chain.dll gets its old name back. The horizon folder
#              (settings, logs, plugins) stays; delete it by hand if you want.
param([ValidateSet('install', 'uninstall')][string]$Mode = 'install')
$ErrorActionPreference = 'Stop'
$game = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$ours = Join-Path $PSScriptRoot 'dinput8.dll'
$target = Join-Path $game 'dinput8.dll'
$chain = Join-Path $game 'dinput8.chain.dll'

function Test-Ours([string]$path) {
    if (-not (Test-Path $path)) { return $false }
    $bytes = [IO.File]::ReadAllBytes($path)
    $text = [Text.Encoding]::Unicode.GetString($bytes)
    return $text.Contains('Horizon Legacy Loader')
}

$exes = @('TmNationsESWC.exe', 'TmSunrise.exe', 'TmForever.exe', 'TmOriginal.exe') | Where-Object { Test-Path (Join-Path $game $_) }
if (-not $exes) {
    Write-Host "No TrackMania exe in $game - put the zip's content next to the game's exe." -ForegroundColor Red
    exit 1
}

if ($Mode -eq 'install') {
    if ((Test-Path $target) -and -not (Test-Ours $target)) {
        if (Test-Path $chain) {
            $backup = Join-Path $game ('dinput8.dll.before-horizon-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
            Move-Item $target $backup
            Write-Host "dinput8.chain.dll exists already: the other dinput8.dll was kept as $(Split-Path -Leaf $backup) (not loaded)."
        } else {
            Move-Item $target $chain
            Write-Host 'The existing dinput8.dll is now dinput8.chain.dll: Horizon loads it first, that mod keeps working.'
        }
    }
    Copy-Item $ours $target -Force
    Write-Host "Horizon Client installed for $($exes -join ', '). Start the game; the menu opens with Insert." -ForegroundColor Green
} else {
    if (Test-Ours $target) { Remove-Item $target -Force }
    if ((Test-Path $chain) -and -not (Test-Path $target)) {
        Move-Item $chain $target
        Write-Host 'dinput8.chain.dll is dinput8.dll again.'
    }
    Write-Host 'Horizon Legacy Loader removed. The horizon folder (settings, logs, plugins) is still there.' -ForegroundColor Green
}
