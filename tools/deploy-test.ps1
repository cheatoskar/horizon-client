# Horizon Client 0.3.1 (release candidate) into my test folders, everything client-related in one go:
#   - the three legacy test copies (ESWC, Sunrise, Original): host.dll, modules.json + icons, all our modules,
#     the example scripts, the Legacy Loader (horizon\loader; dinput8.dll in the game folder only if its size changed or with -Loader),
#     and the TM Vibrant Shaders modules from the TMVS 1.6 / 2.0 release candidate (tmvs, tmvs-capture,
#     tmvs-upscaling; tmvs-packs is Forever only and is moved out);
#   - the TMLoader test product (tmloader-products\Horizon Host\0.3.1 + description.yaml) with all four TMVS modules.
# Node Explorer (developer tool, not in the release any more) is moved out of the copies.
# One backup per copy before anything changes:
#   <copy>\_horizon-backup\0.3.1-rc-<time>\  (host.dll, modules.json, icons, plugins, scripts, loader, dinput8.dll)
#   tmloader-products\_before-031rc\<time>\  (Horizon Host\0.3.1 and description.yaml)
# A copy whose game runs is skipped (TMLoader: any TmForever.exe). Never touched: settings, data, logs, the
# legacy-randomizer module, the TM Vibrant Shaders product folder, %LOCALAPPDATA%\TMLoader, any real game folder.
#   powershell -ExecutionPolicy Bypass -File tools\deploy-test.ps1 [-DryRun] -Package <dir> -Tmvs <dir> -Copies <dir>,<dir> [-Products <dir>]
# Paths come from the parameters or, when a parameter is left out, from these environment variables:
#   HORIZON_TEST_PACKAGE   client package folder (release\Horizon-Client-<version>-*.zip, tmloader\Horizon Host\...)
#   HORIZON_TEST_TMVS      TMVS release folder (horizon\<module>\<module>.dll)
#   HORIZON_TEST_COPIES    test copies, separated by ';' (the game is told by its exe: ESWC, Sunrise or Original)
#   HORIZON_TEST_PRODUCTS  TMLoader products folder with "Horizon Host" (optional; left out = skipped)
param(
    [switch]$DryRun,
    [switch]$Loader,
    [string]$Package = $env:HORIZON_TEST_PACKAGE,
    [string]$Tmvs = $env:HORIZON_TEST_TMVS,
    [string]$Products = $env:HORIZON_TEST_PRODUCTS,
    [string[]]$Copies = @("$env:HORIZON_TEST_COPIES" -split ';' | Where-Object { $_.Trim() })
)
$ErrorActionPreference = 'Stop'
$version = '0.3.1'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$keep = @('legacy-randomizer')                  # other people's modules: never replaced, never removed
$gone = @('node-explorer')                      # not in the release: moved into the backup
$tmvsLegacy = @('tmvs', 'tmvs-capture', 'tmvs-upscaling')
$tmvsAll = $tmvsLegacy + @('tmvs-packs')
$out = New-Object System.Collections.Generic.List[string]

function Say($s) { if ($DryRun) { Write-Host "[dry run] $s" } else { Write-Host $s } }
function Copy-To($from, $to) {
    Say "  copy $from -> $to"
    if ($DryRun) { return }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to) | Out-Null
    Copy-Item -LiteralPath $from -Destination $to -Recurse -Force
}
function Remove-Path($p) {
    if (-not (Test-Path -LiteralPath $p)) { return }
    Say "  remove $p"
    if (-not $DryRun) { Remove-Item -LiteralPath $p -Recurse -Force }
}
function Running-In($dir) {
    @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($dir, [StringComparison]::OrdinalIgnoreCase) })
}
function Hash($p) { if (Test-Path -LiteralPath $p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash } else { '' } }

