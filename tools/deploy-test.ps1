# Copies the packaged 0.3.1 build (tools\package.py, dist\) into my test folders: the TMLoader product test folder
# and the three legacy test copies (host.dll, our modules, the example scripts). Backups: *.bak-vor-031, made once.
# A copy whose game is running is skipped. Never touched: settings, data, logs, the Legacy Loader (dinput8.dll,
# unchanged in 0.3.1), the legacy-randomizer module, %LOCALAPPDATA%\TMLoader and any real game folder.
#   powershell -ExecutionPolicy Bypass -File tools\deploy-test.ps1 [-DryRun]
param([switch]$DryRun)
$ErrorActionPreference = 'Stop'
$W = Split-Path -Parent $PSScriptRoot
$version = '0.3.1'
$tag = 'bak-vor-031'
$zips = Join-Path $W 'dist\release'
$out = @()

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

foreach ($z in 'ESWC', 'Sunrise', 'Original', 'TMLoader') {
    if (-not (Test-Path "$zips\Horizon-Client-$version-$z.zip")) { throw "missing $zips\Horizon-Client-$version-$z.zip: run build.cmd package first" }
}

# TMLoader product test folder: the new version folder next to the old ones, description.yaml updated
$prod = 'C:\Users\osaro\Downloads\TM-Horizon-Test\tmloader-products\Horizon Host'
$src = Join-Path $W "dist\tmloader\Horizon Host\$version"
if (Test-Path $prod) {
    $dst = Join-Path $prod $version
    if (Test-Path $dst) {
        if (-not (Test-Path "$dst.$tag")) { Do-Copy $dst "$dst.$tag" -Recurse }
        Do-Remove $dst
    }
    Do-Copy $src $dst -Recurse
    $desc = Join-Path $prod 'description.yaml'
    if ((Test-Path $desc) -and -not (Test-Path "$desc.$tag")) { Do-Copy $desc "$desc.$tag" }
    Do-Copy (Join-Path $W 'dist\tmloader\Horizon Host\description.yaml') $desc
    $out += "tmloader-products: Horizon Host\$version + description.yaml"
} else {
    $out += "tmloader-products: $prod not found, skipped"
}

# legacy test copies
$copies = @(
    @{ dir = 'C:\Users\osaro\Games\TMN-ESWC-horizon-next'; zip = 'ESWC' },
    @{ dir = 'C:\Users\osaro\Games\TMSunrise-horizon-next'; zip = 'Sunrise' },
    @{ dir = 'C:\Users\osaro\Games\TMO-horizon-test'; zip = 'Original' }
)
$tmp = Join-Path $env:TEMP 'horizon-deploy-031'
foreach ($c in $copies) {
    $hz = Join-Path $c.dir 'horizon'
    if (-not (Test-Path "$hz\host.dll")) { $out += "$($c.dir): no horizon\host.dll, skipped"; continue }
    $running = Running-In $c.dir
    if ($running) { $out += "$($c.dir): SKIPPED, game running ($($running.Name -join ', '))"; continue }
    $unz = Join-Path $tmp $c.zip
    if (Test-Path $unz) { Remove-Item $unz -Recurse -Force }
    Expand-Archive "$zips\Horizon-Client-$version-$($c.zip).zip" $unz
    $st = "$unz\horizon"
    if (-not (Test-Path "$hz\host.dll.$tag")) { Do-Copy "$hz\host.dll" "$hz\host.dll.$tag" }
    Do-Copy "$st\host.dll" "$hz\host.dll"
    $pb = "$hz\plugins.$tag"
    if (-not (Test-Path $pb) -and -not $DryRun) { New-Item -ItemType Directory $pb | Out-Null }
    $ids = @()
    foreach ($p in Get-ChildItem "$st\plugins" -Directory) {
        if ($p.Name -eq 'legacy-randomizer') { continue } # never ours to replace
        $dst = "$hz\plugins\$($p.Name)"
        if ((Test-Path $dst) -and -not (Test-Path "$pb\$($p.Name)")) { Do-Copy $dst "$pb\$($p.Name)" -Recurse }
        if (Test-Path $dst) { Do-Remove $dst }
        Do-Copy $p.FullName $dst -Recurse
        $ids += $p.Name
    }
    if ((Test-Path "$hz\scripts") -and -not (Test-Path "$hz\scripts.$tag")) { Do-Copy "$hz\scripts" "$hz\scripts.$tag" -Recurse }
    if (-not (Test-Path "$hz\scripts") -and -not $DryRun) { New-Item -ItemType Directory "$hz\scripts" | Out-Null }
    foreach ($s in Get-ChildItem "$st\scripts" -Directory) {
        $dst = "$hz\scripts\$($s.Name)"
        if (Test-Path $dst) { Do-Remove $dst }
        Do-Copy $s.FullName $dst -Recurse
    }
    $sha = if ($DryRun) { '(dry run)' } else { (Get-FileHash "$hz\host.dll" -Algorithm SHA256).Hash.Substring(0, 12) }
    $out += "$($c.dir): host.dll $sha, " + ($ids -join ', ') + ", scripts"
}
if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
$out
