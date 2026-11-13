# Copies the built Controller fix module (dist\legacy\horizon\plugins\controller-hotplug) into the Horizon test
# copies of ESWC, Sunrise and Original. Skips a copy whose game is running. Backs up the old module folder once per
# copy as horizon\controller-hotplug.bak-vor-padfix3 (next to plugins\, so the host does not load it as a module).
# Never touches C:\Program Files (x86), %LOCALAPPDATA%\TMLoader or other game folders.
#   powershell -ExecutionPolicy Bypass -File plugins/controller-hotplug/tools/deploy-test-copies.ps1 [-Copies <dir>,<dir>]
# Without -Copies the test copies come from HORIZON_TEST_COPIES (folders separated by ';').
param(
    [string[]]$Copies = @("$env:HORIZON_TEST_COPIES" -split ';' | Where-Object { $_.Trim() }),
    [string]$Backup = 'controller-hotplug.bak-vor-padfix3'
)
$ErrorActionPreference = 'Stop'
if (-not $Copies.Count) { throw 'no test copies: pass -Copies or set HORIZON_TEST_COPIES (folders separated by ;)' }
$Copies = @($Copies | ForEach-Object { $_.Trim().TrimEnd('\') })
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$src = Join-Path $repo 'dist\legacy\horizon\plugins\controller-hotplug'
if (-not (Test-Path (Join-Path $src 'controller-hotplug.dll'))) { throw "not built: $src (run build.cmd first)" }
$ver = (Get-Content (Join-Path $src 'plugin.json') -Raw | ConvertFrom-Json).version
$md5 = (Get-FileHash (Join-Path $src 'controller-hotplug.dll') -Algorithm MD5).Hash
"Controller fix $ver, controller-hotplug.dll md5 $md5"
foreach ($c in $Copies) {
    if ($c -match 'Program Files|\\TMLoader') { "SKIP $c : not a test copy"; continue }
    $running = Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($c + '\', [StringComparison]::OrdinalIgnoreCase) }
    if ($running) { "SKIP $c : $($running.Name -join ', ') is running"; continue }
    $dst = Join-Path $c 'horizon\plugins\controller-hotplug'
    if (-not (Test-Path $dst)) { "SKIP $c : no horizon\plugins\controller-hotplug"; continue }
    $bak = Join-Path $c "horizon\$Backup"
    if (-not (Test-Path $bak)) { Copy-Item $dst $bak -Recurse }
    Copy-Item (Join-Path $src '*') $dst -Force
    $h = (Get-FileHash (Join-Path $dst 'controller-hotplug.dll') -Algorithm MD5).Hash
    "OK   $c : md5 $h (backup $bak)"
}