function Game-Of($dir) {
    if (Test-Path -LiteralPath "$dir\TmNationsESWC.exe") { return 'ESWC' }
    if (Test-Path -LiteralPath "$dir\TmSunrise.exe") { return 'Sunrise' }
    if (Test-Path -LiteralPath "$dir\TMORIGINAL.EXE") { return 'Original' }
    return ''
}

# --- the sources must be complete
if (-not $Package) { throw 'no package folder: pass -Package or set HORIZON_TEST_PACKAGE' }
if (-not $Tmvs) { throw 'no TMVS folder: pass -Tmvs or set HORIZON_TEST_TMVS' }
if (-not $Copies.Count) { throw 'no test copies: pass -Copies or set HORIZON_TEST_COPIES (folders separated by ;)' }
$zips = Join-Path $Package 'release'
foreach ($z in 'ESWC', 'Sunrise', 'Original') {
    if (-not (Test-Path "$zips\Horizon-Client-$version-$z.zip")) { throw "missing $zips\Horizon-Client-$version-$z.zip" }
}
$prodSrc = Join-Path $Package "tmloader\Horizon Host\$version"
if (-not (Test-Path "$prodSrc\HorizonHost.dll")) { throw "missing $prodSrc\HorizonHost.dll" }
if (-not (Test-Path "$prodSrc\modules.json")) { throw "missing $prodSrc\modules.json (an older package?)" }
foreach ($m in $tmvsAll) { if (-not (Test-Path "$Tmvs\horizon\$m\$m.dll")) { throw "missing $Tmvs\horizon\$m\$m.dll" } }

# --- the legacy test copies (ESWC, Sunrise, Original)
$targets = foreach ($d in $Copies) {
    $d = $d.Trim().TrimEnd('\')
    $g = Game-Of $d
    if ($g) { @{ dir = $d; zip = $g } } else { $out.Add("$d : no TmNationsESWC.exe, TmSunrise.exe or TMORIGINAL.EXE, skipped") }
}
$tmp = Join-Path $env:TEMP "horizon-deploy-031rc-$stamp"
foreach ($c in $targets) {
    $dir = $c.dir
    $hz = Join-Path $dir 'horizon'
    Write-Host "== $dir"
    if (-not (Test-Path "$hz\host.dll")) { $out.Add("$dir : no horizon\host.dll, skipped"); continue }
    $running = Running-In $dir
    if ($running.Count) { $out.Add("$dir : SKIPPED, game running ($($running.Name -join ', '))"); continue }

    $unz = Join-Path $tmp $c.zip
    Expand-Archive "$zips\Horizon-Client-$version-$($c.zip).zip" $unz
    $st = "$unz\horizon"

    # one backup of everything this run replaces or removes
    $bk = Join-Path $dir "_horizon-backup\$version-rc-$stamp"
    Say "  backup -> $bk"
    if (-not $DryRun) {
        New-Item -ItemType Directory -Force -Path "$bk\horizon" | Out-Null
        foreach ($n in 'host.dll', 'modules.json', 'icons', 'plugins', 'scripts', 'loader') {
            if (Test-Path -LiteralPath "$hz\$n") { Copy-Item -LiteralPath "$hz\$n" -Destination "$bk\horizon\$n" -Recurse -Force }
        }
        if (Test-Path -LiteralPath "$dir\dinput8.dll") { Copy-Item -LiteralPath "$dir\dinput8.dll" -Destination "$bk\dinput8.dll" -Force }
    }

    # host, modules.json, icons, loader
    Copy-To "$st\host.dll" "$hz\host.dll"
    Copy-To "$st\modules.json" "$hz\modules.json"
    Remove-Path "$hz\icons"
    Copy-To "$st\icons" "$hz\icons"
    Remove-Path "$hz\loader"
    Copy-To "$st\loader" "$hz\loader"
    $loaderNote = 'loader unchanged'
    # the loader has not changed since 0.3.0; every build differs in its time stamp, so only a size change or -Loader replaces it
    if ((Test-Path "$dir\dinput8.dll") -and ($Loader -or (Get-Item "$st\loader\dinput8.dll").Length -ne (Get-Item "$dir\dinput8.dll").Length)) {
        Copy-To "$st\loader\dinput8.dll" "$dir\dinput8.dll"
        $loaderNote = 'dinput8.dll updated'
    }

    # our modules (whole folders: no stale files), then the TMVS modules
    $ids = New-Object System.Collections.Generic.List[string]
    foreach ($p in Get-ChildItem "$st\plugins" -Directory) {
        if ($keep -contains $p.Name) { continue }
        Remove-Path "$hz\plugins\$($p.Name)"
        Copy-To $p.FullName "$hz\plugins\$($p.Name)"
        $ids.Add($p.Name)
    }
    foreach ($m in $tmvsLegacy) {
        Remove-Path "$hz\plugins\$m"
        Copy-To "$Tmvs\horizon\$m" "$hz\plugins\$m"
        $ids.Add($m)
    }
    foreach ($m in $gone + @('tmvs-packs')) { Remove-Path "$hz\plugins\$m" }

    # example scripts (only the folders the package has; the player's own scripts stay)
    foreach ($s in Get-ChildItem "$st\scripts" -Directory) {
        Remove-Path "$hz\scripts\$($s.Name)"
        Copy-To $s.FullName "$hz\scripts\$($s.Name)"
    }
    $sha = if ($DryRun) { '(dry run)' } else { (Hash "$hz\host.dll").Substring(0, 12) }
    $out.Add("$dir : host.dll $sha, $($ids -join ', '), scripts, modules.json, $loaderNote; backup $bk")
}
if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }

# --- the TMLoader test product
Write-Host "== $Products"
$prod = if ($Products) { Join-Path $Products 'Horizon Host' } else { '' }
$tmf = @(Get-Process -Name TmForever -ErrorAction SilentlyContinue)
if (-not $Products) {
    $out.Add('tmloader-products: no folder given (-Products or HORIZON_TEST_PRODUCTS), skipped')
} elseif (-not (Test-Path $prod)) {
    $out.Add("tmloader-products: $prod not found, skipped")
} elseif ($tmf.Count) {
    $out.Add("tmloader-products: SKIPPED, TmForever.exe runs (pid $($tmf.Id -join ', '))")
} else {
    $dst = Join-Path $prod $version
    $bk = Join-Path $Products "_before-031rc\$stamp"
    $kept = @()
    Say "  backup -> $bk"
    if (-not $DryRun) {
        New-Item -ItemType Directory -Force -Path "$bk\Horizon Host" | Out-Null
        if (Test-Path "$prod\description.yaml") { Copy-Item "$prod\description.yaml" "$bk\Horizon Host\description.yaml" -Force }
    }
    if (Test-Path $dst) {
        Say "  move $dst -> $bk\Horizon Host\$version"
        if (-not $DryRun) { Move-Item -LiteralPath $dst -Destination "$bk\Horizon Host\$version" }
    }
    Copy-To $prodSrc $dst
    foreach ($m in $tmvsAll) { Copy-To "$Tmvs\horizon\$m" "$dst\plugins\$m" }
    foreach ($k in $keep) { # someone else's module that was there stays
        $old = if ($DryRun) { "$dst\plugins\$k" } else { "$bk\Horizon Host\$version\plugins\$k" }
        if (Test-Path -LiteralPath $old) { Copy-To $old "$dst\plugins\$k"; $kept += $k }
    }
    Copy-To (Join-Path $Package 'tmloader\Horizon Host\description.yaml') "$prod\description.yaml"
    $sha = if ($DryRun) { '(dry run)' } else { (Hash "$dst\HorizonHost.dll").Substring(0, 12) }
    $out.Add("tmloader-products: Horizon Host\$version (HorizonHost.dll $sha) + TMVS modules $($tmvsAll -join ', ')" +
        $(if ($kept.Count) { " + kept $($kept -join ', ')" } else { '' }) + "; backup $bk")
}

Write-Host ''
$out
